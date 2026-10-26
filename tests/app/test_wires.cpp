// T4.7: cross-device modulation wires through the lowering and the playback link — an LFO export on track 1 wired into
// a filter cutoff on track 3 is audible after the compile (one undo step); the lowered project renders exactly like a
// hand-built ProjectGraph of the same wires (diff 0) and the interpreter like the JIT; a wire that closes a delay-free
// loop gets its 1-sample delay (and only such a wire); a loop through two instruments is refused; the taps the knob
// rings read sweep while the LFO runs; a wire's amount is a param write.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/parse.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, bool jit = false) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

struct Driver {
  ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  golden::Channels out = golden::Channels(2);
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      out[0].insert(out[0].end(), l.begin(), l.end());
      out[1].insert(out[1].end(), r.begin(), r.end());
    }
  }
};

double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[c][i] - b[c][i])));
  }
  return m;
}

// RMS of channel 0 in windows of `window` samples over [from, to).
std::vector<double> windowRms(const std::vector<float>& x, size_t from, size_t to, size_t window) {
  std::vector<double> out;
  for (size_t s = from; s + window <= std::min(to, x.size()); s += window) {
    double e = 0.0;
    for (size_t i = s; i < s + window; ++i) e += static_cast<double>(x[i]) * x[i];
    out.push_back(std::sqrt(e / static_cast<double>(window)));
  }
  return out;
}
double spreadDb(const std::vector<double>& rms) {
  const auto [lo, hi] = std::minmax_element(rms.begin(), rms.end());
  return 20.0 * std::log10(*hi / std::max(*lo, 1e-12));
}

