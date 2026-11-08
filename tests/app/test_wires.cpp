// T4.7: cross-block modulation wires through the lowering and the playback link (GU.11: on modules — LFO, Utility,
// Saturator — and the default instrument group) — an LFO on track 1 wired into a gain on track 3 is audible after the
// compile (one undo step); a cross-track wire renders exactly like the same wire on one track (diff 0) and the
// interpreter like the JIT; a wire that closes a delay-free loop gets its 1-sample delay (and only such a wire); a loop
// through two instruments is refused; the taps the knob rings read sweep while the LFO runs; a wire's amount is a
// param write.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "../support/knob_positions.h"
#include "playback_fixture.h"
#include "../support/module_batch.h"
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

// GU.11: blocks for these cases — the LFO / Utility / Saturator modules (pq/document/modules.h) and the default
// instrument group. A module's controls are value blocks (a wire into one targets its kValueParam); an LFO module
// passes no audio (its Out is the wave), so it sits off the path as a modulation source.
using module_batch::Kind;
using module_batch::Module;

// Track 1: an LFO (off the path of a MIDI track without an instrument); track 2: the default instrument, idle; track 3:
// a held note through a Utility at −30 dB (the wire's target: its Gain).
struct ThreeTracks {
  DocumentStore store;
  TrackId t1, t2, t3;
  DeviceId lfo, inst2, inst3, util, gain;
  explicit ThreeTracks(double lfoRate = 3.0) {
    IdSource& ids = store.ids();
    t1 = ids.track();
    t2 = ids.track();
    t3 = ids.track();
    lfo = ids.device();
    inst2 = ids.device();
    inst3 = ids.device();
    util = ids.device();
    Batch b{"three tracks", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t1, TrackKind::Midi, "LFO"), {}, kAppend});
    b.commands.push_back(AddTrack{playback_fixture::track(t2, TrackKind::Midi, "Pad"), {}, kAppend});
    (void)playback_fixture::instrument(b, ids, t2, inst2);
    b.commands.push_back(AddTrack{playback_fixture::track(t3, TrackKind::Midi, "Keys"), {}, kAppend});
    (void)playback_fixture::instrument(b, ids, t3, inst3);
    const Module l = module_batch::push(b, ids, t1, Kind::Lfo, lfo);
    b.commands.push_back(SetDeviceParam{l.at("Rate"), std::string(kValueParam), lfoRate});
    const Module u = module_batch::push(b, ids, t3, Kind::Utility, util);
    gain = u.at("Gain");
    b.commands.push_back(SetDeviceParam{gain, std::string(kValueParam), -30.0});
    // A note every 2 s (4 beats at 120 BPM), 1.75 s long (single notes: a chord would beat in the 10 ms windows the
    // test measures); track 2 plays nothing.
    std::vector<Note> notes;
    for (int k = 0; k < 16; ++k) notes.push_back(playback_fixture::note(ids, 4 * k * kPpq, 7 * kPpq / 2, 0));
    b.commands.push_back(AddClip{t3, playback_fixture::clip(ids, 0, 64 * kPpq, notes, "notes")});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
    playback_fixture::wireSeries(store, {lfo});  // TX.all-free: wired in series (the LFO off the path)
  }
};

// The wires a test drew (not main links, not chain links, not a module's own wiring).
std::vector<WireState> drawn(std::span<const WireState> states, const Project& p) {
  std::vector<WireState> out;
  for (const WireState& s : playback_fixture::drawn(states, p))
    if (const Wire* w = findWire(p, s.id); w != nullptr && !w->chain && !module_batch::innerWire(p, *w)) out.push_back(s);
  return out;
}
WireList drawnWires(const Project& p) {
  WireList out;
  for (const Wire& w : playback_fixture::drawnWires(p))
    if (!w.chain && !module_batch::innerWire(p, w)) out.push_back(w);
  return out;
}

}  // namespace

