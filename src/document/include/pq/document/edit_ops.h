#pragma once

// Edit operations library (T3.6): pure command builders, so views stay thin.
//
// Every builder reads a snapshot plus a selection and parameters and returns an `Edit`: one `Batch` (= exactly one
// history entry when dispatched) and the selection the view should show afterwards. Builders never mutate anything
// except the IdSource they are given (ids for new entities are drawn from it, as T3.4 requires: pass
// `store.ids()`). An Edit whose batch is empty changes nothing (dispatching it records no history entry).
//
//     auto e = ops::splitClips(*store.current(), store.ids(), selection, playhead);
//     if (e && !e->empty()) store.dispatch(e->command);   // one undo step; select e->clips
//
// Errors (Result): unknown ids, a selection the operation does not accept (e.g. consolidating clips with different
// tunings), parameters out of range. Nothing is half-built: an error returns no command.
//
// Cost: each builder is proportional to what it touches (the clips/notes/points it edits, plus a lookup per
// selected id), never to the project size, except the time edits (insert/delete/duplicate time), which by definition
// visit every track, lane and locator.
//
// Ableton semantics followed throughout (SPEC §6.1, §9.4, §9.6):
// - Clips on a track do not overlap: a clip placed, moved, pasted or grown over others overwrites them (covered
//   clips are removed, partly covered ones are trimmed or split).
// - Resizing a clip edge keeps every note at its absolute time (the start marker moves with the left edge).
// - Time edits (insert / delete / duplicate time) act on all tracks; clips across a cut are split.
//
// Not RT code (UI thread).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/core/grid.h"
#include "pq/core/result.h"
#include "pq/document/commands.h"
#include "pq/document/model.h"
#include "pq/document/store.h"
#include "pq/tuning/tuning.h"

namespace pq::doc::ops {

struct Edit {
  Command command;  // always a Batch labelled with the operation
  // The selection after the edit (created or surviving entities), for the view to adopt.
  std::vector<TrackId> tracks;
  std::vector<ClipRef> clips;
  ClipRef noteClip;               // the clip `notes` belong to
  std::vector<NoteKey> notes;     // keys as they are in the resulting snapshot
  std::vector<BreakpointId> points;

