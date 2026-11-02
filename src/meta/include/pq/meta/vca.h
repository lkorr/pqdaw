#pragma once

// The VCA meta module, `vca` (TMM.14; SPEC §15 2026-11-01 "a VCA meta module", after SeptaBee's Pomegranate): one
// input, one output, one knob. out = in × value, where value comes from the knob's position through a curve between a
// declared min and max (defaults −20000 … +20000).
//
//   inputs   in   (default 1; the chain input): wired, a VCA on it; unwired, the module is a constant source of `value`
//                 (an offset to wire into something else)
//   params   value  0 … 1, default 0.7: the KNOB POSITION p (not the multiplier: see "modulation" below). The face's
//                   knob sweeps 300° (7 o'clock … 5 o'clock); p = 0.3 / 0.7 are 10 / 2 o'clock.
//            min, max  −20000 … 20000, defaults −20000 / 20000: the value at p = 0 and p = 1 (max below min: a
//                   constant min)
//            zone   0.05 … 0.9, default 0.4: the share of the travel the FINE ZONE (|value| ≤ 1) takes when the range
//                   reaches past it on both sides (Log curve)
//            bend   −1 … 1, default 0: the Bend curve's shape (0: linear)
//   setting  curve  0 Log (default), 1 Bend (structural: each curve's expansion is only its own arithmetic)
//   output   out = gain(in, value, 2 ms, start: 1): a param-rate value is applied at block rate and smoothed by a 2 ms
//            one-pole while it moves (settled: one multiply per sample); an audio-rate value is used as it is
//   tap      voice_tap(p) into its VoiceTap request 0: the knob position each voice plays (the face's dots)
//
// THE LOG CURVE (vcaLaw): linear through the fine zone [−1, 1] (clipped to the range), logarithmic beyond it on each
// side. With the defaults: p 0.3 … 0.7 (10 → 2 o'clock) is −1 … +1 linearly, 0.7 … 1 rises to 20000 with equal ratios
// per travel toward the end, 0 … 0.3 mirrors it. In general, with lo = min, hi = max:
//   fine zone  F = [clamp(−1, lo, hi), clamp(1, lo, hi)], width fw
//   outers     positive from sP = max(1, lo) to hi when hi > sP (log extent EP = ln(hi / sP)); negative from
//              −sN = −max(1, −hi) down to lo when −lo > sN (EN = ln(−lo / sN))
//   travel     no outer: F takes all of it (linear). Else F takes zf = zone · fw / 2 and the outers share 1 − zf in
//              proportion to their log extents: negative [0, pN], fine [pN, pP], positive [pP, 1]
//   outer      v = s0 + α·u + b·(e^(K·u) − 1) for u ∈ [0, 1] from the join outward (mirrored on the negative side).
//              Where the fine zone joins it: K = max(ln(end / s0), 1), a = min(slope · len, end − s0), b = (end − s0 −
//              a) / (e^K − 1 − K), α = a − b·K: the value AND the slope are continuous at the join (C1: no kink in
//              the knob's feel), it reaches `end` exactly, and the e^(Ku) term makes the far part log-spaced
//              (×12 per quarter of the outer travel with the defaults: 1.85, 9.8, 137, 1680, 20000). Without a fine
//              zone (both ends on one side beyond 1, e.g. 20 … 20000): pure exponential, v = s0·(end / s0)^u.
// Monotonic (non-decreasing) in p for every state; continuous everywhere.
// THE BEND CURVE: v = lo + (hi − lo)·g(p), g = (e^(Kp) − 1) / (e^K − 1), K = 8·bend (|K| < 1e-3: g = p). bend > 0
// is exponential (fine at the low end), bend < 0 logarithmic (fine at the high end); presets: linear, exp, log.
//
// MODULATION: automation and macros act in KNOB SPACE (`value` is the position: they move the knob as a hand would,
// through the curve, and the face shows each voice's dot on the knob); migrated (Travel) wires too (doc::paramTravel's
// linear travel: pos + Σ amount · source, clipped to [0, 1]). TX.patching (SPEC §15 2026-11-01 "a wire adds its value
// in the target param's own units": reverses Q183's knob-space wires): a new wire into `value` adds to the MULTIPLIER
// (ParamSpec::unitsInBody: the expansion gets `in value_add`, and value = clip(curve(pos) + Σ wires, min, max)), so a
// signal of ±0.1 wobbles the gain by ±0.1 whatever the knob's position. To multiply by a signal's own value, wire it
// into `in` (or chain two VCAs). TX.knob-curves (SPEC §15 2026-11-01 "answers to Q186" + clarification: a wire adds
// before the knob's curve): that is now the law of a migrated Units wire and of an Add wire on the BEND curve (a line
// between min and max at bend 0: the knob's units); on the LOG curve (the default) an Add wire moves the knob position,
// in travel through the curve (Type::unitsLaw), as Q183 first had it. No param of the VCA takes a knob curve
// (ParamSpec::noKnobCurve): its curve box is its curve.
//
// COST: settled, one multiply per sample (the gain atom); the curve is param-rate arithmetic (once per block) unless
// `value` is modulated at audio rate, then per sample: Log ≈ 22 ops + one exp, Bend ≈ 8 ops + one exp. Multiplying by
// an audio-rate value is ring modulation: its sidebands (in ± value frequencies) can fold above Nyquist; it is not
// oversampled (QUESTIONS Q?-TMM.14-a).

#include "pq/meta/type.h"

namespace pq::meta {

const Type& vcaType() noexcept;

enum VcaParam : uint32_t { kVcaValue, kVcaMin, kVcaMax, kVcaZone, kVcaBend, kVcaParams };
enum class VcaCurve : uint8_t { Log = 0, Bend = 1 };
inline constexpr uint32_t kVcaCurveSetting = 0;
inline constexpr double kVcaRange = 20000.0;  // |min|, |max| ≤ this
inline constexpr double kVcaFine = 1.0;       // the fine zone's half-width (in value)
inline constexpr double kVcaBendK = 8.0;      // K = kVcaBendK · bend
inline constexpr double kVcaZoneMin = 0.05, kVcaZoneMax = 0.9;
inline constexpr double kVcaDefaultPos = 0.7;  // value 1 with the default curve and range

// The curve, resolved from a state (the face draws it; the expansion computes the same arithmetic in atoms; tests
// compare the two).
struct VcaLaw {
  VcaCurve curve = VcaCurve::Log;
  double lo = -kVcaRange, hi = kVcaRange;
  // Log
  double pN = 0.3, pP = 0.7;        // the fine zone's travel [pN, pP]
  double fLo = -1.0, slope = 5.0;   // v = fLo + (p − pN)·slope there
  struct Side {
    bool on = false;
    double s0 = 1.0, alpha = 0.0, b = 0.0, k = 0.0, len = 0.0;  // v = ±(s0 + α·u + b·(e^(K·u) − 1)), u = travel / len
  } neg, pos;
  // Bend
  double k = 0.0;
};
VcaLaw vcaLaw(VcaCurve curve, double min, double max, double zone, double bend) noexcept;
VcaLaw vcaLaw(std::span<const double> params, double curveSetting) noexcept;  // from a state (params in VcaParam order)
double vcaValue(const VcaLaw& law, double p) noexcept;        // p clamped to [0, 1]
double vcaPosition(const VcaLaw& law, double value) noexcept; // the inverse: the lowest p whose value reaches `value`

}  // namespace pq::meta
