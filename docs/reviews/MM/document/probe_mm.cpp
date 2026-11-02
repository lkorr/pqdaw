// MM review (document): builds valid schema-16 projects through the document API (the fuzzer's bases), the wire-law
// DC probes (a constant through an Add wire into a user device whose output IS the param's value), and the group /
// ungroup round trips on tricky inputs (document identity checked here; the three renders compared by fuzz_mm.py
// through `pqdaw --render`). Usage: probe_mm.exe <repo root> <out dir>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/meta/type.h"
#include "pq/meta/registry.h"

using namespace pq;
using namespace pq::doc;

namespace {

std::unique_ptr<DeviceLibrary> gLib;
std::string gOut;
int gFail = 0;

void fail(const std::string& what) {
  std::printf("FAIL %s\n", what.c_str());
  ++gFail;
}

bool ok(const Status& s, const std::string& what) {
  if (!s) {
    fail(what + ": " + s.error().message);
    return false;
  }
  return true;
}

template <class R>
bool okr(const R& r, const std::string& what) {
  if (!r.ok()) {
    fail(what + ": " + r.error().message);
    return false;
  }
  return true;
}

bool run(DocumentStore& s, const ops::EditResult& e, const std::string& what) {
  if (!okr(e, what)) return false;
  return ok(s.dispatch(e->command), what + " (dispatch)");
}

DevicePtr metaDevice(DeviceId id, std::string_view type, DeviceKind kind, bool free = false, std::string chainInput = {}) {
  const meta::Type* t = meta::findType(type);
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kMetaDevicePrefix) + std::string(type), kind));
  d->meta = std::make_shared<const MetaMap>(MetaMap{MetaEntry{"", defaultMetaInstance(*t)}});
  d->free = free;
  d->chainInput = std::move(chainInput);
  return d;
}

DevicePtr userDevice(DeviceId id, const std::string& stock, const std::string& text, bool free = false) {
  auto d = std::make_shared<Device>(*makeDevice(id, stock, DeviceKind::AudioEffect));
  auto u = std::make_shared<UserPatch>();
  u->text = text;
  d->source = u;
  d->free = free;
  return d;
}

TrackPtr midiTrack(TrackId id, std::string name, DeviceId inst = {}) {
  Track t = makeTrack(id, TrackKind::Midi, std::move(name));
  if (inst.valid()) t.devices.push_back(makeDefaultInstrument(inst));
  return std::make_shared<const Track>(std::move(t));
}

ClipPtr notesClip(IdSource& ids) {
  std::vector<Note> ns;
  const int32_t degs[] = {0, 4, 7, 12, 7, 4};
  for (int i = 0; i < 6; ++i) {
    Note n;
    n.id = ids.note();
    n.start = i * (kPpq / 2);
    n.length = kPpq / 2 + (i == 2 ? kPpq : 0);
    n.pitch = tuning::PitchId::fromDegree(degs[i] % 12, degs[i] / 12);
    n.velocity = 100.0f;
    ns.push_back(n);
  }
  NoteClip c;
  c.id = ids.clip();
  c.name = "n";
  c.start = 0;
  c.length = 4 * kPpq;
  c.notes = NoteSeq::fromNotes(std::move(ns)).value();
  return std::make_shared<const NoteClip>(std::move(c));
}

std::string dir(const std::string& name) { return gOut + "/" + name; }

void save(const std::string& name, const Project& p, const ViewState& v = {}) {
  std::filesystem::create_directories(dir(name));
  ok(saveProject(dir(name), p, v), "save " + name);
}

std::string text(const Project& p, const ViewState& v = {}) {
  auto t = serializeProject(p, v);
  return t.ok() ? *t : std::string("<serialize error: ") + t.error().message + ">";
}

// ---------------------------------------------------------------------------------------------------------------
// Base projects for the fuzzer.

