#pragma once

// SM.1: a note's MIDI key, one rule shared by the sequence lowering (NoteEvent::key → voice.key), live input's
// computer keyboard and MIDI export (pq/document/midi_export.h, Degrees / 12edo modes).
//   Degree identity   60 + its absolute degree in its tuning (equave · size + degree), or, when the tuning carries a
//                     .kbm, the lowest key the .kbm maps to that degree (none: no key);
//   any other pitch   the nearest 12-TET key of its Hz (key 69 = 440 Hz).
//   Live MIDI input   the raw key (not this function).
// voice.key carries 0 … 127; anything outside (or no key) is tuning::kNoMidiKey (−1). Not RT code.

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "pq/tuning/midi_key.h"
#include "pq/tuning/pitch_id.h"

namespace pq::doc {

struct TuningDef;

// Absolute degree → MIDI key in one tuning: without a .kbm 60 + degree (unclamped); with one the lowest key mapping to
// that degree, else −1. Built once per tuning (a .kbm: 128 key lookups).
class DegreeKeyMap {
public:
  explicit DegreeKeyMap(const TuningDef* def);
  int64_t key(int64_t absDegree) const noexcept;
  bool hasKbm() const noexcept { return kbm_; }

private:
  bool kbm_ = false;
  std::vector<std::pair<int64_t, int16_t>> keyOf_;  // (degree, lowest key), sorted by degree
};

// The key of a note whose own identity is `id`, resolved at `hz` (> 0) with absolute degree `keyDegree`
// (NotePitch::keyDegree; a Degree identity's is exact). −1: none.
int16_t midiKeyOf(const tuning::PitchId& id, std::optional<double> keyDegree, double hz, const DegreeKeyMap& keys) noexcept;

}  // namespace pq::doc
