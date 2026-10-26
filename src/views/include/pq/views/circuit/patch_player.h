#pragma once

// Where the circuit editor's compiled patch plays (T4.4). Plain data out of the views (which never include the
// engine): the editor hands every schedule that compiled cleanly to the player; the app's implementation
// (pq/app/patch_preview.h: a ProgramHost on the audio output) builds the image off the audio thread and publishes it
// through the lock-free hot-swap path (T1.9: state transfer by state key, a 5–20 ms linear crossfade while audible).
// A schedule that never arrives (a parse or check error) leaves the last good program playing.
//
// Seam for T4.1 / T4.13: a device in a track's chain is edited by the same PatchEditor; there the "player" is the
// document: PatchEditor::onCommitted hands the new text to a `SetDevicePatch` command and the project's per-track
// compile (ProjectLink) swaps the unit, so this interface is only the stand-alone preview's.

#include "pq/core/result.h"
#include "pq/graph/schedule.h"

namespace pq::views::circuit {

class PatchPlayer {
public:
  virtual ~PatchPlayer() = default;
  // The rate schedules must be compiled at (OptimiseOptions / ScheduleOptions::sampleRate).
  virtual double sampleRate() const = 0;
  // Starts playing `schedule` (`scheduled` = the TypedPatch its NodeIndices refer to: Compiled::scheduled()), or hot
  // swaps the playing program for it. An error leaves whatever played before playing.
  virtual Status play(const graph::Schedule& schedule, const graph::TypedPatch& scheduled) = 0;
  virtual void stop() = 0;
  virtual bool playing() const = 0;
  // The state layout of the program published last (ScheduleOptions::previous: unchanged state keeps its offsets,
  // so the swap's transfer is a few coalesced copies), or null.
  virtual const graph::StateLayout* stateLayout() const = 0;
};

}  // namespace pq::views::circuit
