#pragma once

// The Waveform's capture (VZ.6; ported from PQ Meter's modules/waveform.cpp ingestSpan): the analysis worker folds the
// signal into BINS of 128 frames — per channel (L, R, mid, side) the minimum, the maximum, the sum of squares and the
// sum of squares of the part above 700 Hz (a one-pole split: the brightness) — in a ring of kRingBins (≈ 87 s at
// 48 kHz). The display folds the bins of its window into its columns (peak envelope, RMS body, colour by level or
// brightness) on an absolute grid (a bin keeps its column while on screen: no shimmer).
//
// Loop playhead: kLoopSlots slots over a span; a frame lands in the slot of its phase, a slot entered anew is cleared
// (each pass overwrites the picture). The phase is the tap's BEAT channel modulo `loopBeats` (the transport's clock:
// the picture locks to bars; dsp::ScopeBeat's rule) while it plays; with no clock (NaN: nothing plays) the stream's
// own frame count modulo `loopSeconds` (PQ Meter's free run); a clock standing still (the transport stopped on a
// position) holds the picture. Each slot carries the stamp of its last write, so a
// reader copies only what changed.
//
// Threads: the worker pushes, the UI reads; configure from the UI. An internal mutex guards it all (per pushed chunk,
// per read). push allocates nothing; the constructor allocates the ring.

#include <cstdint>
#include <mutex>
#include <vector>

namespace pq::dsp {

struct WaveConfig {
  double sampleRate = 48000.0;
  bool loop = false;
  double loopBeats = 4.0;    // the clocked span
  float loopSeconds = 2.0f;  // the free-run span
  bool operator==(const WaveConfig&) const = default;
};

struct WaveBin {
  static constexpr int kChannels = 4;  // L, R, mid, side
  float mn[kChannels], mx[kChannels], ss[kChannels], hs[kChannels];
  uint32_t n;
};

// What the loop shows now.
struct WaveLoopState {
  double phase = -1.0;      // the playhead 0 … 1 (−1: none yet)
  double spanSamples = 0.0; // the span in frames (clocked: at the tempo the clock moved at)
  double bpm = 0.0;         // 0: no clock (free run)
  bool clocked = false, moving = false;
  uint64_t stamp = 0;       // bumps at every pushed chunk that wrote the loop
};

class WaveCapture {
public:
  static constexpr uint32_t kBinSize = 128;
  static constexpr uint32_t kRingBins = 1u << 15;
  static constexpr uint32_t kLoopSlots = 2048;

  explicit WaveCapture(const WaveConfig& c = {});
  void configure(const WaveConfig& c);  // a new loop span clears the loop
  // Frames: the beat position (null or NaN: no clock) and the stereo signal. Allocation-free; the worker's.
  void push(const float* beat, const float* l, const float* r, uint32_t n) noexcept;
  // Silence paced in time with no data (the scroll moves on; the loop holds: no playhead moves without the transport).
  void pushSilence(uint32_t n) noexcept;
  // Frames since the last one above −120 dBFS (the worker's rest test).
  uint64_t quietFrames() const;

  // ---- UI ----
  uint64_t bins() const;  // committed so far (the ring holds the last kRingBins)
  // Bins [from, from + n) ∩ the held ones into out from its start; returns how many; `partial` (non-null): the bin
  // still being filled (n = 0: none).
  uint32_t readBins(uint64_t from, uint32_t n, WaveBin* out, WaveBin* partial = nullptr) const;
  // The loop: its state, and every slot written after `since` (a stamp) into slots / filled (kLoopSlots each).
  // Returns the newest stamp.
  uint64_t readLoop(uint64_t since, WaveBin* slots, uint8_t* filled, WaveLoopState& state) const;

private:
  void clearLoop() noexcept;
  void addFrame(float l, float r, double phase) noexcept;  // phase < 0: no loop write

  mutable std::mutex mutex_;
  WaveConfig cfg_;
  std::vector<WaveBin> ring_;
  WaveBin cur_{};
  uint64_t written_ = 0;
  float lp_[WaveBin::kChannels] = {}, lpA_ = 0.0f;
  std::vector<WaveBin> loop_;
  std::vector<uint64_t> loopStamp_;
  uint64_t stamp_ = 0;
  int64_t slotCur_ = -1;
  WaveLoopState state_;
  double stream_ = 0.0;  // frames pushed (the free run's clock)
  float lastBeat_ = 0.0f;
  bool haveLast_ = false;
  bool anchored_ = false;  // the tempo's baseline: a beat and the frame it was at
  double anchorFrame_ = 0.0, anchorBeat_ = 0.0;
  uint64_t quiet_ = 0;
};

}  // namespace pq::dsp
