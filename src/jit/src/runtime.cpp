// Native helpers called by JIT-compiled code (runtime.h). The special-op kernels repeat the interpreter's
// (src/engine/src/interpreter.cpp, which keeps them private) so both backends have one meaning; the exact
// transcendentals repeat pq_dsp's simd.h S1/S8 expressions (private to pq_dsp) so both call the same CRT/SVML.

#include "runtime.h"

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace pq::jit::rt {

namespace {

inline float horizontalSum(__m256 v) noexcept {  // RT-safe
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_movehdup_ps(s));
  return _mm_cvtss_f32(s);
}

inline __m256 laneMaskVector(uint32_t mask) noexcept {  // RT-safe: bit v → lane v all ones
  const __m256i bits = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
  const __m256i m = _mm256_and_si256(_mm256_set1_epi32(static_cast<int32_t>(mask)), bits);
  return _mm256_castsi256_ps(_mm256_cmpeq_epi32(m, bits));
}

inline bool maskTrue(float x) noexcept { return std::bit_cast<uint32_t>(x) != 0u; }  // RT-safe

inline float reduceFrame(const ReduceInfo& ri, const float* x, float acc, uint32_t mask, uint32_t lastLane) noexcept {
  if (ri.mode == ReduceMode::Last) return lastLane < 8 ? x[lastLane] : acc;
  if (ri.isBool) {
    if (ri.mode == ReduceMode::Lowest) {  // all
      bool all = maskTrue(acc);
      for (uint32_t m = mask; m; m &= m - 1) all = all && maskTrue(x[std::countr_zero(m)]);
      return dsp::boolMask(all);
    }
    bool any = maskTrue(acc);  // sum / highest / loudest: any
    for (uint32_t m = mask; m; m &= m - 1) any = any || maskTrue(x[std::countr_zero(m)]);
    return dsp::boolMask(any);
  }
  switch (ri.mode) {
    case ReduceMode::Sum: return acc + horizontalSum(_mm256_and_ps(_mm256_loadu_ps(x), laneMaskVector(mask)));
    case ReduceMode::Highest:
      for (uint32_t m = mask; m; m &= m - 1) {
        const float v = x[std::countr_zero(m)];
        acc = v > acc ? v : acc;
      }
      return acc;
    case ReduceMode::Lowest:
      for (uint32_t m = mask; m; m &= m - 1) {
        const float v = x[std::countr_zero(m)];
        acc = v < acc ? v : acc;
      }
      return acc;
    case ReduceMode::Loudest:
      for (uint32_t m = mask; m; m &= m - 1) {
        const float v = x[std::countr_zero(m)];
        acc = std::fabs(v) > std::fabs(acc) ? v : acc;
      }
      return acc;
    default: return acc;
  }
}

// ---- exact transcendentals: pq_dsp simd.h S1 (scalar CRT) and S8 (SVML) ------------------------------------
float sinS(float x) noexcept { return std::sin(x); }
float tanS(float x) noexcept { return std::tan(x); }
float tanhS(float x) noexcept { return std::tanh(x); }
float expS(float x) noexcept { return std::exp(x); }
float logS(float x) noexcept { return std::log(x); }
float powS(float a, float b) noexcept { return std::pow(a, b); }
float centsToHzS(float c, float ref) noexcept {
  return static_cast<float>(static_cast<double>(ref) * std::exp2(static_cast<double>(c) / 1200.0));
}
float hzToCentsS(float hz, float ref) noexcept {
  return static_cast<float>(1200.0 * std::log2(static_cast<double>(hz) / static_cast<double>(ref)));
}
float dbToGainS(float db) noexcept { return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0)); }
float gainToDbS(float g) noexcept { return static_cast<float>(20.0 * std::log10(static_cast<double>(g))); }

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

void broadcastKernel(const dsp::KernelArgs& a) noexcept {  // RT-safe
  const float* in = a.in[0];
  float* out = a.out[0];
  if (a.splMask & 1u) {
    const __m256 v = _mm256_set1_ps(in[0]);
    for (uint32_t f = 0; f < a.frames; ++f) _mm256_store_ps(out + f * 8, v);
    return;
  }
  for (uint32_t f = 0; f < a.frames; ++f) _mm256_store_ps(out + f * 8, _mm256_broadcast_ss(in + f));
}

