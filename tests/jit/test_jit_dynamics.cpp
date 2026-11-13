// DY.1: the dynamics atoms through whole images: follow (param- and audio-rate time constants), window (max / min /
// mean, an `smp` length) and latency with a `time` — interpreter and JIT bit-identical in odd block sizes, RtScope on
// every process(); latency(x, frames, time) REPORTS and DELAYS the same whole number of samples, frames +
// round(time · sr), at 44.1 / 48 / 96 kHz (pathLatency == the impulse's position).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../engine/interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/interpreter.h"
#include "pq/graph/latency.h"
#include "pq/jit/compiler.h"

using namespace interp_test;
namespace jit = pq::jit;

namespace {

jit::Compiler& compiler() {
  static std::unique_ptr<jit::Compiler> c = [] {
    auto r = jit::Compiler::create();
    REQUIRE(r.ok());
    return std::move(r.value());
  }();
  return *c;
}

pq::Result<std::unique_ptr<eng::ProgramImage>> buildImage(int backend, const Schedule& s, uint32_t maxBlock) {
  static const eng::Resources res{};
  if (backend == 0) {
    eng::InterpretOptions io;
    io.maxBlockFrames = maxBlock;
    io.resources = &res;
    return eng::buildInterpreted(s, io);
  }
  jit::CompileOptions o;
  o.maxBlockFrames = maxBlock;
  o.resources = &res;
  return compiler().compile(s, o);
}

// Renders a global patch with audio inputs `x` (by ordinal) for `n` frames in blocks of `block`; every output in the
// order of `outNames`.
std::vector<std::vector<float>> render(int backend, const Compiled& c, double sr, const std::vector<std::vector<float>>& x,
                                       const std::vector<std::string>& outNames, uint32_t n, uint32_t block,
                                       uint64_t& violations) {
  auto img = buildImage(backend, c.sched, 64);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const eng::ImageInterface& face = (*img)->io();
  std::vector<std::vector<float>> in(face.globalIn.size(), std::vector<float>(64)), out(face.globalOut.size(), std::vector<float>(64));
  std::vector<const float*> inP;
  std::vector<float*> outP;
  for (auto& v : in) inP.push_back(v.data());
  for (auto& v : out) outP.push_back(v.data());
  REQUIRE(face.globalOut.size() == outNames.size());
  std::vector<std::vector<float>> got(outNames.size());
  const pq::graph::Patch& P = c.sched.rewritten ? c.sched.rewritten->patch : c.typed.patch;
  for (uint32_t t0 = 0; t0 < n; t0 += block) {
    const uint32_t m = std::min(block, n - t0);
    for (size_t s = 0; s < in.size(); ++s) std::copy_n(x[face.globalIn[s].ordinal].begin() + t0, m, in[s].begin());
    eng::ProcessContext ctx;
    ctx.frames = m;
    ctx.sampleRate = sr;
    ctx.sampleTime = t0;
    ctx.in = inP.data();
    ctx.numIn = static_cast<uint32_t>(inP.size());
    ctx.out = outP.data();
    ctx.numOut = static_cast<uint32_t>(outP.size());
    violations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      (*img)->process(ctx);
    });
    for (size_t s = 0; s < out.size(); ++s) {
      const std::string& name = P.nodeName(face.globalOut[s].node);
      const size_t k = static_cast<size_t>(std::find(outNames.begin(), outNames.end(), name) - outNames.begin());
      REQUIRE(k < got.size());
      got[k].insert(got[k].end(), out[s].begin(), out[s].begin() + m);
    }
  }
  return got;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

double outLatency(const pq::graph::TypedPatch& t, const std::string& name) {
  const auto lat = pq::graph::pathLatency(t);
  for (pq::graph::NodeIndex n = 0; n < t.patch.nodeCount(); ++n)
    if (t.patch.node(n).kind == pq::graph::NodeKind::Output && t.patch.nodeName(n) == name)
      return pq::graph::arrivalLatency(t, lat, n);
  FAIL("no output " << name);
  return -1.0;
}

}  // namespace

