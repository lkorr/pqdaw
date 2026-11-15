#pragma once

// A note's effective pitch curve (T7.11a derived bends, QUESTIONS Q289 option b; T7.13 warps; model.h NoteBend /
// NoteCurves / JiWarp): the ONE evaluation of how a note's pitch moves after its start — its group's warps, its bends
// and its free curve — that the piano roll (drawing), pitch_resolve (a note anchored to a moving note) and the
// playback lowering share, so they cannot disagree.
//
// A note's curve at note tick t (ticks from its start, the note being L ticks long), in cents added to the pitch it
// RESOLVES to at its start (pitch_resolve.h NotePitch::hz — for a warped member that already is the written pitch,
// the JI pitch or the point of the glide its start falls on):
//   free(t)      NoteCurves::pitch under the automation curve law (pq/core/automation_curve.h), 0 when empty
//   bendIn       a = min(bendIn.length, L):  in · (1 − s(t / a)) for t < a, else 0        (s = glideShapeValue of its shape)
//   bendOut      b = min(bendOut.length, L): out · s((t − (L − b)) / b) for t > L − b, else 0
//   warp(t)      (w(o + t) − w(o)) · D for a group member with a written identity: w = jiWarpWritten of its group's
//                warps (1 = written, 0 = JI), o = the note's start in group ticks, D = written − JI in cents. A warp's
//                glide starts at JiWarp::at and ends at at + glide; a note that spans it glides mid-note.
//   vibrato(t)   NoteCurves::vibrato (T7.14; model.h NoteVibrato): A(t) · sin(2π · (t − onset) / P) for onset ≤ t ≤ stop
//                (stop = its `end`, or L when that is 0 or beyond L), 0 elsewhere; A(t) = depth · min(1, (t − onset) /
//                fadeIn, (stop − t) / fadeOut) (a fade of 0 ticks is no fade); P = the period in ticks
//                (NoteBendTargets::vibratoPeriod: vibratoPeriodTicks — kPpq / rate for Beats, the ticks per second
//                where the note starts / rate for Hz). P ≤ 0: no vibrato.
//   curve(t) = free(t) + bendIn(t) + bendOut(t) + warp(t) + vibrato(t)
// A note anchored to a vibrating note (pitch_resolve) takes its anchor's pitch WITHOUT the vibrato: an ornament is
// not a pitch to tune to.
// `in` / `out` are the bend TARGETS: where the neighbour (NoteBend::other) is relative to the note, in cents:
// 1200 · log2(other's Hz / the note's resolved Hz), both as pitch_resolve resolves them NOW (the note's NotePitch::hz,
// its pitch at its own start; the neighbour's pitch where the two notes meet, along its free curve and its warps:
// NoteBendEnds, T7.16a). Nothing is stored but the neighbour's id, so moving or
// retuning either note, or changing the tuning, moves the glide. A bend without a target — `other` is not in the clip,
// or one of the two notes does not resolve — contributes nothing.
//
// Not RT code (the flattening allocates its result); the single-tick query is allocation-free and noexcept.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::doc {

struct ClipPitches;  // pitch_resolve.h

// What pitch_resolve records for a group member that has a written identity in a group with warps — or in a group
// Back to Tuning, where its anchor too when that has one (model.h effectiveJiWarps) — (ClipPitches::warped, sorted by
// note id): the two pitches it moves between and where it starts in group ticks. Its NotePitch::hz is the pitch at
// its start: writtenHz, jiHz, or the point of a glide between them.
struct NoteWarpState {
  NoteId note;
  JiGroupId group;
  Ticks offset = 0;        // the note's start in group ticks (note.start − the anchor's start; may be negative)
  double jiHz = 0.0;       // anchor × ratio
  double writtenHz = 0.0;  // the written identity in the clip's tuning
  double cents = 0.0;      // 1200 · log2(writtenHz / jiHz)
  friend bool operator==(const NoteWarpState&, const NoteWarpState&) noexcept = default;
};

