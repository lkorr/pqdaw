#pragma once

// The JIT's compile cache (T1b.4, SPEC §7.4 "Compile cache"; QUESTIONS Q116/b). Every Compiler::compile looks
// its machine code up before running LLVM:
//   1. in memory: an image of the same key already alive in this process shares its code (one copy of the machine
//      code, one link; e.g. several tracks with the same instrument and default strip),
//   2. on disk: `dir` (read-write, the user's cache, %LOCALAPPDATA%\pqdaw\jit-cache by default in pqdaw), then each
//      `readOnly` directory (the AOT seed shipped next to pqdaw.exe, `jit-aot/`, compiled for the baseline CPU),
//   3. otherwise it compiles (Schedule → IR → LLVM passes → machine code) and stores the object in `dir`.
// A disk hit links the stored object into the JIT (no IR, no passes, no instruction selection) and rebuilds the image's
// memory and pointer table from the current schedule exactly as a compile does, so a cached image computes what a
// freshly compiled one computes, bit for bit (tests/jit/test_jit_cache.cpp).
//
// The key (128-bit xxh3) covers everything code generation reads: the schedule minus identities (state keys, noise
// seeds, node indices, diagnostics), the image's memory layout, the compile options, the target CPU name and features,
// the machine-code level, the LLVM version, the entry format and the identity of the executable that compiled it
// (a hash of its file: generated code calls kernels at fixed offsets in that binary, and its own code generator is
// part of the binary). State keys are left out on purpose: two tracks whose units differ only in their track ids share
// one entry; their images still get their own state keys and seeds (the host is built from each schedule).
//
// Disk entries (<32 hex digits>.pqjit): a 64-byte header (magic, format version, key, payload size, xxh3-128 of the
// payload), then the stage function names, the pointer-table description (host slots, kernel offsets), statistics and
// the COFF object. Written to a unique temporary file and renamed over the entry (MoveFileExW), so readers see an old
// entry or a new one, never half of one; concurrent writers of one key both succeed (last rename wins, same content).
// Readers open with FILE_SHARE_DELETE, so an eviction or a replacing rename never waits for them. Anything that does
// not validate (truncated, flipped bits, another format) is a miss: the entry is deleted (read-write dir only) and
// recompiled. `dir` is bounded by `maxBytes`: after a store that takes it over the bound, the least recently used
// entries (a hit refreshes the file's write time) are deleted until it is under 3/4 of it; abandoned temporary files
// older than an hour go too.
//
// The cache never runs on the audio thread: lookups, loads and stores happen inside Compiler::compile (a compiler
// thread). Not a security boundary: whoever can write the cache directory can already run code as the user.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace pq::jit {

struct CacheConfig {
  std::filesystem::path dir;                    // read-write disk cache; empty: no disk cache
  std::vector<std::filesystem::path> readOnly;  // searched after `dir` (AOT seeds), never written or evicted
  uint64_t maxBytes = uint64_t{512} << 20;      // bound of `dir`
  // Tests: every hit (memory or disk) also generates the IR again and compares its hash with the one recorded when
  // the code was compiled (entries record it only when the compiling Compiler had `verify` on). A mismatch means the
  // key misses something code generation reads: it is counted, and the image is compiled instead.
  bool verify = false;
};

// What compile() did, per Compiler (counters(); a test hook: "no JIT compile" = `compiles` unchanged).
struct CacheCounters {
  uint64_t compiles = 0;      // LLVM compiles (IR generation + passes + machine code)
  uint64_t memoryHits = 0;    // images that shared a live image's code
  uint64_t diskHits = 0;      // loaded from `dir`
  uint64_t aotHits = 0;       // loaded from a `readOnly` directory
  uint64_t stores = 0;        // entries written to `dir`
  uint64_t storeErrors = 0;   // writes that failed (disk full, a rename that lost to an eviction …)
  uint64_t rejected = 0;      // entries that did not validate (deleted when in `dir`)
  uint64_t uncacheable = 0;   // compiles whose code points at memory outside the executable (not shared, not stored)
  uint64_t evictedFiles = 0, evictedBytes = 0;
  uint64_t verified = 0;          // hits whose IR hash was compared (verify)
  uint64_t verifyMismatches = 0;  // ... and differed (must stay 0)
  // M1 review L6: machine code is released when its last image goes, without waiting for a compile in progress.
  uint64_t codesReleased = 0;     // machine-code objects released (JITDylibs removed)
  uint64_t releasesDeferred = 0;  // ... whose release found a compile running and was left to it (freed when it ended)
};

// %LOCALAPPDATA%\pqdaw\jit-cache (created if missing); empty if that is not available.
std::filesystem::path defaultCacheDir();

}  // namespace pq::jit
