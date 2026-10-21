#pragma once

#include <cstdint>

#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/callback_stats.h"

namespace pq::engine {

// The per-callback path shared by every AudioDevice backend: the backend's own callback converts its buffers to
// planar float pointers and calls run(), which is the only thing that touches the engine. Keeping it in one
// place means the NullDevice tests (RtScope allocation trap, denormal mode, stats) cover exactly the code the
// RtAudio backend runs on the real audio thread.
class CallbackRunner {
public:
  // Off the audio thread, before the stream starts.
  void prepare(AudioCallback cb, uint32_t numIn, uint32_t numOut, uint32_t bufferFrames, uint32_t sampleRate) {
    cb_ = cb;
    numIn_ = numIn;
    numOut_ = numOut;
    frameTime_ = 0;
    stats_.reset(bufferFrames, sampleRate);
  }

  // One device callback. Opens the realtime scope (allocation trap) and the denormal guard (FTZ/DAZ) for the
  // engine, calls it once, and records timing and xrun flags. RT-safe.
  void run(const float* const* in, float* const* out, uint32_t frames, bool inputOverflow,
           bool outputUnderflow) noexcept {  // RT-safe
    RtScope rt;
    DenormalGuard denormals;
    const uint64_t t0 = readTsc();
    const AudioBlock block{in, numIn_, out, numOut_, frames, frameTime_};
    cb_.fn(cb_.user, block);
    frameTime_ += frames;
    stats_.record(t0, readTsc(), frames, inputOverflow, outputUnderflow);
  }

  const CallbackStats& stats() const noexcept { return stats_; }
  uint32_t numIn() const noexcept { return numIn_; }
  uint32_t numOut() const noexcept { return numOut_; }

private:
  AudioCallback cb_{};
  uint32_t numIn_ = 0;
  uint32_t numOut_ = 0;
  int64_t frameTime_ = 0;
  CallbackStats stats_;
};

}  // namespace pq::engine
