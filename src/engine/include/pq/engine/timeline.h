#pragma once

// The engine timeline (T3.16): the audio-thread transport, the note sequencer and the automation renderer.
//
// ---- Threads -------------------------------------------------------------------------------------------------
//   publishSequence / publishAutomation / publishTempo, collect      any non-RT thread (one at a time)
//   post (commands), poll (reports)                                  the UI thread (one producer, one consumer)
//   process, apply, blockHook, eventSource, accessors                the audio thread (or an offline render loop)
// Images (SequenceImage, AutomationImage, TempoImage) are immutable and built off the audio thread. publish*()
// swaps an atomic pointer and retires the old image to an EpochReclaimer; the audio thread loads the pointers at
// the start of every block (never holding an old one across that point) and announces a quiescent state at the end
// of process(), so collect() frees retired images off the audio thread.
//
// ---- Transport -------------------------------------------------------------------------------------------------
// The song position is an int64 sample on the tempo map (TempoMap::ticksToSamples of the tick). Each block plays
// the song samples [P, P + frames) and the ticks [T0, T1) with T1 = firstTickAtOrAfterSample(P + frames), so a note
// at tick t sounds at exactly ticksToSamples(t), whatever the block size, and consecutive blocks partition the
// ticks. Play(t) / Locate(t) set P = ticksToSamples(t) and start the tick range at exactly t; Stop holds the
// position (Continue resumes there). Commands apply at the start of the next block (frame 0).
// Loop (arrangement loop [start, end) in ticks, Ableton semantics): when the playhead crosses ticksToSamples(end)
// from before it, the block is split at that sample — the wrap frame — and continues at ticksToSamples(start); ticks
// after firstTickAtOrAfterSample(loop end sample) (they round onto the wrap sample) are not played. A playhead
// already past the loop end plays on. Any number of wraps per block (a loop shorter than a block).
// A new tempo map keeps the musical position (the next tick to play) and recomputes the sample position from it.
// The ProcessContext snapshot (TransportSnapshot) describes frame 0 of the block: beat = fractional ticks / PPQ
// (quarter notes), bpm at that tick, beatsPerFrame = bpm / 60 / sr while playing, beatsPerBar and barStartBeat from
// the time-signature map. A loop wrap inside the block is not visible to transport.beat's linear ramp (it is from the
// next block on).
//
// ---- Note events -------------------------------------------------------------------------------------------------
// Per track (the image's track index = the event slot; T3.17 wires slot k to instrument k): note-ons from the
// cursor, note-offs from a per-track heap of sounding notes, written in frame order into that track's event list
// (equal frames: offs before ons). A note-off at min(on + length, end of its clip pass). At a loop wrap, a locate or
// a stop, every sounding note gets a note-off at that frame (so locate releases all sounding voices; voices from other
// sources, e.g. live MIDI on the same instrument, are left alone). Note ids in events are per-track instance numbers
// (unique among sounding notes), not document ids.
// Chance and velocity range: SequenceImage's deterministic per-note-and-pass RNG (sequence.h).
// Image swap (sequence): sounding notes follow their track id. A sounding note that is unchanged in the new image
// (same id, same start, same Hz; findOccurrence) keeps sounding, with its note-off recomputed from the new length;
// one that was removed or changed gets a note-off at frame 0 of the first block of the new image. Notes of a track
// that disappeared are dropped (counted: its instrument goes away with the recompile). Cursors re-seek at the
// current tick: notes starting from it on play from the new image; notes that started earlier are not chased.
// Capacities: maxSoundingPerTrack (beyond it the earliest-ending note is cut early, counted) and maxEventsPerTrack
// per block (a note-on is only emitted while room remains for every pending note-off; dropped ons are counted).
//
// ---- Automation ----------------------------------------------------------------------------------------------
// AutomationRenderer (automation.h) renders every lane per sample for the block's samples (same segments as the
// notes), or holds the value at the playhead while stopped. writeAutomatedParams() writes each lane's frame-0 value
// into a param block for param-rate consumers; automation(lane) is the audio-rate buffer.
//
// ---- Integration -----------------------------------------------------------------------------------------------
// process(sampleTime, frames) runs once per engine block (calls with the same engine sampleTime are no-ops, so the
// hooks below can all trigger it). blockHook() is an ImageRunner/ProgramHost/renderOffline BlockHook: it advances the
// timeline, writes the snapshot into ProcessContext::transport and the automated params. eventSource(k) is the
// EventSource for the VoiceEngine of track k: it copies track k's events of the block.

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/core/result.h"
#include "pq/core/spsc_ring.h"
#include "pq/core/time.h"
#include "pq/engine/automation.h"
#include "pq/engine/image_runner.h"
#include "pq/engine/note_events.h"
#include "pq/engine/program_image.h"
#include "pq/engine/sequence.h"

