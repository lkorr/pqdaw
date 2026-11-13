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
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/engine/project_compile.h"
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