TEST_CASE("wires: an LFO on track 1 wired into a gain on track 3 is audible after the compile, one undo step") {
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
  // Wire it (between two notes): one undo step, a compile (both tracks change: an output on 1, an input on 3; the wire
  // merges their units, which start afresh: QUESTIONS Q135).
  const app::LinkStats s0 = link->stats();
  auto e = ops::connectTravelWire(*f.store.current(), f.store.ids(), f.lfo, "y", f.gain, std::string(kValueParam), 0.6);
  REQUIRE(e.ok());
  const size_t h = f.store.history().cursor();
  REQUIRE(f.store.dispatch(e->command).ok());
  CHECK(f.store.history().cursor() == h + 1);
  link->flush();
  CHECK(link->lastError().empty());
  CHECK(link->stats().compiles == s0.compiles + 1);
  CHECK(link->stats().compileErrors == s0.compileErrors);
  REQUIRE(drawn(link->wireStates(), *f.store.current()).size() == 1);
  CHECK(drawn(link->wireStates(), *f.store.current())[0].status == WireStatus::Lowered);
  runTo(3.6);
  const double swept = spreadOver(2.6, 3.6);  // the second note (from 2 s), modulated
  MESSAGE("10 ms RMS spread over 1 s: unwired " << flat << " dB, wired " << swept << " dB");
  CHECK(flat < 1.0);
  CHECK(swept > 6.0);  // the gain sweeps −30 dB ± 0.6 of the knob travel at 3 Hz: plainly audible
  // The knob ring's tap sweeps both ways while the LFO runs.
  views::ModSweep sw;
  const views::ModTarget tgt{f.gain, std::string(kValueParam)};
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
    const WireId w = drawnWires(*f.store.current()).at(0).id;
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
  CHECK(drawnWires(*f.store.current()).empty());
  CHECK(drawn(link->wireStates(), *f.store.current()).empty());
}

namespace {

// The lowered project's render (interpreter, or `backend`) with fixed notes on slot 0.
golden::Channels renderLowered(const LoweredProject& low, uint32_t frames, engine::UnitBackend backend = {},
                               std::vector<std::string>* warnings = nullptr) {
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  for (const LoweredParam& p : low.params) {
    links.push_back({p.track, p.name, static_cast<uint32_t>(values.size())});
    values.push_back(p.value);
  }
  return renderGraph(low.graph, links, values, frames, backend, warnings);
}

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

LoweredProject lowered(const Project& p) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  auto low = lower(p, lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  return std::move(*low);
}

}  // namespace

