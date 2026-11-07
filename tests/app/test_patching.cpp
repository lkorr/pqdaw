// TX.patching (SPEC §15 2026-11-01 "a wire adds its value in the target param's own units", "free patching in the
// circuit"): wires that ADD in their target's units, from any output into any param or input, at audio rate, and free
// devices (out of the series). Each case renders the lowered project bit for bit like a hand-built ProjectGraph of the
// same math (diff 0) through the interpreter, and the JIT like the interpreter, under the allocation trap:
//   - the note source's Hz into a filter cutoff is +Hz exactly (clamped to the cutoff's range);
//   (GU.4: the Phase accumulator / VCA cases are their modules' now: tests/app/test_module_presets.cpp renders their
//   wires into pm / phase / the VCA's knob against the meta modules' references; TMM.18's reflection is below)
//   - an instrument's output into a later device's audio input sums with the chain (parallel routing);
//   - a free LFO's export wired into a param;
//   - a wire that closes a loop through two inputs gets its 1-sample delay;
// TX.knob-curves: a new (Add) wire adds BEFORE the knob's curve: the cases above that add in units run with the knob on
// its Linear curve (or as a Units wire: TX.patching's law, what a schema-13 … 15 wire migrates to) — the same math, the
// same hand-built graph; tests/app/test_knob_curves.cpp has the curved cases.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/parse.h"
#include "pq/document/modules.h"
#include "pq/meta/wavetable.h"
#include "../support/knob_positions.h"
#include "playback_fixture.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

// Renders a graph through the per-track compile path (the interpreter, or `backend`) with fixed notes on slot 0.
golden::Channels renderGraph(const graph::ProjectGraph& g, const std::vector<engine::ProjectParamLink>& links,
                             const std::vector<float>& values, uint32_t frames, engine::UnitBackend backend = {},
                             std::vector<std::string>* warnings = nullptr) {
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  co.backend = backend;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(g);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  if (warnings != nullptr)
    for (uint32_t u = 0; u < compiler.numUnits(); ++u)
      warnings->insert(warnings->end(), compiler.unit(u).warnings.begin(), compiler.unit(u).warnings.end());
  engine::NoteListSource notes(std::vector<engine::TimedNote>{
      {0, 40000, 220.0f, 0.8f, 0.5f}, {3000, 40000, 277.18f, 0.6f, 0.5f}, {9000, 40000, 329.63f, 0.9f, 0.5f}});
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  pc.eventSources.push_back(notes.source());
  auto prog = engine::ProjectProgram::create(pc, build->layout);
  REQUIRE_MESSAGE(prog.ok(), (prog.ok() ? std::string() : prog.error().message));
  for (uint32_t u = 0; u < build->images.size(); ++u) REQUIRE((*prog)->publish(u, std::move(build->images[u])).ok());
  std::copy(values.begin(), values.end(), (*prog)->projectParams().begin());
  golden::Channels out(2);
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  for (int64_t t = 0; t < frames; t += kBlock) {
    engine::AudioBlock ab{nullptr, 0, ch, 2, kBlock, t};
    CHECK(alloc_trap::countRtViolations([&] {
            DenormalGuard dg;
            (*prog)->process(ab);
          }) == 0);
    out[0].insert(out[0].end(), l.begin(), l.end());
    out[1].insert(out[1].end(), r.begin(), r.end());
  }
  return out;
}

// TX.smooth: the param ramps of a graph's per-unit programs by source: a param by name, a computed param-rate value
// by its ramp's state key (structural: the same in any unit). Every ramp's source is param rate.
std::set<std::string> rampKeys(const graph::ProjectGraph& g, const std::vector<engine::ProjectParamLink>& links) {
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  engine::ProjectCompiler compiler(co);
  REQUIRE(compiler.build(g).ok());
  std::set<std::string> keys;
  for (uint32_t u = 0; u < compiler.numUnits(); ++u) {
    const graph::Schedule& sc = *compiler.unit(u).schedule;
    for (const graph::Ramp& r : sc.ramps) {
      const bool paramRate =
          r.source.kind != graph::Operand::Kind::Buffer || sc.buffers[r.source.index].cls == graph::Buffer::Class::Pattern;
      CHECK(paramRate);
      // A param by its name (its ramp's key also depends on the stage reading it); a computed pattern by its key.
      if (r.source.kind == graph::Operand::Kind::Param)
        keys.insert("param " + std::string(compiler.unit(u).typed->patch.nodeName(sc.params[r.source.index].node)));
      else
        keys.insert("key " + std::to_string(sc.state.entries[sc.ops[r.op].state].key));
    }
  }
  return keys;
}

double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[c][i] - b[c][i])));
  }
  return m;
}

float peakOf(const golden::Channels& a) {
  float p = 0.0f;
  for (const auto& c : a)
    for (float v : c) p = std::max(p, std::fabs(v));
  return p;
}

std::string num(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.17g", v);
  std::string s = b;
  if (s.find_first_of(".e") == std::string::npos) s += ".0";
  return s;
}

