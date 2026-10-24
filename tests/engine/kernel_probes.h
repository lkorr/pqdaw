#pragma once

// Tests only (T1.6-fix1): the M1 review's kernel edge-case probes (tests/engine/patches/kernel_edges.pqp, from
// docs/reviews/M1/probes/) checked by value on one backend: the interpreter (test_interpreter.cpp) or the JIT
// (test_jit.cpp). Each one rendered wrong before the fix; DelayBig compiled and then wrote out of bounds.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "interp_harness.h"

namespace kernel_probes {

using namespace interp_test;

inline const std::vector<Compiled>& probes() {
  static const std::vector<Compiled> all = [] {
    std::vector<std::string> skipped;
    auto v = compileFile(patchesDir() / "kernel_edges.pqp", 48000.0, skipped);
    // H3: DelayBig is the only patch that does not compile, rejected with the delay-line limit.
    REQUIRE(skipped.size() == 1);
    CHECK(skipped[0].find("DelayBig") != std::string::npos);
    CHECK(skipped[0].find("maximum delay") != std::string::npos);
    CHECK(skipped[0].find("must be between 0 and 268435446 samples") != std::string::npos);
    return v;
  }();
  return all;
}

inline const Compiled& probe(const std::string& name) {
  for (const Compiled& c : probes())
    if (c.name == name) return c;
  FAIL("probe patch " << name << " did not compile");
  return probes().front();
}

// Global out slot `slot` (declaration order) of a run.
inline const std::vector<double>& out(const InterpRun& r, uint32_t slot) {
  for (const auto& [k, x] : r.streams)
    if (k.kind == 0 && k.a == slot) return x;
  FAIL("no global out slot " << slot);
  return r.streams.begin()->second;
}

inline InterpRun run(const Compiled& c, uint32_t frames, uint32_t block, const ImageBuilder& b) {
  const Setup st(c, 0, 0);
  InterpRun r = runInterp(st, frames, block, 1024, b);
  REQUIRE_MESSAGE(r.error.empty(), c.name << ": " << r.error);
  CHECK(r.rtViolations == 0);
  return r;
}

inline uint32_t countNot(const std::vector<double>& x, double v) {
  uint32_t n = 0;
  for (double y : x) n += y != v;
  return n;
}

// Every value check of T1.6-fix1's acceptance on the backend `b` builds (blocks of 64 and 37 frames).
inline void check(const ImageBuilder& b) {
  constexpr uint32_t kFrames = 4096;
  for (uint32_t block : {64u, 37u}) {
    INFO("block " << block);
    // M11a: rate −1e−9 and −1e−6 from 0.5 keep playing (the first one stopped a sample after every reset).
    {
      const InterpRun r = run(probe("SrTiny"), kFrames, block, b);
      REQUIRE(r.streams.size() == 2);
      CHECK(countNot(out(r, 0), 1.0) == 0);
      CHECK(countNot(out(r, 1), 1.0) == 0);
    }
    // M11b: rate 2 and rate 1 on a one-sample loop keep looping.
    {
      const InterpRun r = run(probe("LoopFast"), kFrames, block, b);
      CHECK(countNot(out(r, 0), 1.0) == 0);
      CHECK(countNot(out(r, 1), 1.0) == 0);
    }
    // L3: mod(−1e−9, 1) is 0, never 1.
    CHECK(countNot(out(run(probe("ModTiny"), kFrames, block, b), 0), 0.0) == 0);
    // M13: timer(trig, NaN) never fires.
    CHECK(countNot(out(run(probe("TimerNan"), kFrames, block, b), 0), 0.0) == 0);
    // L3: NaN hz is 0 Hz: phase and sine stay 0 (they alternated at Nyquist).
    {
      const InterpRun r = run(probe("PhNan"), kFrames, block, b);
      CHECK(countNot(out(r, 0), 0.0) == 0);
      CHECK(countNot(out(r, 1), 0.0) == 0);
    }
    // M5: fast_sin(1e20) is finite and in [−1, 1]; times 0 it is 0 (it was inf, and inf · 0 NaN).
    {
      const InterpRun r = run(probe("FastSinBig"), kFrames, block, b);
      CHECK(countNot(out(r, 0), 1.0) == 0);
      CHECK(countNot(out(r, 1), 0.0) == 0);
    }
  }
}

}  // namespace kernel_probes
