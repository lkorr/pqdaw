#pragma once

// pqdaw's JIT compile cache and AOT seed (T1b.4; pq/jit/cache.h, QUESTIONS Q116).
//
//   user cache  %LOCALAPPDATA%\pqdaw\jit-cache (read-write, 512 MB, least recently used out). $PQ_JIT_CACHE_DIR
//               names another directory; $PQ_JIT_CACHE_DIR=off turns the disk cache off.
//   AOT seed    <exe dir>/jit-aot (read-only): the stock units — what a new project and default-instrument tracks
//               compile to — compiled at build time by this very executable (`pqdaw --jit-aot <dir>`, a post-build
//               step) for the baseline CPU, so opening them never waits for LLVM, from the first launch on.
// Not RT-safe (file system, compiling).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/jit/cache.h"
#include "pq/jit/compiler.h"

namespace pq::app {

std::filesystem::path aotSeedDir();  // <exe dir>/jit-aot
jit::CacheConfig appJitCache();      // the configuration above
void configureJitCache();            // compile::setJitCache(appJitCache()); pqdaw's modes that compile call it first

struct AotSeedOptions {
  std::vector<double> sampleRates{44100.0, 48000.0, 88200.0, 96000.0};
  std::string stdlib;                 // empty: stdlibDir()
  jit::Target target = jit::Target::Baseline;
  double timeoutSeconds = 600.0;
  bool verify = false;  // record IR hashes (CacheConfig::verify), so verifying loaders can check every hit
};

struct AotSeedStats {
  uint32_t projects = 0;  // stock projects compiled (per rate)
  uint64_t images = 0;    // JIT images built for them (units; equal units share an entry)
  uint64_t compiles = 0;  // LLVM compiles
  uint64_t entries = 0;   // files in the directory afterwards
  uint64_t bytes = 0;
  double seconds = 0.0;
};

// Compiles the stock units into `dir` (its .pqjit entries are replaced) through exactly the playback path
// (ProjectLink: lowering, per-track units, the background JIT), so the keys are those the DAW computes.
Result<AotSeedStats> seedJitAot(const std::filesystem::path& dir, const AotSeedOptions& options = {});

int runJitAot(const std::string& dir);  // `pqdaw --jit-aot <dir>`

}  // namespace pq::app
