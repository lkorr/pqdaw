#pragma once

// The Phase accumulator meta module, `phase_acc` (TMM.3; docs/plans/MM.md "Phase accumulator face", SPEC §15
// 2026-11-01): a phase ramp whose frequency is the voice's note PLUS an offset in Hz.
//
//   inputs   hz  (default voice.hz in a voice context, else 0)   pm (audio, added to the phase; default 0)
//   params   freq   0 … 20 kHz, default 0 Hz, added to hz. Face law t → f = 20 kHz · (2^(14t) − 1)/(2^14 − 1): t = 0 is
//                   exactly 0 Hz (the incoming note plays exactly), t = 1 is 20 kHz (`@curve(exp0, 14)`).
//            phase  0 … 1, default 0, wraps: the phase offset (the start phase: the per-voice phasor resets at note-on)
//            amp    0 … 4, default 1: multiplies the output (SPEC: "a multiplier on the output"; QUESTIONS MM-1)
//   output   out = amp · frac(phasor(hz + freq) + phase + pm)   (the phasor's uint32 accumulator, SPEC §3.1)
//   chain    a chain device at the instrument position (its voices end at note-off); not an audio effect.
//   tap      (TMM.5) voice_tap(frac(…), slot) into its VoiceTap request 0: the face's per-voice dots.
//
// Cost: phasor + 4 ops (hz + freq, + phase, frac, · amp); an unwired pm is folded away (x + (−0) → x); the tap is one
// relaxed load per block while no face reads it.

#include "pq/meta/type.h"

namespace pq::meta {

const Type& phaseAccType() noexcept;

inline constexpr double kPhaseAccMaxHz = 20000.0;
inline constexpr double kPhaseAccCurveK = 14.0;
// The face law and its inverse (the freq ParamSpec's Exp0 curve).
double phaseAccFreqFromTravel(double t) noexcept;
double phaseAccTravelFromFreq(double hz) noexcept;

}  // namespace pq::meta
