#pragma once

// Internals shared by the edit-operation builders (T3.6). Private to pq_doc.

#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "checks.h"
#include "pq/document/automation_curve.h"
#include "pq/document/clip_time.h"
#include "pq/document/edit_ops.h"

namespace pq::doc::ops::detail {

using namespace pq::doc::detail;  // invalid(), notFound(), ok(), PQ_DOC_TRY, check*()

Edit makeEdit(std::string label);
Batch& batchOf(Edit& e);

// ---- Lookups ---------------------------------------------------------------------------------------------------

struct ClipFound {
  TrackPath path;
  const Track* track = nullptr;
  ClipPtr clip;
};
Result<ClipFound> findClip(const Project& p, const ClipRef& r);
Result<const Track*> findMidiTrack(const Project& p, TrackId id);
const AutomationLane* findLane(const Track& t, LaneId id);
// The clip's effective tuning, built (allocates for MOS/Scale).
Result<tuning::Tuning> clipTuning(const Project& p, const ClipFound& c);

// ---- Clips as values ---------------------------------------------------------------------------------------------

// Notes sequences sharing every chunk (cheap identity test; content may still be equal when this is false).
bool sameNotes(const NoteSeq& a, const NoteSeq& b) noexcept;
// The clip with new arrangement bounds [start, end), every note keeping its absolute time (Ableton's start marker
// moves with the left edge). end > start.
NoteClip withBounds(const NoteClip& c, Ticks start, Ticks end);
// Splits at arrangement tick t (start < t < end). Left keeps the id; right gets `rightId`.
std::pair<NoteClip, NoteClip> splitClipAt(const NoteClip& c, Ticks t, ClipId rightId, IdSource& ids);
// Copy with fresh clip and note ids. `onlyPlaying`: for a non-looped clip, copy only the notes that start inside
// the played content window (hidden notes are dropped).
NoteClip cloneClip(const NoteClip& c, IdSource& ids, bool onlyPlaying);
// Builds a sequence from notes known to be valid with unique ids.
NoteSeq seqOf(std::vector<Note> notes);

// A working copy of one track's clip list; the final list is diffed against the track to emit commands.
class ClipEditor {
public:
  explicit ClipEditor(const Track& t);
  TrackId id() const noexcept { return track_->id; }
  const Track& track() const noexcept { return *track_; }

  std::vector<ClipPtr>& clips() noexcept { return clips_; }
  ClipPtr find(ClipId id) const;
  void remove(ClipId id);
  void add(ClipPtr c) { clips_.push_back(std::move(c)); }
  void replace(ClipId id, ClipPtr c);
  // Overwrites [a, b): covered clips are removed, partly covered ones trimmed or split (new ids for right pieces).
  // Clips whose id is in `keep` are left alone.
  void clearRange(Ticks a, Ticks b, IdSource& ids, std::span<const ClipId> keep = {});
  // Appends commands turning the track's clips into the working list: RemoveClip to `removes`, SetClipBounds to
  // `updates`, AddClip to `adds` (changed clips that are more than a bounds change are removed and re-added).
  void emit(Batch& removes, Batch& updates, Batch& adds) const;

private:
  const Track* track_;
  std::vector<ClipPtr> clips_;
};

// A set of ClipEditors keyed by track, emitting in one go (removes, then bounds changes, then adds).
class ClipEdits {
public:
  explicit ClipEdits(const Project& p) : p_(p) {}
  Result<ClipEditor*> editor(TrackId t);  // must be a MIDI track
  ClipEditor& editor(const Track& t);     // t must be a MIDI track of p
  void emit(Batch& out) const;
  bool empty() const noexcept { return eds_.empty(); }

private:
  const Project& p_;
  std::vector<std::unique_ptr<ClipEditor>> eds_;
};

// ---- Automation envelopes ----------------------------------------------------------------------------------------
// Point lists sorted by (tick, id). Points created by an operation draw ids from an IdSource, so "created" means
// id ≥ the IdSource's peek() when the operation started (`firstNew`).

using Points = std::vector<Breakpoint>;

// The envelope on (−∞, t]: the points before t plus, when the envelope continues after t, a terminal point at t with
// v(t−) and curve 0 (and the curvature of the segment it cuts scaled to the kept part). `force` adds the terminal
// whenever the lane has points, also when none lies before t (keeping the run before the first point) or none at or
// after t (holding the last value up to t): for when something else will follow t.
Points envBefore(std::span<const Breakpoint> pts, Ticks t, IdSource& ids, bool force = false);
// The envelope on [t, ∞): a start point at t with v(t) and the remaining curvature when points lie before t and none
// on t, then the points at or after t.
Points envFrom(std::span<const Breakpoint> pts, Ticks t, IdSource& ids);
// The envelope on [a, b] (envFrom(envBefore(pts, b), a)). `freshIds`: every point gets a new id (a copy).
Points envSlice(std::span<const Breakpoint> pts, Ticks a, Ticks b, IdSource& ids, bool freshIds);
void shiftPoints(Points& pts, Ticks dt);
void append(Points& dst, const Points& src);
// Finishes an envelope built by concatenating parts in time order (the list order is the envelope order; ties on a
// tick keep it). Removes points this operation created that change nothing: a created point on the same tick and
// value as its neighbour (merged, keeping an existing id and the later point's curve), a created point inside a
// constant run or at a constant end. Ids in [keepLo, keepHi) count as existing points. Finally gives fresh ids to
// same-tick points whose ids would sort them out of order (a lane orders such points by id).
void simplify(Points& pts, uint32_t firstNew, IdSource& ids, uint32_t keepLo = 0, uint32_t keepHi = 0);
// Commands turning `lane`'s points into `pts`: Remove, Update, Add (nothing if equal).
void emitLaneDiff(Batch& out, TrackId track, const AutomationLane& lane, Points pts);

}  // namespace pq::doc::ops::detail
