#pragma once

// TMM.6: the rate converters of the patch language's `oversample(n) { … }` region (docs/PATCH_LANGUAGE.md §2.3,
// docs/plans/MM.md TMM.6). The compiler wraps every audio-rate value that crosses the region's boundary in one of
// these kernels (pq/dsp/atoms.h: oversample_up / _down / _phase / _hold / _any); the region's own ops run on n·frames
// samples at n·sr.
//
// ---- Half-band filters --------------------------------------------------------------------------------------------
// up / down are cascades of polyphase IIR half-band stages (two parallel chains of first-order allpasses, L. de Soras'
// "hiir" design after Valenzuela & Constantinides), one stage per ×2. Stage s (1-based) runs between 2^(s−1)·sr and
// 2^s·sr; its allpasses run at its low rate:
//   H(z) = ½·(A0(z²) + z⁻¹·A1(z²)),  A0 = allpasses 0, 2, 4, …,  A1 = allpasses 1, 3, 5, …
//   up    x → (A0(x), A1(x))              the two high-rate samples of one low-rate sample, in time order
//   down  (e, o) → ½·(A0(o) + A1(e))       e = the earlier of the two high-rate samples
//   one allpass (c + z⁻¹)/(1 + c·z⁻¹):     y = fma(−c, y[n−1], fma(c, x, x[n−1]))  (one FMA on the recursion)
// Coefficients designed for 44.1 kHz (the narrowest case; every rate above has a wider transition band) so that
// each stage keeps 0 … 20 kHz and rejects everything that would fold below 20 kHz (its stopband starts at
// 2^(s−1)·sr − 20 kHz):
//   stage 1   9 allpasses, transition 0.0465:  stopband ≥ 116.5 dB, passband ripple < 1e−11 dB
//   stage 2   4 allpasses, transition 0.2732:  ≥ 125 dB
//   stage 3   3 allpasses, transition 0.3866:  ≥ 140 dB
//   stage 4   2 allpasses, transition 0.4433:  ≥ 128.9 dB
// (with the float32-rounded coefficients the kernels use; tests/dsp/test_oversample.cpp checks every number).
// up followed by down is A0·A1 per stage: an allpass (|H| = 1 exactly), so a region that passes its input through
// changes only the phase. The phase is not linear. Group delay of up + down in base-rate samples (DC / 10 kHz /
// 20 kHz; DC is the same at every rate):
//   48 kHz    ×2 3.62 / 4.58 / 12.5    ×4 4.74 / 5.76 / 13.9    ×8 5.17 / 6.19 / 14.3    ×16 5.31 / 6.33 / 14.5
//   44.1 kHz  ×2 3.62 / 4.80 / 19.2    ×4 4.74 / 5.99 / 20.6    ×8 5.17 / 6.42 / 21.1    ×16 5.31 / 6.56 / 21.2
// The region does not report it as latency (no PDC): like Saturator HQ's half-bands, it is part of the sound.
//
// ---- Phase, bool -----------------------------------------------------------------------------------------------
//   phase  a phase (any real, normally 0..1 with wraps) by unwrapped linear interpolation: d = x[k] − x[k−1] taken
//          to [−½, ½) (the shortest way round), sub-sample j of frame k = floor(x[k]) + frac(x[k] − d·(n−1−j)/n), so
//          the last sub-sample is x[k] exactly and a ramp stays a ramp through its wraps (exact for phasors). The
//          first frame after a reset holds x. Latency: (n − 1)/n base samples.
//   hold   bool up: each frame's mask repeated n times (a gate stays a level; a one-sample trigger becomes one frame
//          of n high sub-samples, which rise() inside the region turns back into one pulse).
//   any    bool down: a frame is high when any of its n sub-samples is (pulses inside the region survive).
//
// Kernel convention: input 1 is the factor n (Spl; 2, 4, 8 or 16; anything else acts as 1: a copy). KernelArgs::frames
// is the oversampled count (the region's frames · n): up / phase / hold read frames / n samples and write frames,
// down / any read frames and write frames / n. Everything is RT-safe.

#include <array>
#include <complex>
#include <cstdint>

