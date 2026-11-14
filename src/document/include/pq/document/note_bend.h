#pragma once

// Derived bends between notes (T7.11a; QUESTIONS Q289 option b; model.h NoteBend / NoteCurves): the ONE evaluation of
// a note's pitch curve — its free curve plus its bends — that the piano roll (drawing), pitch_resolve (a note anchored
// to a bent note) and the playback lowering share, so they cannot disagree.
//
// A note's curve at note tick t (ticks from its start, the note being L ticks long), in cents added to its pitch:
//   free(t)      NoteCurves::pitch under the automation curve law (pq/core/automation_curve.h), 0 when empty
//   bendIn       a = min(bendIn.length, L):  in · (1 − s(t / a)) for t < a, else 0        (s = jiShapeValue of its shape)
//   bendOut      b = min(bendOut.length, L): out · s((t − (L − b)) / b) for t > L − b, else 0
//   curve(t) = free(t) + bendIn(t) + bendOut(t)
// `in` / `out` are the bend TARGETS: where the neighbour (NoteBend::other) is relative to the note, in cents:
// 1200 · log2(other's resolved Hz / the note's resolved Hz), both as pitch_resolve resolves them NOW (NotePitch::hz:
// each note's pitch at its own start, without its own curve). Nothing is stored but the neighbour's id, so moving or
// retuning either note, or changing the tuning, moves the glide. A bend without a target — `other` is not in the clip,
// or one of the two notes does not resolve — contributes nothing.
//
// Not RT code (the flattening allocates its result); the single-tick query is allocation-free and noexcept.

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::doc {

struct ClipPitches;  // pitch_resolve.h

// The targets of a note's two bends in cents relative to the note's own pitch; nullopt: that bend does nothing.
struct NoteBendTargets {
  std::optional<double> in, out;
  bool any() const noexcept { return in.has_value() || out.has_value(); }
};

// A clip's resolved pitches by note id (ClipPitches is in note order): built once per clip, O(n log n); a lookup is
// O(log n). Unresolved notes have no Hz.
class NotePitchIndex {
public:
  NotePitchIndex() = default;
  explicit NotePitchIndex(const ClipPitches& pitches);
  std::optional<double> hz(NoteId note) const noexcept;

private:
  std::vector<std::pair<uint32_t, double>> byId_;  // (note id, Hz; 0: unresolved), sorted by id
};

// The targets of `c`'s bends from the clip's resolved pitches (see above). O(log notes).
NoteBendTargets noteBendTargets(const NoteCurves& c, const NotePitchIndex& pitches) noexcept;
// The same from Hz values the caller has: the note's own and its neighbours' (nullopt / not positive: no target).
NoteBendTargets noteBendTargets(const NoteCurves& c, std::optional<double> noteHz, std::optional<double> inHz,
                                std::optional<double> outHz) noexcept;

// curve(t) of a note `noteLength` ticks long at note tick `tick` (clamped to [0, noteLength]; at a jump of the free
// curve the value from that tick on). O(log points). Allocation-free.
double noteCurveCents(const NoteCurves& c, Ticks noteLength, const NoteBendTargets& targets, double tick) noexcept;

// The effective pitch breakpoints of a note: curve(t) over the note (see above), ticks from the note's start.
//   - no bend with a target: the free curve exactly as stored (curvature and all; empty when the note has none);
//   - else: straight segments (Breakpoint::curve 0) within `toleranceCents` of curve(t), from tick 0 to at least
//     `noteLength` (free-curve points beyond the note are kept); the bends' shapes and curved free segments are
//     subdivided until they are that close, straight spans cost two points. Two points on one tick are a jump.
// Ids are invalid (as every note curve). Preconditions: noteLength ≥ 1, toleranceCents > 0.
std::vector<Breakpoint> effectiveNoteCurve(const NoteCurves& c, Ticks noteLength, const NoteBendTargets& targets,
                                           double toleranceCents = 0.05);
// The same for note `note` of `clip` (empty when it has no curve record), targets from `pitches`.
std::vector<Breakpoint> effectiveNoteCurve(const NoteClip& clip, const Note& note, const NotePitchIndex& pitches,
                                           double toleranceCents = 0.05);

}  // namespace pq::doc
