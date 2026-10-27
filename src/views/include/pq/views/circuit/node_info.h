#pragma once

// What the circuit editor says about a node when the pointer rests on it (T4.15, SPEC §15 2026-10-26 "circuit-view
// legibility"): the tooltip text, built on demand (once per hover, never per frame), and the recognised forms of
// constants.
//
// Tooltip lines (one per '\n'; the overlay draws them in the tooltip look, pq/ui/overlay.h):
//   1  name and kind        "medium = fast_tanh · atom (math)", "sat_shape · subcircuit", "Drive (drive) · param",
//                           "type · in (real const)", "0.14814814814814814 · constant"
//   2  description          atoms: AtomSignature::summary (the table next to the signatures, pq/graph/atoms.h);
//                           subcircuits: the first sentence of the comment above the callee's `patch` line;
//                           params: unit, range and the current value (the instance's, else the default);
//                           constants: the recognised form ("= 4/27"); ins: the structural value
//   3  the comment after the node's declaration in the text, when it has one
//   4  inferred kind / rate / poly of its output (the canvas's wire-style inference: real · audio rate · per voice)
//   5  "Inactive: …" when a structural constant switches the node off (it is drawn only with Show Inactive)
//
// Recognised forms (recognisedForm): a value matches an entry when it is within one float ulp (relative 2^-23) of it;
// values that already read clearly as decimals (at most 6 significant digits: 0.5, 0.25, 0.001) get none. The entries,
// in the order they are tried: p/q (1 ≤ p ≤ 12, 2 ≤ q ≤ 32, reduced), multiples and fractions of π and 1/π
// (pπ/q, p/(qπ)), √2, 1/√2, √3, 1/√3, √3/2, 2^(1/12), 2^(−1/12), e, 1/e, ln 2, 1/ln 2, ln 10, 1/ln 10, log₁₀ 2,
// 20/ln 10 (dB per neper), ln 10/20, and the gains of whole dB from −120 to +48 ("−3 dB"). Negative values get "−".
// UI thread only.

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/graph/parse.h"
#include "pq/graph/patch.h"
#include "pq/graph/structure.h"
#include "pq/views/circuit/circuit_scene.h"

namespace pq::views::circuit {

// The recognised form of `v` ("4/27", "1/(2π)", "π/2", "1/√2", "ln 2", "−6 dB"), or "" when there is none.
std::string recognisedForm(double v);

// The description of a patch definition: the first sentence of the comment block directly above its `patch` keyword
// at `patchBegin` in `source` (for a file's first patch with nothing directly above, the file's leading comment block),
// word-wrapped at about 96 characters. "" when there is no comment.
std::string patchDescription(std::string_view source, uint32_t patchBegin);

struct TooltipContext {
  const graph::Patch* patch = nullptr;
  const graph::StructuralInfo* structure = nullptr;   // the shown instance's structural constants (optional)
  std::span<const graph::ConstArg> paramValues;       // the instance's param values by name (a device's), optional
  std::string_view source;                            // the patch's text and its node spans (trailing comments)
  std::span<const graph::SourceSpan> spans;
  std::span<const WireStyle> outputStyles;            // per flat output port (circuit_scene.h outputStyles), optional
  std::function<std::string(std::string_view callee)> describeCallee;  // a callee's patchDescription, optional
};

// The tooltip of node n (see above).
std::string nodeTooltip(const TooltipContext& c, graph::NodeIndex n);

}  // namespace pq::views::circuit
