#pragma once

// The patch IR (T1.1): one `patch` of the patch language (docs/PATCH_LANGUAGE.md) as a dataflow graph.
//
// Representation (flat arrays, dense indices; built for fast whole-graph passes in the compiler):
// - Nodes live in one vector, addressed by a dense NodeIndex. Each node also has a stable NodeId that survives
//   edits (hot-swap state is keyed by it, SPEC §7.5). Nodes are always sorted by NodeId (ids are handed out in
//   increasing order and erase keeps relative order), so indexOf(NodeId) is a binary search.
// - Every node's ports are one contiguous range of a flat PortType array: inputs first, then outputs. A port is
//   addressed by PortRef{node, port-within-direction}; flatInput()/flatOutput() give the flat index that per-port
//   side tables (inference results, buffer assignments) are indexed by.
// - Wires are a vector of (output PortRef → input PortRef). Adjacency is built on demand (adjacency()), as CSR.
// - Cold data (names, layout, wire routing points, literals, param declarations, subcircuit references) lives in
//   parallel or side vectors so whole-graph passes only touch the hot Node/PortType/Wire arrays.
//
// Everything in the language is a node, so wires are the only edges:
//   Atom        a leaf atom or operator (atoms.h)          Const     a literal (one output)
//   Param       `param` (one output, param rate)          Input     `in` (output = the value; optional input
//   Output      `out` (one input)                                    "default" = value when unconnected)
//   Export      `export` (one input)                      Subcircuit  an instance of another patch
//   Unknown     `unknown` in a solve block (optional input "init" = initial guess; output = the solution)
//   Equation    `lhs == rhs` in a solve block (inputs lhs, rhs)
//
// Names: declarations (param/in/out/export/unknown) must be named; other nodes may be named (a local binding
// `x = expr` names the node that computes expr). `y = svf(x).lp` names the svf node with nameOutput = lp, so `y`
// refers to that output and layout `node y` places the svf node. Pure aliases (`y = x`) are resolved by the parser
// and have no IR form. Text can only share a value through a name, so a parsed patch never has an anonymous node
// with more than one consumer; the printer (T1.2) relies on that.
//
// Construction never fails and does no checking, so tests and editors can build broken graphs; validate() reports
// every structural and declared-type problem. Not RT-safe: all of this runs on the UI/compiler threads.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/atoms.h"
#include "pq/graph/types.h"

namespace pq::graph {

enum class NodeKind : uint8_t { Atom, Const, Param, Input, Output, Export, Subcircuit, Unknown, Equation };
std::string_view toText(NodeKind) noexcept;

inline constexpr uint16_t kWholeNode = 0xFFFF;  // Node::nameOutput when the name refers to the node itself
inline constexpr uint32_t kMaxSolveIterations = 64;

struct Node {
  NodeId id;
  NodeKind kind = NodeKind::Atom;
  AtomId atom = 0;              // Atom: index into the atom registry
  uint32_t firstPort = 0;       // flat index of input 0; outputs follow the inputs
  uint16_t numInputs = 0;
  uint16_t numOutputs = 0;
  uint32_t payload = kNone;     // Const/Param/Subcircuit: side-table index; Unknown/Equation: SolveIndex
  uint16_t nameOutput = kWholeNode;
};

struct PortRef {
  NodeIndex node = kNone;
  uint16_t port = 0;  // index among the node's inputs (wire target) or outputs (wire source)
  friend bool operator==(const PortRef&, const PortRef&) = default;
};

struct Wire {
  PortRef from;  // an output
  PortRef to;    // an input
  friend bool operator==(const Wire&, const Wire&) = default;
};

// A `solve(iterations: n) { … }` block. Its unknowns and equations are nodes whose payload is the block's index.
struct Solve {
  uint32_t iterations = 1;
  friend bool operator==(const Solve&, const Solve&) = default;
};

struct SubcircuitPort {
  std::string name;
  PortType type;
  bool optional = false;  // has a default in the callee (an `in` with `= expr`, or any `param`)
  friend bool operator==(const SubcircuitPort&, const SubcircuitPort&) = default;
};

// A node that instantiates another patch (a call `Name(args)`, PATCH_LANGUAGE §3). The ports are a copy of the
// callee's signature at instantiation (subcircuitSignature() in patch_file.h): inputs = the callee's `in`s then its
// `param`s, outputs = its `out`s then its `export`s. Keeping the copy makes a patch checkable on its own;
// PatchFile::validate() checks the copy against the callee when the callee is in the same file.
struct SubcircuitRef {
  std::string patch;
  std::vector<SubcircuitPort> inputs;
  std::vector<SubcircuitPort> outputs;
  friend bool operator==(const SubcircuitRef&, const SubcircuitRef&) = default;
};

// Wires by endpoint, built on demand (CSR). driver[] is indexed by flat port index and is kNone for unconnected
// inputs (and for output ports). Out-of-range wires are skipped; with several drivers on one input, the first wins
// (validate() reports the rest).
struct Adjacency {
  std::vector<WireIndex> driver;
  std::vector<uint32_t> outBegin;  // nodeCount + 1 offsets into outWires
  std::vector<WireIndex> outWires; // grouped by source node, in wire order within a node
  std::span<const WireIndex> outgoing(NodeIndex n) const noexcept {
    return {outWires.data() + outBegin[n], outWires.data() + outBegin[n + 1]};
  }
};

class Patch {
public:
  Patch() = default;
  explicit Patch(std::string name) : name_(std::move(name)) {}

