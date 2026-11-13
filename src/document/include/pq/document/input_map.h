#pragma once

// Live note input → note identities (T6.1; docs/plans/M6.md "The two note worlds"; model.h "Note input").
//
// Every incoming key — a MIDI port's note, the computer keyboard's — becomes a note through ONE map per MIDI track, built
// off the audio thread from the track's effective tuning (pitch_resolve.h effectiveTuningRef; KB.2: its live clip's,
// pitch_resolve.h liveClip) and its NoteInput:
//   Keys  key → a Degree identity: by the tuning's .kbm (tuning::mapKey; an unmapped key plays nothing), else
//         consecutive degrees from key 60 (= degree 0, equave 0), or the degree nearest the key's 12-TET pitch.
//   Hz    key → that degree's Hz in the tuning (or the key's 12-TET Hz), frozen as a Cents identity (freezeHz): exact,
//         no 128-key grid of its own. A pitch bend at note-on is frozen in too (atNoteOn); in Keys mode a bend only
//         moves the sounding voice (the stored identity is the note-on one; curves are M7).
// Each entry is resolved by resolvePitch (pitch_resolve.h) — the path resolveTrackPitches takes for a drawn note — so a
// live note sounds exactly as the same note drawn in a clip of the track, tuning signals (PitchFollow) included.
// The map is an immutable image: 128 entries, one indexed load per event (at(): RT-safe). Raw input data (port, channel,
// key, velocity, bend, time) is the capture buffer's business (T6.4 / T6.9), never lost here.

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include "pq/core/result.h"
#include "pq/document/model.h"
#include "pq/document/pitch_resolve.h"

namespace pq::doc {

struct InputPitch {
  tuning::PitchId id;                        // what a recorded note stores (Degree: Keys mode; Cents: Hz mode)
  double hz = 0.0;                           // what it sounds at (the drawn note's static Hz)
  PitchFollow follow = PitchFollow::Static;  // how it follows its tuning signal (as the drawn note)
  TuningId signal;                           // the main tuning it follows (invalid: static)
  int32_t degree = 0, equave = 0;            // follow Degree: the identity's degree and equave
  float noteDegree = std::numeric_limits<float>::quiet_NaN();  // absolute degree (voice.noteDegree; NaN: none)
  bool mapped = false;                       // false: the key plays nothing
};

// M6 review M4 / M5: the identity a live note stores, from what was kept at its note-on — the map's identity then and
// the source's pitch bend then (the record tap / the capture ring): a Cents identity (Hz mode) gets the bend added,
// which is exactly the pitch the note-on played; any other kind (Keys mode's Degree) stays the key's identity (the bend
// moved only the voice). Never re-read from the tuning or the track's note input mode as they are now.
inline tuning::PitchId playedIdentity(const tuning::PitchId& id, double bendCents) noexcept {
  if (id.kind() != tuning::PitchKind::Cents || bendCents == 0.0 || !std::isfinite(bendCents)) return id;
  return tuning::PitchId::fromCents(id.cents() + bendCents);
}

class InputPitchMap {
public:
  static constexpr uint32_t kKeys = 128;

  // The pitch of MIDI key `key` (0 … 127; higher bits ignored). RT-safe.
  const InputPitch& at(uint8_t key) const noexcept { return keys_[key & 127u]; }
  // The pitch a note-on of `key` stores with `bendCents` of pitch bend: Hz mode freezes the bent pitch (the key's Hz ×
  // 2^(bend/1200)); Keys mode ignores the bend (the identity stays the degree). Not RT code (resolves: no allocation,
  // but bounded work per call — the caller is the input thread, not the audio thread).
  InputPitch atNoteOn(uint8_t key, double bendCents) const;
  // The computer keyboard (and any degree source): absolute degree `d` of the track's tuning (equave · size + degree)
  // through this map's mode — Keys: that degree; Hz: its Hz frozen. With a 12-TET key map, `d` counts semitones from
  // key 60 as a key would. Not RT code (as atNoteOn).
  InputPitch pitchForDegree(int64_t d) const;
  // MF.2 (MIDI import of a bent / MTS-retuned key): the absolute frequency `hz` through this map's mode — Keys: the
  // degree of the track's tuning nearest `hz` when within `snapCents` of it, else `hz` frozen (Cents); Hz: `hz` frozen.
  // `id` is always set (unmapped only when `hz` is not a positive finite number). Not RT code (as atNoteOn).
  InputPitch atHz(double hz, double snapCents) const;

  const NoteInput& input() const noexcept { return input_; }
  int64_t degreesPerEquave() const noexcept { return size_; }

private:
  friend Result<std::shared_ptr<const InputPitchMap>> buildInputPitchMap(const Project&, TrackId, const NoteInput&,
                                                                          std::shared_ptr<const tuning::Tuning>,
                                                                          const NoteClip*);
  InputPitch resolved(const tuning::PitchId& id) const;                          // in play_ (what sounds)
  InputPitch resolvedIn(const tuning::Tuning& t, const tuning::PitchId& id) const;
  InputPitch keysPitch(const tuning::PitchId& degreeId) const;  // the mode applied to a Degree identity
  InputPitch hzPitch(double hz) const;                          // a frozen Hz
  std::optional<tuning::PitchId> nearestDegree(double hz) const;
  InputPitch twelveTet(int64_t key, double bendCents) const;

  std::array<InputPitch, kKeys> keys_{};
  NoteInput input_;
  std::shared_ptr<const tuning::Tuning> tuning_;  // the document's definition: the keys' Hz
  std::shared_ptr<const tuning::Tuning> play_;    // where the static Hz is resolved (T6.4: the playback link's node table)
  double referenceHz_ = tuning::kDefaultReferenceHz;
  TuningId signal_;
  int64_t size_ = 0;
};
using InputPitchMapPtr = std::shared_ptr<const InputPitchMap>;

// The map of MIDI track `track` with input settings `input` (its own Track::noteInput in the second form). Errors:
// NotFound (no such track), InvalidArgument (not a MIDI track), the tuning's build error (a dangling reference).
// T6.4: `signalTuning` (optional) = ResolveOptions::signalTuning's answer for the track's main tuning when it has a signal:
// the tuning the playback link resolves drawn notes' static Hz in (its node table's definition, which can lag the
// document after a live tuning edit). Entries resolve their static Hz there, exactly as the drawn notes do; the keys'
// Hz (Hz mode, the 12-TET maps) still come from the document's definition. Ignored for a track without a signal.
// KB.2: `clip` (optional, one of the track's clips: its live clip, pitch_resolve.h liveClip) = the clip whose effective
// tuning the map plays in (effectiveTuningRef(p, path, clip)) instead of the track's; `signalTuning` is then that main
// tuning's.
Result<InputPitchMapPtr> buildInputPitchMap(const Project& p, TrackId track, const NoteInput& input,
                                            std::shared_ptr<const tuning::Tuning> signalTuning = nullptr,
                                            const NoteClip* clip = nullptr);
Result<InputPitchMapPtr> buildInputPitchMap(const Project& p, TrackId track);

// A frequency as an exact Cents identity of tuning `def` (cents above its reference pitch): the Hz world's notes.
tuning::PitchId freezeHz(double hz, const TuningDef& def) noexcept;
tuning::PitchId freezeHz(double hz, double referenceHz) noexcept;
// The 12-TET pitch of MIDI key `key` (key 69 = 440 Hz).
inline double twelveTetHz(double key) noexcept { return 440.0 * std::exp2((key - 69.0) / 12.0); }

}  // namespace pq::doc
