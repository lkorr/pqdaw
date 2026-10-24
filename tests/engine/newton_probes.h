#pragma once

// Tests only (T1b.1-fix1): Newton probes from the M1 review (docs/reviews/M1/probes/solve_scale.pqp) and the
// fallback paths of pq/engine/newton.h, checked by value on one backend: the interpreter (test_interpreter.cpp) or
// the JIT (test_jit.cpp, which also requires bit-identity with the interpreter).
//   SolveScale    log(u) == log(0.002) from 0.001: tangents (AD). With the old step h = 2^-8 · max(|u|, 1) the central
//                 difference left log's domain and the solve stayed at 0.001 forever.
//   SolveScaleFd  the same with a shaper on the body's path (no tangent rule): finite differences with the relative
//                 step 2^-8 · max(|u|, 1e-6).
//   SolveSqrt     sqrt(u) == 0.5 from u = 0: the tangent Jacobian is infinite at 0 (d sqrt = 1 / (2·0)); the lane falls
//                 back to finite differences, whose u − h side is outside sqrt's domain (one-sided difference). Without
//                 the fallback the update is −0.5 / inf = −0 and u stays 0.
//   SolveTiny     tanh(i · 1e5) == 0.5 from i = 1e-6 (scale 1e-5): AD gets the exact slope; the old absolute step (2^-8)
//                 saturated tanh on both sides (J ≈ 256 instead of ≈ 1e5) and Newton oscillated.

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "interp_harness.h"

namespace newton_probes {

using namespace interp_test;

inline const char* kText = R"(
patch SolveScale {
  out y : audio
  target = 0.002 + 0 * sine(phasor(1Hz))
  init = 0.001 + 0 * sine(phasor(1Hz))
  solve(iterations: 8) {
    unknown u = init
    log(u) == log(target)
  }
  y = u
}
patch SolveScaleFd {
  out y : audio
  target = 0.002 + 0 * sine(phasor(1Hz))
  init = 0.001 + 0 * sine(phasor(1Hz))
  solve(iterations: 8) {
    unknown u = init
    log(u) + shaper(u) * 0 == log(target)
  }
  y = u
}
patch SolveSqrt {
  out y : audio
  init = 0 * sine(phasor(1Hz))
  solve(iterations: 12) {
    unknown u = init
    sqrt(u) == 0.5
  }
  y = u
}
patch SolveTiny {
  out y : audio
  init = 0.000001 + 0 * sine(phasor(1Hz))
  solve(iterations: 8) {
    unknown i = init
    tanh(i * 100000) == 0.5
  }
  y = i * 100000
}
)";

inline const std::vector<Compiled>& probes() {
  static const std::vector<Compiled> all = [] {
    std::vector<Compiled> v;
    const std::string text = kText;
    for (const char* name : {"SolveScale", "SolveScaleFd", "SolveSqrt", "SolveTiny"}) {
      // compileText compiles the first patch of a text: one patch at a time
      const size_t at = text.find(std::string("patch ") + name + " {");
      REQUIRE(at != std::string::npos);
      const size_t end = text.find("\npatch ", at + 1);
      v.push_back(compileText(text.substr(at, end == std::string::npos ? std::string::npos : end - at)));
      v.back().name = name;
    }
    return v;
  }();
  return all;
}

inline const Compiled& probe(const std::string& name) {
  for (const Compiled& c : probes())
    if (c.name == name) return c;
  FAIL("newton probe " << name << " did not compile");
  return probes().front();
}

inline InterpRun run(const Compiled& c, uint32_t frames, uint32_t block, const ImageBuilder& b) {
  const Setup st(c, 0, 0);
  InterpRun r = runInterp(st, frames, block, 1024, b);
  REQUIRE_MESSAGE(r.error.empty(), c.name << ": " << r.error);
  CHECK(r.rtViolations == 0);
  return r;
}

// Max |y − want| / |want| over every frame of the single output.
inline double maxRelErr(const InterpRun& r, double want) {
  double e = 0;
  for (const auto& [k, x] : r.streams)
    for (double y : x) e = std::max(e, std::isfinite(y) ? std::fabs(y - want) / std::fabs(want) : INFINITY);
  return e;
}

// Every probe converges on the backend `b` builds.
inline void check(const ImageBuilder& b = {}) {
  struct Want {
    const char* name;
    double value;
  };
  for (const Want& w : {Want{"SolveScale", 0.002}, Want{"SolveScaleFd", 0.002}, Want{"SolveSqrt", 0.25},
                        Want{"SolveTiny", std::atanh(0.5)}}) {
    const InterpRun r = run(probe(w.name), 512, 64, b);
    const double e = maxRelErr(r, w.value);
    MESSAGE(std::string(w.name) << ": max relative error " << e);
    CHECK_MESSAGE(e <= 1e-6, w.name);
  }
}

}  // namespace newton_probes
