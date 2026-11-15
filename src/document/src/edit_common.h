#pragma once

// Internals shared by the edit-operation builders (T3.6). Private to pq_doc.

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "checks.h"
#include "pq/document/audio_clip.h"
#include "pq/document/automation_curve.h"
#include "pq/document/clip_time.h"
#include "pq/document/edit_ops.h"
#include "pq/document/note_bend.h"

namespace pq::doc::ops::detail {

using namespace pq::doc::detail;  // invalid(), notFound(), ok(), PQ_DOC_TRY, check*()

Edit makeEdit(std::string label);
Batch& batchOf(Edit& e);

// ---- Lookups ---------------------------------------------------------------------------------------------------

struct ClipFound {
  TrackPath path;
  const Track* track = nullptr;
  ClipPtr clip;        // the note clip (null for an audio clip)
  AudioClipPtr audio;  // T5.3: the audio clip (null for a note clip)
  ClipSlot slot() const { return ClipSlot{audio ? audio->id : clip->id, clip, audio}; }
};
// A note clip (an audio clip: InvalidArgument).
Result<ClipFound> findClip(const Project& p, const ClipRef& r);
// T5.3: a clip of either kind.
Result<ClipFound> findAnyClip(const Project& p, const ClipRef& r);
Result<const Track*> findMidiTrack(const Project& p, TrackId id);
// T5.3: a track that holds clips (MIDI or audio).
Result<const Track*> findClipTrack(const Project& p, TrackId id);
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
// T7.11a: `p` / `path`: the project and the clip's track (a note parted from its anchor is re-anchored in its
// tuning: reanchorWithout).
std::pair<NoteClip, NoteClip> splitClipAt(const NoteClip& c, Ticks t, ClipId rightId, IdSource& ids, const Project& p,
                                          const TrackPath& path);
// Copy with fresh clip and note ids. `onlyPlaying`: for a non-looped clip, copy only the notes that start inside
// the played content window (hidden notes are dropped).
NoteClip cloneClip(const NoteClip& c, IdSource& ids, bool onlyPlaying);
// The degree of `t` nearest to `cents` (above its reference); nullopt for a tuning without a usable lattice.
std::optional<tuning::PitchId> nearestDegreePitch(const tuning::Tuning& t, double cents);
// Drops the curves of notes that are not in c.notes (after notes left the clip), and the bends to notes that are not
// (T7.11a). O(notes + curves) when it has curves.
void pruneCurves(NoteClip& c);
// T7.13: drops the JI groups whose anchor is not in c.notes and the written identities of notes that are neither
// members nor anchors of a group of the clip. O(notes log notes) when it has either.
void pruneJi(NoteClip& c);
// T7.11a: old → new ids of copied notes (sources[i] was copied to copies[i]), sorted by old id.
using NoteIdMap = std::vector<std::pair<NoteId, NoteId>>;
NoteIdMap noteIdMap(std::span<const NoteId> sources, std::span<const Note> copies);
// The copy of `id` (invalid: it was not copied).
NoteId remapNote(NoteId id, const NoteIdMap& map) noexcept;
// A copy of a curve record for the copied note `note`: a bend to a note that was copied too glides to that copy; to
// one that was not, it stays when that note is a note of `target` (a copy inside its own clip) and is dropped
// otherwise (target null: a copy into another clip).
NoteCurves remapNoteCurves(const NoteCurves& nc, NoteId note, const NoteIdMap& map, const NoteClip* target = nullptr);
// The curve records of notes copied into `target` as ONE command (M13): copies[i] is the copy of note sources[i]
// (invalid: none), `curves` the source notes' records (any order, matched by id, O((curves + copies) · log)); bends
// re-pointed as remapNoteCurves. nullopt: no copy has a curve. Errors: an invalid record.
Result<std::optional<SetNoteCurves>> copiedCurves(ClipRef clip, const NoteClip& target, std::span<const NoteCurves> curves,
                                                  std::span<const NoteId> sources, std::span<const Note> copies);
// T7.13: copies of notes carry their JI structure. copies[i] is the copy of note sources[i] (invalid: none) about to
// be added to `target`; `source` holds the groups and written identities of where they come from (the same clip for a
// duplicate, a clipboard for a paste). The copies anchored to a copied note are re-pointed to the copy (in place); a
// group whose anchor and at least one member were copied is copied with a fresh id; a copied member keeps its written
// identity. The commands replace the target's whole lists (push them after the AddNotes).
struct CopiedJi {
  std::optional<SetJiGroups> groups;
  std::optional<SetNotesWritten> written;
  std::vector<JiGroupId> made;
};
Result<CopiedJi> copyJi(ClipRef clip, const NoteClip& target, const JiSource& source, std::span<const NoteId> sources,
                        std::span<Note> copies, IdSource& ids);
// A note's pitch as an exact ratio to the reference: a chain of note anchors (≤ 8 links) that ends on a
// reference-anchored Ratio, none of its notes with a free curve, a bend or a warp (they move what is anchored to
// them: pitch_resolve.h; a vibrato does not, T7.16a M8). nullopt: it is not one (also for n == nullptr). O(links · notes).
std::optional<tuning::Rational> exactReferenceRatio(const NoteClip& clip, const NotePitchIndex& hz, const Note* n);
// T7.16a (M7 review M9): a note's pitch as an exact ratio to the ROOT of its chain of note anchors (≤ 8 links; the
// first note on it that is not anchored to a note — itself when it is not), none of the notes on the way nor the root
// with a free curve, a bend or a warp. Two notes with one root are an exact ratio apart. nullopt: none.
struct RootedRatio {
  NoteId root;
  tuning::Rational ratio{1};
};
std::optional<RootedRatio> rootedRatio(const NoteClip& clip, const NotePitchIndex& hz, const Note* n);
// T7.16a (M7 review M7): a group's warps when its anchor's start moves `by` ticks later (a new anchor): the same
// points in the clip (group ticks count from the anchor's start and may be negative).
std::vector<JiWarp> shiftedWarps(std::span<const JiWarp> warps, Ticks by);
// T7.11a (M7 review H3): what keeps the remaining notes of `clip` (a clip of the track at `path`, or a working copy of
// one) sounding at the same pitch when the notes `gone` (sorted ids) leave it — deleted, or parted from them by a
// split. A note anchored to a leaving note is re-anchored:
//   - the anchor's pitch is an exact ratio to the reference (a chain of note anchors ending on a reference-anchored
//     Ratio, no curve on the way): the exact reference-anchored Ratio;
//   - else, when the anchor sounds at one pitch for all its dependants (no free curve or bend, no warp; a vibrato is
//     no pitch to them, T7.16a M8) and there are several: the LOWEST dependant without such a curve carries the pitch
//     as Cents and the others are anchored to it by their exact ratios to it;
//   - else Cents at the pitch it resolves to now.
// T7.13: when the leaving note is the anchor of a JI group, the group lives on — its lowest remaining member (without
// a curve) is promoted to anchor in either of the first two cases (it carries the pitch: the exact reference Ratio,
// else Cents at its JI pitch), the others keep their exact ratios to it, and the group's index becomes the number the
// new anchor had, so every member keeps its number and its Hz. Its warps stay where they are in the clip (shifted by
// how far the anchor's start moved: shiftedWarps); the new anchor keeps its written identity (an anchor does not
// warp; a group Back to Tuning plays it as written). A group that finds no member to
// promote goes with its anchor. Harmonic steps of leaving notes are dropped. A note that does not resolve is left as
// it is. Nothing to do costs one pass over the notes (no resolution).
struct Reanchored {
  std::vector<Note> updates;                         // remaining notes with their new identity
  std::optional<ClipFundamental> fundamental;        // set: the clip's fundamental becomes this
  std::optional<std::vector<JiGroup>> groups;        // set: the clip's group list becomes this
  std::optional<std::vector<NoteWritten>> written;   // set: the clip's written identities become this
  bool empty() const noexcept { return updates.empty() && !fundamental && !groups && !written; }
};
Reanchored reanchorWithout(const Project& p, const TrackPath& path, const NoteClip& clip, std::span<const NoteId> gone);
// The result applied to a clip value / as commands (UpdateNotes, SetClipFundamental, SetJiGroups, SetNotesWritten)
// appended to a batch (before the RemoveNotes).
void applyReanchored(NoteClip& c, Reanchored r);
void appendReanchored(Batch& b, const ClipRef& clip, Reanchored r);
// T7.13: a note whose pitch an UpdateNotes of `b` changes is no longer the note that was written: appends the
// SetNotesWritten that drops such notes' written identities (`before`: the clip the batch applies to). Nothing for a
// clip without any. For the builders that re-pitch notes one by one (steps, drags, type-ins).
void dropRepitchedWritten(Batch& b, const ClipRef& clip, const NoteClip& before);
// Builds a sequence from notes known to be valid with unique ids.
NoteSeq seqOf(std::vector<Note> notes);

// T5.3: a copy of a clip of either kind with fresh ids (an audio clip: a fresh clip id; a note clip: cloneClip).
ClipSlot cloneSlot(const ClipSlot& c, IdSource& ids, bool onlyPlaying);

// A working copy of one track's clip list (note or audio clips); the final list is diffed against the track to emit
// commands.
class ClipEditor {
public:
  ClipEditor(const Project& p, const Track& t);
  TrackId id() const noexcept { return track_->id; }
  const Track& track() const noexcept { return *track_; }

