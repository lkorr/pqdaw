#pragma once

#include <cstdint>

#include "pq/core/aligned_buffer.h"
#include "pq/engine/audio_block.h"

namespace pq::engine {

// Runs an engine callback at a fixed internal block size, independent of the device buffer size (SPEC §2).
// Sits between a device (which calls deviceCallback() with whatever frame count it uses) and the engine (which
// is always called with exactly blockFrames frames). A fixed block lets block-rate work (control-rate
// modulation, parameter smoothing steps, event quantisation) happen at the same sample positions whatever the
// device buffer is, so realtime and offline renders agree, and lets compiled Programs assume a constant trip
// count.
//
// Two modes, chosen in prepare() from the device buffer size:
// - Direct (deviceFrames is a multiple of blockFrames, e.g. 480 = 15 × 32, 256 = 8 × 32): the engine runs
//   deviceFrames / blockFrames times on pointers into the device buffers. No copies, no added latency.
// - FIFO (any other size, e.g. 37 or 1): input is gathered into a blockFrames FIFO and output is played from the
//   previous engine block. Adds exactly blockFrames frames of latency, costs one copy in and one copy out.
// If a direct-mode stream ever delivers a frame count that is not a multiple of blockFrames (backends with a
// fixed buffer never do), the adapter switches to FIFO mode for the rest of the stream: output shifts by
// blockFrames frames once, and modeSwitches() counts it.
class BlockAdapter {
public:
  static constexpr uint32_t kMaxBlockFrames = 4096;

  // Off the audio thread. Allocates the FIFOs (zeroed). blockFrames in [1, kMaxBlockFrames], channel counts
  // ≤ kMaxChannels. Returns false on invalid arguments.
  bool prepare(AudioCallback engine, uint32_t numIn, uint32_t numOut, uint32_t blockFrames, uint32_t deviceFrames);

  // The callback to give to an AudioDevice. Its numIn/numOut must match prepare().
  AudioCallback deviceCallback() noexcept { return {&BlockAdapter::onDevice, this}; }

  // One device callback. RT-safe.
  void process(const AudioBlock& device) noexcept;

  bool directMode() const noexcept { return direct_; }
  uint32_t latencyFrames() const noexcept { return direct_ ? 0 : blockFrames_; }
  uint32_t blockFrames() const noexcept { return blockFrames_; }
  uint64_t modeSwitches() const noexcept { return modeSwitches_; }
  int64_t engineFrames() const noexcept { return engineTime_; }

private:
  static void onDevice(void* self, const AudioBlock& block) noexcept {  // RT-safe
    static_cast<BlockAdapter*>(self)->process(block);
  }
  void runEngine(const float* const* in, float* const* out) noexcept;
  void processFifo(const AudioBlock& device) noexcept;

  AudioCallback engine_{};
  uint32_t numIn_ = 0;
  uint32_t numOut_ = 0;
  uint32_t blockFrames_ = 0;
  uint32_t pos_ = 0;  // FIFO fill position in [0, blockFrames)
  bool direct_ = false;
  uint64_t modeSwitches_ = 0;
  int64_t engineTime_ = 0;
  AlignedBuffer<float> fifo_;  // numIn + numOut channels × blockFrames
  float* inFifo_[kMaxChannels] = {};
  float* outFifo_[kMaxChannels] = {};
};

}  // namespace pq::engine