TEST_CASE("wires: a cross-track wire renders exactly like the same wire on one track; a global source into a voice "
          "control; interpreter == JIT") {
  // GU.11 (no stock patches to hand-build the graph from): Track A: an LFO (5 Hz) wired across tracks into Keys'
  // Utility Gain (amount 0.4) and, inside Keys, a free LFO 2 (0.7 Hz, triangle) into the Utility's Balance (amount −0.3)
  // and the instrument's Pitch (amount 0.25; a voice control from a global source: one LFO for every voice). Its second
  // form: the first LFO on Keys itself (the same wires within one track) — the same math per sample: bit for bit.
  auto build = [](bool crossTrack) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId ta = ids.track(), keys = ids.track();
    const DeviceId lfoA = ids.device(), inst = ids.device(), util = ids.device(), lfoB = ids.device();
    Batch b{"wired", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(ta, TrackKind::Midi, "A"), {}, kAppend});
    b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys"), {}, kAppend});
    (void)playback_fixture::instrument(b, ids, keys, inst);
    const Module la = module_batch::push(b, ids, crossTrack ? ta : keys, Kind::Lfo, lfoA);
    const Module u = module_batch::push(b, ids, keys, Kind::Utility, util);
    const Module lb = module_batch::push(b, ids, keys, Kind::Lfo, lfoB);
    b.commands.push_back(SetDeviceParam{la.at("Rate"), std::string(kValueParam), 5.0});
    b.commands.push_back(SetDeviceParam{lb.at("Rate"), std::string(kValueParam), 0.7});
    b.commands.push_back(SetDeviceParam{lb.at("Shape"), std::string(kValueParam), 2.0});  // triangle
    b.commands.push_back(SetDeviceParam{u.at("Gain"), std::string(kValueParam), -6.0});
    REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());  // GU.1: values → positions
    playback_fixture::wireSeries(store, {lfoA, lfoB});  // TX.all-free: wired in series (the LFOs off the paths)
    const DeviceId pitch = test_blocks::member(*store.current(), inst, kPaPitch);
    REQUIRE(pitch.valid());
    Batch w{"wires", {}};
    w.commands.push_back(AddWire{Wire{ids.wire(), lfoA, "y", u.at("Gain"), std::string(kValueParam), 0.4, WireReducer::Sum, {}, WireLaw::Travel}});
    w.commands.push_back(AddWire{Wire{ids.wire(), lfoB, "y", u.at("Balance"), std::string(kValueParam), -0.3, WireReducer::Sum, {}, WireLaw::Travel}});
    w.commands.push_back(AddWire{Wire{ids.wire(), lfoB, "y", pitch, std::string(kValueParam), 0.25, WireReducer::Sum, {}, WireLaw::Travel}});
    REQUIRE(store.dispatch(w).ok());
    LoweredProject low = lowered(*store.current());
    REQUIRE(drawn(low.wires, *store.current()).size() == 3);
    for (const WireState& s : low.wires) CHECK(s.status == WireStatus::Lowered);  // no loop
    CHECK(low.taps.size() == 3);
    return low;
  };
  constexpr uint32_t kFrames = 48000;
  const LoweredProject cross = build(true), one = build(false);
  std::vector<std::string> warnings;
  const golden::Channels a = renderLowered(cross, kFrames, {}, &warnings);
  CHECK(warnings.empty());  // no implicit z1 anywhere: the lowering left no delay-free cycle to the scheduler
  float peak = 0.0f;
  for (const auto& c : a)
    for (float v : c) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.02f);
  const double diff = maxDiff(a, renderLowered(one, kFrames));
  MESSAGE("cross-track wire vs the same wire on one track: max |diff| = " << diff);
  CHECK(diff == 0.0);
  // The JIT renders the lowered project bit for bit like the interpreter.
  const double dj = maxDiff(a, renderLowered(cross, kFrames, compile::jitUnitBackend()));
  MESSAGE("interpreter vs JIT: max |diff| = " << dj);
  CHECK(dj == 0.0);
}

