#include "pq/engine/asio_convert.h"

#include <cstring>

#include <immintrin.h>

#include "pq/engine/asio_abi.h"
#include "pq/engine/sample_convert.h"

namespace pq::engine {

namespace {

using namespace asio;

// --- float → device ---------------------------------------------------------------------------------------------

void toFloat32(const float* src, uint32_t frames, void* dst) noexcept {  // RT-safe
  std::memcpy(dst, src, sizeof(float) * frames);
}

// Int16/packed Int24/Int32 are the mono case of the WASAPI converter (AVX2, same rounding and clamping).
template <SampleFormat F>
void toPcm(const float* src, uint32_t frames, void* dst) noexcept {  // RT-safe
  const float* const in[1] = {src};
  planarToInterleaved(in, 1, 1, frames, F, dst);
}

void toFloat64(const float* src, uint32_t frames, void* dst) noexcept {  // RT-safe
  auto* d = static_cast<double*>(dst);
  uint32_t i = 0;
  for (; i + 8 <= frames; i += 8) {
    const __m256 v = _mm256_loadu_ps(src + i);
    _mm256_storeu_pd(d + i, _mm256_cvtps_pd(_mm256_castps256_ps128(v)));
    _mm256_storeu_pd(d + i + 4, _mm256_cvtps_pd(_mm256_extractf128_ps(v, 1)));
  }
  for (; i < frames; ++i) d[i] = src[i];
}

// N-bit sample right-justified in an int32 container (Int32Lsb16/18/20/24).
template <int Bits>
void toInt32Low(const float* src, uint32_t frames, void* dst) noexcept {  // RT-safe
  constexpr float scale = static_cast<float>(1u << (Bits - 1));
  constexpr float lo = -scale;
  constexpr float hi = scale - 1.0f;
  auto* d = static_cast<uint8_t*>(dst);
  uint32_t i = 0;
  const __m256 vs = _mm256_set1_ps(scale), vlo = _mm256_set1_ps(lo), vhi = _mm256_set1_ps(hi);
  for (; i + 8 <= frames; i += 8) {
    __m256 x = _mm256_mul_ps(_mm256_loadu_ps(src + i), vs);
    x = _mm256_max_ps(x, vlo);  // NaN in the first operand → the second (negative full scale)
    x = _mm256_min_ps(x, vhi);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 4 * i), _mm256_cvtps_epi32(x));
  }
  for (; i < frames; ++i) {
    float x = src[i] * scale;
    x = x > lo ? x : lo;
    x = x < hi ? x : hi;
    const int32_t v = _mm_cvtss_si32(_mm_set_ss(x));
    std::memcpy(d + 4 * i, &v, 4);
  }
}

// --- device → float ---------------------------------------------------------------------------------------------

void fromFloat32(const void* src, uint32_t frames, float* dst) noexcept {  // RT-safe
  std::memcpy(dst, src, sizeof(float) * frames);
}

void fromFloat64(const void* src, uint32_t frames, float* dst) noexcept {  // RT-safe
  const auto* s = static_cast<const double*>(src);
  uint32_t i = 0;
  for (; i + 8 <= frames; i += 8) {
    const __m128 a = _mm256_cvtpd_ps(_mm256_loadu_pd(s + i));
    const __m128 b = _mm256_cvtpd_ps(_mm256_loadu_pd(s + i + 4));
    _mm256_storeu_ps(dst + i, _mm256_insertf128_ps(_mm256_castps128_ps256(a), b, 1));
  }
  for (; i < frames; ++i) dst[i] = static_cast<float>(s[i]);
}

// Int32 container holding a sign-extended N-bit value (N = 32 for Int32Lsb).
template <int Bits>
void fromInt32(const void* src, uint32_t frames, float* dst) noexcept {  // RT-safe
  constexpr float k = 1.0f / static_cast<float>(1ull << (Bits - 1));
  const auto* s = static_cast<const uint8_t*>(src);
  const __m256 vk = _mm256_set1_ps(k);
  uint32_t i = 0;
  for (; i + 8 <= frames; i += 8) {
    const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 4 * i));
    _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_cvtepi32_ps(v), vk));
  }
  for (; i < frames; ++i) {
    int32_t v;
    std::memcpy(&v, s + 4 * i, 4);
    dst[i] = static_cast<float>(v) * k;
  }
}

void fromInt16(const void* src, uint32_t frames, float* dst) noexcept {  // RT-safe
  constexpr float k = 1.0f / 32768.0f;
  const auto* s = static_cast<const uint8_t*>(src);
  const __m256 vk = _mm256_set1_ps(k);
  uint32_t i = 0;
  for (; i + 8 <= frames; i += 8) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 2 * i));
    _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(v)), vk));
  }
  for (; i < frames; ++i) {
    int16_t v;
    std::memcpy(&v, s + 2 * i, 2);
    dst[i] = static_cast<float>(v) * k;
  }
}

