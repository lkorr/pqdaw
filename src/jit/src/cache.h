#pragma once

// Internals of the compile cache (pq/jit/cache.h): the key, the executable's identity, disk entries, eviction.
// Not RT code: the compiler thread.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "jit_image.h"
#include "pq/graph/schedule.h"

namespace pq::jit::detail {

struct Key {
  uint64_t lo = 0, hi = 0;
  friend bool operator==(const Key&, const Key&) = default;
  std::string hex() const;  // 32 lowercase hex digits (hi then lo)
};
struct KeyHash {
  size_t operator()(const Key& k) const noexcept { return static_cast<size_t>(k.lo ^ (k.hi * 0x9E3779B97F4A7C15ull)); }
};

Key hashBytes(const void* data, size_t n) noexcept;

// Everything code generation reads from the schedule and the image layout (minus identities: state keys, seeds, node
// indices, diagnostics), and the pointer-table slots the host assigned (`hostSlots`).
Key codegenDigest(const graph::Schedule& s, const HostLayout& layout, uint32_t hostSlots);

struct KeyOptions {
  std::string_view cpu, features;  // the target the machine code is for
  uint32_t codeGenLevel = 3, optLevel = 1;
  bool contract = false;
  uint8_t voiceCode = 0;  // the resolved VoiceCode (T1b.2: groups, 256- or 512-bit pairs, scalar lanes)
};
// The cache key: codegenDigest + the options + the target + LLVM version + entry format + the executable's identity.
Key cacheKey(const Key& digest, const KeyOptions& o, const Key& binary);

// The executable (or DLL) that holds this code: where it is mapped, and a hash of its file.
struct BinaryIdentity {
  bool ok = false;
  uintptr_t base = 0;
  size_t size = 0;  // SizeOfImage
  Key hash;
  double hashMs = 0;  // what computing it cost (once per process)
};
const BinaryIdentity& binaryIdentity();  // computed on the first call (thread-safe)

// One disk entry's payload.
struct Entry {
  std::vector<std::string> stageFunctions;
  std::vector<std::string> pairFunctions;  // per stage: the two-group function's name, or empty (T1b.2)
  uint32_t hostSlots = 0;
  std::vector<uint64_t> fnOffsets;  // pointer-table slots hostSlots… as offsets from BinaryIdentity::base
  Key irHash;                       // hash of the generated IR (zero: not recorded)
  uint64_t objectBytes = 0;
  uint32_t irInstructions = 0, inlinedOps = 0, calledOps = 0;
  bool stackProbes = false;
  std::vector<uint8_t> object;      // the COFF object
};

enum class ReadResult { Miss, Hit, Rejected };
std::filesystem::path entryPath(const std::filesystem::path& dir, const Key& key);
// Reads and validates `dir`/<key>.pqjit. Rejected entries are deleted when `writable`; a hit in a writable directory
// refreshes the file's write time (least recently used eviction).
ReadResult readEntry(const std::filesystem::path& dir, const Key& key, bool writable, Entry& out);
// Writes the entry atomically (temporary file + rename). Returns the bytes written, 0 on failure.
uint64_t writeEntry(const std::filesystem::path& dir, const Key& key, const Entry& e);
// Deletes least recently used entries until the directory holds at most maxBytes · 3/4; abandoned temporary files
// older than an hour too. Returns the directory's size afterwards.
struct EvictResult {
  uint64_t bytes = 0, evictedFiles = 0, evictedBytes = 0;
};
EvictResult evict(const std::filesystem::path& dir, uint64_t maxBytes);
uint64_t directoryBytes(const std::filesystem::path& dir);  // entries only

}  // namespace pq::jit::detail
