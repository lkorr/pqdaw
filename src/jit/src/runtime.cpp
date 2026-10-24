// Native helpers called by JIT-compiled code (runtime.h): the exact transcendentals repeat pq_dsp's simd.h S1/S8
// expressions (private to pq_dsp) so both backends call the same SVML functions (Q94: S1 = S8 on lane 0).

#include "runtime.h"

#include <immintrin.h>

#include <cmath>

namespace pq::jit::rt {

namespace {

// ---- exact transcendentals: pq_dsp simd.h S1 and S8, both SVML (Q94: S1 = S8 on lane 0 of a broadcast) -------
inline float lane0(__m256 v) noexcept { return _mm256_cvtss_f32(v); }
inline __m256d wideD(float x) noexcept { return _mm256_set1_pd(static_cast<double>(x)); }
float sinS(float x) noexcept { return lane0(_mm256_sin_ps(_mm256_set1_ps(x))); }
float tanS(float x) noexcept { return lane0(_mm256_tan_ps(_mm256_set1_ps(x))); }
float tanhS(float x) noexcept { return lane0(_mm256_tanh_ps(_mm256_set1_ps(x))); }
float expS(float x) noexcept { return lane0(_mm256_exp_ps(_mm256_set1_ps(x))); }
float logS(float x) noexcept { return lane0(_mm256_log_ps(_mm256_set1_ps(x))); }
float powS(float a, float b) noexcept { return lane0(_mm256_pow_ps(_mm256_set1_ps(a), _mm256_set1_ps(b))); }
float centsToHzS(float c, float ref) noexcept {
  return _mm_cvtss_f32(_mm256_cvtpd_ps(_mm256_mul_pd(wideD(ref), _mm256_exp2_pd(_mm256_div_pd(wideD(c), _mm256_set1_pd(1200.0))))));
}
float hzToCentsS(float hz, float ref) noexcept {
  return _mm_cvtss_f32(_mm256_cvtpd_ps(_mm256_mul_pd(_mm256_set1_pd(1200.0), _mm256_log2_pd(_mm256_div_pd(wideD(hz), wideD(ref))))));
}
float dbToGainS(float db) noexcept {
  return _mm_cvtss_f32(_mm256_cvtpd_ps(_mm256_exp10_pd(_mm256_div_pd(wideD(db), _mm256_set1_pd(20.0)))));
}
float gainToDbS(float g) noexcept {
  return _mm_cvtss_f32(_mm256_cvtpd_ps(_mm256_mul_pd(_mm256_set1_pd(20.0), _mm256_log10_pd(wideD(g)))));
}

template <class Fn>
inline __m256 viaDouble(__m256 x, Fn f) noexcept {
  const __m128 lo = _mm256_cvtpd_ps(f(_mm256_cvtps_pd(_mm256_castps256_ps128(x))));
  const __m128 hi = _mm256_cvtpd_ps(f(_mm256_cvtps_pd(_mm256_extractf128_ps(x, 1))));
  return _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1);
}
template <class Fn>
inline __m256 viaDouble2(__m256 x, __m256 y, Fn f) noexcept {
  const __m128 lo = _mm256_cvtpd_ps(f(_mm256_cvtps_pd(_mm256_castps256_ps128(x)), _mm256_cvtps_pd(_mm256_castps256_ps128(y))));
  const __m128 hi = _mm256_cvtpd_ps(f(_mm256_cvtps_pd(_mm256_extractf128_ps(x, 1)), _mm256_cvtps_pd(_mm256_extractf128_ps(y, 1))));
  return _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1);
}

