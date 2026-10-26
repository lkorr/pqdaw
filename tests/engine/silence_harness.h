#pragma once

// Tests only (T1.x-silence): drives a ProgramImage's global stages block by block over a stimulus with silences in it
// (the sleep plan's sources are the root inputs), and records its outputs, its sleep statistics and its silence
// reports. Voice stages are not driven (no voice table): their reducers read 0.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"

namespace silence_test {

using namespace pq::graph;
namespace eng = pq::engine;

// The value of global root input slot `slot` (its ordinal and channel in `io`) at frame t; bools as masks.
using Input = std::function<float(const eng::IoSlot& slot, bool isBool, bool isParam, uint64_t t)>;
// Param values before the block starting at t (may change them).
using ParamsAt = std::function<void(uint64_t t, std::vector<float>& params)>;

struct Run {
  std::vector<std::vector<float>> out;  // per global out slot, every frame
  eng::SleepStats stats;
  uint64_t rtViolations = 0;
  std::vector<uint8_t> tailsDone, dormant;  // per block
  std::vector<uint64_t> blockStart;
  std::string error;
};

inline Run run(const interp_test::Compiled& c, eng::SleepMode mode, const Input& input, uint64_t frames,
               const std::vector<uint32_t>& blocks, const interp_test::ImageBuilder& build = {},
               const ParamsAt& paramsAt = {}, uint32_t maxBlock = 1024) {
  Run res;
  eng::InterpretOptions io;
  io.maxBlockFrames = maxBlock;
  io.maxVoiceGroups = 1;
  io.resources = &interp_test::tables().resources;
  io.sleep = mode;
  auto built = build ? build(c.sched, io) : eng::buildInterpreted(c.sched, io);
  if (!built.ok()) {
    res.error = built.error().message;
    return res;
  }
  eng::ProgramImage& img = **built;
  const eng::ImageInterface& face = img.io();
  const interp_test::Setup setup(c, 0, 0);
  std::vector<std::vector<float>> gIn(face.globalIn.size(), std::vector<float>(maxBlock)),
      gOut(face.globalOut.size(), std::vector<float>(maxBlock));
  std::vector<const float*> gInP;
  std::vector<float*> gOutP;
  for (auto& b : gIn) gInP.push_back(b.data());
  for (auto& b : gOut) gOutP.push_back(b.data());
  std::vector<uint8_t> outSilent(face.globalOut.size());
  std::vector<float> params(face.params.size(), std::numeric_limits<float>::quiet_NaN());
  res.out.assign(face.globalOut.size(), {});
  std::vector<std::vector<float>> trackIn(2, std::vector<float>(maxBlock, 0.0f));
  std::vector<const float*> trackP{trackIn[0].data(), trackIn[1].data()};
  size_t bi = 0;
  for (uint64_t t0 = 0; t0 < frames;) {
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(blocks[bi++ % blocks.size()], frames - t0));
    for (uint32_t s = 0; s < face.globalIn.size(); ++s) {
      const eng::IoSlot& sl = face.globalIn[s];
      const bool b = setup.inBool.count(sl.ordinal) && setup.inBool.at(sl.ordinal);
      const bool p = setup.inParam.count(sl.ordinal) && setup.inParam.at(sl.ordinal);
      for (uint32_t f = 0; f < n; ++f) gIn[s][f] = input(sl, b, p, t0 + f);
    }
    if (paramsAt) paramsAt(t0, params);
    eng::ProcessContext ctx;
    ctx.frames = n;
    ctx.sampleRate = c.sched.sampleRate;
    ctx.sampleTime = static_cast<int64_t>(t0);
    ctx.transport = interp_test::Setup::transport();
    ctx.in = gInP.data();
    ctx.numIn = static_cast<uint32_t>(gInP.size());
    ctx.out = gOutP.data();
    ctx.numOut = static_cast<uint32_t>(gOutP.size());
    ctx.trackIn = trackP.data();
    ctx.numTrackIn = 2;
    ctx.sidechainIn = trackP.data();
    ctx.numSidechainIn = 2;
    ctx.params = params.data();
    ctx.numParams = static_cast<uint32_t>(params.size());
    ctx.outSilent = outSilent.data();
    res.rtViolations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      img.process(ctx);
    });
    for (uint32_t s = 0; s < face.globalOut.size(); ++s) {
      bool zero = true;
      for (uint32_t f = 0; f < n; ++f) {
        res.out[s].push_back(gOut[s][f]);
        zero = zero && (std::bit_cast<uint32_t>(gOut[s][f]) & 0x7FFFFFFFu) == 0;
      }
      if (zero != (outSilent[s] != 0)) res.error = "outSilent disagrees with the output at t = " + std::to_string(t0);
    }
    res.tailsDone.push_back(ctx.tailsDone ? 1 : 0);
    res.dormant.push_back(ctx.dormant ? 1 : 0);
    res.blockStart.push_back(t0);
    t0 += n;
  }
  if (const eng::SleepStats* st = img.sleepStats()) res.stats = *st;
  return res;
}