namespace pq::engine {

// The tempo and time-signature maps, as one immutable image.
class TempoImage {
public:
  TempoImage();  // 120 BPM, 4/4
  // Off the audio thread. Same rules as TempoMap::create / TimeSigMap::create (empty spans: the defaults).
  static Result<std::unique_ptr<TempoImage>> create(std::span<const TempoPoint> tempo,
                                                    std::span<const TimeSigChange> signatures);
  const TempoMap& tempo() const noexcept { return tempo_; }
  const TimeSigMap& signatures() const noexcept { return sig_; }
  // True when the tempo is one constant over [t0, t1] (both in one segment that does not ramp). RT-safe.
  bool constantBetween(double t0, double t1) const noexcept;
  // The tempo segment containing t: [*from, *to) (to = INT64_MAX for the last one); true when its BPM is constant.
  // RT-safe.
  bool segmentAt(Ticks t, Ticks* from, Ticks* to) const noexcept;

private:
  TempoMap tempo_;
  TimeSigMap sig_;
  std::vector<Ticks> pointTick_;   // segment starts
  std::vector<uint8_t> ramps_;     // per segment: the BPM changes inside it
};

enum class TransportCommandKind : uint8_t {
  Play,      // locate to `tick` and play
  Stop,      // stop, holding the position
  Continue,  // play from the held position
  Locate,    // move to `tick` (playing or not)
  SetLoop,   // arrangement loop [loopStart, loopEnd), on when loopEnabled and loopEnd > loopStart
};

struct TransportCommand {
  TransportCommandKind kind = TransportCommandKind::Stop;
  bool loopEnabled = false;
  Ticks tick = 0;
  Ticks loopStart = 0, loopEnd = 0;
};

// What the UI hears back (lossy: when the ring is full the report is dropped; the UI keeps the newest it polled).
struct TransportReport {
  uint64_t block = 0;          // timeline blocks processed before this one
  Ticks tick = 0;              // the playhead at frame 0 of the block (the next tick to play)
  SampleTime songSample = 0;   // the same position in samples
  double bpm = 120.0;
  uint64_t loopWraps = 0;
  uint64_t commands = 0;       // commands taken from post() so far (T3.17: the UI knows which of its commands this
                               // report already reflects)
  bool playing = false;
};

struct TimelineConfig {
  double sampleRate = 48000.0;
  uint32_t maxBlockFrames = 1024;
  uint32_t maxTracks = 64;              // images with more tracks are refused
  uint32_t maxEventsPerTrack = 512;     // note events per track per block
  uint32_t maxSoundingPerTrack = 256;   // notes sounding at once per track
  uint32_t maxAutomationLanes = 256;    // images with more lanes are refused
  uint32_t commandCapacity = 64;        // UI → audio commands in flight (power of two)
  uint32_t reportCapacity = 64;         // audio → UI reports (power of two)
  uint32_t reportEveryFrames = 256;     // while playing: a report at most every this many frames (0: every block);
                                        // transport commands always report at the next block
};

struct TimelineStats {
  uint64_t blocks = 0;
  uint64_t noteOns = 0, noteOffs = 0;
  uint64_t chanceSkips = 0;        // note-ons skipped by chance
  uint64_t droppedNoteOns = 0;     // no room in the track's event list this block
  uint64_t earlyOffs = 0;          // maxSoundingPerTrack exceeded: the earliest-ending note was cut
  uint64_t orphanedNotes = 0;      // sounding notes of a track missing from a new image
  uint64_t keptOnSwap = 0, stoppedOnSwap = 0;
  uint64_t sequenceSwaps = 0, automationSwaps = 0, tempoSwaps = 0;
  uint64_t loopWraps = 0;
  uint64_t droppedReports = 0;
};

class Timeline {
public:
  // Non-RT: allocates every buffer, ring and the reclaimer, registers the audio thread as its reader.
  explicit Timeline(const TimelineConfig& config);
  // Non-RT, after the audio thread stopped calling process(). Frees every image.
  ~Timeline();
  Timeline(const Timeline&) = delete;
  Timeline& operator=(const Timeline&) = delete;