  const std::string& name() const noexcept { return name_; }
  void setName(std::string name) { name_ = std::move(name); }

  // ---- construction (no checks; see validate) ----------------------------------------------------------------
  // `repeat` = number of instances of a variadic atom's repeated port (0 = the signature's minimum).
  NodeIndex addAtom(AtomId atom, uint16_t repeat = 0);
  NodeIndex addConst(Constant c);
  NodeIndex addParam(std::string name, ParamDecl decl);
  NodeIndex addInput(std::string name, PortType type);
  NodeIndex addOutput(std::string name, PortType type);
  NodeIndex addExport(std::string name);
  NodeIndex addSubcircuit(SubcircuitRef ref);
  SolveIndex addSolve(uint32_t iterations);
  NodeIndex addUnknown(std::string name, SolveIndex solve);
  NodeIndex addEquation(SolveIndex solve);

  void setNodeName(NodeIndex n, std::string name, uint16_t nameOutput = kWholeNode);
  void setLayout(NodeIndex n, const NodeLayout& layout) { layout_[n] = layout; }
  ParamDecl& paramDecl(NodeIndex n) { return params_[nodes_[n].payload]; }
  Constant& constant(NodeIndex n) { return constants_[nodes_[n].payload]; }
  Solve& solve(SolveIndex s) { return solves_[s]; }

  WireIndex connect(PortRef from, PortRef to);
  // Connect output 0 of `from` to input `input` of `to` (the common case).
  WireIndex connect(NodeIndex from, NodeIndex to, uint16_t input) { return connect(PortRef{from, 0}, PortRef{to, input}); }
  void setWireVia(WireIndex w, std::vector<Point2> via) { wireVia_[w] = std::move(via); }
  // Removes wire w; the last wire moves into index w (wire order carries no meaning).
  void disconnect(WireIndex w);
  // Removes the nodes and every wire touching them. Remaining nodes keep their NodeIds and relative order; dense
  // indices are compacted. O(nodes + wires). Out-of-range or duplicate indices are ignored.
  void eraseNodes(std::span<const NodeIndex> nodes);
  void addComment(LayoutComment c) { comments_.push_back(std::move(c)); }

  // ---- access -------------------------------------------------------------------------------------------------
  uint32_t nodeCount() const noexcept { return static_cast<uint32_t>(nodes_.size()); }
  uint32_t wireCount() const noexcept { return static_cast<uint32_t>(wires_.size()); }
  uint32_t portCount() const noexcept { return static_cast<uint32_t>(ports_.size()); }
  uint32_t solveCount() const noexcept { return static_cast<uint32_t>(solves_.size()); }