// Track 1: an LFO (a MIDI track without an instrument: the LFO's audio passes silence); track 2: the default
// instrument, idle; track 3: a held note through EQ Eight with band 1 a 12 dB low-pass ("a filter cutoff").
struct ThreeTracks {
  DocumentStore store;
  TrackId t1, t2, t3;
  DeviceId lfo, inst2, inst3, eq;
  explicit ThreeTracks(double lfoRate = 3.0) {
    IdSource& ids = store.ids();
    t1 = ids.track();
    t2 = ids.track();
    t3 = ids.track();
    lfo = ids.device();
    inst2 = ids.device();
    inst3 = ids.device();
    eq = ids.device();
    Batch b{"three tracks", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t1, TrackKind::Midi, "LFO"), {}, kAppend});
    b.commands.push_back(AddTrack{playback_fixture::track(t2, TrackKind::Midi, "Pad", inst2), {}, kAppend});
    b.commands.push_back(AddTrack{playback_fixture::track(t3, TrackKind::Midi, "Keys", inst3), {}, kAppend});
    b.commands.push_back(AddDevice{t1, makeDevice(lfo, "lfo_device", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(SetDeviceParam{lfo, "rate", lfoRate});
    b.commands.push_back(AddDevice{t3, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(SetDeviceSetting{eq, "on1", 1.0});
    b.commands.push_back(SetDeviceSetting{eq, "type1", 6.0});  // High Cut 12
    b.commands.push_back(SetDeviceParam{eq, "freq1", 800.0});
    // A note every 2 s (4 beats at 120 BPM), 1.75 s long (single notes: a chord would beat in the 10 ms windows the
    // test measures); track 2 plays nothing.
    std::vector<Note> notes;
    for (int k = 0; k < 16; ++k) notes.push_back(playback_fixture::note(ids, 4 * k * kPpq, 7 * kPpq / 2, 0));
    b.commands.push_back(AddClip{t3, playback_fixture::clip(ids, 0, 64 * kPpq, notes, "notes")});
    REQUIRE(store.dispatch(b).ok());
  }
};

}  // namespace

TEST_CASE("wires: an LFO on track 1 wired into a filter cutoff on track 3 is audible after the compile, one undo step") {
  ThreeTracks f;
  auto link = makeLink(f.store.current());
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  auto runTo = [&](double seconds) { d.run(static_cast<uint32_t>((seconds * kRate - static_cast<double>(d.time)) / kBlock)); };
  auto spreadOver = [&](double from, double to) {
    return spreadDb(windowRms(d.out[0], static_cast<size_t>(from * kRate), static_cast<size_t>(to * kRate), 480));
  };
  link->play(0);
  runTo(1.6);
  const double flat = spreadOver(0.6, 1.6);  // the first note on its sustain
  runTo(1.8);
  // Wire it (between two notes): one undo step, a compile (both tracks change: an export on 1, an input on 3; the
  // wire merges their units, which start afresh: QUESTIONS Q?-T4.7-a).
  const app::LinkStats s0 = link->stats();
  auto e = ops::connectWire(*f.store.current(), f.store.ids(), f.lfo, "lfo", f.eq, "freq1", 0.6);
  REQUIRE(e.ok());
  const size_t h = f.store.history().cursor();
  REQUIRE(f.store.dispatch(e->command).ok());
  CHECK(f.store.history().cursor() == h + 1);
  link->flush();
  CHECK(link->lastError().empty());
  CHECK(link->stats().compiles == s0.compiles + 1);
  CHECK(link->stats().compileErrors == s0.compileErrors);
  REQUIRE(link->wireStates().size() == 1);
  CHECK(link->wireStates()[0].status == WireStatus::Lowered);
  runTo(3.6);
  const double swept = spreadOver(2.6, 3.6);  // the second note (from 2 s), modulated
  MESSAGE("10 ms RMS spread over 1 s: unwired " << flat << " dB, wired " << swept << " dB");
  CHECK(flat < 1.0);
  CHECK(swept > 6.0);  // the cutoff sweeps 800 Hz ± 0.6 of the knob travel at 3 Hz: plainly audible
  // The knob ring's tap sweeps both ways while the LFO runs.
  views::ModSweep sw;
  const views::ModTarget tgt{f.eq, "freq1"};
  link->readModulation(std::span(&tgt, 1), std::span(&sw, 1));
  runTo(4.0);
  link->readModulation(std::span(&tgt, 1), std::span(&sw, 1));
  CHECK(sw.known);
  CHECK(sw.up > 0.3f);
  CHECK(sw.down > 0.3f);
  CHECK(sw.up <= 0.6f + 1e-4f);
  // The amount is a param: a write, no compile.
  {
    const app::LinkStats s1 = link->stats();
    const WireId w = wiresOf(*f.store.current()).at(0).id;
    REQUIRE(f.store.dispatch(ops::setWireAmount(*f.store.current(), w, 0.0)->command).ok());
    link->flush();
    CHECK(link->stats().compiles == s1.compiles);
    CHECK(link->stats().paramWrites == s1.paramWrites + 1);
    CHECK(link->paramSlot("wire" + std::to_string(w.value) + ":amount") != ProjectLink::kNoSlot);
    runTo(5.6);
    CHECK(spreadOver(4.6, 5.6) < 1.0);  // the third note, unmodulated again
  }
  // Undo of the wire: a compile back to the unwired graph.
  REQUIRE(f.store.undo());
  REQUIRE(f.store.undo());
  link->flush();
  CHECK(wiresOf(*f.store.current()).empty());
  CHECK(link->wireStates().empty());
}

namespace {

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

// The modulation math of lower.h for a Log param (lo, hi) driven by `s` into `value`, with its tap (locals pk<t>, p<t>).
std::string logModulation(const std::string& t, const std::string& knob, double lo, double hi, const std::string& s,
                          const std::string& value, const std::string& tap) {
  const double L = std::log(hi / lo);
  const std::string pk = "pk" + t, p = "p" + t;
  return "  " + pk + " = log(" + knob + " * " + num(1.0 / lo) + ") * " + num(1.0 / L) + "\n"
         "  " + p + " = clip(" + pk + " + " + s + ", 0, 1)\n"
         "  " + value + " = exp(" + p + " * " + num(L) + " + " + num(std::log(lo)) + ")\n"
         "  export " + tap + " = pack(max(" + p + " - " + pk + ", 0), max(" + pk + " - " + p + ", 0))\n";
}

}  // namespace

TEST_CASE("wires: a lowered project with wires renders exactly like the hand-built ProjectGraph; interpreter == JIT") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  // Track A: LFO (rate 5 Hz) only — its `lfo` export wired across tracks into Keys' EQ Eight freq1 (amount 0.4) and,
  // inside Keys, BasicSynth → EQ Eight → LFO2 whose export modulates the EQ's gain2 (amount −0.3) and BasicSynth's
  // bright (amount 0.25; voice param from a global source).
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId ta = ids.track(), keys = ids.track();
  const DeviceId lfoA = ids.device(), inst = ids.device(), eq = ids.device(), lfoB = ids.device();
  const WireId w1 = ids.wire(), w2 = ids.wire(), w3 = ids.wire();
  Batch b{"wired", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(ta, TrackKind::Midi, "A"), {}, kAppend});
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{ta, makeDevice(lfoA, "lfo_device", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(lfoB, "lfo_device", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceParam{lfoA, "rate", 5.0});
  b.commands.push_back(SetDeviceParam{lfoB, "rate", 0.7});
  b.commands.push_back(SetDeviceSetting{lfoB, "shape", 1.0});  // triangle
  b.commands.push_back(SetDeviceSetting{eq, "on1", 1.0});
  b.commands.push_back(SetDeviceSetting{eq, "type1", 6.0});
  b.commands.push_back(SetDeviceParam{eq, "freq1", 900.0});
  b.commands.push_back(SetDeviceParam{eq, "gain2", 2.0});  // +6 dB at 100 Hz (a linear gain)
  b.commands.push_back(AddWire{Wire{w1, lfoA, "lfo", eq, "freq1", 0.4, WireReducer::Sum}});
  b.commands.push_back(AddWire{Wire{w2, lfoB, "lfo", eq, "gain2", -0.3, WireReducer::Sum}});
  b.commands.push_back(AddWire{Wire{w3, lfoB, "lfo", inst, "bright", 0.25, WireReducer::Sum}});
  REQUIRE(store.dispatch(b).ok());
  LowerOptions lo;
  lo.library = &(*lib)->library();
  lo.slotOf = [](TrackId) { return 0u; };
  auto low = lower(*store.current(), lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  REQUIRE(low->wires.size() == 3);
  for (const WireState& s : low->wires) CHECK(s.status == WireStatus::Lowered);  // LFO2 after the EQ: no loop
  CHECK(low->taps.size() == 3);
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  for (const LoweredParam& p : low->params) {
    links.push_back({p.track, p.name, static_cast<uint32_t>(values.size())});
    values.push_back(p.value);
  }
  constexpr uint32_t kFrames = 48000;
  std::vector<std::string> warnings;
  const golden::Channels lowered = renderGraph(low->graph, links, values, kFrames, {}, &warnings);
  CHECK(warnings.empty());  // no implicit z1 anywhere: the lowering left no delay-free cycle to the scheduler

  // The same written by hand (lower.h: the chains, the mixer stages, the modulation math).
  const auto i = [](auto id) { return std::to_string(id.value); };
  // The ends of the travels exactly as the device declares them (freq1: 10 Hz … 22 kHz, @curve(exp); gain2: ±15 dB,
  // a dB gain: Log too).
  auto travel = [&](const char* name) {
    const graph::Patch* eqp = (*lib)->library().find("eq_eight");
    REQUIRE(eqp != nullptr);
    const auto n = eqp->findByName(name);
    REQUIRE(n.has_value());
    return paramTravel(eqp->paramDecl(*n));
  };
  const ParamTravel tf = travel("freq1"), tg = travel("gain2");
  CHECK(tf.log);
  CHECK(tg.log);
  const double fLo = tf.lo, fHi = tf.hi, gLo = tg.lo, gHi = tg.hi;
  std::string text = "patch A {\n  param rate = 5\n  out out : audio global stereo\n"
                     "  l = lfo_device(pack(0, 0), rate: rate)\n"
                     "  export m" + i(w1) + " = channel(l.lfo, 0)\n" + midiStages("l.out") + "}\n";
  text += "patch Keys {\n  param bright = 0.35 [0..1]\n  param f1 = 900 [" + num(fLo) + ".." + num(fHi) +
          "]\n  param g2 = 2 [" + num(gLo) + ".." +
          num(gHi) + "]\n  param brate = 0.7\n  param w" + i(w1) + " = 0.4 [-1..1]\n  param w" + i(w2) +
          " = -0.3 [-1..1]\n  param w" + i(w3) + " = 0.25 [-1..1]\n  in m" + i(w1) + " : real global mono = 0\n"
          "  out out : audio global stereo\n  out done : bool voice mono\n"
          "  s = BasicSynth(bright: bv)\n  x = voice_sum(s.out)\n"
          "  e = eq_eight(pack(x, x), on1: 1, type1: 6, freq1: fv, gain2: gv)\n"
          "  b = lfo_device(e.out, shape: 1, rate: brate)\n  src = channel(b.lfo, 0)\n"
          "  done = s.done\n";
  // freq1 ← m (wire 1); gain2 ← src (wire 2); bright ← src (wire 3; Linear 0..1)
  text += logModulation("a", "f1", fLo, fHi, "w" + i(w1) + " * m" + i(w1), "fv", "tap_d" + i(eq) + "_freq1");
  text += logModulation("b", "g2", gLo, gHi, "w" + i(w2) + " * src", "gv", "tap_d" + i(eq) + "_gain2");
  text += "  pkc = (bright - 0.0) * 1.0\n  pc = clip(pkc + w" + i(w3) + " * src, 0, 1)\n  bv = pc * 1.0 + 0.0\n"
          "  export tap_d" + i(inst) + "_bright = pack(max(pc - pkc, 0), max(pkc - pc, 0))\n";
  text += midiStages("b.out") + "}\n";
  text += "patch Main {\n  param volume = 0 [-70..6]\n  param pan = 0 [-1..1]\n  in in : audio global stereo\n"
          "  out out : audio global stereo\n  vg = volume <= -70 ? 0 : db_to_gain(volume)\n"
          "  out = gain(in, pack(min(1, 1 - pan) * vg, min(1, 1 + pan) * vg), 0.002)\n}\n";
  graph::ParseOptions po;
  po.externals = (*lib)->signatures();
  auto file = graph::parse(text, "hand.pqp", po);
  REQUIRE_MESSAGE(file.ok(), (file.ok() ? std::string() : file.error().message + "\n" + text));
  graph::PatchLibrary handLib;
  handLib.add(*file);
  handLib.addAll((*lib)->library());
  graph::ProjectGraph hand;
  hand.library = &handLib;
  hand.tracks.push_back({ta.value, "A", file->find("A"), graph::TrackRole::Audio, graph::kNone});
  hand.tracks.push_back({keys.value, "Keys", file->find("Keys"), graph::TrackRole::Instrument, 0});
  hand.tracks.push_back({store.current()->master->id.value, "Main", file->find("Main"), graph::TrackRole::Master, graph::kNone});
  hand.wires.push_back({{0, "out"}, {2, "in"}, graph::WireKind::Audio});
  hand.wires.push_back({{1, "out"}, {2, "in"}, graph::WireKind::Audio});
  hand.wires.push_back({{0, "m" + i(w1)}, {1, "m" + i(w1)}, graph::WireKind::Modulation});
  hand.output = {2, "out"};
  // The hand params: the same values as the lowered ones (by meaning).
  std::vector<engine::ProjectParamLink> handLinks;
  std::vector<float> handValues;
  auto link = [&](uint32_t track, const std::string& name, float v) {
    handLinks.push_back({track, name, static_cast<uint32_t>(handValues.size())});
    handValues.push_back(v);
  };
  link(0, "rate", 5.0f);
  link(1, "bright", 0.35f);
  link(1, "f1", 900.0f);
  link(1, "g2", 2.0f);
  link(1, "brate", 0.7f);
  link(1, "w" + i(w1), 0.4f);
  link(1, "w" + i(w2), -0.3f);
  link(1, "w" + i(w3), 0.25f);
  for (uint32_t t : {0u, 1u}) {
    link(t, "volume", 0.0f);
    link(t, "pan", 0.0f);
    link(t, "gate", 1.0f);
    link(t, "sendGate", 1.0f);
    link(t, "mute", 0.0f);
  }
  link(2, "volume", 0.0f);
  link(2, "pan", 0.0f);
  const golden::Channels manual = renderGraph(hand, handLinks, handValues, kFrames);
  float peak = 0.0f;
  for (const auto& c : manual)
    for (float v : c) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.02f);
  const double diff = maxDiff(lowered, manual);
  MESSAGE("lowered wires vs hand-built graph: max |diff| = " << diff);
  CHECK(diff == 0.0);

  // The JIT renders the lowered project bit for bit like the interpreter.
  const golden::Channels jit = renderGraph(low->graph, links, values, kFrames, compile::jitUnitBackend());
  const double dj = maxDiff(lowered, jit);
  MESSAGE("interpreter vs JIT: max |diff| = " << dj);
  CHECK(dj == 0.0);
}

TEST_CASE("wires: a wire that closes a delay-free loop gets a 1-sample delay; an LFO's own loop does not") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId t = ids.track(), grp = ids.track(), other = ids.track();
  const DeviceId inst = ids.device(), util = ids.device(), sat = ids.device(), lfo = ids.device(), glfo = ids.device(),
                 inst2 = ids.device(), lfo2 = ids.device();
  Batch b{"loops", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(grp, TrackKind::Group, "Grp"), {}, kAppend});
  b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "Keys", inst), grp, kAppend});
  b.commands.push_back(AddTrack{playback_fixture::track(other, TrackKind::Midi, "Other", inst2), {}, kAppend});
  b.commands.push_back(AddDevice{t, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{t, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{t, makeDevice(lfo, "lfo_device", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{grp, makeDevice(glfo, "lfo_device", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{other, makeDevice(lfo2, "lfo_device", DeviceKind::AudioEffect), kAppend});
  const WireId wLoop = ids.wire(), wSelf = ids.wire(), wBack = ids.wire(), wFeed = ids.wire(), wGroup = ids.wire();
  // saturator.wet depends on utility's output: wet → utility.gain closes a loop (delayed).
  b.commands.push_back(AddWire{Wire{wLoop, sat, "wet", util, "gain", 0.2, WireReducer::Sum}});
  // the LFO's export reads its rate through the phasor's frequency, which the scheduler cannot split a loop at
  // (PATCH_LANGUAGE §3, Q43): a loop, delayed like any other.
  b.commands.push_back(AddWire{Wire{wSelf, lfo, "lfo", lfo, "rate", 0.1, WireReducer::Sum}});
  // the LFO (last in the chain) into the first effect: its export does not depend on its audio input: not a loop.
  b.commands.push_back(AddWire{Wire{wBack, lfo, "lfo", util, "width", 0.5, WireReducer::Sum}});
  // across tracks: the saturator's wet into the group LFO's depth (Keys plays into the group: no loop yet); then the
  // group LFO into the saturator's drive closes Keys' saturator → group LFO → saturator (delayed; one instrument in
  // the track cycle, so the partition merges Keys and the group into one unit).
  b.commands.push_back(AddWire{Wire{wFeed, sat, "wet", glfo, "depth", 0.5, WireReducer::Sum}});
  b.commands.push_back(AddWire{Wire{wGroup, glfo, "lfo", sat, "drive", 0.3, WireReducer::Sum}});
  REQUIRE(store.dispatch(b).ok());
  const std::vector<WireState> st = wireStates(*store.current(), (*lib)->library());
  REQUIRE(st.size() == 5);
  CHECK(st[0] == WireState{wLoop, WireStatus::Delayed});
  CHECK(st[1] == WireState{wSelf, WireStatus::Delayed});
  CHECK(st[2] == WireState{wBack, WireStatus::Lowered});
  CHECK(st[3] == WireState{wFeed, WireStatus::Lowered});
  CHECK(st[4] == WireState{wGroup, WireStatus::Delayed});
  // Through the link: it compiles (no implicit z1 needed beyond the wire's), and the status line counts them.
  auto link = makeLink(store.current());
  CHECK(link->lastError().empty());
  CHECK(link->stats().delayedWires == 3);
  CHECK(link->stats().refusedWires == 0);
  Driver d{*link};
  link->play(0);
  d.run(200);
  // A loop through two instrument tracks is refused (the engine runs one voice table per image, Q84).
  const WireId wa = ids.wire(), wb = ids.wire();
  REQUIRE(store.dispatch(Batch{"cross", {AddWire{Wire{wa, lfo, "lfo", inst2, "bright", 0.2, WireReducer::Sum}},
                                         AddWire{Wire{wb, lfo2, "lfo", inst, "bright", 0.2, WireReducer::Sum}}}})
              .ok());
  const std::vector<WireState> st2 = wireStates(*store.current(), (*lib)->library());
  REQUIRE(st2.size() == 7);
  CHECK(st2[5] == WireState{wa, WireStatus::Lowered});
  CHECK(st2[6] == WireState{wb, WireStatus::Refused});
  // Deactivating a device leaves its wires out (inactive), and the project still compiles.
  REQUIRE(store.dispatch(SetDeviceActive{sat, false}).ok());
  const std::vector<WireState> st3 = wireStates(*store.current(), (*lib)->library());
  CHECK(st3[0].status == WireStatus::Inactive);
  CHECK(st3[4].status == WireStatus::Inactive);
  auto link2 = makeLink(store.current());
  CHECK(link2->lastError().empty());
}

TEST_CASE("wires: a voice source into a global target is reduced by the wire's reducer") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  // BasicSynth exports nothing: a probe instrument with a per-voice export (its velocity) stands in for Operator's
  // oscillator exports (T4.12).
  graph::ParseOptions po;
  po.externals = (*lib)->signatures();
  auto probe = graph::parse(R"(
patch Probe {
  out out : audio voice mono
  out done : bool voice mono
  out = 0.1 * sine(phasor(voice.hz))
  done = !voice.gate
  export vel = voice.velocity
}
)", "probe.pqp", po);
  REQUIRE_MESSAGE(probe.ok(), (probe.ok() ? std::string() : probe.error().message));
  graph::PatchLibrary plib;
  plib.add(*probe);
  plib.addAll((*lib)->library());
  auto render = [&](WireReducer reducer, bool* reducerFound) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId t = ids.track();
    const DeviceId inst = ids.device(), util = ids.device();
    Batch b{"probe", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T"), {}, kAppend});
    b.commands.push_back(AddDevice{t, makeDevice(inst, "Probe", DeviceKind::Instrument), 0});
    b.commands.push_back(AddDevice{t, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
    b.commands.push_back(AddWire{Wire{ids.wire(), inst, "vel", util, "balance", 0.8, reducer}});
    REQUIRE(store.dispatch(b).ok());
    LowerOptions lo;
    lo.library = &plib;
    lo.slotOf = [](TrackId) { return 0u; };
    auto low = lower(*store.current(), lo);
    REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
    REQUIRE(low->wires.size() == 1);
    CHECK(low->wires[0].status == WireStatus::Lowered);
    const graph::Patch& tp = low->patches[0]->patch;
    const std::string_view want = reducer == WireReducer::Last ? "voice_last" : "voice_sum";
    *reducerFound = false;
    for (graph::NodeIndex n = 0; n < tp.nodeCount(); ++n)
      if (tp.node(n).kind == graph::NodeKind::Atom && graph::atomSignature(tp.node(n).atom).name == want &&
          tp.node(n).id.value == nodeIdFor(NodeRole::Wire, wiresOf(*store.current())[0].id.value, 4))
        *reducerFound = true;
    std::vector<engine::ProjectParamLink> links;
    std::vector<float> values;
    for (const LoweredParam& p : low->params) {
      links.push_back({p.track, p.name, static_cast<uint32_t>(values.size())});
      values.push_back(p.value);
    }
    return renderGraph(low->graph, links, values, 24000);
  };
  bool sumFound = false, lastFound = false;
  const golden::Channels sum = render(WireReducer::Sum, &sumFound);
  const golden::Channels last = render(WireReducer::Last, &lastFound);
  CHECK(sumFound);
  CHECK(lastFound);
  // Three voices of velocities 0.8, 0.6, 0.9: the summed velocity pans hard right sooner than the last one alone.
  CHECK(maxDiff(sum, last) > 1e-3);
}

TEST_CASE("wires: deleting a device deletes its wires, in one undo step") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId t = ids.track();
  const DeviceId lfo = ids.device(), util = ids.device();
  REQUIRE(store.dispatch(Batch{"t", {AddTrack{playback_fixture::track(t, TrackKind::Midi, "T"), {}, kAppend},
                                     AddDevice{t, makeDevice(lfo, "lfo_device", DeviceKind::AudioEffect), kAppend},
                                     AddDevice{t, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend}}})
              .ok());
  auto e = ops::connectWire(*store.current(), ids, lfo, "lfo", util, "balance", 1.0);
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  CHECK_FALSE(ops::connectWire(*store.current(), ids, lfo, "lfo", util, "balance").ok());  // already wired
  REQUIRE(store.dispatch(ops::deleteDevices(*store.current(), std::vector<DeviceId>{lfo})->command).ok());
  CHECK(wiresOf(*store.current()).empty());
  REQUIRE(store.undo());
  CHECK(wiresOf(*store.current()).size() == 1);  // one undo step restores the device and its wire
}

// T1.x-silence: a modulation source on another track keeps the target's unit running (and its ring moving) while it is
// non-zero, through the bus-silence flags; the tap is zero exactly when the modulation is.
TEST_CASE("wires: a cross-track modulation keeps the target unit awake while it is non-zero") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId a = ids.track(), bt = ids.track();
  const DeviceId instA = ids.device(), utilA = ids.device(), instB = ids.device(), utilB = ids.device();
  Batch b{"two", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(a, TrackKind::Midi, "Src", instA), {}, kAppend});
  b.commands.push_back(AddTrack{playback_fixture::track(bt, TrackKind::Midi, "Dst", instB), {}, kAppend});
  b.commands.push_back(AddDevice{a, makeDevice(utilA, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{bt, makeDevice(utilB, "utility", DeviceKind::AudioEffect), kAppend});
  // Src plays a note 2.0–2.5 s; Dst never plays (two instruments: the wire stays a bus between their units, Q84).
  b.commands.push_back(AddClip{a, playback_fixture::clip(ids, 0, 16 * kPpq, {playback_fixture::note(ids, 4 * kPpq, kPpq, 0)}, "n")});
  b.commands.push_back(AddWire{Wire{ids.wire(), utilA, "mid", utilB, "width", 0.5, WireReducer::Sum}});
  REQUIRE(store.dispatch(b).ok());
  auto link = makeLink(store.current());
  REQUIRE(link->lastError().empty());
  REQUIRE(link->wireStates().size() == 1);
  CHECK(link->wireStates()[0].status == WireStatus::Lowered);
  Driver d{*link};
  engine::ProjectProgram& program = *link->engine().program();
  REQUIRE(program.numUnits() >= 3);
  const views::ModTarget tgt{utilB, "width"};
  views::ModSweep sw;
  link->play(0);
  d.run(static_cast<uint32_t>(1.5 * kRate / kBlock));  // silence: nothing modulates
  link->readModulation(std::span(&tgt, 1), std::span(&sw, 1));
  d.run(static_cast<uint32_t>(0.4 * kRate / kBlock));
  link->readModulation(std::span(&tgt, 1), std::span(&sw, 1));
  CHECK(sw.known);
  CHECK(sw.up == 0.0f);
  CHECK(sw.down == 0.0f);
  uint64_t dstIdle = 0;
  for (uint32_t u = 0; u < program.numUnits(); ++u)
    if (program.layout().units[u].name == "Dst") dstIdle = program.unitStats(u).idleBlocks;
  MESSAGE("Dst's unit: blocks skipped while its wire carried silence: " << dstIdle);
  // Not asserted: the modulated value (knob ± modulation) is not 0 at silence, so the region computing it is not
  // sleepable (Q130.3: no rate demotion while a modulator is silent): a wired target stays awake. QUESTIONS
  // Q?-T4.7-a item 7 records the cost and the follow-up.
  d.run(static_cast<uint32_t>(0.4 * kRate / kBlock));  // the note: Src's mid moves Dst's width
  link->readModulation(std::span(&tgt, 1), std::span(&sw, 1));
  CHECK(sw.up + sw.down > 1e-3f);
}
