#pragma once

// Tests only (FD.1): tests/engine/patches/delay_bounds.pqp on one backend — the interpreter (test_interpreter.cpp) or
// the JIT (test_jit.cpp). Each patch is a feedback loop whose sub-block comes from the interval analysis of a delay
// time and the cut choice (pq/graph/schedule.h "Loop"). A bound that is too long makes a tap read a frame its
// sub-block has not written yet, and then the output depends on the block size: at block 1 every loop runs one frame
// at a time (the per-sample schedule), so the same bits at blocks 1, 37, 64 and 1024 is the comparison with it.
// Both param sets: the defaults (every time at its range minimum: the tap exactly as old as the bound) and set 1.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "interp_harness.h"
#include "liveness_probes.h"

namespace delay_bounds_probes {

using namespace interp_test;

inline const std::vector<Compiled>& probes() {
  static const std::vector<Compiled> all = [] {
    std::vector<std::string> skipped;
    auto v = compileFile(patchesDir() / "delay_bounds.pqp", 48000.0, skipped);
    REQUIRE(skipped.empty());
    return v;
  }();
  return all;
}

inline std::vector<uint32_t> loopSubs(const Compiled& c) {  // ascending
  std::vector<uint32_t> subs;
  for (const Region& r : c.sched.regions)
    if (r.kind == RegionKind::Loop) subs.push_back(r.subBlock);
  std::sort(subs.begin(), subs.end());
  return subs;
}

inline InterpRun run(const Compiled& c, int paramSet, uint32_t frames, uint32_t block, const ImageBuilder& b) {
  const Setup st(c, 0, paramSet);
  InterpRun r = runInterp(st, frames, block, 1024, b);
  REQUIRE_MESSAGE(r.error.empty(), c.name << ": " << r.error);
  CHECK(r.rtViolations == 0);  // RtScope around every process()
  return r;
}

inline void check(const ImageBuilder& b) {
  constexpr uint32_t kFrames = 8192;
  // The schedules under test: sub-blocks from the shortest main tap (3.1 ms = 148.8 samples); the modulated 4 ms tap
  // (190.08, cubic ⌊d⌋ − 1); lerp's low end, 3 ms less its rounding allowance (143.998, cubic); the 2 ms ring, and the
  // allpass tap's own loop (48.504: ⌊d⌋); the 2.5 ms lane.
  const std::vector<std::pair<std::string, std::vector<uint32_t>>> want = {
      {"FdnPack8", {148}}, {"Fdn8", {148}}, {"ModTap", {189}}, {"LerpTap", {142}}, {"ShortLong", {48, 96}}, {"PerLane", {120}}};
  REQUIRE(probes().size() == want.size());
  for (const auto& [name, sub] : want) {
    const Compiled& c = liveness_probes::probe(probes(), name);
    INFO(name);
    CHECK(loopSubs(c) == sub);
    for (int paramSet : {0, 1}) {
      INFO("param set " << paramSet);
      const InterpRun r1 = run(c, paramSet, kFrames, 1, b);
      double energy = 0;
      for (const auto& [k, x] : r1.streams)
        for (double v : x) energy += v * v;
      CHECK(energy > 1e-3);  // it sounds (the loop feeds back)
      for (uint32_t block : {37u, 64u, 1024u}) {
        INFO("block " << block);
        CHECK(liveness_probes::bitEqual(r1.streams, run(c, paramSet, kFrames, block, b).streams));
      }
    }
  }
}

}  // namespace delay_bounds_probes
