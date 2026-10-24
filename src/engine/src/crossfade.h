#pragma once

// Engine-internal (T1.9, shared with T1.x-arch1's per-unit swap): the hot-swap crossfade and the output peak.

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace pq::engine::detail {

// dst = a + g · (b − a), g = (pos + f + 1) / len per frame f; exactly b from g = 1 on. Bit-exact where a == b.
inline void crossfade(float* dst, const float* a, const float* b, uint32_t n, uint32_t pos, uint32_t len) noexcept {  // RT-safe
  const float inv = 1.0f / static_cast<float>(len);
  const uint32_t ramp = pos >= len ? 0u : std::min(n, len - 1 - std::min(len - 1, pos));  // frames with g < 1
  uint32_t f = 0;
  const __m256 vinv = _mm256_set1_ps(inv);
  const __m256i step = _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 8);
  for (; f + 8 <= ramp; f += 8) {
    const __m256i k = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int>(pos + f)), step);
    const __m256 g = _mm256_mul_ps(_mm256_cvtepi32_ps(k), vinv);
    const __m256 va = _mm256_loadu_ps(a + f), vb = _mm256_loadu_ps(b + f);
    _mm256_storeu_ps(dst + f, _mm256_add_ps(va, _mm256_mul_ps(g, _mm256_sub_ps(vb, va))));
  }
  for (; f < ramp; ++f) {
    const float g = static_cast<float>(pos + f + 1) * inv;
    dst[f] = a[f] + g * (b[f] - a[f]);
  }
  if (f < n && dst != b) std::memcpy(dst + f, b + f, sizeof(float) * (n - f));  // dst == b: in place
}

inline float peakAbs(const float* x, uint32_t n) noexcept {  // RT-safe
  const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
  __m256 m = _mm256_setzero_ps();
  uint32_t f = 0;
  for (; f + 8 <= n; f += 8) m = _mm256_max_ps(m, _mm256_and_ps(_mm256_loadu_ps(x + f), absMask));
  alignas(32) float t[8];
  _mm256_store_ps(t, m);
  float p = 0.0f;
  for (float v : t) p = std::max(p, v);
  for (; f < n; ++f) p = std::max(p, std::fabs(x[f]));
  return p;
}

}  // namespace pq::engine::detail
