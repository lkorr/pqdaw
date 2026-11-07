#pragma once

// Device panels (T4.6, SPEC §7.3): a device is a patch; its front panel's controls are the patch's `param` pins
// (written at run time, no recompile) and its structural settings, the `in … : real const` inputs (a change recompiles
// and crossfades, SPEC §7.5; QUESTIONS Q121). This header reads both from the patch and maps their values to what a
// control shows: knob position (0..1), display text, typed values.
//
// Values are always in the patch's internal units (PATCH_LANGUAGE §1: a dB param holds a linear gain, % a fraction,
// Hz Hz); display units come from @unit(…) or the default's unit suffix. GU.1: the document stores a param's KNOB
// POSITION; a control's position ↔ value is its curve (ParamInfo::curve: the instance's, else the declared one;
// pq/dsp/curve.h), what toPosition / fromPosition follow.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/lower.h"
#include "pq/graph/patch.h"
#include "pq/ui/draw_list.h"

namespace pq::views::devices {

enum class DisplayUnit : uint8_t { None, Hz, Db, Percent, Seconds };

// One `param` of the device patch.
struct ParamInfo {
  std::string name;
  std::string label;        // @label("…"), else the name
  DisplayUnit unit = DisplayUnit::None;
  double min = 0.0, max = 1.0, def = 0.0;  // internal units
  bool exp = false;         // @curve(exp): knob position is logarithmic in the value
  double exp0 = 0.0;        // TMM.10: @curve(exp0, k): k > 0, value = min + (max − min)·(2^(k·t) − 1)/(2^k − 1)
                            // (exponential from exactly min: the Phase accumulator's frequency; Q167 8)
  int32_t steps = 0;        // @steps(n): n integer positions (0 = continuous)
  bool isBool = false;
  bool hidden = false;      // @hidden
  // GU.1: the curve the knob plays (the instance's, doc::Device::curves, else `declared`) and the one its declaration
  // gives (graph::declaredCurve). toPosition / fromPosition follow `curve`.
  dsp::Curve curve, declared;
};

// One structural setting: an `in name : real const = default` input.
struct ConstInfo {
  std::string name;
  double def = 0.0;
};

struct DeviceSignature {
  std::string patch;                 // the device patch's name
  std::vector<ParamInfo> params;     // declaration order
  std::vector<ConstInfo> consts;
  std::vector<std::string> exports;  // `export` jacks (SPEC §7.3: the output jacks on top of the device)
  std::string audioIn;               // the first non-const `in` (the signal input; "" for an instrument)
  uint16_t audioInWidth = 2;         // its declared channels (2 when not declared)
  const ParamInfo* findParam(std::string_view name) const noexcept;
  const ConstInfo* findConst(std::string_view name) const noexcept;
};

// Reads the signature of a device patch.
DeviceSignature signatureOf(const graph::Patch& patch);

// ---- value mapping (UI thread, cheap: once per change) ---------------------------------------------------------------
// GU.1: knob / slider position in [0, 1] ↔ value through the knob's curve (ParamInfo::curve): fromPosition is
// dsp::evalCurve, toPosition dsp::curvePosition (the position nearest `hint` that gives the value; one the curve never
// reaches snaps to the nearest it does).
double toPosition(const ParamInfo& p, double value, double hint = 0.0) noexcept;
double fromPosition(const ParamInfo& p, double position) noexcept;
// GU.1: the curve presets the knob's curve menu offers (doc::curvePresets of the declared curve: continuous params only),
// in menu order, and the label of each ("Octave", "Exponential", "Logarithmic", "Linear").
std::vector<dsp::CurvePreset> knobCurves(const ParamInfo& p);
const char* knobCurveLabel(dsp::CurvePreset c) noexcept;
// GU.1: whether the knob plays a curve other than its declared one (its glyph is drawn).
inline bool curveSwitched(const ParamInfo& p) noexcept { return !(p.curve == p.declared); }
// GU.1: the knob's position after a wire's sweep `delta` (a tap's up / down, doc::ModulationTap::Domain) from knob
// position `position`: in position, in the curve's units (dB for a dB gain), or in the DSP units after the curve.
double sweptPosition(const ParamInfo& p, double position, double delta, doc::ModulationTap::Domain domain) noexcept;
// A small drawing of curve `c` in `box` (left → right = position 0 → 1, bottom → top = its value in its units: dB for a
// dB gain), for a knob switched off its declared curve.
inline constexpr size_t kCurveGlyphPoints = 9;
void curveGlyphPoints(const dsp::Curve& c, ui::Rect box, std::array<ui::Vec2, kCurveGlyphPoints>& out) noexcept;
// GU.1: the knob position device `d` stores for param `p` (its default's position through the declared curve when it
// stores none), the curve it plays it on (doc::Device::curves, else the declared one) and the value in units.
double storedPosition(const ParamInfo& p, const doc::Device& d) noexcept;
const dsp::Curve& storedCurve(const ParamInfo& p, const doc::Device& d) noexcept;
double storedValue(const ParamInfo& p, const doc::Device& d) noexcept;
// The value shown (dB for a gain, % for a fraction, Hz).
double toDisplay(const ParamInfo& p, double value) noexcept;
double fromDisplay(const ParamInfo& p, double display) noexcept;
// "1.00 kHz", "120 Hz", "-6.0 dB", "-inf dB" (a gain at a −70 dB or lower minimum), "50 %", "0.71", "On"/"Off".
std::string formatValue(const ParamInfo& p, double value);
// Typed input in display units, with an optional unit ("1k", "1.5 kHz", "-3", "-3 dB", "50", "50%", "-inf").
std::optional<double> parseValue(const ParamInfo& p, std::string_view text);

}  // namespace pq::views::devices
