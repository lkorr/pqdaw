#pragma once

// Fast float32 approximations of exp2, log2, sin, tan and tanh, scalar and AVX2 8-wide (SPEC §7.2 `fast_` atoms).
//
// One implementation serves both widths: each kernel is a template over a tiny ops struct (SSE 4-wide for the
// scalar entry points, AVX2 8-wide for __m256), so scalar and SIMD results are bit-identical (tested). Every
// kernel uses FMA (part of our /arch:AVX2 baseline) and contains no branches, tables or divisions except where
// noted, so it vectorises across voices with no lane divergence.
//
// Accuracy policy: each approximation is a minimax polynomial (or rational, for tanh) fitted by
// tools/fit/fast_math_coeffs.py and chosen to be close to float32 resolution, because one more FMA costs far
// less than an audible or measurable error in pitch (cents) or filter cutoff. Bounds below are verified by
// sweep tests against double-precision std:: references (tests/core/test_fast_math.cpp).
//
// Special values are honest (no "no-NaN" assumptions): NaN in gives NaN out for every function; behaviour for
// infinities and out-of-domain input is documented per function and tested. Nothing here reads or changes
// MXCSR, so results are the same with or without FTZ/DAZ, except that denormal results can be flushed.
//
// All functions are RT-safe: no allocation, no locks, no syscalls, no exceptions.

#include <immintrin.h>

#include <cstdint>
#include <limits>

