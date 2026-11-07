// TX.patching (SPEC §15 2026-11-01 "a wire adds its value in the target param's own units", "free patching in the
// circuit"): wires that ADD in their target's units, from any output into any param or input, at audio rate, and free
// blocks (off the path). Each case renders the lowered project through the interpreter, and the JIT like the
// interpreter, under the allocation trap. GU.11 (one hierarchy: no stock patches to call by name, so no hand-built
// graphs of them): each wire is compared bit for bit with a SECOND FORM of the same math built from blocks —
//   - the note source's Hz into a control adds exactly its Hz: the same as an `add` atom of the control and the notes'
//     Hz, clipped to the control's range by a `clip` atom;
//   - a block's output into a later block's input sums with the path (parallel routing): the same as an `add` atom
//     summing the two before that block;
//   - a free LFO's output wired into a control; a free block nothing reads changes nothing (the same render without it);
//   - a wire that closes a loop through two inputs gets its 1-sample delay: the same as a `z1` atom on that wire;
//   (GU.4: the Phase accumulator / VCA cases are their modules' now: tests/app/test_module_presets.cpp renders their
//   wires into pm / phase / the VCA's knob against the meta modules' references; TMM.18's reflection is below)
// TX.knob-curves: a new (Add) wire adds BEFORE the knob's curve on a curved control (in position), after it on a linear
// one (in units): the LFO's Rate (octave) and Utility's Gain (dB, linear) below.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
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
#include "pq/document/modules.h"
#include "pq/meta/wavetable.h"
#include "../support/knob_positions.h"
#include "../support/module_batch.h"
#include "playback_fixture.h"

using namespace pq;
using namespace pq::doc;
using module_batch::Kind;
using module_batch::Module;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

// Renders a graph through the per-track compile path (the interpreter, or `backend`) with fixed notes on slot 0.
golden::Channels renderGraph(const graph::ProjectGraph& g, const std::vector<engine::ProjectParamLink>& links,
                             const std::vector<float>& values, uint32_t frames, engine::UnitBackend backend = {}) {
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  co.backend = backend;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(g);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
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

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

// TX.all-free: the blocks made `free` (placed off the main path): wired() leaves them out of the series.
std::vector<DeviceId> gOffPath;

// A native engine of `type` (its default state), optionally free (off the series).
DevicePtr metaDevice(DeviceId id, std::string_view type, DeviceKind kind, bool free = false) {
  const meta::Type* t = meta::findType(type);
  REQUIRE(t != nullptr);
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kMetaDevicePrefix) + std::string(type), kind));
  d->meta = std::make_shared<const MetaMap>(MetaMap{MetaEntry{"", defaultMetaInstance(*t)}});
  if (free) gOffPath.push_back(id);
  return d;
}

// GU.4: a free Value block that is the constant `v` (its curve min = max = v): what an unwired VCA was (a constant source
// of its value), wired from its output `y`.
DevicePtr constantDevice(DeviceId id, double v, bool free = true) {
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kValueDevice), DeviceKind::AudioEffect));
  d->curves = {ControlCurve{std::string(kValueParam), dsp::curvePreset(dsp::CurvePreset::Linear, v, v)}};
  if (free) gOffPath.push_back(id);
  return d;
}

