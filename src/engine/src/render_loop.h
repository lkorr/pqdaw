#pragma once

// Engine-internal (T1.x-arch2): the one offline render loop — renderOffline (one image, through a ProgramHost), the
// ProgramHost form and Engine::renderOffline all run it. Whole blocks into a chunk buffer under an RtScope and
// DenormalGuard (the realtime path), the sink and `between` outside them.

#include <cstdint>

#include "pq/core/result.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/offline_render.h"

namespace pq::engine::detail {

struct RenderLoop {
  AudioCallback callback;            // one device block (RT)
  uint32_t numChannels = 1;
  double sampleRate = 48000.0;
  uint64_t frames = 0;
  uint32_t blockFrames = 64;         // ≥ 1
  uint32_t chunkFrames = 8192;       // rounded up to whole blocks
  int64_t startFrame = 0;            // AudioBlock::frameTime of the first block
  float gain = 1.0f;                 // applied to the device output (1: untouched)
  void (*between)(void* user, int64_t nextFrame) = nullptr;  // non-RT, after each chunk's sink write
  void* user = nullptr;
};

Result<RenderStats> runRenderLoop(const RenderLoop& loop, AudioSink& sink);

}  // namespace pq::engine::detail