  bool empty() const noexcept;    // the batch has no commands
  const Batch& batch() const noexcept;
};
using EditResult = Result<Edit>;

// ---- Tracks ----------------------------------------------------------------------------------------------------

// Ctrl+Shift+T (Midi) / Ctrl+Alt+G-style group / return. Inserted right after `after` among its siblings (invalid `after`:
// at the end of the top level; Return tracks always go to the returns list, after `after` if it is a return).
// An empty name gets Live-style defaults: "<n> MIDI", "<n> Group", "<letter> Return".
EditResult createTrack(const Project& p, IdSource& ids, TrackKind kind, TrackId after = {}, std::string name = {},
                       Colour colour = {});
// Deletes the tracks (a selected group takes its members with it). The master is refused.
EditResult deleteTracks(const Project& p, std::span<const TrackId> tracks);
EditResult renameTrack(const Project& p, TrackId track, std::string name);
EditResult setTracksColour(const Project& p, std::span<const TrackId> tracks, Colour colour);
// Moves the tracks (kept in their tree order) under `parent` (invalid: top level) right before sibling `before`
// (invalid: at the end). Tracks inside other selected groups move with their group.
EditResult moveTracks(const Project& p, std::span<const TrackId> tracks, TrackId parent, TrackId before = {});
// Ctrl+G: wraps the selection (any parents, nesting allowed) in a new group placed where the first selected track
// was. Tracks inside selected groups stay inside them.
EditResult groupTracks(const Project& p, IdSource& ids, std::span<const TrackId> tracks, std::string name = {});
// Replaces each selected group by its members.
EditResult ungroupTracks(const Project& p, std::span<const TrackId> groups);
EditResult setTracksActive(const Project& p, std::span<const TrackId> tracks, bool active);
// `0`: deactivates the selection if any selected track is active, else activates it all.
EditResult toggleTracksActive(const Project& p, std::span<const TrackId> tracks);

// ---- Mixer (T3.10) -----------------------------------------------------------------------------------------------
// Mute and solo are Param edits (Q15: the track activator is the mute, no recompile); arm is Meta (Q26).

// The track activator (Live's numbered button) = mute. The master has none (refused).
EditResult setTracksMute(const Project& p, std::span<const TrackId> tracks, bool mute);
// `0` with tracks selected, or a click on an activator: if any of the tracks is on (not muted) all are muted, else
// all are switched back on.
EditResult toggleTracksMute(const Project& p, std::span<const TrackId> tracks);
// A click on Solo (Live's Exclusive Solo, its default): on an unsoloed track it solos the track and unsolos every
// other track (main tree and returns); on a soloed track it unsolos just that track. `additive` (Ctrl-click) toggles
// only this track. The master is refused. What solo does to the sound is pq/document/audibility.h.
EditResult soloTrack(const Project& p, TrackId track, bool additive);
// The arm button, same exclusive / additive rule over the MIDI tracks (Live's Exclusive Arm).
EditResult armTrack(const Project& p, TrackId track, bool additive);
// A return's Pre/Post switch (Live): every other track's send to `ret` taps before or after its fader (Structural:
// the tap point moves). Tracks without a send (the master) are skipped.
EditResult setReturnSendsPreFader(const Project& p, TrackId ret, bool preFader);
// True when every track that can send to `ret` sends pre-fader (what the switch shows). False for no senders.
bool returnSendsPreFader(const Project& p, TrackId ret) noexcept;

// Fold is view state (Q15/Q26): not undoable, so it edits the store's ViewState directly and returns nothing.
void setFolded(ViewState& view, TrackId track, bool folded);
bool toggleFolded(ViewState& view, TrackId track);  // returns the new state

// ---- Clips -----------------------------------------------------------------------------------------------------

// Ctrl+Shift+M: an empty looped clip over [start, end) on a MIDI track (overwriting what was there).
EditResult insertEmptyClip(const Project& p, IdSource& ids, TrackId track, Ticks start, Ticks end,
                           std::string name = {});
EditResult deleteClips(const Project& p, std::span<const ClipRef> clips);

struct ClipMove {
  ClipRef clip;
  TrackId toTrack;  // invalid = same track
  Ticks start = 0;  // new arrangement start
};
struct MoveClipsOptions {
  bool copy = false;  // Alt+drag: copies with fresh clip and note ids; the originals stay
  // Automation follows the clips (SPEC §9.6, Ableton's "lock envelopes" off): for clips that stay on their track,
  // every lane on that track moves its envelope over the clip's range with the clip (exactly, curves included);
  // the vacated range holds the value it had on its left; the destination range is overwritten.
  bool moveAutomation = false;
};
// Moves (or copies) clips, each to its own destination; destinations overwrite other clips.
EditResult moveClips(const Project& p, IdSource& ids, std::span<const ClipMove> moves, MoveClipsOptions opt = {});
// Convenience: every clip by the same time delta, staying on its track.
EditResult moveClipsBy(const Project& p, IdSource& ids, std::span<const ClipRef> clips, Ticks delta,
                       MoveClipsOptions opt = {});

enum class ClipEdge : uint8_t { Start, End };
// Moves one edge of each clip by `delta` (clamped so a clip keeps ≥ 1 tick). Notes keep their absolute time; a grown
// edge overwrites neighbouring clips.
EditResult resizeClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips, ClipEdge edge, Ticks delta);
// Ctrl+E: splits every given clip at every cut point strictly inside it (a time selection passes its two edges).
// Absolute time and pitch identity of every note are kept: a non-looped clip's notes go to the piece they start in
// (same ids); a looped clip's later pieces get copies of its loop content (fresh ids), each piece playing exactly
// what the original played there.
EditResult splitClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips, std::span<const Ticks> at);
// Ctrl+D: copies the clips right after the selection (per the selection's overall span [min start, max end)).
EditResult duplicateClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips);
// Ctrl+J: per track, merges the selected clips (and any clip between them) into one clip spanning them. Every note
// that played keeps its absolute time and pitch identity; looped content is unrolled; notes are cut where playback
// cut them (clip end, loop wrap); notes of deactivated clips come in deactivated. The first occurrence of a note
// keeps its id, repeats get fresh ids. Refused if the clips' tuning assignments differ.
EditResult consolidateClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips);
// Ctrl+L in the clip view: loop brace = content [start, end), loop on.
EditResult loopClipToSelection(const Project& p, ClipRef clip, Ticks contentStart, Ticks contentEnd);
// Ctrl+L in the arrangement: arrangement loop = [start, end), on.
EditResult loopArrangementToSelection(const Project& p, Ticks start, Ticks end);