  // ---- Non-RT (one publishing thread at a time) ----
  // Errors: null image, more tracks / lanes than configured.
  Status publishSequence(std::unique_ptr<SequenceImage> image);
  Status publishAutomation(std::unique_ptr<AutomationImage> image);
  Status publishTempo(std::unique_ptr<TempoImage> image);
  // Frees retired images every reader has passed; returns how many. publish*() calls it too.
  size_t collect();
  size_t pendingImages() const { return reclaimer_.pending(); }

  // ---- UI thread ----
  bool post(const TransportCommand& command) noexcept;  // false when the ring is full. RT-safe (wait-free)
  bool poll(TransportReport& report) noexcept;          // the oldest unread report. RT-safe (wait-free)

  // ---- Audio thread ----
  // One engine block starting at engine time `sampleTime` (frames ≤ maxBlockFrames). A second call with the same
  // sampleTime does nothing (several hooks may trigger it). RT-safe.
  void process(int64_t sampleTime, uint32_t frames) noexcept;
  // Applies a command now (audio thread, or any thread while process() is not running: tests, offline). It acts at
  // frame 0 of the next block. RT-safe.
  void apply(const TransportCommand& command) noexcept;
  BlockHook blockHook() noexcept { return {&Timeline::onBlockHook, this}; }
  EventSource eventSource(uint32_t track) noexcept { return {&Timeline::onEventSource, &sources_[track]}; }
  // Lets collect() free images while the audio thread is not running (device stopped). RT-safe.
  void audioOffline() noexcept { reclaimer_.goOffline(reader_); }
  void audioOnline() noexcept { reclaimer_.goOnline(reader_); }

  // Results of the last block (audio thread). RT-safe.
  EventList events(uint32_t track) const noexcept {
    return {events_.data() + static_cast<size_t>(track) * cfg_.maxEventsPerTrack, eventCount_[track]};
  }
  uint32_t numTracks() const noexcept { return numTracks_; }
  const TransportSnapshot& snapshot() const noexcept { return snapshot_; }
  const AutomationRenderer& automation() const noexcept { return automation_; }
  // params[target] = frame-0 value, for every lane whose target < params.size(). RT-safe.
  void writeAutomatedParams(std::span<float> params) const noexcept;

  // Audio-thread state (tests, inspection). RT-safe.
  bool playing() const noexcept { return playing_; }
  Ticks position() const noexcept;                         // the next tick to play
  SampleTime songSample() const noexcept { return pos_; }  // its sample
  uint32_t soundingNotes(uint32_t track) const noexcept { return tracks_[track].heapSize; }
  const TimelineStats& stats() const noexcept { return stats_; }
  const SequenceImage* sequence() const noexcept { return seq_; }
  const TempoImage& tempoImage() const noexcept { return *tempo_; }

private:
  static constexpr Ticks kNever = INT64_MAX;

  struct Sounding {
    Ticks off;
    uint64_t order;       // tie-break: earlier note-ons end first at equal ticks
    Ticks on;
    uint32_t instance;    // the event note id
    uint32_t noteId;      // the document note id
    float hz;
    float releaseVelocity;
  };
  struct Cursor {
    uint32_t clip = 0, clipEnd = 0;  // into the image's clips
    int64_t pass = 0;
    Ticks passStart = 0, passContent = 0, passEnd = 0;
    uint32_t note = 0, noteEnd = 0;
    Ticks nextOn = kNever;
  };
  struct TrackState {
    Cursor cur;
    Sounding* heap = nullptr;  // min-heap by (off, order), capacity maxSoundingPerTrack
    uint32_t heapSize = 0;
    uint32_t id = 0;
    uint64_t order = 0;
  };
  struct SourceSlot {
    Timeline* timeline;
    uint32_t track;
  };

  static void onBlockHook(void* self, int64_t sampleTime, uint32_t frames, TransportSnapshot& transport,
                          std::span<float> params) noexcept;
  static void onEventSource(void* slot, int64_t sampleTime, uint32_t frames, EventBuffer& out) noexcept;