// The mixer stages of lower.h for a MIDI track (constant-power pan, out / post / pre) as text, after signal `x`.
std::string midiStages(const std::string& x) {
  return "  param volume = 0 [-70..6]\n  param pan = 0 [-1..1]\n  param gate = 1 [0..1]\n  param sendGate = 1 [0..1]\n"
         "  param mute = 0 [0..1]\n"
         "  vg = volume <= -70 ? 0 : db_to_gain(volume)\n"
         "  th = (pan + 1) * 0.78539816339744828\n  pl = sin(th + 1.5707963267948966)\n  pr = sin(th)\n"
         "  un = mute < 0.5\n  go = un ? gate : 0\n  so = un ? sendGate : 0\n"
         "  out = gain(" + x + ", pack(pl * (vg * go), pr * (vg * go)), 0.002)\n"
         "  export post = gain(" + x + ", pack(pl * (vg * so), pr * (vg * so)), 0.002)\n"
         "  export pre = gain(" + x + ", pack(pl * so, pr * so), 0.002)\n";
}
const char* kMainText =
    "patch Main {\n  param volume = 0 [-70..6]\n  param pan = 0 [-1..1]\n  in in : audio global stereo\n"
    "  out out : audio global stereo\n  vg = volume <= -70 ? 0 : db_to_gain(volume)\n"
    "  out = gain(in, pack(min(1, 1 - pan) * vg, min(1, 1 + pan) * vg), 0.002)\n}\n";

std::string id(auto x) { return std::to_string(x.value); }

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

// A chain meta device of `type` (its default state), optionally free (out of the series) or unplugged.
// TX.all-free: the devices made `free` (placed off the main path): wired() leaves them out of the series.
std::vector<DeviceId> gOffPath;

DevicePtr metaDevice(DeviceId id, std::string_view type, DeviceKind kind, bool free = false, std::string chainInput = {}) {
  const meta::Type* t = meta::findType(type);
  REQUIRE(t != nullptr);
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kMetaDevicePrefix) + std::string(type), kind));
  d->meta = std::make_shared<const MetaMap>(MetaMap{MetaEntry{"", defaultMetaInstance(*t)}});
  if (free) gOffPath.push_back(id);
  d->chainInput = std::move(chainInput);
  return d;
}

// GU.4: a free Value block that is the constant `v` (its curve min = max = v): what an unwired VCA was (a constant source
// of its value), wired from its output `y`.
DevicePtr constantDevice(DeviceId id, double v) {
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kValueDevice), DeviceKind::AudioEffect));
  d->curves = {ControlCurve{std::string(kValueParam), dsp::curvePreset(dsp::CurvePreset::Linear, v, v)}};
  gOffPath.push_back(id);
  return d;
}

// GU.4: a Phase Accumulator module (its group's id `id`, the members fresh) as AddDevice commands into `t` (the group
// first), and its internal wires as AddWire commands after them; `roles` its blocks' ids.
void addPhaseAccumulator(Batch& b, IdSource& ids, TrackId t, DeviceId id, PhaseAccumulatorRoles& roles, bool free = false) {
  PhaseAccumulatorOptions o;
  o.id = id;
  const BlockSet set = phaseAccumulatorBlocks(ids, o, &roles);
  for (const DevicePtr& d : set.devices) b.commands.push_back(AddDevice{t, d, kAppend});
  for (const Wire& w : set.wires) b.commands.push_back(AddWire{w});
  for (const DevicePtr& d : set.devices)
    if (d->group.valid() || free) gOffPath.push_back(d->id);
}

// TX.all-free: every track built from raw commands wired in series (the devices made free left off the path).
void wired(DocumentStore& s, std::vector<DeviceId> offPath = {}) {
  offPath.insert(offPath.end(), gOffPath.begin(), gOffPath.end());
  gOffPath.clear();
  playback_fixture::wireSeries(s, offPath);
}

// The lowered project of `store` (one MIDI event slot) and its param links.
struct Lowered {
  LoweredProject low;
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  std::vector<WireState> cables;  // TX.all-free: the states of the wires that are not main links (the ones a test draws)
};
Lowered lowerIt(const DocumentStore& store) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  Project gp = *store.current();
  (void)migrateToInstrumentGroups(gp);  // IG.1-2: as an older file of it loads (its voice run in an instrument group)
  auto low = lower(gp, lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  Lowered out{std::move(*low), {}, {}, {}};
  for (const WireState& st : out.low.wires)
    if (const Wire* w = findWire(*store.current(), st.id); w != nullptr && !w->mainLink() && !w->chain) out.cables.push_back(st);
  for (const LoweredParam& p : out.low.params) {
    out.links.push_back({p.track, p.name, static_cast<uint32_t>(out.values.size())});
    out.values.push_back(p.value);
  }
  return out;
}

// A hand-built project graph: track 0 (`name`, an instrument track on slot 0 when `instrument`) → Main.
struct Hand {
  graph::PatchLibrary lib;
  std::shared_ptr<graph::PatchFile> file;
  graph::ProjectGraph g;
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  void link(uint32_t track, const std::string& name, float v) {
    links.push_back({track, name, static_cast<uint32_t>(values.size())});
    values.push_back(v);
  }
};
std::unique_ptr<Hand> hand(const std::string& trackText, uint32_t trackId, uint32_t masterId, bool instrument) {
  auto h = std::make_unique<Hand>();
  graph::ParseOptions po;
  po.externals = library().signatures();
  const std::string text = trackText + kMainText;
  auto file = graph::parse(text, "hand.pqp", po);
  REQUIRE_MESSAGE(file.ok(), (file.ok() ? std::string() : file.error().message + "\n" + text));
  h->file = std::make_shared<graph::PatchFile>(std::move(*file));
  h->lib.add(*h->file);
  h->lib.addAll(library().library());
  h->g.library = &h->lib;
  h->g.tracks.push_back({trackId, "T", h->file->find("T"), instrument ? graph::TrackRole::Instrument : graph::TrackRole::Audio,
                         instrument ? 0u : graph::kNone});
  h->g.tracks.push_back({masterId, "Main", h->file->find("Main"), graph::TrackRole::Master, graph::kNone});
  h->g.wires.push_back({{0, "out"}, {1, "in"}, graph::WireKind::Audio});
  h->g.output = {1, "out"};
  for (const char* p : {"volume", "pan", "gate", "sendGate", "mute"})
    h->link(0, p, std::string_view(p) == "gate" || std::string_view(p) == "sendGate" ? 1.0f : 0.0f);
  h->link(1, "volume", 0.0f);
  h->link(1, "pan", 0.0f);
  return h;
}

