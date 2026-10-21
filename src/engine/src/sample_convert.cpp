#include "pq/engine/sample_convert.h"

#include <cstring>

#include <immintrin.h>

namespace pq::engine {

const char* sampleFormatName(SampleFormat f) noexcept {
  switch (f) {
    case SampleFormat::Float32: return "float32";
    case SampleFormat::Int32: return "int32";
    case SampleFormat::Int24In32: return "int24-in-32";
    case SampleFormat::Int24: return "int24";
    case SampleFormat::Int16: return "int16";
  }
  return "?";
}

uint32_t sampleFormatBytes(SampleFormat f) noexcept {
  switch (f) {
    case SampleFormat::Float32:
    case SampleFormat::Int32:
    case SampleFormat::Int24In32: return 4;
    case SampleFormat::Int24: return 3;
    case SampleFormat::Int16: return 2;
  }
  return 0;
}

namespace {

// Quantiser for an integer format: x · scale, clamped to [lo, hi], rounded to nearest (MXCSR mode, i.e. the
// same rounding in the scalar and the AVX2 paths). Comparisons are written so a NaN becomes lo.
struct Quant {
  float scale, lo, hi;
  int shift;  // left shift after rounding (8 for 24-in-32)
};

constexpr Quant quantFor(SampleFormat f) noexcept {
  switch (f) {
    case SampleFormat::Int32: return {2147483648.0f, -2147483648.0f, 2147483520.0f, 0};  // hi: largest float < 2^31
    case SampleFormat::Int24In32: return {8388608.0f, -8388608.0f, 8388607.0f, 8};
    case SampleFormat::Int24: return {8388608.0f, -8388608.0f, 8388607.0f, 0};
    case SampleFormat::Int16: return {32768.0f, -32768.0f, 32767.0f, 0};
    case SampleFormat::Float32: break;
  }
  return {1.0f, -1.0f, 1.0f, 0};
}

inline int32_t quantScalar(float v, const Quant& q) noexcept {  // RT-safe
  float x = v * q.scale;
  x = x > q.lo ? x : q.lo;
  x = x < q.hi ? x : q.hi;
  return _mm_cvtss_si32(_mm_set_ss(x)) << q.shift;
}

inline __m256i quantVec(__m256 v, const Quant& q) noexcept {  // RT-safe
  __m256 x = _mm256_mul_ps(v, _mm256_set1_ps(q.scale));
  x = _mm256_max_ps(x, _mm256_set1_ps(q.lo));  // NaN in the first operand → second operand
  x = _mm256_min_ps(x, _mm256_set1_ps(q.hi));
  const __m256i i = _mm256_cvtps_epi32(x);
  return q.shift ? _mm256_slli_epi32(i, 8) : i;
}

inline void store3(uint8_t* d, int32_t v) noexcept {
  d[0] = static_cast<uint8_t>(v);
  d[1] = static_cast<uint8_t>(v >> 8);
  d[2] = static_cast<uint8_t>(v >> 16);
}

// Packs eight int32 (low 24 bits each) into 24 contiguous bytes at d. Writes exactly 24 bytes.
inline void pack24(uint8_t* d, __m256i v) noexcept {
  const __m256i shuf = _mm256_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1,  //
                                        0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
  const __m256i packed = _mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(v, shuf), _mm256_setr_epi32(0, 1, 2, 4, 5, 6, 3, 7));
  _mm256_maskstore_epi32(reinterpret_cast<int*>(d), _mm256_setr_epi32(-1, -1, -1, -1, -1, -1, 0, 0), packed);
}

// Writes one sample in format F at byte pointer d; returns the pointer past it.
template <SampleFormat F>
inline uint8_t* putScalar(uint8_t* d, float v) noexcept {  // RT-safe
  if constexpr (F == SampleFormat::Float32) {
    std::memcpy(d, &v, 4);
    return d + 4;
  } else {
    constexpr Quant q = quantFor(F);
    const int32_t i = quantScalar(v, q);
    if constexpr (F == SampleFormat::Int16) {
      const int16_t s = static_cast<int16_t>(i);
      std::memcpy(d, &s, 2);
      return d + 2;
    } else if constexpr (F == SampleFormat::Int24) {
      store3(d, i);
      return d + 3;
    } else {
      std::memcpy(d, &i, 4);
      return d + 4;
    }
  }
}

template <SampleFormat F>
void generic(const float* const* in, uint32_t numIn, uint32_t ch, uint32_t from, uint32_t frames, uint8_t* d) noexcept {
  for (uint32_t i = from; i < frames; ++i)
    for (uint32_t c = 0; c < ch; ++c) d = putScalar<F>(d, c < numIn ? in[c][i] : 0.0f);
}

// Stores eight already-quantised samples (in output order) in format F.
template <SampleFormat F>
inline void storeInts(uint8_t* d, __m256i v) noexcept {  // 8 samples
  if constexpr (F == SampleFormat::Int24) {
    pack24(d, v);
  } else if constexpr (F == SampleFormat::Int16) {
    const __m256i p = _mm256_permute4x64_epi64(_mm256_packs_epi32(v, _mm256_setzero_si256()), 0xD8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(d), _mm256_castsi256_si128(p));
  } else {
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), v);
  }
}

