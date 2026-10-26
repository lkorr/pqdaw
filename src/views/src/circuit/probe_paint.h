#pragma once

// T4.5: drawing the probe readout and the cost labels over the circuit canvas (probes.cpp). Private to views.

#include <span>
#include <string>

#include "pq/ui/widget.h"
#include "pq/views/circuit/probes.h"

namespace pq::views::circuit {

// The hovered probe's readout (value + mini-scope) next to `anchor` (device px), kept inside `clip`.
void paintProbeReadout(ui::PaintContext& ctx, const ProbeOverlay& o, ui::Vec2 anchor, const ui::IRect& clip);
// Cost labels, each centred on `at` (device px), above or below it; heat 0..1 tints it. The costliest go first, and a
// label that would overlap one already drawn is left out.
struct CostLabelAt {
  ui::Vec2 at;
  const std::string* text = nullptr;
  float heat = 0.0f;
  bool above = true;
};
void paintCostLabels(ui::PaintContext& ctx, std::span<const CostLabelAt> labels);

}  // namespace pq::views::circuit
