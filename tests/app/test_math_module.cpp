// MX.2 (docs/plans/MX.md "Semantics", "MX.2"; pq/document/modules.h "the Math module"):
//   ops         every op of mathOpInfos() plays mathOpReference over every pair of 16 edge values for A and B (±0,
//               ±1e-30, ±1e-9, ±1e18, …) on the interpreter and the JIT, read back through the module's own trace tap
//               (A, B and Out of the same samples): gates exactly, arithmetic within float rounding, wrap / fold / mod in
//               range where the quotient is past float resolution; nothing non-finite ever leaves. Every block runs
//               under RtScope (the allocation trap).
//   oversample  the group's `oversample` setting puts the module into one oversample region at its factor
//   (perf)      an off-screen Math module (its trace tap unwatched) against a bare op and against the bare op plus an
//               off-screen Oscilloscope module
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/core/tap_ring.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/document/panel.h"
#include "pq/dsp/curve.h"
#include "pq/engine/project_compile.h"
#include "playback_fixture.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

const DeviceLibrary& library() { return playback_fixture::unitsLibrary(); }

struct Doc {
  DocumentStore store;
  TrackId track;
  Doc() {
    store.setDeviceLibrary(&library());
    track = edit(ops::createTrack(doc(), store.ids(), TrackKind::Audio, {}, "T")).tracks.at(0);
  }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  void cmd(Command c) {
    const Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  DeviceId block(std::string patch) {
    return edit(ops::addDevice(doc(), store.ids(), track, std::move(patch), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  }
  DeviceId atom(std::string name) { return block(std::string(kAtomDevicePrefix) + name); }
  DeviceId constant(double v) {  // a value block giving v (a flat linear curve)
    const DeviceId d = block(std::string(kValueDevice));
    cmd(SetParamCurve{d, std::string(kValueParam), dsp::curvePreset(dsp::CurvePreset::Linear, v, v), true});
    return d;
  }
  void wire(DeviceId from, std::string port, DeviceId to, std::string param) {
    edit(ops::connectWire(doc(), store.ids(), from, std::move(port), to, std::move(param)));
  }
  void toMixer(DeviceId from, std::string port) {
    Wire w;
    w.from = from;
    w.port = std::move(port);
    w.mixer = track;
    edit(ops::connect(doc(), store.ids(), w));
  }
  DeviceId module(std::string_view key) {
    return edit(ops::addDevice(doc(), store.ids(), track, std::string(kModuleItemPrefix) + std::string(key), DeviceKind::AudioEffect,
                               ops::kOffPath)).devices.at(0);
  }
  // Math with op `op` and B typed 0 (MX.4: picking an op may set B to its default, e.g. clip 1; the sweep wired into B adds
  // to the typed value, so it is held at 0 to play the edge values exactly).
  DeviceId math(std::string_view op) {
    const DeviceId g = module("math");
    edit(ops::setMathOp(doc(), store.ids(), g, op));
    const DeviceId b = mathInfo(doc(), g)->roles.b;
    const pq::dsp::Curve& c = *findDevice(doc(), b).device->curve(kValueParam);
    edit(ops::setDeviceParam(doc(), b, std::string(kValueParam), pq::dsp::curvePosition(c, pq::dsp::curveOutput(c, 0.0), 0.5)));
    REQUIRE(std::fabs(valueBlockUnits(*findDevice(doc(), b).device)) < 1e-12);
    return g;
  }
  MathInfo info(DeviceId g) const {
    const std::optional<MathInfo> m = mathInfo(doc(), g);
    REQUIRE(m.has_value());
    return *m;
  }
  const Device& dev(DeviceId d) const { return *findDevice(doc(), d).device; }
  // A source stepping through `values` at audio rate: values[floor(phasor(hz) · n)] (a chain of two-input muxes).
  std::pair<DeviceId, std::string> stepper(double hz, const std::vector<double>& values) {
    const double n = static_cast<double>(values.size());
    const DeviceId ph = atom("phasor");
    wire(constant(hz), std::string(kValueOutput), ph, "hz");
    const DeviceId scaled = atom("mul");
    wire(ph, "out", scaled, "a");
    wire(constant(n), std::string(kValueOutput), scaled, "b");
    const DeviceId index = atom("floor");
    wire(scaled, "out", index, "x");
    std::pair<DeviceId, std::string> r{constant(values.back()), std::string(kValueOutput)};
    for (size_t i = values.size() - 1; i-- > 0;) {
      std::pair<DeviceId, std::string> at{index, "out"};
      if (i > 0) {
        const DeviceId s = atom("sub");
        wire(index, "out", s, "a");
        wire(constant(static_cast<double>(i)), std::string(kValueOutput), s, "b");
        at = {s, "out"};
      }
      const DeviceId m = atom("mux");
      wire(at.first, at.second, m, "index");
      wire(constant(values[i]), std::string(kValueOutput), m, "in0");
      wire(r.first, r.second, m, "in1");
      r = {m, "out"};
    }
    return r;
  }
};

// The module's trace scope element (the panel's "scope" bound to A, B and Out).
const PanelElement& traceElement(const Device& g) {
  REQUIRE(g.hasPanel());
  for (const PanelElement& e : g.panel->elements)
    if (!elementTraces(e).empty()) return e;
  FAIL("no trace scope");
  return g.panel->elements.front();
}

std::unique_ptr<app::ProjectLink> makeLink(const Snapshot& s, bool jit) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  c.idleUnits = false;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  REQUIRE((*l)->waitForMetaTables());
  if (jit) REQUIRE((*l)->waitForJit(180.0));
  return std::move(*l);
}

void process(app::ProjectLink& link, uint32_t blocks, int64_t& t) {
  std::vector<float> L(kBlock), R(kBlock);
  for (uint32_t b = 0; b < blocks; ++b) {
    float* ch[2] = {L.data(), R.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
    }
    t += kBlock;
  }
}

// 16 edge values (A steps every sample, B every 16 samples: 256 samples hold every pair).
const std::vector<double> kEdges = {0.0, -0.0, 1e-30, -1e-30, 1e-9, -1e-9, 1e18, -1e18, 0.5, -0.5, 1.0, -1.0, 2.5, -3.0, 7.0, -0.25};

struct Check {
  uint64_t frames = 0, bad = 0, nonFinite = 0;
  double worst = 0.0;
  std::string first;
};

// One frame of op `key`: Out against the reference (`tol` the Tolerance block's value).
void checkFrame(std::string_view key, const MathOpInfo& op, double a, double b, double out, double tol, Check& c) {
  ++c.frames;
  if (!std::isfinite(out)) {
    ++c.nonFinite;
    return;
  }
  const double ref = mathOpReference(key, a, b, tol);
  const double m = std::fabs(b);
  bool ok = true;
  double err = 0.0;
  if (op.gate) {
    ok = out == ref;
  } else if ((key == "wrap" || key == "fold") && m > 0.0 && std::fabs(a) / m > 1024.0) {
    ok = std::fabs(out) <= m * (1.0 + 1e-6);  // the quotient is past float resolution: any point of the range
  } else if (key == "mod" && b != 0.0 && std::fabs(a) / m > 1024.0) {
    ok = out >= std::min(b, 0.0) * (1.0 + 1e-6) && out <= std::max(b, 0.0) * (1.0 + 1e-6);
  } else {
    double scale = std::max({std::fabs(ref), 1e-28});
    if (key == "add" || key == "sub" || key == "avg" || key == "absdiff" || key == "hypot") scale = std::max({scale, std::fabs(a), std::fabs(b)});
    if (key == "wrap" || key == "fold" || key == "mod") scale = std::max(scale, m);  // the quotient's rounding × |B|
    // (+ 1e-20: products of ±1e-30 underflow to 0 under the audio thread's flush-to-zero)
    const double tolAbs = 1e-20 + (key == "atan2" ? 4e-6 : key == "wrap" || key == "fold" || key == "mod" ? 2e-3 * scale : 2e-6 * scale);
    err = std::fabs(out - ref);
    ok = err <= tolAbs;
  }
  if (!ok) {
    if (c.bad++ == 0) {
      char s[160];
      std::snprintf(s, sizeof s, "A %.9g B %.9g out %.9g ref %.9g", a, b, out, ref);
      c.first = s;
    }
    c.worst = std::max(c.worst, err);
  }
}

}  // namespace

TEST_CASE("MX.2 Math: every op against its reference over the edge sweep, interpreter and JIT, read through its trace "
          "tap; nothing non-finite out; RT-safe") {
  Doc d;
  const auto srcA = d.stepper(kRate / 16.0, kEdges);   // a new A every sample
  const auto srcB = d.stepper(kRate / 256.0, kEdges);  // a new B every 16 samples
  struct Mod {
    const MathOpInfo* op;
    DeviceId group;
  };
  std::vector<Mod> mods;
  for (const MathOpInfo& op : mathOpInfos()) {
    const DeviceId g = d.math(op.key);
    const MathInfo mi = d.info(g);
    REQUIRE(mi.op == &op);
    d.wire(srcA.first, srcA.second, g, std::string(kMainPort));         // A: the group's In
    d.wire(srcB.first, srcB.second, mi.roles.inB, std::string(kMainPort));  // B: the second input
    mods.push_back({&op, g});
  }
  const double tol = valueBlockUnits(d.dev(d.info(mods.front().group).roles.tolerance));
  CHECK(tol == doctest::Approx(kMathDefaultTolerance).epsilon(1e-6));
  for (const WireState& s : wireStates(d.doc(), library())) CHECK(s.status == WireStatus::Lowered);
  for (const bool jit : {false, true}) {
    CAPTURE(jit);
    auto link = makeLink(d.store.current(), jit);
    std::vector<std::shared_ptr<TapRing>> rings;
    for (const Mod& m : mods) {
      const PanelElement& e = traceElement(d.dev(m.group));
      rings.push_back(link->metaSignalTap(e.binding(0)->device, traceTapPath(e), 0));
      REQUIRE_MESSAGE(rings.back() != nullptr, std::string(m.op->key));
      rings.back()->addReader();
    }
    int64_t t = 0;
    process(*link, 8, t);  // 512 frames: every (A, B) pair twice
    CHECK(link->lastError().empty());
    for (size_t i = 0; i < mods.size(); ++i) {
      const std::string_view key = mods[i].op->key;
      CAPTURE(key);
      TapRing& ring = *rings[i];
      const uint64_t n = ring.written();
      REQUIRE(n >= 512);
      std::vector<float> a(n), b(n), out(n);
      REQUIRE(ring.read(1, 0, static_cast<uint32_t>(n), a.data()) == n);
      REQUIRE(ring.read(2, 0, static_cast<uint32_t>(n), b.data()) == n);
      REQUIRE(ring.read(3, 0, static_cast<uint32_t>(n), out.data()) == n);
      Check c;
      std::map<std::pair<float, float>, int> pairs;
      for (uint64_t f = 0; f < n; ++f) {
        checkFrame(key, *mods[i].op, a[f], b[f], out[f], tol, c);
        ++pairs[{a[f], b[f]}];
      }
      CHECK_MESSAGE(c.bad == 0, c.bad << " frames off (worst " << c.worst << "), first: " << c.first);
      CHECK(c.nonFinite == 0);
      CHECK(pairs.size() >= 15 * 15);  // (−0 and 0 compare equal as floats: 15 distinct values each)
      ring.removeReader();
    }
  }
}

TEST_CASE("MX.2 Math: the oversample setting puts the module into one region at its factor (2x, 4x); it plays the 1x "
          "result for a static op (+)") {
  Doc d;
  const auto srcA = d.stepper(kRate / 64.0, {0.25, -0.5, 1.0, 0.75});
  const DeviceId g = d.math("add");
  d.wire(srcA.first, srcA.second, g, std::string(kMainPort));
  d.toMixer(g, std::string(kMainPort));
  for (const uint32_t e : {1u, 2u}) {
    d.edit(ops::setDeviceSetting(d.doc(), g, std::string(kGroupOversample), static_cast<double>(e)));
    CHECK(d.store.lastChanges().structural());
    CHECK(groupOversampleFactor(d.dev(g)) == (1u << e));
    LowerOptions lo;
    lo.library = &library().library();
    lo.devices = &library();
    lo.slotOf = [](TrackId) { return 0u; };
    lo.clipSlotOf = [](TrackId) { return 0u; };
    auto low = lower(d.doc(), lo);
    REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
    engine::ProjectCompileOptions co;
    co.sampleRate = kRate;
    co.maxBlockFrames = kBlock;
    engine::ProjectCompiler compiler(co);
    auto build = compiler.build(low->graph);
    REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
    std::vector<uint32_t> factors;
    for (uint32_t u = 0; u < compiler.numUnits(); ++u)
      for (const graph::OversampleInfo& o : compiler.unit(u).typed->oversample) factors.push_back(o.factor);
    CHECK(factors == std::vector<uint32_t>{1u << e});
  }
}

TEST_CASE("MX.2 Math (perf): an off-screen Math module costs its arithmetic plus what an off-screen Oscilloscope "
          "costs" * doctest::test_suite("perf")) {
  constexpr uint32_t kN = 64;
  constexpr uint32_t kBlocks = 3000;
  // Projects with the same two audio-rate sources and kN instances, each output into the mixer. Every instance has its
  // own params (no two compute the same thing, so nothing is merged):
  //   0  Math modules, op + (A + its value, B + its value, the sum; its trace tap unwatched)
  //   1  the same arithmetic as bare blocks: add(srcA, vA), add(srcB, vB), their sum
  //   2  1 plus an off-screen Oscilloscope module on the sum
  //   3  a bare op: add(srcA, v) alone
  auto build = [&](int variant) {
    auto d = std::make_unique<Doc>();
    const auto a = d->stepper(220.0, {0.1, -0.4, 0.8, 0.3});
    const auto b = d->stepper(330.0, {0.2, 0.5, -0.7});
    auto param = [&](uint32_t i) {  // a value block: a runtime param (its knob), distinct per instance
      const DeviceId v = d->block(std::string(kValueDevice));
      d->cmd(SetDeviceParam{v, std::string(kValueParam), 0.001 * (i + 1)});
      return v;
    };
    for (uint32_t i = 0; i < kN; ++i) {
      if (variant == 0) {
        const DeviceId g = d->math("add");
        const MathInfo mi = d->info(g);
        d->cmd(SetDeviceParam{mi.roles.a, std::string(kValueParam), 0.5 + 0.001 * (i + 1)});
        d->wire(a.first, a.second, g, std::string(kMainPort));
        d->wire(b.first, b.second, mi.roles.inB, std::string(kMainPort));
        d->toMixer(g, std::string(kMainPort));
      } else if (variant == 3) {
        const DeviceId x = d->atom("add");
        d->wire(a.first, a.second, x, "a");
        d->wire(param(i), std::string(kValueOutput), x, "b");
        d->toMixer(x, "out");
      } else {
        const DeviceId sa = d->atom("add"), sb = d->atom("add"), x = d->atom("add");
        d->wire(a.first, a.second, sa, "a");
        d->wire(param(i), std::string(kValueOutput), sa, "b");
        d->wire(b.first, b.second, sb, "a");
        d->wire(param(i), std::string(kValueOutput), sb, "b");
        d->wire(sa, "out", x, "a");
        d->wire(sb, "out", x, "b");
        if (variant == 1) {
          d->toMixer(x, "out");
        } else {
          const DeviceId s = d->module("oscilloscope");
          d->wire(x, "out", s, std::string(kMainPort));
          d->toMixer(s, std::string(kMainPort));
        }
      }
    }
    return d;
  };
  const char* names[4] = {"Math module (op +, off-screen)", "its arithmetic as bare blocks",
                          "bare blocks + Oscilloscope module (off-screen)", "one bare add"};
  double ns[4] = {};
  for (int v = 0; v < 4; ++v) {
    auto d = build(v);
    auto link = makeLink(d->store.current(), true);
    int64_t t = 0;
    process(*link, 200, t);
    double best = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
      const auto t0 = std::chrono::steady_clock::now();
      process(*link, kBlocks, t);
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    ns[v] = best * 1e9 / (static_cast<double>(kBlocks) * kBlock * kN);
    MESSAGE(std::string(names[v]) << ": " << ns[v] << " ns per instance per frame (" << kN << " instances, JIT)");
  }
  MESSAGE("Math over its bare arithmetic: " << ns[0] - ns[1] << " ns/frame; over arithmetic + Oscilloscope: " << ns[0] - ns[2]
                                            << " ns/frame; over one bare add: " << ns[0] - ns[3] << " ns/frame");
  CHECK(ns[0] <= ns[2] * 1.15 + 0.02);  // the module's trace tap costs no more than an off-screen Oscilloscope
}
