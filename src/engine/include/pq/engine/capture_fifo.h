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
// Drift (T6.x-fix-ui-b, M6 review M7): two devices run on two crystals (±50 ppm is normal), so without correction the
// delay above walks until a drop or an underflow cuts the recording (one every ~7 min at 50 ppm). pull() therefore
// takes the play time of its period's first output frame (`playQpc100ns`, QPC, from the render clock) and measures the
// delay d = play time − capture time of the first input frame taken, in frames, continuous (both are interpolated from
// timestamps: no packet-phase sawtooth). After priming, d's mean over kSetpointSeconds is the setpoint; a one-pole
// low-pass (kFilterSeconds) of d − setpoint is the error e. While |e| ≤ kEngageSeconds (24 frames at 48 kHz) the FIFO
// stays a plain copy: one clock (an interface's in and out), or no timestamps, is bit-exact and costs nothing. Past it
// the read becomes a fractional one through pq::dsp's High windowed sinc (64 taps, the unstretched q = 256 table: flat
// to 0.42·fs, stopband below −85 dB), at `ratio` input frames per output frame from a PI loop (critically damped,
// kLoopSeconds): ratio = 1 + kp·e + ∫ki·e, clamped to 1 ± kMaxDeviation (1000 ppm, under 2 cents; the integral
// starts at e's slope since the setpoint). It holds d at the setpoint, so the round trip the recorder uses stays true
// for the whole take, and at ±200 ppm nothing is dropped or inserted (tests/engine/test_capture_drift.cpp: 10 min).
// The read position is r_ + frac_ (32.32 fixed point). The drop / underflow paths above stay as the last resort (a
// glitch beyond the reserve, drift beyond the clamp); each re-measures the setpoint. reset() forgets the drift.
//
// Storage: planar float, one power-of-two ring per channel, allocated by prepare(). push() converts the packet's
// interleaved samples (float32 / int32 / 24-in-32 / packed 24 / int16) straight into the rings: one pass, no scratch.
// While resampling, push keeps kHistory frames behind the read position (the filter's left half), and pull copies each
// chunk's input window out of the ring (wrap) into a small scratch before filtering it.

#include <cmath>
#include <cstdint>

#include "pq/core/aligned_buffer.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/sample_convert.h"

namespace pq::dsp {
struct SincTable;
}

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
  // `frames` frames into out[0 .. channels()) (see the file comment). `playQpc100ns`: when the first of them plays
  // (QPC, 100 ns; 0: unknown, no drift tracking). Returns the capture time (QPC, 100 ns) of the first frame taken
  // (fractional while resampling), 0 when the period is silence (unprimed, an underflow) or no packet had a time.
  // RT-safe.
  uint64_t pull(float* const* out, uint32_t frames, uint64_t playQpc100ns = 0) noexcept;

  uint32_t channels() const noexcept { return channels_; }
  uint32_t capacity() const noexcept { return mask_ + 1; }
  uint32_t fill() const noexcept { return static_cast<uint32_t>(w_ - r_); }
  bool primed() const noexcept { return primed_; }
  uint32_t target() const noexcept { return target_; }
  uint64_t underflows() const noexcept { return underflows_; }  // periods played as silence after priming
  uint64_t dropped() const noexcept { return dropped_; }        // frames dropped (full, over `high`, underflow rest)
  // Drift tracking (see the file comment).
  bool resampling() const noexcept { return resampling_; }
  double ratio() const noexcept { return ratio_; }        // input frames per output frame (1 while copying)
  double delayError() const noexcept { return err_; }     // e: the filtered delay − setpoint, frames

  static constexpr double kSetpointSeconds = 0.5;
  static constexpr double kFilterSeconds = 0.5;
  static constexpr double kLoopSeconds = 4.0;
  static constexpr double kEngageSeconds = 0.0005;
  static constexpr double kMaxDeviation = 1e-3;
  static constexpr uint32_t kHistory = 64;    // frames kept behind the read position while resampling
  static constexpr uint32_t kChunk = 256;     // output frames per filtered chunk
  static constexpr uint32_t kWindow = kChunk + 128;  // its input window (≤ kChunk·(1 + kMaxDeviation) + 66)
  // What a target needs on top of the jitter reserve (one capture packet + one render period) for the drift loop:
  // the filter's right half (33 frames) and the loop's transient error (≤ 1 ms) — a capture packet's phase against
  // the render periods slips by a whole packet over the run, so the fill at a pull spans the full packet below the
  // primed fill.
  static uint32_t driftReserve(double rate) noexcept {  // RT-safe
    return kHistory + static_cast<uint32_t>(std::ceil(0.001 * (rate > 0.0 ? rate : 48000.0)));
  }

private:
  float* ch(uint32_t c) noexcept { return data_.data() + static_cast<size_t>(c) * (mask_ + 1); }
  void drop(uint64_t n) noexcept {
    r_ += n;
    dropped_ += n;
    haveSetpoint_ = false;  // the delay jumped
    setSum_ = 0.0;
    setFrames_ = 0;
  }
  void resampleInto(float* const* out, uint32_t frames) noexcept;
  void track(uint64_t playQpc, uint64_t capQpc, uint32_t frames) noexcept;

  AlignedBuffer<float> data_;
  uint32_t channels_ = 0, mask_ = 0, target_ = 0, high_ = 0;
  double hnsPerFrame_ = 0.0;
  uint64_t w_ = 0, r_ = 0;  // frames written / read so far (indices into the rings & mask_)
  bool primed_ = false;
  uint64_t qpc_ = 0, qpcFrame_ = 0;  // the last timed packet: its first frame's capture time and write index
  uint64_t underflows_ = 0, dropped_ = 0;
  // Drift tracking.
  const dsp::SincTable* sinc_ = nullptr;
  AlignedBuffer<float> window_, spare_;  // channels × kWindow input frames; kChunk frames a mono read also writes
  double rate_ = 0.0, kp_ = 0.0, ki_ = 0.0, tauFilter_ = 1.0, engage_ = 0.0, setpointFrames_ = 0.0;
  bool haveSetpoint_ = false, resampling_ = false;
  double setSum_ = 0.0, setFrames_ = 0.0, setpoint_ = 0.0, sinceSetpoint_ = 0.0;
  double err_ = 0.0, integ_ = 0.0, ratio_ = 1.0;
  int64_t step_ = int64_t{1} << 32;  // ratio_ in 32.32
  uint32_t frac_ = 0;                // the read position's fraction (32.32 with r_)
};

}  // namespace pq::engine
