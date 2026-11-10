#pragma once

// The recorder (T6.7; docs/plans/M6.md "### T6.7"): the audio thread's side of recording — device inputs captured into
// lock-free rings with the song time of every frame — and of input monitoring (the monitor gate of every audio track
// that has an input). The app's RecordingService (pq/app/recording.h) drains the rings on its own writer
// thread into WAV files and commits takes to the document.
//
// ---- Capture ------------------------------------------------------------------------------------------------------
// While the timeline records (Timeline::recording(): a Record command, until Stop / RecordOff / Play) the segment ring
// gets two kinds of CaptureSegment:
//   Run    every SONG run the transport plays in an image block (Timeline::runs; count-in runs are not recorded):
//          {engine time, song sample, frames, session} — the song-time map of the session, which note recording reads
//          too (a MIDI-only session pushes only these).
//   Audio  input frames, placed on what was PLAYED when they were captured (M6 review M6): the frame captured at engine
//          time t was played against the song sample the transport played at engine time t − offset (the record
//          latency, below; a negative total counts as 0). The recorder keeps the session's recent runs (contiguous ones
//          merged, kPlayedRuns of them) and maps the block's captured interval [time − offset, time + frames − offset)
//          through them, so frames that arrive after a loop wrap or a locate go with the pass that played them. A piece
//          is kept when its played song samples lie in the capture window: everything, or with the punch on
//          [punchIn, punchOut). {engine time and song sample it was PLAYED at, frames, session, capture set}. Played
//          time only moves forward: a frame is placed once (an offset that grows does not repeat frames; one that
//          shrinks leaves a gap the writer fills with silence). Frames played before the session started (the first
//          offset frames captured) or in a count-in belong to nothing and are not kept.
// The captured frames of every channel of the capture set (CaptureSet: the device input channels the armed tracks use,
// rings allocated off the audio thread — by the app on first arm) are copied into that channel's FloatRing: ONE copy
// per used channel per piece. A ring (or the segment ring) without room for a piece drops it whole for every channel
// (the segment says how many frames: the writer writes silence there, so the file stays aligned to song time; counted
// in overflows()), and never blocks. Nothing armed (no capture set) and not recording: one flag test per image block.
//
// ---- Record latency -------------------------------------------------------------------------------------------------
// offset = device input latency + device output latency (both incl. the BlockAdapter's FIFO; setDeviceLatency, the
// audio output sets them per open stream) + the master's PDC total (ProjectProgram::outputLatency, read per block) +
// the manual offset (setManualOffset: prefs, signed). An input frame captured at song sample s was played against song
// sample s − offset. noteOffset() = output latency + PDC: what a live note's player heard (notes have no input path).
//
// ---- Monitoring -----------------------------------------------------------------------------------------------------
// MonitorImage (immutable, built by the app): per audio track with an input, its monitor gate's project param slot, its
// MonitorMode, its arm and its clip slot. Every image block (in the block hook, before any unit runs) the gate is
// In → 1; Off → 0; Auto → armed && (transport stopped || recording || no clip of the track sounding in the block
// (ClipPlayer::active)). A value is written only when the param holds another one (the app's own writes of the gate are
// corrected in the same block), and each write counts as a param change for the idle-unit scan.
//
// ---- Threads ------------------------------------------------------------------------------------------------------
//   setCapture, setMonitors, setDeviceLatency, setManualOffset, collect      one non-RT thread (the UI thread)
//   popSegment and the CaptureSet's rings' pop side                           one consumer thread (the recording writer)
//   beginBlock, capture, monitor                                               the audio thread. RT-safe.
// Images (capture sets, monitor images) are published by pointer swap; a replaced one is freed by collect() once the
// audio thread has passed a block since (or at once while it does not run).

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/spsc_ring.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/timeline.h"