// Renders `low` and `h` and checks they are equal (and the JIT equals the interpreter); returns the interpreter render.
golden::Channels sameAsHand(const Lowered& low, const Hand& h, uint32_t frames, const std::string& what) {
  std::vector<std::string> warnings;
  const golden::Channels lowered = renderGraph(low.low.graph, low.links, low.values, frames, {}, &warnings);
  for (const std::string& w : warnings) MESSAGE(what << ": " << w);
  graph::ProjectGraph hg = h.g;  // GU.1: the hand graph's `curve` atoms read the lowering's own curve slots
  hg.curveSlots = low.low.graph.curveSlots;
  hg.curveOwner = low.low.graph.curveOwner;
  const golden::Channels manual = renderGraph(hg, h.links, h.values, frames);
  CHECK(peakOf(manual) > 0.01f);
  const double diff = maxDiff(lowered, manual);
  MESSAGE(what << ": lowered vs hand-built graph, max |diff| = " << diff);
  CHECK(diff == 0.0);
  const golden::Channels jit = renderGraph(low.low.graph, low.links, low.values, frames, compile::jitUnitBackend());
  const double dj = maxDiff(lowered, jit);
  MESSAGE(what << ": interpreter vs JIT, max |diff| = " << dj);
  CHECK(dj == 0.0);
  return lowered;
}

// The range of a device param as its patch declares it (the Add law clamps to it).
std::pair<double, double> rangeOf(const char* patch, const char* param) {
  const graph::Patch* p = library().library().find(patch);
  REQUIRE(p != nullptr);
  const auto n = p->findByName(param);
  REQUIRE(n.has_value());
  const graph::ParamDecl& d = p->paramDecl(*n);
  return {d.rangeMin.value(), d.rangeMax.value()};
}

// TX.knob-curves: the wires an edit adds, under TX.patching's units law (WireLaw::Units: a migrated schema-15 wire).
void asUnits(ops::EditResult& e) {
  REQUIRE(e.ok());
  if (auto* b = std::get_if<Batch>(&e->command.v))
    for (Command& c : b->commands)
      if (auto* a = std::get_if<AddWire>(&c.v)) a->wire.law = WireLaw::Units;
}

// GU.1: the curve of `patch`'s param `param` on `preset` (doc::curveWithPreset over its declaration), the slot a
// lowering gave a driven control's `curve` atom, and the knob position a device of the store holds.
dsp::Curve presetCurve(const char* patch, const char* param, dsp::CurvePreset preset) {
  const graph::Patch* p = library().library().find(patch);
  REQUIRE(p != nullptr);
  const graph::ParamDecl* d = findParamDecl(*p, param);
  REQUIRE(d != nullptr);
  return curveWithPreset(declaredCurve(*d), preset);
}
std::string slotOf(const Lowered& low, DeviceId d, std::string_view param) {
  for (const CurveSite& c : low.low.curves)
    if (c.device == d && c.param == param) return std::to_string(c.slot);
  FAIL("no curve site for " << param);
  return {};
}
double knobOf(const DocumentStore& store, DeviceId d, const char* param) {
  const Device* dev = findDevice(*store.current(), d).device;
  REQUIRE(dev != nullptr);
  REQUIRE(dev->param(param) != nullptr);
  return dev->param(param)->value;
}

const WireState* stateOf(const LoweredProject& low, WireId w) {
  for (const WireState& s : low.wires)
    if (s.id == w) return &s;
  return nullptr;
}

}  // namespace

TEST_CASE("patching: the note source's Hz into a filter cutoff adds exactly its Hz (hand-built graph diff 0, JIT)") {
  // TX.knob-curves: on the cutoff's Linear curve (an Add wire), and as a Units wire on its default octave curve.
  for (const bool units : {false, true}) {
  CAPTURE(units);
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), eq = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceSetting{eq, "on1", 1.0});
  b.commands.push_back(SetDeviceSetting{eq, "type1", 6.0});  // High Cut 12
  if (!units) b.commands.push_back(SetParamCurve{eq, "freq1", presetCurve("eq_eight", "freq1", dsp::CurvePreset::Linear), true});
  b.commands.push_back(SetDeviceParam{eq, "freq1", 300.0});  // (GU.1: 300 Hz on its curve: knobPositions)
  REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
  wired(store);  // TX.all-free: the chain as cables
  playback_fixture::asInstrumentGroups(store);  // IG.1-2: the voice run in its instrument group
  auto e = ops::connectNoteWire(*store.current(), ids, keys, "hz", eq, "freq1");
  REQUIRE(e.ok());
  if (units) asUnits(e);
  REQUIRE(store.dispatch(e->command).ok());
  const Wire& w = *findWire(*store.current(), e->wires.front());  // (TX.all-free: beside the main links)
  CHECK(w.law == (units ? WireLaw::Units : WireLaw::Add));
  CHECK(w.amount == 1.0);
  const Lowered low = lowerIt(store);
  REQUIRE(low.cables.size() == 1);
  CHECK(low.cables[0].status == WireStatus::Lowered);
  REQUIRE(low.low.taps.size() == 1);
  CHECK(low.low.taps[0].domain == (units ? ModulationTap::Domain::Dsp : ModulationTap::Domain::Curve));
  // No amount param for an added wire.
  for (const LoweredParam& p : low.low.params) CHECK(p.key.find(":amount") == std::string::npos);
  // value = clip(curve(knob) + Σ voices' Hz, lo, hi) in Hz: the voices' pitches summed (a global target reduces them).
  // GU.1: the knob is a position; its curve atom gives 300 Hz (Linear, or the octave curve under the Units wire).
  const auto [lo, hi] = rangeOf("eq_eight", "freq1");
  const double f1 = knobOf(store, eq, "freq1");
  const std::string text = "patch T {\n  param f1 = " + num(f1) + " [0..1]\n  out out : audio global stereo\n"
                           "  out done : bool voice mono\n  s = BasicSynth()\n  x = voice_sum(s.out)\n"
                           "  fu = curve(f1, 1, " + slotOf(low, eq, "freq1") + ")\n"
                           "  fv = clip(fu + voice_sum(voice.hz), " + num(lo) + ", " + num(hi) + ")\n"
                           "  e = eq_eight(pack(x, x), on1: 1, type1: 6, freq1: fv)\n"
                           "  export tap_d" + id(eq) + "_freq1 = pack(max(fv - fu, 0), max(fu - fv, 0))\n"
                           "  done = s.done\n" + midiStages("e.out") + "}\n";
  auto h = hand(text, keys.value, store.current()->master->id.value, true);
  h->link(0, "f1", static_cast<float>(f1));
  (void)sameAsHand(low, *h, 24000, units ? "Hz into cutoff (Units wire)" : "Hz into a Linear cutoff");
  }
}

