#include "pq/engine/block_adapter.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace pq::engine {

bool BlockAdapter::prepare(AudioCallback engine, uint32_t numIn, uint32_t numOut, uint32_t blockFrames,
                           uint32_t deviceFrames) {
  if (!engine || blockFrames == 0 || blockFrames > kMaxBlockFrames || numIn > kMaxChannels ||
      numOut > kMaxChannels || deviceFrames == 0)
    return false;
  engine_ = engine;
  numIn_ = numIn;
  numOut_ = numOut;
  blockFrames_ = blockFrames;
  pos_ = 0;
  direct_ = deviceFrames % blockFrames == 0;
  modeSwitches_ = 0;
  engineTime_ = 0;
  // FIFOs exist in both modes so a direct-mode stream can fall back without allocating on the audio thread.
  // Each channel starts on a cache line.
  const uint32_t stride = (blockFrames + 15u) & ~15u;
  fifo_ = AlignedBuffer<float>(static_cast<std::size_t>(stride) * (numIn + numOut));
  for (uint32_t c = 0; c < kMaxChannels; ++c) {
    inFifo_[c] = c < numIn ? fifo_.data() + static_cast<std::size_t>(stride) * c : nullptr;
    outFifo_[c] = c < numOut ? fifo_.data() + static_cast<std::size_t>(stride) * (numIn + c) : nullptr;
  }
  return true;
}

void BlockAdapter::runEngine(const float* const* in, float* const* out) noexcept {  // RT-safe
  const AudioBlock block{in, numIn_, out, numOut_, blockFrames_, engineTime_};
  engine_.fn(engine_.user, block);
  engineTime_ += blockFrames_;
}

void BlockAdapter::process(const AudioBlock& device) noexcept {  // RT-safe
  assert(device.numIn == numIn_ && device.numOut == numOut_);
  if (direct_) {
    if (device.frames % blockFrames_ == 0) {
      const float* in[kMaxChannels];
      float* out[kMaxChannels];
      for (uint32_t off = 0; off < device.frames; off += blockFrames_) {
        for (uint32_t c = 0; c < numIn_; ++c) in[c] = device.in[c] + off;
        for (uint32_t c = 0; c < numOut_; ++c) out[c] = device.out[c] + off;
        runEngine(in, out);
      }
      return;
    }
    // Not a multiple after all: FIFO from here on. The output FIFO is still all zeros (direct mode never
    // touches it), so the shift is blockFrames of silence, once.
    direct_ = false;
    pos_ = 0;
    ++modeSwitches_;
  }
  processFifo(device);
}

void BlockAdapter::processFifo(const AudioBlock& device) noexcept {  // RT-safe
  // Invariant: outFifo holds the engine output of the previous block; positions [0, pos_) of it have been played
  // and positions [0, pos_) of inFifo have been gathered for the next block. Latency is exactly blockFrames_.
  uint32_t offset = 0;
  while (offset < device.frames) {
    const uint32_t n = std::min(device.frames - offset, blockFrames_ - pos_);
    const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(float);
    for (uint32_t c = 0; c < numIn_; ++c) std::memcpy(inFifo_[c] + pos_, device.in[c] + offset, bytes);
    for (uint32_t c = 0; c < numOut_; ++c) std::memcpy(device.out[c] + offset, outFifo_[c] + pos_, bytes);
    pos_ += n;
    offset += n;
    if (pos_ == blockFrames_) {
      runEngine(inFifo_, outFifo_);
      pos_ = 0;
    }
  }
}

}  // namespace pq::engine
