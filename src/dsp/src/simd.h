#pragma once

// Lane-width abstraction for the leaf-atom kernels (private to pq_dsp).
//
// Every kernel body is written once as a template over an ops struct S:
//   S8: one AVX2 __m256 = 8 voices of one voice group (L = 8), or 8 consecutive samples of a mono signal when a
//       stateless atom is vectorised over time.
//   S1: one float in lane 0 of an SSE register (L = 1, the scalar path). Using SSE for the scalar path, like
//       pq/core/fast_math.h does, makes S1 and S8 bit-identical per lane for every op built from these
//       primitives (the only exception is the `exact` transcendental family, which uses the CRT's scalar
//       functions on S1 and MSVC's SVML vector functions on S8; both are tested against double references).
//
// Bool masks are floats with all bits set or clear (kernel.h). Only lane 0 of an S1 value is meaningful; the
// other SSE lanes hold harmless garbage and are never stored or gathered.
// Everything here is RT-safe: pure register arithmetic.

#include <immintrin.h>

#include <cmath>
#include <cstdint>

#include "pq/core/fast_math.h"

namespace pq::dsp::simd {

struct S8 {
  static constexpr uint32_t kW = 8;
  using F = __m256;
  using I = __m256i;

  static F load(const float* p) noexcept { return _mm256_loadu_ps(p); }
  static void store(float* p, F v) noexcept { _mm256_storeu_ps(p, v); }
  static I loadi(const int32_t* p) noexcept { return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)); }
  static void storei(int32_t* p, I v) noexcept { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), v); }
  static I loadu32(const uint32_t* p) noexcept { return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)); }
  static void storeu32(uint32_t* p, I v) noexcept { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), v); }
  static F set(float v) noexcept { return _mm256_set1_ps(v); }
  static I seti(int32_t v) noexcept { return _mm256_set1_epi32(v); }
  static F zero() noexcept { return _mm256_setzero_ps(); }
  static F ones() noexcept { return _mm256_castsi256_ps(_mm256_set1_epi32(-1)); }
  // Lane index 0..7 as int (used to build gather offsets into lane-interleaved buffers).
  static I laneIndex() noexcept { return _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7); }

  static F add(F a, F b) noexcept { return _mm256_add_ps(a, b); }
  static F sub(F a, F b) noexcept { return _mm256_sub_ps(a, b); }
  static F mul(F a, F b) noexcept { return _mm256_mul_ps(a, b); }
  static F div(F a, F b) noexcept { return _mm256_div_ps(a, b); }
  // min/max return the SECOND operand if either is NaN.
  static F min(F a, F b) noexcept { return _mm256_min_ps(a, b); }
  static F max(F a, F b) noexcept { return _mm256_max_ps(a, b); }
  static F fma(F a, F b, F c) noexcept { return _mm256_fmadd_ps(a, b, c); }    // a·b + c
  static F fnma(F a, F b, F c) noexcept { return _mm256_fnmadd_ps(a, b, c); }  // c − a·b
  static F sqrt(F a) noexcept { return _mm256_sqrt_ps(a); }
  static F floor(F a) noexcept { return _mm256_round_ps(a, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC); }
  static F round(F a) noexcept { return _mm256_round_ps(a, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
  static F abs(F a) noexcept { return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a); }
  static F neg(F a) noexcept { return _mm256_xor_ps(_mm256_set1_ps(-0.0f), a); }

  // Comparisons → masks. Ordered (false on NaN) except ne, which is true on NaN (IEEE !=).
  static F lt(F a, F b) noexcept { return _mm256_cmp_ps(a, b, _CMP_LT_OQ); }
  static F le(F a, F b) noexcept { return _mm256_cmp_ps(a, b, _CMP_LE_OQ); }
  static F gt(F a, F b) noexcept { return _mm256_cmp_ps(a, b, _CMP_GT_OQ); }
  static F ge(F a, F b) noexcept { return _mm256_cmp_ps(a, b, _CMP_GE_OQ); }
  static F eq(F a, F b) noexcept { return _mm256_cmp_ps(a, b, _CMP_EQ_OQ); }
  static F ne(F a, F b) noexcept { return _mm256_cmp_ps(a, b, _CMP_NEQ_UQ); }

  static F band(F a, F b) noexcept { return _mm256_and_ps(a, b); }
  static F bor(F a, F b) noexcept { return _mm256_or_ps(a, b); }
  static F bxor(F a, F b) noexcept { return _mm256_xor_ps(a, b); }
  static F andnot(F a, F b) noexcept { return _mm256_andnot_ps(a, b); }  // ~a & b
  static F bnot(F a) noexcept { return _mm256_xor_ps(a, ones()); }
  static F select(F m, F a, F b) noexcept { return _mm256_blendv_ps(b, a, m); }  // m ? a : b
  static bool any(F m) noexcept { return _mm256_movemask_ps(m) != 0; }

  // Integer lanes (two's complement, wrapping).
  static I addi(I a, I b) noexcept { return _mm256_add_epi32(a, b); }
  static I subi(I a, I b) noexcept { return _mm256_sub_epi32(a, b); }
  static I muli(I a, I b) noexcept { return _mm256_mullo_epi32(a, b); }
  static I andi(I a, I b) noexcept { return _mm256_and_si256(a, b); }
  static I xori(I a, I b) noexcept { return _mm256_xor_si256(a, b); }
  static I mini(I a, I b) noexcept { return _mm256_min_epi32(a, b); }
  static I maxi(I a, I b) noexcept { return _mm256_max_epi32(a, b); }
  template <int N> static I slli(I a) noexcept { return _mm256_slli_epi32(a, N); }
  template <int N> static I srli(I a) noexcept { return _mm256_srli_epi32(a, N); }
  template <int N> static I srai(I a) noexcept { return _mm256_srai_epi32(a, N); }
  static I eqi(I a, I b) noexcept { return _mm256_cmpeq_epi32(a, b); }
  static I gti(I a, I b) noexcept { return _mm256_cmpgt_epi32(a, b); }
  static I selecti(I m, I a, I b) noexcept { return _mm256_blendv_epi8(b, a, m); }  // m ? a : b
  static I truncToInt(F a) noexcept { return _mm256_cvttps_epi32(a); }  // NaN/out of range → INT_MIN
  static I roundToInt(F a) noexcept { return _mm256_cvtps_epi32(a); }   // nearest-even (default MXCSR)
  static F toFloat(I a) noexcept { return _mm256_cvtepi32_ps(a); }
  static F asF(I a) noexcept { return _mm256_castsi256_ps(a); }
  static I asI(F a) noexcept { return _mm256_castps_si256(a); }

  // Table read: base[idx[lane]] for each lane. Callers guarantee every index is in bounds.
  static F gather(const float* base, I idx) noexcept { return _mm256_i32gather_ps(base, idx, 4); }
  // Adjacent pairs: a = base[idx], b = base[idx + 1] per lane, as two 4-element 64-bit gathers (half the load
  // uops of two 8-element float gathers) and a de-interleave.
  static void gatherPair(const float* base, I idx, F& a, F& b) noexcept {
    const __m256 lo = _mm256_castsi256_ps(
        _mm256_i32gather_epi64(reinterpret_cast<const long long*>(base), _mm256_castsi256_si128(idx), 4));
    const __m256 hi = _mm256_castsi256_ps(
        _mm256_i32gather_epi64(reinterpret_cast<const long long*>(base), _mm256_extracti128_si256(idx, 1), 4));
    // lo = [a0 b0 a1 b1 | a2 b2 a3 b3], hi = [a4 b4 a5 b5 | a6 b6 a7 b7]
    const __m256 ev = _mm256_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0));  // [a0 a1 a4 a5 | a2 a3 a6 a7]
    const __m256 od = _mm256_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1));
    a = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(ev), _MM_SHUFFLE(3, 1, 2, 0)));
    b = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(od), _MM_SHUFFLE(3, 1, 2, 0)));
  }

  // round(x · scale) as int32, computed in double (scale is exact enough to give the nearest integer of the
  // real product except within 2^-22 of a tie). Results must fit int32. Used for uint32 phase increments.
  static I roundMulToInt(F x, double scale) noexcept {
    const __m256d s = _mm256_set1_pd(scale);
    const __m128i lo = _mm256_cvtpd_epi32(_mm256_mul_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(x)), s));
    const __m128i hi = _mm256_cvtpd_epi32(_mm256_mul_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(x, 1)), s));
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
  }

  // Fast approximations (pq/core/fast_math.h).
  static F fastExp2(F x) noexcept { return pq::fastExp2(x); }
  static F fastLog2(F x) noexcept { return pq::fastLog2(x); }
  static F fastSin(F x) noexcept { return pq::fastSin(x); }
  static F fastTan(F x) noexcept { return pq::fastTan(x); }
  static F fastTanh(F x) noexcept { return pq::fastTanh(x); }

  // "Exact" family: MSVC SVML (float, ≤ a few ulp; verified by tests against double references).
  static F exactSin(F x) noexcept { return _mm256_sin_ps(x); }
  static F exactTan(F x) noexcept { return _mm256_tan_ps(x); }
  static F exactTanh(F x) noexcept { return _mm256_tanh_ps(x); }
  static F exactExp(F x) noexcept { return _mm256_exp_ps(x); }
  static F exactLog(F x) noexcept { return _mm256_log_ps(x); }
  static F exactPow(F a, F b) noexcept { return _mm256_pow_ps(a, b); }

  // Unit conversions in double (correctly rounded to float in practice). f(d) maps __m256d → __m256d.
  template <class Fn>
  static F viaDouble(F x, Fn f) noexcept {
    const __m128 lo = _mm256_cvtpd_ps(f(_mm256_cvtps_pd(_mm256_castps256_ps128(x))));
    const __m128 hi = _mm256_cvtpd_ps(f(_mm256_cvtps_pd(_mm256_extractf128_ps(x, 1))));
    return _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1);
  }
  template <class Fn>
  static F viaDouble2(F x, F y, Fn f) noexcept {
    const __m128 lo = _mm256_cvtpd_ps(f(_mm256_cvtps_pd(_mm256_castps256_ps128(x)),
                                        _mm256_cvtps_pd(_mm256_castps256_ps128(y))));
    const __m128 hi = _mm256_cvtpd_ps(f(_mm256_cvtps_pd(_mm256_extractf128_ps(x, 1)),
                                        _mm256_cvtps_pd(_mm256_extractf128_ps(y, 1))));
    return _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1);
  }
  static F exactCentsToHz(F c, F ref) noexcept {
    return viaDouble2(c, ref, [](__m256d cd, __m256d rd) {
      return _mm256_mul_pd(rd, _mm256_exp2_pd(_mm256_div_pd(cd, _mm256_set1_pd(1200.0))));
    });
  }
  static F exactHzToCents(F hz, F ref) noexcept {
    return viaDouble2(hz, ref, [](__m256d hd, __m256d rd) {
      return _mm256_mul_pd(_mm256_set1_pd(1200.0), _mm256_log2_pd(_mm256_div_pd(hd, rd)));
    });
  }
  static F exactDbToGain(F db) noexcept {
    return viaDouble(db, [](__m256d d) { return _mm256_exp10_pd(_mm256_div_pd(d, _mm256_set1_pd(20.0))); });
  }
  static F exactGainToDb(F g) noexcept {
    return viaDouble(g, [](__m256d d) { return _mm256_mul_pd(_mm256_set1_pd(20.0), _mm256_log10_pd(d)); });
  }
};

