#pragma once

// Circuit-diagram layout (T4.3, SPEC §7.3 "Layout is stored, with an auto-route and tidy command"): automatic layered
// placement and Manhattan routing of a patch, written into the patch's layout (NodeLayout per node, routing points per
// wire) — the `layout { … }` block of the .pqp text, which the compiler ignores and the printer keeps.
//
// Coordinates are world units (logical px of the circuit canvas at zoom 1). A node's position is its top-left corner;
// its input pins sit on its left edge (x = 0), its output pins on its right edge (x = w), at the y offsets the caller's
// geometry gives (the symbol library, views/circuit/symbols.h; defaultGeometry() for tools and tests). Signal flows
// left to right.
//
// autoLayout ("tidy"): a layered (Sugiyama) drawing.
//   1. Feedback: edges closing a cycle are reversed for layering (which ones: below).
//   2. Layers: longest path from the sources, then as late as possible (each node right before its earliest consumer:
//      constants and params next to what they feed); `in` declarations stay in layer 0, `out`/`export` in the last.
//   3. Long edges get a chain of dummy nodes, one per layer crossed; the fan-out of one output pin shares one chain
//      (a trunk with branches, as a schematic draws it). A reversed edge runs right to left through dummies from the
//      source's layer back to the target's.
//   4. Crossing minimisation: barycentric ordering, alternating down / up sweeps (pin-accurate positions), keeping the
//      order with the fewest crossings (counted exactly between adjacent layers).
//   5. Vertical placement: per layer, isotonic regression (pool adjacent violators) of the positions that would make
//      each node's wires straight, under the layer's order and minimum gaps; alternating sweeps.
//   6. Routing: every wire leaves its pin horizontally, turns once in the channel between two layers, and enters the
//      next layer horizontally. Each channel's vertical segments are grouped per net and given tracks: the order of
//      overlapping segments minimises stub crossings (pairwise cost, sorted then improved by adjacent swaps), packing
//      keeps that order. Channel widths follow their track counts; layer columns are as wide as their widest node.
//   Deterministic; O((nodes + wires) · layers-crossed) plus the per-channel ordering. Not RT-safe (UI thread).
//
// routeUnrouted: wires without routing points (a hand-placed layout, a new wire) get a simple Manhattan route: none
// when the pins line up left to right, a Z through the middle of the gap, or a detour below both nodes when the target
// is left of the source. No obstacle avoidance (a tidy gives the full router's result).
//
// Feedback (step 1 above): for each delay inside a strongly connected component (an atom with a delayed input — z1.x,
// delay.x, phasor.hz — wired from its own loop), either its loop inputs or its loop outputs are reversed, whichever are
// fewer (ties: the outputs): y = z1(x + y·k) draws x → + → z⁻¹ with the ·k path running back, and the SVF's integrators,
// each read three times inside the loop, sit at the start of their loops with one wire running back into them. Cycles
// without a delay are cut by a DFS from the sources. In vertical placement, links between two dummies weigh 4× (long
// wires straight).

#include <cstdint>
#include <span>
#include <vector>

#include "pq/graph/patch.h"
#include "pq/graph/patch_file.h"

namespace pq::graph {

struct NodeShape {
  float w = 0.0f, h = 0.0f;
};

// Size and pin offsets of every node of a patch (parallel to its nodes and ports).
struct LayoutGeometry {
  std::vector<NodeShape> shape;  // [node]
  std::vector<float> inPinY;     // [flat input port]: y offset of the pin from the node's top edge (x = 0)
  std::vector<float> outPinY;    // [flat output port]: y offset (x = w); indexed by Patch::flatOutput too
  float inPin(const Patch& p, NodeIndex n, uint32_t i) const noexcept { return inPinY[p.flatInput(n, i)]; }
  float outPin(const Patch& p, NodeIndex n, uint32_t i) const noexcept { return outPinY[p.flatOutput(n, i)]; }
};

// Plain boxes: 48 wide, 16 per pin row (+ 8 margin), pins centred in their rows. For tools and graph tests; the canvas
// uses its symbol library's geometry.
LayoutGeometry defaultGeometry(const Patch& p);

struct AutoLayoutOptions {
  float layerGap = 24.0f;     // minimum horizontal space on each side of a channel's tracks
  float nodeGap = 16.0f;      // minimum vertical space between two nodes of a layer
  float dummyGap = 8.0f;      // minimum vertical space next to a wire passing through a layer
  float trackSpacing = 8.0f;  // between two vertical wire tracks of a channel
  uint32_t orderSweeps = 12;  // crossing-minimisation sweeps (down + up = 2)
  uint32_t placeSweeps = 8;   // vertical-placement sweeps
  float grid = 4.0f;          // node positions are multiples of it (pins keep their offsets)
};

struct AutoLayoutStats {
  uint32_t layers = 0;
  uint32_t dummies = 0;
  uint32_t reversed = 0;           // feedback edges
  uint64_t crossingsInitial = 0;   // between adjacent layers, before ordering
  uint64_t crossings = 0;          // after ordering
  uint32_t tracks = 0;             // vertical tracks over all channels
  float width = 0.0f, height = 0.0f;
};

// Places every node (NodeLayout: placed, rotation 0, no flip) and routes every wire (its routing points) of `p`.
// Comments are kept. `geometry` must match `p` (sizes of its nodes, offsets of its ports).
AutoLayoutStats autoLayout(Patch& p, const LayoutGeometry& geometry, const AutoLayoutOptions& options = {});

// Every node placed?
bool fullyPlaced(const Patch& p) noexcept;

// Gives unplaced nodes the positions an autoLayout of the whole patch would give them, moved down until they overlap no
// placed node (the placed ones keep theirs), then routes the wires that have no routing points (routeUnrouted). A patch
// with no placed node gets autoLayout. Returns the number of nodes it placed.
uint32_t completeLayout(Patch& p, const LayoutGeometry& geometry, const AutoLayoutOptions& options = {});

// Routes every wire without routing points (see above). Returns how many it routed.
uint32_t routeUnrouted(Patch& p, const LayoutGeometry& geometry, float stub = 12.0f);

// Carries layout across a text edit (T4.3 for T4.4): every node of `next` without a position takes the position of the
// node of `previous` with the same NodeId (inheritNodeIds pairs them), and every wire without routing points whose two
// ends map to a wire of `previous` takes its points when both end nodes kept their positions. Patches pair by name.
// Returns the number of nodes placed this way.
uint32_t carryLayout(PatchFile& next, const PatchFile& previous);
uint32_t carryLayout(Patch& next, const Patch& previous);

}  // namespace pq::graph