TEST_CASE("wires: a wire that closes a delay-free loop gets a 1-sample delay; an LFO's own loop does not") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId t = ids.track(), grp = ids.track(), other = ids.track();
  const DeviceId inst = ids.device(), util = ids.device(), sat = ids.device(), lfo = ids.device(), glfo = ids.device(),
                 inst2 = ids.device(), lfo2 = ids.device();
  Batch b{"loops", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(grp, TrackKind::Group, "Grp"), {}, kAppend});
  b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "Keys"), grp, kAppend});
  (void)playback_fixture::instrument(b, ids, t, inst);
  b.commands.push_back(AddTrack{playback_fixture::track(other, TrackKind::Midi, "Other"), {}, kAppend});
  (void)playback_fixture::instrument(b, ids, other, inst2);
  const Module u = module_batch::push(b, ids, t, Kind::Utility, util);
  const Module s = module_batch::push(b, ids, t, Kind::Saturator, sat);
  const Module l = module_batch::push(b, ids, t, Kind::Lfo, lfo);
  const Module gl = module_batch::push(b, ids, grp, Kind::Lfo, glfo);
  (void)module_batch::push(b, ids, other, Kind::Lfo, lfo2);
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store, {lfo, glfo, lfo2});  // TX.all-free: wired in series (the LFOs off the paths)
  const WireId wLoop = ids.wire(), wSelf = ids.wire(), wBack = ids.wire(), wFeed = ids.wire(), wGroup = ids.wire();
  const std::string v(kValueParam);
  Batch w{"wires", {}};
  // the Saturator's output depends on the Utility's: Saturator → Utility.Gain closes a loop (delayed).
  w.commands.push_back(AddWire{Wire{wLoop, sat, "y", u.at("Gain"), v, 0.2, WireReducer::Sum, {}, WireLaw::Travel}});
  // the LFO's output reads its rate through the phasor's frequency, which the scheduler cannot split a loop at
  // (PATCH_LANGUAGE §3, Q43): a loop, delayed like any other.
  w.commands.push_back(AddWire{Wire{wSelf, lfo, "y", l.at("Rate"), v, 0.1, WireReducer::Sum, {}, WireLaw::Travel}});
  // the LFO into the first effect: its output does not depend on the audio: not a loop.
  w.commands.push_back(AddWire{Wire{wBack, lfo, "y", u.at("Width"), v, 0.5, WireReducer::Sum, {}, WireLaw::Travel}});
  // across tracks: the Saturator's output into the group LFO's depth (Keys plays into the group: no loop yet); then the
  // group LFO into the Saturator's drive closes Keys' Saturator → group LFO → Saturator (delayed; one instrument in the
  // track cycle, so the partition merges Keys and the group into one unit).
  w.commands.push_back(AddWire{Wire{wFeed, sat, "y", gl.at("Depth"), v, 0.5, WireReducer::Sum, {}, WireLaw::Travel}});
  w.commands.push_back(AddWire{Wire{wGroup, glfo, "y", s.at("Drive"), v, 0.3, WireReducer::Sum, {}, WireLaw::Travel}});
  REQUIRE(store.dispatch(w).ok());
  const std::vector<WireState> st = drawn(wireStates(*store.current(), library()), *store.current());
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
  const DeviceId pitch = test_blocks::member(*store.current(), inst, kPaPitch);
  const DeviceId pitch2 = test_blocks::member(*store.current(), inst2, kPaPitch);
  REQUIRE(store.dispatch(Batch{"cross", {AddWire{Wire{wa, lfo, "y", pitch2, v, 0.2, WireReducer::Sum, {}, WireLaw::Travel}},
                                         AddWire{Wire{wb, lfo2, "y", pitch, v, 0.2, WireReducer::Sum, {}, WireLaw::Travel}}}})
              .ok());
  const std::vector<WireState> st2 = drawn(wireStates(*store.current(), library()), *store.current());
  REQUIRE(st2.size() == 7);
  CHECK(st2[5] == WireState{wa, WireStatus::Lowered});
  CHECK(st2[6] == WireState{wb, WireStatus::Refused, WireRefusal::VoiceLoop});
  // Deactivating a block leaves its wires out (inactive), and the project still compiles.
  REQUIRE(store.dispatch(SetDeviceActive{sat, false}).ok());
  const std::vector<WireState> st3 = drawn(wireStates(*store.current(), library()), *store.current());
  CHECK(st3[0].status == WireStatus::Inactive);
  // GU.11: a wire into a control INSIDE the switched-off module (its Drive value block, itself on) still reads as
  // lowered (reported: the status does not look at the enclosing group's activator); the loop it closed is gone, so no
  // delay.
  CHECK(st3[4].status != WireStatus::Delayed);
  auto link2 = makeLink(store.current());
  CHECK(link2->lastError().empty());
}