  std::vector<ClipSlot>& clips() noexcept { return clips_; }
  const ClipSlot* find(ClipId id) const;
  void remove(ClipId id);
  void add(ClipSlot c) { clips_.push_back(std::move(c)); }
  void replace(ClipId id, ClipSlot c);
  // The clip with arrangement bounds [s, e) (e > s), every note / source frame keeping its absolute time
  // (withBounds; audio_clip.h audioWithBounds with this track's tuning and the clip's sample rate).
  ClipSlot bounded(const ClipSlot& c, Ticks s, Ticks e) const;
  // Splits at t (start < t < end): left keeps the id, right gets `rightId`.
  std::pair<ClipSlot, ClipSlot> split(const ClipSlot& c, Ticks t, ClipId rightId, IdSource& ids) const;
  // The clip at a new start (an audio Off clip's length re-derived there).
  ClipSlot moved(const ClipSlot& c, Ticks start) const;
  // cloneSlot(c, ids, true): a copy with fresh ids of what the clip plays (a non-looped note clip's hidden notes are
  // dropped), a playing note anchored to a dropped one keeping its pitch (T7.11a, reanchorWithout).
  ClipSlot clonePlaying(const ClipSlot& c, IdSource& ids) const;
  // Overwrites [a, b): covered clips are removed, partly covered ones trimmed or split (new ids for right pieces).
  // Clips whose id is in `keep` are left alone.
  void clearRange(Ticks a, Ticks b, IdSource& ids, std::span<const ClipId> keep = {});
  // Appends commands turning the track's clips into the working list: RemoveClip to `removes`, SetClipBounds (note
  // clips) / SetAudioClip (audio clips) to `updates`, AddClip to `adds` (changed note clips that are more than a
  // bounds change are removed and re-added).
  void emit(Batch& removes, Batch& updates, Batch& adds) const;

private:
  double rateOf(const AudioClip& c) const noexcept;
  double ratioOf(const AudioClip& c) const;
  const Project* p_;
  const Track* track_;
  TrackPath path_;
  std::vector<ClipSlot> clips_;
};

// A set of ClipEditors keyed by track, emitting in one go (removes, then bounds changes, then adds).
class ClipEdits {
public:
  explicit ClipEdits(const Project& p) : p_(p) {}
  Result<ClipEditor*> editor(TrackId t);  // must be a MIDI or audio track
  ClipEditor& editor(const Track& t);     // t must be a MIDI or audio track of p
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