TEST_CASE("follow / window / latency(time): interpreter == JIT bit for bit, odd blocks; RtScope") {
  constexpr double kSr = 48000.0;
  const Compiled c = compileText(R"(
patch D {
  in x : audio
  in a : audio
  param att = 2ms [0ms..1s]
  param rel = 50ms [1ms..10s]
  out f1 : audio
  out f2 : audio
  out f3 : audio
  out wmax : audio
  out wmin : audio
  out wmean : audio
  out lat : audio
  f1 = follow(abs(x), att, rel)
  f2 = follow(abs(x), a, 0.02)
  f3 = follow(x, 0, rel)
  wmax = window(x, 1ms, 0)
  wmin = window(x, 7smp, 1)
  wmean = window(x * x, 5ms, 2)
  lat = latency(x, 6, 0.0101)
}
)", kSr);
  constexpr uint32_t kN = 6000;
  std::vector<float> x(kN), a(kN);
  for (uint32_t i = 0; i < kN; ++i) {
    x[i] = static_cast<float>(std::sin(i * 0.031) * (0.2 + 0.8 * ((i / 700) % 2)) + 0.1 * std::sin(i * 0.77));
    a[i] = static_cast<float>(0.001 + 0.004 * (0.5 + 0.5 * std::sin(i * 0.003)));
  }
  const std::vector<std::string> names = {"f1", "f2", "f3", "wmax", "wmin", "wmean", "lat"};
  uint64_t violations = 0;
  const auto i37 = render(0, c, kSr, {x, a}, names, kN, 37, violations);
  const auto j37 = render(1, c, kSr, {x, a}, names, kN, 37, violations);
  const auto j64 = render(1, c, kSr, {x, a}, names, kN, 64, violations);
  for (size_t k = 0; k < names.size(); ++k) {
    INFO(names[k]);
    CHECK(sameBits(i37[k], j37[k]));
    CHECK(sameBits(j37[k], j64[k]));
  }
  CHECK(violations == 0);
  // Spot values: the window max of the last 48 frames and the 7-frame min, against the input; the latency's delay.
  for (uint32_t t = 100; t < kN; t += 397) {
    float mx = -1e30f, mn = 1e30f;
    for (uint32_t k = 0; k < 48; ++k) mx = std::max(mx, x[t - k]);
    for (uint32_t k = 0; k < 7; ++k) mn = std::min(mn, x[t - k]);
    CHECK(i37[3][t] == mx);
    CHECK(i37[4][t] == mn);
  }
  for (uint32_t t = 491; t < kN; t += 211) CHECK(i37[6][t] == x[t - 491]);  // 6 + round(0.0101 · 48000) = 6 + 485
  CHECK(outLatency(c.typed, "lat") == 491.0);
}

TEST_CASE("latency(x, frames, time): reports and delays frames + round(time · sr) whole samples at 44.1 / 48 / 96 kHz") {
  for (const double sr : {44100.0, 48000.0, 96000.0}) {
    INFO("sr " << sr);
    const Compiled c = compileText(R"(
patch L {
  in x : audio
  out y : audio
  out z : audio
  y = latency(x, 6, 0.0101)
  z = latency(x, 0, 10ms)
}
)", sr);
    const double wantY = 6.0 + std::round(0.0101 * sr), wantZ = std::round(0.010 * sr);
    CHECK(wantY == pq::graph::latencyFrames(6.0, 0.0101, sr));
    CHECK(outLatency(c.typed, "y") == wantY);
    CHECK(outLatency(c.typed, "z") == wantZ);
    constexpr uint32_t kN = 2048;
    std::vector<float> imp(kN, 0.0f);
    imp[0] = 1.0f;
    uint64_t violations = 0;
    for (int backend = 0; backend < 2; ++backend) {
      const auto out = render(backend, c, sr, {imp}, {"y", "z"}, kN, 53, violations);
      for (size_t k = 0; k < 2; ++k) {
        const double want = k == 0 ? wantY : wantZ;
        for (uint32_t t = 0; t < kN; ++t) {
          INFO("backend " << backend << ", out " << k << ", frame " << t);
          REQUIRE(out[k][t] == (t == static_cast<uint32_t>(want) ? 1.0f : 0.0f));
        }
      }
    }
    CHECK(violations == 0);
  }
}

TEST_CASE("latency(time): optimise() folds the time into the frames; without a sample rate the scheduler refuses it") {
  const char* text = "patch L {\n  in x : audio\n  out y : audio\n  y = latency(x, 6, 0.0101)\n}\n";
  ParseResult pr = parseDetailed(text, "test.pqp");
  REQUIRE(pr.ok());
  PatchLibrary lib;
  lib.add(pr.file);
  InferOptions io;
  io.library = &lib;
  for (const double optSr : {48000.0, 0.0}) {
    INFO("optimise sample rate " << optSr);
    auto inf = infer(pr.file.patches[0], io);
    REQUIRE(inf.ok());
    OptimiseOptions oo;
    oo.sampleRate = optSr;
    auto opt = optimise(std::move(inf.value()), oo);
    REQUIRE(opt.ok());
    const pq::graph::Patch& P = opt.value().patch;
    const pq::graph::AtomId latency = pq::graph::atomId("latency");
    uint32_t found = 0;
    for (pq::graph::NodeIndex n = 0; n < P.nodeCount(); ++n) {
      if (P.node(n).kind != pq::graph::NodeKind::Atom || P.node(n).atom != latency) continue;
      ++found;
      const pq::graph::Adjacency adj = P.adjacency();
      const pq::graph::WireIndex fw = adj.driver[P.flatInput(n, 1)], tw = adj.driver[P.flatInput(n, 2)];
      REQUIRE(fw != pq::graph::kNone);
      const double frames = P.constant(P.wire(fw).from.node).value();
      if (optSr > 0.0) {
        CHECK(frames == 491.0);
        CHECK(tw == pq::graph::kNone);
      } else {
        CHECK(frames == 6.0);
        CHECK(tw != pq::graph::kNone);
      }
    }
    CHECK(found == 1);
    ScheduleOptions so;
    so.sampleRate = 48000.0;
    auto s = schedule(opt.value(), so);
    CHECK(s.ok() == (optSr > 0.0));
    if (!s.ok()) CHECK(s.error().message.find("was not folded into its frames") != std::string::npos);
  }
}