TEST_CASE("wires: a voice source into a global target is reduced by the wire's reducer") {
  // A probe instrument group (Out = voice.hz) holding a per-voice `voice.velocity` atom, wired into a Utility's Balance
  // after the group: the voices' velocities reduced by the wire's reducer.
  auto render = [&](WireReducer reducer, bool* reducerFound) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId t = ids.track();
    REQUIRE(store.dispatch(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T"), {}, kAppend}).ok());
    const DeviceId inst = test_blocks::addProbeInstrument(store, t);
    const DeviceId vel = ids.device();
    auto a = std::make_shared<Device>(*makeDevice(vel, std::string(kAtomDevicePrefix) + "voice.velocity", DeviceKind::AudioEffect));
    a->group = inst;
    REQUIRE(store.dispatch(AddDevice{t, a, kAppend}).ok());
    const DeviceId util = test_blocks::appendAfter(store, t, "module:utility");
    const DeviceId balance = test_blocks::member(*store.current(), util, "Balance");
    const WireId wid = ids.wire();
    REQUIRE(store.dispatch(AddWire{Wire{wid, vel, "out", balance, std::string(kValueParam), 0.8, reducer, {}, WireLaw::Travel}}).ok());
    const LoweredProject low = lowered(*store.current());
    const WireState* ws = nullptr;
    for (const WireState& s : low.wires)
      if (s.id == wid) ws = &s;
    REQUIRE(ws != nullptr);
    CHECK(ws->status == WireStatus::Lowered);
    const graph::Patch& tp = low.patches[0]->patch;
    const std::string_view want = reducer == WireReducer::Last ? "voice_last" : "voice_sum";
    *reducerFound = false;
    for (graph::NodeIndex n = 0; n < tp.nodeCount(); ++n)
      if (tp.node(n).kind == graph::NodeKind::Atom && graph::atomSignature(tp.node(n).atom).name == want &&
          tp.node(n).id.value == nodeIdFor(NodeRole::Wire, wid.value, 4))
        *reducerFound = true;
    return renderLowered(low, 24000);
  };
  bool sumFound = false, lastFound = false;
  const golden::Channels sum = render(WireReducer::Sum, &sumFound);
  const golden::Channels last = render(WireReducer::Last, &lastFound);
  CHECK(sumFound);
  CHECK(lastFound);
  // Three voices of velocities 0.8, 0.6, 0.9: the summed velocity pans hard right sooner than the last one alone.
  CHECK(maxDiff(sum, last) > 1e-3);
}

TEST_CASE("wires: deleting a block deletes its wires, in one undo step") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId t = ids.track();
  const DeviceId lfo = ids.device(), util = ids.device();
  Batch b{"t", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T"), {}, kAppend});
  (void)module_batch::push(b, ids, t, Kind::Lfo, lfo);
  const Module u = module_batch::push(b, ids, t, Kind::Utility, util);
  REQUIRE(store.dispatch(b).ok());
  const std::string v(kValueParam);
  auto e = ops::connectTravelWire(*store.current(), ids, lfo, "y", u.at("Balance"), v, 1.0);
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  CHECK_FALSE(ops::connectTravelWire(*store.current(), ids, lfo, "y", u.at("Balance"), v).ok());  // already wired
  REQUIRE(store.dispatch(ops::deleteDevices(*store.current(), std::vector<DeviceId>{lfo})->command).ok());
  CHECK(drawnWires(*store.current()).empty());
  REQUIRE(store.undo());
  CHECK(drawnWires(*store.current()).size() == 1);  // one undo step restores the block and its wire
}

// T1.x-silence: a modulation source on another track keeps the target's unit running (and its ring moving) while it is
// non-zero, through the bus-silence flags; the tap is zero exactly when the modulation is.
TEST_CASE("wires: a cross-track modulation keeps the target unit awake while it is non-zero") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId a = ids.track(), bt = ids.track();
  const DeviceId instA = ids.device(), utilA = ids.device(), instB = ids.device(), utilB = ids.device();
  Batch b{"two", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(a, TrackKind::Midi, "Src"), {}, kAppend});
  (void)playback_fixture::instrument(b, ids, a, instA);
  b.commands.push_back(AddTrack{playback_fixture::track(bt, TrackKind::Midi, "Dst"), {}, kAppend});
  (void)playback_fixture::instrument(b, ids, bt, instB);
  (void)module_batch::push(b, ids, a, Kind::Utility, utilA);
  const Module ub = module_batch::push(b, ids, bt, Kind::Utility, utilB);
  // Src plays a note 2.0–2.5 s; Dst never plays (two instruments: the wire stays a bus between their units, Q84).
  b.commands.push_back(AddClip{a, playback_fixture::clip(ids, 0, 16 * kPpq, {playback_fixture::note(ids, 4 * kPpq, kPpq, 0)}, "n")});
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store);  // TX.all-free: wired in series
  // Src's Utility output (its left channel) into Dst's Utility Balance.
  REQUIRE(store.dispatch(AddWire{Wire{ids.wire(), utilA, "y", ub.at("Balance"), std::string(kValueParam), 0.5, WireReducer::Sum, {}, WireLaw::Travel}}).ok());
  auto link = makeLink(store.current());
  REQUIRE(link->lastError().empty());
  REQUIRE(drawn(link->wireStates(), *store.current()).size() == 1);
  CHECK(drawn(link->wireStates(), *store.current())[0].status == WireStatus::Lowered);
  Driver d{*link};
  engine::ProjectProgram& program = *link->engine().program();
  REQUIRE(program.numUnits() >= 3);
  const views::ModTarget tgt{ub.at("Balance"), std::string(kValueParam)};
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
  // Q135 item 7 records the cost and the follow-up.
  d.run(static_cast<uint32_t>(0.4 * kRate / kBlock));  // the note: Src's output moves Dst's Balance
  link->readModulation(std::span(&tgt, 1), std::span(&sw, 1));
  CHECK(sw.up + sw.down > 1e-3f);
}

