#pragma once

// GU.1 (docs/plans/GU.md "Curves"): the model part of schema 19 → 20 (serialize.cpp migrateKnobPositions marks the
// file): every param a file stored as a VALUE becomes the KNOB POSITION that gives it, through the param's curve —
// TX.knob-curves' named curve when the file switched the knob (that curve is then stored as a curve object), else the
// declared one — by the nearest-position rule (dsp::curvePosition): a device's params (a rack's macros and selector
// keep their 0 … 127 values), its nested meta modules' params, and the breakpoints of device-param automation lanes.
// Macro mappings and wire amounts already are positions / depths. A device whose patch the library does not know keeps
// its values (it is bypassed until it is found). Not RT code.

#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/model.h"

namespace pq::doc {

class DeviceLibrary;

// A schema 16 … 19 "curves" entry (serialize.cpp kKnobCurveNames: 1 linear, 2 octave, 3 exponential, 4 logarithmic).
struct LegacyCurve {
  DeviceId device;
  std::string param;
  uint8_t curve = 0;
};

// The params of `d` (and of the devices inside it) a conversion would touch, as "<device id>:<param>" (nested meta
// params "<device id>:<path>.<param>"); empty: none.
std::vector<std::string> knobPositionsToConvert(const Device& d);
// Converts `p` in place. `lib` null: only a project with nothing to convert loads (else Unsupported, naming why).
Status migrateToKnobPositions(Project& p, const DeviceLibrary* lib, const std::vector<LegacyCurve>& legacy);

}  // namespace pq::doc