// GU.11: an atom block `atom:<name>` (on the path unless `free`).
DevicePtr atomDevice(DeviceId id, std::string_view name, bool free = false) {
  if (free) gOffPath.push_back(id);
  return makeDevice(id, std::string(kAtomDevicePrefix) + std::string(name), DeviceKind::AudioEffect);
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

// TX.all-free: every track built from raw commands wired in series (the blocks made free left off the path).
void wired(DocumentStore& s, std::vector<DeviceId> offPath = {}) {
  offPath.insert(offPath.end(), gOffPath.begin(), gOffPath.end());
  gOffPath.clear();
  playback_fixture::wireSeries(s, offPath);
}

void connect(DocumentStore& store, DeviceId from, std::string port, DeviceId to, std::string param, bool units = false) {
  auto e = ops::connectWire(*store.current(), store.ids(), from, std::move(port), to, std::move(param));
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  if (units)
    if (auto* b = std::get_if<Batch>(&e->command.v))
      for (Command& c : b->commands)
        if (auto* a = std::get_if<AddWire>(&c.v)) a->wire.law = WireLaw::Units;
  REQUIRE(store.dispatch(e->command).ok());
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
    if (const Wire* w = findWire(*store.current(), st.id); w != nullptr && !w->mainLink() && !w->chain &&
        !module_batch::innerWire(*store.current(), *w))
      out.cables.push_back(st);
  for (const LoweredParam& p : out.low.params) {
    out.links.push_back({p.track, p.name, static_cast<uint32_t>(out.values.size())});
    out.values.push_back(p.value);
  }
  return out;
}

golden::Channels render(const Lowered& low, uint32_t frames, engine::UnitBackend backend = {}) {
  return renderGraph(low.low.graph, low.links, low.values, frames, backend);
}

// `a` and `b` (two forms of the same math) render bit for bit alike, and `a`'s JIT like its interpreter; `a`'s render.
golden::Channels sameAs(const Lowered& a, const Lowered& b, uint32_t frames, const std::string& what) {
  const golden::Channels x = render(a, frames), y = render(b, frames);
  CHECK(peakOf(y) > 0.01f);
  const double diff = maxDiff(x, y);
  MESSAGE(what << ": wired vs its second form, max |diff| = " << diff);
  CHECK(diff == 0.0);
  const double dj = maxDiff(x, render(a, frames, compile::jitUnitBackend()));
  MESSAGE(what << ": interpreter vs JIT, max |diff| = " << dj);
  CHECK(dj == 0.0);
  return x;
}

// A MIDI track `keys` with the default instrument group `inst` (first in its list) into `b`.
void keysTrack(Batch& b, IdSource& ids, TrackId keys, DeviceId inst) {
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys"), {}, kAppend});
  (void)playback_fixture::instrument(b, ids, keys, inst);
}

const WireState* stateOf(const LoweredProject& low, WireId w) {
  for (const WireState& s : low.wires)
    if (s.id == w) return &s;
  return nullptr;
}

}  // namespace

