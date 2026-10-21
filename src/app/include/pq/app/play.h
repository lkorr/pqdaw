#pragma once

// `pqdaw --play` (T1.7): a .pqp file → parse (imports relative to the file) → infer → optimise → schedule →
// interpreted ProgramImage, for one patch with at least one global `out`. Minimal on purpose: T1.10 adds the
// offline renderer and `pqc`; voices (T1.8) are needed before an instrument patch can play.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "pq/core/result.h"
#include "pq/engine/program_image.h"

namespace pq::app {

struct CompiledPlay {
  std::unique_ptr<engine::ProgramImage> image;
  std::string patchName;   // the patch compiled
  uint32_t ops = 0;        // schedule size, for the summary line
  uint32_t outputs = 0;    // global `out` slots played
};

// `patch` empty: the first patch of the file. Not RT-safe (file I/O, compiling).
Result<CompiledPlay> compilePatchFile(const std::string& path, std::string_view patch, double sampleRate,
                                      uint32_t maxBlockFrames);

}  // namespace pq::app
