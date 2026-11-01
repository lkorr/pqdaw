#pragma once

// The Phase accumulator meta module, `phase_acc` (TMM.3; docs/plans/MM.md "Phase accumulator face", SPEC §15
// 2026-11-01): a phase ramp that always runs freely at its frequency bar PLUS whatever reaches its frequency input.
// TMM.10-fix1 (SPEC §15 2026-11-01 "notes are a signal"): nothing about it is an instrument — with nothing wired it
// runs once at the bar (at 0 Hz its output is a DC offset: its phase); it plays notes only because a note's frequency
// is wired into `hz` (the chain's wire from the note source, or `voice.hz` in a circuit).
//
//   inputs   hz  (Reads::Frequency, default 0; the chain input)   pm (Reads::Phase, added to the phase; default 0)
//   params   freq   0 … 20 kHz, default 0 Hz, added to hz. Face law t → f = 20 kHz · (2^(14t) − 1)/(2^14 − 1): t = 0 is
//                   exactly 0 Hz (an incoming note plays exactly), t = 1 is 20 kHz (`@curve(exp0, 14)`).
//            phase  0 … 1, default 0, wraps: the phase offset (the start phase: the per-voice phasor resets at note-on)
//            amp    0 … 4, default 1: multiplies the output (SPEC: "a multiplier on the output"; QUESTIONS MM-1)
//   output   out = amp · frac(phasor(hz + freq) + phase + pm)   (the phasor's uint32 accumulator, SPEC §3.1)
//   chain    any chain position: the chain feeds `hz` (Type::chainInput) — the note source (one voice per note), a
//            voice signal (FM per voice) or audio (FM) —, or nothing when the device unplugs it (doc::Device::chainInput
//            "-": one free-running voice).
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
