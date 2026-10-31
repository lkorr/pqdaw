#pragma once

// Content hashes of audio files (T5.1). Not cryptographic; collisions only cost a redundant copy or a wrong cache
// hit, and every key also carries the file size.
//   contentHash: fast identity of a file's content for caches (decoded / waveform caches, the browser): the size
//                plus three 64 KiB windows (start, middle, end). Reads ≤ 192 KiB whatever the file size.
//   fullContentHash: every byte, 128 bits, for deduplication when collecting samples into a project.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "pq/core/result.h"

namespace pq::media {

struct Hash128 {
  uint64_t lo = 0;
  uint64_t hi = 0;
  friend bool operator==(const Hash128&, const Hash128&) = default;
};

inline constexpr std::size_t kContentHashWindow = 64u << 10;

// 64-bit hash of a byte range (4 independent multiply-xorshift lanes; ~10 GB/s). Pure function, RT-safe.
uint64_t hashBytes(const void* data, std::size_t size, uint64_t seed) noexcept;

Result<uint64_t> contentHash(const std::filesystem::path& path);
// The same on a whole file in memory (identical result to contentHash of that file).
uint64_t contentHashBytes(std::span<const uint8_t> file) noexcept;
Result<Hash128> fullContentHash(const std::filesystem::path& path);
Hash128 fullContentHashBytes(std::span<const uint8_t> file) noexcept;

}  // namespace pq::media
