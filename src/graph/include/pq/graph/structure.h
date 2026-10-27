#pragma once

// Structural constants of a patch AS WRITTEN (T4.15, SPEC §15 2026-10-26 "circuit-view legibility"): which values
// of one patch are compile-time constants for a given set of `real const` / `bool const` input arguments, and which
// nodes a structural choice switches off. The circuit editor hides those nodes (a view decision: nothing here changes
// the patch, its text or what is compiled).
//
// Constant values follow the optimiser's rules (fold.h, shared with optimise.cpp): literals; a const-rate `in` takes
// its argument (else its default expression); a pure kernel atom whose inputs are all constant is evaluated by its own
// dsp kernel (an unconnected optional input reads its registry default); select(c, a, b) / mux(i, …) with a constant
// selector take the picked input's value. Params are never constant (they are knobs, never folded), nor are stateful
// atoms, subcircuit outputs, solve unknowns or anything on a delay-free cycle. Bools are lane masks while evaluated
// (dsp::boolMask), reported as 0 / 1. eq / ne on bool operands are not evaluated (infer() rewrites them; unknown is
// the conservative answer here: nothing is hidden because of them).
//
// Inactive nodes: a wire into an arm of a select / mux that its constant selector does not pick is DEAD. A node is
// LIVE when a path of non-dead wires leads from it to a sink — a node with no outgoing wire: an `out`, an `export`, a
// side-effect atom, a solve equation, or a node nothing reads yet (one being wired up in the editor). A node is
// INACTIVE when it is not live but feeds a dead wire (directly or through other nodes): exactly what is reachable
// only through switched-off branches. A node that is neither (an isolated feedback loop) stays active. An inactive
// wire is a dead wire or a wire into an inactive node. The optimiser removes the same nodes (select / mux
// simplification, then dead-node elimination) in the compiled program.
//
// Cost: linear in nodes + wires (one topological evaluation, two graph walks); evaluating an atom runs its kernel for
// one frame. UI / compiler thread; not RT-safe.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pq/graph/patch.h"

namespace pq::graph {

// A structural argument of a patch: the value of one of its const-rate `in`s (by name). Bool inputs: 0 / 1.
struct ConstArg {
  std::string name;
  double value = 0.0;
  friend bool operator==(const ConstArg&, const ConstArg&) = default;
};

struct StructuralInfo {
  std::vector<uint8_t> known;       // [flat output port] 1: the value is a compile-time constant
  std::vector<double> value;        // [flat output port] that constant (bool: 0 / 1)
  std::vector<uint8_t> deadWire;    // [wire] 1: into an arm its constant selector does not pick
  std::vector<uint8_t> nodeInactive;  // [node] 1: switched off (see above)
  std::vector<uint8_t> wireInactive;  // [wire] 1: a dead wire, or into an inactive node
  uint32_t inactiveNodes = 0, inactiveWires = 0;
  // The constant at output `port` of node n, if it is one.
  bool constantAt(const Patch& p, NodeIndex n, uint32_t port, double& out) const noexcept;
};

// The analysis of `p` with these arguments (unnamed or non-const inputs in `args` are ignored).
StructuralInfo analyseStructure(const Patch& p, std::span<const ConstArg> args = {});

// The arguments a subcircuit call node `call` of `p` passes to its callee: every const-rate input of the callee's
// signature (SubcircuitRef::inputs) whose pin carries a constant under `info`. Unconnected pins are left out (the
// callee then reads its own default).
std::vector<ConstArg> callArguments(const Patch& p, const StructuralInfo& info, NodeIndex call);

}  // namespace pq::graph
