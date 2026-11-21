#pragma once

// Note stages (AP.1, docs/plans/AP.md "Where it sits", "Realtime and CPU"): notes in, notes out, on the audio thread,
// between a track slot's merged events (timeline + auditions + live input, engine.cpp onSlot) and its VoiceEngine:
//     merged events → stage 0 → stage 1 → … → VoiceEngine::process
// The arpeggiator (pq/engine/arp.h) is the first stage; later note devices (chord, scale, strum) use the same chain.
//
// ---- Cost --------------------------------------------------------------------------------------------------------
// A slot without a chain merges straight into the unit's EventBuffer as before (one relaxed load per block: no call,
// no copy). With a chain the merge goes into the chain's input buffer and every stage is ONE call per block through a
// function pointer (no virtual, no per-event or per-sample dispatch), writing into the next stage's preallocated
// buffer; the last stage writes into the unit's buffer. Every buffer is allocated when the chain is built.
//
// ---- Note ids ----------------------------------------------------------------------------------------------------
// A stage's generated notes use stageNoteId(index, n) = 0x80000000 | index << 24 | n (n < 2^24, wrapping; index =
// the stage's place in its chain, < kMaxNoteStages). Disjoint from every other producer of a slot: the timeline's
// instances stay below 0x80000000 (timeline.h), live notes are 0xE…, auditions 0xF… (live_input.h).
//
// ---- Clock -------------------------------------------------------------------------------------------------------
// NoteStageBlock::clock describes the block as segments of block frames: a Song segment plays song samples
// [pos, pos + len) at frames [frame, frame + len) (the timeline's TransportRuns); a Free segment is transport
// stopped or counting in. A loop wrap or a locate shows as a Song segment whose pos does not continue the previous
// one; a stop as Song → Free. Stages see positions only through this struct (and the tempo map it names), so the
// same stage runs offline over a clip's notes with segments the caller makes up (AP.7: preview == playback).
//
// ---- Handover (Engine::setNoteStages) ------------------------------------------------------------------------------
// A chain is built off the audio thread (stages added, buffers sized) and published per slot; the audio thread takes
// it at the start of the slot's next block. The chain it replaces is flushed at frame 0 of that block: its
// generated notes get note-offs (into the voices), and the source notes its first stage still holds under a key
// (NoteStage::flush `held`) are re-sent as note-ons into the new chain — or straight to the voices when the new chain
// is empty — so held keys keep sounding through a structural edit. AP.2b: when the new chain starts with the old
// one's stages (same device keys, same kinds) those stages ADOPT the old ones' running state instead
// (NoteStageChain::takeOver): an arp keeps its clock, step and lane positions, held set and sounding notes through a
// structural edit of itself (a lane switched on, a length, the seed) or of its chain's tail. The old chain is freed
// by Engine::collect().
// Params are not part of the handover: a stage reads its runtime params from NoteStageBlock::params (the project
// param block) at every block start (arp.h ArpStage::map), so a knob or automation never rebuilds a chain.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pq/core/time.h"
#include "pq/engine/note_events.h"