TEST_CASE("patching: an instrument's output into a later device's audio input sums with the chain (parallel routing)") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), u1 = ids.device(), u2 = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(u1, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(u2, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceParam{u1, "width", 0.0});  // the chain's signal through u1 in mono …
  REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
  wired(store);  // TX.all-free: the chain as cables
  playback_fixture::asInstrumentGroups(store);  // IG.1-2: the voice run in its instrument group
  auto e = ops::connectWire(*store.current(), ids, inst, "out", u2, "x");  // … plus the instrument directly into u2
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const Lowered low = lowerIt(store);
  REQUIRE(low.cables.size() == 1);
  CHECK(low.cables[0].status == WireStatus::Lowered);
  CHECK(low.low.taps.empty());  // an input has no knob: no tap
  const std::string text = "patch T {\n  param w1 = 0 [0..4]\n  out out : audio global stereo\n  out done : bool voice mono\n"
                           "  s = BasicSynth()\n  x = voice_sum(s.out)\n  a = utility(pack(x, x), width: w1)\n"
                           "  c = utility(a.out + voice_sum(s.out))\n  done = s.done\n" + midiStages("c.out") + "}\n";
  auto h = hand(text, keys.value, store.current()->master->id.value, true);
  h->link(0, "w1", 0.0f);
  (void)sameAsHand(low, *h, 24000, "parallel input");
}

TEST_CASE("patching: a free LFO's export wired into a param; a free device costs nothing while nothing reads it") {
  // The cutoff on its declared octave curve, so the wire adds in knob position (GU.1: value = curve(knob + lfo)).
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), eq = ids.device(), lfo = ids.device(), idle = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(lfo, "lfo_device", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(idle, "saturator", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceSetting{eq, "on1", 1.0});
  b.commands.push_back(SetDeviceSetting{eq, "type1", 6.0});
  b.commands.push_back(SetDeviceParam{eq, "freq1", 400.0});
  b.commands.push_back(SetDeviceParam{lfo, "rate", 3.0});
  REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
  wired(store, {lfo, idle});  // TX.all-free: the chain as cables, the LFO and the idle Saturator off the path
  CHECK(!playback_fixture::onPath(*store.current(), lfo));
  auto e = ops::connectWire(*store.current(), ids, lfo, "lfo", eq, "freq1");
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const Lowered low = lowerIt(store);
  REQUIRE(low.cables.size() == 1);
  CHECK(low.cables[0].status == WireStatus::Lowered);
  // Both free devices are calls of the track patch (the views draw them), out of the chain: the chain is the
  // instrument → EQ → mixer; the unread saturator is dropped by the compiler.
  const graph::Patch& tp = *low.low.graph.tracks[0].patch;
  CHECK(tp.findByName("d" + id(lfo)).has_value());
  CHECK(tp.findByName("d" + id(idle)).has_value());
  // GU.1: the knob position + the LFO in position, through the curve atom (the old travel map without an amount).
  const double f1 = knobOf(store, eq, "freq1");
  const std::string text = "patch T {\n  param f1 = " + num(f1) + " [0..1]\n  param r = 3\n"
                           "  out out : audio global stereo\n  out done : bool voice mono\n"
                           "  s = BasicSynth()\n  x = voice_sum(s.out)\n  l = lfo_device(-0.0, rate: r)\n"
                           "  p = f1 + channel(l.lfo, 0)\n"
                           "  fv = curve(p, 1, " + slotOf(low, eq, "freq1") + ")\n"
                           "  e = eq_eight(pack(x, x), on1: 1, type1: 6, freq1: fv)\n"
                           "  export tap_d" + id(eq) + "_freq1 = pack(max(p - f1, 0), max(f1 - p, 0))\n"
                           "  done = s.done\n" + midiStages("e.out") + "}\n";
  auto h = hand(text, keys.value, store.current()->master->id.value, true);
  h->link(0, "f1", static_cast<float>(f1));
  h->link(0, "r", 3.0f);
  (void)sameAsHand(low, *h, 24000, "free LFO into a param");
}

