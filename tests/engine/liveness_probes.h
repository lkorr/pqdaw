#pragma once

// Tests only (T1.5-fix1): the M1 review's buffer-liveness probes (tests/engine/patches/liveness.pqp, from
// docs/reviews/M1/probes/) checked by value on one backend: the interpreter (test_interpreter.cpp) or the JIT
// (test_jit.cpp). Each one rendered wrong (or did not build) on both backends before the fix.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "interp_harness.h"

namespace liveness_probes {

using namespace interp_test;

inline const Compiled& probe(const std::vector<Compiled>& all, const std::string& name) {
  for (const Compiled& c : all)
    if (c.name == name) return c;
  FAIL("probe patch " << name << " did not compile");
  return all.front();
}

inline const std::vector<Compiled>& probes() {
  static const std::vector<Compiled> all = [] {
    std::vector<std::string> skipped;
    auto v = compileFile(patchesDir() / "liveness.pqp", 48000.0, skipped);
    REQUIRE(skipped.empty());
    return v;
  }();
  return all;
}

inline bool bitEqual(const Streams& a, const Streams& b) {
  if (a.size() != b.size()) return false;
  for (const auto& [k, x] : a) {
    const auto it = b.find(k);
    if (it == b.end() || it->second.size() != x.size()) return false;
    if (std::memcmp(x.data(), it->second.data(), x.size() * sizeof(double)) != 0) return false;
  }
  return true;
}

inline InterpRun run(const Compiled& c, uint32_t voices, uint32_t frames, uint32_t block, const ImageBuilder& b) {
  const Setup st(c, voices, 0);
  InterpRun r = runInterp(st, frames, block, 1024, b);
  REQUIRE_MESSAGE(r.error.empty(), c.name << ": " << r.error);
  CHECK(r.rtViolations == 0);
  return r;
}

// Every value check of T1.5-fix1's acceptance on the backend `b` builds.
inline void check(const ImageBuilder& b) {
  constexpr uint32_t kFrames = 4096;
  // C1: u·u == x − u·u with x = 4 ± 1e-4: u = √(x/2) = √2 ± 3.6e-5 (it was 1 on every frame).
  {
    const InterpRun r = run(probe(probes(), "SolveInplace"), 0, kFrames, 64, b);
    REQUIRE(r.streams.size() == 1);
    double worst = 0;
    for (double y : r.streams.begin()->second) worst = std::max(worst, std::fabs(y - std::sqrt(2.0)));
    MESSAGE("SolveInplace: max |y - sqrt(2)| = " << worst);
    CHECK(worst <= 3.6e-5);
  }
  // C1, two unknowns: a·2 == b, a + b == x → b = 2x/3 ∈ 8/3 ± 6.7e-5.
  {
    const InterpRun r = run(probe(probes(), "SolveUnk2"), 0, kFrames, 64, b);
    REQUIRE(r.streams.size() == 1);
    double worst = 0;
    for (double z : r.streams.begin()->second) worst = std::max(worst, std::fabs(z - 8.0 / 3.0));
    MESSAGE("SolveUnk2: max |z - 8/3| = " << worst);
    CHECK(worst <= 6.7e-5);
  }
  // C2: o1 = a, o2 = a · 0.5 (both peaked at 0.5).
  {
    const InterpRun r = run(probe(probes(), "VoiceOut"), 8, kFrames, 64, b);
    std::map<uint32_t, double> peak;
    for (const auto& [k, x] : r.streams) {
      if (k.kind != 1) continue;
      for (double v : x) peak[k.a] = std::max(peak[k.a], std::fabs(v));
    }
    REQUIRE(peak.size() == 2);
    MESSAGE("VoiceOut: peaks o1 " << peak[0] << ", o2 " << peak[1]);
    CHECK(peak[0] == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(peak[1] == doctest::Approx(0.5).epsilon(1e-4));
  }
  // H2: a stereo feedback delay builds; both channels carry their own signal.
  {
    const InterpRun r = run(probe(probes(), "StereoFb"), 0, kFrames, 64, b);
    REQUIRE(r.streams.size() == 2);
    std::vector<double> rms;
    for (const auto& [k, x] : r.streams) {
      double s = 0;
      for (double v : x) s += v * v;
      rms.push_back(std::sqrt(s / static_cast<double>(x.size())));
    }
    CHECK(rms[0] > 0.01);
    CHECK(rms[1] > 0.01);
    CHECK(rms[0] != rms[1]);
  }
  // M10: the 20.75 ms loop gives the same bits at blocks 1, 64 and 1024.
  {
    const Compiled& c = probe(probes(), "DelayLb");
    const InterpRun r1 = run(c, 0, kFrames, 1, b);
    CHECK(bitEqual(r1.streams, run(c, 0, kFrames, 64, b).streams));
    CHECK(bitEqual(r1.streams, run(c, 0, kFrames, 1024, b).streams));
  }
}

}  // namespace liveness_probes
