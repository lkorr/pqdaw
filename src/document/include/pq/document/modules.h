#pragma once

// GU.4 (SPEC §15 2026-11-06 "built GUIs"; docs/plans/GU.md "Phases" 4): the BUILT-IN MODULES made with the kit — group
// presets of atom blocks and value blocks with a panel of generic elements, built here (not files: the node menu, the
// browser and the load conversion paste them; a user saving one gets an ordinary .pqpreset group preset). They replace
// the meta modules of the same names, which are retired (pq/meta/retired.h):
//
//   Phase Accumulator   a group: In → [hz + Frequency] → × 2^(Pitch/1200) (cents_to_hz(Pitch, 1)) → phasor → + Phase
//                       → frac → × Amplitude → Out. The value blocks carry the old params' curves: Frequency 0 … 20 kHz
//                       exp0 (k 14), SIGNED (a wire may take it below 0: the phase runs backwards); Phase 0 … 1, WRAP;
//                       Amplitude 0 … 4; Pitch ±4800 cents (GU.5b: ±1200 over the middle half of the travel, as the
//                       old face's arc); and a constant 1 (the ratio's reference). Its panel (GU.5b): the old face — a
//                       body circle, four `arc` elements round it and the `orbit` in the middle. The meta
//                       module's `pm` input is an `add` after the phase, made only when something feeds it (a converted
//                       module whose pm was wired): an unused phase-modulation input costs no op.
//   VCA                 a group: In → mul ← Value → Out; the value block's curve is the VCA's law (dsp::CurvePreset::Vca,
//                       ±20000, the fine ±1 zone over 40 % of the travel), its knob at 0.7 (× 1). Its panel (GU.5b): the
//                       old face, one `dial` (the curve's ±1 / ±10 / ±100 points as its markers, range and curve in its
//                       popup).
//
// Both play exactly the atoms the meta modules' expansions scheduled (the Phase Accumulator bit for bit; the VCA's law
// is a curve now: the same function, evaluated by dsp::evalCurve / the curve atom instead of its own atom chain, and
// `mul` instead of the 2 ms smoothed gain: tests/app/test_module_presets.cpp).
//
// LOAD CONVERSION (convertRetiredModules, after every migration of parseProject / parsePreset): each retired meta
// device of a track's own list becomes its module's group, which KEEPS THE DEVICE'S ID (its place, view state, the
// cables into and out of its main ports, its group membership); the members get fresh ids. Its wires, automation lanes
// and (none can reach it: a macro maps only devices inside its rack, and a module is never inside a rack) mappings
// follow onto the inner blocks:
//   into a control (freq / phase / amp / pitch, the VCA's value)   the value block's `value` (a VCA on its Bend curve:
//                                                                   the wire's Add law becomes Units — it added to the
//                                                                   multiplier after the law, as a Units wire does)
//   into the chain input (hz, the VCA's in; or the main port)     the group's main input (the In terminal feeds it)
//   into another input (pm; hz when the chain input is elsewhere) the inner block's input, across the group's edge
//   out of `out` / the main output                                the group's main output
//   onto the VCA's min / max / zone / bend / markers             dropped (the curve holds them; curves are edited,
//                                                                   never automated or wired: GU.md "Decided")
// A VCA whose `in` nothing feeds (the old module was then a constant source of its value) becomes a Value block with
// the same curve and knob (keeping the id). A retired device inside a rack chain cannot be converted (a group lives in
// a track's circuit): the file is refused. The VCA's marker offsets are not carried over (QUESTIONS Q?-GU.4-a).
//
// Not RT code.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "pq/core/result.h"
#include "pq/document/ids.h"
#include "pq/document/model.h"
#include "pq/dsp/curve.h"

namespace pq::doc {

enum class ModuleKind : uint8_t { PhaseAccumulator, Vca };
struct ModuleInfo {
  ModuleKind kind;
  std::string_view key;   // "phase_acc", "vca" (a menu / browser item: kModuleItemPrefix + key)
  std::string_view name;  // "Phase Accumulator", "VCA"
  std::string_view description;
};
inline constexpr std::string_view kModuleItemPrefix = "module:";
std::span<const ModuleInfo> moduleInfos() noexcept;
const ModuleInfo* findModule(std::string_view key) noexcept;  // by key or name

// The value blocks' names (also their panel labels) and the atoms' (Device::name).
inline constexpr std::string_view kPaFrequency = "Frequency", kPaPhase = "Phase", kPaAmplitude = "Amplitude",
                                  kPaPitch = "Pitch", kPaReference = "1";
inline constexpr std::string_view kVcaValueName = "Value";

// The roles of a built Phase Accumulator (its blocks' ids).
struct PhaseAccumulatorRoles {
  DeviceId group, freq, phase, amp, pitch, ref;  // the group, the value blocks
  DeviceId sum, ratio, pitched, phasor, addPhase, pm, frac, out;  // the atoms (pm invalid: not made)
};
struct PhaseAccumulatorOptions {
  DeviceId id;                  // the group's id (invalid: a fresh one)
  DeviceId inGroup;             // the group it sits in (nested), or none
  std::string name;             // the group's name ("" = "Phase Accumulator")
  double freq = 0.0, phase = 0.0, amp = 0.25, pitch = 0.5;  // knob positions (the defaults: 0 Hz, 0, × 1, 0 cents)
  std::optional<dsp::Curve> freqCurve, phaseCurve, ampCurve, pitchCurve;  // (none: the module's own)
  enum class Feed : uint8_t { Hz, Pm, None } feed = Feed::Hz;  // what the In terminal feeds (the old chain input)
  bool pm = false;              // make the phase-modulation add (fed by `feed` Pm or by wires)
  bool active = true;
};
BlockSet phaseAccumulatorBlocks(IdSource& ids, const PhaseAccumulatorOptions& o = {}, PhaseAccumulatorRoles* roles = nullptr);
// The module's own control curves.
dsp::Curve phaseAccumulatorCurve(std::string_view control);  // kPaFrequency …

struct VcaRoles {
  DeviceId group, value, mul;
};
struct VcaOptions {
  DeviceId id;
  DeviceId inGroup;
  std::string name;  // "" = "VCA"
  double value = 0.7;  // the knob position
  std::optional<dsp::Curve> curve;  // none: the VCA's law (vcaCurve())
  bool plugged = true;              // the In terminal feeds the multiplier
  bool active = true;
};
BlockSet vcaBlocks(IdSource& ids, const VcaOptions& o = {}, VcaRoles* roles = nullptr);
// The VCA's curve: its law over [min, max] with fine zone `zone` (0: the default 0.4).
dsp::Curve vcaCurve(double min = -20000.0, double max = 20000.0, double zone = 0.0);

// A module's default group preset with ids from 1 (the group first): what spawning it pastes (ops::pasteBlocks gives
// fresh ids).
BlockSet moduleBlocks(ModuleKind k);

// ---- Load conversion ----
// Whether `p` holds a retired meta device (a track's own block or inside a rack chain).
bool holdsRetiredModules(const Project& p) noexcept;
// Converts every retired meta device of `p` (see above). Errors: one inside a rack chain.
Status convertRetiredModules(Project& p);
// A preset file's device that is a retired meta device, as its module's block set (the group first); nullopt: not one.
std::optional<BlockSet> convertRetiredPreset(const Device& d);

}  // namespace pq::doc
