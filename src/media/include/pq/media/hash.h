#pragma once

// The content hash of a sample file (T5.1). It is the project's one sample content identity: bit for bit the hash of
// doc::fileIdentity (pq/document/samples.h, T5.3/T5.8; QUESTIONS Q145 item 8), so the hashes the document stores,
// the sample pool's keys and the decoded-cache file names agree. `media` cannot include `document` (it sits below),
// so the definition is repeated here and a test checks both against each other:
//   FNV-1a 64 over the file size (8 bytes, little endian), then the bytes of three 64 KiB windows: the start
//   (the whole file when it is ≤ 64 KiB, then only that window), the middle ((size − 64 KiB) / 2) and the end
//   (size − 64 KiB). Reads ≤ 192 KiB whatever the file size. Not cryptographic: keys also carry the size.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "pq/core/result.h"

namespace pq::media {

inline constexpr std::size_t kContentHashWindow = 64u << 10;

Result<uint64_t> contentHash(const std::filesystem::path& path);
// The same on a whole file in memory (identical result to contentHash of that file).
uint64_t contentHashBytes(std::span<const uint8_t> file) noexcept;

}  // namespace pq::media
