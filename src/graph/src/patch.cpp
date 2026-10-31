#include "pq/graph/patch.h"

#include <algorithm>
#include <cstdio>

namespace pq::graph {

std::string_view toText(NodeKind k) noexcept {
  switch (k) {
    case NodeKind::Atom: return "atom";
    case NodeKind::Const: return "const";
    case NodeKind::Param: return "param";
    case NodeKind::Input: return "in";
    case NodeKind::Output: return "out";
    case NodeKind::Export: return "export";
    case NodeKind::Subcircuit: return "subcircuit";
    case NodeKind::Unknown: return "unknown";
    case NodeKind::Equation: return "equation";
  }
  return "?";
}

NodeIndex Patch::addNode(NodeKind kind, std::string name, uint32_t payload, uint16_t numInputs,
                         uint16_t numOutputs) {
  Node n;
  n.id = ids_.next();
  n.kind = kind;
  n.firstPort = static_cast<uint32_t>(ports_.size());
  n.numInputs = numInputs;
  n.numOutputs = numOutputs;
  n.payload = payload;
  ports_.resize(ports_.size() + numInputs + numOutputs);
  nodes_.push_back(n);
  names_.push_back(std::move(name));
  layout_.emplace_back();
  const auto index = static_cast<NodeIndex>(nodes_.size() - 1);
  if (!byId_.empty()) byId_.push_back(index);  // the new id is the largest
  return index;
}

NodeIndex Patch::addNode(NodeKind kind, std::string name, uint32_t payload, std::span<const PortType> inputs,
                         std::span<const PortType> outputs) {
  // Port counts are 16-bit (Node): more ports than that are cut off here (never written past the node's ports; M4
  // review M9), and the patch's checks then refuse the node (its ports do not match what it was made from).
  inputs = inputs.first(std::min<size_t>(inputs.size(), kMaxPorts));
  outputs = outputs.first(std::min<size_t>(outputs.size(), kMaxPorts));
  const NodeIndex n = addNode(kind, std::move(name), payload, static_cast<uint16_t>(inputs.size()),
                              static_cast<uint16_t>(outputs.size()));
  PortType* p = ports_.data() + nodes_[n].firstPort;
  std::copy(inputs.begin(), inputs.end(), p);
  std::copy(outputs.begin(), outputs.end(), p + inputs.size());
  return n;
}

NodeIndex Patch::addAtom(AtomId atom, uint16_t repeat) {
  const AtomSignature& sig = atomSignature(atom);
  const uint32_t count = repeat ? repeat : sig.minRepeat;
  const uint32_t fixedIn = sig.fixedInputs(), fixedOut = sig.fixedOutputs();
  const uint32_t numIn = fixedIn + (sig.has(atom_flags::kVariadicInputs) ? count : 0);
  const uint32_t numOut = fixedOut + (sig.has(atom_flags::kVariadicOutputs) ? count : 0);
  const NodeIndex n = addNode(NodeKind::Atom, {}, kNone, static_cast<uint16_t>(numIn), static_cast<uint16_t>(numOut));
  nodes_[n].atom = atom;
  // Port types come from the signature; a variadic signature's last spec covers every repeated port.
  PortType* p = ports_.data() + nodes_[n].firstPort;
  for (uint32_t i = 0; i < numIn; ++i) p[i] = sig.inputs[std::min(i, static_cast<uint32_t>(sig.inputs.size()) - 1)].type;
  for (uint32_t i = 0; i < numOut; ++i)
    p[numIn + i] = sig.outputs[std::min(i, static_cast<uint32_t>(sig.outputs.size()) - 1)].type;
  return n;
}

NodeIndex Patch::addConst(Constant c) {
  const PortType out = portType(c.kind, Rate::Const, Poly::Global, kWidthMono);
  const auto payload = static_cast<uint32_t>(constants_.size());
  constants_.push_back(c);
  return addNode(NodeKind::Const, {}, payload, {}, {&out, 1});
}

NodeIndex Patch::addParam(std::string name, ParamDecl decl) {
  const PortType out = portType(decl.kind, Rate::Param, Poly::Global, kWidthMono);
  const auto payload = static_cast<uint32_t>(params_.size());
  params_.push_back(std::move(decl));
  return addNode(NodeKind::Param, std::move(name), payload, {}, {&out, 1});
}

NodeIndex Patch::addInput(std::string name, PortType type) {
  return addNode(NodeKind::Input, std::move(name), kNone, {&type, 1}, {&type, 1});
}

NodeIndex Patch::addOutput(std::string name, PortType type) {
  return addNode(NodeKind::Output, std::move(name), kNone, {&type, 1}, {});
}

NodeIndex Patch::addExport(std::string name) {
  const PortType any{};
  return addNode(NodeKind::Export, std::move(name), kNone, {&any, 1}, {});
}

NodeIndex Patch::addSubcircuit(SubcircuitRef ref) {
  const auto payload = static_cast<uint32_t>(subcircuits_.size());
  // Port counts are 16-bit (see addNode): never written past the node's ports.
  const auto numIn = static_cast<uint16_t>(std::min<size_t>(ref.inputs.size(), kMaxPorts));
  const auto numOut = static_cast<uint16_t>(std::min<size_t>(ref.outputs.size(), kMaxPorts));
  const NodeIndex n = addNode(NodeKind::Subcircuit, {}, payload, numIn, numOut);
  PortType* p = ports_.data() + nodes_[n].firstPort;
  for (uint16_t i = 0; i < numIn; ++i) *p++ = ref.inputs[i].type;
  for (uint16_t i = 0; i < numOut; ++i) *p++ = ref.outputs[i].type;
  subcircuits_.push_back(std::move(ref));
  return n;
}

SolveIndex Patch::addSolve(uint32_t iterations) {
  solves_.push_back(Solve{iterations});
  return static_cast<SolveIndex>(solves_.size() - 1);
}

NodeIndex Patch::addUnknown(std::string name, SolveIndex solve) {
  const PortType real = portType(Kind::Real);
  return addNode(NodeKind::Unknown, std::move(name), solve, {&real, 1}, {&real, 1});
}

NodeIndex Patch::addEquation(SolveIndex solve) {
  const PortType real[2] = {portType(Kind::Real), portType(Kind::Real)};
  return addNode(NodeKind::Equation, {}, solve, real, {});
}

void Patch::setNodeName(NodeIndex n, std::string name, uint16_t nameOutput) {
  names_[n] = std::move(name);
  nodes_[n].nameOutput = nameOutput;
}

WireIndex Patch::connect(PortRef from, PortRef to) {
  wires_.push_back(Wire{from, to});
  wireVia_.emplace_back();
  return static_cast<WireIndex>(wires_.size() - 1);
}

void Patch::disconnect(WireIndex w) {
  if (w >= wires_.size()) return;
  if (w + 1 != wires_.size()) {
    wires_[w] = wires_.back();
    wireVia_[w] = std::move(wireVia_.back());
  }
  wires_.pop_back();
  wireVia_.pop_back();
}

void Patch::eraseNodes(std::span<const NodeIndex> erase) {
  const auto n = static_cast<uint32_t>(nodes_.size());
  std::vector<uint32_t> remap(n, 0);  // 1 = erase; then the new index, or kNone
  bool any = false;
  for (NodeIndex i : erase) {
    if (i < n) {
      remap[i] = 1;
      any = true;
    }
  }
  if (!any) return;

  // In-place forward compaction of every per-node array. Side-table payloads are in node order (each Const/Param/
  // Subcircuit node's payload is the number of earlier nodes of its kind), so each write position is at or before
  // its read position, and the invariant survives.
  uint32_t outNode = 0, outPort = 0, outConst = 0, outParam = 0, outSub = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (remap[i]) {
      remap[i] = kNone;
      continue;
    }
    Node nd = nodes_[i];
    const uint32_t count = static_cast<uint32_t>(nd.numInputs) + nd.numOutputs;
    if (outPort != nd.firstPort)
      std::copy(ports_.begin() + nd.firstPort, ports_.begin() + nd.firstPort + count, ports_.begin() + outPort);
    nd.firstPort = outPort;
    outPort += count;
    switch (nd.kind) {
      case NodeKind::Const:
        if (outConst != nd.payload) constants_[outConst] = constants_[nd.payload];
        nd.payload = outConst++;
        break;
      case NodeKind::Param:
        if (outParam != nd.payload) params_[outParam] = std::move(params_[nd.payload]);
        nd.payload = outParam++;
        break;
      case NodeKind::Subcircuit:
        if (outSub != nd.payload) subcircuits_[outSub] = std::move(subcircuits_[nd.payload]);
        nd.payload = outSub++;
        break;
      default: break;  // Unknown/Equation payload is a SolveIndex; solve blocks are kept
    }
    if (outNode != i) {
      names_[outNode] = std::move(names_[i]);
      layout_[outNode] = layout_[i];
    }
    nodes_[outNode] = nd;
    remap[i] = outNode++;
  }
  nodes_.resize(outNode);
  names_.resize(outNode);
  layout_.resize(outNode);
  ports_.resize(outPort);
  constants_.resize(outConst);
  params_.resize(outParam);
  subcircuits_.resize(outSub);

