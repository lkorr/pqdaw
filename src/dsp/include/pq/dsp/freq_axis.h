#pragma once

// The frequency axis every frequency display draws on (VZ.2 X1): Logarithmic, Mel or Linear between fMin and fMax, as a
// 0 … 1 axis fraction. Ported from PQ Meter (pq meter/src/dsp/freq_axis.h, 1.0.21), shared by the Spectrum (analysis
// points and display) and the Spectrogram (rows), so the two put 1 kHz in the same place for the same setting.
//
//   Log      log2(f / fMin) / log2(fMax / fMin); with fMin < 20 Hz the HYBRID axis (8f6b6ef): linear from fMin up to a
//            corner at 40 − fMin Hz, log above, slope-continuous at the corner, so the axis is continuous in fMin (it
//            used to switch at exactly 0 Hz: one notch up the slider threw the picture a quarter of the panel)
//   Mel      O'Shaughnessy / HTK mel: 2595 · log10(1 + f / 700)
//   Linear   (f − fMin) / (fMax − fMin)
//
// FreqAxis holds one axis with the hybrid's shape precomputed: unit() / hz() cost one log2 / exp2 (Log), one log10 /
// pow (Mel) or nothing (Linear). freqGridLines gives the rules (decade ladder on Log / Mel, even round steps on Linear:
// 2 kHz on a 20 kHz span), highest priority first.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace pq::dsp {

// APPEND-ONLY: instance data stores the index.
enum class FreqScale : uint8_t { Log, Mel, Linear };
inline constexpr int kFreqScaleCount = 3;
inline constexpr const char* kFreqScaleNames[] = {"Logarithmic", "Mel", "Linear"};
inline constexpr const char* kFreqScaleShort[] = {"LOG", "MEL", "LIN"};
inline FreqScale freqScaleOf(int i) noexcept { return i >= 0 && i < kFreqScaleCount ? static_cast<FreqScale>(i) : FreqScale::Log; }

inline float hzToMel(float f) noexcept { return 2595.0f * std::log10(1.0f + f / 700.0f); }
inline float melToHz(float m) noexcept { return 700.0f * (std::pow(10.0f, m / 2595.0f) - 1.0f); }

inline constexpr float kFreqAxisCornerHz = 40.0f;
inline constexpr float kFreqAxisHybridBelowHz = 20.0f;
inline bool freqAxisHybrid(float fMin) noexcept { return fMin < kFreqAxisHybridBelowHz; }

// The hybrid log axis' shape: corner Hz, octaves in the log zone, the axis fraction A the linear zone takes and its
// slope (fraction per Hz). Slope continuity at the corner, A / (fc − fMin) = (1 − A) / (fc · L · ln 2), gives
// A = k / (1 + k) with k = (fc − fMin) / (fc · L · ln 2).
struct FreqHybrid {
  float fc, L, A, linSlope;
};
inline FreqHybrid freqHybrid(float fMin, float fMax) noexcept {
  FreqHybrid h;
  h.fc = kFreqAxisCornerHz - std::max(fMin, 0.0f);
  h.L = std::log2(fMax / h.fc);
  const float d = h.fc * h.L * 0.6931472f;
  const float k = (h.fc - fMin) / d;
  h.A = k / (1.0f + k);
  h.linSlope = 1.0f / ((1.0f + k) * d);
  return h;
}

// One axis: [fMin, fMax] under `scale`, the per-scale constants precomputed.
class FreqAxis {
public:
  FreqAxis() noexcept : FreqAxis(FreqScale::Log, 10.0f, 22000.0f) {}
  FreqAxis(FreqScale scale, float fMin, float fMax) noexcept : scale_(scale), fMin_(std::max(0.0f, fMin)), fMax_(fMax) {
    if (scale_ == FreqScale::Log && !freqAxisHybrid(fMin_)) fMin_ = std::max(fMin_, 1e-3f);
    fMax_ = std::max(fMax_, fMin_ + 1.0f);
    switch (scale_) {
      case FreqScale::Linear: invSpan_ = 1.0f / std::max(1e-6f, fMax_ - fMin_); break;
      case FreqScale::Mel:
        m0_ = hzToMel(fMin_);
        invSpan_ = 1.0f / std::max(1e-6f, hzToMel(fMax_) - m0_);
        break;
      case FreqScale::Log:
      default:
        hybrid_ = freqAxisHybrid(fMin_);
        if (hybrid_) h_ = freqHybrid(fMin_, fMax_);
        else invSpan_ = 1.0f / std::log2(fMax_ / fMin_);
        break;
    }
  }
  FreqScale scale() const noexcept { return scale_; }
  float fMin() const noexcept { return fMin_; }
  float fMax() const noexcept { return fMax_; }
  bool hybrid() const noexcept { return hybrid_; }
  const FreqHybrid& hybridShape() const noexcept { return h_; }
  bool operator==(const FreqAxis& o) const noexcept { return scale_ == o.scale_ && fMin_ == o.fMin_ && fMax_ == o.fMax_; }

