#pragma once

// JI object evaluation (T7.2; SPEC §5; docs/plans/M7.md "Data model", amendment A1; model.h "JI objects and note
// curves"): what a JI object sounds like at every tick. The ONE evaluation the piano roll (drawing), the playback
// lowering (per-note pitch / gain curves) and flattening share, so they cannot disagree.
//
// Pitches are CENTS ABOVE THE TUNING'S REFERENCE (tuning::Tuning::referenceHz), as double; Hz = reference ·
// 2^(cents / 1200). Ticks are OBJECT ticks (0 = JiObject::start), valid over [0, length]. The tuning is the clip's
// effective tuning, built (pitch_resolve.h effectiveTuningRef + buildTuning): it resolves the fundamental and the
// pitches of tuning stops. A modulated tuning is not followed here: callers evaluate with the tuning as it is at the
// object's start (documented limit, A1).
//
// The model of one voice v:
//   stop pitch   proportion stop s: fundamental · scale_s · term (undertone: · scale_s / term), term = the stop's entry
//                jiVoiceEntry(o, s, v); exact as a Rational (jiVoiceRatio).
//                tuning stop s: the entry's own pitch in the tuning (the fundamental plays no part).
//   timing       the voice ARRIVES at stop s at arrive_s = max(at_s + delay, arrive_{s−1}) and glides from
//                begin_s = max(arrive_s − glide, arrive_{s−1}) (jiTransition; delay / glide / shape from the stop's
//                timing or the voice's override). Between arrive_{s−1} and begin_s it holds stop s−1. Stop 0 is held
//                from tick 0.
//   transition   cents = a + (b − a) · jiShapeValue(shape, u), u = (t − begin) / (arrive − begin); a jump when
//                begin == arrive (from arrive on the voice is at b).
//   ride         JiObject::ride (cents, the automation curve law) is added to every voice and to the fundamental.
// The fundamental itself moves the same way through fundamental · scale_s with each stop's default timing
// (jiFundamentalCents), also across tuning stops (their scale is kept for exactly that).
//
// Cost: the single queries are allocation-free and noexcept: O(stops at or before the tick) to find the segment (a
// linear scan, because per-voice delays make arrivals a running maximum), two pitch evaluations, one ride lookup. Use
// jiVoicePlan (one pass per voice, allocates its result) for anything that walks an object.

#include <cstdint>
#include <optional>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/model.h"
#include "pq/tuning/rational.h"
#include "pq/tuning/tuning.h"

