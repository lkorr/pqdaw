#pragma once

// Main tunings model, resolution side (T2.7, SPEC §3.2): which tuning a track's notes play in and what each note
// sounds at. Named project tunings, track/clip assignment with Inherit / Main / Private, and the per-tuning reference
// pitch live in model.h (T3.4); this is the one place that turns an assignment into note pitches, so the playback
// lowering (T3.17) and tests agree on it:
//   - effectiveTuningRef: the tuning a track (and optionally one of its clips) uses, following Inherit up the group
//     chain to the project's first main tuning, plus WHICH main tuning that is (none for a private tuning);
//   - resolveTrackPitches: every note of every clip of a track, resolved in its clip's effective tuning: the static
//     Hz (double; resolveHzDouble, a Ratio anchored to another note of the clip follows that note), and for a note
//     in a main tuning with an audio-rate linear form (EDO, MOS without MODMOS / generator sequence:
//     tuning::linearTuningForm) the tuning signal it follows and how (T2.6, T2.6-fix3: PitchFollow below; the
//     playback link's engine::PitchSource).
// Reassigning a track (SetTrackTuning / SetClipTuning, or a group above an Inherit track) changes the effective
// tuning, so resolving again re-resolves all its notes; editing a main tuning (SetTuningDef / SetTuningParam)
// re-resolves every track that follows it. Not RT code (builds the tunings: allocates).

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/model.h"

namespace pq::doc {

struct EffectiveTuning {
  const TuningDef* def = nullptr;  // nullptr only for a dangling Main reference (commands never produce one)
  TuningId main;                   // the main tuning `def` belongs to; invalid for a private tuning
};
EffectiveTuning effectiveTuningRef(const Project& p, const TrackPath& path, const NoteClip* clip = nullptr) noexcept;

// True if the tuning has an audio-rate linear form (T2.6): EDO, or MOS without MODMOS alterations or a generator
// sequence. Scale tunings and MODMOS play static pitches (QUESTIONS Q100).
bool hasTuningSignal(const TuningDef& def) noexcept;

// How a note follows its clip's tuning signal (T2.6-fix3, M3 review H4; QUESTIONS Q1 / Q100.1):
//   Degree     a Degree note, or a Ratio anchored (through any chain of note anchors) to one: the node computes the
//              root anchor's degree (`degree`, `equave`) at the tuning's current parameters, and the note sounds at
//              that pitch times its fixed ratio to it (its static Hz over the root's);
//   Reference  a Ratio anchored to the reference, a Cents note, or a Ratio anchored (through notes) to one of them:
//              hz = static hz · reference(t) / reference₀ (it keeps its ratio to the tuning's reference);
//   Static     everything else (a private / Scala / MODMOS tuning, Harmonic and JI-fundamental identities).
enum class PitchFollow : uint8_t { Static, Degree, Reference };

struct NotePitch {
  NoteId note;
  std::optional<double> hz;  // nullopt: unresolvable (a missing or cyclic anchor, a Harmonic identity without its JI
                             // object (M7), a pitch outside float32's range)
  TuningId signal;           // valid: the note follows this main tuning's signal (T2.6); invalid: static Hz
  PitchFollow follow = PitchFollow::Static;
  int32_t degree = 0;        // Degree: the (root anchor's) Degree identity
  int32_t equave = 0;
  // T4.17 (M4 review M7): the note's absolute degree in its clip's tuning (equave · size + degree for a Degree identity;
  // between two degrees, fractional in log frequency, for any other pitch): what an Instrument Rack's Degree key zones
  // compare (voice.noteDegree). nullopt with `hz`.
  std::optional<double> keyDegree;
};

struct ClipPitches {
  ClipId clip;
  EffectiveTuning tuning;
  std::vector<NotePitch> notes;  // in the clip's note order (start, id)
};

struct TrackPitches {
  TrackId track;
  std::vector<ClipPitches> clips;  // in the track's clip order
};

struct ResolveOptions {
  // T2.6-fix3: the tuning a clip following main tuning `id` (with definition `def`, which has a signal) resolves its
  // static Hz in, instead of `def` itself (the playback link passes its node table's definition, which lags the
  // document after a live edit, so an unmodulated node passes the static Hz through exactly); null: `def`.
  std::function<const tuning::Tuning*(TuningId id, const TuningDef& def)> signalTuning;
};

// Errors: NotFound (no such track). A clip whose tuning does not build (a dangling reference) has every note
// unresolved.
Result<TrackPitches> resolveTrackPitches(const Project& p, TrackId track, const ResolveOptions& options = {});

}  // namespace pq::doc
