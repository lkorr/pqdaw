#pragma once

// TX.knob-curves (SPEC §15 2026-11-01 "answers to Q186"): the "Knob Curve" submenu every continuous knob's context menu
// carries — Device view panels and faces, circuit panels, meta faces' sliders (doc::KnobCurve; doc::ops::setParamCurve).
// UI thread only.

#include <optional>
#include <string>

#include "pq/document/model.h"
#include "pq/ui/overlay.h"

namespace pq::views {

class ViewContext;

// "Knob Curve ▸ Octave / Exponential / Logarithmic / Linear" for `param` of `device` (the one it is on checked, the
// declared one marked "(default)"; choosing the declared one stores Default), or nullopt when the knob has no curve to
// choose (a rack's macro, a bool or stepped param, an input, a param the patch does not declare, a unitsInBody knob such
// as the VCA's, whose curve is its own curve box).
std::optional<ui::MenuItem> knobCurveMenu(ViewContext& ctx, doc::DeviceId device, const std::string& param);

}  // namespace pq::views