void genFull() {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track(), bass = ids.track(), aud = ids.track();
  const DeviceId pa = ids.device(), wt = ids.device(), env = ids.device(), vca = ids.device(), user = ids.device(),
                 sat = ids.device(), lfo = ids.device(), util = ids.device(), eq = ids.device(), scope = ids.device(),
                 spg = ids.device(), lfo2 = ids.device(), binst = ids.device(), bflt = ids.device(), pafree = ids.device(),
                 avca = ids.device();
  Batch b{"full", {}};
  b.commands.push_back(AddTrack{midiTrack(keys, "Keys"), {}, kAppend});
  b.commands.push_back(AddTrack{midiTrack(bass, "Bass", binst), {}, kAppend});
  b.commands.push_back(AddTrack{std::make_shared<const Track>(makeTrack(aud, TrackKind::Audio, "Aud")), {}, kAppend});
  b.commands.push_back(AddClip{keys, notesClip(ids)});
  b.commands.push_back(AddClip{bass, notesClip(ids)});
  b.commands.push_back(AddDevice{keys, metaDevice(pa, "phase_acc", DeviceKind::Instrument), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(wt, "wavetable", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(env, "envelope", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(vca, "vca", DeviceKind::AudioEffect), kAppend});
  const std::string userText =
      "patch utility {\n  in x : audio stereo\n  param rate = 2Hz [0.1Hz..20Hz] @unit(Hz) @curve(exp)\n"
      "  param depth = 0.3 [0..1]\n  out out : audio stereo\n  m = meta_phase_acc(rate)\n"
      "  out = x * (1 - depth * m)\n  export lfo = m\n}\n";
  b.commands.push_back(AddDevice{keys, userDevice(user, "utility", userText), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(lfo, "lfo_device", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(scope, "oscilloscope", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(spg, "spectrogram", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(lfo2, "lfo_device", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(pafree, "phase_acc", DeviceKind::AudioEffect, true, "-"), kAppend});
  b.commands.push_back(SetDeviceFree{lfo, true});
  b.commands.push_back(SetDeviceFree{lfo2, true});
  b.commands.push_back(SetDeviceParam{lfo, "rate", 5.0});
  b.commands.push_back(SetDeviceParam{pafree, "freq", 3.0});
  b.commands.push_back(SetDeviceParam{sat, "drive", 6.0});
  b.commands.push_back(SetDeviceSetting{eq, "on1", 1.0});
  b.commands.push_back(SetDeviceSetting{eq, "type1", 6.0});
  b.commands.push_back(SetDeviceParam{eq, "freq1", 3000.0});
  b.commands.push_back(SetParamCurve{eq, "freq1", KnobCurve::Linear});
  b.commands.push_back(SetParamCurve{lfo, "rate", KnobCurve::Exponential});
  b.commands.push_back(SetParamCurve{sat, "drive", KnobCurve::Logarithmic});
  b.commands.push_back(AddDevice{bass, makeDevice(bflt, "auto_filter", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{aud, metaDevice(avca, "vca", DeviceKind::AudioEffect), kAppend});
  if (!ok(store.dispatch(b), "full batch")) return;
  const Project* p = store.current().get();
  run(store, ops::connectWire(*p, ids, lfo, "lfo", sat, "drive"), "w lfo->sat.drive");
  run(store, ops::connectWire(*store.current(), ids, pafree, "out", eq, "freq1"), "w pa->eq.freq1");
  run(store, ops::connectNoteWire(*store.current(), ids, keys, "hz", bflt, "freq"), "note wire keys.hz -> bass filter");
  run(store, ops::connectWire(*store.current(), ids, user, "lfo", util, "gain"), "w user.lfo -> util.gain");
  run(store, ops::connectWire(*store.current(), ids, lfo2, "lfo", avca, "value"), "w lfo2 -> aud vca (cross-track)");
  run(store, ops::connectTravelWire(*store.current(), ids, lfo2, "lfo", util, "width", 0.4), "travel wire");
  // Group the stock series sat → util (lfo free in between: interleaved).
  auto g = ops::groupInCircuit(store.current(), ids, *gLib, std::vector<DeviceId>{sat, util});
  if (run(store, g, "group sat+util")) {}
  ViewState v;
  v.patchNudges[wt.value] = PatchNudge{12.0f, -4.0f, 1.75f};
  v.patchNudges[keys.value] = PatchNudge{0.0f, 30.0f, 1.0f};
  v.patchNudges[scope.value] = PatchNudge{0.0f, 0.0f, 0.5f};
  save("base_full", *store.current(), v);
  std::printf("base_full: %zu wires\n", wiresOf(*store.current()).size());
}

// ---------------------------------------------------------------------------------------------------------------
// Wire-law DC probes: an audio track, a user device whose output is pack(f, f), a free unplugged VCA at a constant c
// wired (Add) into f. `knob` f's value; `curve` its curve. Saved as law_<name> (wired) and law_<name>_ref (no wire,
// knob at `expect`). fuzz_mm.py compares their last samples.

struct LawCase {
  std::string name;
  std::string decl;  // the param declaration after "param f = "
  double knob;
  double c;
  KnobCurve curve;
  double expect;
};

void genLaw(const LawCase& lc, bool wired, bool travel = false, double amount = 1.0) {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId t = ids.track();
  const DeviceId dev = ids.device(), vca = ids.device();
  const std::string tx = "patch utility {\n  in x : audio stereo\n  param f = " + lc.decl +
                         "\n  out out : audio stereo\n  out = pack(f, f)\n}\n";
  Batch b{"law", {}};
  b.commands.push_back(AddTrack{std::make_shared<const Track>(makeTrack(t, TrackKind::Audio, "T")), {}, kAppend});
  b.commands.push_back(AddDevice{t, userDevice(dev, "utility", tx), kAppend});
  b.commands.push_back(SetDeviceParam{dev, "f", wired ? lc.knob : lc.expect});
  if (lc.curve != KnobCurve::Default) b.commands.push_back(SetParamCurve{dev, "f", lc.curve});
  if (wired && travel) {  // T4.7's ends: a Travel wire comes from an export (a free user device exporting c)
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", lc.c);
    b.commands.push_back(AddDevice{t, userDevice(vca, "utility",
                                                 std::string("patch utility {\n  in x : audio stereo\n  out out : audio stereo\n"
                                                             "  out = x\n  export k = ") + buf + "\n}\n", true), kAppend});
  } else if (wired) {
    b.commands.push_back(AddDevice{t, metaDevice(vca, "vca", DeviceKind::AudioEffect, true, "-"), kAppend});
    b.commands.push_back(SetDeviceParam{vca, "min", lc.c});
    b.commands.push_back(SetDeviceParam{vca, "max", lc.c});
  }
  if (!ok(store.dispatch(b), "law batch " + lc.name)) return;
  if (wired) {
    auto e = travel ? ops::connectTravelWire(*store.current(), ids, vca, "k", dev, "f", amount)
                    : ops::connectWire(*store.current(), ids, vca, "out", dev, "f");
    if (!run(store, e, "law wire " + lc.name)) return;
  }
  save("law_" + lc.name + (wired ? "" : "_ref"), *store.current());
}

void genLaws() {
  const double lo = 20, hi = 20000, K = 4.8;
  auto tr = [&](double v) { return std::log(v / lo) / std::log(hi / lo); };
  auto oct = [&](double t) { return lo * std::pow(hi / lo, std::clamp(t, 0.0, 1.0)); };
  auto expo = [&](double t, double k) { t = std::clamp(t, 0.0, 1.0); return lo + (hi - lo) * std::expm1(k * t) / std::expm1(k); };
  auto expoInv = [&](double v, double k) { return std::log1p((v - lo) / (hi - lo) * std::expm1(k)) / k; };
  const std::string hz = "1000 [20..20000] @curve(exp)";
  const std::string lin = "1000 [20..20000]";
  std::vector<LawCase> cases = {
      {"octave_declared", hz, 1000, 0.1, KnobCurve::Default, oct(tr(1000) + 0.1)},
      {"octave_declared_m1", hz, 1000, -1.0, KnobCurve::Default, lo},
      {"octave_declared_plus1", hz, 1000, 1.0, KnobCurve::Default, hi},
      {"octave_switched_linear", hz, 1000, 100.0, KnobCurve::Linear, 1100},
      {"linear_declared", lin, 1000, 100.0, KnobCurve::Default, 1100},
      {"linear_to_octave", lin, 1000, 0.1, KnobCurve::Octave, oct(tr(1000) + 0.1)},
      {"linear_clip_hi", lin, 1000, 1e6, KnobCurve::Default, hi},
      {"exponential", lin, 1000, 0.1, KnobCurve::Exponential, expo(expoInv(1000, K) + 0.1, K)},
      {"logarithmic", lin, 1000, 0.1, KnobCurve::Logarithmic, expo(expoInv(1000, -K) + 0.1, -K)},
      {"db_linear", "0dB [-70dB..35dB] @unit(dB)", 1.0, 6.0, KnobCurve::Default, std::pow(10.0, 6.0 / 20)},
      {"db_minus12", "0dB [-70dB..35dB] @unit(dB)", std::pow(10.0, -12.0 / 20), 6.0, KnobCurve::Default,
       std::pow(10.0, -6.0 / 20)},
      {"db_octave_refused", "0dB [-70dB..35dB] @unit(dB)", 1.0, 6.0, KnobCurve::Octave, std::pow(10.0, 6.0 / 20)},
      {"wrap_linear", "0.3 [0..1] @wrap", 0.3, 0.9, KnobCurve::Default, 1.2},
      {"wrap_exponential", "0.3 [0..1] @wrap", 0.3, 0.9, KnobCurve::Exponential, 0.0},  // expect filled below
      {"signed_linear", "100 [0..1000] @signed", 100, -300, KnobCurve::Default, -200},
      {"signed_linear_floor", "100 [0..1000] @signed", 100, -5000, KnobCurve::Default, -1000},
      {"steps", "2 [0..8] @steps(9)", 2, 2.4, KnobCurve::Default, 4},
      {"unranged", "5", 5, 100, KnobCurve::Default, 105},
  };
  {  // @wrap on an Exponential knob: the travel wraps (frac), value = curve(frac(p0 + c))
    auto e01 = [&](double t) { return std::expm1(K * t) / std::expm1(K); };
    auto i01 = [&](double v) { return std::log1p(v * std::expm1(K)) / K; };
    const double p = i01(0.3) + 0.9;
    cases[13].expect = e01(p - std::floor(p));
  }
  for (const LawCase& c : cases) {
    genLaw(c, true);
    genLaw(c, false);
    std::printf("law %s expect %.9g\n", c.name.c_str(), c.expect);
  }
  // A migrated Travel wire (amount 0.5) on the octave knob: curve(t0 + 0.5 c).
  LawCase t{"travel_octave", hz, 1000, 0.2, KnobCurve::Default, oct(tr(1000) + 0.5 * 0.2)};
  genLaw(t, true, true, 0.5);
  genLaw(t, false);
  std::printf("law %s expect %.9g\n", t.name.c_str(), t.expect);
  // A Travel wire on a knob switched to Linear: travel along the switched curve (Q201 3): lo + (hi-lo)(t0 + 0.1)
  LawCase t2{"travel_linear_switched", hz, 1000, 0.2, KnobCurve::Linear,
             lo + (hi - lo) * ((1000 - lo) / (hi - lo) + 0.5 * 0.2)};
  genLaw(t2, true, true, 0.5);
  genLaw(t2, false);
  std::printf("law %s expect %.9g\n", t2.name.c_str(), t2.expect);
}

// ---------------------------------------------------------------------------------------------------------------
// Group round trips.

struct RT {
  DocumentStore store;
  TrackId keys;
  RT() {
    keys = store.ids().track();
  }
};

void roundTrip(const std::string& name, DocumentStore& store, std::vector<DeviceId> sel, bool viaFile = true) {
  const Snapshot before = store.current();
  auto e = ops::groupInCircuit(store.current(), store.ids(), *gLib, sel);
  if (!e.ok()) {
    std::printf("group %s: REFUSED: %s\n", name.c_str(), e.error().message.c_str());
    return;
  }
  if (!ok(store.dispatch(e->command), "group dispatch " + name)) return;
  const DeviceId g = e->devices.front();
  Snapshot grouped = store.current();
  // Through the file, as a user would: save, load, then ungroup.
  if (viaFile) {
    auto t = serializeProject(*grouped);
    auto back = parseProject(*t);
    if (!okr(back, "reload grouped " + name)) return;
    if (!(*back->project == *grouped)) fail(name + ": grouped project changes through save/load");
    grouped = back->project;
  }
  auto u = ops::ungroupInCircuit(grouped, *gLib, g);
  if (!u.ok()) {
    fail(name + ": ungroup refused: " + u.error().message);
    save("grp_" + name + "_a", *before);
    save("grp_" + name + "_b", *grouped);
    return;
  }
  DocumentStore s2;
  ChangeSet cs;
  auto after = apply(grouped, u->command, cs);
  if (!okr(after, "ungroup apply " + name)) return;
  const bool same = **after == *before;
  const bool sameText = text(**after) == text(*before);
  std::printf("group %s: grouped OK (group %u), ungroup identical: %s (text %s)\n", name.c_str(), g.value,
              same ? "yes" : "NO", sameText ? "yes" : "NO");
  if (!same || !sameText) {
    fail(name + ": ungroup(group(doc)) != doc");
    std::filesystem::create_directories(dir("grp_" + name + "_diff"));
    FILE* f = std::fopen((dir("grp_" + name + "_diff") + "/before.json").c_str(), "wb");
    const std::string a = text(*before), c = text(**after);
    std::fwrite(a.data(), 1, a.size(), f);
    std::fclose(f);
    f = std::fopen((dir("grp_" + name + "_diff") + "/after.json").c_str(), "wb");
    std::fwrite(c.data(), 1, c.size(), f);
    std::fclose(f);
  }
  save("grp_" + name + "_a", *before);
  save("grp_" + name + "_b", *grouped);
  save("grp_" + name + "_c", **after);
}

void genGroups() {
  // G1: interleaved free devices: inst → sat → [free lfo] → util → [free pa] → eq; group {sat, util, eq}.
  {
    DocumentStore s;
    IdSource& ids = s.ids();
    const TrackId k = ids.track();
    const DeviceId inst = ids.device(), sat = ids.device(), lfo = ids.device(), util = ids.device(), pa = ids.device(),
                   eq = ids.device();
    Batch b{"g1", {}};
    b.commands.push_back(AddTrack{midiTrack(k, "K", inst), {}, kAppend});
    b.commands.push_back(AddClip{k, notesClip(ids)});
    b.commands.push_back(AddDevice{k, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(lfo, "lfo_device", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, metaDevice(pa, "phase_acc", DeviceKind::AudioEffect, true, "-"), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(SetDeviceFree{lfo, true});
    b.commands.push_back(SetDeviceParam{sat, "drive", 14.0});
    b.commands.push_back(SetDeviceParam{lfo, "rate", 4.0});
    b.commands.push_back(SetDeviceParam{pa, "freq", 2.0});
    b.commands.push_back(SetDeviceSetting{eq, "on1", 1.0});
    b.commands.push_back(SetDeviceSetting{eq, "type1", 6.0});
    b.commands.push_back(SetDeviceParam{eq, "freq1", 1500.0});
    ok(s.dispatch(b), "g1");
    run(s, ops::connectWire(*s.current(), ids, lfo, "lfo", util, "gain"), "g1 w1");
    run(s, ops::connectWire(*s.current(), ids, pa, "out", eq, "freq1"), "g1 w2");
    roundTrip("interleaved", s, {sat, util, eq});
  }
  // G2: a feedback loop: util.out → sat.x (closes a loop: the 1-sample delay). Group the loop, and only half of it.
  for (int half = 0; half < 2; ++half) {
    DocumentStore s;
    IdSource& ids = s.ids();
    const TrackId k = ids.track();
    const DeviceId inst = ids.device(), sat = ids.device(), util = ids.device(), eq = ids.device();
    Batch b{"g2", {}};
    b.commands.push_back(AddTrack{midiTrack(k, "K", inst), {}, kAppend});
    b.commands.push_back(AddClip{k, notesClip(ids)});
    b.commands.push_back(AddDevice{k, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(SetDeviceParam{util, "gain", 0.3});
    ok(s.dispatch(b), "g2");
    run(s, ops::connectWire(*s.current(), ids, util, "out", sat, "x"), "g2 loop");
    if (half == 0) roundTrip("loop_whole", s, {sat, util});
    else roundTrip("loop_half", s, {util, eq});
  }
  // G3: note-reading chain head: PA (notes) → WT → Env; group {PA, WT}, then {PA, WT, Env}, then {WT, Env}.
  for (int v = 0; v < 3; ++v) {
    DocumentStore s;
    IdSource& ids = s.ids();
    const TrackId k = ids.track();
    const DeviceId pa = ids.device(), wt = ids.device(), env = ids.device(), sat = ids.device();
    Batch b{"g3", {}};
    b.commands.push_back(AddTrack{midiTrack(k, "K"), {}, kAppend});
    b.commands.push_back(AddClip{k, notesClip(ids)});
    b.commands.push_back(AddDevice{k, metaDevice(pa, "phase_acc", DeviceKind::Instrument), kAppend});
    b.commands.push_back(AddDevice{k, metaDevice(wt, "wavetable", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, metaDevice(env, "envelope", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
    if (!ok(s.dispatch(b), "g3")) continue;
    const char* nm[] = {"head_pa_wt", "head_pa_wt_env", "run_wt_env"};
    std::vector<std::vector<DeviceId>> sels = {{pa, wt}, {pa, wt, env}, {wt, env}};
    roundTrip(nm[v], s, sels[v]);
  }
  // G4: a switched-off device in the series; and switched-off at the head.
  for (int v = 0; v < 2; ++v) {
    DocumentStore s;
    IdSource& ids = s.ids();
    const TrackId k = ids.track();
    const DeviceId inst = ids.device(), sat = ids.device(), util = ids.device(), eq = ids.device();
    Batch b{"g4", {}};
    b.commands.push_back(AddTrack{midiTrack(k, "K", inst), {}, kAppend});
    b.commands.push_back(AddClip{k, notesClip(ids)});
    b.commands.push_back(AddDevice{k, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(SetDeviceParam{sat, "drive", 20.0});
    b.commands.push_back(SetDeviceParam{util, "gain", 0.5});
    b.commands.push_back(SetDeviceActive{v == 0 ? util : sat, false});
    if (!ok(s.dispatch(b), "g4")) continue;
    roundTrip(v == 0 ? "off_middle" : "off_head", s, {sat, util, eq});
  }
  // G5: wires crossing the boundary both ways (an outside LFO into a member; a member's export into an outside param).
  {
    DocumentStore s;
    IdSource& ids = s.ids();
    const TrackId k = ids.track();
    const DeviceId inst = ids.device(), sat = ids.device(), util = ids.device(), eq = ids.device(), lfo = ids.device();
    Batch b{"g5", {}};
    b.commands.push_back(AddTrack{midiTrack(k, "K", inst), {}, kAppend});
    b.commands.push_back(AddClip{k, notesClip(ids)});
    b.commands.push_back(AddDevice{k, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(lfo, "lfo_device", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(SetDeviceFree{lfo, true});
    b.commands.push_back(SetDeviceSetting{eq, "on1", 1.0});
    b.commands.push_back(SetDeviceSetting{eq, "type1", 6.0});
    ok(s.dispatch(b), "g5");
    run(s, ops::connectWire(*s.current(), ids, lfo, "lfo", util, "gain"), "g5 w1");
    run(s, ops::connectWire(*s.current(), ids, sat, "driven", eq, "freq1"), "g5 w2");
    run(s, ops::connectWire(*s.current(), ids, inst, "out", util, "x"), "g5 w3 (parallel into member input)");
    roundTrip("crossing", s, {sat, util});
  }
  // G6: two groups on one track; ungroup the first after the second exists.
  {
    DocumentStore s;
    IdSource& ids = s.ids();
    const TrackId k = ids.track();
    const DeviceId inst = ids.device(), sat = ids.device(), util = ids.device(), eq = ids.device(), af = ids.device();
    Batch b{"g6", {}};
    b.commands.push_back(AddTrack{midiTrack(k, "K", inst), {}, kAppend});
    b.commands.push_back(AddClip{k, notesClip(ids)});
    b.commands.push_back(AddDevice{k, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(af, "auto_filter", DeviceKind::AudioEffect), kAppend});
    ok(s.dispatch(b), "g6");
    auto e = ops::groupInCircuit(s.current(), ids, *gLib, std::vector<DeviceId>{eq, af});
    run(s, e, "g6 first group");
    roundTrip("second_group", s, {sat, util});
  }
  // G7: a VCA on an audio track whose chain input is plugged into a non-default input, then grouped.
  {
    DocumentStore s;
    IdSource& ids = s.ids();
    const TrackId k = ids.track();
    const DeviceId inst = ids.device(), vca = ids.device(), sat = ids.device(), pa = ids.device();
    Batch b{"g7", {}};
    b.commands.push_back(AddTrack{midiTrack(k, "K", inst), {}, kAppend});
    b.commands.push_back(AddClip{k, notesClip(ids)});
    b.commands.push_back(AddDevice{k, metaDevice(vca, "vca", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, metaDevice(pa, "phase_acc", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(SetDeviceParam{vca, "value", 0.7});
    ok(s.dispatch(b), "g7");
    roundTrip("vca_pa_tail", s, {vca, sat, pa});
    roundTrip("pa_tail_alone", s, {pa});
  }
  // G8: a group on a note-wired series: note Hz into a member's param.
  {
    DocumentStore s;
    IdSource& ids = s.ids();
    const TrackId k = ids.track();
    const DeviceId inst = ids.device(), af = ids.device(), util = ids.device();
    Batch b{"g8", {}};
    b.commands.push_back(AddTrack{midiTrack(k, "K", inst), {}, kAppend});
    b.commands.push_back(AddClip{k, notesClip(ids)});
    b.commands.push_back(AddDevice{k, makeDevice(af, "auto_filter", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
    ok(s.dispatch(b), "g8");
    run(s, ops::connectNoteWire(*s.current(), ids, k, "hz", af, "freq"), "g8 note wire");
    roundTrip("note_wired_member", s, {af, util});
  }
  // G9: a single device in the instrument slot on a MIDI track whose head is a stock instrument (refused, by design).
  {
    DocumentStore s;
    IdSource& ids = s.ids();
    const TrackId k = ids.track();
    const DeviceId inst = ids.device(), sat = ids.device();
    Batch b{"g9", {}};
    b.commands.push_back(AddTrack{midiTrack(k, "K", inst), {}, kAppend});
    b.commands.push_back(AddDevice{k, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
    ok(s.dispatch(b), "g9");
    roundTrip("stock_instrument", s, {inst, sat});
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  gOut = argv[2];
  std::filesystem::create_directories(gOut);
  auto lib = loadDeviceLibrary(std::string(argv[1]) + "/stdlib");
  if (!lib.ok()) {
    std::printf("library: %s\n", lib.error().message.c_str());
    return 2;
  }
  gLib = std::move(*lib);
  if (argc > 3 && std::string(argv[3]) == "states") {  // probe_mm.exe <root> <project dir> states: the wire states
    auto p = loadProject(argv[2]);
    if (!p.ok()) {
      std::printf("load: %s\n", p.error().message.c_str());
      return 1;
    }
    const char* names[] = {"Lowered", "Delayed", "Inactive", "Invalid", "Refused"};
    for (const WireState& s : wireStates(*p->project, *gLib))
      std::printf("wire %u: %s\n", s.id.value, names[static_cast<int>(s.status)]);
    LowerOptions lo;
    lo.library = &gLib->library();
    lo.devices = gLib.get();
    lo.slotOf = [](TrackId) { return 0u; };
    auto low = lower(*p->project, lo);
    if (!low.ok()) {
      std::printf("lower: %s\n", low.error().message.c_str());
      return 1;
    }
    for (const LoweredParam& x : low->params) std::printf("param %s = %g\n", x.key.c_str(), x.value);
    return 0;
  }
  genFull();
  genLaws();
  genGroups();
  std::printf("probe failures: %d\n", gFail);
  return gFail == 0 ? 0 : 1;
}
