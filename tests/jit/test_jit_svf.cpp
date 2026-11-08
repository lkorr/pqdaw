// EQ.1: the svf atom through whole images — interpreter and JIT bit-identical (param-rate and audio-rate cutoff, odd
// block sizes), both equal to svfResponse of the design (the impulse response, transformed) within 0.01 dB / 0.1°, for
// every mode, both prewarps and stages 1 / 2 / 8 (64: slow) (the cutoff sweep 20 Hz … 0.45·sr is tests/dsp/test_svf.cpp's,
// on the kernel both backends call); RtScope on every process(). (The atom:svf block: tests/document/test_blocks.cpp.)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../engine/interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/svf_design.h"
#include "pq/engine/interpreter.h"
#include "pq/jit/compiler.h"

using namespace interp_test;
namespace jit = pq::jit;
namespace dsp = pq::dsp;
using cd = std::complex<double>;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = std::numbers::pi;

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
std::vector<std::vector<float>> render(int backend, const Compiled& c, const std::vector<std::vector<float>>& x,
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
    ctx.sampleRate = kSr;
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

struct Spec {
  float cutoff, q, gain;
  uint32_t stages;
};

// Worst |dB| and |°| of the impulse response's transform against svfResponse, where |H| is within 60 dB of its
// maximum on 160 frequencies (as tests/dsp/test_svf.cpp).
void checkResponse(const std::vector<float>& h, const dsp::SvfParams& p, const std::string& what) {
  std::vector<double> w;
  const double lo = std::log(10.0), hi = std::log(0.49 * kSr);
  for (int i = 0; i < 96; ++i) w.push_back(2.0 * kPi * std::exp(lo + (hi - lo) * i / 95.0) / kSr);
  const double a = std::log(p.cutoffHz * std::sqrt(0.5)), b = std::log(std::min(p.cutoffHz * std::sqrt(2.0), 0.49 * kSr));
  for (int i = 0; i < 64; ++i) w.push_back(2.0 * kPi * std::exp(a + (b - a) * i / 63.0) / kSr);
  std::vector<cd> want;
  double top = 0.0;
  for (double x : w) {
    want.push_back(dsp::svfResponse(p, x));
    top = std::max(top, std::abs(want.back()));
  }
  double worstDb = 0.0, worstDeg = 0.0;
  int compared = 0;
  for (size_t i = 0; i < w.size(); ++i) {
    if (std::abs(want[i]) < top * 1e-3) continue;
    cd acc(0.0, 0.0), z(1.0, 0.0);
    const cd rot = std::polar(1.0, -w[i]);
    for (size_t n = 0; n < h.size(); ++n) {
      acc += static_cast<double>(h[n]) * z;
      z = (n & 4095) == 4095 ? std::polar(1.0, -w[i] * static_cast<double>(n + 1)) : z * rot;
    }
    const cd r = acc / want[i];
    worstDb = std::max(worstDb, std::fabs(20.0 * std::log10(std::abs(r))));
    worstDeg = std::max(worstDeg, std::fabs(std::arg(r)) * 180.0 / kPi);
    ++compared;
  }
  INFO(what << ": " << worstDb << " dB, " << worstDeg << " deg over " << compared << " frequencies");
  CHECK(compared >= 4);
  CHECK(worstDb <= 0.01);
  CHECK(worstDeg <= 0.1);
}

}  // namespace

namespace {
void backends(const std::vector<Spec>& specs, uint32_t kN) {
  std::vector<float> impulse(kN, 0.0f), cut(kN);
  impulse[0] = 1.0f;
  for (uint32_t i = 0; i < kN; ++i) cut[i] = static_cast<float>(800.0 + 600.0 * std::sin(i * 0.002));
  uint64_t violations = 0;
  for (uint32_t mode = 0; mode < dsp::kSvfModeCount; ++mode)
    for (uint32_t prewarp : {0u, 1u}) {
      std::string text = "patch S {\n  in x : audio\n  in c : audio\n";
      std::vector<std::string> names;
      for (size_t k = 0; k < specs.size(); ++k) {
        names.push_back("y" + std::to_string(k));
        text += "  out " + names.back() + " : audio\n";
      }
      names.push_back("ya");
      text += "  out ya : audio\n";
      char line[256];
      for (size_t k = 0; k < specs.size(); ++k) {
        const Spec& s = specs[k];
        std::snprintf(line, sizeof line, "  y%zu = svf(x, cutoff: %.9g, q: %.9g, gain: %.9g, mode: %u, stages: %u, prewarp: %u)\n", k,
                      s.cutoff, s.q, s.gain, mode, s.stages, prewarp);
        text += line;
      }
      // The cutoff at audio rate (a design per frame).
      std::snprintf(line, sizeof line, "  ya = svf(x, cutoff: c, q: 0.9, gain: 1.5, mode: %u, stages: 2, prewarp: %u)\n}\n", mode,
                    prewarp);
      text += line;
      const Compiled c = compileText(text, kSr);
      std::vector<std::vector<float>> outs[2];
      for (int backend = 0; backend < 2; ++backend) outs[backend] = render(backend, c, {impulse, cut}, names, kN, 37, violations);
      for (size_t k = 0; k < names.size(); ++k) {
        INFO("mode " << mode << " prewarp " << prewarp << " " << names[k]);
        CHECK(sameBits(outs[0][k], outs[1][k]));
      }
      for (size_t k = 0; k < specs.size(); ++k) {
        const Spec& s = specs[k];
        const dsp::SvfParams p = dsp::svfParamsFromInputs(s.cutoff, s.q, s.gain, static_cast<float>(mode),
                                                          static_cast<float>(s.stages), static_cast<float>(prewarp), kSr);
        checkResponse(outs[1][k], p, "mode " + std::to_string(mode) + " prewarp " + std::to_string(prewarp) + " " + names[k]);
      }
    }
  CHECK(violations == 0);
}
}  // namespace

TEST_CASE("svf atom: interpreter == JIT, both = svfResponse (every mode, both prewarps, stages 1 / 2 / 8); RtScope") {
  backends({{1000.0f, 0.70710678f, 2.0f, 1}, {1000.0f, 1.5f, 0.5f, 2}, {1000.0f, 0.70710678f, 2.0f, 8},
            {21600.0f, 1.5f, 3.0f, 1}, {21600.0f, 0.70710678f, 0.3f, 8}},
           1u << 13);
}

TEST_CASE("svf atom: interpreter == JIT, both = svfResponse at 64 stages (the double cascade)" * doctest::test_suite("slow")) {
  backends({{1000.0f, 0.70710678f, 0.5f, 64}, {3000.0f, 0.70710678f, 2.0f, 64}, {21600.0f, 1.5f, 1.0f, 64}}, 1u << 15);
}

TEST_CASE("svf atom: positional arguments (x, cutoff, q, gain, mode, stages, prewarp); interpreter == JIT") {
  const Compiled c = compileText(R"(
patch B {
  in x : audio
  out y : audio
  y = svf(x, 2000, 2, 1, 4, 1, 1)
}
)", kSr);
  std::vector<float> x(512, 0.0f);
  x[0] = 1.0f;
  uint64_t violations = 0;
  const auto a = render(0, c, {x}, {"y"}, 512, 64, violations);
  const auto b = render(1, c, {x}, {"y"}, 512, 64, violations);
  CHECK(sameBits(a[0], b[0]));
  CHECK(violations == 0);
  CHECK(a[0][0] != 0.0f);
}
