#pragma once

// The Envelope meta module, `envelope` (TMM.11; docs/plans/MM.md TMM.11, SPEC §15 2026-11-01): attack, decay,
// sustain and release with a curve per segment, used as a VCA or as a modulation source.
//
//   inputs   in    (default 1): out = env · amount · in — wired, a VCA (a chain voice run's signal, the Meta Operator's
//                  oscillator); unwired, the envelope itself
//            gate  (Bool, default voice.gate; false outside a voice context)
//   params   attack  0 … 60 s, default 5 ms      the rise 0 → 1 (log travel: `@curve(exp0, k)`, k = log2(60001), so a
//            decay   1 ms … 60 s, default 400 ms  face's travel t is (60001^t − 1) ms: equal steps per decade)
//            sustain 0 … 1, default 0.6
//            release 1 ms … 60 s, default 250 ms
//            acurve, dcurve, rcurve  −1 … 1, default 0: each segment's bend (below)
//            amount  0 … 1, default 1: multiplies the output
//   outputs  out; done (Bool: released and below −80 dB, 1e-4 — Type::voiceEnd: an instrument chain's voices end there,
//            a meta voice run whose last device is an Envelope ends its voices there instead of the fixed release)
//   tap      voice_tap(0.4 · level + (gate ? 0 : 0.5)) into its VoiceTap request 0: the face's dots (EnvelopeTap
//            decodes it: the level and whether the note is released).
//
// THE DSP is stdlib/envelopes/msenv.pqp's (mode None, soft start; Initial 0, Peak 1, End 0), written as atoms with each
// segment generalised by its curve, so that with every curve 0 it computes msenv's arithmetic exactly (bit-identical:
// tests/meta/test_envelope.cpp, both backends):
//   attack   curve 0: linear at 1/(a·sr) per sample, as msenv. c ≠ 0: the one-pole recursion L ← T + (L − T)·k toward
//            an overshoot target T = 1 + ov (c > 0: concave, fast then slow) or away from T = −ov (c < 0: convex),
//            reaching 1 exactly at time a from 0: depth δ = ln(10⁴)·|c|, q = e^−δ, ov = q / (1 − q), k = e^∓δ/N.
//            Implemented as msenv's step `min((T − b)·m, rate)` (m = 1 − k; curve 0: T = 1, m = 1, msenv's rate; the
//            attack only rises, so msenv's clip(dist, ±rate) is this min), so the linear case is msenv's expression.
//   decay /  curve 0: exponential toward the goal, −80 dB of the distance after the time (msenv: δ = ln 10⁴).
//   release  c > 0 bends toward a straight line: δ = ln(10⁴)·(1 − c) and an overshoot ov = q / (1 − q) beyond the goal
//            of the segment's span (decay: 1 − sustain, release: 1, the full scale), so the goal is reached exactly at
//            the time (from the span's top); c < 0 bends further: δ = ln(10⁴)·(1 + 2|c|), no overshoot. Per sample:
//            msenv's L ← g + (L − g)·k, minus sign(L − g)·w (w = ov·span·(1 − k): the overshoot's pull), clamped
//            between L and g (never past the goal; curve 0: w = 0 and the clamp is exact, so this is msenv's value).
// Every recursion is autonomous (its next value depends only on the level), so a retrigger or an early release
// continues the same curve from the level where it is: no clicks.
//
// Cost (per voice-sample): msenv's ≈ 22 ops + the curves (≈ 8: the attack step 2, the decay / release pull and clamp
// 6) + the tap (2) + amount (1); everything else (laws, coefficients) is param rate. Measured (JIT, 16 held voices,
// tests/meta/test_envelope.cpp perf): 0.86 ns/voice-sample against msenv's 0.55 and a bare voice's 0.24 (the tap is
// 0.09 of it); the curves cost the same at every value (no branch).

#include "pq/meta/type.h"

namespace pq::meta {

const Type& envelopeType() noexcept;

// Params, in Type::params order.
enum EnvelopeParam : uint32_t { kEnvAttack, kEnvDecay, kEnvSustain, kEnvRelease, kEnvACurve, kEnvDCurve, kEnvRCurve,
                                kEnvAmount, kEnvParams };
inline constexpr double kEnvMaxSeconds = 60.0;
inline constexpr double kEnvCurveK = 15.872698924987581;  // log2(60001): travel t → (60001^t − 1) ms on [0, 60 s]
inline constexpr double kEnvDepth = 9.210340371976184;  // ln(10⁴): msenv's −80 dB per stage time

// ---- the shape (the face draws it, tests check the DSP against it): continuous time, u = time / the segment's time.
// A segment's law from its curve c ∈ [−1, 1].
struct EnvSegmentLaw {
  bool linear = false;  // attack only: c = 0 (|c| < 1e-3)
  double delta = 0.0;   // depth (nepers over the segment's time)
  double ov = 0.0;      // overshoot, in spans beyond the goal (attack: beyond 1 for c > 0, below 0 for c < 0)
  bool convex = false;  // attack only: c < 0 (away from −ov)
};
EnvSegmentLaw envAttackLaw(double curve) noexcept;
EnvSegmentLaw envFallLaw(double curve) noexcept;  // decay and release
// The attack's level at u ∈ [0, 1] (0 → 1) and its inverse (the u at which it reaches `level`).
double envAttackLevel(const EnvSegmentLaw& law, double u) noexcept;
double envAttackTime(const EnvSegmentLaw& law, double level) noexcept;
// A falling segment from `from` toward `goal` (span: the overshoot's unit: decay 1 − sustain, release 1): its level at
// u ≥ 0 and the u at which it reaches `level` (clamped to [0, the time it arrives]; a level beyond `from`: 0).
double envFallLevel(const EnvSegmentLaw& law, double from, double goal, double span, double u) noexcept;
double envFallTime(const EnvSegmentLaw& law, double from, double goal, double span, double level) noexcept;

// What the tap carries, decoded (the face's dots).
struct EnvelopeTap {
  double level = 0.0;
  bool released = false;
};
EnvelopeTap decodeEnvelopeTap(double value) noexcept;
double encodeEnvelopeTap(double level, bool released) noexcept;  // what the expansion records (tests, synthetic voices)

}  // namespace pq::meta