  const Node& node(NodeIndex n) const noexcept { return nodes_[n]; }
  std::span<const Node> nodes() const noexcept { return nodes_; }
  const Wire& wire(WireIndex w) const noexcept { return wires_[w]; }
  std::span<const Wire> wires() const noexcept { return wires_; }
  std::span<const PortType> portTypes() const noexcept { return ports_; }
  std::span<const Solve> solves() const noexcept { return solves_; }
  std::span<const LayoutComment> comments() const noexcept { return comments_; }

  uint32_t flatInput(NodeIndex n, uint32_t i) const noexcept { return nodes_[n].firstPort + i; }
  uint32_t flatOutput(NodeIndex n, uint32_t i) const noexcept { return nodes_[n].firstPort + nodes_[n].numInputs + i; }
  const PortType& inputType(NodeIndex n, uint32_t i) const noexcept { return ports_[flatInput(n, i)]; }
  const PortType& outputType(NodeIndex n, uint32_t i) const noexcept { return ports_[flatOutput(n, i)]; }
  std::string_view inputName(NodeIndex n, uint32_t i) const noexcept;
  std::string_view outputName(NodeIndex n, uint32_t i) const noexcept;
  // True if input i may stay unconnected (it then reads a default).
  bool inputOptional(NodeIndex n, uint32_t i) const noexcept;
  std::optional<uint32_t> findInput(NodeIndex n, std::string_view name) const noexcept;
  std::optional<uint32_t> findOutput(NodeIndex n, std::string_view name) const noexcept;

  const std::string& nodeName(NodeIndex n) const noexcept { return names_[n]; }
  const NodeLayout& layout(NodeIndex n) const noexcept { return layout_[n]; }
  std::span<const Point2> wireVia(WireIndex w) const noexcept { return wireVia_[w]; }
  const Constant& constant(NodeIndex n) const noexcept { return constants_[nodes_[n].payload]; }
  const ParamDecl& paramDecl(NodeIndex n) const noexcept { return params_[nodes_[n].payload]; }
  const SubcircuitRef& subcircuit(NodeIndex n) const noexcept { return subcircuits_[nodes_[n].payload]; }

  std::optional<NodeIndex> indexOf(NodeId id) const noexcept;               // O(log n)
  std::optional<NodeIndex> findByName(std::string_view name) const noexcept;  // O(n)
  // Human-readable node reference for messages: `'name' (atom mul, #12)`.
  std::string describe(NodeIndex n) const;

  // ---- analysis -----------------------------------------------------------------------------------------------
  Adjacency adjacency() const;
  // Every structural and declared-type problem (empty = valid). See validate.cpp for the rules.
  std::vector<Error> validateAll() const;
  // The first problem, or Ok.
  Status validate() const;

  // Deep structural equality: same name, declarations in the same order, same nodes with the same names, payloads,
  // port types and layout, the same wires (as a set, with routing points), solve blocks and comments. NodeId values,
  // dense node order and wire order are ignored, so parse(print(p)) == p can hold although text carries no ids.
  // Nodes are matched by a canonical order: declarations, solve members, named nodes by name, then depth-first
  // through input wires; anonymous dead code is ordered by a structural hash (details in patch_equal.cpp). Never
  // reports a false "equal"; can only report a false "unequal" for symmetric dead code or a 64-bit hash collision.
  friend bool operator==(const Patch& a, const Patch& b);

private:
  NodeIndex addNode(NodeKind kind, std::string name, uint32_t payload, std::span<const PortType> inputs,
                    std::span<const PortType> outputs);
  NodeIndex addNode(NodeKind kind, std::string name, uint32_t payload, uint16_t numInputs, uint16_t numOutputs);

  std::string name_;
  std::vector<Node> nodes_;
  std::vector<PortType> ports_;
  std::vector<Wire> wires_;
  // Cold, parallel to nodes_ / wires_.
  std::vector<std::string> names_;
  std::vector<NodeLayout> layout_;
  std::vector<std::vector<Point2>> wireVia_;
  // Side tables indexed by Node::payload.
  std::vector<Constant> constants_;
  std::vector<ParamDecl> params_;
  std::vector<SubcircuitRef> subcircuits_;
  std::vector<Solve> solves_;
  std::vector<LayoutComment> comments_;
  StableIdAllocator<NodeTag> ids_;
};

}  // namespace pq::graph
