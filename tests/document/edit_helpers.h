#pragma once

// Helpers for the edit-operation tests (T3.6): dispatching an Edit as exactly one undoable history entry, and the
// "played notes" of a track (what the clips actually play, in absolute time) to check that edits keep absolute time
// and pitch identity.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "doc_fixture.h"
#include "pq/document/automation_curve.h"
#include "pq/document/clip_time.h"
#include "pq/document/edit_ops.h"

namespace edit_test {

using namespace pq::doc;
using pq::Ticks;

inline constexpr Ticks kBar = pq::kTicksPerWhole;

// Dispatches the edit and checks the T3.6 contract: exactly one history entry, undo restores the identical
// snapshot pointer, redo the identical result. Returns the snapshot before the edit.
inline Snapshot commit(DocumentStore& store, const pq::Result<ops::Edit>& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  const Snapshot before = store.current();
  const size_t entries = store.history().cursor();
  const pq::Status st = store.dispatch(e->command);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  const Snapshot after = store.current();
  if (after == before) {
    CHECK(store.history().cursor() == entries);  // a no-op records nothing
    return before;
  }
  CHECK(store.history().cursor() == entries + 1);
  REQUIRE(store.undo());
  CHECK(store.current() == before);
  REQUIRE(store.redo());
  CHECK(store.current() == after);
  return before;
}

// One note as the track plays it.
struct Occ {
  Ticks start = 0;
  Ticks length = 0;
  pq::tuning::PitchId pitch;
  bool enabled = true;
  float velocity = 0.0f;
};
inline bool occLess(const Occ& a, const Occ& b) {
  if (a.start != b.start) return a.start < b.start;
  if (a.length != b.length) return a.length < b.length;
  const int c = std::memcmp(&a.pitch, &b.pitch, sizeof a.pitch);
  if (c != 0) return c < 0;
  return a.velocity < b.velocity;
}
inline bool operator==(const Occ& a, const Occ& b) {
  return a.start == b.start && a.length == b.length && a.pitch == b.pitch && a.enabled == b.enabled &&
         a.velocity == b.velocity;
}

inline std::vector<Occ> played(const Track& t) {
  std::vector<Occ> out;
  for (const ClipSlot& s : t.clips) {
    const NoteClip& c = *s.clip;
    forEachClipSegment(c, c.start, clipEnd(c), [&](const ClipSegment& seg) {
      for (auto [it, end] = c.notes.range(seg.contentFrom, seg.contentTo); it != end; ++it)
        out.push_back({seg.at + (it->start - seg.contentFrom), std::min(it->length, seg.contentTo - it->start),
                       it->pitch, it->enabled() && c.active, it->velocity});
    });
  }
  std::sort(out.begin(), out.end(), occLess);
  return out;
}

inline std::vector<Occ> sorted(std::vector<Occ> v) {
  std::sort(v.begin(), v.end(), occLess);
  return v;
}

inline const AutomationLane& laneOf(const Project& p, TrackId t, LaneId l) {
  const Track* tr = findTrackPtr(p, t);
  REQUIRE(tr);
  for (const LanePtr& lp : tr->lanes)
    if (lp->id == l) return *lp;
  FAIL("lane not found");
  return *tr->lanes.front();
}

inline std::vector<uint32_t> allNoteIds(const Project& p) {
  std::vector<uint32_t> ids;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const ClipSlot& s : t.clips)
      for (const Note& n : s.clip->notes) ids.push_back(n.id.value);
  });
  std::sort(ids.begin(), ids.end());
  return ids;
}

inline bool idsUnique(const Project& p) {
  std::vector<uint32_t> ids = allNoteIds(p);
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const ClipSlot& s : t.clips) ids.push_back(s.id.value);
    for (const LanePtr& l : t.lanes)
      for (const Breakpoint& b : l->points) ids.push_back(b.id.value);
  });
  std::sort(ids.begin(), ids.end());
  return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

}  // namespace edit_test
