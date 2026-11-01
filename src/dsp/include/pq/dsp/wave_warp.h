#pragma once

// The wavetable module's warp / fold / sync formulas (TMM.1; SPEC §15 2026-11-01 "meta modules", docs/plans/MM.md
// Decisions → DSP). ONE formula set, shared by the frame-table builder (which bakes static values into the table,
// evaluating these templates in double and on 2nd-order jets for the bake's discontinuity corrections), by the
// live audio-rate path (TMM.8, float / SIMD instantiations) and by the tests.
//
// Order of application for an input phase φ ∈ [0, 1) and the frame's cycle x(·) (period 1):
//   ψ = syncPhase(φ, sync);  t = warpPhase(mode, ψ, warp);  v = (1 − t.mixB)·x(t.a) + t.mixB·x(t.b);
//   out = foldValue(v, fold)
// Every control's 0 is the identity (exactly, in any precision): warp 0 → t.a = ψ, t.mixB = 0; fold 0 → v; sync 0
// → φ. The ranges keep the read phase's slope ≤ 16 per control (sync ≤ 16×, pulse width ≤ 16×, bend ≤ 4×), which is
// what the builder's 16× bake resolution is sized for (combinations multiply: frameBakeResolution).
//
// T is the phase / value type (double, float, the builder's jets); the control amounts are plain scalars (they are
// constant over a bake and per block on the live path). Formulas only use + − × ÷, comparisons, floor and sin, found
// by argument-dependent lookup so a jet type can supply them.
//
//   syncPhase   ψ = frac(φ·s), s = 1 + 15·sync, sync ∈ [0, 1]: hard sync, the cycle read s times per period (the last
//               read is cut short when s is not an integer: a jump at φ = 0, which the bake corrects analytically).
//   warpPhase   warp ∈ [−1, 1]:
//               Bend        a = ψ / (c + (1 − c)·ψ), c = 2^(−2·warp) ∈ [¼, 4]: a smooth (rational) asymmetric curve,
//                           slope 1/c at 0 and c at 1; warp > 0 hurries the first part of the cycle.
//               Mirror      a = ψ, b = the phase folded about ½ (warp > 0: ½ − |ψ − ½|, reading [0, ½] forth and
//                           back; warp < 0: ½ + |ψ − ½|, the second half), mixB = |warp|. Continuous in ψ and in
//                           warp; |warp| = 1 is the fully mirrored cycle. (A phase-only mirror that is continuous
//                           and reaches the identity at 0 cannot have a bounded slope, so Mirror crossfades two reads.)
//               PulseWidth  the half-cycles squeezed to w and 1 − w: a = ψ·½/w below w, ½ + (ψ − w)·½/(1 − w) above;
//                           w = ½·(1 − (15/16)·warp) ∈ [1/32, 31/32]. Continuous.
//   foldValue   fold ∈ [0, 1], θ = 4π·fold: v → sin(θ·v)/sin(θ) while θ < π/2 (a soft saturation that keeps ±1 at ±1
//               and tends to v as θ → 0), sin(θ·v) above (folding: up to 8 folds of a full-scale input at fold = 1).

#include <cmath>
#include <cstdint>
#include <numbers>

namespace pq::dsp {

enum class WarpMode : uint8_t { Bend, Mirror, PulseWidth };
inline constexpr uint32_t kWarpModeCount = 3;

inline constexpr double kSyncMaxRatio = 16.0;      // s at sync = 1
inline constexpr double kBendMaxSlope = 4.0;       // c at warp = −1 (1/c at +1)
inline constexpr double kPulseMinWidth = 1.0 / 32.0;
inline constexpr double kFoldMaxTheta = 4.0 * std::numbers::pi;

template <class T>
struct WarpTaps {
  T a;          // first read phase
  T b;          // second read phase (Mirror only)
  double mixB;  // weight of the second read (0 except Mirror)
};

// ψ = frac(φ·(1 + 15·sync)). φ in [0, 1). RT-safe for arithmetic T.
template <class T, class A>
inline T syncPhase(T phase, A sync) noexcept {
  using std::floor;
  if (sync == A(0)) return phase;
  const T s = phase * T(1.0 + (kSyncMaxRatio - 1.0) * static_cast<double>(sync));
  return s - floor(s);
}

// The bend curve's c for an amount (computed once per block / bake).
inline double bendCurve(double warp) noexcept { return std::exp2(-2.0 * warp); }
// Pulse width w for an amount.
inline double pulseWidth(double warp) noexcept { return 0.5 * (1.0 - (1.0 - 2.0 * kPulseMinWidth) * warp); }

template <class T, class A>
inline WarpTaps<T> warpPhase(WarpMode mode, T phase, A warp) noexcept {
  const double w = static_cast<double>(warp);
  if (w == 0.0) return {phase, phase, 0.0};
  switch (mode) {
    case WarpMode::Bend: {
      const double c = bendCurve(w);
      return {phase / (T(c) + T(1.0 - c) * phase), phase, 0.0};
    }
    case WarpMode::Mirror: {
      const T b = phase < T(0.5) ? (w > 0 ? phase : T(1.0) - phase) : (w > 0 ? T(1.0) - phase : phase);
      return {phase, b, w > 0 ? w : -w};
    }
    case WarpMode::PulseWidth: {
      const double pw = pulseWidth(w);
      const T a = phase < T(pw) ? phase * T(0.5 / pw) : T(0.5) + (phase - T(pw)) * T(0.5 / (1.0 - pw));
      return {a, phase, 0.0};
    }
  }
  return {phase, phase, 0.0};
}

// The fold's θ and normalisation for an amount (once per block / bake): out = sin(θ·v)·norm.
struct FoldShape {
  double theta = 0;
  double norm = 1;
};
inline FoldShape foldShape(double fold) noexcept {
  const double theta = kFoldMaxTheta * fold;
  return {theta, theta < 0.5 * std::numbers::pi ? 1.0 / std::sin(theta) : 1.0};
}

template <class T, class A>
inline T foldValue(T v, A fold) noexcept {
  using std::sin;
  if (fold == A(0)) return v;
  const FoldShape f = foldShape(static_cast<double>(fold));
  return sin(v * T(f.theta)) * T(f.norm);
}

// The phases φ ∈ [0, 1) where the composed read (sync, then warp) has a kink or a jump: every sync sub-period start
// k/s (k = 0 … ⌈s⌉ − 1; φ = 0 is always one) and, inside each sub-period, the warp's kink (Mirror ½, PulseWidth w).
// Writes at most `cap` ascending values; returns the count. Used by the builder's bake (discontinuity corrections).
inline uint32_t warpBreakpoints(WarpMode mode, double warp, double sync, double* out, uint32_t cap) noexcept {
  const double s = 1.0 + (kSyncMaxRatio - 1.0) * sync;
  double kink = -1.0;
  if (warp != 0.0) {
    if (mode == WarpMode::Mirror) kink = 0.5;
    if (mode == WarpMode::PulseWidth) kink = pulseWidth(warp);
  }
  uint32_t n = 0;
  for (uint32_t k = 0; n < cap; ++k) {
    const double start = static_cast<double>(k) / s;
    if (start >= 1.0) break;
    out[n++] = start;
    if (kink > 0.0 && n < cap) {
      const double p = (static_cast<double>(k) + kink) / s;
      if (p < 1.0) out[n++] = p;
    }
  }
  return n;
}

}  // namespace pq::dsp
