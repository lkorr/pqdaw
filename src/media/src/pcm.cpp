#include "pq/media/pcm.h"

#include <immintrin.h>

#include <cstring>

namespace pq::media {

namespace {

constexpr float kS8 = 1.0f / 128.0f;
constexpr float kS16 = 1.0f / 32768.0f;
constexpr float kS24 = 1.0f / 8388608.0f;
constexpr float kS32 = 1.0f / 2147483648.0f;

// ---- scalar loads ----------------------------------------------------------------------------------------------
template <PcmEncoding E, bool BE>
inline float load(const uint8_t* p) noexcept {
  if constexpr (E == PcmEncoding::U8) {
    return static_cast<float>(static_cast<int>(p[0]) - 128) * kS8;
  } else if constexpr (E == PcmEncoding::S8) {
    return static_cast<float>(static_cast<int8_t>(p[0])) * kS8;
  } else if constexpr (E == PcmEncoding::S16) {
    const uint32_t u = BE ? (uint32_t{p[0]} << 8 | p[1]) : (uint32_t{p[1]} << 8 | p[0]);
    return static_cast<float>(static_cast<int16_t>(u)) * kS16;
  } else if constexpr (E == PcmEncoding::S24) {
    const uint32_t u = BE ? (uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8)
                          : (uint32_t{p[2]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[0]} << 8);
    return static_cast<float>(static_cast<int32_t>(u)) * kS32;  // (s24 << 8) · 2^-31 = s24 · 2^-23, exact
  } else if constexpr (E == PcmEncoding::S32) {
    uint32_t u;
    std::memcpy(&u, p, 4);
    if constexpr (BE) u = _byteswap_ulong(u);
    return static_cast<float>(static_cast<int32_t>(u)) * kS32;
  } else if constexpr (E == PcmEncoding::F32) {
    uint32_t u;
    std::memcpy(&u, p, 4);
    if constexpr (BE) u = _byteswap_ulong(u);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  } else {
    uint64_t u;
    std::memcpy(&u, p, 8);
    if constexpr (BE) u = _byteswap_uint64(u);
    double d;
    std::memcpy(&d, &u, 8);
    return static_cast<float>(d);
  }
}

template <PcmEncoding E, bool BE>
void scalarFrames(const uint8_t* src, uint32_t channels, std::size_t from, std::size_t to, float* const* dst) noexcept {
  constexpr uint32_t kBytes = E == PcmEncoding::U8 || E == PcmEncoding::S8 ? 1
                              : E == PcmEncoding::S16                      ? 2
                              : E == PcmEncoding::S24                      ? 3
                              : E == PcmEncoding::F64                      ? 8
                                                                           : 4;
  const uint8_t* p = src + from * channels * kBytes;
  for (std::size_t f = from; f < to; ++f)
    for (uint32_t c = 0; c < channels; ++c, p += kBytes) dst[c][f] = load<E, BE>(p);
}

// ---- AVX2 -------------------------------------------------------------------------------------------------------
// Eight interleaved stereo samples a (frames 0..3) and b (frames 4..7) → 8 left, 8 right.
inline void storeStereo(__m256 a, __m256 b, float* l, float* r) noexcept {
  const __m256 lq = _mm256_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0));  // L0 L1 L4 L5 | L2 L3 L6 L7
  const __m256 rq = _mm256_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
  _mm256_storeu_ps(l, _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(lq), _MM_SHUFFLE(3, 1, 2, 0))));
  _mm256_storeu_ps(r, _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(rq), _MM_SHUFFLE(3, 1, 2, 0))));
}

// 8 samples starting at p, as floats. S24 reads 28 bytes (4 past the 24 it converts): callers stop 4 bytes early.
template <PcmEncoding E, bool BE>
inline __m256 load8(const uint8_t* p) noexcept {
  if constexpr (E == PcmEncoding::S16) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    if constexpr (BE) v = _mm_shuffle_epi8(v, _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14));
    return _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(v)), _mm256_set1_ps(kS16));
  } else if constexpr (E == PcmEncoding::S24) {
    const __m256i v = _mm256_loadu2_m128i(reinterpret_cast<const __m128i*>(p + 12), reinterpret_cast<const __m128i*>(p));
    const __m256i mask = BE ? _mm256_setr_epi8(-128, 2, 1, 0, -128, 5, 4, 3, -128, 8, 7, 6, -128, 11, 10, 9,  //
                                               -128, 2, 1, 0, -128, 5, 4, 3, -128, 8, 7, 6, -128, 11, 10, 9)
                            : _mm256_setr_epi8(-128, 0, 1, 2, -128, 3, 4, 5, -128, 6, 7, 8, -128, 9, 10, 11,  //
                                               -128, 0, 1, 2, -128, 3, 4, 5, -128, 6, 7, 8, -128, 9, 10, 11);
    return _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_shuffle_epi8(v, mask)), _mm256_set1_ps(kS32));
  } else if constexpr (E == PcmEncoding::S32) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    if constexpr (BE)
      v = _mm256_shuffle_epi8(v, _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,  //
                                                  3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    return _mm256_mul_ps(_mm256_cvtepi32_ps(v), _mm256_set1_ps(kS32));
  } else {  // F32
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    if constexpr (BE)
      v = _mm256_shuffle_epi8(v, _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,  //
                                                  3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    return _mm256_castsi256_ps(v);
  }
}

