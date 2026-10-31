#pragma once

// What a committed edit changed, and how expensive it is for the engine (T3.4; consumed by T3.17).
//
// Classes, cheapest first. A ChangeSet can carry several (a batch, a gesture); the engine link does the work of
// every class present, and Structural subsumes the others (a recompile rebuilds params, sequences and automation).
//   Meta        names, colours, clip signatures, locators, record arm: no engine work (views only).
//   Param       values in the param block (volume, pan, mute, solo, sends, continuous tuning params, device params): EngineCommand
//               param writes, listed in `params`. Never recompiles (SPEC §2).
//   Sequence    clip/note data: rebuild and publish the SequenceImage of the touched tracks (T3.16); T5.3: audio clips
//               (every field, also a tempo edit that re-derives warp-Off lengths): the tracks' audio sequences.
//   Automation  breakpoints of existing lanes: rebuild the AutomationImage of the touched lanes. Kept apart from
//               Sequence so a breakpoint drag does not rebuild note sequences (and vice versa).
//   Timeline    tempo map, time signatures, loop region: transport update.
//   Structural  graph shape (tracks, routing, sends' tap point, activation, lanes added/removed — an automated
//               param becomes an audio-rate signal — tuning slots and assignments, discrete tuning shape; T4.1:
//               device chains, device activation and device settings; T4.7: wires added / removed, a wire's
//               reducer; a wire's amount is a Param):
//               debounced recompile + hot swap.

#include <cstdint>
#include <vector>

#include "pq/document/ids.h"
#include "pq/document/model.h"

namespace pq::doc {

enum ChangeFlag : uint8_t {
  kChangeMeta = 1u << 0,
  kChangeParam = 1u << 1,
  kChangeSequence = 1u << 2,
  kChangeAutomation = 1u << 3,
  kChangeTimeline = 1u << 4,
  kChangeStructural = 1u << 5,
};

struct ChangeSet {
  uint8_t flags = 0;
  // Touched entities (sorted, unique after normalize()). Removed entities are listed too.
  std::vector<TrackId> tracks;
  std::vector<ClipId> clips;
  std::vector<NoteId> notes;
  std::vector<LaneId> lanes;
  std::vector<TuningId> tunings;
  std::vector<LocatorId> locators;
  std::vector<DeviceId> devices;  // T4.1: devices added, removed, moved or edited (their track is in `tracks` too)
  std::vector<WireId> wires;      // T4.7: wires added, removed or edited (a removed device's wires too)
  std::vector<SampleId> samples;  // T5.3: sample table entries added, removed or replaced
  std::vector<ParamRef> params;  // params whose value changed (Param class)

  bool empty() const noexcept { return flags == 0; }
  bool has(ChangeFlag f) const noexcept { return (flags & f) != 0; }
  bool structural() const noexcept { return has(kChangeStructural); }
  // Exactly one engine-relevant class (Meta may accompany it).
  bool paramOnly() const noexcept { return (flags & ~kChangeMeta) == kChangeParam; }
  bool sequenceOnly() const noexcept { return (flags & ~kChangeMeta) == kChangeSequence; }
  bool automationOnly() const noexcept { return (flags & ~kChangeMeta) == kChangeAutomation; }
  bool timelineOnly() const noexcept { return (flags & ~kChangeMeta) == kChangeTimeline; }
  bool metaOnly() const noexcept { return flags == kChangeMeta; }

  void mark(ChangeFlag f) noexcept { flags = static_cast<uint8_t>(flags | f); }
  // Union (for batches and gestures). Call normalize() when done.
  void merge(const ChangeSet& o);
  // Sorts and dedups the id lists.
  void normalize();
  void clear() noexcept;
};

}  // namespace pq::doc