TEST_CASE("patching: a wire from a later device's output into an earlier one's input closes a loop: 1-sample delay") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), u1 = ids.device(), u2 = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(u1, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(u2, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceParam{u2, "gain", std::pow(10.0, -6.0 / 20.0)});  // −6 dB: the loop decays
  REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
  wired(store);  // TX.all-free: the chain as cables
  playback_fixture::asInstrumentGroups(store);  // IG.1-2: the voice run in its instrument group
  auto e = ops::connectWire(*store.current(), ids, u2, "out", u1, "x");
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const Lowered low = lowerIt(store);
  REQUIRE(low.cables.size() == 1);
  CHECK(low.cables[0].status == WireStatus::Delayed);
  const float g = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  const std::string text = "patch T {\n  param g2 = " + num(g) + "\n  out out : audio global stereo\n"
                           "  out done : bool voice mono\n  s = BasicSynth()\n  x = voice_sum(s.out)\n"
                           "  a = utility(pack(x, x) + z1(c.out))\n  c = utility(a.out, gain: g2)\n  done = s.done\n" +
                           midiStages("c.out") + "}\n";
  auto h = hand(text, keys.value, store.current()->master->id.value, true);
  h->link(0, "g2", g);
  (void)sameAsHand(low, *h, 24000, "loop through two inputs");
}

TEST_CASE("patching: a feedback wire through the voices' sum is refused, naming why; the track still plays (TMM.13-fix1 H3)") {
  // The review's loop_voice: a meta module at the notes (GU.4: an Envelope, was a Phase accumulator) → Wavetable →
  // Saturator, with the Saturator's output wired back into the Envelope's in. The loop passes the voice run's voice_sum (per-voice → global), so its
  // 1-sample delay would need the voices and the global part to alternate per sample: the scheduler refused the whole
  // unit ("feedback loop through add#10 crosses the voice/global boundary") and the track fell silent while the wire
  // showed as Delayed. Now the wire alone is refused (VoiceLoop) and everything else plays.
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId pa = ids.device(), wt = ids.device(), sat = ids.device(), pa2 = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys"), {}, kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(pa, "envelope", DeviceKind::Instrument), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(wt, "wavetable", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(pa2, "wavetable", DeviceKind::AudioEffect, true), kAppend});
  REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
  wired(store);  // TX.all-free: the chain as cables
  playback_fixture::asInstrumentGroups(store);  // IG.1-2: the voice run in its instrument group
  const Lowered plain = lowerIt(store);
  const golden::Channels before = renderGraph(plain.low.graph, plain.links, plain.values, 12000);
  CHECK(peakOf(before) > 0.01f);
  auto e = ops::connectWire(*store.current(), ids, sat, "out", pa, "in");
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(store.dispatch(e->command).ok());
  const WireId loop = doc::wiresOf(*store.current()).back().id;
  {
    const Lowered low = lowerIt(store);
    const WireState* st = stateOf(low.low, loop);
    REQUIRE(st != nullptr);
    CHECK(st->status == WireStatus::Refused);
    CHECK(st->reason == WireRefusal::VoiceLoop);
    CHECK(wireStates(*store.current(), library()) == low.low.wires);
    // It compiles (renderGraph requires it) and plays as without the wire.
    const golden::Channels after = renderGraph(low.low.graph, low.links, low.values, 12000);
    CHECK(maxDiff(before, after) == 0.0);
  }
  // A loop inside the voice run stays a per-voice delayed loop (the Wavetable's output into the Envelope's in: per-voice
  // feedback), and so does one through a free device the voices feed (inferred per voice).
  auto inRun = ops::connectWire(*store.current(), ids, wt, "out", pa, "in");
  REQUIRE(inRun.ok());
  REQUIRE(store.dispatch(inRun->command).ok());
  const WireId runLoop = doc::wiresOf(*store.current()).back().id;
  // (IG.1-2: the free Wavetable inside the run's instrument group, so its loop stays inside: per voice.)
  REQUIRE(store.dispatch(SetDeviceGroup{pa2, findDevice(*store.current(), pa).device->group}).ok());
  auto toFree = ops::connectWire(*store.current(), ids, wt, "out", pa2, "x");
  REQUIRE(toFree.ok());
  REQUIRE(store.dispatch(toFree->command).ok());
  auto fromFree = ops::connectWire(*store.current(), ids, pa2, "out", wt, "pos");
  REQUIRE(fromFree.ok());
  REQUIRE(store.dispatch(fromFree->command).ok());
  const WireId freeLoop = doc::wiresOf(*store.current()).back().id;
  const Lowered low = lowerIt(store);
  CHECK(stateOf(low.low, loop)->status == WireStatus::Refused);
  CHECK(stateOf(low.low, runLoop)->status == WireStatus::Delayed);
  CHECK(stateOf(low.low, freeLoop)->status == WireStatus::Delayed);
  const golden::Channels fm = renderGraph(low.low.graph, low.links, low.values, 12000);
  CHECK(peakOf(fm) > 0.01f);
  CHECK(maxDiff(before, fm) > 0.0);  // the per-voice loops play
  // Through the link: no compile error, the track plays, the refusal is counted.
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto link = app::ProjectLink::create(c, store.current());
  REQUIRE(link.ok());
  CHECK((*link)->lastError().empty());
  CHECK((*link)->stats().refusedWires == 1);
}

TEST_CASE("patching: an audio-rate wire costs per sample only where it is; an unwired meta param stays param rate") {
  // The unwired expansion is the one before TX.patching (no extra inputs, same key). (GU.4: on the Wavetable; it was the
  // Phase accumulator's.)
  const meta::Type& t = meta::wavetableType();
  meta::ExpandContext c;
  c.form = meta::Form::ChainNotes;
  auto plain = meta::expand(t, c, "p");
  REQUIRE(plain.ok());
  const std::vector<uint8_t> none(t.inputs.size(), 0);
  c.wired = none;  // nothing wired: the same patch and key as before
  auto same = meta::expand(t, c, "p");
  REQUIRE(same.ok());
  CHECK(same->key == plain->key);
  CHECK(same->patch == plain->patch);
  CHECK_FALSE(plain->patch.findByName("pos").has_value());  // ChainNotes: the note's phase and the defaults
  const std::vector<uint8_t> pos{0, 1};
  c.wired = pos;
  auto wired = meta::expand(t, c, "p");
  REQUIRE(wired.ok());
  CHECK(wired->key != plain->key);
  CHECK(wired->patch.findByName("pos").has_value());
}

