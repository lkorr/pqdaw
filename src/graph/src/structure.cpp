// Structural constants and switched-off branches of a patch as written (T4.15): pq/graph/structure.h.

#include "pq/graph/structure.h"

#include <algorithm>
#include <cmath>

#include "fold.h"
#include "pq/dsp/kernel.h"

namespace pq::graph {

namespace {

using namespace atom_flags;

uint32_t specIndex(const AtomSignature& sig, uint32_t i) noexcept {
  const auto n = static_cast<uint32_t>(sig.inputs.size());
  return i < n ? i : n - 1;
}

struct Evaluator {
  const Patch& p;
  std::span<const ConstArg> args;
  Adjacency adj;
  std::vector<float> lane;      // [flat output port] value in the kernels' domain (bool: mask)
  std::vector<uint8_t> known;   // [flat output port]
  std::vector<uint8_t> isBool;  // [flat output port] the value is a bool (best effort: for the eq / ne guard)
  std::vector<uint8_t> done;    // [node] evaluated (known outputs written)
  AtomId select = 0, mux = 0, eq = 0, ne = 0;

  Evaluator(const Patch& patch, std::span<const ConstArg> a) : p(patch), args(a), adj(patch.adjacency()) {
    lane.assign(p.portCount(), 0.0f);
    known.assign(p.portCount(), 0);
    isBool.assign(p.portCount(), 0);
    done.assign(p.nodeCount(), 0);
    select = atomId("select");
    mux = atomId("mux");
    eq = atomId("eq");
    ne = atomId("ne");
  }

  // The driver's output port of input i of n (kNone: unconnected).
  uint32_t driverPort(NodeIndex n, uint32_t i) const noexcept {
    const WireIndex w = adj.driver[p.flatInput(n, i)];
    if (w == kNone) return kNone;
    const Wire& wr = p.wire(w);
    return p.flatOutput(wr.from.node, wr.from.port);
  }

  // Input i of atom n as a constant: its driver's value, or the registry default of an unconnected optional input.
  bool input(NodeIndex n, uint32_t i, float& v, bool& b) const noexcept {
    const uint32_t d = driverPort(n, i);
    if (d != kNone) {
      if (!known[d]) return false;
      v = lane[d];
      b = isBool[d] != 0;
      return true;
    }
    const Node& nd = p.node(n);
    if (nd.kind != NodeKind::Atom) return false;
    const AtomSignature& sig = atomSignature(nd.atom);
    const PortSpec& ps = sig.inputs[specIndex(sig, i)];
    if (!ps.optional) return false;
    b = ps.type.kind == Kind::Bool;
    v = b ? dsp::boolMask(ps.defaultValue != 0.0f) : ps.defaultValue;
    return true;
  }

  void set(NodeIndex n, uint32_t port, float v, bool b) noexcept {
    const uint32_t o = p.flatOutput(n, port);
    lane[o] = v;
    isBool[o] = b ? 1 : 0;
    known[o] = 1;
  }