  uint32_t outWire = 0;
  for (size_t w = 0; w < wires_.size(); ++w) {
    Wire wr = wires_[w];
    // Wires whose endpoints are out of range are kept as they are (validate() reports them); wires touching an
    // erased node go.
    const bool fromOk = wr.from.node < n, toOk = wr.to.node < n;
    if ((fromOk && remap[wr.from.node] == kNone) || (toOk && remap[wr.to.node] == kNone)) continue;
    if (fromOk) wr.from.node = remap[wr.from.node];
    if (toOk) wr.to.node = remap[wr.to.node];
    wires_[outWire] = wr;
    if (outWire != w) wireVia_[outWire] = std::move(wireVia_[w]);
    ++outWire;
  }
  wires_.resize(outWire);
  wireVia_.resize(outWire);

  if (!byId_.empty()) {  // still sorted by id after dropping the erased nodes and renumbering the rest
    uint32_t out = 0;
    for (NodeIndex i : byId_)
      if (remap[i] != kNone) byId_[out++] = remap[i];
    byId_.resize(out);
    if (std::is_sorted(byId_.begin(), byId_.end())) byId_.clear();
  }
}

bool Patch::setNodeIds(std::span<const NodeId> ids) {
  const auto n = static_cast<uint32_t>(nodes_.size());
  if (ids.size() != n) return false;
  uint32_t maxId = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (!ids[i].valid()) return false;
    maxId = std::max(maxId, ids[i].value);
  }
  // order = node indices sorted by id: a counting pass when the ids are dense (as inheritNodeIds produces), else a
  // sort. Either way a duplicate is found.
  std::vector<NodeIndex> order;
  order.reserve(n);
  if (maxId <= 8ull * n + 1024u) {
    std::vector<NodeIndex> slot(static_cast<size_t>(maxId) + 1, kNone);
    for (uint32_t i = 0; i < n; ++i) {
      if (slot[ids[i].value] != kNone) return false;
      slot[ids[i].value] = i;
    }
    for (NodeIndex i : slot)
      if (i != kNone) order.push_back(i);
  } else {
    for (uint32_t i = 0; i < n; ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](NodeIndex a, NodeIndex b) { return ids[a] < ids[b]; });
    for (uint32_t k = 1; k < n; ++k)
      if (ids[order[k - 1]] == ids[order[k]]) return false;
  }
  for (uint32_t i = 0; i < n; ++i) nodes_[i].id = ids[i];
  if (n) ids_.reserveThrough(ids[order.back()]);
  if (std::is_sorted(order.begin(), order.end())) {
    byId_.clear();
  } else {
    byId_ = std::move(order);
  }
  return true;
}