TEST_CASE("patching (TMM.18): a negative frequency reflects — into the Phase Accumulator's Frequency (signed) or its In") {
  // GU.4: the Phase Accumulator module on an audio track (its In the track's silence: it runs freely at its Frequency)
  // and a constant ±440 (a Value block) wired into its Frequency value block (on a Linear curve: an Add wire in Hz,
  // signed: ±20 kHz; on its default exp0 curve: in position, the signed curve mirroring it, so −p plays exactly −f) or
  // into its In (the hz input: never clamped). The phasor's uint32 increment at −440 is exactly the negated one (the
  // start glides of TX.smooth are the same ramp mirrored), so the backward ramp is the forward one reversed:
  // out(−f) + out(f) = the stage gain (1 − 2^−24 where the phase's low bits are not 0), sample for sample.
  enum Into { kFreqLinear, kFreqCurved, kHz };
  auto render = [](Into into, double hz, engine::UnitBackend backend) {
    const dsp::Curve exp0 = phaseAccumulatorCurve(kPaFrequency);
    const double v = into == kFreqCurved ? std::copysign(dsp::curvePosition(exp0, std::fabs(hz), 0.0), hz) : hz;
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId t = ids.track();
    const DeviceId pa = ids.device(), src = ids.device();
    Batch b{"t", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Audio, "T"), {}, kAppend});
    PhaseAccumulatorRoles roles;
    addPhaseAccumulator(b, ids, t, pa, roles);
    b.commands.push_back(AddDevice{t, constantDevice(src, v), kAppend});
    if (into == kFreqLinear) {
      dsp::Curve lin = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 20000.0);
      lin.unit = dsp::CurveUnit::Hz;
      lin.mode = dsp::CurveMode::Signed;
      b.commands.push_back(SetParamCurve{roles.freq, std::string(kValueParam), lin, true});
    }
    REQUIRE(store.dispatch(b).ok());
    wired(store);  // TX.all-free: the path as cables (the module's group on it)
    auto e = ops::connectWire(*store.current(), ids, src, std::string(kValueOutput), into == kHz ? pa : roles.freq,
                              into == kHz ? std::string() : std::string(kValueParam));
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    const Lowered low = lowerIt(store);
    for (const WireState& s : low.low.wires) CHECK(s.status == WireStatus::Lowered);
    return renderGraph(low.low.graph, low.links, low.values, 9600, backend);
  };
  for (const Into into : {kFreqLinear, kFreqCurved, kHz}) {
    CAPTURE(static_cast<int>(into));
    const bool intoFreq = into != kHz;
    const golden::Channels fwd = render(into, 440.0, {});
    const golden::Channels bwd = render(into, -440.0, {});
    CHECK(maxDiff(bwd, render(into, -440.0, compile::jitUnitBackend())) == 0.0);
    float gain = 0.0f;
    for (size_t i = 0; i < fwd[0].size(); ++i) gain = std::max(gain, fwd[0][i]);
    REQUIRE(gain > 0.3f);
    // From 50 ms on (the stage's fade-in is over): every sum is the stage gain g (to the truncation).
    double lo = 1e9, hi = -1e9;
    uint32_t compared = 0;
    for (size_t i = 2400; i < fwd[0].size(); ++i) {
      if (fwd[0][i] == 0.0f && bwd[0][i] == 0.0f) continue;  // the phase exactly 0
      ++compared;
      const double sum = static_cast<double>(fwd[0][i]) + bwd[0][i];
      lo = std::min(lo, sum);
      hi = std::max(hi, sum);
    }
    const double worst = hi - lo;
    MESSAGE(std::string(intoFreq ? "Frequency" : "In") << " wired at -440 Hz: out(-f) + out(f) in [" << lo << ", " << hi
                                                         << "] (stage gain " << gain << ")");
    CHECK(hi <= 1.0001 * gain / (1.0 - 440.0 / 48000.0));
    CHECK(compared > 7000);
    CHECK(worst <= 2.0 * gain * 5.9604644775390625e-08 + 1e-7);
  }
}


// ---- TX.knob-curves (SPEC §15 2026-11-01 "answers to Q186" + clarification: a wire adds before the knob's curve) ----

namespace {

// Keys: BasicSynth → `target` (an effect, `param` its knob at `knob`), and a FREE constant `value` (GU.4: a Value block
// whose curve is that constant; it was an unwired VCA) wired into `param` with an Add wire — or no wire, `param`'s knob
// at `knob`. `curve` the knob's curve.
Lowered constantInto(const char* target, const char* param, double knob, std::optional<double> value,
                     std::optional<dsp::CurvePreset> curve = std::nullopt, std::vector<Command> extra = {}) {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), dev = ids.device(), vca = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(dev, target, DeviceKind::AudioEffect), kAppend});
  for (Command& c : extra) {
    if (auto* s = std::get_if<SetDeviceSetting>(&c.v)) s->device = dev;
    b.commands.push_back(std::move(c));
  }
  if (curve) b.commands.push_back(SetParamCurve{dev, param, presetCurve(target, param, *curve), true});
  b.commands.push_back(SetDeviceParam{dev, param, knob});  // (GU.1: in units on that curve: knobPositions)
  REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
  if (value) REQUIRE(store.dispatch(AddDevice{keys, constantDevice(vca, *value), kAppend}).ok());
  wired(store);  // TX.all-free: the chain as cables
  playback_fixture::asInstrumentGroups(store);  // IG.1-2: the voice run in its instrument group
  if (value) {
    auto e = ops::connectWire(*store.current(), ids, vca, std::string(kValueOutput), dev, param);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  }
  Lowered low = lowerIt(store);
  for (const WireState& s : low.low.wires) CHECK(s.status == WireStatus::Lowered);
  return low;
}