namespace pq::doc {

// The entry of stop `s` (index into terms / pitches) voice `voice` plays: JiStop::voiceTerm, else the spread rule
// ⌊voice · n / V⌋ (7 voices on 5 entries: 0, 0, 1, 2, 2, 3, 4). Preconditions: voice < o.voices(), a valid object.
uint32_t jiVoiceEntry(const JiObject& o, const JiStop& s, uint32_t voice) noexcept;

// The exact ratio of a voice to the object's fundamental at a proportion stop: scale · term (undertone: scale / term).
// nullopt: a tuning stop, an index out of range.
std::optional<tuning::Rational> jiVoiceRatio(const JiObject& o, uint32_t voice, uint32_t stop) noexcept;

// s(u) of a transition shape, u clamped to [0, 1]; s(0) = 0, s(1) = 1.
double jiShapeValue(const JiShape& shape, double u) noexcept;

// When a voice glides into a stop (see "timing" above); stop 0: {0, 0}. `shape` is the transition's.
struct JiTransition {
  Ticks begin = 0;
  Ticks arrive = 0;
  JiShape shape;
};
// O(stop). Preconditions: voice < o.voices(), stop < o.stops.size().
JiTransition jiTransition(const JiObject& o, uint32_t voice, uint32_t stop) noexcept;

// Where a voice is at a tick: holding stop `stop` (next < 0), or `u` ∈ (0, 1) of the way in time into stop `next`.
// voice < 0: the fundamental (every stop's default timing). At a jump: the stop arrived at. O(stops at or before).
struct JiPosition {
  uint32_t stop = 0;
  int32_t next = -1;
  double u = 0.0;
  bool holding() const noexcept { return next < 0; }
};
JiPosition jiPosition(const JiObject& o, int32_t voice, double tick) noexcept;

// Cents above the tuning's reference of a pitch a fundamental or a tuning stop may hold (Degree, Cents, a
// reference-anchored Ratio). nullopt: another identity, or one the tuning cannot resolve.
std::optional<double> jiPitchCents(const tuning::PitchId& p, const tuning::Tuning& t) noexcept;

// The pitch of voice `voice` at object tick `tick` (clamped to [0, length]; at a jump: the value from that tick on),
// ride included. nullopt: voice ≥ V, or a pitch the tuning cannot resolve.
std::optional<double> jiVoiceCents(const JiObject& o, const tuning::Tuning& t, uint32_t voice, double tick) noexcept;
// The same in Hz. While the voice holds a stop and the ride is 0 there is no log / exp round trip: a proportion stop
// gives the fundamental's Hz times the exact ratio, a tuning stop the entry's pitch as the tuning resolves it (the Hz
// of the same note written without the object), so identities stay exact at holds.
std::optional<double> jiVoiceHz(const JiObject& o, const tuning::Tuning& t, uint32_t voice, double tick) noexcept;
// The fundamental (× the stops' scales, morphing with each stop's default timing; ride included).
std::optional<double> jiFundamentalCents(const JiObject& o, const tuning::Tuning& t, double tick) noexcept;
std::optional<double> jiFundamentalHz(const JiObject& o, const tuning::Tuning& t, double tick) noexcept;

// ---- The plan: every voice over the object's span ----------------------------------------------------------------

// A voice's pitch as breakpoints, linear in cents between them. Ticks ascend from 0 to length; two points on one tick
// are a jump (the second is the value from that tick on).
struct JiPitchPoint {
  Ticks tick = 0;
  double cents = 0.0;
  friend bool operator==(const JiPitchPoint&, const JiPitchPoint&) noexcept = default;
};
// A voice's gain (a factor on the object's level) as breakpoints, linear between them, same conventions.
// `sync` (T7.7; -1: none) is the voice a unison is completed against (its leader). On the point a tick's value lands
// on: the step onto it happens only together with the Syncs to that voice at that tick. On the first point of a jump:
// this voice is Synced to that voice first (engine/sequence.h "Synced unisons").
struct JiGainPoint {
  Ticks tick = 0;
  float gain = 1.0f;
  int16_t sync = -1;
  friend bool operator==(const JiGainPoint&, const JiGainPoint&) noexcept = default;
};
// From `from` (the voice's arrival at a stop; for a unison completed by a Sync, the tick it is complete) until the
// next entry: the voice shares its pitch with `size` voices (itself included). `locked`: it takes part in the unison
// — it is there when the last of them arrives — so it is, or is Synced to be, the identical lane of `leader` (the
// lowest such voice); `lockedSize` of the class are. Not locked: it moves on before the unison is complete and is
// parked against `leader` meanwhile.
struct JiUnison {
  Ticks from = 0;
  uint8_t leader = 0;
  uint8_t size = 1;
  uint8_t lockedSize = 1;
  bool locked = true;
  friend bool operator==(const JiUnison&, const JiUnison&) noexcept = default;
};
struct JiVoicePlan {
  std::vector<JiPitchPoint> pitch;  // ≥ 2 points: first at 0, last at length
  std::vector<JiGainPoint> gain;    // ≥ 2 points: first at 0, last at length
  std::vector<JiUnison> unison;     // ≥ 1 entry: first at 0; an entry only where something changes
};
struct JiPlan {
  std::vector<JiVoicePlan> voices;  // V entries
};
struct JiPlanOptions {
  // The flattened pitch stays within this many cents of the exact curve (transition shapes and ride curvature are
  // subdivided until it does; holds and straight lines cost two points).
  double toleranceCents = 0.05;
  // The fade of a voice that moves on before the unison it glided into is complete (it is parked against it), in
  // ticks (the caller converts its time, 20 ms in the lowering, at the object's tempo).
  Ticks gainRamp = kPpq / 16;
};
// Gains (M7 core decision 3, T7.7; Q281). A unison is k identical lanes at 1 / k each: one voice's level, exactly.
// Voices are identical lanes when they share a pitch from tick 0, or after a Sync; they stay identical while they
// move the same way (same entries, same timing).
//   - MERGE. A voice gliding into a unison fades with its distance to the unison's keeper (its lowest voice):
//     gain = (distance / the distance when the glide began)², distances in cents — 1 where the glide begins, 0 where
//     it arrives, and already small when the two are a few Hz apart, where two free-running voices beat slowly (Q281:
//     the troughs of those beats, not the fast roughness of the interval, are what a merge without it sounds like).
//     The keeper keeps its level throughout. When the last voice of the unison has arrived, the merged voices are
//     Synced to the keeper and every voice of the unison steps to 1 / k at that tick (JiGainPoint::sync): on
//     identical lanes the sum does not move. A refused Sync leaves everything as it is — the keeper at its level, the
//     merged voice parked at 0 — which is the same sound.
//   - SPLIT, the mirror image. At the tick the first voice of a unison begins to leave, the voices that stay with
//     its lowest voice step to 1 / (their number) and the others to 0 (identical lanes: the sum does not move); each
//     leaving voice then rises with its distance to that voice: (distance / the distance at the end)². The level of
//     the object rises monotonically from one voice's to the chord's.
//   - A voice that glides onto a unison's pitch but moves on before the unison is complete fades like a merging
//     voice and is parked (over gainRamp at most) when it moves on; it comes back like a leaving voice.
// A jump (glide 0) steps. Voices that split and merge at once take the product of both.
// nullopt: an invalid object (no stops / voices) or a pitch the tuning cannot resolve. O(V · (stops + ride points) ·
// log) plus the subdivision of curved spans; a voice identical to the previous one copies its pitch points.
std::optional<JiPlan> jiVoicePlan(const JiObject& o, const tuning::Tuning& t, const JiPlanOptions& options = {});

}  // namespace pq::doc