namespace pq {

// ---- Documented error bounds (verified by tests/core/test_fast_math.cpp) ------------------------------------
// Each bound is the maximum measured over a dense sweep (millions of points, all binades) rounded up ~25%.
//
// fastExp2(x)  domain [-126, 127]: max relative error kFastExp2MaxRelErr (≈ kFastExp2MaxRelErr·1731 cents when
//              used as a pitch ratio). Exact for integer x (2^n). x < -126.5 → +0 (flush), x ≥ 127.5 → +inf,
//              -inf → +0, +inf → +inf. Results in [2^-126.5, 2^-126) may be denormal.
inline constexpr float kFastExp2MaxRelErr = 2.0e-7f;  // measured 1.55e-7 = 0.00027 cents
//
// fastLog2(x)  domain all positive normal floats: max absolute error kFastLog2MaxAbsErr (log2 units; ×1200 =
//              cents) for x in [2^-16, 2^16]; over all normals kFastLog2MaxAbsErrAll, which is float32 output
//              resolution (|log2| up to 128 has an ulp of 7.6e-6), not the approximation. For x in [0.5, 2] max
//              relative error kFastLog2MaxRelErrNear1. Exact for powers of two.
//              x == ±0 or denormal → -inf; x < 0 (incl. -inf) → NaN; +inf → +inf.
inline constexpr float kFastLog2MaxAbsErr = 1.0e-6f;
inline constexpr float kFastLog2MaxAbsErrAll = 4.5e-6f;   // measured 3.9e-6 at log2 ≈ 97
inline constexpr float kFastLog2MaxRelErrNear1 = 4.0e-7f; // measured 3.2e-7
//
// fastSin(x)   x in radians, domain |x| ≤ kFastSinMaxArg: max absolute error kFastSinMaxAbsErr against
//              sin(double(x)). sin(±0) = +0. ±inf → NaN. |x| > kFastSinMaxArg: unspecified (not checked).
inline constexpr float kFastSinMaxArg = 65536.0f;
inline constexpr float kFastSinMaxAbsErr = 1.5e-7f;  // measured 1.15e-7
//
// fastTan(x)   domain |x| < π/2 (intended for the bilinear prewarp g = tan(π·fc/fs), fc < fs/2): max relative
//              error kFastTanMaxRelErr, including right up to the pole. Input is clamped to ±kFastTanMaxArg (the
//              largest float below π/2), so for any non-NaN input the result is finite (|tan| ≤ ~1.4e7):
//              fc ≥ fs/2 saturates instead of producing inf/NaN in filter state. Uses one division.
inline constexpr float kFastTanMaxArg = 1.57079625f;  // nextafter(float(π/2), 0); float(π/2) itself is above π/2
inline constexpr float kFastTanMaxRelErr = 4.0e-7f;  // measured 3.0e-7
//
// fastTanh(x)  domain all reals: max absolute error kFastTanhMaxAbsErr, max relative error kFastTanhMaxRelErr.
//              Odd, |result| ≤ 1 exactly, ±inf → ±1, tanh(±0) = ±0. Uses one division.
inline constexpr float kFastTanhMaxAbsErr = 4.0e-7f;  // measured 3.3e-7
inline constexpr float kFastTanhMaxRelErr = 5.5e-7f;  // measured 4.4e-7

namespace detail::fm {

// 8-wide ops (AVX2 + FMA).
struct V8 {
  using F = __m256;
  using I = __m256i;
  static F set(float v) noexcept { return _mm256_set1_ps(v); }
  static I seti(int32_t v) noexcept { return _mm256_set1_epi32(v); }
  static F add(F a, F b) noexcept { return _mm256_add_ps(a, b); }
  static F sub(F a, F b) noexcept { return _mm256_sub_ps(a, b); }
  static F mul(F a, F b) noexcept { return _mm256_mul_ps(a, b); }
  static F div(F a, F b) noexcept { return _mm256_div_ps(a, b); }
  static F fma(F a, F b, F c) noexcept { return _mm256_fmadd_ps(a, b, c); }    // a·b + c
  static F fnma(F a, F b, F c) noexcept { return _mm256_fnmadd_ps(a, b, c); }  // c − a·b
  // min/max return the SECOND operand when either is NaN: callers pass the constant first so NaN propagates.
  static F min(F a, F b) noexcept { return _mm256_min_ps(a, b); }
  static F max(F a, F b) noexcept { return _mm256_max_ps(a, b); }
  static F andnot(F a, F b) noexcept { return _mm256_andnot_ps(a, b); }  // ~a & b
  static F band(F a, F b) noexcept { return _mm256_and_ps(a, b); }
  static F bor(F a, F b) noexcept { return _mm256_or_ps(a, b); }
  static F bxor(F a, F b) noexcept { return _mm256_xor_ps(a, b); }
  static F roundNearest(F x) noexcept { return _mm256_round_ps(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
  static I truncToInt(F x) noexcept { return _mm256_cvttps_epi32(x); }
  static F intToFloat(I x) noexcept { return _mm256_cvtepi32_ps(x); }
  static I addi(I a, I b) noexcept { return _mm256_add_epi32(a, b); }
  static I subi(I a, I b) noexcept { return _mm256_sub_epi32(a, b); }
  template <int N> static I shl(I x) noexcept { return _mm256_slli_epi32(x, N); }
  template <int N> static I sar(I x) noexcept { return _mm256_srai_epi32(x, N); }
  static F asF(I x) noexcept { return _mm256_castsi256_ps(x); }
  static I asI(F x) noexcept { return _mm256_castps_si256(x); }
  static F lt(F a, F b) noexcept { return _mm256_cmp_ps(a, b, _CMP_LT_OQ); }     // false if NaN
  static F notLe(F a, F b) noexcept { return _mm256_cmp_ps(a, b, _CMP_NLE_UQ); } // true if NaN
  static F blend(F a, F b, F mask) noexcept { return _mm256_blendv_ps(a, b, mask); }  // mask ? b : a
};

// 4-wide ops (SSE4.1/AVX-128 + FMA); the scalar entry points use lane 0.
struct V4 {
  using F = __m128;
  using I = __m128i;
  static F set(float v) noexcept { return _mm_set1_ps(v); }
  static I seti(int32_t v) noexcept { return _mm_set1_epi32(v); }
  static F add(F a, F b) noexcept { return _mm_add_ps(a, b); }
  static F sub(F a, F b) noexcept { return _mm_sub_ps(a, b); }
  static F mul(F a, F b) noexcept { return _mm_mul_ps(a, b); }
  static F div(F a, F b) noexcept { return _mm_div_ps(a, b); }
  static F fma(F a, F b, F c) noexcept { return _mm_fmadd_ps(a, b, c); }
  static F fnma(F a, F b, F c) noexcept { return _mm_fnmadd_ps(a, b, c); }
  static F min(F a, F b) noexcept { return _mm_min_ps(a, b); }
  static F max(F a, F b) noexcept { return _mm_max_ps(a, b); }
  static F andnot(F a, F b) noexcept { return _mm_andnot_ps(a, b); }
  static F band(F a, F b) noexcept { return _mm_and_ps(a, b); }
  static F bor(F a, F b) noexcept { return _mm_or_ps(a, b); }
  static F bxor(F a, F b) noexcept { return _mm_xor_ps(a, b); }
  static F roundNearest(F x) noexcept { return _mm_round_ps(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
  static I truncToInt(F x) noexcept { return _mm_cvttps_epi32(x); }
  static F intToFloat(I x) noexcept { return _mm_cvtepi32_ps(x); }
  static I addi(I a, I b) noexcept { return _mm_add_epi32(a, b); }
  static I subi(I a, I b) noexcept { return _mm_sub_epi32(a, b); }
  template <int N> static I shl(I x) noexcept { return _mm_slli_epi32(x, N); }
  template <int N> static I sar(I x) noexcept { return _mm_srai_epi32(x, N); }
  static F asF(I x) noexcept { return _mm_castsi128_ps(x); }
  static I asI(F x) noexcept { return _mm_castps_si128(x); }
  static F lt(F a, F b) noexcept { return _mm_cmp_ps(a, b, _CMP_LT_OQ); }
  static F notLe(F a, F b) noexcept { return _mm_cmp_ps(a, b, _CMP_NLE_UQ); }
  static F blend(F a, F b, F mask) noexcept { return _mm_blendv_ps(a, b, mask); }
};

// 2^x. Split x = n + f with n = round(x), f in [-0.5, 0.5] (exact); 2^f = 1 + f·P(f), P degree 4 (minimax
// relative, 9.9e-8 before float rounding); 2^n is built in the exponent field. Clamping x to [-127, 128] makes
// n = -127 produce exponent bits 0 (→ +0) and n = 128 produce exponent 255 (→ +inf), so no extra blends.
template <class S>
inline typename S::F exp2(typename S::F x) noexcept {  // RT-safe
  x = S::max(S::set(-127.0f), x);  // constant first: NaN propagates
  x = S::min(S::set(128.0f), x);
  const auto n = S::roundNearest(x);
  const auto f = S::sub(x, n);
  auto q = S::set(0.0013266394843173667f);
  q = S::fma(q, f, S::set(0.0096719582065429594f));
  q = S::fma(q, f, S::set(0.05550745789510219f));
  q = S::fma(q, f, S::set(0.24022233785333449f));
  q = S::fma(q, f, S::set(0.69314694538055188f));
  const auto p = S::fma(q, f, S::set(1.0f));
  // NaN: truncToInt gives INT_MIN, so scale is finite garbage, but f (and p) are NaN, so the product is NaN.
  const auto scale = S::asF(S::template shl<23>(S::addi(S::truncToInt(n), S::seti(127))));
  return S::mul(p, scale);
}

// log2(x). Split x = m·2^e with m in [√½, √2) using integer ops on the bit pattern; t = m − 1 (exact);
// log2(m) = t·P(t), P degree 7 (minimax relative, 1.9e-7); result = fma(t, P, e) (one rounding).
template <class S>
inline typename S::F log2(typename S::F x) noexcept {  // RT-safe
  const auto bits = S::asI(x);
  const auto e = S::template sar<23>(S::subi(bits, S::seti(0x3F3504F3)));  // 0x3F3504F3 = √½
  const auto m = S::asF(S::subi(bits, S::template shl<23>(e)));
  const auto t = S::sub(m, S::set(1.0f));
  auto q = S::set(-0.1459443443185002f);
  q = S::fma(q, t, S::set(0.23458358063986023f));
  q = S::fma(q, t, S::set(-0.24902718721648978f));
  q = S::fma(q, t, S::set(0.2870396621192145f));
  q = S::fma(q, t, S::set(-0.36022487421220006f));
  q = S::fma(q, t, S::set(0.48092493070814096f));
  q = S::fma(q, t, S::set(-0.72135304562256186f));
  q = S::fma(q, t, S::set(1.442694954169498f));
  auto r = S::fma(t, q, S::intToFloat(e));
  // Special values: zero/denormal/negative → -inf, then negative → NaN (all-ones is a NaN), NaN/+inf → x.
  r = S::blend(r, S::set(-std::numeric_limits<float>::infinity()), S::lt(x, S::set(1.17549435e-38f)));
  r = S::bor(r, S::lt(x, S::set(0.0f)));
  r = S::blend(r, x, S::notLe(x, S::set(3.40282347e+38f)));
  return r;
}

// sin(x), x in radians. k = round(x/π); r = x − k·π with a two-constant Cody–Waite reduction (each step one FMA,
// exact product); sin(x) = (−1)^k·sin(r); sin(r) = r + r³·P(r²), P degree 3 (minimax relative on
// |r| ≤ π/2 + 0.01, 1.8e-8). The sign flip comes from bit 0 of k shifted into the sign bit.
template <class S>
inline typename S::F sin(typename S::F x) noexcept {  // RT-safe
  const auto k = S::roundNearest(S::mul(x, S::set(0.318309886183790672f)));
  auto r = S::fnma(k, S::set(3.14159274101257324f), x);  // float(π)
  r = S::fnma(k, S::set(-8.74227800037248e-08f), r);     // π − float(π)
  const auto u = S::mul(r, r);
  auto q = S::set(2.6217112448405671e-06f);
  q = S::fma(q, u, S::set(-0.00019817365741503661f));
  q = S::fma(q, u, S::set(0.008333179077226565f));
  q = S::fma(q, u, S::set(-0.16666664119478575f));
  const auto s = S::fma(S::mul(r, u), q, r);
  const auto sign = S::asF(S::template shl<31>(S::truncToInt(k)));
  return S::bxor(s, sign);
}

// tan(x) for |x| < π/2. tan(x) = x·P(x²) / ((π/2 − x)(π/2 + x)): the pole is factored out explicitly, so
// P(u), degree 4 in u = x² (minimax relative, 4.7e-8), is smooth over the whole interval and the relative
// error stays flat up to the pole. π/2 ± x use a two-constant split (the first subtraction is exact near the
// pole by Sterbenz), so the small factor keeps full relative precision.
template <class S>
inline typename S::F tan(typename S::F x) noexcept {  // RT-safe
  x = S::max(S::set(-kFastTanMaxArg), x);
  x = S::min(S::set(kFastTanMaxArg), x);
  const auto hi = S::set(1.57079637050628662f);  // float(π/2), above π/2
  const auto lo = S::set(-4.37113900018624e-08f);  // π/2 − float(π/2)
  const auto a = S::add(S::sub(hi, x), lo);
  const auto b = S::add(S::add(hi, x), lo);
  const auto u = S::mul(x, x);
  auto q = S::set(-9.9407543316968712e-06f);
  q = S::fma(q, u, S::set(-0.00016632014312408942f));
  q = S::fma(q, u, S::set(-0.0043515972719862022f));
  q = S::fma(q, u, S::set(-0.17753145862568345f));
  q = S::fma(q, u, S::set(2.4674010501233603f));
  return S::div(S::mul(x, q), S::mul(a, b));
}

// tanh(x) = sign(x)·min(1, a·N(a²)/D(a²)), a = min(|x|, 9); [9/8] minimax rational (relative, 2.5e-7).
// Beyond 9, tanh rounds to 1 in float32. The final min(1, ·) guarantees |result| ≤ 1.
template <class S>
inline typename S::F tanh(typename S::F x) noexcept {  // RT-safe
  const auto signMask = S::set(-0.0f);
  const auto sign = S::band(signMask, x);
  auto a = S::andnot(signMask, x);
  a = S::min(S::set(9.0f), a);  // constant first: NaN propagates
  const auto u = S::mul(a, a);
  auto n = S::set(1.4104955193705166e-08f);
  n = S::fma(n, u, S::set(2.1267678992103765e-05f));
  n = S::fma(n, u, S::set(0.0035413935250930514f));
  n = S::fma(n, u, S::set(0.13421763276148324f));
  n = S::fma(n, u, S::set(1.0000002541326505f));
  auto d = S::set(8.1171001404063493e-07f);
  d = S::fma(d, u, S::set(0.00033563277332916529f));
  d = S::fma(d, u, S::set(0.026057829858016916f));
  d = S::fma(d, u, S::set(0.46755201645660105f));
  d = S::fma(d, u, S::set(1.0f));
  auto r = S::div(S::mul(a, n), d);
  r = S::min(S::set(1.0f), r);
  return S::bor(r, sign);
}

}  // namespace detail::fm

// ---- Scalar ----------------------------------------------------------------------------------------------------
// RT-safe. Bit-identical to lane i of the __m256 overloads for the same input.
inline float fastExp2(float x) noexcept { return _mm_cvtss_f32(detail::fm::exp2<detail::fm::V4>(_mm_set_ss(x))); }
inline float fastLog2(float x) noexcept { return _mm_cvtss_f32(detail::fm::log2<detail::fm::V4>(_mm_set_ss(x))); }
inline float fastSin(float x) noexcept { return _mm_cvtss_f32(detail::fm::sin<detail::fm::V4>(_mm_set_ss(x))); }
inline float fastTan(float x) noexcept { return _mm_cvtss_f32(detail::fm::tan<detail::fm::V4>(_mm_set_ss(x))); }
inline float fastTanh(float x) noexcept { return _mm_cvtss_f32(detail::fm::tanh<detail::fm::V4>(_mm_set_ss(x))); }

// ---- AVX2 8-wide -------------------------------------------------------------------------------------------------
// RT-safe. One voice (or sample) per lane.
inline __m256 fastExp2(__m256 x) noexcept { return detail::fm::exp2<detail::fm::V8>(x); }
inline __m256 fastLog2(__m256 x) noexcept { return detail::fm::log2<detail::fm::V8>(x); }
inline __m256 fastSin(__m256 x) noexcept { return detail::fm::sin<detail::fm::V8>(x); }
inline __m256 fastTan(__m256 x) noexcept { return detail::fm::tan<detail::fm::V8>(x); }
inline __m256 fastTanh(__m256 x) noexcept { return detail::fm::tanh<detail::fm::V8>(x); }

}  // namespace pq
