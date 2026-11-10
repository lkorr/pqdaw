#pragma once

// T6.7b: the hand-over from a WASAPI capture stream to the render callback (pq/engine/wasapi_device.h). Both run on
// the device's one audio thread: each wake first drains the capture packets into this FIFO (push), then every render
// period takes its input frames from it (pull). One thread: plain indices, no atomics.
//
// Capture packets arrive in the audio engine's period (10 ms in shared mode, unless IAudioClient3 runs it shorter);
// the render callback wants `period` frames on its own schedule. The FIFO therefore holds a jitter reserve: pull()
// plays silence until `target` frames are buffered (primed), then takes exactly what it is asked for. The two streams'
// clocks may differ (two devices): fewer than a period buffered → silence for that period, the rest dropped, primed
// again (an underflow); more than `high` after a pull → the oldest dropped back to `target` (counted). Between those
// events the input runs sample-continuous at a constant delay, which is what recording needs (the delay itself is
// measured from the capture timestamps: pull() returns the capture time of the first frame it took).
//
// Storage: planar float, one power-of-two ring per channel, allocated by prepare(). push() converts the packet's
// interleaved samples (float32 / int32 / 24-in-32 / packed 24 / int16) straight into the rings: one pass, no scratch.

#include <cstdint>

#include "pq/core/aligned_buffer.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/sample_convert.h"

namespace pq::engine {

class CaptureFifo {
public:
  // Off the audio thread. `channels` (≤ kMaxChannels) planar channels with room for at least `capacityFrames`;
  // `target` ≤ `high` < capacity. `rate`: frames per second (for the capture times).
  void prepare(uint32_t channels, uint32_t capacityFrames, uint32_t target, uint32_t high, double rate);
  // Empty and unprimed (the counters stay). RT-safe.
  void reset() noexcept;
  // Appends `frames` interleaved frames of `deviceChannels` samples in `fmt` (null: silence); device channel c feeds
  // channel c < channels(), the others are skipped. A full FIFO drops its oldest frames first (dropped()).
  // `qpc100ns`: the capture time of the packet's first frame (QPC, 100 ns units; 0: unknown). RT-safe.
  void push(const void* data, SampleFormat fmt, uint32_t deviceChannels, uint32_t frames, uint64_t qpc100ns) noexcept;
  // `frames` frames into out[0 .. channels()) (see the file comment). Returns the capture time (QPC, 100 ns) of the
  // first frame taken, 0 when the period is silence (unprimed, an underflow) or no packet had a time. RT-safe.
  uint64_t pull(float* const* out, uint32_t frames) noexcept;

  uint32_t channels() const noexcept { return channels_; }
  uint32_t capacity() const noexcept { return mask_ + 1; }
  uint32_t fill() const noexcept { return static_cast<uint32_t>(w_ - r_); }
  bool primed() const noexcept { return primed_; }
  uint32_t target() const noexcept { return target_; }
  uint64_t underflows() const noexcept { return underflows_; }  // periods played as silence after priming
  uint64_t dropped() const noexcept { return dropped_; }        // frames dropped (full, over `high`, underflow rest)

private:
  float* ch(uint32_t c) noexcept { return data_.data() + static_cast<size_t>(c) * (mask_ + 1); }
  void drop(uint64_t n) noexcept {
    r_ += n;
    dropped_ += n;
  }

  AlignedBuffer<float> data_;
  uint32_t channels_ = 0, mask_ = 0, target_ = 0, high_ = 0;
  double hnsPerFrame_ = 0.0;
  uint64_t w_ = 0, r_ = 0;  // frames written / read so far (indices into the rings & mask_)
  bool primed_ = false;
  uint64_t qpc_ = 0, qpcFrame_ = 0;  // the last timed packet: its first frame's capture time and write index
  uint64_t underflows_ = 0, dropped_ = 0;
};

}  // namespace pq::engine
