// TX.patching (SPEC §15 2026-11-01 "a wire adds its value in the target param's own units", "free patching in the
// circuit"): wires that ADD in their target's units, from any output into any param or input, at audio rate, and free
// devices (out of the series). Each case renders the lowered project bit for bit like a hand-built ProjectGraph of the
// same math (diff 0) through the interpreter, and the JIT like the interpreter, under the allocation trap:
//   - the note source's Hz into a filter cutoff is +Hz exactly (clamped to the cutoff's range);
//   - a free Phase accumulator's output into another's phase is phase modulation at audio rate, per voice (the free
//     one runs per voice because its output feeds voice processing); the same wire into the `pm` input is identical;
//   - an instrument's output into a later device's audio input sums with the chain (parallel routing);
//   - a free LFO's export wired into a param;
//   - a wire that closes a loop through two inputs gets its 1-sample delay;
//   - a VCA's value modulated in value space (the multiplier, clamped to its min / max; reverses Q183).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/parse.h"
#include "pq/meta/phase_acc.h"
#include "pq/meta/vca.h"
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
DevicePtr metaDevice(DeviceId id, std::string_view type, DeviceKind kind, bool free = false, std::string chainInput = {}) {
  const meta::Type* t = meta::findType(type);
  REQUIRE(t != nullptr);
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kMetaDevicePrefix) + std::string(type), kind));
  d->meta = std::make_shared<const MetaMap>(MetaMap{MetaEntry{"", defaultMetaInstance(*t)}});
  d->free = free;
  d->chainInput = std::move(chainInput);
  return d;
}

// The lowered project of `store` (one MIDI event slot) and its param links.
struct Lowered {
  LoweredProject low;
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
};
Lowered lowerIt(const DocumentStore& store) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  auto low = lower(*store.current(), lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  Lowered out{std::move(*low), {}, {}};
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
  const golden::Channels manual = renderGraph(h.g, h.links, h.values, frames);
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

const WireState* stateOf(const LoweredProject& low, WireId w) {
  for (const WireState& s : low.wires)
    if (s.id == w) return &s;
  return nullptr;
}

}  // namespace

TEST_CASE("patching: the note source's Hz into a filter cutoff adds exactly its Hz (hand-built graph diff 0, JIT)") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), eq = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceSetting{eq, "on1", 1.0});
  b.commands.push_back(SetDeviceSetting{eq, "type1", 6.0});  // High Cut 12
  b.commands.push_back(SetDeviceParam{eq, "freq1", 300.0});
  REQUIRE(store.dispatch(b).ok());
  auto e = ops::connectNoteWire(*store.current(), ids, keys, "hz", eq, "freq1");
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const Wire& w = wiresOf(*store.current()).front();
  CHECK(w.law == WireLaw::Add);
  CHECK(w.amount == 1.0);
  const Lowered low = lowerIt(store);
  REQUIRE(low.low.wires.size() == 1);
  CHECK(low.low.wires[0].status == WireStatus::Lowered);
  REQUIRE(low.low.taps.size() == 1);
  CHECK(low.low.taps[0].units);
  // No amount param for an added wire.
  for (const LoweredParam& p : low.low.params) CHECK(p.key.find(":amount") == std::string::npos);
  // value = clip(knob + Σ voices' Hz, lo, hi) in Hz: the voices' pitches summed (a global target reduces them).
  const auto [lo, hi] = rangeOf("eq_eight", "freq1");
  const std::string text = "patch T {\n  param f1 = 300 [" + num(lo) + ".." + num(hi) + "]\n  out out : audio global stereo\n"
                           "  out done : bool voice mono\n  s = BasicSynth()\n  x = voice_sum(s.out)\n"
                           "  fv = clip(f1 + voice_sum(voice.hz), " + num(lo) + ", " + num(hi) + ")\n"
                           "  e = eq_eight(pack(x, x), on1: 1, type1: 6, freq1: fv)\n"
                           "  export tap_d" + id(eq) + "_freq1 = pack(max(fv - f1, 0), max(f1 - fv, 0))\n"
                           "  done = s.done\n" + midiStages("e.out") + "}\n";
  auto h = hand(text, keys.value, store.current()->master->id.value, true);
  h->link(0, "f1", 300.0f);
  (void)sameAsHand(low, *h, 24000, "Hz into cutoff");
}

