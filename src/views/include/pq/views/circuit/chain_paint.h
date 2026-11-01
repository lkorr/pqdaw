#pragma once

// Painting a chain circuit's boundaries (pq/views/circuit/chain_circuit.h) on a circuit canvas. T5.x-circuit-chain drew
// a whole track's chain this way in a separate Track-level view; T5.x-one-circuit merged that view into the project
// patch view (SPEC §15 2026-10-31: the whole project is one circuit), which draws an opened rack's circuit inside the
// rack's frame with these.
//
// CPU: one pass over the boxes (a few rects and a title each; boxes outside the clip skipped). UI thread.

#include "pq/ui/widget.h"
#include "pq/views/circuit/chain_circuit.h"
#include "pq/views/circuit/circuit_canvas.h"

namespace pq::views::circuit {

// Paints `c`'s boundaries on `canvas`, translated by (dx, dy) world units: in track mode the track's band (its colour,
// its name), then every box, parents first (fill, outline and title strip in the kind's accent; the title at every
// zoom where the box is wide enough to read it). Called from a canvas's paintUnderlay.
void paintChainBoxes(ui::PaintContext& ctx, const CircuitCanvas& canvas, const ChainCircuit& c, float dx = 0.0f, float dy = 0.0f);
// Over the scene: deactivated devices / chains veiled, "off" in their title strips.
void paintChainVeils(ui::PaintContext& ctx, const CircuitCanvas& canvas, const ChainCircuit& c, float dx = 0.0f, float dy = 0.0f);

}  // namespace pq::views::circuit
