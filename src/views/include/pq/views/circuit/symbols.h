#pragma once

// Schematic symbol library of the circuit-diagram view (T4.2, SPEC §7.3): which symbol a patch node is drawn as, its
// size and pins (world units: logical px at zoom 1, multiples of 4 so auto-layout puts wires on whole units), and how
// its body is painted.
//
//   logic gates     and / or / xor / nand / nor / not in the standard (ANSI) shapes
//   op-amp triangle comparators (lt le gt ge eq ne, schmitt) and the summing amp (add, sub)
//   circles         multiplier ×, divider ÷; oscillators with their waveform (phasor saw, sine, wavetable, noise)
//   boxes           z⁻¹ and delay lines labelled with the delay; filters with a response glyph (the stdlib filter
//                   subcircuits); functions with their name
//   trapezoid       select, mux, demux
//   IC rectangle    flip-flops, counter, clockdiv, timer, pulse, edge detectors, sample & hold, latch: labelled pins
//   terminals       const (the literal), param (its name), voice.* / transport.* / sr sources, in / out / export flags
//   block           subcircuit instances: the callee's name, labelled pins
//
// Pins: inputs on the left edge (x = 0), outputs on the right edge (x = w), like graph::LayoutGeometry. Each pin has a
// stub length: the canvas draws the pin stub (from the edge to the body) with the wire layer, crisp and snapped like
// the wires, and the symbol body (painted here) starts where the stubs end. Unconnected optional inputs (they read
// their default) have no pin drawn.
//
// Bodies are painted in two layers, each one colour: Fill (the body's fill) and Stroke (outline + glyph). The canvas
// rasterises both layers of a symbol once per zoom level into coverage tiles (pq/ui/tile_atlas.h) and then draws each
// symbol as two quads; the same functions paint directly when there is no tile. UI thread only.

#include <cstdint>
#include <string>
#include <vector>

#include "pq/graph/layout.h"
#include "pq/graph/patch.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/theme.h"

namespace pq::views::circuit {

enum class Symbol : uint8_t {
  Box, And, Or, Xor, Nand, Nor, Not, Comparator, Summer, Multiplier, Divider, Delay, Filter, Oscillator, Mux, Ic,
  Const, Param, Source, Input, Output, Export, Subcircuit, Unknown, Equation,
};
inline constexpr uint32_t kSymbolCount = static_cast<uint32_t>(Symbol::Equation) + 1;

enum class Glyph : uint8_t {
  None,
  // oscillators
  Saw, Sine, Table, Noise,
  // filter responses
  Lowpass, Highpass, Bandpass, Notch, Allpass, Shelf, Peak, Comb,
  // comparators
  Lt, Le, Gt, Ge, Eq, Ne, Hysteresis,
  // summer
  Plus, Minus,
  // delays: one sample (z⁻¹) or a delay line (Δ)
  UnitDelay, DelayLine,
};

// The body layer a paint call draws.
enum class Layer : uint8_t { Fill, Stroke };

struct PinSpec {
  float y = 0.0f;     // offset from the top edge
  float stub = 0.0f;  // length from the edge to the body
  bool visible = true;
  std::string label;  // shown inside labelled symbols (IC, subcircuit, filter); empty: none
};

// How one node is drawn.
struct SymbolSpec {
  Symbol symbol = Symbol::Box;
  Glyph glyph = Glyph::None;
  float w = 40.0f, h = 32.0f;
  std::vector<PinSpec> inputs, outputs;  // one per port of the node (hidden ones keep a y for layout)
  std::string title;    // text inside the body (function name, value, callee, IC name); "" for pure symbols
  std::string caption;  // the node's binding name, shown above the body ("" when anonymous)
  // Identifies the body's pixels for tile caching: equal keys paint identical bodies at equal zoom.
  uint64_t bodyKey = 0;
};

// The symbol for node n of p. `wired[i]` tells whether input i is connected (unconnected optional inputs get no pin).
SymbolSpec describe(const graph::Patch& p, graph::NodeIndex n, const std::vector<uint8_t>& wiredInputs);

// describe() for every node, as the geometry the auto-layout and router use (graph/layout.h).
std::vector<SymbolSpec> describeAll(const graph::Patch& p);
graph::LayoutGeometry geometryOf(const graph::Patch& p, const std::vector<SymbolSpec>& specs);

// Body fill colour of a symbol (theme group circuit.*).
ui::Rgba fillColour(const ui::Theme& theme, Symbol s) noexcept;

// Paints the body layer of `spec` with its top-left at `origin` (device px, whole pixels for tile == direct), `scale`
// device px per world unit, in colour `col`. Strokes are max(0.75, 1.2·scale) px wide, capped at 3.
void paintBody(ui::DrawList& dl, const SymbolSpec& spec, ui::Vec2 origin, float scale, Layer layer, ui::Rgba col);

// The stroke width paintBody uses at `scale` (device px).
float strokeWidth(float scale) noexcept;

}  // namespace pq::views::circuit
