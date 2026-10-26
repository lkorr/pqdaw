#pragma once

// T4.5: drawing the probe readout and the cost labels over the circuit canvas (probes.cpp). Private to views.

#include <string>

#include "pq/ui/widget.h"
#include "pq/views/circuit/probes.h"

namespace pq::views::circuit {

// The hovered probe's readout (value + mini-scope) next to `anchor` (device px), kept inside `clip`.
void paintProbeReadout(ui::PaintContext& ctx, const ProbeOverlay& o, ui::Vec2 anchor, const ui::IRect& clip);
// One cost label centred on `at` (device px), above or below it; heat 0..1 tints it.
void paintCostLabel(ui::PaintContext& ctx, ui::Vec2 at, const std::string& text, float heat, bool above);

}  // namespace pq::views::circuit