  // Evaluates n if it can be now; true when it produced its value(s).
  bool evaluate(NodeIndex n) {
    const Node& nd = p.node(n);
    switch (nd.kind) {
      case NodeKind::Const: {
        const Constant& c = p.constant(n);
        const bool b = c.kind == Kind::Bool;
        set(n, 0, b ? dsp::boolMask(c.literal != 0.0) : static_cast<float>(c.value()), b);
        return true;
      }
      case NodeKind::Input: {
        const PortType t = p.outputType(n, 0);
        if (t.rate != Rate::Const) return false;
        const bool b = t.kind == Kind::Bool;
        for (const ConstArg& a : args)
          if (a.name == p.nodeName(n)) {
            set(n, 0, b ? dsp::boolMask(a.value != 0.0) : static_cast<float>(a.value), b);
            return true;
          }
        const uint32_t d = nd.numInputs > 0 ? driverPort(n, 0) : kNone;  // the default expression
        if (d == kNone || !known[d]) return false;
        set(n, 0, lane[d], b || isBool[d] != 0);
        return true;
      }
      case NodeKind::Atom: break;
      default: return false;  // params, subcircuits, outputs, solve members: never constant
    }
    const AtomSignature& sig = atomSignature(nd.atom);
    if (sig.has(kStateful) || sig.has(kSideEffect) || nd.numOutputs == 0) return false;
    float v = 0.0f;
    bool b = false;
    if (nd.atom == select || nd.atom == mux) {
      uint32_t pick = 0;
      if (!input(n, 0, v, b)) return false;
      if (nd.atom == select) pick = dsp::maskIsTrue(v) ? 1u : 2u;
      else pick = 1u + detail::muxPick(v, nd.numInputs - 1u);
      if (pick >= nd.numInputs || !input(n, pick, v, b)) return false;
      set(n, 0, v, b);
      return true;
    }
    float in[detail::kMaxFoldPorts], out[detail::kMaxFoldPorts];
    if (nd.numInputs > detail::kMaxFoldPorts || nd.numOutputs > detail::kMaxFoldPorts) return false;
    bool anyBool = false;
    for (uint32_t i = 0; i < nd.numInputs; ++i) {
      if (!input(n, i, in[i], b)) return false;
      anyBool = anyBool || b;
    }
    if ((nd.atom == eq || nd.atom == ne) && anyBool) return false;  // infer() rewrites these (kernels are real-only)
    if (!detail::foldPureAtom(nd.atom, in, nd.numInputs, out, nd.numOutputs)) return false;
    for (uint32_t j = 0; j < nd.numOutputs; ++j) {
      const PortSpec& ps = sig.outputs[std::min<size_t>(j, sig.outputs.size() - 1)];
      const bool ob = ps.type.kind == Kind::Bool;
      if (!ob && !std::isfinite(out[j])) return false;  // stays a computation (as in the optimiser)
    }
    for (uint32_t j = 0; j < nd.numOutputs; ++j) {
      const PortSpec& ps = sig.outputs[std::min<size_t>(j, sig.outputs.size() - 1)];
      set(n, j, out[j], ps.type.kind == Kind::Bool);
    }
    return true;
  }

