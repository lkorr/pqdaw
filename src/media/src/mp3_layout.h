#pragma once

// Where an MP3 file's frames are (shared by the probe and the MP3 decoder; implemented in probe.cpp).

#include <cstdint>
#include <span>

namespace pq::media::detail {

// Offset after any leading ID3v2 tags (0 if none).
uint64_t skipId3(std::span<const uint8_t> file) noexcept;
// End of the audio frames: before a trailing ID3v1 tag and/or APEv2 tag.
uint64_t mp3End(std::span<const uint8_t> file) noexcept;

}  // namespace pq::media::detail