// TMM.10-fix2 (SPEC §15 2026-11-01 "the note source is a Hz piano roll"): wires from a MIDI track's note source. Its
// velocity into the instrument's Pitch is per voice (each of three notes of velocities 0.8 / 0.6 / 0.9 detuned by its
// own); into an effect's control (Utility's Balance) through the wire's reducer (sum vs last differ). Every block runs
// under the allocation trap (renderGraph); the JIT renders it bit for bit like the interpreter.
TEST_CASE("wires (TMM.10-fix2): the note source's outputs wired per voice and through a reducer; interpreter == JIT") {
  auto render = [&](bool velocityWire, WireReducer reducer, engine::UnitBackend backend, std::vector<WireStatus>* states) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId t = ids.track();
    const DeviceId inst = ids.device(), util = ids.device();
    Batch b{"notes", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T"), {}, kAppend});
    (void)playback_fixture::instrument(b, ids, t, inst);
    const Module u = module_batch::push(b, ids, t, Kind::Utility, util);
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
    if (velocityWire) {
      auto e = ops::connectTravelNoteWire(*store.current(), ids, t, "velocity", test_blocks::member(*store.current(), inst, kPaPitch),
                                          std::string(kValueParam), 0.6);
      REQUIRE(e.ok());
      REQUIRE(store.dispatch(e->command).ok());
    }
    auto e = ops::connectTravelNoteWire(*store.current(), ids, t, "velocity", u.at("Balance"), std::string(kValueParam), 0.8);
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
    REQUIRE(store.dispatch(ops::setWireReducer(*store.current(), e->wires.at(0), reducer)->command).ok());
    const LoweredProject low = lowered(*store.current());
    for (const WireState& s : low.wires) states->push_back(s.status);
    return renderLowered(low, 24000, backend);
  };
  std::vector<WireStatus> st;
  const golden::Channels sum = render(true, WireReducer::Sum, {}, &st);
  const golden::Channels last = render(true, WireReducer::Last, {}, &st);
  const golden::Channels plain = render(false, WireReducer::Sum, {}, &st);
  const golden::Channels jit = render(true, WireReducer::Sum, compile::jitUnitBackend(), &st);
  for (const WireStatus s : st) CHECK(s == WireStatus::Lowered);
  float peak = 0.0f;
  for (const auto& c : sum)
    for (float v : c) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.02f);
  CHECK(maxDiff(sum, last) > 1e-3);   // the reducer decides what the global balance hears
  CHECK(maxDiff(sum, plain) > 1e-3);  // the per-voice velocity wire into the instrument is heard
  const double dj = maxDiff(sum, jit);
  MESSAGE("note-source wires, interpreter vs JIT: max |diff| = " << dj);
  CHECK(dj == 0.0);
}