// The relative difference of two renders after the first 50 ms (a knob's start glide, TX.smooth, against a wired value
// that has none): max |a − b| / peak of b.
double relDiff(const golden::Channels& a, const golden::Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c)
    for (size_t i = 2400; i < a[c].size() && i < b[c].size(); ++i)
      m = std::max(m, static_cast<double>(std::fabs(a[c][i] - b[c][i])));
  return m / std::max(1e-9, static_cast<double>(peakOf(b)));
}

}  // namespace

TEST_CASE("knob curves: the octave cutoff + a wire of ±0.5 = ± half the range in octaves (interpreter == JIT)") {
  // EQ Eight's band 1 as a High Cut 12 at 400 Hz (10 Hz … 22 kHz, @curve(exp)): a constant ±0.5 through an Add wire
  // plays the cutoff at 400 · (22000 / 10)^±0.5 — half the knob's travel up or down, in octaves (the user: "with the
  // cutoff on its octave curve, a wire carrying +1 adds +1 knob travel"). The same render as the knob set there by hand
  // (to float rounding: the wire's path computes the value per sample); far from the TX.patching law (400 ± 0.5 Hz).
  const auto [lo, hi] = rangeOf("eq_eight", "freq1");
  std::vector<Command> eq{SetDeviceSetting{{}, "on1", 1.0}, SetDeviceSetting{{}, "type1", 6.0}};
  for (const double s : {0.5, -0.5}) {
    CAPTURE(s);
    const double expect = 400.0 * std::pow(hi / lo, s);
    const Lowered wired = constantInto("eq_eight", "freq1", 400.0, s, std::nullopt, eq);
    const Lowered byHand = constantInto("eq_eight", "freq1", expect, std::nullopt, std::nullopt, eq);
    const Lowered unitsLaw = constantInto("eq_eight", "freq1", 400.0 + s, std::nullopt, std::nullopt, eq);
    REQUIRE(wired.low.taps.size() == 1);
    CHECK(wired.low.taps[0].domain == ModulationTap::Domain::Position);  // the tap is in knob position
    const golden::Channels a = renderGraph(wired.low.graph, wired.links, wired.values, 9600);
    const golden::Channels j = renderGraph(wired.low.graph, wired.links, wired.values, 9600, compile::jitUnitBackend());
    CHECK(maxDiff(a, j) == 0.0);
    const golden::Channels h = renderGraph(byHand.low.graph, byHand.links, byHand.values, 9600);
    const golden::Channels u = renderGraph(unitsLaw.low.graph, unitsLaw.links, unitsLaw.values, 9600);
    MESSAGE("cutoff 400 Hz + " << s << " travel: vs the knob at " << expect << " Hz " << relDiff(a, h)
                               << ", vs 400 + " << s << " Hz " << relDiff(a, u));
    CHECK(relDiff(a, h) < 1e-4);
    CHECK(relDiff(a, u) > 0.05);
  }
  // The law itself, exactly: on the octave curve, +0.5 of the knob's travel from any knob is × √(hi / lo).
  const dsp::Curve t = presetCurve("eq_eight", "freq1", dsp::CurvePreset::Octave);
  for (const double knob : {20.0, 100.0, 400.0})  // (+0.5 stays inside the range)
    CHECK(dsp::evalCurve(t, dsp::curvePosition(t, knob, 0.0) + 0.5) == doctest::Approx(knob * std::sqrt(hi / lo)).epsilon(1e-12));
}

TEST_CASE("knob curves: a dB knob + 6 = +6 dB (the param and its wires in dB; interpreter == JIT)") {
  // Utility's gain (−70 … +35 dB, a linear gain inside): a constant 6 through an Add wire on 0 dB plays +6 dB, as the
  // knob at +6 dB by hand; on −12 dB it plays −6 dB. TX.patching added 6 to the linear gain (×7).
  for (const double knobDb : {0.0, -12.0}) {
    CAPTURE(knobDb);
    auto gain = [](double db) { return std::pow(10.0, db / 20.0); };
    const Lowered wired = constantInto("utility", "gain", gain(knobDb), 6.0);
    const Lowered byHand = constantInto("utility", "gain", gain(knobDb + 6.0), std::nullopt);
    REQUIRE(wired.low.taps.size() == 1);
    CHECK(wired.low.taps[0].domain == ModulationTap::Domain::Curve);  // in dB
    const golden::Channels a = renderGraph(wired.low.graph, wired.links, wired.values, 9600);
    CHECK(maxDiff(a, renderGraph(wired.low.graph, wired.links, wired.values, 9600, compile::jitUnitBackend())) == 0.0);
    const golden::Channels h = renderGraph(byHand.low.graph, byHand.links, byHand.values, 9600);
    MESSAGE("gain " << knobDb << " dB + 6: vs the knob at " << knobDb + 6.0 << " dB by hand " << relDiff(a, h));
    CHECK(relDiff(a, h) < 1e-5);
  }
  // On an Exponential curve (bent in dB) the same wire adds travel: +6 is past the end (clipped to +35 dB).
  const Lowered bent = constantInto("utility", "gain", 1.0, 6.0, dsp::CurvePreset::Exponential);
  const Lowered top = constantInto("utility", "gain", std::pow(10.0, 35.0 / 20.0), std::nullopt);
  CHECK(bent.low.taps[0].domain == ModulationTap::Domain::Position);
  const golden::Channels a = renderGraph(bent.low.graph, bent.links, bent.values, 9600);
  const golden::Channels h = renderGraph(top.low.graph, top.links, top.values, 9600);
  CHECK(relDiff(a, h) < 1e-5);
}

