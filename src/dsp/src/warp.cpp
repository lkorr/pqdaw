// Kernels of the time-stretch warp modes (T6.3): pq/dsp/warp.h.
#include "pq/dsp/warp.h"

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pq::dsp {

void warpWindowGain(const float* win, const float* env, float gain, float* out, uint32_t n) noexcept {  // RT-safe
  uint32_t i = 0;
  if (env != nullptr) {
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(out + i, _mm256_mul_ps(_mm256_loadu_ps(win + i), _mm256_loadu_ps(env + i)));
    for (; i < n; ++i) out[i] = win[i] * env[i];
  } else {
    const __m256 g = _mm256_set1_ps(gain);
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(out + i, _mm256_mul_ps(_mm256_loadu_ps(win + i), g));
    for (; i < n; ++i) out[i] = win[i] * gain;
  }
}

namespace {

inline float clamp01(float v) noexcept { return std::min(1.0f, std::max(0.0f, v)); }

}  // namespace

float rampValue(float base, float dg, int32_t j) noexcept {  // RT-safe; the same fused arithmetic as the kernels
  const __m128 v = _mm_fmadd_ss(_mm_set_ss(static_cast<float>(j)), _mm_set_ss(dg), _mm_set_ss(base));
  return clamp01(_mm_cvtss_f32(v));
}

void warpRampGain(float base, float dg, int32_t k, const float* env, float gain, float* out, uint32_t n) noexcept {  // RT-safe
  const __m256 zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1.0f);
  const __m256 step = _mm256_set1_ps(dg), b = _mm256_set1_ps(base), g8 = _mm256_set1_ps(gain);
  const __m256i idx = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256 j = _mm256_cvtepi32_ps(_mm256_add_epi32(idx, _mm256_set1_epi32(k + static_cast<int32_t>(i))));
    const __m256 r = _mm256_min_ps(one, _mm256_max_ps(zero, _mm256_fmadd_ps(j, step, b)));
    _mm256_storeu_ps(out + i, _mm256_mul_ps(r, env != nullptr ? _mm256_loadu_ps(env + i) : g8));
  }
  for (; i < n; ++i) out[i] = rampValue(base, dg, k + static_cast<int32_t>(i)) * (env != nullptr ? env[i] : gain);
}

void warpRampMul(float base, float dg, int32_t k, float* out, uint32_t n) noexcept {  // RT-safe
  const __m256 zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1.0f);
  const __m256 step = _mm256_set1_ps(dg), b = _mm256_set1_ps(base);
  const __m256i idx = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256 j = _mm256_cvtepi32_ps(_mm256_add_epi32(idx, _mm256_set1_epi32(k + static_cast<int32_t>(i))));
    const __m256 r = _mm256_min_ps(one, _mm256_max_ps(zero, _mm256_fmadd_ps(j, step, b)));
    _mm256_storeu_ps(out + i, _mm256_mul_ps(r, _mm256_loadu_ps(out + i)));
  }
  for (; i < n; ++i) out[i] *= rampValue(base, dg, k + static_cast<int32_t>(i));
}

std::vector<float> hannWindow(uint32_t n) {
  std::vector<float> w(n);
  for (uint32_t j = 0; j < n; ++j)
    w[j] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(j) / static_cast<double>(n)));
  return w;
}

double warpJitter(uint32_t seed, int64_t k) noexcept {  // RT-safe
  // splitmix64 of (seed, k): well mixed, platform independent.
  uint64_t z = (static_cast<uint64_t>(seed) << 32) ^ static_cast<uint64_t>(k) ^ 0x9E3779B97F4A7C15ull;
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}

void warpMonoDecimate(const float* const* in, uint32_t channels, int64_t first, uint32_t stride, uint32_t count,
                      float* out) noexcept {  // RT-safe
  const float* a = in[0] + first;
  if (channels >= 2) {
    const float* b = in[1] + first;
    for (uint32_t i = 0; i < count; ++i) out[i] = a[static_cast<size_t>(i) * stride] + b[static_cast<size_t>(i) * stride];
  } else {
    for (uint32_t i = 0; i < count; ++i) out[i] = a[static_cast<size_t>(i) * stride];
  }
}

namespace {

inline float dot(const float* a, const float* b, uint32_t n) noexcept {  // RT-safe
  __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps();
  uint32_t i = 0;
  for (; i + 16 <= n; i += 16) {
    s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
    s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), s1);
  }
  for (; i + 8 <= n; i += 8) s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
  s0 = _mm256_add_ps(s0, s1);
  const __m128 h = _mm_add_ps(_mm256_castps256_ps128(s0), _mm256_extractf128_ps(s0, 1));
  const __m128 h2 = _mm_add_ps(h, _mm_movehl_ps(h, h));
  float s = _mm_cvtss_f32(_mm_add_ss(h2, _mm_shuffle_ps(h2, h2, 1)));
  for (; i < n; ++i) s += a[i] * b[i];
  return s;
}

}  // namespace

uint32_t warpBestLag(const float* ref, const float* cand, uint32_t len, uint32_t lags, uint32_t prefer,
                     double* frac) noexcept {  // RT-safe
  if (frac != nullptr) *frac = 0.0;
  if (lags == 0) return 0;
  double e = 0.0;
  for (uint32_t i = 0; i < len; ++i) e += static_cast<double>(cand[i]) * cand[i];
  uint32_t best = std::min(prefer, lags - 1);
  double bestScore = -1e300;
  bool any = false;
  for (uint32_t l = 0; l < lags; ++l) {
    if (l > 0) {
      const double o = cand[l - 1], in = cand[l + len - 1];
      e = std::max(0.0, e - o * o + in * in);
    }
    const double score = e > 1e-20 ? static_cast<double>(dot(ref, cand + l, len)) / std::sqrt(e) : 0.0;
    const auto dist = [&](uint32_t x) { return x > prefer ? x - prefer : prefer - x; };
    if (!any || score > bestScore || (score == bestScore && dist(l) < dist(best))) {
      best = l;
      bestScore = score;
      any = true;
    }
  }
  if (frac != nullptr && best > 0 && best + 1 < lags) {
    auto scoreAt = [&](uint32_t l) noexcept {
      double en = 0.0;
      for (uint32_t i = 0; i < len; ++i) en += static_cast<double>(cand[l + i]) * cand[l + i];
      return en > 1e-20 ? static_cast<double>(dot(ref, cand + l, len)) / std::sqrt(en) : 0.0;
    };
    const double a = scoreAt(best - 1), b = scoreAt(best), c = scoreAt(best + 1);
    const double den = a - 2.0 * b + c;
    if (den < 0.0) *frac = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
  }
  return best;
}

}  // namespace pq::dsp