namespace pq::engine {

// A single-producer single-consumer ring of floats with bulk push / pop (memcpy, two pieces at the wrap).
class FloatRing {
public:
  explicit FloatRing(uint32_t capacityPow2);  // allocates; a power of two ≥ 2
  uint32_t capacity() const noexcept { return mask_ + 1; }
  // Producer: free room / push exactly n (false, nothing pushed, when there is no room). RT-safe.
  uint32_t space() const noexcept {
    return capacity() - static_cast<uint32_t>(head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire));
  }
  bool push(const float* x, uint32_t n) noexcept;
  // Consumer: frames available / pop exactly n into `out` (false when fewer are there). RT-safe.
  uint32_t available() const noexcept {
    return static_cast<uint32_t>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed));
  }
  bool pop(float* out, uint32_t n) noexcept;

private:
  AlignedBuffer<float> data_;
  uint32_t mask_ = 0;
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) std::atomic<uint64_t> tail_{0};
};

// One entry of the segment ring (see "Capture").
struct CaptureSegment {
  enum Kind : uint8_t { Run, Audio };
  Kind kind = Run;
  int64_t engineTime = 0;   // Run: engine frame of its first frame; Audio: the engine frame its first frame was PLAYED at
  int64_t songSample = 0;   // the song sample of that frame (Audio: the one it was played against)
  uint32_t frames = 0;      // run length / captured frames
  uint32_t dropped = 0;     // Audio: = frames when the rings had no room (nothing pushed: the writer writes silence)
  uint32_t session = 0;     // Timeline::recordSession()
  uint32_t captureSet = 0;  // Audio: CaptureSet::serial() of the set the frames went to
};

// The device input channels being captured and their rings (built off the audio thread).
class CaptureSet {
public:
  // `channels`: device input channel indices (each < kMaxChannels, distinct); `ringFrames` rounded up to a power of
  // two. Allocates.
  CaptureSet(std::span<const uint16_t> channels, uint32_t ringFrames, uint32_t serial);
  std::span<const uint16_t> channels() const noexcept { return channels_; }
  FloatRing& ring(size_t i) noexcept { return *rings_[i]; }
  uint32_t serial() const noexcept { return serial_; }
  // Index of device channel `ch` in channels(), or −1.
  int32_t indexOf(uint16_t ch) const noexcept;

private:
  std::vector<uint16_t> channels_;
  std::vector<std::unique_ptr<FloatRing>> rings_;
  uint32_t serial_ = 0;
};

// Monitoring (see "Monitoring").
struct MonitorEntry {
  enum Mode : uint8_t { Auto, In, Off };  // doc::MonitorMode's order
  uint32_t param = 0;                     // project param slot of the gate "<id>:monitor"
  Mode mode = Auto;
  bool armed = false;
  uint32_t clipSlot = kNoSlot;            // the track's clip slot (kNoSlot: none: never "a clip under the playhead")
};
struct MonitorImage {
  std::vector<MonitorEntry> entries;
};

struct RecorderStats {
  uint64_t segments = 0;        // segments pushed
  uint64_t capturedFrames = 0;  // frames pushed per channel
  uint64_t droppedFrames = 0;   // frames dropped (rings full)
  uint64_t droppedSegments = 0; // the segment ring was full (the run is lost to the time map too)
  uint64_t monitorWrites = 0;
};

class Recorder {
public:
  static constexpr uint32_t kSegmentRing = 1u << 15;  // ≈ 21 s of 64-frame blocks at 48 kHz (a Run + an Audio each)
  static constexpr uint32_t kPlayedRuns = 256;        // the session's recent played runs (contiguous ones merged)