TEST_CASE("patching: the note source's Hz into a control adds exactly its Hz (= add + clip atoms; JIT)") {
  // A free Value block (Linear 0 … 20 kHz, its knob at 300 Hz) wired into the Keys mixer; the notes' Hz wired into its
  // control: an Add wire (in units on the Linear curve) and a Units wire — both clip(300 + Σ voices' Hz, 0, 20000) (a
  // block outside the instrument group reduces the voices by their sum). Its second form: the same Value block into an
  // `add` atom whose other input the notes' Hz feed, clipped by a `clip` atom.
  auto build = [](bool second, bool units) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId keys = ids.track();
    const DeviceId inst = ids.device(), v = ids.device(), add = ids.device(), clip = ids.device(), lo = ids.device(),
                   hi = ids.device();
    Batch b{"keys", {}};
    keysTrack(b, ids, keys, inst);
    auto value = std::make_shared<Device>(*constantDevice(v, 0.0));
    dsp::Curve lin = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 20000.0);
    lin.unit = dsp::CurveUnit::Hz;
    value->curves = {ControlCurve{std::string(kValueParam), lin}};
    value->params = {DeviceValue{std::string(kValueParam), dsp::curvePosition(lin, 300.0, 0.0)}};
    b.commands.push_back(AddDevice{keys, value, kAppend});
    if (second) {
      b.commands.push_back(AddDevice{keys, atomDevice(add, "add", true), kAppend});
      b.commands.push_back(AddDevice{keys, atomDevice(clip, "clip", true), kAppend});
      b.commands.push_back(AddDevice{keys, constantDevice(lo, 0.0), kAppend});
      b.commands.push_back(AddDevice{keys, constantDevice(hi, 20000.0), kAppend});
    }
    REQUIRE(store.dispatch(b).ok());
    wired(store);  // TX.all-free: the chain as cables
    if (!second) {
      auto e = ops::connectNoteWire(*store.current(), ids, keys, "hz", v, std::string(kValueParam));
      REQUIRE(e.ok());
      if (units)
        if (auto* bb = std::get_if<Batch>(&e->command.v))
          for (Command& c : bb->commands)
            if (auto* a = std::get_if<AddWire>(&c.v)) a->wire.law = WireLaw::Units;
      REQUIRE(store.dispatch(e->command).ok());
      const Wire& w = *findWire(*store.current(), e->wires.front());
      CHECK(w.law == (units ? WireLaw::Units : WireLaw::Add));
      CHECK(w.amount == 1.0);
    } else {
      connect(store, v, std::string(kValueOutput), add, "a");
      auto e = ops::connectNoteWire(*store.current(), ids, keys, "hz", add, "b");
      REQUIRE(e.ok());
      REQUIRE(store.dispatch(e->command).ok());
      connect(store, add, "out", clip, "x");
      connect(store, lo, std::string(kValueOutput), clip, "lo");
      connect(store, hi, std::string(kValueOutput), clip, "hi");
    }
    Wire out;
    out.id = ids.wire();
    out.from = second ? clip : v;
    out.port = second ? "out" : std::string(kValueOutput);
    out.mixer = keys;
    REQUIRE(store.dispatch(AddWire{out}).ok());
    Lowered low = lowerIt(store);
    for (const WireState& s : low.low.wires) CHECK(s.status == WireStatus::Lowered);
    if (!second) {
      REQUIRE(low.low.taps.size() == 1);
      CHECK(low.low.taps[0].domain == (units ? ModulationTap::Domain::Dsp : ModulationTap::Domain::Curve));
      for (const LoweredParam& p : low.low.params)  // no amount param for an added wire
        for (const Wire& w : wiresOf(*store.current()))
          if (w.fromNoteSource() && !w.port.empty()) CHECK(p.key.find("wire" + std::to_string(w.id.value) + ":amount") == std::string::npos);
    }
    return low;
  };
  const Lowered ref = build(true, false);
  for (const bool units : {false, true}) {
    CAPTURE(units);
    (void)sameAs(build(false, units), ref, 24000, units ? "Hz into a control (Units wire)" : "Hz into a Linear control");
  }
}

TEST_CASE("patching: an instrument's output into a later block's input sums with the path (= an add atom; parallel "
          "routing)") {
  // Keys: Basic Synth → Utility (Width 0: mono) → Utility, and the instrument's output wired into the second Utility too.
  // Its second form: an `add` atom on the path before the second Utility, summing the first Utility and the instrument.
  auto build = [](bool second) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId keys = ids.track();
    const DeviceId inst = ids.device(), u1 = ids.device(), add = ids.device(), u2 = ids.device();
    Batch b{"keys", {}};
    keysTrack(b, ids, keys, inst);
    const Module m1 = module_batch::push(b, ids, keys, Kind::Utility, u1);
    if (second) b.commands.push_back(AddDevice{keys, atomDevice(add, "add"), kAppend});
    (void)module_batch::push(b, ids, keys, Kind::Utility, u2);
    b.commands.push_back(SetDeviceParam{m1.at("Width"), std::string(kValueParam), 0.0});  // the path through u1 in mono …
    REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
    wired(store);  // TX.all-free: the chain as cables
    connect(store, inst, "y", second ? add : u2, second ? "b" : "x");  // … plus the instrument directly into u2
    Lowered low = lowerIt(store);
    REQUIRE(low.cables.size() == 1);
    CHECK(low.cables[0].status == WireStatus::Lowered);
    CHECK(low.low.taps.empty());  // an input has no knob: no tap
    return low;
  };
  (void)sameAs(build(false), build(true), 24000, "parallel input");
}

