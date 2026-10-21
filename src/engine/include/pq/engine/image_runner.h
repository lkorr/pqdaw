#pragma once

// Drives a ProgramImage from the audio callback path (T1.7): device → CallbackRunner → BlockAdapter → ImageRunner →
// ProgramImage::process. A minimal engine for one global patch (no voices, no events; T1.8/T3.16 build the real
// engine on the same pieces):
//   - device input channel c feeds the image's global root input slot c (io().globalIn);
//   - device output channel c plays the c-th global `out` slot (exports are not played); a mono patch plays on
//     every channel; channels without a slot are silent;
//   - the output is multiplied by `gain` and, with `limit`, clamped to [−1, 1] (a safety net for test playback);
//   - the param block holds the declared defaults (writable before start through params());
//   - the transport is stopped at beat 0 unless set before start (it then advances by beatsPerFrame).

#include <cstdint>
#include <span>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/program_image.h"

namespace pq::engine {

class ImageRunner {
public:
  // Off the audio thread; the image must outlive the stream. Returns false for channel counts above kMaxChannels.
  bool prepare(ProgramImage* image, uint32_t numIn, uint32_t numOut, float gain = 1.0f, bool limit = false);

  AudioCallback callback() noexcept { return {&ImageRunner::onBlock, this}; }
  // One engine block (any frame count: blocks above the image's maxBlockFrames are split). RT-safe.
  void process(const AudioBlock& block) noexcept;

  std::span<float> params() noexcept { return params_; }       // set before start
  TransportSnapshot& transport() noexcept { return transport_; }  // set before start
  // Global output slot played on device channel c, or graph::kNone.
  uint32_t slotForChannel(uint32_t c) const noexcept { return c < kMaxChannels ? chanSlot_[c] : graph::kNone; }
  uint64_t blocks() const noexcept { return blocks_; }

private:
  static void onBlock(void* self, const AudioBlock& b) noexcept {  // RT-safe
    static_cast<ImageRunner*>(self)->process(b);
  }

  ProgramImage* image_ = nullptr;
  uint32_t numIn_ = 0, numOut_ = 0;
  float gain_ = 1.0f;
  bool limit_ = false;
  std::vector<float> params_;
  TransportSnapshot transport_;
  AlignedBuffer<float> scratch_;    // one maxBlock buffer per global output slot
  std::vector<float*> slotPtrs_;
  uint32_t chanSlot_[kMaxChannels] = {};
  uint64_t blocks_ = 0;
};

}  // namespace pq::engine