void reduceKernel(const dsp::KernelArgs& a) noexcept {  // RT-safe
  const ReduceInfo& ri = *static_cast<const ReduceInfo*>(a.state);
  const uint32_t mask = ri.run->laneMask;
  const uint32_t last = ri.run->lastLane;
  if (mask == 0 && !(ri.mode == ReduceMode::Last && last < 8)) return;
  const float* in = a.in[0];
  float* out = a.out[0];
  const uint32_t inStride = ri.inPattern ? 0u : 8u;
  if (ri.outPattern) {
    const float v = reduceFrame(ri, in, out[0], mask, last);
    for (uint32_t l = 0; l < 8; ++l) out[l] = v;
    return;
  }
  const float* gain = ri.run->gain;  // per-sample lane gain (VoiceGroupIO::gain), or null
  if (ri.mode == ReduceMode::Sum && !ri.isBool) {
    const __m256 m = laneMaskVector(mask);
    if (gain != nullptr) {  // select(gain ≠ 0, x, 0) · gain: a pre-onset / idle lane's NaN never gets in
      const __m256 z = _mm256_setzero_ps();
      for (uint32_t f = 0; f < a.frames; ++f) {
        const __m256 g = _mm256_load_ps(gain + f * 8);
        const __m256 x = _mm256_and_ps(_mm256_loadu_ps(in + f * inStride), _mm256_and_ps(m, _mm256_cmp_ps(g, z, _CMP_NEQ_OQ)));
        out[f] += horizontalSum(_mm256_mul_ps(x, g));
      }
      return;
    }
    for (uint32_t f = 0; f < a.frames; ++f) out[f] += horizontalSum(_mm256_and_ps(_mm256_loadu_ps(in + f * inStride), m));
    return;
  }
  if (gain != nullptr && ri.mode == ReduceMode::Last) {  // the last voice only from its onset (gain ≠ 0)
    if (last >= 8) return;
    for (uint32_t f = 0; f < a.frames; ++f)
      if (gain[f * 8 + last] != 0.0f) out[f] = in[f * inStride + last];
    return;
  }
  // Highest / Lowest / Loudest and bool any / all keep the block lane mask: their comparisons skip NaN.
  for (uint32_t f = 0; f < a.frames; ++f) out[f] = reduceFrame(ri, in + f * inStride, out[f], mask, last);
}

void sendKernel(const dsp::KernelArgs& a) noexcept {  // RT-safe
  const SendInfo& si = *static_cast<const SendInfo*>(a.state);
  const RunCtx& rc = *si.run;
  if (si.bus >= rc.numSends || rc.sends == nullptr) return;
  const engine::SendBus& bus = rc.sends[si.bus];
  if (si.channel >= bus.numChannels || bus.channels == nullptr || bus.channels[si.channel] == nullptr) return;
  float* dst = bus.channels[si.channel];
  const float* in = a.in[0];
  if (si.lanes == 1) {
    if (si.inPattern) {
      for (uint32_t f = 0; f < a.frames; ++f) dst[f] += in[0];
    } else {
      for (uint32_t f = 0; f < a.frames; ++f) dst[f] += in[f];
    }
    return;
  }
  const __m256 m = laneMaskVector(rc.laneMask);
  const uint32_t stride = si.inPattern ? 0u : 8u;
  if (rc.gain != nullptr) {  // as voice_sum: select(gain ≠ 0, x, 0) · gain
    const __m256 z = _mm256_setzero_ps();
    for (uint32_t f = 0; f < a.frames; ++f) {
      const __m256 g = _mm256_load_ps(rc.gain + f * 8);
      const __m256 x = _mm256_and_ps(_mm256_loadu_ps(in + f * stride), _mm256_and_ps(m, _mm256_cmp_ps(g, z, _CMP_NEQ_OQ)));
      dst[f] += horizontalSum(_mm256_mul_ps(x, g));
    }
    return;
  }
  for (uint32_t f = 0; f < a.frames; ++f) dst[f] += horizontalSum(_mm256_and_ps(_mm256_loadu_ps(in + f * stride), m));
}

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

void newtonLinear(float* u, const float* J, const float* r0, double* M, uint32_t n, uint32_t lanes) noexcept {  // RT-safe
  const uint32_t w = n + 1;
  for (uint32_t l = 0; l < lanes; ++l) {
    for (uint32_t i = 0; i < n; ++i) {
      for (uint32_t j = 0; j < n; ++j) M[i * w + j] = J[(i * n + j) * 8 + l];
      M[i * w + n] = r0[i * 8 + l];
    }
    bool ok = true;
    for (uint32_t c = 0; c < n && ok; ++c) {
      uint32_t piv = c;
      for (uint32_t i = c + 1; i < n; ++i)
        if (std::fabs(M[i * w + c]) > std::fabs(M[piv * w + c])) piv = i;
      if (!(std::fabs(M[piv * w + c]) > 0.0)) {
        ok = false;
        break;
      }
      if (piv != c)
        for (uint32_t j = 0; j < w; ++j) std::swap(M[c * w + j], M[piv * w + j]);
      for (uint32_t i = c + 1; i < n; ++i) {
        const double f = M[i * w + c] / M[c * w + c];
        for (uint32_t j = c; j < w; ++j) M[i * w + j] -= f * M[c * w + j];
      }
    }
    if (!ok) continue;
    for (uint32_t i = n; i-- > 0;) {
      double v = M[i * w + n];
      for (uint32_t j = i + 1; j < n; ++j) v -= M[i * w + j] * M[j * w + n];
      M[i * w + n] = v / M[i * w + i];
    }
    for (uint32_t i = 0; i < n; ++i) {
      const double nu = static_cast<double>(u[i * 8 + l]) - M[i * w + n];
      if (!std::isfinite(nu) || !std::isfinite(static_cast<float>(nu))) ok = false;
    }
    if (!ok) continue;
    for (uint32_t i = 0; i < n; ++i) u[i * 8 + l] = static_cast<float>(static_cast<double>(u[i * 8 + l]) - M[i * w + n]);
  }
}

}  // namespace pq::jit::rt