// ---- Notes -----------------------------------------------------------------------------------------------------

// Adds notes (their `id` fields are ignored: fresh ids are assigned). Returns their keys.
EditResult addNotes(const Project& p, IdSource& ids, ClipRef clip, std::span<const Note> notes);
EditResult deleteNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes);

// Identity-aware pitch steps (SPEC §6.1 microtonal Up/Down, T3.6).
enum class StepUnit : uint8_t {
  Grid,    // Up/Down: one display-grid step
  Equave,  // Shift+Up/Down: one equave
};
struct PitchStepContext {
  const tuning::Tuning* tuning = nullptr;     // the clip's effective tuning (required for Degree/Cents/Ratio-equave)
  const tuning::Edo* displayGrid = nullptr;   // optional EDO overlay (SPEC §3.2): Cents notes step on its lines
  // Reference-anchored Ratio notes: the clip's virtual fundamental (doc::virtualFundamental, T3.6-fix1); nullopt =
  // each note's own fundamental 1/d. Ratios anchored to other notes always step on their own.
  std::optional<tuning::Rational> fundamental;
};
// One note's pitch after `steps` steps (negative = down). nullopt = the note cannot move that way (left unchanged):
//   Degree   Grid: ±1 degree of the tuning (one EDO step / one scale or MOS degree), carrying into the equave.
//            Equave: equave ± 1.
//   Cents    Grid: to the next display-grid line above/below (a line within 1e-6 ¢ counts as "on" it). The lines
//            are every degree of the tuning in every equave (degree d of equave e at centsOf(d, e), stretch
//            included), or the displayGrid EDO's steps if one is given. Equave: ± the tuning's equave in cents.
//   Ratio    Grid: next harmonic of the fundamental F' = gcd(n/d, F) (the largest fundamental the note and F are both
//            harmonics of; F = 1/D gives 1/lcm(d, D); without F: 1/d): n/d = m·F' → (m ± 1)·F', so on F = 1/4
//            5/4 → 6/4 (= 3/2) → 7/4. Fails below the first harmonic (m ± steps < 1).
//            Equave: × / ÷ the tuning's equave ratio (exact for EDO ratio equaves and Scala ratio equaves; a cents
//            equave within 0.001 ¢ of a ratio p/q with p, q ≤ 64 uses that ratio; otherwise fails).
//   Harmonic Grid: next voice of the JI object (voiceIndex ± 1, ≥ 0). Equave: fails (a voice has no equave).
std::optional<tuning::PitchId> stepPitch(const tuning::PitchId& pitch, int32_t steps, StepUnit unit,
                                         const PitchStepContext& ctx);

// Moves notes in time by `dt` and in pitch by `steps` display-grid steps (stepPitch). A note whose pitch cannot
// step keeps its pitch (it still moves in time).
// Both step reference-anchored Ratio notes on the clip's virtual fundamental (doc::virtualFundamental) and, unless it
// is locked, record in the same Batch (SetClipFundamental) the fundamental each stepped note was on when that is
// finer than its new reduced denominator (a HarmonicStep), so 5/4 → 3/2 → 7/4 keeps 1/4 (T3.6-fix1, Q40).
EditResult moveNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, Ticks dt, int32_t steps = 0,
                     const tuning::Edo* displayGrid = nullptr);
// Up/Down (unit Grid) and Shift+Up/Down (unit Equave), using the clip's effective tuning.
EditResult transposeNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, int32_t steps,
                          StepUnit unit, const tuning::Edo* displayGrid = nullptr);
// The explicit fundamental lock (b + index, SPEC §6.2): "this note is the index-th harmonic", so the clip's virtual
// fundamental becomes the note's ratio / index until unlocked. The note must be a reference-anchored Ratio note
// (other identities have no exact ratio to divide; InvalidArgument), index ≥ 1. One undo step each.
EditResult lockFundamental(const Project& p, ClipRef clip, NoteKey note, uint32_t harmonicIndex);
EditResult unlockFundamental(const Project& p, ClipRef clip);

