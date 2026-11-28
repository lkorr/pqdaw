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
// RT.2 (docs/plans/RT.md; "record from a track"): a capture set may also hold INTERNAL SOURCES — audio tracks recording a
// cable onto their Audio In. Each is a record source index (the program's ProjectLayout::busRecord: the track's clip
// slot) with two rings (a mono source duplicated), read after the units of every image block through the source reader
// (setSourceReader: the engine's, ProjectProgram::recordSource). Its capture offset is only the graph's latency
// compensation of the recording track (CaptureSet::setSourceOffset, frames: its unit's arrival; the signal is on the
// song timeline already — no device latency, no manual offset). A set with sources places its pieces after the units
// (captureSources, not capture) with ONE session offset, the largest of its channels' (the device channels' as below,
// each source's): every ring's frames go through its own history (kHistoryFrames, written every image block while it
// records) and are read at the engine time its own offset puts them, so every ring of a piece holds the same played song
// samples and the writer side is unchanged. A ring whose offset is more than kHistoryFrames − block below the session's
// reads silence. Without sources nothing of this runs (capture() places as before).
//
// ---- Record latency -------------------------------------------------------------------------------------------------
// offset = device input latency + device output latency (both incl. the BlockAdapter's FIFO; setDeviceLatency, the
// audio output sets them per open stream) + the master's PDC total (ProjectProgram::outputLatency, read per block) +
// the manual offset (setManualOffset: prefs, signed). An input frame captured at song sample s was played against song
// sample s − offset. noteOffset() = output latency + PDC: what a live note's player heard (notes have no input path).
// M6 review M7: capture uses the device latencies as they were when the session started (a measured WASAPI round trip
// that moves mid-take does not move the take); a setDeviceLatency during a session counts from the next one.
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
//   beginBlock, capture, captureSources, monitor                               the audio thread. RT-safe.
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

// The device input channels (RT.2: and internal sources) being captured and their rings (built off the audio thread).
class CaptureSet {
public:
  static constexpr uint32_t kHistoryFrames = 1u << 17;  // RT.2: per ring of a set with sources (≈ 2.7 s at 48 kHz)
  // `channels`: device input channel indices (each < kMaxChannels, distinct); RT.2 `sources`: internal record sources
  // (distinct), two rings each, after the channels' rings; `ringFrames` rounded up to a power of two. Allocates.
  CaptureSet(std::span<const uint16_t> channels, uint32_t ringFrames, uint32_t serial);
  CaptureSet(std::span<const uint16_t> channels, std::span<const uint32_t> sources, uint32_t ringFrames, uint32_t serial);
  std::span<const uint16_t> channels() const noexcept { return channels_; }
  std::span<const uint32_t> sources() const noexcept { return sources_; }
  size_t rings() const noexcept { return rings_.size(); }  // channels().size() + 2 · sources().size()
  FloatRing& ring(size_t i) noexcept { return *rings_[i]; }
  uint32_t serial() const noexcept { return serial_; }
  // Index of device channel `ch` in channels() (= its ring), or −1.
  int32_t indexOf(uint16_t ch) const noexcept;
  // RT.2: the ring of channel `c` (0, 1) of internal source `source`, or −1.
  int32_t indexOfSource(uint32_t source, uint16_t c) const noexcept;
  // RT.2: internal source j's capture offset, frames (any thread; the recorder takes it at a session's start).
  void setSourceOffset(size_t j, uint32_t frames) noexcept {
    if (j < sources_.size()) offsets_[j].store(frames, std::memory_order_relaxed);
  }
  uint32_t sourceOffset(size_t j) const noexcept {
    return j < sources_.size() ? offsets_[j].load(std::memory_order_relaxed) : 0u;
  }
  // RT.2, audio thread: ring i's history (kHistoryFrames floats, indexed by engine time; null without sources).
  float* history(size_t i) noexcept { return history_.size() != 0 ? history_.data() + i * kHistoryFrames : nullptr; }

private:
  std::vector<uint16_t> channels_;
  std::vector<uint32_t> sources_;
  std::vector<std::unique_ptr<FloatRing>> rings_;
  std::unique_ptr<std::atomic<uint32_t>[]> offsets_;
  AlignedBuffer<float> history_;
  uint32_t serial_ = 0;
};
// RT.2: reads channel `channel` (0, 1) of internal source `source` for the image block just run (null: silence).
// RT-safe.
using CaptureSourceFn = const float* (*)(void* user, uint32_t source, uint16_t channel) noexcept;

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
  // RT.2: the internal sources' reader (the engine's; before the audio thread runs).
  void setSourceReader(CaptureSourceFn fn, void* user) noexcept {
    sourceFn_ = fn;
    sourceUser_ = user;
  }
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
  // RT.2: the same image block after its units ran: with a capture set that has internal sources, every ring's frames
  // into its history and the block's pieces placed (see "Capture"); nothing otherwise. RT-safe.
  void captureSources(const Timeline& timeline, int64_t time, uint32_t frames, uint32_t pdc) noexcept;
  // Writes the monitor gates for this image block into `params`; true when one changed. RT-safe.
  bool monitor(const Timeline& timeline, std::span<float> params) noexcept;
  // A gate was open in the last monitor() (the engine's idle gate then stays awake: the input must be heard).
  bool monitoring() const noexcept { return monitoring_; }
  // A snapshot, from any thread (M6 review L12: the counters are relaxed atomics only the audio thread writes).
  RecorderStats stats() const noexcept {
    auto get = [](const std::atomic<uint64_t>& a) noexcept { return a.load(std::memory_order_relaxed); };
    return RecorderStats{get(stats_.segments), get(stats_.capturedFrames), get(stats_.droppedFrames),
                         get(stats_.droppedSegments), get(stats_.monitorWrites)};
  }

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
  uint32_t sessionIn_ = 0, sessionOut_ = 0;  // M6 review M7: the device latencies frozen at the session's start
  void pushPiece(CaptureSet* set, int64_t engine, int64_t song, int64_t n, int64_t off, uint32_t session) noexcept;
  // The pieces of the played window [time − off, time + frames − off) (see "Capture"). RT-safe.
  void place(const Timeline& tl, CaptureSet* set, int64_t time, uint32_t frames, int64_t off, uint32_t session) noexcept;
  // RT.2: ring i's capture offset in the current session (a device channel's: devOff_; a source's: its frozen offset).
  int64_t ringOffset(const CaptureSet& set, size_t i, int64_t off) const noexcept;
  static constexpr size_t kMaxSources = 64;  // RT.2: internal sources whose offset a session freezes
  std::array<uint32_t, kMaxSources> sessionSrc_{};
  bool sessionSrcFrozen_ = false;  // RT.2: sessionSrc_ holds this session's offsets
  int64_t devOff_ = 0;          // RT.2: the device channels' offset in the block captureSources places
  uint32_t blockFrames_ = 0;    // RT.2: that block's frames
  CaptureSourceFn sourceFn_ = nullptr;
  void* sourceUser_ = nullptr;
  const float* const* in_ = nullptr;
  uint32_t numInNow_ = 0;
  int64_t blockTime_ = 0;
  bool monitoring_ = false;
  struct Counters {
    std::atomic<uint64_t> segments{0}, capturedFrames{0}, droppedFrames{0}, droppedSegments{0}, monitorWrites{0};
  };
  static void bump(std::atomic<uint64_t>& a, uint64_t n = 1) noexcept {  // RT-safe: one writer, no locked RMW
    a.store(a.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
  }
  Counters stats_;
};

}  // namespace pq::engine