TEST_CASE("patching: a free LFO's output wired into a control; a free block costs nothing while nothing reads it") {
  // Keys: Basic Synth → Utility, a free LFO (3 Hz) into the Utility's Balance (an Add wire in units: linear), and
  // an idle free Saturator. The idle block changes nothing (the same render without it); the wire is heard.
  auto build = [](bool idle, bool wire) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId keys = ids.track();
    const DeviceId inst = ids.device(), util = ids.device(), lfo = ids.device(), sat = ids.device();
    Batch b{"keys", {}};
    keysTrack(b, ids, keys, inst);
    const Module u = module_batch::push(b, ids, keys, Kind::Utility, util);
    const Module l = module_batch::push(b, ids, keys, Kind::Lfo, lfo);
    if (idle) (void)module_batch::push(b, ids, keys, Kind::Saturator, sat);
    b.commands.push_back(SetDeviceParam{l.at("Rate"), std::string(kValueParam), 3.0});
    REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
    wired(store, {lfo, sat});  // TX.all-free: the chain as cables, the LFO and the idle Saturator off the path
    CHECK(!playback_fixture::onPath(*store.current(), lfo));
    if (wire) connect(store, lfo, "", u.at("Balance"), std::string(kValueParam));
    Lowered low = lowerIt(store);
    REQUIRE(low.cables.size() == (wire ? 1u : 0u));
    for (const WireState& s : low.cables) CHECK(s.status == WireStatus::Lowered);
    CHECK(low.low.taps.size() == (wire ? 1u : 0u));
    return low;
  };
  const golden::Channels a = sameAs(build(true, true), build(false, true), 24000, "free LFO into a control, an idle block");
  CHECK(maxDiff(a, render(build(true, false), 24000)) > 1e-3);  // the wire is heard
}

TEST_CASE("patching: a wire from a later block's output into an earlier one's input closes a loop: 1-sample delay (= a "
          "z1 atom)") {
  // Keys: Basic Synth → Utility → Utility (Gain −6 dB: the loop decays), the second's output wired back into the
  // first's input. Its second form: the same wire through a `z1` atom (no loop left to delay).
  auto build = [](bool second) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId keys = ids.track();
    const DeviceId inst = ids.device(), u1 = ids.device(), u2 = ids.device(), z = ids.device();
    Batch b{"keys", {}};
    keysTrack(b, ids, keys, inst);
    (void)module_batch::push(b, ids, keys, Kind::Utility, u1);
    const Module m2 = module_batch::push(b, ids, keys, Kind::Utility, u2);
    if (second) b.commands.push_back(AddDevice{keys, atomDevice(z, "z1", true), kAppend});
    b.commands.push_back(SetDeviceParam{m2.at("Gain"), std::string(kValueParam), -6.0});
    REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
    wired(store);  // TX.all-free: the chain as cables
    if (second) {
      connect(store, u2, "y", z, "x");
      connect(store, z, "out", u1, "x");
    } else {
      connect(store, u2, "y", u1, "x");
    }
    Lowered low = lowerIt(store);
    REQUIRE(low.cables.size() == (second ? 2u : 1u));
    for (const WireState& s : low.cables) CHECK(s.status == (second ? WireStatus::Lowered : WireStatus::Delayed));
    return low;
  };
  (void)sameAs(build(false), build(true), 24000, "loop through two inputs");
}