  void drainCommands() noexcept;
  void pickUpImages() noexcept;
  void swapSequence(const SequenceImage* next) noexcept;
  void swapTempo(const TempoImage* next) noexcept;
  void updateLoopSamples() noexcept;
  void releaseAll(uint32_t frame) noexcept;           // note-offs for every sounding note at `frame`
  void seekAll(Ticks t) noexcept;
  void seek(uint32_t track, Ticks t) noexcept;
  void setupPass0(Cursor& c) noexcept;
  void computeNextOn(Cursor& c) noexcept;
  void playBlock(uint32_t frames) noexcept;
  Ticks resolveTick() noexcept;                       // nextTick_, computing it from pos_ when lazy
  void refreshMinNext() noexcept;
  void playTrack(uint32_t track, uint32_t frame, SampleTime s0, uint32_t len, Ticks t0, Ticks t1) noexcept;
  void renderAutomation(uint32_t frame, SampleTime s0, uint32_t len, bool wholeBlock) noexcept;
  void computeSnapshot() noexcept;
  // Event output.
  bool pushEvent(uint32_t track, const NoteEvent& e) noexcept;
  void emitOff(uint32_t track, const Sounding& s, uint32_t frame) noexcept;
  // Heap.
  void heapPush(TrackState& t, const Sounding& s) noexcept;
  Sounding heapPop(TrackState& t) noexcept;
  void refreshNext(uint32_t track) noexcept;

  TimelineConfig cfg_;
  // Publication.
  std::atomic<SequenceImage*> pubSeq_{nullptr};
  std::atomic<AutomationImage*> pubAuto_{nullptr};
  std::atomic<TempoImage*> pubTempo_{nullptr};
  EpochReclaimer reclaimer_{1};
  EpochReclaimer::ReaderId reader_{0};
  SpscRing<TransportCommand> commands_;
  SpscRing<TransportReport> reports_;
  std::unique_ptr<TempoImage> defaultTempo_;

  // Audio thread: current images.
  const SequenceImage* seq_ = nullptr;
  const AutomationImage* auto_ = nullptr;
  const TempoImage* tempo_ = nullptr;
  // Transport.
  bool playing_ = false;
  bool releasePending_ = false;  // release every sounding note at the next block's frame 0 (stop, locate, wrap)
  bool seekPending_ = false;     // re-seek the cursors at nextTick_ before the next block plays
  bool snapshotDirty_ = true;
  bool reportPending_ = true;
  bool holdValid_ = false;       // stopped: the automation buffers hold the values at the playhead
  bool loopEnabled_ = false, loopOn_ = false;
  Ticks loopStart_ = 0, loopEnd_ = 0;
  SampleTime loopStartSample_ = 0, loopEndSample_ = 0;
  SampleTime pos_ = 0;           // song sample at frame 0 of the next block
  Ticks nextTick_ = 0;           // first tick of the next block's range (when !tickLazy_)
  bool tickLazy_ = false;        // nextTick_ is firstTickAtOrAfterSample(pos_), not computed yet
  int64_t lastSampleTime_ = INT64_MIN;
  TransportSnapshot snapshot_;
  Ticks barStartTick_ = 0, barEndTick_ = 0;  // the bar containing the last snapshot's tick
  double beatsPerBar_ = 4.0;
  Ticks bpmFrom_ = 0, bpmTo_ = 0;            // the constant-tempo segment of the last snapshot (empty: none)
  double bpmConst_ = 120.0;
  uint64_t framesSinceReport_ = UINT64_MAX / 2;
  uint64_t commandsPosted_ = 0;  // commands drained from the post() ring
  // Sequencer.
  uint32_t numTracks_ = 0;
  uint32_t nextInstance_ = 1;    // event note ids: unique across tracks (wraps after 2^32 notes, skipping 0)
  std::vector<TrackState> tracks_;
  std::vector<Ticks> next_;      // per track: min(next note-on, next note-off) — the per-track fast path
  SampleTime minNextSample_ = INT64_MAX;  // ticksToSamples(min over tracks of next_) — the per-block fast path
  bool minNextDirty_ = true;
  AlignedBuffer<Sounding> heapPool_[2];
  uint32_t heapPoolSide_ = 0;
  std::vector<TrackState> scratchTracks_;
  std::vector<uint8_t> matchedScratch_;
  AlignedBuffer<NoteEvent> events_;
  std::vector<uint32_t> eventCount_;
  bool anyEvents_ = false;
  std::vector<SourceSlot> sources_;
  // Automation.
  AutomationRenderer automation_;
  AlignedBuffer<double> tickBuf_;  // per-sample ticks while the tempo ramps
  SampleTime exactFrom_ = 0;               // the run TickSpan::exact describes
  SampleTime lastTickSample_ = INT64_MIN;  // samplesToTicks cache: the last segment's end
  double lastTickValue_ = 0.0;
  TimelineStats stats_;
};

}  // namespace pq::engine