  // Worklist: every node once, then the consumers of each node that produced a value.
  void run() {
    const uint32_t n = p.nodeCount();
    std::vector<NodeIndex> work;
    work.reserve(n);
    for (NodeIndex v = n; v-- > 0;) work.push_back(v);
    while (!work.empty()) {
      const NodeIndex v = work.back();
      work.pop_back();
      if (done[v] || !evaluate(v)) continue;
      done[v] = 1;
      for (const WireIndex w : adj.outgoing(v)) {
        const NodeIndex t = p.wire(w).to.node;
        if (!done[t]) work.push_back(t);
      }
    }
  }
};

}  // namespace

bool StructuralInfo::constantAt(const Patch& p, NodeIndex n, uint32_t port, double& out) const noexcept {
  if (n >= p.nodeCount() || port >= p.node(n).numOutputs) return false;
  const uint32_t o = p.flatOutput(n, port);
  if (o >= known.size() || !known[o]) return false;
  out = value[o];
  return true;
}

StructuralInfo analyseStructure(const Patch& p, std::span<const ConstArg> args) {
  StructuralInfo info;
  Evaluator ev(p, args);
  ev.run();
  const uint32_t n = p.nodeCount(), nw = p.wireCount();
  info.known = ev.known;
  info.value.assign(p.portCount(), 0.0);
  for (uint32_t o = 0; o < p.portCount(); ++o)
    if (ev.known[o]) info.value[o] = ev.isBool[o] ? (dsp::maskIsTrue(ev.lane[o]) ? 1.0 : 0.0) : static_cast<double>(ev.lane[o]);
  for (NodeIndex v = 0; v < n; ++v)  // literals report what was written (not its float rounding)
    if (p.node(v).kind == NodeKind::Const && p.constant(v).kind == Kind::Real) info.value[p.flatOutput(v, 0)] = p.constant(v).value();

  // Dead wires: into the arms a constant selector does not pick.
  info.deadWire.assign(nw, 0);
  bool anyDead = false;
  for (NodeIndex v = 0; v < n; ++v) {
    const Node& nd = p.node(v);
    if (nd.kind != NodeKind::Atom || (nd.atom != ev.select && nd.atom != ev.mux) || nd.numInputs < 2) continue;
    float s = 0.0f;
    bool b = false;
    if (!ev.input(v, 0, s, b)) continue;
    const uint32_t pick = nd.atom == ev.select ? (dsp::maskIsTrue(s) ? 1u : 2u) : 1u + detail::muxPick(s, nd.numInputs - 1u);
    for (uint32_t i = 1; i < nd.numInputs; ++i) {
      if (i == pick) continue;
      const WireIndex w = ev.adj.driver[p.flatInput(v, i)];
      if (w == kNone) continue;
      info.deadWire[w] = 1;
      anyDead = true;
    }
  }
  info.nodeInactive.assign(n, 0);
  info.wireInactive.assign(nw, 0);
  if (!anyDead) return info;

  // Live: reaches a sink through non-dead wires. Dead-reaching: feeds a dead wire.
  std::vector<uint8_t> live(n, 0), feedsDead(n, 0);
  std::vector<NodeIndex> stack;
  for (NodeIndex v = 0; v < n; ++v)
    if (ev.adj.outgoing(v).empty()) {
      live[v] = 1;
      stack.push_back(v);
    }
  while (!stack.empty()) {
    const NodeIndex v = stack.back();
    stack.pop_back();
    for (uint32_t i = 0; i < p.node(v).numInputs; ++i) {
      const WireIndex w = ev.adj.driver[p.flatInput(v, i)];
      if (w == kNone || info.deadWire[w]) continue;
      const NodeIndex s = p.wire(w).from.node;
      if (!live[s]) {
        live[s] = 1;
        stack.push_back(s);
      }
    }
  }
  for (WireIndex w = 0; w < nw; ++w) {
    if (!info.deadWire[w]) continue;
    const NodeIndex s = p.wire(w).from.node;
    if (!feedsDead[s]) {
      feedsDead[s] = 1;
      stack.push_back(s);
    }
  }
  while (!stack.empty()) {
    const NodeIndex v = stack.back();
    stack.pop_back();
    for (uint32_t i = 0; i < p.node(v).numInputs; ++i) {
      const WireIndex w = ev.adj.driver[p.flatInput(v, i)];
      if (w == kNone) continue;
      const NodeIndex s = p.wire(w).from.node;
      if (!feedsDead[s]) {
        feedsDead[s] = 1;
        stack.push_back(s);
      }
    }
  }
  for (NodeIndex v = 0; v < n; ++v)
    if (!live[v] && feedsDead[v]) {
      info.nodeInactive[v] = 1;
      ++info.inactiveNodes;
    }
  for (WireIndex w = 0; w < nw; ++w)
    if (info.deadWire[w] || info.nodeInactive[p.wire(w).to.node]) {
      info.wireInactive[w] = 1;
      ++info.inactiveWires;
    }
  return info;
}

std::vector<ConstArg> callArguments(const Patch& p, const StructuralInfo& info, NodeIndex call) {
  std::vector<ConstArg> out;
  if (call >= p.nodeCount() || p.node(call).kind != NodeKind::Subcircuit) return out;
  const SubcircuitRef& ref = p.subcircuit(call);
  const Adjacency adj = p.adjacency();
  for (uint32_t i = 0; i < ref.inputs.size() && i < p.node(call).numInputs; ++i) {
    if (ref.inputs[i].type.rate != Rate::Const) continue;
    const WireIndex w = adj.driver[p.flatInput(call, i)];
    if (w == kNone) continue;
    const Wire& wr = p.wire(w);
    double v = 0.0;
    if (info.constantAt(p, wr.from.node, wr.from.port, v)) out.push_back({ref.inputs[i].name, v});
  }
  return out;
}

}  // namespace pq::graph