// T7.16a (M7 review M2): where a note's bends end, as pitch_resolve finds them (ClipPitches::bends, sorted by note
// id): the neighbour's pitch at the tick the two notes meet — the note's start for bendIn, its end for bendOut,
// clamped to the neighbour's span — along the neighbour's free curve and its group's warps (not its own bends or its
// vibrato: one level, so two notes that bend into each other cannot recurse). 0 Hz: no such bend / unresolved.
struct NoteBendEnds {
  NoteId note;
  NoteId inOther, outOther;  // the neighbours they were found for (a gesture's preview may name others)
  double inHz = 0.0, outHz = 0.0;
  friend bool operator==(const NoteBendEnds&, const NoteBendEnds&) noexcept = default;
};

// The targets of a note's two bends in cents relative to the note's own pitch; nullopt: that bend does nothing.
struct NoteBendTargets {
  std::optional<double> in, out;
  double vibratoPeriod = 0.0;  // T7.14: ticks per cycle of the note's vibrato; ≤ 0: none (or it is not evaluated)
  bool any() const noexcept { return in.has_value() || out.has_value() || vibratoPeriod > 0.0; }
};
// The period of a vibrato in ticks: Beats: kPpq / rate; Hz: ticksPerSecond / rate. 0: not set. Never below
// kMinVibratoPeriodTicks. RT-safe.
inline constexpr double kMinVibratoPeriodTicks = 8.0;
double vibratoPeriodTicks(const NoteVibrato& v, double ticksPerSecond) noexcept;
// Ticks per second where content tick `contentTick` of `clip` plays on the project's timeline (120 bpm without one).
double clipTicksPerSecond(const Project& p, const NoteClip& clip, Ticks contentTick) noexcept;
// Where a vibrato stops on a note `noteLength` long (see vibrato(t) above).
Ticks vibratoStop(const NoteVibrato& v, Ticks noteLength) noexcept;
// A vibrato's amplitude A(t) (see vibrato(t) above) as a non-negative swing: |depth| · min(1, (t − onset) / fadeIn,
// (stop − t) / fadeOut) inside [onset, stop], 0 outside. What the wave swings either way around the rest of the curve:
// vibrato(t) = sign(depth) · at(t) · sin(…). The views draw a vibrato whose cycles are too short to show as this band.
// RT-safe.
struct VibratoEnvelope {
  double depth = 0.0;  // |depthCents|; 0: no vibrato (on a note too short for it to start, too)
  double on = 0.0, stop = 0.0, fadeIn = 0.0, fadeOut = 0.0;
  VibratoEnvelope() = default;
  VibratoEnvelope(const NoteVibrato& v, Ticks noteLength) noexcept;
  bool any() const noexcept { return depth != 0.0; }
  double at(double t) const noexcept {
    if (depth == 0.0 || t <= on || t >= stop) return 0.0;
    double a = 1.0;
    if (fadeIn > 0.0) a = std::min(a, (t - on) / fadeIn);
    if (fadeOut > 0.0) a = std::min(a, (stop - t) / fadeOut);
    return depth * a;
  }
  // A(t) inside [onset, stop] with its ends included (where a fade of 0 ticks makes A jump: the full depth there, not
  // 0) — what a band over that span is drawn with. Not clamped to the span.
  double within(double t) const noexcept {
    double a = 1.0;
    if (fadeIn > 0.0) a = std::min(a, (t - on) / fadeIn);
    if (fadeOut > 0.0) a = std::min(a, (stop - t) / fadeOut);
    return depth * std::max(a, 0.0);
  }
  // The ticks where A(t) bends (onset, the fade-in's end, the fade-out's start, stop, and where two overlapping fades
  // meet), ascending, inside [onset, stop]; returns how many of `out` it wrote (0 without a vibrato).
  int corners(double (&out)[5]) const noexcept;
};

// A group member's warp as its curve needs it (see warp(t) above). `warps` points into the clip's group.
struct NoteWarp {
  std::span<const JiWarp> warps;
  Ticks offset = 0;    // the note's start in group ticks
  double cents = 0.0;  // D: 1200 · log2(written Hz / JI Hz)
  bool any() const noexcept { return !warps.empty() && cents != 0.0; }
};