// Packed 24-bit: 8 samples = 24 bytes, read as two overlapping 16-byte loads (bytes 0..15 and 8..23, so nothing
// past the 24 bytes is touched). Each sample's 3 bytes go to the top of an int32 lane; an arithmetic shift right
// by 8 sign-extends.
void fromInt24(const void* src, uint32_t frames, float* dst) noexcept {  // RT-safe
  constexpr float k = 1.0f / 8388608.0f;
  const auto* s = static_cast<const uint8_t*>(src);
  const __m128i shufLo = _mm_setr_epi8(-1, 0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11);
  const __m128i shufHi = _mm_setr_epi8(-1, 4, 5, 6, -1, 7, 8, 9, -1, 10, 11, 12, -1, 13, 14, 15);
  const __m256 vk = _mm256_set1_ps(k);
  uint32_t i = 0;
  for (; i + 8 <= frames; i += 8) {
    const uint8_t* p = s + 3 * i;
    const __m128i a = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), shufLo);
    const __m128i b = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 8)), shufHi);
    const __m256i v = _mm256_srai_epi32(_mm256_inserti128_si256(_mm256_castsi128_si256(a), b, 1), 8);
    _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_cvtepi32_ps(v), vk));
  }
  for (; i < frames; ++i) {
    const uint8_t* p = s + 3 * i;
    const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                                           (static_cast<uint32_t>(p[2]) << 24)) >> 8;
    dst[i] = static_cast<float>(v) * k;
  }
}

}  // namespace

bool asioSampleTypeSupported(int32_t type) noexcept { return asioToDeviceFn(type) != nullptr; }

const char* asioSampleTypeName(int32_t type) noexcept {
  switch (type) {
    case kInt16Msb: return "Int16MSB";
    case kInt24Msb: return "Int24MSB";
    case kInt32Msb: return "Int32MSB";
    case kFloat32Msb: return "Float32MSB";
    case kFloat64Msb: return "Float64MSB";
    case kInt32Msb16: return "Int32MSB16";
    case kInt32Msb18: return "Int32MSB18";
    case kInt32Msb20: return "Int32MSB20";
    case kInt32Msb24: return "Int32MSB24";
    case kInt16Lsb: return "Int16LSB";
    case kInt24Lsb: return "Int24LSB";
    case kInt32Lsb: return "Int32LSB";
    case kFloat32Lsb: return "Float32LSB";
    case kFloat64Lsb: return "Float64LSB";
    case kInt32Lsb16: return "Int32LSB16";
    case kInt32Lsb18: return "Int32LSB18";
    case kInt32Lsb20: return "Int32LSB20";
    case kInt32Lsb24: return "Int32LSB24";
    case kDsdInt8Lsb1: return "DSDInt8LSB1";
    case kDsdInt8Msb1: return "DSDInt8MSB1";
    case kDsdInt8Ner8: return "DSDInt8NER8";
    default: return "?";
  }
}

uint32_t asioSampleTypeBytes(int32_t type) noexcept {
  switch (type) {
    case kInt16Lsb: return 2;
    case kInt24Lsb: return 3;
    case kInt32Lsb:
    case kFloat32Lsb:
    case kInt32Lsb16:
    case kInt32Lsb18:
    case kInt32Lsb20:
    case kInt32Lsb24: return 4;
    case kFloat64Lsb: return 8;
    default: return 0;
  }
}

ToDeviceFn asioToDeviceFn(int32_t type) noexcept {
  switch (type) {
    case kInt16Lsb: return &toPcm<SampleFormat::Int16>;
    case kInt24Lsb: return &toPcm<SampleFormat::Int24>;
    case kInt32Lsb: return &toPcm<SampleFormat::Int32>;
    case kFloat32Lsb: return &toFloat32;
    case kFloat64Lsb: return &toFloat64;
    case kInt32Lsb16: return &toInt32Low<16>;
    case kInt32Lsb18: return &toInt32Low<18>;
    case kInt32Lsb20: return &toInt32Low<20>;
    case kInt32Lsb24: return &toInt32Low<24>;
    default: return nullptr;
  }
}

FromDeviceFn asioFromDeviceFn(int32_t type) noexcept {
  switch (type) {
    case kInt16Lsb: return &fromInt16;
    case kInt24Lsb: return &fromInt24;
    case kInt32Lsb: return &fromInt32<32>;
    case kFloat32Lsb: return &fromFloat32;
    case kFloat64Lsb: return &fromFloat64;
    case kInt32Lsb16: return &fromInt32<16>;
    case kInt32Lsb18: return &fromInt32<18>;
    case kInt32Lsb20: return &fromInt32<20>;
    case kInt32Lsb24: return &fromInt32<24>;
    default: return nullptr;
  }
}

}  // namespace pq::engine