std::string_view Patch::inputName(NodeIndex n, uint32_t i) const noexcept {
  const Node& nd = nodes_[n];
  switch (nd.kind) {
    case NodeKind::Atom: {
      const AtomSignature& sig = atomSignature(nd.atom);
      return sig.inputs[std::min<size_t>(i, sig.inputs.size() - 1)].name;
    }
    case NodeKind::Subcircuit: return subcircuits_[nd.payload].inputs[i].name;
    case NodeKind::Input: return "default";
    case NodeKind::Unknown: return "init";
    case NodeKind::Equation: return i == 0 ? "lhs" : "rhs";
    case NodeKind::Output:
    case NodeKind::Export: return "in";
    case NodeKind::Const:
    case NodeKind::Param: break;
  }
  return "";
}

std::string_view Patch::outputName(NodeIndex n, uint32_t i) const noexcept {
  const Node& nd = nodes_[n];
  switch (nd.kind) {
    case NodeKind::Atom: {
      const AtomSignature& sig = atomSignature(nd.atom);
      return sig.outputs[std::min<size_t>(i, sig.outputs.size() - 1)].name;
    }
    case NodeKind::Subcircuit: return subcircuits_[nd.payload].outputs[i].name;
    case NodeKind::Const:
    case NodeKind::Param:
    case NodeKind::Input:
    case NodeKind::Unknown: return "out";
    case NodeKind::Output:
    case NodeKind::Export:
    case NodeKind::Equation: break;
  }
  return "";
}

