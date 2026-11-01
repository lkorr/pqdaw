#pragma once

// Private to pq_dsp: the frame-table read steps shared by the TMM.1 readers (frame_table_read.cpp) and the TMM.8 live
// kernels (kernels_wave_live.cpp): the 6 samples of 8 lanes as row vectors, the 6-point polynomial, and the split of
// a phase into a row index and fraction. Written once for 8 lanes (AVX2) and once for lane 0 of an SSE register (the
// live kernels' 1-lane path), with the same operations in the same order, so a lane of the 8-lane form and the 1-lane
// form give the same bits.

#include <immintrin.h>

#include <cstdint>

#include "frame_table_ops.h"
#include "pq/dsp/frame_table.h"

namespace pq::dsp::detail::ft {

inline constexpr uint32_t kInfoOffsetMask = (1u << 17) - 1;  // packed level info: offset (bits 0–16) | len << 17

// Rows y_{−2} … y_3 (r[0..5]) of the 8 lanes whose sample i − 2 sits at d + idx[l].
__forceinline void rows6(const float* d, __m256i idx, __m256 (&r)[6]) noexcept {  // RT-safe
  // The 8 indices through memory (one store, 8 scalar loads on the load ports) rather than 8 extracts, which would
  // compete with the transpose for port 5.
  int32_t ix[8];
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(ix), idx);
  __m256 a[4], b[4];
  for (int m = 0; m < 4; ++m) {
    const float* lo = d + ix[m];
    const float* hi = d + ix[m + 4];
    a[m] = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_loadu_ps(lo)), _mm_loadu_ps(hi), 1);
    b[m] = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_loadu_ps(lo + 4)), _mm_loadu_ps(hi + 4), 1);
  }
  // a[m] = [lane m: y−2 y−1 y0 y1 | lane m+4: …]; transpose 4×4 inside each half.
  const __m256 t0 = _mm256_unpacklo_ps(a[0], a[1]), t1 = _mm256_unpacklo_ps(a[2], a[3]);
  const __m256 t2 = _mm256_unpackhi_ps(a[0], a[1]), t3 = _mm256_unpackhi_ps(a[2], a[3]);
  r[0] = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(1, 0, 1, 0));
  r[1] = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(3, 2, 3, 2));
  r[2] = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(1, 0, 1, 0));
  r[3] = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(3, 2, 3, 2));
  const __m256 u0 = _mm256_unpacklo_ps(b[0], b[1]), u1 = _mm256_unpacklo_ps(b[2], b[3]);
  r[4] = _mm256_shuffle_ps(u0, u1, _MM_SHUFFLE(1, 0, 1, 0));
  r[5] = _mm256_shuffle_ps(u0, u1, _MM_SHUFFLE(3, 2, 3, 2));
}

// Lane 0 only: y_{−2} … y_3 at d + idx.
__forceinline void rows6(const float* d, __m128i idx, __m128 (&r)[6]) noexcept {  // RT-safe
  const float* p = d + _mm_cvtsi128_si32(idx);
  for (int q = 0; q < 6; ++q) r[q] = _mm_load_ss(p + q);
}

__forceinline __m256 coef(int n, __m256 a0, __m256 a1, __m256 a2) noexcept {
  return _mm256_fmadd_ps(_mm256_set1_ps(kInterp[2][n]), a2,
                         _mm256_fmadd_ps(_mm256_set1_ps(kInterp[1][n]), a1, _mm256_mul_ps(_mm256_set1_ps(kInterp[0][n]), a0)));
}
__forceinline __m128 coef(int n, __m128 a0, __m128 a1, __m128 a2) noexcept {
  return _mm_fmadd_ps(_mm_set1_ps(kInterp[2][n]), a2,
                      _mm_fmadd_ps(_mm_set1_ps(kInterp[1][n]), a1, _mm_mul_ps(_mm_set1_ps(kInterp[0][n]), a0)));
}

// The polynomial on rows r (y_{q−2} = r[q]) at fraction x ∈ [0, 1).
__forceinline __m256 poly6(const __m256 (&r)[6], __m256 x) noexcept {  // RT-safe
  const __m256 e0 = _mm256_add_ps(r[2], r[3]), o0 = _mm256_sub_ps(r[2], r[3]);
  const __m256 e1 = _mm256_add_ps(r[1], r[4]), o1 = _mm256_sub_ps(r[1], r[4]);
  const __m256 e2 = _mm256_add_ps(r[0], r[5]), o2 = _mm256_sub_ps(r[0], r[5]);
  const __m256 z = _mm256_sub_ps(x, _mm256_set1_ps(0.5f));
  __m256 acc = coef(5, o0, o1, o2);
  acc = _mm256_fmadd_ps(acc, z, coef(4, e0, e1, e2));
  acc = _mm256_fmadd_ps(acc, z, coef(3, o0, o1, o2));
  acc = _mm256_fmadd_ps(acc, z, coef(2, e0, e1, e2));
  acc = _mm256_fmadd_ps(acc, z, coef(1, o0, o1, o2));
  return _mm256_fmadd_ps(acc, z, coef(0, e0, e1, e2));
}
__forceinline __m128 poly6(const __m128 (&r)[6], __m128 x) noexcept {  // RT-safe
  const __m128 e0 = _mm_add_ps(r[2], r[3]), o0 = _mm_sub_ps(r[2], r[3]);
  const __m128 e1 = _mm_add_ps(r[1], r[4]), o1 = _mm_sub_ps(r[1], r[4]);
  const __m128 e2 = _mm_add_ps(r[0], r[5]), o2 = _mm_sub_ps(r[0], r[5]);
  const __m128 z = _mm_sub_ps(x, _mm_set1_ps(0.5f));
  __m128 acc = coef(5, o0, o1, o2);
  acc = _mm_fmadd_ps(acc, z, coef(4, e0, e1, e2));
  acc = _mm_fmadd_ps(acc, z, coef(3, o0, o1, o2));
  acc = _mm_fmadd_ps(acc, z, coef(2, e0, e1, e2));
  acc = _mm_fmadd_ps(acc, z, coef(1, o0, o1, o2));
  return _mm_fmadd_ps(acc, z, coef(0, e0, e1, e2));
}

// Row index (of sample i − 2, relative to the frame's start) and fraction of phase p in the level with packed info
// `info`; frameOff is added to the index (k·frameStride). frac(p); ±inf / NaN give NaN, which max turns into 0.
template <class S>
__forceinline void splitPhase(typename S::I info, typename S::F p, typename S::I frameOff, typename S::I& idx,
                              typename S::F& x) noexcept {  // RT-safe
  const auto off = S::andi(info, S::seti(static_cast<int32_t>(kInfoOffsetMask)));
  const auto len = S::toFloat(S::template srli<17>(info));
  const auto f = S::max(S::sub(p, S::floor(p)), S::zero());
  const auto pos = S::mul(f, len);
  const auto err = S::fma(f, len, S::neg(pos));  // f·len − pos, exact
  const auto fi = S::truncToInt(pos);            // pos ≥ 0: truncation is the floor
  x = S::add(S::sub(pos, S::toFloat(fi)), err);
  idx = S::addi(S::addi(off, frameOff), S::subi(fi, S::seti(2)));
}

}  // namespace pq::dsp::detail::ft