namespace pq::dsp::os {

inline constexpr uint32_t kMaxFactor = 16;
inline constexpr uint32_t kMaxStages = 4;      // log2(kMaxFactor)
inline constexpr uint32_t kAllpasses = 18;     // over all stages: 9 + 4 + 3 + 2

// Allpass coefficients per stage (double, as designed); the kernels use them rounded to float.
inline constexpr std::array<uint32_t, kMaxStages> kStageSize = {9, 4, 3, 2};
inline constexpr std::array<uint32_t, kMaxStages> kStageFirst = {0, 9, 13, 16};  // index of the stage's first coefficient
inline constexpr double kCoefs[kAllpasses] = {
    // stage 1 (transition 0.04649)
    0.030056471459502642, 0.11361863776018441, 0.23391209165023141, 0.37095740573090213, 0.50809226737546276,
    0.63527789133248092, 0.74925801983651086, 0.85216123100965158, 0.95007945502645574,
    // stage 2 (0.2732)
    0.0400293981753434, 0.16276072472691966, 0.38124956271820354, 0.7376963807594471,
    // stage 3 (0.3866)
    0.055202094936981777, 0.24139983733145295, 0.64512771133500513,
    // stage 4 (0.4433)
    0.10694060928464291, 0.5304659173007289};

// 1, 2, 4, 8 or 16 for a factor value as the kernels read it (anything else: 1). RT-safe.
constexpr uint32_t sanitizeFactor(float f) noexcept {
  return f == 2.0f ? 2u : f == 4.0f ? 4u : f == 8.0f ? 8u : f == 16.0f ? 16u : 1u;
}
constexpr uint32_t stagesOf(uint32_t factor) noexcept {
  return factor >= 16 ? 4u : factor >= 8 ? 3u : factor >= 4 ? 2u : factor >= 2 ? 1u : 0u;
}
constexpr bool validFactor(uint32_t n) noexcept { return n == 1 || n == 2 || n == 4 || n == 8 || n == 16; }

// TX.group-oversample: the converters' frequency responses at a base-rate frequency ω (radians per base sample), as the
// kernels compute them (the float32-rounded coefficients), for the analytic response through a region
// (pq/graph/response.h). The region's sub-sample n·k + j is base time k + j/n; stage s (0-based) runs between 2^s·sr
// and 2^(s+1)·sr, θ = ω / 2^(s+1) at its high rate, its allpasses at the low rate (z⁻¹ = e^(−j2θ)):
//   up    ½·(A0 + e^(−jθ)·A1)    (x → (A0(x), A1(x)) in time order: zero-stuffing then H(z) = ½(A0(z²) + z⁻¹A1(z²)),
//                                  the image rejected)
//   down  ½·(e^(jθ)·A0 + A1)     ((e, o) → ½(A0(o) + A1(e)): o is one high-rate sample later than e)
// the product over the stages. up · down is A0·A1 per stage (|H| = 1). What a region's images add (the up filter's
// leak above the base Nyquist, folded back by the down) is below the stopbands (≥ 116 dB) up to 20 kHz
// (no allocation, RT-safe).
inline std::complex<double> allpassChain(uint32_t first, uint32_t count, std::complex<double> zInv) noexcept {  // RT-safe
  std::complex<double> h{1.0, 0.0};
  for (uint32_t k = 0; k < count; ++k) {
    const double c = static_cast<double>(static_cast<float>(kCoefs[first + 2 * k]));
    h *= (c + zInv) / (1.0 + c * zInv);
  }
  return h;
}
inline std::complex<double> halfBandResponse(uint32_t factor, double omega, bool up) noexcept {  // RT-safe
  std::complex<double> h{1.0, 0.0};
  const uint32_t stages = stagesOf(factor);
  for (uint32_t s = 0; s < stages; ++s) {
    const double theta = omega / static_cast<double>(2u << s);
    const std::complex<double> zl = std::polar(1.0, -2.0 * theta);
    const uint32_t first = kStageFirst[s], size = kStageSize[s];
    const std::complex<double> a0 = allpassChain(first, (size + 1) / 2, zl), a1 = allpassChain(first + 1, size / 2, zl);
    h *= up ? 0.5 * (a0 + std::polar(1.0, -theta) * a1) : 0.5 * (std::polar(1.0, theta) * a0 + a1);
  }
  return h;
}

}  // namespace pq::dsp::os
