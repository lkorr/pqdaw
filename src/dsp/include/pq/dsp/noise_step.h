#pragma once

// NZ.1: the `noise_step` atom's colour filter and constants (docs/plans/NZ.md; the atom: pq/dsp/atoms.h "Generators",
// the kernel: kernels_noise_step.cpp; the module around it: pq/meta/noise.h).
//
// COLOUR is a tilt filter run on the SEQUENCE of drawn values (once per step, never per sample), so in step-normalised
// frequency it is a fixed function of the colour c ∈ [−6, +6] dB/oct: at a step rate of 48 kHz its slope is c within
// ±0.418 dB over 20 Hz … 20 kHz (measured on the float coefficients for 481 colours: tools/fit/noise_colour.py), and at
// a slower step rate the same band scaled by the rate. −6 is brown (a leaky integrator: its lowest pole sits at
// ≈ 10 Hz · rate / 48 kHz), −3 pink, 0 white (the identity: no filter runs), +3 blue, +6 violet.
//   H(z) = g · Π_{k<4} (1 − z_k z⁻¹) / (1 − p_k z⁻¹)
// with four FIXED poles p_k (a joint minimax over the colours) and per-colour zeros z_k (a minimax per colour on a
// 0.25 dB/oct grid, continued from c = 0 where z = p). The zeros and the gain g are interpolated linearly between the
// grid rows; g makes the power gain 1 (within 0.105 dB between rows), so the cascade's output RMS is the drawn
// distribution's at every colour. Cost: 8 multiply-adds per step (two per section; g scales the draw).
// Q?-NZ.1-a (the user, 2026-11-12): colours ≤ 0 are FOLDED into [−1, 1] after the cascade (a brown walk reflects at the
// Range's ends); colours > 0 are SCALED instead, by `bound` = 1 / Σ|h| (h the unit-power design's impulse response,
// lowered by the worst ratio of the interpolated designs: tools/fit/noise_colour.py) and by 1 / the draw's bound
// (Gaussian: 1 / 1.1547), so the output can never leave [−1, 1] and its slope stays the design's exactly. That costs
// level: uniform −5.3 dB at +1, −5.8 at +1.5 (the most), −4.85 at +3, −3.83 at +6 dB/oct against white (the gain
// rises again toward violet: a steep tilt has a shorter impulse response); Gaussian a further −1.25 dB.
//
// Not a hot-path header: the kernel includes it; the views (the face's slope line) and the tests read the design.

#include <cstdint>

namespace pq::dsp {

inline constexpr float kNoiseColourMin = -6.0f, kNoiseColourMax = 6.0f;  // dB/oct
inline constexpr uint32_t kNoiseColourSections = 4;
// Density at or above this is continuous (every step keeps its value): the top of the module's Density control.
inline constexpr float kNoiseDensityContinuous = 100000.0f;
// Gaussian draws: the centred sum of four 16-bit uniforms (Irwin–Hall 4) scaled to σ = 1/3; |value| ≤ 1.1547.
inline constexpr float kNoiseGaussScale = 8.80936915e-6f;  // (1/3) / (65536 · √(1/3))
inline constexpr float kNoiseGaussBound = 1.15470054f;     // |a Gaussian draw| ≤ 2 · 65535 · kNoiseGaussScale

struct NoiseColourDesign {
  float z[kNoiseColourSections] = {};  // zeros (the poles are kNoiseColourPoles)
  float gain = 1.0f;                   // the input scale giving unit power gain
  float bound = 1.0f;                  // colours > 0: the output scale that keeps it in [−1, 1] (else 1)
  bool white = true;                   // colour 0 (or NaN): the identity, no filter
};
extern const float kNoiseColourPoles[kNoiseColourSections];
// The design the kernel uses at colour `c` (clamped to [−6, 6]; NaN = 0). RT-safe (a table read and a lerp).
NoiseColourDesign noiseColourDesign(float c) noexcept;
// |H| of a design at f cycles per step (0 … 0.5), in dB, gain included (double evaluation; tests and the face).
double noiseColourResponseDb(const NoiseColourDesign& d, double f) noexcept;

// Reflection into [−1, 1] (period 4: 1.2 → 0.8, −1.5 → −0.5); |x| ≤ 1 passes unchanged. NaN → 0. RT-safe.
float noiseFold(float x) noexcept;

}  // namespace pq::dsp