// A clip's resolved pitches by note id (ClipPitches is in note order): built once per clip, O(n log n); a lookup is
// O(log n). Unresolved notes have no Hz. It also carries the clip's warped members (ClipPitches::warped, copied)
// and its vibrato periods (ClipPitches::vibrato, copied).
class NotePitchIndex {
public:
  NotePitchIndex() = default;
  explicit NotePitchIndex(const ClipPitches& pitches);
  std::optional<double> hz(NoteId note) const noexcept;
  // The warp of note `note` of `clip` (the clip the pitches were resolved from); !any(): it has none.
  NoteWarp warp(const NoteClip& clip, NoteId note) const noexcept;
  // What pitch_resolve recorded for a warped member (its JI and written Hz); nullptr: not one.
  const NoteWarpState* warped(NoteId note) const noexcept;
  bool anyWarp() const noexcept { return !warped_.empty(); }
  // T7.14: the period in ticks of the note's vibrato; 0: it has none.
  double vibratoPeriod(NoteId note) const noexcept;
  // T7.16a: where the note's bends end (NoteBendEnds); nullptr: none recorded.
  const NoteBendEnds* bendEnds(NoteId note) const noexcept;

private:
  std::vector<std::pair<uint32_t, double>> byId_;  // (note id, Hz; 0: unresolved), sorted by id
  std::vector<NoteWarpState> warped_;              // sorted by note id
  std::vector<std::pair<NoteId, double>> vibrato_;  // sorted by note id
  std::vector<NoteBendEnds> bends_;                 // sorted by note id
};

// The targets of `c`'s bends from the clip's resolved pitches (see above: the neighbour's pitch where the two notes
// meet, NotePitchIndex::bendEnds, when it was recorded for that neighbour; else its resolved pitch) and its vibrato's
// period. O(log notes).
NoteBendTargets noteBendTargets(const NoteCurves& c, const NotePitchIndex& pitches) noexcept;
// The same from Hz values the caller has: the note's own and its neighbours' (nullopt / not positive: no target).
// The vibrato period is left 0 (the caller sets it when it wants the vibrato evaluated).
NoteBendTargets noteBendTargets(const NoteCurves& c, std::optional<double> noteHz, std::optional<double> inHz,
                                std::optional<double> outHz) noexcept;

// curve(t) of a note `noteLength` ticks long at note tick `tick` (clamped to [0, noteLength]; at a jump of the free
// curve or of a warp the value from that tick on). `c` null: no curve record (a warp only). O(log points + log
// warps). Allocation-free.
double noteCurveCents(const NoteCurves* c, Ticks noteLength, const NoteBendTargets& targets, const NoteWarp& warp,
                      double tick) noexcept;
inline double noteCurveCents(const NoteCurves& c, Ticks noteLength, const NoteBendTargets& targets, double tick) noexcept {
  return noteCurveCents(&c, noteLength, targets, NoteWarp{}, tick);
}

// The effective pitch breakpoints of a note: curve(t) over the note (see above), ticks from the note's start.
//   - no bend with a target, no warp and no vibrato: the free curve exactly as stored (curvature and all; empty when
//     the note has none);
//   - else: straight segments (Breakpoint::curve 0) within `toleranceCents` of curve(t), from tick 0 to at least
//     `noteLength` (free-curve points beyond the note are kept); the glides' shapes and curved free segments are
//     subdivided until they are that close, straight spans cost two points. Two points on one tick are a jump. A
//     note its group's warps do not move (it starts and stays in one state) and that has no curve record: empty.
//     A vibrato has a knot on every quarter cycle (its zeros and swings are exact) and is subdivided between them
//     to the tolerance: about π · sqrt(depth / (2 · tolerance)) points per cycle (35 ¢ at 1 ¢: 16).
// Ids are invalid (as every note curve). `c` null: no curve record. Preconditions: noteLength ≥ 1, toleranceCents > 0.
std::vector<Breakpoint> effectiveNoteCurve(const NoteCurves* c, Ticks noteLength, const NoteBendTargets& targets,
                                           const NoteWarp& warp, double toleranceCents = 0.05);
inline std::vector<Breakpoint> effectiveNoteCurve(const NoteCurves& c, Ticks noteLength, const NoteBendTargets& targets,
                                                  double toleranceCents = 0.05) {
  return effectiveNoteCurve(&c, noteLength, targets, NoteWarp{}, toleranceCents);
}
// The same for note `note` of `clip` (empty when it has no curve record and no warp): bend targets and the warp from
// `pitches` (resolved from this clip). What the roll draws and the lowering plays.
std::vector<Breakpoint> effectiveNoteCurve(const NoteClip& clip, const Note& note, const NotePitchIndex& pitches,
                                           double toleranceCents = 0.05);

}  // namespace pq::doc