TEST_CASE("patching: a free Phase accumulator into another's phase is per-voice phase modulation at audio rate") {
  // Keys: PA2 first (it takes the notes: per voice), PA1 free at 100 Hz with its output wired into PA2's phase (an
  // @wrap param: added, not clamped). PA1's output feeds voice processing, so the compiler runs it per voice (its phase
  // from note-on): exactly what a hand-written patch of the two calls does.
  auto build = [](bool intoPm) {
    auto s = std::make_unique<DocumentStore>();
    IdSource& ids = s->ids();
    const TrackId keys = ids.track();
    const DeviceId pa2 = ids.device(), pa1 = ids.device();
    Batch b{"keys", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys"), {}, kAppend});
    b.commands.push_back(AddDevice{keys, metaDevice(pa2, "phase_acc", DeviceKind::Instrument), kAppend});
    b.commands.push_back(AddDevice{keys, metaDevice(pa1, "phase_acc", DeviceKind::AudioEffect, true), kAppend});
    b.commands.push_back(SetDeviceParam{pa1, "freq", 100.0});
    REQUIRE(s->dispatch(b).ok());
    auto e = ops::connectWire(*s->current(), ids, pa1, "out", pa2, intoPm ? "pm" : "phase");
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(s->dispatch(e->command).ok());
    return std::make_tuple(std::move(s), keys, pa1, pa2);
  };
  auto [store, keys, pa1, pa2] = build(false);
  const Lowered low = lowerIt(*store);
  REQUIRE(low.low.wires.size() == 1);
  CHECK(low.low.wires[0].status == WireStatus::Lowered);
  CHECK(low.low.missingDevices.empty());
  // The hand-written patch: the two calls, PA1 into PA2's phase, and the voice run's 5 ms release (lower.h).
  const std::string text =
      "patch T {\n  param ph2 = 0 [0..1]\n  param f1 = 100Hz [0Hz..20000Hz]\n  out out : audio global stereo\n"
      "  out done : bool voice mono\n"
      "  a1 = meta_phase_acc(freq: f1)\n"
      "  a2 = meta_phase_acc(voice.hz, phase: ph2 + channel(a1, 0))\n"
      "  step = 200 / sr\n  env = voice.gate ? 1 : max(z1(env) - step, 0)\n"
      "  x = voice_sum(a2 * env)\n  done = !voice.gate && env <= 0\n"
      "  export tap_d" + id(pa2) + "_phase = pack(max(ph2 + channel(a1, 0) - ph2, 0), max(ph2 - (ph2 + channel(a1, 0)), 0))\n" +
      midiStages("x") + "}\n";
  auto h = hand(text, keys.value, store->current()->master->id.value, true);
  h->link(0, "ph2", 0.0f);
  h->link(0, "f1", 100.0f);
  const golden::Channels viaPhase = sameAsHand(low, *h, 24000, "PA into phase");
  // The same wire into the `pm` input (added to the phase inside the module): the same samples.
  auto [store2, keys2, pa1b, pa2b] = build(true);
  const Lowered low2 = lowerIt(*store2);
  REQUIRE(low2.low.wires.size() == 1);
  CHECK(low2.low.wires[0].status == WireStatus::Lowered);
  const golden::Channels viaPm = renderGraph(low2.low.graph, low2.links, low2.values, 24000);
  CHECK(maxDiff(viaPhase, viaPm) == 0.0);
  // TX.smooth: the audio-rate wire gets no param ramp (only the knobs do): the same ramps either way.
  CHECK(rampKeys(low.low.graph, low.links) == rampKeys(low2.low.graph, low2.links));
  (void)keys2;
  (void)pa1b;
  (void)pa2b;
  (void)pa1;
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
  REQUIRE(store.dispatch(b).ok());
  auto e = ops::connectWire(*store.current(), ids, inst, "out", u2, "x");  // … plus the instrument directly into u2
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const Lowered low = lowerIt(store);
  REQUIRE(low.low.wires.size() == 1);
  CHECK(low.low.wires[0].status == WireStatus::Lowered);
  CHECK(low.low.taps.empty());  // an input has no knob: no tap
  const std::string text = "patch T {\n  param w1 = 0 [0..4]\n  out out : audio global stereo\n  out done : bool voice mono\n"
                           "  s = BasicSynth()\n  x = voice_sum(s.out)\n  a = utility(pack(x, x), width: w1)\n"
                           "  c = utility(a.out + voice_sum(s.out))\n  done = s.done\n" + midiStages("c.out") + "}\n";
  auto h = hand(text, keys.value, store.current()->master->id.value, true);
  h->link(0, "w1", 0.0f);
  (void)sameAsHand(low, *h, 24000, "parallel input");
}

TEST_CASE("patching: a free LFO's export wired into a param; a free device costs nothing while nothing reads it") {
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
  REQUIRE(store.dispatch(b).ok());
  const DeviceId free[] = {lfo, idle};
  auto f = ops::setDevicesFree(*store.current(), free, true);
  REQUIRE(f.ok());
  REQUIRE(store.dispatch(f->command).ok());
  CHECK(findDevice(*store.current(), lfo).device->free);
  auto e = ops::connectWire(*store.current(), ids, lfo, "lfo", eq, "freq1");
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const Lowered low = lowerIt(store);
  REQUIRE(low.low.wires.size() == 1);
  CHECK(low.low.wires[0].status == WireStatus::Lowered);
  // Both free devices are calls of the track patch (the views draw them), out of the chain: the chain is the
  // instrument → EQ → mixer; the unread saturator is dropped by the compiler.
  const graph::Patch& tp = *low.low.graph.tracks[0].patch;
  CHECK(tp.findByName("d" + id(lfo)).has_value());
  CHECK(tp.findByName("d" + id(idle)).has_value());
  const auto [lo, hi] = rangeOf("eq_eight", "freq1");
  const std::string text = "patch T {\n  param f1 = 400 [" + num(lo) + ".." + num(hi) + "]\n  param r = 3\n"
                           "  out out : audio global stereo\n  out done : bool voice mono\n"
                           "  s = BasicSynth()\n  x = voice_sum(s.out)\n  l = lfo_device(-0.0, rate: r)\n"
                           "  fv = clip(f1 + channel(l.lfo, 0), " + num(lo) + ", " + num(hi) + ")\n"
                           "  e = eq_eight(pack(x, x), on1: 1, type1: 6, freq1: fv)\n"
                           "  export tap_d" + id(eq) + "_freq1 = pack(max(fv - f1, 0), max(f1 - fv, 0))\n"
                           "  done = s.done\n" + midiStages("e.out") + "}\n";
  auto h = hand(text, keys.value, store.current()->master->id.value, true);
  h->link(0, "f1", 400.0f);
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
  REQUIRE(store.dispatch(b).ok());
  auto e = ops::connectWire(*store.current(), ids, u2, "out", u1, "x");
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const Lowered low = lowerIt(store);
  REQUIRE(low.low.wires.size() == 1);
  CHECK(low.low.wires[0].status == WireStatus::Delayed);
  const float g = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  const std::string text = "patch T {\n  param g2 = " + num(g) + "\n  out out : audio global stereo\n"
                           "  out done : bool voice mono\n  s = BasicSynth()\n  x = voice_sum(s.out)\n"
                           "  a = utility(pack(x, x) + z1(c.out))\n  c = utility(a.out, gain: g2)\n  done = s.done\n" +
                           midiStages("c.out") + "}\n";
  auto h = hand(text, keys.value, store.current()->master->id.value, true);
  h->link(0, "g2", g);
  (void)sameAsHand(low, *h, 24000, "loop through two inputs");
}

TEST_CASE("patching: a wire into a VCA's value adds to the multiplier (value space, clamped to its min / max)") {
  // Three MIDI tracks without instruments (role Audio): a VCA unplugged (its `in` reads 1: out = its value) at the
  // position of ×0.25, with (A) and without (B) a free Phase accumulator's output wired into its value, and (C) that
  // Phase accumulator alone in the series. A − B is C: the wire adds the accumulator's samples to the multiplier.
  const meta::VcaLaw law = meta::vcaLaw(meta::VcaCurve::Log, -meta::kVcaRange, meta::kVcaRange, 0.4, 0.0);
  const double pos = meta::vcaPosition(law, 0.25);
  std::set<std::string> ramps[3];
  auto render = [&](int which, double max) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId t = ids.track();
    const DeviceId vca = ids.device(), pa = ids.device();
    Batch b{"t", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T"), {}, kAppend});
    if (which == 2) {
      b.commands.push_back(AddDevice{t, metaDevice(pa, "phase_acc", DeviceKind::AudioEffect, false, "-"), kAppend});
    } else {
      b.commands.push_back(AddDevice{t, metaDevice(vca, "vca", DeviceKind::AudioEffect, false, "-"), kAppend});
      b.commands.push_back(SetDeviceParam{vca, "value", pos});
      b.commands.push_back(SetDeviceParam{vca, "max", max});
      b.commands.push_back(AddDevice{t, metaDevice(pa, "phase_acc", DeviceKind::AudioEffect, true), kAppend});
    }
    b.commands.push_back(SetDeviceParam{pa, "freq", 55.0});
    REQUIRE(store.dispatch(b).ok());
    if (which == 0) {
      auto e = ops::connectWire(*store.current(), ids, pa, "out", vca, "value");
      REQUIRE(e.ok());
      REQUIRE(store.dispatch(e->command).ok());
    }
    const Lowered low = lowerIt(store);
    for (const WireState& s : low.low.wires) CHECK(s.status == WireStatus::Lowered);
    if (which == 0) {
      // The value stays the knob (a param-rate runtime param); the wire enters at value_add.
      const graph::Patch& tp = *low.low.graph.tracks[0].patch;
      const auto call = tp.findByName("d" + id(vca));
      REQUIRE(call.has_value());
      const auto add = tp.findInput(*call, "value_add");
      REQUIRE(add.has_value());
      CHECK(tp.adjacency().driver[tp.flatInput(*call, *add)] != graph::kNone);
      const auto value = tp.findInput(*call, "value");
      REQUIRE(value.has_value());
      CHECK(tp.adjacency().driver[tp.flatInput(*call, *value)] == graph::kNone);
    }
    ramps[which] = rampKeys(low.low.graph, low.links);
    const golden::Channels a = renderGraph(low.low.graph, low.links, low.values, 9600);
    const golden::Channels j = renderGraph(low.low.graph, low.links, low.values, 9600, compile::jitUnitBackend());
    CHECK(maxDiff(a, j) == 0.0);
    return a;
  };
  const golden::Channels a = render(0, meta::kVcaRange), b = render(1, meta::kVcaRange), c = render(2, 0.0);
  CHECK(peakOf(c) > 0.3f);
  // TX.smooth: the wire into value_add (audio rate) gets no ramp (rampKeys checks every source is param rate). The
  // wired track has the VCA track's ramps (its knobs, the mixer) and the accumulator's, plus the VCA's clamp bounds:
  // `min` and one value computed from the knobs, which only the clamp of value + value_add reads (live once wired).
  std::set<std::string> both = ramps[1];
  both.insert(ramps[2].begin(), ramps[2].end());
  CHECK(!ramps[1].empty());
  std::vector<std::string> extra;
  for (const std::string& k : ramps[0])
    if (!both.count(k)) extra.push_back(k);
  for (const std::string& k : both) CHECK(ramps[0].count(k) == 1);
  CHECK(extra.size() <= 2);
  auto endsWith = [](const std::string& k, std::string_view e) { return k.size() >= e.size() && k.compare(k.size() - e.size(), e.size(), e) == 0; };
  for (const std::string& k : extra) {
    const bool bound = k.rfind("key ", 0) == 0 || (k.rfind("param ", 0) == 0 && (endsWith(k, "__min") || endsWith(k, "__max")));
    CHECK_MESSAGE(bound, k);
  }
  double worst = 0.0;
  for (size_t i = 0; i < a[0].size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(a[0][i] - b[0][i] - c[0][i])));
  MESSAGE("VCA value + PA − VCA value − PA: max |diff| = " << worst);
  CHECK(worst < 1e-6);
  // Clamped to its max: with max ×0.5 the multiplier never exceeds 0.5 (out ≤ 0.5 · the stage's gain).
  const golden::Channels clamped = render(0, 0.5);
  const float stage = static_cast<float>(std::sin(0.78539816339744828 + 1.5707963267948966));  // centre pan, left
  float top = 0.0f, unclampedTop = 0.0f;
  for (float v : clamped[0]) top = std::max(top, v);
  for (float v : a[0]) unclampedTop = std::max(unclampedTop, v);
  CHECK(unclampedTop > 1.2f * stage);  // ×0.25 + the accumulator's ramp to ≈ 1
  CHECK(top <= 0.5f * stage + 1e-6f);
  // Where the knob then sits (the curve follows the new max), the ramp reaches the clamp.
  const double v0 = meta::vcaValue(meta::vcaLaw(meta::VcaCurve::Log, -meta::kVcaRange, 0.5, 0.4, 0.0), pos);
  if (v0 + 0.99 > 0.5) CHECK(top > 0.49f * stage);
}

