#pragma once

// Keeping NodeIds across text edits (T1.x-text-ids, QUESTIONS Q68).
//
// Hot-swap state is keyed by the NodeId call path (TypedPatch::stateKey), and the parser hands out ids in text
// order (declarations first), so re-parsing an edited `.pqp` would renumber nodes and every node would restart.
// inheritNodeIds() is a structural diff of the re-parsed patch against the previous one: it pairs nodes that
// stand for the same thing and gives each paired node of `next` the id of its partner in `previous`; unpaired
// nodes get ids above every id `previous` ever handed out (never an id an older node had). Dense order is not
// changed (it is text order, which signatures and output order depend on), so ParseResult::nodeSpans stay valid.
// The parser runs it when ParseOptions::previous is set.
//
// Pairing, in this order (each step only pairs nodes still unpaired; every pair has the same NodeKind, and
// subcircuit nodes the same callee):
//   1. names: declarations (param / in / out / export / unknown) by kind + name, named bindings by name,
//      equations by position in their solve block;
//   2. structure, from every pair outwards (strong: the same label — atom, literal value, callee, port count):
//      down through the inputs (input i's driver to input i's driver) and up through the outputs (a consumer edge
//      whose label and input port are unique among the unpaired consumers on both sides);
//   3. structural hashes: nodes whose hash over their inputs (4 rounds, paired nodes hashed as their pair) is
//      unique among the unpaired nodes of both patches; equal-hash leftovers (symmetric duplicates) in order;
//   4. weak structure: as 2, but labels may differ (an atom changed, a constant edited, a subcircuit's ports
//      changed): ports then correspond by name. Such a node keeps its old id; the transfer plan's tag check
//      decides whether its state carries over.
// A pair is never made between two named nodes whose names differ while either name still exists on the other
// side (a swap of two bindings is not a rename); a rename is a name that disappeared paired with one that
// appeared. Consequence: two bindings that swap names keep their state by name.
//
// Cost: O((nodes + wires) · log) with a few flat arrays per patch; not RT-safe (compiler / UI threads).

#include <cstdint>

#include "pq/graph/patch_file.h"

namespace pq::graph {

struct IdMatchStats {
  uint32_t nodes = 0;          // nodes of `next`
  uint32_t previousNodes = 0;  // nodes of `previous`
  uint32_t matched = 0;        // nodes of `next` that took an id from `previous`
  uint32_t byName = 0;         // … of them in step 1
  uint32_t byStructure = 0;    // step 2
  uint32_t byHash = 0;         // step 3
  uint32_t weak = 0;           // step 4 (the label differs)
  uint32_t fresh() const noexcept { return nodes - matched; }
  IdMatchStats& operator+=(const IdMatchStats& o) noexcept {
    nodes += o.nodes;
    previousNodes += o.previousNodes;
    matched += o.matched;
    byName += o.byName;
    byStructure += o.byStructure;
    byHash += o.byHash;
    weak += o.weak;
    return *this;
  }
};

// Gives `next` the NodeIds of the corresponding nodes of `previous` (see above).
IdMatchStats inheritNodeIds(Patch& next, const Patch& previous);
// Per patch, paired by patch name; patches without a namesake in `previous` keep their ids.
IdMatchStats inheritNodeIds(PatchFile& next, const PatchFile& previous);

}  // namespace pq::graph
