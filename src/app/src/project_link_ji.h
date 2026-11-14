#pragma once

// T7.3 (docs/plans/M7.md "T7.3", amendment A1): how a note clip's JI objects and note curves become the engine's
// per-note curves (engine/sequence.h "Per-note curves"). Used by ProjectLink::lowerTrack only.
//
//   a note with a pitch curve (doc::NoteCurves)        its curve, flattened (the automation curve law → lines)
//   an inner note of an object (Harmonic(obj, voice))  its voice's curve over the note's span + its own curve
//   a Ratio anchored to an object's fundamental        the fundamental's curve over the note's span + its own
//   an object with `sounds`                            one note per voice over the object's span: hz, ratio,
//                                                      harmonic and unison class at stop 0, the voice's pitch and
//                                                      gain curves (doc::jiVoicePlan)
// Curve values are cents relative to the note's own Hz — the pitch the document resolves it to at its start
// (pitch_resolve.h) — so a hold on a proportion stop is the exact ratio (its cents come from the same doubles).
//
// Event volume. The engine sends one event per breakpoint and note; a track's event list is finite
// (TimelineConfig::maxEventsPerTrack per image block). So:
//   - shapes are flattened to kJiPitchTolerance cents (1 c: inaudible while the pitch moves, and a tenth of the
//     points 0.01 c would take; holds and straight lines are exact and cost two points);
//   - inside a curved span, breakpoints closer than a minimum spacing are dropped: 1 ms, or longer for an object with
//     many voices so that all of them together send about a quarter of the event list per block
//     (JiLowerLimits). Corners (where a hold begins or ends, jumps) are always kept.
// Gain ramps (a voice parked on a merge, back on a split) last 20 ms at the tempo where the object starts.
// Inside a tempo ramp, sloped segments are cut to a sixteenth note (the engine interpolates over samples).
// Tuning stops are flattened in the tuning as the document defines it: a modulated tuning is followed through the
// object's fundamental (the notes keep their ratio to it), not through a tuning stop's own degrees (A1).

#include <optional>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/ji_object.h"
#include "pq/document/model.h"
#include "pq/engine/sequence.h"
#include "pq/tuning/tuning.h"

namespace pq::app {

inline constexpr double kJiPitchTolerance = 1.0;   // cents
inline constexpr double kJiGainRampSeconds = 0.020;
inline constexpr double kJiMinSpacingSeconds = 0.001;

struct JiLowerLimits {
  double blockSeconds = 64.0 / 48000.0;  // the engine's image block
  uint32_t eventsPerBlock = 256;         // TimelineConfig::maxEventsPerTrack
};

// One note clip's JI lowering. Plans are made once per object, on first use. Not RT code.
class ClipJiLowering {
public:
  // `tuning`: the clip's effective tuning as its notes are resolved in (null: nothing resolves). `tempoMoves`: the
  // tempo map is not one constant tempo.
  ClipJiLowering(const doc::NoteClip& clip, const tuning::Tuning* tuning, const TempoMap& tempo, bool tempoMoves,
                 const JiLowerLimits& limits);

  // The pitch curve of clip note `n` (see above) into nd.pitchCurve; nd.start must be set.
  void noteCurve(const doc::Note& n, engine::NoteDesc& nd);
  // What a sounding object's voices play: one NoteDesc each (start, length, id, hz, velocity, ratio, harmonic, unison,
  // curves; `pitch` / keyDegree left for the caller), appended to `out`. Returns the object's voice count appended
  // (0: not sounding, or unresolvable).
  uint32_t voices(const doc::JiObject& o, std::vector<engine::NoteDesc>& out);

  struct Point {
    Ticks tick;
    double value;
  };

private:
  struct ObjectPlan {
    bool tried = false, fundamentalTried = false;
    bool followed = false;  // a note of the clip follows the object
    std::optional<doc::JiPlan> plan;
    std::vector<doc::JiPitchPoint> fundamental;  // empty: unresolvable
    Ticks spacing = 1, gainRamp = 1;
  };
  ObjectPlan& planOf(const doc::JiObject& o);
  const std::vector<doc::JiPitchPoint>& fundamentalOf(const doc::JiObject& o, ObjectPlan& p);
  double bpmAt(Ticks contentTick) const noexcept;
  Ticks ticksOf(double seconds, Ticks contentTick) const noexcept;
  // Points (content ticks, cents relative to the note's Hz) → the note's curve (empty when it never leaves 0).
  void emit(const std::vector<Point>& pts, std::vector<engine::CurvePoint>& out) const;
  void voiceCurve(const std::vector<doc::JiPitchPoint>& plan, Ticks origin, Ticks spacing,
                  std::vector<engine::CurvePoint>& out) const;

  const doc::NoteClip& clip_;
  const tuning::Tuning* tuning_;
  const TempoMap& tempo_;
  bool tempoMoves_;
  JiLowerLimits limits_;
  std::vector<ObjectPlan> plans_;  // by index into the clip's object list
  std::vector<Point> a_, b_, c_;   // scratch
};

}  // namespace pq::app