TEST_CASE("patching: an audio-rate wire costs per sample only where it is; an unwired meta param stays param rate") {
  // Indicative timing (check preset, shared machine): a Phase accumulator voice with its phase wired from a free one
  // against the same voice unwired. The unwired expansion is the one before TX.patching (no extra inputs, same key).
  const meta::Type& t = meta::phaseAccType();
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
  uint32_t ins = 0;
  for (graph::NodeIndex n = 0; n < plain->patch.nodeCount(); ++n) ins += plain->patch.node(n).kind == graph::NodeKind::Input ? 1u : 0u;
  CHECK(ins == 0);  // ChainNotes: the note's Hz and the defaults, no `in` at all
  const std::vector<uint8_t> pm{0, 1};
  c.wired = pm;
  auto wired = meta::expand(t, c, "p");
  REQUIRE(wired.ok());
  CHECK(wired->key != plain->key);
  CHECK(wired->patch.findByName("pm").has_value());
}

TEST_CASE("patching (TMM.18): a negative frequency reflects — into the Phase accumulator's freq (@signed) or its hz input") {
  // A free VCA as a constant ±440 (min ±440, max beyond it the other way: a constant min; its `in` unwired reads 1)
  // wired into an unplugged Phase accumulator's `freq` (the Add law, clamped to ±20 kHz: @signed) or its `hz` input
  // (inputs are never clamped). The phasor's uint32 increment at −440 is exactly the negated one (the start glides of
  // TX.smooth are the same ramp mirrored), so the backward ramp is the forward one reversed: out(−f) + out(f) = the
  // stage gain (1 − 2^−24 where the phase's low bits are not 0), sample for sample. Before TMM.18 the freq wire was
  // clamped to [0, 20 kHz]: the accumulator stood at 0 Hz.
  auto render = [](bool intoFreq, double hz, engine::UnitBackend backend) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId t = ids.track();
    const DeviceId pa = ids.device(), vca = ids.device();
    Batch b{"t", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T"), {}, kAppend});
    b.commands.push_back(AddDevice{t, metaDevice(pa, "phase_acc", DeviceKind::AudioEffect, false, "-"), kAppend});
    b.commands.push_back(AddDevice{t, metaDevice(vca, "vca", DeviceKind::AudioEffect, true), kAppend});
    b.commands.push_back(SetDeviceParam{vca, "min", hz});
    b.commands.push_back(SetDeviceParam{vca, "max", hz < 0 ? hz - 60.0 : hz - 140.0});
    REQUIRE(store.dispatch(b).ok());
    auto e = ops::connectWire(*store.current(), ids, vca, "out", pa, intoFreq ? "freq" : "hz");
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    const Lowered low = lowerIt(store);
    for (const WireState& s : low.low.wires) CHECK(s.status == WireStatus::Lowered);
    return renderGraph(low.low.graph, low.links, low.values, 9600, backend);
  };
  for (const bool intoFreq : {true, false}) {
    CAPTURE(intoFreq);
    const golden::Channels fwd = render(intoFreq, 440.0, {});
    const golden::Channels bwd = render(intoFreq, -440.0, {});
    CHECK(maxDiff(bwd, render(intoFreq, -440.0, compile::jitUnitBackend())) == 0.0);
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
    MESSAGE(std::string(intoFreq ? "freq" : "hz") << " wired at -440 Hz: out(-f) + out(f) in [" << lo << ", " << hi
                                                     << "] (stage gain " << gain << ")");
    CHECK(hi <= 1.0001 * gain / (1.0 - 440.0 / 48000.0));
    CHECK(compared > 7000);
    CHECK(worst <= 2.0 * gain * 5.9604644775390625e-08 + 1e-7);
  }
}
