#pragma once

// Device panels (T4.6, SPEC §7.3): a device is a patch; its front panel's controls are the patch's `param` pins
// (written at run time, no recompile) and its structural settings, the `in … : real const` inputs (a change recompiles
// and crossfades, SPEC §7.5; QUESTIONS Q121). This header reads both from the patch and maps their values to what a
// control shows: knob position (0..1), display text, typed values.
//
// Values are always in the patch's internal units (PATCH_LANGUAGE §1: a dB param holds a linear gain, % a fraction,
// Hz Hz); display units come from @unit(…) or the default's unit suffix.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/patch.h"

namespace pq::views::devices {

enum class DisplayUnit : uint8_t { None, Hz, Db, Percent, Seconds };

// One `param` of the device patch.
struct ParamInfo {
  std::string name;
  std::string label;        // @label("…"), else the name
  DisplayUnit unit = DisplayUnit::None;
  double min = 0.0, max = 1.0, def = 0.0;  // internal units
  bool exp = false;         // @curve(exp): knob position is logarithmic in the value
  int32_t steps = 0;        // @steps(n): n integer positions (0 = continuous)
  bool isBool = false;
  bool hidden = false;      // @hidden
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
// Knob / slider position in [0, 1]: linear in the display value (dB for gains), logarithmic for @curve(exp).
double toPosition(const ParamInfo& p, double value) noexcept;
double fromPosition(const ParamInfo& p, double position) noexcept;
// The value shown (dB for a gain, % for a fraction, Hz).
double toDisplay(const ParamInfo& p, double value) noexcept;
double fromDisplay(const ParamInfo& p, double display) noexcept;
// "1.00 kHz", "120 Hz", "-6.0 dB", "-inf dB" (a gain at a −70 dB or lower minimum), "50 %", "0.71", "On"/"Off".
std::string formatValue(const ParamInfo& p, double value);
// Typed input in display units, with an optional unit ("1k", "1.5 kHz", "-3", "-3 dB", "50", "50%", "-inf").
std::optional<double> parseValue(const ParamInfo& p, std::string_view text);

}  // namespace pq::views::devices