struct S1 {
  static constexpr uint32_t kW = 1;
  using F = __m128;
  using I = __m128i;

  static F load(const float* p) noexcept { return _mm_load_ss(p); }
  static void store(float* p, F v) noexcept { _mm_store_ss(p, v); }
  static I loadi(const int32_t* p) noexcept { return _mm_cvtsi32_si128(*p); }
  static void storei(int32_t* p, I v) noexcept { *p = _mm_cvtsi128_si32(v); }
  static I loadu32(const uint32_t* p) noexcept { return _mm_cvtsi32_si128(static_cast<int32_t>(*p)); }
  static void storeu32(uint32_t* p, I v) noexcept { *p = static_cast<uint32_t>(_mm_cvtsi128_si32(v)); }
  static F set(float v) noexcept { return _mm_set1_ps(v); }
  static I seti(int32_t v) noexcept { return _mm_set1_epi32(v); }
  static F zero() noexcept { return _mm_setzero_ps(); }
  static F ones() noexcept { return _mm_castsi128_ps(_mm_set1_epi32(-1)); }
  static I laneIndex() noexcept { return _mm_setzero_si128(); }

  static F add(F a, F b) noexcept { return _mm_add_ps(a, b); }
  static F sub(F a, F b) noexcept { return _mm_sub_ps(a, b); }
  static F mul(F a, F b) noexcept { return _mm_mul_ps(a, b); }
  static F div(F a, F b) noexcept { return _mm_div_ps(a, b); }
  static F min(F a, F b) noexcept { return _mm_min_ps(a, b); }
  static F max(F a, F b) noexcept { return _mm_max_ps(a, b); }
  static F fma(F a, F b, F c) noexcept { return _mm_fmadd_ps(a, b, c); }
  static F fnma(F a, F b, F c) noexcept { return _mm_fnmadd_ps(a, b, c); }
  static F sqrt(F a) noexcept { return _mm_sqrt_ps(a); }
  static F floor(F a) noexcept { return _mm_round_ps(a, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC); }
  static F round(F a) noexcept { return _mm_round_ps(a, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
  static F abs(F a) noexcept { return _mm_andnot_ps(_mm_set1_ps(-0.0f), a); }
  static F neg(F a) noexcept { return _mm_xor_ps(_mm_set1_ps(-0.0f), a); }

  static F lt(F a, F b) noexcept { return _mm_cmp_ps(a, b, _CMP_LT_OQ); }
  static F le(F a, F b) noexcept { return _mm_cmp_ps(a, b, _CMP_LE_OQ); }
  static F gt(F a, F b) noexcept { return _mm_cmp_ps(a, b, _CMP_GT_OQ); }
  static F ge(F a, F b) noexcept { return _mm_cmp_ps(a, b, _CMP_GE_OQ); }
  static F eq(F a, F b) noexcept { return _mm_cmp_ps(a, b, _CMP_EQ_OQ); }
  static F ne(F a, F b) noexcept { return _mm_cmp_ps(a, b, _CMP_NEQ_UQ); }

  static F band(F a, F b) noexcept { return _mm_and_ps(a, b); }
  static F bor(F a, F b) noexcept { return _mm_or_ps(a, b); }
  static F bxor(F a, F b) noexcept { return _mm_xor_ps(a, b); }
  static F andnot(F a, F b) noexcept { return _mm_andnot_ps(a, b); }
  static F bnot(F a) noexcept { return _mm_xor_ps(a, ones()); }
  static F select(F m, F a, F b) noexcept { return _mm_blendv_ps(b, a, m); }
  static bool any(F m) noexcept { return (_mm_movemask_ps(m) & 1) != 0; }

  static I addi(I a, I b) noexcept { return _mm_add_epi32(a, b); }
  static I subi(I a, I b) noexcept { return _mm_sub_epi32(a, b); }
  static I muli(I a, I b) noexcept { return _mm_mullo_epi32(a, b); }
  static I andi(I a, I b) noexcept { return _mm_and_si128(a, b); }
  static I xori(I a, I b) noexcept { return _mm_xor_si128(a, b); }
  static I mini(I a, I b) noexcept { return _mm_min_epi32(a, b); }
  static I maxi(I a, I b) noexcept { return _mm_max_epi32(a, b); }
  template <int N> static I slli(I a) noexcept { return _mm_slli_epi32(a, N); }
  template <int N> static I srli(I a) noexcept { return _mm_srli_epi32(a, N); }
  template <int N> static I srai(I a) noexcept { return _mm_srai_epi32(a, N); }
  static I eqi(I a, I b) noexcept { return _mm_cmpeq_epi32(a, b); }
  static I gti(I a, I b) noexcept { return _mm_cmpgt_epi32(a, b); }
  static I selecti(I m, I a, I b) noexcept { return _mm_blendv_epi8(b, a, m); }
  static I truncToInt(F a) noexcept { return _mm_cvttps_epi32(a); }
  static I roundToInt(F a) noexcept { return _mm_cvtps_epi32(a); }
  static F toFloat(I a) noexcept { return _mm_cvtepi32_ps(a); }
  static F asF(I a) noexcept { return _mm_castsi128_ps(a); }
  static I asI(F a) noexcept { return _mm_castps_si128(a); }

  static F gather(const float* base, I idx) noexcept { return _mm_load_ss(base + _mm_cvtsi128_si32(idx)); }
  static void gatherPair(const float* base, I idx, F& a, F& b) noexcept {
    const float* p = base + _mm_cvtsi128_si32(idx);
    a = _mm_load_ss(p);
    b = _mm_load_ss(p + 1);
  }

  static I roundMulToInt(F x, double scale) noexcept {
    return _mm_cvtpd_epi32(_mm_mul_sd(_mm_cvtss_sd(_mm_setzero_pd(), x), _mm_set_sd(scale)));
  }

  static F fastExp2(F x) noexcept { return pq::detail::fm::exp2<pq::detail::fm::V4>(x); }
  static F fastLog2(F x) noexcept { return pq::detail::fm::log2<pq::detail::fm::V4>(x); }
  static F fastSin(F x) noexcept { return pq::detail::fm::sin<pq::detail::fm::V4>(x); }
  static F fastTan(F x) noexcept { return pq::detail::fm::tan<pq::detail::fm::V4>(x); }
  static F fastTanh(F x) noexcept { return pq::detail::fm::tanh<pq::detail::fm::V4>(x); }

  static float f0(F x) noexcept { return _mm_cvtss_f32(x); }
  static F exactSin(F x) noexcept { return _mm_set_ss(std::sin(f0(x))); }
  static F exactTan(F x) noexcept { return _mm_set_ss(std::tan(f0(x))); }
  static F exactTanh(F x) noexcept { return _mm_set_ss(std::tanh(f0(x))); }
  static F exactExp(F x) noexcept { return _mm_set_ss(std::exp(f0(x))); }
  static F exactLog(F x) noexcept { return _mm_set_ss(std::log(f0(x))); }
  static F exactPow(F a, F b) noexcept { return _mm_set_ss(std::pow(f0(a), f0(b))); }
  static F exactCentsToHz(F c, F ref) noexcept {
    return _mm_set_ss(static_cast<float>(static_cast<double>(f0(ref)) * std::exp2(static_cast<double>(f0(c)) / 1200.0)));
  }
  static F exactHzToCents(F hz, F ref) noexcept {
    return _mm_set_ss(static_cast<float>(1200.0 * std::log2(static_cast<double>(f0(hz)) / static_cast<double>(f0(ref)))));
  }
  static F exactDbToGain(F db) noexcept {
    return _mm_set_ss(static_cast<float>(std::pow(10.0, static_cast<double>(f0(db)) / 20.0)));
  }
  static F exactGainToDb(F g) noexcept {
    return _mm_set_ss(static_cast<float>(20.0 * std::log10(static_cast<double>(f0(g)))));
  }
};

// Input accessor for template-specialised rates. Spl: the pattern is loaded once (lane 0 for S1, 8 lanes for
// S8). Blk: one load per frame at f * kW.
template <class S, bool kSpl>
struct In {
  using F = typename S::F;
  const float* p;
  F v;
  explicit In(const float* ptr) noexcept : p(ptr), v(kSpl ? S::load(ptr) : S::zero()) {}
  F at(uint32_t f) const noexcept {
    if constexpr (kSpl) {
      return v;
    } else {
      return S::load(p + static_cast<size_t>(f) * S::kW);
    }
  }
};

}  // namespace pq::dsp::simd