  // The fraction of hz along the axis. Not clamped, except that the hybrid log axis holds hz inside [0, fMax].
  float unit(float hz) const noexcept {
    switch (scale_) {
      case FreqScale::Linear: return (hz - fMin_) * invSpan_;
      case FreqScale::Mel: return (hzToMel(std::max(hz, 0.0f)) - m0_) * invSpan_;
      case FreqScale::Log:
      default:
        if (hybrid_) {
          const float f = std::clamp(hz, 0.0f, fMax_);
          if (f <= h_.fc) return h_.linSlope * (f - fMin_);
          return h_.A + (1.0f - h_.A) * std::log2(f / h_.fc) / h_.L;
        }
        return std::log2(std::max(hz, 1e-6f) / fMin_) * invSpan_;
    }
  }
  // The frequency at fraction u (the hybrid clamps u to [0, 1]).
  float hz(float u) const noexcept {
    switch (scale_) {
      case FreqScale::Linear: return fMin_ + u * (fMax_ - fMin_);
      case FreqScale::Mel: return melToHz(m0_ + u / invSpan_);
      case FreqScale::Log:
      default:
        if (hybrid_) {
          const float t = std::clamp(u, 0.0f, 1.0f);
          if (t <= h_.A) return fMin_ + t / h_.linSlope;
          return h_.fc * std::exp2((t - h_.A) / (1.0f - h_.A) * h_.L);
        }
        return fMin_ * std::exp2(u / invSpan_);
    }
  }

private:
  FreqScale scale_;
  float fMin_, fMax_;
  float invSpan_ = 1.0f, m0_ = 0.0f;
  bool hybrid_ = false;
  FreqHybrid h_{};
};

// Grid rules, highest priority first (a caller that drops colliding labels keeps the landmarks). Log and Mel: the
// decade ladder; Linear: even round steps (at most 10 across the span), every other step first. Rules outside
// (fMin · 1.02, fMax · 0.98), and on fMin itself, are left out. Returns the count written (≤ maxN).
struct FreqGridLine {
  float hz;
  char label[8];
};
inline int freqGridLines(FreqScale scale, float fMin, float fMax, FreqGridLine* out, int maxN) noexcept {
  int n = 0;
  auto add = [&](float hz) {
    if (n >= maxN || hz <= fMin || hz < fMin * 1.02f || hz > fMax * 0.98f) return;
    out[n].hz = hz;
    if (hz >= 1000.0f) std::snprintf(out[n].label, sizeof(out[n].label), "%gk", static_cast<double>(hz / 1000.0f));
    else std::snprintf(out[n].label, sizeof(out[n].label), "%g", static_cast<double>(hz));
    ++n;
  };
  if (scale == FreqScale::Linear) {
    const float span = std::max(1.0f, fMax - fMin);
    constexpr float kSteps[] = {100, 200, 500, 1000, 2000, 5000, 10000};
    float step = kSteps[6];
    for (const float s : kSteps)
      if (span / s <= 10.0f) {
        step = s;
        break;
      }
    const float first = std::ceil(fMin / step) * step;
    for (int pass = 0; pass < 2; ++pass)
      for (int k = pass; first + static_cast<float>(k) * step <= fMax; k += 2) add(first + static_cast<float>(k) * step);
    return n;
  }
  constexpr float kLadder[] = {20, 100, 1000, 10000, 20000, 50, 200, 500, 2000, 5000, 10, 30, 300, 3000, 15000};
  for (const float hz : kLadder) add(hz);
  return n;
}
inline int freqGridLines(const FreqAxis& a, FreqGridLine* out, int maxN) noexcept {
  return freqGridLines(a.scale(), a.fMin(), a.fMax(), out, maxN);
}

}  // namespace pq::dsp