namespace pq::engine {

inline constexpr uint32_t kMaxNoteStages = 32;           // per chain (index < 0x60: ids stay below 0xE0000000)
inline constexpr uint32_t kStageNoteIdBit = 0x80000000u;  // the high bit: a generated note
inline constexpr uint32_t stageNoteId(uint32_t stageIndex, uint32_t n) noexcept {
  return kStageNoteIdBit | (stageIndex << 24) | (n & 0x00FFFFFFu);
}
inline constexpr bool isStageNoteId(uint32_t id) noexcept { return id >= kStageNoteIdBit && id < 0xE0000000u; }

struct ClockSegment {
  uint32_t frame = 0, len = 0;  // block frames [frame, frame + len)
  bool song = false;            // the transport plays song samples [pos, pos + len) there
  SampleTime pos = 0;           // Song: the song sample at `frame`
};

struct StageClock {
  const TempoMap* tempo = nullptr;  // the song's tempo map (required when a segment is Song)
  double sampleRate = 48000.0;
  double bpm = 120.0;               // the free clock's tempo (the song tempo at the playhead)
  const TimeSigMap* signatures = nullptr;  // AP.2b: the song's time signatures (null: 4/4 throughout)
  double beatsPerBar = 4.0;                // AP.2b: the free clock's bar, in quarter notes (the bar at the playhead)
  const ClockSegment* segments = nullptr;  // in frame order, covering [0, frames); none: one Free segment
  uint32_t numSegments = 0;
};

struct NoteStageBlock {
  int64_t time = 0;     // engine sample time of frame 0 (monotonic)
  uint32_t frames = 0;
  StageClock clock;
  std::span<const float> params;  // the project param block (runtime params, read at block start)
  std::span<const float> mods;    // AP.2b: the bus values (ProjectProgram::busValuesView: the previous block's)
};

// One stage kind. The chain calls `process` once per block (in: this block's events, sorted by frame; out: cleared
// by the chain) and `flush` once when the chain is replaced. process returns true while the stage holds notes or
// has notes sounding: the engine's idle gate stays awake for it. Both RT-safe.
using NoteStageProcessFn = bool (*)(void* stage, const NoteStageBlock& block, const EventList& in,
                                    EventBuffer& out) noexcept;
using NoteStageFlushFn = void (*)(void* stage, uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept;
// AP.2b: `stage` takes over the running state of `old` (the same kind: same adoptFn), keeping its own structure.
using NoteStageAdoptFn = void (*)(void* stage, void* old) noexcept;

// Base of a stage object: owns the stage's state for the chain's life (destroyed off the audio thread).
class NoteStage {
public:
  virtual ~NoteStage() = default;
  NoteStageProcessFn processFn = nullptr;
  NoteStageFlushFn flushFn = nullptr;
  // AP.2b: a stage with a key (the document's device id; 0: none) and an adoptFn continues an old chain's stage at
  // the same place with the same key and kind instead of restarting (NoteStageChain::takeOver).
  NoteStageAdoptFn adoptFn = nullptr;
  uint64_t key = 0;
  // Non-RT, called by NoteStageChain::add: the stage's place in its chain (its generated id space).
  virtual void setIndex(uint32_t index) = 0;
};

class NoteStageChain {
public:
  // Non-RT. `eventCapacity`: events per block of every buffer (ProjectConfig::eventCapacity).
  explicit NoteStageChain(uint32_t eventCapacity = 1024);
  NoteStageChain(const NoteStageChain&) = delete;
  NoteStageChain& operator=(const NoteStageChain&) = delete;

  // Non-RT: appends a stage (at most kMaxNoteStages; false beyond). Allocates its output buffer.
  bool add(std::unique_ptr<NoteStage> stage);
  uint32_t size() const noexcept { return static_cast<uint32_t>(stages_.size()); }
  NoteStage* stage(uint32_t i) noexcept { return stages_[i].get(); }

  // The buffer the slot's sources merge into when the chain is not empty. RT-safe.
  EventBuffer& input() noexcept { return in_; }
  // Runs input() → stages → `out` (appended). Returns true while any stage is busy. RT-safe.
  bool run(const NoteStageBlock& block, EventBuffer& out) noexcept;
  // The handover: every stage ends its generated notes at `frame` into `out`; stage 0's key-held source notes go to
  // `held` as note-ons at `frame`. RT-safe.
  void flush(uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept;
  // AP.2b, the handover with state: this chain replaces `old`. Its leading stages that match old's (same place, key
  // and adoptFn) take over their running state (clock, held set, sounding notes: nothing ends, nothing restarts);
  // old's stages after them are flushed (their notes end at `frame`); with no match the whole of `old` is flushed as
  // in flush (its key-held notes into `held`). RT-safe.
  void takeOver(NoteStageChain& old, uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept;

private:
  std::vector<std::unique_ptr<NoteStage>> stages_;
  std::vector<EventBuffer> mid_;  // stage i's output for i + 1 (size() − 1 of them)
  EventBuffer in_;
  uint32_t capacity_;
};

}  // namespace pq::engine
