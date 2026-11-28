#pragma once

// Tests only (FD.1): tests/engine/patches/delay_bounds.pqp on one backend — the interpreter (test_interpreter.cpp) or
// the JIT (test_jit.cpp). Each patch is a feedback loop whose sub-block comes from the interval analysis of a delay
// time and the cut choice (pq/graph/schedule.h "Loop"). A bound that is too long makes a tap read a frame its
// sub-block has not written yet, and then the output depends on the block size: at block 1 every loop runs one frame
// at a time (the per-sample schedule), so the same bits at blocks 1, 37, 64 and 1024 is the comparison with it.
// Both param sets: the defaults (every time at its range minimum: the tap exactly as old as the bound) and set 1.
// FadeTap (delay_fade, the Delay Tap's glided read) runs with MOVING params — time and mode step every 2048 frames,
// so the read head glides and crossfades inside sub-blocks and the param ramps run — against the per-sample schedule
// of the same patch (ScheduleOptions::maxLoopSubBlock = 1) at the same block size.

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

inline Compiled compileWith(const std::string& text, ScheduleOptions so) {
  ParseResult pr = parseDetailed(text, "test.pqp");
  REQUIRE(pr.ok());
  PatchLibrary lib;
  lib.add(pr.file);
  InferOptions io;
  io.library = &lib;
  auto inf = infer(pr.file.patches[0], io);
  REQUIRE(inf.ok());
  OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = optimise(std::move(inf.value()), oo);
  REQUIRE(opt.ok());
  so.sampleRate = 48000.0;
  auto s = schedule(opt.value(), so);
  REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
  Compiled c;
  c.name = pr.file.patches[0].name();
  c.typed = std::move(opt.value());
  c.sched = std::move(s.value());
  return c;
}

inline const char* const kFadeTap = R"(patch FadeTap {
  param time = 3.01ms [3.01ms..50ms]
  param sync = 0.5 [0..1]
  param mode = 0 [0..1]
  param glide = 0 [0..1]
  out y : audio
  src = noise() * 0.1
  synced = min(max(sync * 20ms, 5ms), 50ms)
  d = delay_fade(src + 0.6 * d, lerp(time, synced, mode), 100ms, 30ms, glide)
  y = d
}
)";

// FadeTap, sub-blocked against per sample, params stepping every 2048 frames (a multiple of every block size used, so
// each frame reads the same params whatever the block).
inline void checkFade(const ImageBuilder& b) {
  constexpr uint32_t kFrames = 2048 * 12;
  const Compiled sub = compileWith(kFadeTap, {});
  ScheduleOptions per;
  per.maxLoopSubBlock = 1;
  const Compiled ref = compileWith(kFadeTap, per);
  CHECK(loopSubs(sub) == std::vector<uint32_t>{143});  // 3.01 ms = 144.48 samples (lerp's allowance is 0.002): cubic 144 − 1
  CHECK(loopSubs(ref) == std::vector<uint32_t>{1});
  auto run = [&](const Compiled& c, uint32_t block) {
    Setup st(c, 0, 0);
    st.paramAt = [&c](uint32_t, const ParamSlot& info, uint64_t t0, float base) {
      const std::string& name = c.typed.patch.nodeName(info.node);
      const uint64_t seg = t0 / 2048;
      // time: the floor (a fractional delay: the newest tap, exactly a sub-block old, has weight), a jump up (more than an octave: a crossfade when glide is 1), small moves (glides), back down.
      static constexpr float kTimes[] = {0.00301f, 0.00301f, 0.0304f, 0.0333f, 0.00407f, 0.00353f, 0.00301f, 0.0123f, 0.00301f, 0.05f, 0.00301f, 0.00301f};
      if (name == "time") return kTimes[seg % 12];
      if (name == "mode") return seg == 5 || seg == 6 ? 1.0f : seg == 7 ? 0.5f : 0.0f;
      if (name == "sync") return seg % 2 ? 0.2f : 0.9f;
      if (name == "glide") return seg >= 6 ? 1.0f : 0.0f;
      return base;
    };
    InterpRun r = runInterp(st, kFrames, block, 1024, b);
    REQUIRE_MESSAGE(r.error.empty(), r.error);
    CHECK(r.rtViolations == 0);
    return r;
  };
  for (uint32_t block : {64u, 1024u, 256u}) {
    INFO("FadeTap block " << block);
    const InterpRun want = run(ref, block), got = run(sub, block);
    double energy = 0;
    for (const auto& [k, x] : want.streams)
      for (double v : x) energy += v * v;
    CHECK(energy > 1e-3);
    CHECK(liveness_probes::bitEqual(want.streams, got.streams));
  }
}

inline void check(const ImageBuilder& b) {
  checkFade(b);
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
