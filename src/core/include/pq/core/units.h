#pragma once

// Pitch and level unit conversions: precise (double, std:: math) and fast (float32 / AVX2, built on fast_math.h).
//
// Precise versions are for document-level work and for resolving constant pitches (SPEC §3.1: "exact centsToHz
// uses the precise path"). Fast versions are for audio-rate modulation (`cents↔hz`, `db↔gain` atoms, SPEC §7.2).
// All are RT-safe (pure math; the CRT functions used do not allocate or lock).

#include <immintrin.h>

#include <cmath>

#include "pq/core/fast_math.h"

namespace pq {

// Default pitch reference: C3 = 261.626 Hz (Ableton's middle-C naming; SPEC §3.1). Changeable per project/clip.
inline constexpr double kDefaultReferenceHz = 261.626;

// ---- Precise (double) ---------------------------------------------------------------------------------------
// centsToHz(0, ref) == ref exactly; centsToHz(1200·k, ref) == ref·2^k exactly (std::exp2 is exact on integers).
inline double centsToHz(double cents, double refHz = kDefaultReferenceHz) noexcept {  // RT-safe
  return refHz * std::exp2(cents / 1200.0);
}
// hz ≤ 0 → -inf (0) or NaN (negative), as std::log2.
inline double hzToCents(double hz, double refHz = kDefaultReferenceHz) noexcept {  // RT-safe
  return 1200.0 * std::log2(hz / refHz);
}
// dbToGain(-inf) == 0; gainToDb(0) == -inf; gainToDb(negative) == NaN.
inline double dbToGain(double db) noexcept { return std::pow(10.0, db / 20.0); }      // RT-safe
inline double gainToDb(double gain) noexcept { return 20.0 * std::log10(gain); }      // RT-safe

// ---- Fast (float32 scalar and AVX2 8-wide; bit-identical per lane) --------------------------------------------
// Error bounds verified by tests/core/test_fast_math.cpp over the stated domains:
//
// Most of these errors are float32 rounding of the input scaling (cents/1200, dB·log2(10)/20) or of the output
// (24000 cents has an ulp of 0.002), not the approximations themselves.
//
// fastCentsToHz: cents in [-12000, 12000] (±10 octaves) → max error kFastCentsToHzMaxErrCents cents.
//   Below 2^-126.5 × ref the result flushes to 0; NaN → NaN.
inline constexpr float kFastCentsToHzMaxErrCents = 1.5e-3f;  // measured 1.18e-3
// fastHzToCents: hz/ref in [2^-20, 2^20] → max absolute error kFastHzToCentsMaxErrCents cents.
//   hz ≤ 0 → -inf (0) / NaN (negative), as fastLog2.
inline constexpr float kFastHzToCentsMaxErrCents = 3.0e-3f;  // measured 2.3e-3 (output ulp-bound)
// fastDbToGain: dB in [-300, 300] → max relative error kFastDbToGainMaxRelErr. -inf → 0. dB < ~-764 → 0.
inline constexpr float kFastDbToGainMaxRelErr = 2.0e-6f;  // measured 1.57e-6
// fastGainToDb: gain in [1e-15, 1e15] → max absolute error kFastGainToDbMaxAbsErrDb dB. 0 → -inf.
inline constexpr float kFastGainToDbMaxAbsErrDb = 4.0e-5f;  // measured 3.0e-5 (300 dB has an ulp of 3e-5)

namespace detail::units {
inline constexpr float kInv1200 = 1.0f / 1200.0f;
inline constexpr float kDbToLog2 = 0.166096404744368118f;  // log2(10)/20
inline constexpr float kLog2ToDb = 6.02059991327962390f;   // 20/log2(10) = 20·log10(2)
}  // namespace detail::units

inline float fastCentsToHz(float cents, float refHz) noexcept {  // RT-safe
  return refHz * fastExp2(cents * detail::units::kInv1200);
}
inline __m256 fastCentsToHz(__m256 cents, __m256 refHz) noexcept {  // RT-safe
  return _mm256_mul_ps(refHz, fastExp2(_mm256_mul_ps(cents, _mm256_set1_ps(detail::units::kInv1200))));
}

// The division by refHz is loop-invariant in typical use and gets hoisted by the compiler.
inline float fastHzToCents(float hz, float refHz) noexcept {  // RT-safe
  return 1200.0f * fastLog2(hz * (1.0f / refHz));
}
inline __m256 fastHzToCents(__m256 hz, __m256 refHz) noexcept {  // RT-safe
  const __m256 ratio = _mm256_mul_ps(hz, _mm256_div_ps(_mm256_set1_ps(1.0f), refHz));
  return _mm256_mul_ps(_mm256_set1_ps(1200.0f), fastLog2(ratio));
}

inline float fastDbToGain(float db) noexcept {  // RT-safe
  return fastExp2(db * detail::units::kDbToLog2);
}
inline __m256 fastDbToGain(__m256 db) noexcept {  // RT-safe
  return fastExp2(_mm256_mul_ps(db, _mm256_set1_ps(detail::units::kDbToLog2)));
}

inline float fastGainToDb(float gain) noexcept {  // RT-safe
  return fastLog2(gain) * detail::units::kLog2ToDb;
}
inline __m256 fastGainToDb(__m256 gain) noexcept {  // RT-safe
  return _mm256_mul_ps(fastLog2(gain), _mm256_set1_ps(detail::units::kLog2ToDb));
}

}  // namespace pq
