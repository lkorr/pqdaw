#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "pq/core/aligned_buffer.h"
#include "pq/engine/audio_device.h"
#include "pq/engine/callback_runner.h"

namespace pq::engine {

// A device with no hardware, for tests and headless runs. Two ways to drive it once open():
// - manually: processBlock() runs one device callback on the calling thread (the stream must not be started);
// - threaded: start() spawns a thread that calls processBlock() every bufferFrames / sampleRate seconds against
//   absolute deadlines (a late wake-up is caught up, so the long-run callback rate is exact). Sleep-based
//   (high-resolution waitable timer): no realtime guarantees, which a null device does not need.
// Both run the same CallbackRunner as the RtAudio backend.
class NullDevice final : public AudioDevice {
public:
  NullDevice() = default;
  ~NullDevice() override;

  Backend backend() const noexcept override { return Backend::Null; }
  // One device: id 0, "Null device", 32 in / 32 out, any sample rate (preferred 48 kHz).
  Result<std::vector<DeviceInfo>> enumerate() override;
  // Accepts any bufferFrames ≥ 1 and channel counts ≤ kMaxChannels; sampleRate 0 means 48000. Allocates the
  // channel buffers (zeroed) here.
  Result<StreamInfo> open(const StreamConfig& config, AudioCallback cb) override;
  Status start() override;
  Status stop() override;
  void close() noexcept override;
  bool isOpen() const noexcept override { return open_; }
  bool isRunning() const noexcept override { return running_.load(std::memory_order_acquire); }
  const CallbackStats& stats() const noexcept override { return runner_.stats(); }

  // Manual mode: one callback of `frames` frames (0 = the configured bufferFrames; at most that many) with the
  // given backend status flags, so tests can exercise the xrun counter. RT-safe.
  void processBlock(uint32_t frames = 0, bool inputOverflow = false, bool outputUnderflow = false) noexcept;

  // Channel buffers (bufferFrames samples each). Tests write input() before processBlock() and read output()
  // after it. Valid while open.
  float* input(uint32_t ch) noexcept { return inPtrs_[ch]; }
  const float* output(uint32_t ch) const noexcept { return outPtrs_[ch]; }
  uint32_t bufferFrames() const noexcept { return bufferFrames_; }
  uint32_t sampleRate() const noexcept { return sampleRate_; }

private:
  void threadMain() noexcept;

  CallbackRunner runner_;
  AlignedBuffer<float> storage_;
  float* inPtrs_[kMaxChannels] = {};
  float* outPtrs_[kMaxChannels] = {};
  uint32_t bufferFrames_ = 0;
  uint32_t sampleRate_ = 0;
  bool open_ = false;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace pq::engine
