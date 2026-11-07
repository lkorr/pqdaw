#pragma once

// RETIRED meta-module types (GU.4; docs/plans/GU.md "Phases" 4): the Phase Accumulator (`phase_acc`, TMM.3) and the
// VCA (`vca`, TMM.14) are group presets now, built from atom blocks and value blocks (pq/document/modules.h). What is
// kept here is only the DESCRIPTION of their old state — inputs (with their meanings), params (with their declared
// curves), settings, outputs, chain input — exactly as they were, so that a file saved before GU.4 decodes, migrates
// (schema 18 → 19 groups its voice runs, 19 → 20 turns its values into knob positions through these curves) and is then
// converted into the presets (doc::convertRetiredModules). A retired type has no body, expansion, face or prelude
// entry: findType() never returns one, so nothing lowers, draws or creates it; the document's load path asks
// findTypeOrRetired().
//
// The old law of each (for the conversion and its tests):
//   phase_acc   out = amp · frac(phasor((hz + freq) · 2^(pitch/1200)) + phase + pm); freq 0 … 20 kHz (exp0, k = 14,
//               signed), phase 0 … 1 (wraps), amp 0 … 4, pitch ±4800 cents; inputs hz (Frequency; the chain input), pm
//               (Phase)
//   vca         out = gain(in, value(p), 2 ms, start: 1); `value` the knob position p, `min` / `max` ±20000, `zone`
//               0.05 … 0.9 (the Log curve's fine zone: dsp::CurvePreset::Vca reproduces it), `bend` −1 … 1 (the Bend
//               curve: lo + (hi − lo)·(e^(8·bend·p) − 1)/(e^(8·bend) − 1)), six marker offsets mk_n100 … mk_p100 (a
//               warp of the Log curve through ±1, ±10, ±100; not carried over); setting `curve` 0 Log / 1 Bend; input
//               `in` (default 1; the chain input)
//
// Not RT code.

#include <string_view>

#include "pq/meta/type.h"

namespace pq::meta {

inline constexpr std::string_view kRetiredPhaseAcc = "phase_acc";
inline constexpr std::string_view kRetiredVca = "vca";

// The retired type `id` (its state description only: body null), or null.
const Type* findRetiredType(std::string_view id) noexcept;
// A registered type, else a retired one (the document's decode and migrations), else null.
const Type* findTypeOrRetired(std::string_view id) noexcept;

// The VCA's state, by param (the retired type's params, in this order) and its setting.
enum RetiredVcaParam : uint32_t { kRetiredVcaValue, kRetiredVcaMin, kRetiredVcaMax, kRetiredVcaZone, kRetiredVcaBend };
inline constexpr uint32_t kRetiredVcaMarks = 6;  // mk_n100 … mk_p100 after kRetiredVcaBend
inline constexpr double kRetiredVcaRange = 20000.0;
inline constexpr double kRetiredVcaBendK = 8.0;     // the Bend curve's K = 8 · bend (|K| < 0.01: linear)
inline constexpr double kRetiredVcaLinearK = 0.01;
inline constexpr double kRetiredVcaDefaultPos = 0.7;

}  // namespace pq::meta