template <SampleFormat F>
void mono(const float* l, uint32_t frames, uint8_t* d) noexcept {  // RT-safe
  constexpr uint32_t bytes8 = 8 * (F == SampleFormat::Int24 ? 3u : F == SampleFormat::Int16 ? 2u : 4u);
  uint32_t i = 0;
  if (l) {
    for (; i + 8 <= frames; i += 8, d += bytes8) {
      const __m256 v = _mm256_loadu_ps(l + i);
      if constexpr (F == SampleFormat::Float32)
        _mm256_storeu_ps(reinterpret_cast<float*>(d), v);
      else
        storeInts<F>(d, quantVec(v, quantFor(F)));
    }
  }
  const float* const ins[1] = {l};
  generic<F>(ins, l ? 1u : 0u, 1, i, frames, d);
}

template <SampleFormat F>
void stereo(const float* l, const float* r, uint32_t frames, uint8_t* d) noexcept {  // RT-safe
  constexpr uint32_t bytes8 = 8 * (F == SampleFormat::Int24 ? 3u : F == SampleFormat::Int16 ? 2u : 4u);
  uint32_t i = 0;
  if (l) {
    for (; i + 8 <= frames; i += 8, d += 2 * bytes8) {
      const __m256 lv = _mm256_loadu_ps(l + i);
      const __m256 rv = r ? _mm256_loadu_ps(r + i) : _mm256_setzero_ps();
      if constexpr (F == SampleFormat::Float32) {
        const __m256 a = _mm256_unpacklo_ps(lv, rv);  // L0 R0 L1 R1 | L4 R4 L5 R5
        const __m256 b = _mm256_unpackhi_ps(lv, rv);  // L2 R2 L3 R3 | L6 R6 L7 R7
        _mm256_storeu_ps(reinterpret_cast<float*>(d), _mm256_permute2f128_ps(a, b, 0x20));
        _mm256_storeu_ps(reinterpret_cast<float*>(d) + 8, _mm256_permute2f128_ps(a, b, 0x31));
      } else {
        constexpr Quant q = quantFor(F);
        const __m256i li = quantVec(lv, q);
        const __m256i ri = quantVec(rv, q);
        const __m256i a = _mm256_unpacklo_epi32(li, ri);
        const __m256i b = _mm256_unpackhi_epi32(li, ri);
        storeInts<F>(d, _mm256_permute2x128_si256(a, b, 0x20));
        storeInts<F>(d + bytes8, _mm256_permute2x128_si256(a, b, 0x31));
      }
    }
  }
  const float* const ins[2] = {l, r};
  generic<F>(ins, !l ? 0u : r ? 2u : 1u, 2, i, frames, d);
}

template <SampleFormat F>
void convert(const float* const* in, uint32_t numIn, uint32_t ch, uint32_t frames, uint8_t* d) noexcept {
  if (numIn > ch) numIn = ch;
  if (ch == 2)
    stereo<F>(numIn > 0 ? in[0] : nullptr, numIn > 1 ? in[1] : nullptr, frames, d);
  else if (ch == 1)
    mono<F>(numIn > 0 ? in[0] : nullptr, frames, d);
  else
    generic<F>(in, numIn, ch, 0, frames, d);
}

}  // namespace

void planarToInterleaved(const float* const* in, uint32_t numIn, uint32_t deviceChannels, uint32_t frames,
                         SampleFormat fmt, void* dst) noexcept {  // RT-safe
  auto* d = static_cast<uint8_t*>(dst);
  switch (fmt) {
    case SampleFormat::Float32: convert<SampleFormat::Float32>(in, numIn, deviceChannels, frames, d); break;
    case SampleFormat::Int32: convert<SampleFormat::Int32>(in, numIn, deviceChannels, frames, d); break;
    case SampleFormat::Int24In32: convert<SampleFormat::Int24In32>(in, numIn, deviceChannels, frames, d); break;
    case SampleFormat::Int24: convert<SampleFormat::Int24>(in, numIn, deviceChannels, frames, d); break;
    case SampleFormat::Int16: convert<SampleFormat::Int16>(in, numIn, deviceChannels, frames, d); break;
  }
}

}  // namespace pq::engine