TEST_CASE("patching: a feedback wire through the voices' sum is refused, naming why; the track still plays (TMM.13-fix1 H3)") {
  // The review's loop_voice: an engine at the notes (GU.4: an Envelope, was a Phase accumulator) → Wavetable →
  // Saturator, with the Saturator's output wired back into the Envelope's in. The loop passes the voice run's voice_sum
  // (per-voice → global), so its 1-sample delay would need the voices and the global part to alternate per sample: the
  // scheduler refused the whole unit and the track fell silent while the wire showed as Delayed. Now the wire alone is
  // refused (VoiceLoop) and everything else plays.
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId pa = ids.device(), wt = ids.device(), sat = ids.device(), pa2 = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys"), {}, kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(pa, "envelope", DeviceKind::Instrument), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(wt, "wavetable", DeviceKind::AudioEffect), kAppend});
  (void)module_batch::push(b, ids, keys, Kind::Saturator, sat);
  b.commands.push_back(AddDevice{keys, metaDevice(pa2, "wavetable", DeviceKind::AudioEffect, true), kAppend});
  REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
  wired(store);  // TX.all-free: the chain as cables
  playback_fixture::asInstrumentGroups(store);  // IG.1-2: the voice run in its instrument group
  const Lowered plain = lowerIt(store);
  const golden::Channels before = render(plain, 12000);
  CHECK(peakOf(before) > 0.01f);
  auto e = ops::connectWire(*store.current(), ids, sat, "", pa, "in");
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(store.dispatch(e->command).ok());
  const WireId loop = e->wires.front();
  {
    const Lowered low = lowerIt(store);
    const WireState* st = stateOf(low.low, loop);
    REQUIRE(st != nullptr);
    CHECK(st->status == WireStatus::Refused);
    CHECK(st->reason == WireRefusal::VoiceLoop);
    CHECK(wireStates(*store.current(), library()) == low.low.wires);
    // It compiles (renderGraph requires it) and plays as without the wire.
    CHECK(maxDiff(before, render(low, 12000)) == 0.0);
  }
  // A loop inside the voice run stays a per-voice delayed loop (the Wavetable's output into the Envelope's in: per-voice
  // feedback), and so does one through a free block the voices feed (inferred per voice).
  auto inRun = ops::connectWire(*store.current(), ids, wt, "out", pa, "in");
  REQUIRE(inRun.ok());
  REQUIRE(store.dispatch(inRun->command).ok());
  const WireId runLoop = inRun->wires.front();
  // (IG.1-2: the free Wavetable inside the run's instrument group, so its loop stays inside: per voice.)
  REQUIRE(store.dispatch(SetDeviceGroup{pa2, findDevice(*store.current(), pa).device->group}).ok());
  auto toFree = ops::connectWire(*store.current(), ids, wt, "out", pa2, "x");
  REQUIRE(toFree.ok());
  REQUIRE(store.dispatch(toFree->command).ok());
  auto fromFree = ops::connectWire(*store.current(), ids, pa2, "out", wt, "pos");
  REQUIRE(fromFree.ok());
  REQUIRE(store.dispatch(fromFree->command).ok());
  const WireId freeLoop = fromFree->wires.front();
  const Lowered low = lowerIt(store);
  CHECK(stateOf(low.low, loop)->status == WireStatus::Refused);
  CHECK(stateOf(low.low, runLoop)->status == WireStatus::Delayed);
  CHECK(stateOf(low.low, freeLoop)->status == WireStatus::Delayed);
  const golden::Channels fm = render(low, 12000);
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
  auto wiredExp = meta::expand(t, c, "p");
  REQUIRE(wiredExp.ok());
  CHECK(wiredExp->key != plain->key);
  CHECK(wiredExp->patch.findByName("pos").has_value());
}