void sinV(float* io) noexcept { _mm256_storeu_ps(io, _mm256_sin_ps(_mm256_loadu_ps(io))); }
void tanV(float* io) noexcept { _mm256_storeu_ps(io, _mm256_tan_ps(_mm256_loadu_ps(io))); }
void tanhV(float* io) noexcept { _mm256_storeu_ps(io, _mm256_tanh_ps(_mm256_loadu_ps(io))); }
void expV(float* io) noexcept { _mm256_storeu_ps(io, _mm256_exp_ps(_mm256_loadu_ps(io))); }
void logV(float* io) noexcept { _mm256_storeu_ps(io, _mm256_log_ps(_mm256_loadu_ps(io))); }
void powV(float* io) noexcept { _mm256_storeu_ps(io, _mm256_pow_ps(_mm256_loadu_ps(io), _mm256_loadu_ps(io + 8))); }
void centsToHzV(float* io) noexcept {
  _mm256_storeu_ps(io, viaDouble2(_mm256_loadu_ps(io), _mm256_loadu_ps(io + 8), [](__m256d cd, __m256d rd) {
                     return _mm256_mul_pd(rd, _mm256_exp2_pd(_mm256_div_pd(cd, _mm256_set1_pd(1200.0))));
                   }));
}
void hzToCentsV(float* io) noexcept {
  _mm256_storeu_ps(io, viaDouble2(_mm256_loadu_ps(io), _mm256_loadu_ps(io + 8), [](__m256d hd, __m256d rd) {
                     return _mm256_mul_pd(_mm256_set1_pd(1200.0), _mm256_log2_pd(_mm256_div_pd(hd, rd)));
                   }));
}
void dbToGainV(float* io) noexcept {
  _mm256_storeu_ps(io, viaDouble(_mm256_loadu_ps(io), [](__m256d d) {
                     return _mm256_exp10_pd(_mm256_div_pd(d, _mm256_set1_pd(20.0)));
                   }));
}
void gainToDbV(float* io) noexcept {
  _mm256_storeu_ps(io, viaDouble(_mm256_loadu_ps(io), [](__m256d d) {
                     return _mm256_mul_pd(_mm256_set1_pd(20.0), _mm256_log10_pd(d));
                   }));
}

using Unary = float (*)(float) noexcept;
using Binary = float (*)(float, float) noexcept;
using Vector = void (*)(float*) noexcept;

}  // namespace

const void* exactScalar(Exact e) noexcept {  // RT-safe
  switch (e) {
    case Exact::Sin: return reinterpret_cast<const void*>(static_cast<Unary>(&sinS));
    case Exact::Tan: return reinterpret_cast<const void*>(static_cast<Unary>(&tanS));
    case Exact::Tanh: return reinterpret_cast<const void*>(static_cast<Unary>(&tanhS));
    case Exact::Exp: return reinterpret_cast<const void*>(static_cast<Unary>(&expS));
    case Exact::Log: return reinterpret_cast<const void*>(static_cast<Unary>(&logS));
    case Exact::Pow: return reinterpret_cast<const void*>(static_cast<Binary>(&powS));
    case Exact::CentsToHz: return reinterpret_cast<const void*>(static_cast<Binary>(&centsToHzS));
    case Exact::HzToCents: return reinterpret_cast<const void*>(static_cast<Binary>(&hzToCentsS));
    case Exact::DbToGain: return reinterpret_cast<const void*>(static_cast<Unary>(&dbToGainS));
    case Exact::GainToDb: return reinterpret_cast<const void*>(static_cast<Unary>(&gainToDbS));
    default: return nullptr;
  }
}

const void* exactVector(Exact e) noexcept {  // RT-safe
  switch (e) {
    case Exact::Sin: return reinterpret_cast<const void*>(static_cast<Vector>(&sinV));
    case Exact::Tan: return reinterpret_cast<const void*>(static_cast<Vector>(&tanV));
    case Exact::Tanh: return reinterpret_cast<const void*>(static_cast<Vector>(&tanhV));
    case Exact::Exp: return reinterpret_cast<const void*>(static_cast<Vector>(&expV));
    case Exact::Log: return reinterpret_cast<const void*>(static_cast<Vector>(&logV));
    case Exact::Pow: return reinterpret_cast<const void*>(static_cast<Vector>(&powV));
    case Exact::CentsToHz: return reinterpret_cast<const void*>(static_cast<Vector>(&centsToHzV));
    case Exact::HzToCents: return reinterpret_cast<const void*>(static_cast<Vector>(&hzToCentsV));
    case Exact::DbToGain: return reinterpret_cast<const void*>(static_cast<Vector>(&dbToGainV));
    case Exact::GainToDb: return reinterpret_cast<const void*>(static_cast<Vector>(&gainToDbV));
    default: return nullptr;
  }
}

}  // namespace pq::jit::rt
