#pragma once

// AP.7 (docs/plans/AP.md "Roll preview + Flatten"): note stages run OFFLINE — the roll preview and Flatten to clip
// play a track's sequence through its note-stage chain off the audio thread, with exactly what the engine gives the
// stages while it plays:
//   the events   a private Timeline plays the SequenceImage (chance, velocity range, curves, loop passes: the same
//                sequencer code), merged per block as Engine::onSlot merges them (no live input, no auditions)
//   the clock    the block's song runs as ClockSegments (timelineSegments: the one function Engine::runStages uses
//                too), the tempo / signature maps, the snapshot's bpm and bar
//   the params   a param block the caller fills (the stages' mapped slots: their knob positions)
// So a deterministic stage (ArpCore: counter RNG, song-grid clock) produces here the events playback produces from the
// same transport start, sample for sample, at any block size. Non-RT (allocates the timeline and its buffers); any
// thread but the audio thread.

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/engine/note_events.h"
#include "pq/engine/note_stage.h"
#include "pq/engine/timeline.h"

namespace pq::engine {

// The block's clock segments from the timeline's transport runs (Song runs as Song segments, Free between them):
// what Engine::runStages hands its stages. `segs` holds at least 2 · Timeline::kMaxRuns + 1. Returns the count. RT-safe.
uint32_t timelineSegments(std::span<const TransportRun> runs, uint32_t frames, ClockSegment* segs) noexcept;
// AP.8a (review L9): the block a note-stage chain sees from timeline `tl` after its process() — engine time, frames,
// the clock (tempo map, signatures, snapshot bpm and bar, the song runs as segments written into `segs`, a tempo
// remap) — the ONE setup Engine::runStages and renderNoteStages share; params / mods are the caller's. RT-safe.
NoteStageBlock timelineStageBlock(const Timeline& tl, int64_t time, uint32_t frames, double sampleRate,
                                  ClockSegment* segs) noexcept;

// One event of a render, at song sample `song` (−1: the transport was stopped there).
struct TimedNoteEvent {
  SampleTime song = 0;
  NoteEvent e;  // e.frame: its frame in the block it came in (unused)
};

struct NoteStageRenderConfig {
  double sampleRate = 48000.0;
  uint32_t blockFrames = 512;
  // The sequencer's limits (EngineConfig::timeline: events / sounding notes per track); its sample rate, block size,
  // track and clip-player sizes are set here (one track, no audio clips).
  TimelineConfig timeline;
};

struct NoteStageRender {
  std::vector<TimedNoteEvent> input;   // what the sequencer gave the chain
  std::vector<TimedNoteEvent> output;  // what the chain gave the voices
  std::vector<std::pair<uint32_t, uint32_t>> notes;  // per sequencer note-on: (its event id, its NoteDesc::id)
};

// Plays track 0 of `sequence` from song tick `from` for `frames` samples through `chain` (whose stages read `params`
// at every block start), starting at engine time 0. Errors: the timeline refused an image.
Result<NoteStageRender> renderNoteStages(std::unique_ptr<SequenceImage> sequence, std::unique_ptr<TempoImage> tempo,
                                         NoteStageChain& chain, std::span<const float> params, Ticks from,
                                         SampleTime frames, const NoteStageRenderConfig& config = {});

}  // namespace pq::engine
