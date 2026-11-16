#pragma once

// The waterfall's analysis (VZ.7; docs/plans/VZ.md "VZ.7"), ported from PQ Meter (pq meter/src/modules/waterfall.cpp:
// processAudio, frame, rebuildHop, maxSpanSec). The level curve is VZ.2's spectrum analysis (pq/dsp/spectrum.h: the
// shared frequency axis, the long window below 500 Hz, the tilt, attack / release) on the channel asked for (mono
// (L + R) / 2, L, R, mid, side); every `analysisHop` (~90 Hz, the Spectrum's cadence) it advances, and every slice pitch
// — derived from the span so the history holds about kTargetSlices slices (PQ: one Span control instead of two that
// fought) — the curve is committed to a ring of kMaxSlices. Not for the audio thread (the analysis worker); push()
// never allocates.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pq/dsp/spectrum.h"

namespace pq::dsp {

enum class WaterfallChannel : uint8_t { Mono, Left, Right, Mid, Side };

struct WaterfallConfig {
  double sampleRate = 48000.0;
  uint32_t fftSize = 2048;
  bool multiRes = true;
  WaterfallChannel channel = WaterfallChannel::Mono;
  float slopeDbPerOct = 0.0f;  // PQ: Off / 3 / 4.5 / 6
  FreqScale scale = FreqScale::Log;
  float attackMs = 25.0f, releaseMs = 350.0f;
  float spanSec = 6.0f;
  bool operator==(const WaterfallConfig&) const = default;
};

class WaterfallAnalyzer {
public:
  static constexpr uint32_t kPoints = 512, kMaxSlices = 300, kTargetSlices = 260;
  static constexpr float kFMin = 20.0f, kFMax = 20000.0f;
  explicit WaterfallAnalyzer(const WaterfallConfig& c = {});
  ~WaterfallAnalyzer();
  // A new size, multi-res, scale, channel or rate restarts the analysis and clears the slices; ballistics, slope and span
  // apply going on (the span re-derives the pitch).
  void configure(const WaterfallConfig& c);
  const WaterfallConfig& config() const noexcept { return cfg_; }
  // The span the window can hold at the slice cap (the pitch tops out at the window): the span is clamped to it.
  static float maxSpanSec(double sampleRate, uint32_t fftSize) noexcept;
  uint32_t slicePitch() const noexcept { return pitch_; }  // samples
  uint32_t analysisHop() const noexcept { return aHop_; }

  void push(const float* l, const float* r, uint32_t n) noexcept;  // RT-safe
  uint64_t slices() const noexcept { return slices_; }             // committed so far (free-running)
  // Slice s (0 … slices − 1, the newest kMaxSlices held): kPoints dB, low frequency first; null when not held.
  const float* slice(uint64_t s) const noexcept;
  std::span<const float> live() const noexcept;  // the curve now (the leading edge)
  // Samples analysed since the last commit / the pitch (the sub-slice scroll, 0 … 1).
  float phase() const noexcept { return static_cast<float>(since_) / static_cast<float>(pitch_); }
  uint32_t generation() const noexcept { return gen_; }  // bumps at each restart
  bool atRest() const noexcept;                          // the curve under −139 dB and silent input

private:
  WaterfallConfig cfg_;
  std::unique_ptr<SpectrumAnalyzer> an_;
  std::vector<float> mono_, ring_;
  uint32_t pitch_ = 1024, aHop_ = 512, since_ = 0, gen_ = 0;
  uint64_t slices_ = 0, quiet_ = 0;
};

}  // namespace pq::dsp
