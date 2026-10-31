#pragma once

// PCM conversion (T5.1): interleaved file samples → planar float32, in one pass. Used by the PCM decoder, by the
// decoders whose libraries give interleaved output (dr_flac s32, dr_mp3 f32), and by the disk thread when it fills
// stream rings. AVX2 paths for mono and stereo S16 / S24 / S32 / F32 little endian (the common cases); scalar for
// the rest. Integers scale by 2^-(bits-1), exactly (power-of-two scale, no rounding beyond the int → float
// conversion that 32-bit integers need).
//
// RT-safe (no allocation, no I/O), though only non-RT threads call it today.

#include <cstddef>
#include <cstdint>

#include "pq/media/sample_info.h"

namespace pq::media {

// Converts `frames` frames of `channels` interleaved samples at `src` into dst[c][0 .. frames). The source has no
// alignment requirement; it is read exactly frames · channels · pcmBytes(enc) bytes.
void pcmToPlanar(PcmEncoding enc, bool bigEndian, const uint8_t* src, uint32_t channels, std::size_t frames,
                 float* const* dst) noexcept;  // RT-safe

// Interleaved float32 → planar (dr_mp3 output).
void deinterleaveF32(const float* src, uint32_t channels, std::size_t frames, float* const* dst) noexcept;  // RT-safe
// Interleaved left-justified int32 (dr_flac output: a b-bit sample is value << (32 − b)) → planar float, × 2^-31.
void deinterleaveS32(const int32_t* src, uint32_t channels, std::size_t frames, float* const* dst) noexcept;  // RT-safe

}  // namespace pq::media