bool Patch::inputOptional(NodeIndex n, uint32_t i) const noexcept {
  const Node& nd = nodes_[n];
  switch (nd.kind) {
    case NodeKind::Atom: {
      const AtomSignature& sig = atomSignature(nd.atom);
      return sig.inputs[std::min<size_t>(i, sig.inputs.size() - 1)].optional;
    }
    case NodeKind::Subcircuit: return subcircuits_[nd.payload].inputs[i].optional;
    case NodeKind::Input:    // unconnected "default": the `in` has no default, so callers must connect it
    case NodeKind::Unknown:  // unconnected "init": the guess is the previous sample's solution
      return true;
    default: return false;
  }
}

std::optional<uint32_t> Patch::findInput(NodeIndex n, std::string_view name) const noexcept {
  for (uint32_t i = 0; i < nodes_[n].numInputs; ++i)
    if (inputName(n, i) == name) return i;
  return std::nullopt;
}

std::optional<uint32_t> Patch::findOutput(NodeIndex n, std::string_view name) const noexcept {
  for (uint32_t i = 0; i < nodes_[n].numOutputs; ++i)
    if (outputName(n, i) == name) return i;
  return std::nullopt;
}

std::optional<NodeIndex> Patch::indexOf(NodeId id) const noexcept {
  if (!byId_.empty()) {
    const auto it = std::lower_bound(byId_.begin(), byId_.end(), id,
                                     [&](NodeIndex i, NodeId v) { return nodes_[i].id < v; });
    if (it != byId_.end() && nodes_[*it].id == id) return *it;
    return std::nullopt;
  }
  if (nodes_.empty() || id < nodes_.front().id) return std::nullopt;
  // Ids increase strictly with the index, so a node's index is at most its id's offset from the first id; with no
  // erases since the first node, it is exactly that (one probe).
  const uint32_t guess = id.value - nodes_.front().id.value;
  if (guess < nodes_.size() && nodes_[guess].id == id) return guess;
  const auto end = nodes_.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(guess, nodes_.size()));
  const auto it = std::lower_bound(nodes_.begin(), end, id, [](const Node& n, NodeId v) { return n.id < v; });
  if (it != end && it->id == id) return static_cast<NodeIndex>(it - nodes_.begin());
  return std::nullopt;
}

std::optional<NodeIndex> Patch::findByName(std::string_view name) const noexcept {
  for (uint32_t i = 0; i < names_.size(); ++i)
    if (names_[i] == name) return i;
  return std::nullopt;
}

std::string Patch::describe(NodeIndex n) const {
  if (n >= nodes_.size()) return "#" + std::to_string(n) + " (no such node)";
  const Node& nd = nodes_[n];
  std::string s;
  if (!names_[n].empty()) s = "'" + names_[n] + "' ";
  s += '(';
  s += toText(nd.kind);
  if (nd.kind == NodeKind::Atom) {
    s += ' ';
    s += atomSignature(nd.atom).name;
  } else if (nd.kind == NodeKind::Subcircuit) {
    s += ' ';
    s += subcircuits_[nd.payload].patch;
  }
  s += ", #" + std::to_string(n) + ')';
  return s;
}

Adjacency Patch::adjacency() const {
  Adjacency a;
  const auto n = static_cast<uint32_t>(nodes_.size());
  a.driver.assign(ports_.size(), kNone);
  a.outBegin.assign(n + 1, 0);
  auto inRange = [&](const Wire& w) {
    return w.from.node < n && w.to.node < n && w.from.port < nodes_[w.from.node].numOutputs &&
           w.to.port < nodes_[w.to.node].numInputs;
  };
  for (WireIndex w = 0; w < wires_.size(); ++w) {
    const Wire& wr = wires_[w];
    if (!inRange(wr)) continue;
    uint32_t& d = a.driver[flatInput(wr.to.node, wr.to.port)];
    if (d == kNone) d = w;
    ++a.outBegin[wr.from.node + 1];
  }
  for (uint32_t i = 0; i < n; ++i) a.outBegin[i + 1] += a.outBegin[i];
  a.outWires.resize(a.outBegin[n]);
  std::vector<uint32_t> fill(a.outBegin.begin(), a.outBegin.end() - 1);
  for (WireIndex w = 0; w < wires_.size(); ++w)
    if (inRange(wires_[w])) a.outWires[fill[wires_[w].from.node]++] = w;
  return a;
}

Status Patch::validate() const {
  std::vector<Error> errors = validateAll();
  if (errors.empty()) return Ok{};
  return std::move(errors.front());
}

}  // namespace pq::graph
