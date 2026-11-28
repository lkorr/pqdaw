#pragma once

// BR.J: the granulator's spawn law (pq/dsp/granular.h "One voice") for one voice from its trigger, as a pure function:
// each spawn's onset (a whole sample of the voice's clock), its length, and the slot it took or that it was skipped
// (no slot free at its grid cell's start). The `granular` kernel (kernels_granular.cpp) runs the same law inline, block
// by block; tests/dsp/test_granular.cpp's reference model spawns through this function and must match the kernel's
// output, so the three agree. The Sampler face's grainstream draws from it (bug 2026-11-27_21-16-56: the display had
// its own continuous-time copy of the law, which drifted from the kernel's whole-sample onsets and grid-cell slots).
//
// Onsets: t_0 = 0, t_{k+1} = ceil(t_k + max(interval_k, 1)), interval_k = sr / hz(t_k) (Classic), sr / density (Loop),
// sr / density · −ln(1 − 0.999999·u_k) (Cloud, u_k = grainUniform(seed, voiceSlot, k, 0)). A grain lasts
// llround(size · sr) samples (≥ 2); it takes the first of `grains` slots whose grain ended at or before
// floor(t_k / kGrainGrid) · kGrainGrid, else it is skipped (counted, its k used). Parameters are clamped as the kernel
// clamps its inputs.
//
// Not used on the audio thread (the kernel has its own copy); allocation-free and noexcept all the same.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "pq/dsp/granular.h"

namespace pq::dsp {

struct GrainSpawnLaw {
  GranularMode mode = GranularMode::Loop;
  double density = 30.0;  // Hz (Loop, Cloud)
  double size = 0.08;     // seconds
  uint32_t grains = 16;   // slots used (1 … kGrainsMax)
  double sr = 48000.0;    // the voice clock's rate
  uint32_t seed = 0;      // Cloud's onsets: the node's seed and the lane's voice slot
  uint32_t voiceSlot = 0;
};

struct GrainSpawn {
  int64_t start = 0;  // samples since the trigger
  int64_t len = 0;    // samples
  uint32_t k = 0;     // the grain number since the trigger (skipped spawns count)
  uint32_t slot = 0;  // the slot it took (kGrainsMax: skipped)
  bool skipped() const noexcept { return slot >= kGrainsMax; }
};

// Calls onSpawn(const GrainSpawn&) for every spawn with start < frames, in order; hzAt(int64_t t) is the voice's pitch
// at sample t (read for Classic only; the kernel's input as it sees it). Returns the number skipped.
template <class Hz, class OnSpawn>
uint32_t grainSchedule(const GrainSpawnLaw& law, int64_t frames, Hz&& hzAt, OnSpawn&& onSpawn) noexcept {
  const double sr = std::clamp(law.sr, 1000.0, 768000.0);
  const double density = std::clamp(law.density, 0.01, 4000.0);
  const uint32_t grains = std::clamp<uint32_t>(law.grains, 1u, kGrainsMax);
  const auto len = static_cast<int64_t>(std::clamp<int64_t>(std::llround(std::clamp(law.size, 0.001, 4.0) * sr), 2, int64_t{1} << 24));
  std::array<int64_t, kGrainsMax> ends{};  // per slot: its grain's end
  std::array<bool, kGrainsMax> used{};
  double next = 0.0;
  uint32_t skipped = 0;
  for (uint32_t k = 0;; ++k) {
    const auto ts = static_cast<int64_t>(std::ceil(next));
    if (ts >= frames) break;
    double interval = sr / density;
    if (law.mode == GranularMode::Classic) interval = sr / std::max(static_cast<double>(hzAt(ts)), 1.0);
    if (law.mode == GranularMode::Cloud)
      interval *= -std::log(1.0 - 0.999999 * static_cast<double>(grainUniform(law.seed, law.voiceSlot, k, 0)));
    next = static_cast<double>(ts) + std::max(interval, 1.0);
    const int64_t cell = ts - ts % static_cast<int64_t>(kGrainGrid);
    GrainSpawn g{ts, len, k, kGrainsMax};
    for (uint32_t j = 0; j < grains; ++j)
      if (!used[j] || ends[j] <= cell) {
        g.slot = j;
        break;
      }
    if (g.skipped()) {
      ++skipped;
    } else {
      used[g.slot] = true;
      ends[g.slot] = ts + len;
    }
    onSpawn(static_cast<const GrainSpawn&>(g));
  }
  return skipped;
}

}  // namespace pq::dsp