TEST_CASE("patching (TMM.18): a negative frequency reflects — into the Phase Accumulator's Frequency (signed) or its In") {
  // GU.4: the Phase Accumulator module on an audio track (its In the track's silence: it runs freely at its Frequency)
  // and a constant ±440 (a Value block) wired into its Frequency value block (on a Linear curve: an Add wire in Hz,
  // signed: ±20 kHz; on its default exp0 curve: in position, the signed curve mirroring it, so −p plays exactly −f) or
  // into its In (the hz input: never clamped). The phasor's uint32 increment at −440 is exactly the negated one (the
  // start glides of TX.smooth are the same ramp mirrored), so the backward ramp is the forward one reversed:
  // out(−f) + out(f) = the stage gain (1 − 2^−24 where the phase's low bits are not 0), sample for sample.
  enum Into { kFreqLinear, kFreqCurved, kHz };
  auto renderPa = [](Into into, double hz, engine::UnitBackend backend) {
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
    connect(store, src, std::string(kValueOutput), into == kHz ? pa : roles.freq,
            into == kHz ? std::string() : std::string(kValueParam));
    const Lowered low = lowerIt(store);
    for (const WireState& s : low.low.wires) CHECK(s.status == WireStatus::Lowered);
    return render(low, 9600, backend);
  };
  for (const Into into : {kFreqLinear, kFreqCurved, kHz}) {
    CAPTURE(static_cast<int>(into));
    const bool intoFreq = into != kHz;
    const golden::Channels fwd = renderPa(into, 440.0, {});
    const golden::Channels bwd = renderPa(into, -440.0, {});
    CHECK(maxDiff(bwd, renderPa(into, -440.0, compile::jitUnitBackend())) == 0.0);
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

// A track with module `kind` on its path (Keys with the default instrument before it, or an audio track: the LFO, a
// source), its control `control` at `knob` (in units), and a FREE constant `value` (GU.4: a Value block whose curve is
// that constant; it was an unwired VCA) wired into the control with an Add wire — or no wire. `curve` the control's
// curve (none: the module's own).
Lowered constantInto(Kind kind, const char* control, double knob, std::optional<double> value,
                     std::optional<dsp::Curve> curve = std::nullopt) {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId t = ids.track();
  const DeviceId inst = ids.device(), dev = ids.device(), vca = ids.device();
  Batch b{"t", {}};
  if (kind == Kind::Lfo)
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Audio, "T"), {}, kAppend});
  else
    keysTrack(b, ids, t, inst);
  const Module m = module_batch::push(b, ids, t, kind, dev);
  if (curve) b.commands.push_back(SetParamCurve{m.at(control), std::string(kValueParam), *curve, true});
  b.commands.push_back(SetDeviceParam{m.at(control), std::string(kValueParam), knob});  // (in units: knobPositions)
  REQUIRE(store.dispatch(knob_positions::knobPositions(*store.current(), b, library())).ok());
  if (value) REQUIRE(store.dispatch(AddDevice{t, constantDevice(vca, *value), kAppend}).ok());
  wired(store);  // TX.all-free: the chain as cables
  if (value) connect(store, vca, std::string(kValueOutput), m.at(control), std::string(kValueParam));
  Lowered low = lowerIt(store);
  for (const WireState& s : low.low.wires) CHECK(s.status == WireStatus::Lowered);
  return low;
}

