#pragma once

#include <cstdint>

namespace pq::engine {

// Per-channel sample conversion between the engine's planar float and an ASIO driver's channel buffers
// (T0.6-fix2). ASIO buffers are already planar (one buffer per channel), so a conversion is one pass over one
// channel. `type` is an asio::SampleType value (pq/engine/asio_abi.h). Supported (little-endian, the formats
// Windows drivers use): Int16Lsb, Int24Lsb (packed), Int32Lsb, Float32Lsb, Float64Lsb, Int32Lsb16/18/20/24.
//
// Float → integer: x · 2^(N-1), clamped to [-2^(N-1), 2^(N-1) - 1], rounded to nearest-even (no dither); a NaN
// sample becomes negative full scale. Integer → float: x / 2^(N-1). Every loop is AVX2 over 8 samples with a
// scalar tail; buffers need no alignment. Int16/Int24/Int32 output reuse planarToInterleaved (sample_convert.h).

bool asioSampleTypeSupported(int32_t type) noexcept;
const char* asioSampleTypeName(int32_t type) noexcept;  // "Int32LSB", …; "?" if unknown
uint32_t asioSampleTypeBytes(int32_t type) noexcept;     // container bytes per sample; 0 if unsupported

using ToDeviceFn = void (*)(const float* src, uint32_t frames, void* dst) noexcept;
using FromDeviceFn = void (*)(const void* src, uint32_t frames, float* dst) noexcept;

// The converter for a supported type, resolved once when the stream opens (one indirect call per channel per
// buffer on the audio thread); nullptr if the type is unsupported. RT-safe to call the returned functions.
ToDeviceFn asioToDeviceFn(int32_t type) noexcept;
FromDeviceFn asioFromDeviceFn(int32_t type) noexcept;

}  // namespace pq::engine