// Equal as floats (±0 equal: a zero fill writes +0 where running would have computed −0).
inline bool sameValues(const Run& a, const Run& b, std::string& where) {
  if (a.out.size() != b.out.size()) {
    where = "different slot counts";
    return false;
  }
  for (size_t s = 0; s < a.out.size(); ++s) {
    if (a.out[s].size() != b.out[s].size()) {
      where = "different lengths";
      return false;
    }
    for (size_t t = 0; t < a.out[s].size(); ++t) {
      const float x = a.out[s][t], y = b.out[s][t];
      if (x == y || (x != x && y != y)) continue;
      where = "slot " + std::to_string(s) + " t = " + std::to_string(t) + ": " + std::to_string(x) + " vs " + std::to_string(y);
      return false;
    }
  }
  return true;
}

// Bit for bit (interpreter vs JIT: both sleep identically).
inline bool sameBits(const Run& a, const Run& b, std::string& where) {
  if (a.out.size() != b.out.size()) return false;
  for (size_t s = 0; s < a.out.size(); ++s) {
    if (a.out[s].size() != b.out[s].size()) return false;
    for (size_t t = 0; t < a.out[s].size(); ++t)
      if (std::bit_cast<uint32_t>(a.out[s][t]) != std::bit_cast<uint32_t>(b.out[s][t])) {
        where = "slot " + std::to_string(s) + " t = " + std::to_string(t);
        return false;
      }
  }
  return true;
}

inline double maxAbsDiff(const Run& a, const Run& b) {
  double m = 0.0;
  for (size_t s = 0; s < a.out.size() && s < b.out.size(); ++s)
    for (size_t t = 0; t < a.out[s].size() && t < b.out[s].size(); ++t)
      m = std::max(m, static_cast<double>(std::fabs(a.out[s][t] - b.out[s][t])));
  return m;
}

// The stimulus of the corpus runs: sound in [0, 2000), silence, sound again from `resume` (mid-block) for 4000
// frames, silence after. Bools: toggling while sounding, false in silence; param-rate inputs: constants.
inline Input burstInput(uint64_t resume) {
  return [resume](const eng::IoSlot& sl, bool isBool, bool isParam, uint64_t t) -> float {
    if (isParam) return isBool ? pq::dsp::boolMask(true) : 0.3f + 0.1f * static_cast<float>(sl.ordinal);
    const bool sounding = t < 2000 || (t >= resume && t < resume + 4000);
    if (!sounding) return 0.0f;
    if (isBool) return pq::dsp::boolMask(((t / (17 + sl.ordinal)) % 3) == 0);
    return static_cast<float>(0.6 * std::sin(0.031 * static_cast<double>(t) * (1 + sl.ordinal) + sl.channel));
  };
}

}  // namespace silence_test