TEST_CASE("knob curves: the Phase Accumulator's Frequency — exp0 by default (in position), its Pitch linear (cents)") {
  // GU.4: the module on an audio track (it runs freely at its Frequency): a constant wired into its Frequency value
  // block (knob 0 Hz) plays the bar at that position, through its curve — the same as the knob put there; into its
  // Pitch (linear, cents) it adds cents.
  auto pa = [](double freq, double pitch, std::optional<std::pair<bool, double>> wire) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId t = ids.track();
    const DeviceId acc = ids.device(), src = ids.device();
    Batch b{"t", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Audio, "T"), {}, kAppend});
    PhaseAccumulatorRoles roles;
    addPhaseAccumulator(b, ids, t, acc, roles);
    b.commands.push_back(SetDeviceParam{roles.freq, std::string(kValueParam),
                                        dsp::curvePosition(phaseAccumulatorCurve(kPaFrequency), freq, 0.0)});
    b.commands.push_back(SetDeviceParam{roles.pitch, std::string(kValueParam),
                                        dsp::curvePosition(phaseAccumulatorCurve(kPaPitch), pitch, 0.5)});
    if (wire) b.commands.push_back(AddDevice{t, constantDevice(src, wire->second), kAppend});
    REQUIRE(store.dispatch(b).ok());
    wired(store);  // TX.all-free: the path as cables
    if (wire) {
      auto e = ops::connectWire(*store.current(), ids, src, std::string(kValueOutput), wire->first ? roles.freq : roles.pitch,
                                std::string(kValueParam));
      REQUIRE(e.ok());
      REQUIRE(store.dispatch(e->command).ok());
    }
    const Lowered low = lowerIt(store);
    const golden::Channels a = renderGraph(low.low.graph, low.links, low.values, 9600);
    CHECK(maxDiff(a, renderGraph(low.low.graph, low.links, low.values, 9600, compile::jitUnitBackend())) == 0.0);
    return a;
  };
  const double t440 = dsp::curvePosition(phaseAccumulatorCurve(kPaFrequency), 440.0, 0.0);
  const golden::Channels viaWire = pa(0.0, 0.0, std::make_pair(true, t440));
  const golden::Channels byKnob = pa(440.0, 0.0, std::nullopt);
  MESSAGE("Frequency + " << t440 << " in position vs the knob at 440 Hz: " << relDiff(viaWire, byKnob));
  CHECK(relDiff(viaWire, byKnob) < 1e-3);
  const golden::Channels up = pa(440.0, 0.0, std::make_pair(false, 1200.0));
  const golden::Channels octave = pa(440.0, 1200.0, std::nullopt);
  MESSAGE("Pitch + 1200 cents vs the knob at +1200: " << relDiff(up, octave));
  CHECK(relDiff(up, octave) < 1e-3);
}

TEST_CASE("knob curves: an Add wire into the VCA's knob — on its law in position (through the curve), on a line in units") {
  // GU.4: the VCA module (In → mul ← Value): a wire into its value block's knob. On the VCA's law (a curved knob) the
  // wire moves the position and the curve atom maps the sum; on a linear curve it adds in units after it (one op
  // fewer). Both lower, render, and agree between the interpreter and the JIT.
  for (const bool linear : {false, true}) {
    CAPTURE(linear);
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId t = ids.track();
    const DeviceId vca = ids.device(), pa = ids.device();
    Batch b{"t", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Audio, "T"), {}, kAppend});
    VcaOptions vo;
    vo.id = vca;
    if (linear) vo.curve = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 2.0);
    VcaRoles vr;
    const BlockSet set = vcaBlocks(ids, vo, &vr);
    for (const DevicePtr& d : set.devices) b.commands.push_back(AddDevice{t, d, kAppend});
    for (const Wire& w : set.wires) b.commands.push_back(AddWire{w});
    for (const DevicePtr& d : set.devices)
      if (d->group.valid()) gOffPath.push_back(d->id);
    PhaseAccumulatorRoles roles;
    addPhaseAccumulator(b, ids, t, pa, roles, true);
    b.commands.push_back(SetDeviceParam{roles.freq, std::string(kValueParam),
                                        dsp::curvePosition(phaseAccumulatorCurve(kPaFrequency), 55.0, 0.0)});
    REQUIRE(store.dispatch(b).ok());
    wired(store);  // TX.all-free: the path as cables (the VCA on it, the Phase Accumulator free)
    auto e = ops::connectWire(*store.current(), ids, pa, "", vr.value, std::string(kValueParam));
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
    auto in = ops::connectWire(*store.current(), ids, pa, "", vca, "");  // the signal it multiplies, too
    REQUIRE(in.ok());
    REQUIRE(store.dispatch(in->command).ok());
    const Lowered low = lowerIt(store);
    for (const WireState& s : low.low.wires) CHECK(s.status == WireStatus::Lowered);
    CHECK(addsInUnits(*findDevice(*store.current(), vr.value).device->curve(kValueParam)) == linear);
    const golden::Channels a = renderGraph(low.low.graph, low.links, low.values, 4800);
    CHECK(peakOf(a) > 0.0f);
    CHECK(maxDiff(a, renderGraph(low.low.graph, low.links, low.values, 4800, compile::jitUnitBackend())) == 0.0);
  }
}