  Recorder();
  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  // ---- UI thread ----
  // The channels to capture (null: none). The set is shared with the consumer (the writer keeps it while it drains).
  void setCapture(std::shared_ptr<CaptureSet> set);
  void setMonitors(std::shared_ptr<const MonitorImage> image);
  // Device latencies of the open stream, frames (input path, output path; each with the adapter's FIFO). Any thread.
  void setDeviceLatency(uint32_t inputFrames, uint32_t outputFrames) noexcept {
    inLatency_.store(inputFrames, std::memory_order_relaxed);
    outLatency_.store(outputFrames, std::memory_order_relaxed);
  }
  // The manual record offset (prefs), frames, signed. Any thread.
  void setManualOffset(int32_t frames) noexcept { manual_.store(frames, std::memory_order_relaxed); }
  // Frees replaced images the audio thread has left (all of them when `audioStopped`).
  void collect(bool audioStopped);
  // The record latency (see the file comment) with PDC total `pdc`, and the notes' part. Any thread.
  int64_t latencyOffset(uint32_t pdc) const noexcept {
    return int64_t{inLatency_.load(std::memory_order_relaxed)} + outLatency_.load(std::memory_order_relaxed) + pdc +
           manual_.load(std::memory_order_relaxed);
  }
  int64_t noteOffset(uint32_t pdc) const noexcept { return int64_t{outLatency_.load(std::memory_order_relaxed)} + pdc; }
  uint32_t outputLatency() const noexcept { return outLatency_.load(std::memory_order_relaxed); }
  // Device input channels of the last block the audio thread saw. Any thread.
  uint32_t deviceInputs() const noexcept { return numIn_.load(std::memory_order_relaxed); }
  // Overflows so far (frames dropped, segments dropped). Any thread.
  uint64_t droppedFrames() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  // ---- Consumer thread ----
  bool popSegment(CaptureSegment& out) noexcept { return segments_.tryPop(out); }

  // ---- Audio thread ----
  // The device block the engine runs now (its input channels for capture() and the tracks' inputs). RT-safe.
  void beginBlock(const AudioBlock& device) noexcept;
  // Input channel `ch` at engine time `time` (inside the current device block), or null (no such channel).
  const float* input(uint32_t ch, int64_t time) const noexcept {
    return ch < numInNow_ && in_ != nullptr && in_[ch] != nullptr ? in_[ch] + (time - blockTime_) : nullptr;
  }
  uint32_t inputs() const noexcept { return numInNow_; }
  // One image block at engine time `time`: the song runs the timeline played in it, and the block's input frames placed
  // on what was played (see "Capture"), while it records. `pdc`: ProjectProgram::outputLatency(). RT-safe.
  void capture(const Timeline& timeline, int64_t time, uint32_t frames, uint32_t pdc) noexcept;
  // Writes the monitor gates for this image block into `params`; true when one changed. RT-safe.
  bool monitor(const Timeline& timeline, std::span<float> params) noexcept;
  // A gate was open in the last monitor() (the engine's idle gate then stays awake: the input must be heard).
  bool monitoring() const noexcept { return monitoring_; }
  const RecorderStats& stats() const noexcept { return stats_; }

private:
  template <class T>
  struct Published {
    std::atomic<T*> ptr{nullptr};
    std::vector<std::pair<uint64_t, std::shared_ptr<T>>> retired;  // (generation retired at, image) — UI thread
    std::shared_ptr<T> current;                                     // UI thread
  };
  template <class T>
  void publish(Published<T>& p, std::shared_ptr<T> next);
  template <class T>
  void collectOf(Published<T>& p, uint64_t seen, bool all);

  Published<CaptureSet> capture_;
  Published<const MonitorImage> monitors_;
  std::atomic<uint64_t> gen_{1};   // bumped by every publish (after the pointer)
  std::atomic<uint64_t> seen_{0};  // the generation the audio thread loaded at its last block
  std::atomic<uint32_t> inLatency_{0}, outLatency_{0};
  std::atomic<int32_t> manual_{0};
  std::atomic<uint32_t> numIn_{0};
  std::atomic<uint64_t> dropped_{0};
  SpscRing<CaptureSegment> segments_;
  // Audio thread.
  struct Played {
    int64_t engine = 0, song = 0, len = 0;
  };
  std::array<Played, kPlayedRuns> played_{};  // ring: the newest at playedHead_ − 1
  uint32_t playedHead_ = 0, playedCount_ = 0;
  uint32_t playedSession_ = 0;
  int64_t attributed_ = INT64_MIN;  // played engine time up to which captured frames were placed
  void pushPiece(CaptureSet* set, int64_t engine, int64_t song, int64_t n, int64_t off, uint32_t session) noexcept;
  const float* const* in_ = nullptr;
  uint32_t numInNow_ = 0;
  int64_t blockTime_ = 0;
  bool monitoring_ = false;
  RecorderStats stats_;
};

}  // namespace pq::engine
