#pragma once

#include <cstdint>

namespace pq::engine {

// Interleaved sample formats a device buffer can use (WASAPI exclusive mode takes whatever the driver accepts).
enum class SampleFormat : uint8_t {
  Float32,   // IEEE float, 4 bytes
  Int32,     // signed 32-bit PCM, 4 bytes
  Int24In32, // 24 valid bits left-justified in a 4-byte container (low byte zero)
  Int24,     // packed signed 24-bit PCM, 3 bytes little-endian
  Int16,     // signed 16-bit PCM, 2 bytes
};

const char* sampleFormatName(SampleFormat f) noexcept;
uint32_t sampleFormatBytes(SampleFormat f) noexcept;  // container bytes per sample

// Writes `frames` interleaved frames of `deviceChannels` channels in format `fmt` to dst from planar float
// channels in[0..numIn). Device channels ≥ numIn are written as silence. Float input is clamped to the integer
// range ([-1, 1) full scale), rounded to nearest (no dither); a NaN sample becomes negative full scale.
// AVX2 for mono and stereo (the common cases), scalar for other channel counts. dst needs no alignment.
// RT-safe.
void planarToInterleaved(const float* const* in, uint32_t numIn, uint32_t deviceChannels, uint32_t frames,
                         SampleFormat fmt, void* dst) noexcept;

}  // namespace pq::engine
