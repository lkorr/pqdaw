#pragma once

// SM.1: a pitch's MIDI key (voice.key, MIDI export): the shared arithmetic. The rules per pitch kind are
// doc::midiKeyOf's (pq/document/midi_key.h); the arpeggiator (RT) uses these directly.

#include <cmath>
#include <cstdint>

namespace pq::tuning {

inline constexpr int16_t kNoMidiKey = -1;  // unknown, or outside 0 … 127

// The nearest 12-TET key (key 69 = 440 Hz), unclamped; hz > 0 and finite.
inline int64_t nearestMidiKey(double hz) noexcept { return std::llround(69.0 + 12.0 * std::log2(hz / 440.0)); }  // RT-safe

// A key as voice.key carries it: 0 … 127, anything else kNoMidiKey.
inline int16_t midiKeyOrNone(int64_t key) noexcept {  // RT-safe
  return key >= 0 && key <= 127 ? static_cast<int16_t>(key) : kNoMidiKey;
}

// The nearest 12-TET key of `hz` as voice.key carries it (kNoMidiKey for a non-positive / non-finite hz).
inline int16_t nearestMidiKeyOrNone(double hz) noexcept {  // RT-safe
  return hz > 0.0 && hz < 1e30 ? midiKeyOrNone(nearestMidiKey(hz)) : kNoMidiKey;
}

}  // namespace pq::tuning