template <PcmEncoding E, bool BE>
void simdFrames(const uint8_t* src, uint32_t channels, std::size_t frames, float* const* dst) noexcept {
  constexpr std::size_t kBytes = E == PcmEncoding::S16 ? 2 : E == PcmEncoding::S24 ? 3 : 4;
  constexpr std::size_t kOverread = E == PcmEncoding::S24 ? 4 : 0;
  const std::size_t totalBytes = frames * channels * kBytes;
  std::size_t f = 0;
  if (channels == 1) {
    float* d = dst[0];
    for (; (f + 8) * kBytes + kOverread <= totalBytes; f += 8) _mm256_storeu_ps(d + f, load8<E, BE>(src + f * kBytes));
  } else if (channels == 2) {
    float* l = dst[0];
    float* r = dst[1];
    for (; (f + 8) * 2 * kBytes + kOverread <= totalBytes; f += 8) {
      const uint8_t* p = src + f * 2 * kBytes;
      storeStereo(load8<E, BE>(p), load8<E, BE>(p + 8 * kBytes), l + f, r + f);
    }
  }
  scalarFrames<E, BE>(src, channels, f, frames, dst);
}

template <bool BE>
void dispatch(PcmEncoding enc, const uint8_t* src, uint32_t channels, std::size_t frames, float* const* dst) noexcept {
  switch (enc) {
    case PcmEncoding::U8: scalarFrames<PcmEncoding::U8, BE>(src, channels, 0, frames, dst); break;
    case PcmEncoding::S8: scalarFrames<PcmEncoding::S8, BE>(src, channels, 0, frames, dst); break;
    case PcmEncoding::S16: simdFrames<PcmEncoding::S16, BE>(src, channels, frames, dst); break;
    case PcmEncoding::S24: simdFrames<PcmEncoding::S24, BE>(src, channels, frames, dst); break;
    case PcmEncoding::S32: simdFrames<PcmEncoding::S32, BE>(src, channels, frames, dst); break;
    case PcmEncoding::F32: simdFrames<PcmEncoding::F32, BE>(src, channels, frames, dst); break;
    case PcmEncoding::F64: scalarFrames<PcmEncoding::F64, BE>(src, channels, 0, frames, dst); break;
    case PcmEncoding::None: break;
  }
}

}  // namespace

uint32_t pcmBytes(PcmEncoding e) noexcept {
  switch (e) {
    case PcmEncoding::U8:
    case PcmEncoding::S8: return 1;
    case PcmEncoding::S16: return 2;
    case PcmEncoding::S24: return 3;
    case PcmEncoding::S32:
    case PcmEncoding::F32: return 4;
    case PcmEncoding::F64: return 8;
    case PcmEncoding::None: break;
  }
  return 0;
}

void pcmToPlanar(PcmEncoding enc, bool bigEndian, const uint8_t* src, uint32_t channels, std::size_t frames,
                 float* const* dst) noexcept {
  if (frames == 0 || channels == 0) return;
  if (bigEndian)
    dispatch<true>(enc, src, channels, frames, dst);
  else
    dispatch<false>(enc, src, channels, frames, dst);
}

void deinterleaveF32(const float* src, uint32_t channels, std::size_t frames, float* const* dst) noexcept {
  pcmToPlanar(PcmEncoding::F32, false, reinterpret_cast<const uint8_t*>(src), channels, frames, dst);
}

void deinterleaveS32(const int32_t* src, uint32_t channels, std::size_t frames, float* const* dst) noexcept {
  pcmToPlanar(PcmEncoding::S32, false, reinterpret_cast<const uint8_t*>(src), channels, frames, dst);
}

}  // namespace pq::media