// A module control's own curve (as its group preset builds it).
dsp::Curve controlCurveOf(Kind kind, std::string_view control) {
  IdSource ids;
  Batch b{"x", {}};
  const Module m = module_batch::push(b, ids, TrackId{1}, kind, ids.device());
  for (const DevicePtr& d : m.set.devices)
    if (d->id == m.at(control)) {
      const dsp::Curve* c = d->curve(kValueParam);
      REQUIRE(c != nullptr);
      return *c;
    }
  FAIL("no control");
  return {};
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

TEST_CASE("knob curves: the LFO's octave Rate + a wire of ±x = ±x of the range in octaves (interpreter == JIT)") {
  // GU.11 (EQ Eight's octave cutoff retired): the LFO module's Rate (0.01 … 40 Hz, an octave curve) at 4 Hz: a constant
  // ±x through an Add wire plays the rate at 4 · (40 / 0.01)^±x — that part of the knob's travel up or down, in octaves
  // (the user: "on its octave curve, a wire carrying +1 adds +1 knob travel"). The same render as the knob set there by
  // hand (to float rounding: the wire's path computes the value per sample); far from the TX.patching law (4 ± x Hz).
  const double lo = 0.01, hi = 40.0;
  for (const double s : {0.5, -0.25}) {
    CAPTURE(s);
    const double expect = 4.0 * std::pow(hi / lo, s);
    const Lowered wiredLow = constantInto(Kind::Lfo, "Rate", 4.0, s);
    const Lowered byHand = constantInto(Kind::Lfo, "Rate", expect, std::nullopt);
    const Lowered unitsLaw = constantInto(Kind::Lfo, "Rate", 4.0 + s, std::nullopt);
    REQUIRE(wiredLow.low.taps.size() == 1);
    CHECK(wiredLow.low.taps[0].domain == ModulationTap::Domain::Position);  // the tap is in knob position
    const golden::Channels a = render(wiredLow, 9600);
    CHECK(maxDiff(a, render(wiredLow, 9600, compile::jitUnitBackend())) == 0.0);
    const golden::Channels h = render(byHand, 9600);
    const golden::Channels u = render(unitsLaw, 9600);
    MESSAGE("rate 4 Hz + " << s << " travel: vs the knob at " << expect << " Hz " << relDiff(a, h) << ", vs 4 + " << s
                           << " Hz " << relDiff(a, u));
    CHECK(relDiff(a, h) < 1e-4);
    CHECK(relDiff(a, u) > 0.05);
  }
  // The law itself, exactly: on the octave curve, +0.5 of the knob's travel from any knob is × √(hi / lo).
  const dsp::Curve t = controlCurveOf(Kind::Lfo, "Rate");
  for (const double knob : {0.05, 0.2, 0.5})  // (+0.5 stays inside the range)
    CHECK(dsp::evalCurve(t, dsp::curvePosition(t, knob, 0.0) + 0.5) == doctest::Approx(knob * std::sqrt(hi / lo)).epsilon(1e-9));
}

TEST_CASE("knob curves: a dB knob + 6 = +6 dB (the control and its wires in dB; interpreter == JIT)") {
  // Utility's Gain (−70 … +35 dB, linear in dB): a constant 6 through an Add wire on 0 dB plays +6 dB, as the knob at
  // +6 dB by hand; on −12 dB it plays −6 dB.
  for (const double knobDb : {0.0, -12.0}) {
    CAPTURE(knobDb);
    const Lowered wiredLow = constantInto(Kind::Utility, "Gain", knobDb, 6.0);
    const Lowered byHand = constantInto(Kind::Utility, "Gain", knobDb + 6.0, std::nullopt);
    REQUIRE(wiredLow.low.taps.size() == 1);
    CHECK(wiredLow.low.taps[0].domain == ModulationTap::Domain::Curve);  // in dB
    const golden::Channels a = render(wiredLow, 9600);
    CHECK(maxDiff(a, render(wiredLow, 9600, compile::jitUnitBackend())) == 0.0);
    const golden::Channels h = render(byHand, 9600);
    MESSAGE("gain " << knobDb << " dB + 6: vs the knob at " << knobDb + 6.0 << " dB by hand " << relDiff(a, h));
    CHECK(relDiff(a, h) < 1e-5);
  }
  // On an Exponential curve (bent in dB) the same wire adds travel: +6 is past the end (clipped to +35 dB).
  const dsp::Curve bentCurve = curveWithPreset(controlCurveOf(Kind::Utility, "Gain"), dsp::CurvePreset::Exponential);
  const Lowered bent = constantInto(Kind::Utility, "Gain", 0.0, 6.0, bentCurve);
  const Lowered top = constantInto(Kind::Utility, "Gain", 35.0, std::nullopt);
  REQUIRE(bent.low.taps.size() == 1);
  CHECK(bent.low.taps[0].domain == ModulationTap::Domain::Position);
  CHECK(relDiff(render(bent, 9600), render(top, 9600)) < 1e-5);
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
    if (wire) connect(store, src, std::string(kValueOutput), wire->first ? roles.freq : roles.pitch, std::string(kValueParam));
    const Lowered low = lowerIt(store);
    const golden::Channels a = render(low, 9600);
    CHECK(maxDiff(a, render(low, 9600, compile::jitUnitBackend())) == 0.0);
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
    connect(store, pa, "", vr.value, std::string(kValueParam));
    connect(store, pa, "", vca, "");  // the signal it multiplies, too
    const Lowered low = lowerIt(store);
    for (const WireState& s : low.low.wires) CHECK(s.status == WireStatus::Lowered);
    CHECK(addsInUnits(*findDevice(*store.current(), vr.value).device->curve(kValueParam)) == linear);
    const golden::Channels a = render(low, 4800);
    CHECK(peakOf(a) > 0.0f);
    CHECK(maxDiff(a, render(low, 4800, compile::jitUnitBackend())) == 0.0);
  }
}
