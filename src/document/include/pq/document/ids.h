#pragma once

// Stable ids of document entities (SPEC §2, T3.4). Every entity kind has its own typed id so ids of different
// kinds cannot be mixed up at compile time, but all of them are drawn from ONE project-wide counter (IdSource):
// a raw id value names exactly one entity in a project, which keeps ChangeSets, ParamRefs and engine state keyed
// by raw ids unambiguous.
//
// Ids are allocated by the DocumentStore's IdSource *before* a command is built, so a command is a pure function of
// (snapshot, command data) and ids are never reused, not even after undo: an undone track's id never names a
// different track later (engine state transfer is keyed by ids, T3.17).

#include <cassert>
#include <cstdint>

#include "pq/core/stable_id.h"

namespace pq::doc {

struct TrackTag {};
struct ClipTag {};
struct NoteTag {};
struct LaneTag {};
struct BreakpointTag {};
struct TuningTag {};
struct LocatorTag {};
struct DeviceTag {};  // T4.1: a device instance in a track's chain

using TrackId = StableId<TrackTag>;
using ClipId = StableId<ClipTag>;
using NoteId = StableId<NoteTag>;
using LaneId = StableId<LaneTag>;
using BreakpointId = StableId<BreakpointTag>;
using TuningId = StableId<TuningTag>;
using LocatorId = StableId<LocatorTag>;
using DeviceId = StableId<DeviceTag>;

// The project-wide id counter. Not thread-safe (owned by the UI thread's DocumentStore).
class IdSource {
public:
  IdSource() noexcept = default;

  template <class Tag>
  StableId<Tag> next() noexcept {
    assert(next_ != 0 && "IdSource exhausted");
    return StableId<Tag>{next_++};
  }
  TrackId track() noexcept { return next<TrackTag>(); }
  ClipId clip() noexcept { return next<ClipTag>(); }
  NoteId note() noexcept { return next<NoteTag>(); }
  LaneId lane() noexcept { return next<LaneTag>(); }
  BreakpointId breakpoint() noexcept { return next<BreakpointTag>(); }
  TuningId tuning() noexcept { return next<TuningTag>(); }
  LocatorId locator() noexcept { return next<LocatorTag>(); }
  DeviceId device() noexcept { return next<DeviceTag>(); }

  // The value next() would hand out.
  uint32_t peek() const noexcept { return next_; }
  // Never hand out ids ≤ raw (after loading a project: reserveThrough(maxEntityId(project))).
  void reserveThrough(uint32_t raw) noexcept {
    if (raw >= next_) next_ = raw + 1;
  }

private:
  uint32_t next_ = 1;
};

}  // namespace pq::doc
