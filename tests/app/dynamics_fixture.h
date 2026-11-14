#pragma once

// DY.4 / DY.6 test support (tests/app/test_compressor_module.cpp, test_limiter_module.cpp): the dynamics modules played
// through the real lowering and the link, every block under RtScope.
//   render       a project for `frames` (64-frame blocks, 48 kHz, interpreter or JIT), both channels; its reported
//                output latency
//   SourceKit    a group of atom and value blocks to feed a module: op("mul", {{"a", x}, {"b", V(2)}}) is one atom
//                block with its inputs wired; from(beat, b) is [beat ≥ b] (a step within one frame); finish(l, r) wires
//                Out = pack(l, r)
//   sourceBlocks a level schedule a0 + Σ dᵢ·[beat ≥ bᵢ] (transport.beat, 120 BPM: a beat is 24000 frames) times DC or a
//                1 kHz sine, on both channels
//   atomsOf      the atoms of a project's optimised program (every unit), by name
//   sleepRender  AU.dsp (RB-DY H4): render (interpreter, the link's sleep and idle units on or off) and, per 64-frame
//                block, whether every unit of the program was skipped as idle; noteTrack: a MIDI track (its default
//                instrument) playing `notes` = (start, length) in ticks, degree 0
//   NoteRig      a MIDI track (its default instrument) playing short notes, then module `key` on its path; set() a
//                value block of the module in its curve's units (the H4 sleep tests)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../golden/golden_audio.h"
#include "../support/blocks.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/document/store.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/atoms.h"

namespace dynamics_fixture {

using namespace pq;
using namespace pq::doc;

inline constexpr uint32_t kBlock = 64;
inline constexpr double kRate = 48000.0;
inline constexpr double kBeat = 24000.0;  // frames per beat at the default 120 BPM

inline const DeviceLibrary& library() { return playback_fixture::unitsLibrary(); }

inline golden::Channels render(const Snapshot& s, uint32_t frames, bool jit = false, uint32_t* latency = nullptr) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  app::ProjectLink& link = **l;
  REQUIRE(link.waitForMetaTables());
  if (jit) REQUIRE(link.waitForJit(180.0));
  if (latency != nullptr) *latency = link.outputLatency();
  link.play(0);
  golden::Channels out(2);
  std::vector<float> L(kBlock), R(kBlock);
  int64_t t = 0;
  for (uint32_t b = 0; b < frames / kBlock; ++b) {
    float* ch[2] = {L.data(), R.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
    }
    t += kBlock;
    out[0].insert(out[0].end(), L.begin(), L.end());
    out[1].insert(out[1].end(), R.begin(), R.end());
  }
  CHECK(link.lastError().empty());
  return out;
}

struct SleepRun {
  golden::Channels out;
  std::vector<uint8_t> idle;  // per block: every unit was skipped as idle
};

inline SleepRun sleepRender(const Snapshot& s, uint32_t frames, bool idleUnits) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  c.idleUnits = idleUnits;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  app::ProjectLink& link = **l;
  REQUIRE(link.waitForMetaTables());
  link.play(0);
  SleepRun r;
  r.out.assign(2, {});
  std::vector<float> L(kBlock), R(kBlock);
  int64_t t = 0;
  engine::ProjectProgram* prog = link.engine().program();
  REQUIRE(prog != nullptr);
  std::vector<uint64_t> before(prog->numUnits(), 0);
  for (uint32_t b = 0; b < frames / kBlock; ++b) {
    float* ch[2] = {L.data(), R.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
    }
    t += kBlock;
    r.out[0].insert(r.out[0].end(), L.begin(), L.end());
    r.out[1].insert(r.out[1].end(), R.begin(), R.end());
    bool all = true;
    for (uint32_t u = 0; u < prog->numUnits(); ++u) {
      all = all && prog->unitStats(u).idleBlocks == before[u] + 1;
      before[u] = prog->unitStats(u).idleBlocks;
    }
    r.idle.push_back(all ? 1 : 0);
  }
  CHECK(link.lastError().empty());
  return r;
}

inline double db(double x) { return 20.0 * std::log10(std::max(x, 1e-30)); }
inline double peakOf(const std::vector<float>& c, size_t from, size_t to) {
  double p = 0.0;
  for (size_t i = from; i < std::min(to, c.size()); ++i) p = std::max(p, static_cast<double>(std::fabs(c[i])));
  return p;
}

