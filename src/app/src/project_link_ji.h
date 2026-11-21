#pragma once

// T7.3 / T7.13 (docs/plans/M7.md "Rework: notes-first JI"): how a note clip's notes become the engine's per-note
// pitch curves (engine/sequence.h "Per-note curves"). Used by ProjectLink::lowerTrack only.
//
// The note is the only primitive. A note with an EFFECTIVE curve (doc/note_bend.h: its free curve, the bends in from /
// out to its neighbours — whose far ends are the neighbours' pitches as resolved now — and, for a member of a JI group
// with a written identity, its group's warps between the written and the JI pitch) gets that curve flattened to
// lines. Curve values are cents relative to the note's own Hz — the pitch the document resolves it to at its start
// (pitch_resolve.h: for a warped member the written pitch, the JI pitch or the point of the glide it starts on) — so a
// note that starts after a warp is the exact ratio with no curve at all, and one that glides into JI ends within the
// float32 of the cents between the two pitches.
// Members of a JI group also carry voice.harmonic (their number in the group) and voice.ratio (to the anchor); the
// anchor its number.
//
// Event volume. The engine sends one event per breakpoint and note; a track's event list is finite
// (TimelineConfig::maxEventsPerTrack per image block). So shapes are flattened to kJiPitchTolerance cents (1 c:
// inaudible while the pitch moves, and a tenth of the points 0.01 c would take; holds and straight lines are exact and
// cost two points), and inside a curved span breakpoints closer than 1 ms are dropped. Corners (where a hold begins or
// ends, jumps) are always kept. Inside a tempo ramp, sloped segments are cut to a sixteenth note (the engine
// interpolates over samples).
// Unisons: notes that bend or warp into one pitch are not phase-locked (the engine's Sync / Gain events stay, tested at
// engine level; nothing here emits them — deferred, docs/TASKS.md).

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/model.h"
#include "pq/document/note_bend.h"
#include "pq/document/pitch_resolve.h"
#include "pq/engine/sequence.h"

namespace pq::app {

inline constexpr double kJiPitchTolerance = 1.0;  // cents
inline constexpr double kJiMinSpacingSeconds = 0.001;

// One note clip's curve lowering. Not RT code.
class ClipCurveLowering {
public:
  // True when the clip has anything to lower here (a curve record, a group): lowerTrack skips the rest otherwise.
  static bool needed(const doc::NoteClip& clip) noexcept { return clip.curves != nullptr || clip.groups != nullptr; }
  // T7.16b (review H5): whether what this lowers for the clip depends on the tempo map — a curve record (a Hz vibrato's
  // period in ticks, the 1 ms thinning, the cuts inside tempo ramps) or a group with warps (thinned and cut likewise).
  // ProjectLink::sequenceKey keys such clips on the timeline node.
  static bool readsTempo(const doc::NoteClip& clip) noexcept {
    if (clip.curves != nullptr) return true;
    for (const doc::JiGroup& g : doc::jiGroupsOf(clip))
      if (!g.warps.empty()) return true;
    return false;
  }

  // `tempoMoves`: the tempo map is not one constant tempo. `pitches`: the clip's resolved pitches (what its notes'
  // bends aim at and its members' warps move between); it must outlive this object.
  // The harmonic numbers of every group come from one pass over the clip's notes (T7.16b, review M17: the members
  // bucketed by their anchor; the numbers are doc::groupHarmonics', group by group).
  ClipCurveLowering(const doc::NoteClip& clip, const TempoMap& tempo, bool tempoMoves, const doc::ClipPitches& pitches);

  // The effective pitch curve of clip note `n` into nd.pitchCurve (nd.start must be set), and for a note of a JI
  // group nd.harmonic (and nd.ratio for a member).
  void note(const doc::Note& n, engine::NoteDesc& nd);

  struct Point {
    Ticks tick;
    double value;
  };

private:
  double bpmAt(Ticks contentTick) const noexcept;
  Ticks ticksOf(double seconds, Ticks contentTick) const noexcept;
  // Points (content ticks, cents relative to the note's Hz) → the note's curve (empty when it never leaves 0).
  void emit(const std::vector<Point>& pts, std::vector<engine::CurvePoint>& out) const;

  const doc::NoteClip& clip_;
  const TempoMap& tempo_;
  bool tempoMoves_;
  const doc::ClipPitches& pitches_;
  std::optional<doc::NotePitchIndex> pitchIndex_;  // built for the first note with a bend or a warp
  std::vector<std::pair<doc::NoteId, int64_t>> harmonics_;  // (note, its number in its group; > 0), sorted by note
  std::vector<Point> a_;                                     // scratch
};

// AP.7: one note clip's engine description — its notes' resolved Hz, identity signals (PitchSource from `signals`, the
// track's tuning signals: doc::trackTuningSignals), velocities, chance, curves (ClipCurveLowering) — from its resolved
// pitches. The ONE lowering of a clip: ProjectLink::lowerTrack (playback) and the offline note-stage renders
// (note_stage_render.h: the roll preview, Flatten) both call it.
engine::ClipDesc lowerClipDesc(const doc::Project& p, const doc::NoteClip& c, const doc::ClipPitches& cp,
                               std::span<const doc::TuningId> signals, bool tempoMoves);

}  // namespace pq::app