enum class NoteEdge : uint8_t { Start, End };
// Moves one edge of each note by `delta`; the other edge stays (length ≥ 1 tick).
EditResult resizeNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, NoteEdge edge, Ticks delta);
// Sets a property on every note (value clamped to the property's range: velocity [1, 127], release [0, 127],
// chance [0, 1], velocity range [−127, 127]; Enabled: ≠ 0 enables).
EditResult setNotesProperty(const Project& p, ClipRef clip, std::span<const NoteKey> notes, NoteProperty prop,
                            float value);
// Adds `delta` to the property of every note (clamped per note), e.g. a velocity drag over a selection.
EditResult offsetNotesProperty(const Project& p, ClipRef clip, std::span<const NoteKey> notes, NoteProperty prop,
                               float delta);
// Ctrl+U: quantizes note starts (and ends in StartEnd mode) to `grid` (pq::quantize). The grid is evaluated at
// content tick + gridOrigin (0 = the clip's own bar grid; clip.start − clip.contentStart = the arrangement grid).
// An empty `notes` span quantizes every note of the clip.
EditResult quantizeNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, const Grid& grid,
                         Ticks gridOrigin = 0, int amountPercent = 100, QuantizeMode mode = QuantizeMode::StartOnly);
// Ctrl+D in the clip view: copies the notes `offset` ticks later (0 = the selection's span, max end − min start).
EditResult duplicateNotes(const Project& p, IdSource& ids, ClipRef clip, std::span<const NoteKey> notes,
                          Ticks offset = 0);

// SPEC §3.1 explicit identity conversions. Degree and Ratio (reference-anchored) → Cents: the exact same pitch
// ("freeze"). Ratio/Cents → Degree: the nearest degree of the clip's tuning ("snap"). Other notes are left alone.
EditResult convertNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, tuning::PitchKind to);

// ---- Time selection (all tracks) ------------------------------------------------------------------------------

// Ctrl+I: inserts `length` ticks of silence at `at`. Clips across `at` are split; clips, automation (exactly: the
// envelope holds its value through the gap), locators, the loop, tempo points and time-signature changes after `at`
// move later by `length`.
EditResult insertTime(const Project& p, IdSource& ids, Ticks at, Ticks length);
// Deletes [start, end) and closes the gap: clips inside are removed, clips across the edges trimmed/split, later
// material moves earlier; locators inside are removed; automation keeps its values on both sides (a jump at the
// seam where they differ).
EditResult deleteTime(const Project& p, IdSource& ids, Ticks start, Ticks end);
// Ctrl+Shift+D: inserts a copy of [start, end) right after it (clips trimmed to the range, automation exact).
EditResult duplicateTime(const Project& p, IdSource& ids, Ticks start, Ticks end);

// ---- Automation ------------------------------------------------------------------------------------------------

// Adds one breakpoint (value clamped to the parameter's range, curve to [−1, 1]).
EditResult addBreakpoint(const Project& p, IdSource& ids, TrackId track, LaneId lane, Ticks tick, double value,
                         float curve = 0.0f);
// Moves breakpoints by (dt, dv) (values clamped per point).
EditResult moveBreakpoints(const Project& p, TrackId track, LaneId lane, std::span<const BreakpointId> points,
                           Ticks dt, double dv);
EditResult deleteBreakpoints(const Project& p, TrackId track, LaneId lane, std::span<const BreakpointId> points);
// Alt-drag: curvature of the segment that starts at `point` (clamped to [−1, 1]).
EditResult setSegmentCurve(const Project& p, TrackId track, LaneId lane, BreakpointId point, float curve);

enum class ShapeKind : uint8_t { RampUp, RampDown, Sine, Triangle, Square, Saw };
struct ShapeParams {
  ShapeKind kind = ShapeKind::Sine;
  double low = 0.0;   // parameter units (clamped to the parameter's range)
  double high = 1.0;
  int32_t cycles = 1;  // periods over the range (ramps ignore it), ≥ 1
};
// Replaces the envelope over [start, end) with a shape; the envelope outside the range is unchanged (exact).
// Sine uses 16 linear segments per cycle; Square and Saw use jumps.
EditResult insertShape(const Project& p, IdSource& ids, TrackId track, LaneId lane, Ticks start, Ticks end,
                       const ShapeParams& shape);

}  // namespace pq::doc::ops