// A group of atom and value blocks (In unused).
struct SourceKit {
  using Ref = std::pair<DeviceId, bool>;  // (block, true): a value block's `y`; (block, false): an atom's `out`
  IdSource& ids;
  BlockSet s;
  DeviceId g;
  explicit SourceKit(IdSource& i) : ids(i), g(i.device()) {
    auto group = std::make_shared<Device>(*makeGroup(g));
    group->name = "Source";
    s.devices.push_back(group);
  }
  void wire(DeviceId from, std::string port, DeviceId to, std::string param) {
    Wire w;
    w.id = ids.wire();
    w.from = from;
    w.port = std::move(port);
    w.to = to;
    w.param = std::move(param);
    w.law = WireLaw::Add;
    s.wires.push_back(std::move(w));
  }
  Ref V(double v) {
    const DeviceId id = ids.device();
    auto d = std::make_shared<Device>(*makeDevice(id, std::string(kValueDevice), DeviceKind::AudioEffect));
    d->group = g;
    d->params = {DeviceValue{std::string(kValueParam), 1.0}};
    d->curves = {ControlCurve{std::string(kValueParam), dsp::curvePreset(dsp::CurvePreset::Linear, v, v)}};
    s.devices.push_back(d);
    return {id, true};
  }
  Ref op(const char* name, std::vector<std::pair<const char*, Ref>> ins) {
    const DeviceId id = ids.device();
    auto d = std::make_shared<Device>(*makeDevice(id, std::string(kAtomDevicePrefix) + name, DeviceKind::AudioEffect));
    d->group = g;
    s.devices.push_back(d);
    for (const auto& [port, src] : ins) wire(src.first, src.second ? std::string(kValueOutput) : std::string("out"), id, port);
    return {id, false};
  }
  Ref mul(Ref a, Ref b) { return op("mul", {{"a", a}, {"b", b}}); }
  Ref add(Ref a, Ref b) { return op("add", {{"a", a}, {"b", b}}); }
  Ref sub(Ref a, Ref b) { return op("sub", {{"a", a}, {"b", b}}); }
  // [x ≥ b] as clip((x − b)·1e7, 0, 1): a step within one frame of a beat ramp
  Ref from(Ref x, double b) { return op("clip", {{"x", mul(sub(x, V(b)), V(1e7))}, {"lo", V(0.0)}, {"hi", V(1.0)}}); }
  BlockSet finish(Ref left, Ref right) {
    const Ref pk = op("pack", {{"in0", left}, {"in1", right}});
    wire(pk.first, "out", g, std::string(kGroupOutPort));
    return std::move(s);
  }
};

// The level schedule: `a0` until the first step, then each step's level from its beat on.
struct Step {
  double beat, level;
};

// Out = (level schedule · carrier) on two channels.
inline BlockSet sourceBlocks(IdSource& ids, double a0, const std::vector<Step>& steps, bool sine) {
  SourceKit k(ids);
  const SourceKit::Ref beat = k.op("transport.beat", {});
  SourceKit::Ref acc = k.V(a0);
  double prev = a0;
  for (const Step& st : steps) {
    acc = k.op("muladd", {{"a", k.from(beat, st.beat)}, {"b", k.V(st.level - prev)}, {"c", acc}});
    prev = st.level;
  }
  SourceKit::Ref x = acc;
  if (sine) x = k.mul(acc, k.op("sine", {{"phase", k.op("phasor", {{"hz", k.V(1000.0)}})}}));
  return k.finish(x, x);
}

struct NoteRig {
  DocumentStore store;
  TrackId track;
  DeviceId mod;
  // Notes (start, length) in ticks, degree 0.
  NoteRig(std::string_view key, const std::vector<std::pair<Ticks, Ticks>>& notes) {
    store.setDeviceLibrary(&library());
    auto e = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Synth");
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
    track = e->tracks.at(0);
    std::vector<Note> ns;
    Ticks end = 0;
    for (const auto& [start, length] : notes) {
      ns.push_back(playback_fixture::note(store.ids(), start, length, 0));
      end = std::max(end, start + length);
    }
    REQUIRE(store.dispatch(AddClip{track, playback_fixture::clip(store.ids(), 0, end + kPpq, ns, "Notes")}).ok());
    mod = test_blocks::addModule(store, track, key);
  }
  void set(std::string_view name, double value) {
    const DeviceId v = test_blocks::member(*store.current(), mod, name);
    REQUIRE_MESSAGE(v.valid(), std::string(name));
    const dsp::Curve& c = *findDevice(*store.current(), v).device->curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)}).ok());
  }
};

// AU.dsp (RB-DY H4): notes at 0 (1/8 s) and at `secondBeat`: every unit idle from within `tailS` of the first note's end
// until the second note, the output exactly 0 while idle, the second note audible, and the same samples as a render
// that runs every unit. Returns when the program fell asleep (seconds).
inline double checkSleeps(const Snapshot& s, double secondBeat, double tailS) {
  const double second = secondBeat * kBeat;
  const uint32_t frames = static_cast<uint32_t>((second + 0.5 * kRate) / kBlock) * kBlock;
  const SleepRun a = sleepRender(s, frames, true), b = sleepRender(s, frames, false);
  REQUIRE(a.out[0].size() == b.out[0].size());
  CHECK(std::memcmp(a.out[0].data(), b.out[0].data(), a.out[0].size() * sizeof(float)) == 0);
  CHECK(std::memcmp(a.out[1].data(), b.out[1].data(), a.out[1].size() * sizeof(float)) == 0);
  const size_t secondBlock = static_cast<size_t>(second) / kBlock;
  size_t from = secondBlock;  // the first block of the idle run that lasts to the second note
  while (from > 0 && a.idle[from - 1]) --from;
  const double asleepS = static_cast<double>(from * kBlock) / kRate, noteEndS = 0.125;
  MESSAGE("every unit idle from " << asleepS << " s (the note ends at " << noteEndS << " s, the next at " << second / kRate << " s)");
  CHECK(from < secondBlock);
  CHECK(asleepS <= noteEndS + tailS);
  for (size_t i = from * kBlock; i < secondBlock * kBlock; ++i) REQUIRE(a.out[0][i] == 0.0f);
  CHECK(peakOf(a.out[0], secondBlock * kBlock, a.out[0].size()) > 1e-3);
  return asleepS;
}

// The atoms of `s`'s optimised program (every unit), by name.
inline std::map<std::string, int> atomsOf(const Snapshot& s) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  auto low = lower(*s, lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(low->graph);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  std::map<std::string, int> out;
  for (uint32_t u = 0; u < compiler.numUnits(); ++u)
    for (const graph::Node& n : compiler.unit(u).typed->patch.nodes())
      if (n.kind == graph::NodeKind::Atom) ++out[std::string(graph::atomSignature(n.atom).name)];
  return out;
}

}  // namespace dynamics_fixture
