// DY.3: the control evaluator shared by LinearResponse and StaticTransfer (pq/graph/control_eval.h).

#include "pq/graph/control_eval.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string_view>

#include "fold.h"
#include "pq/dsp/kernel.h"
#include "pq/graph/kernel_map.h"

namespace pq::graph {

namespace {

bool pureKernel(AtomId atom) noexcept {
  const KernelMapping& km = kernelMapping(atom);
  if (km.lowering != Lowering::Kernel) return false;
  const dsp::KernelInfo& ki = dsp::kernelInfo(km.kernel);
  if (ki.resource != dsp::Resource::None || ki.splRequired != 0 || ki.stateSize != 0) return false;
  for (const ArgMap& am : km.args)
    if (am.use != ArgUse::Input) return false;
  return true;
}

}  // namespace

ControlEval::Graph::Graph(const TypedPatch& t) : typed(t), p(t.patch), adj(t.patch.adjacency()) {
  const uint32_t nodes = p.nodeCount();
  owner.assign(p.portCount(), kNone);
  for (NodeIndex n = 0; n < nodes; ++n)
    for (uint32_t o = 0; o < p.node(n).numOutputs; ++o) owner[p.flatOutput(n, o)] = n;
  setOut.assign(nodes, 0);
  state.assign(nodes, 0);
}

void ControlEval::Graph::cut(std::span<const uint32_t> settable, NodeIndex keep) {
  for (const uint32_t f : settable)
    if (f < owner.size() && owner[f] != kNone && owner[f] != keep && setOut[owner[f]] == 0)
      setOut[owner[f]] = static_cast<uint16_t>(1u + (f - p.flatOutput(owner[f], 0)));
}

uint32_t ControlEval::Graph::driverPort(NodeIndex n, uint32_t i) const noexcept {
  const WireIndex w = adj.driver[p.flatInput(n, i)];
  if (w == kNone) return kNone;
  const Wire& wr = p.wire(w);
  return p.flatOutput(wr.from.node, wr.from.port);
}

uint16_t ControlEval::Graph::width(uint32_t flatPort) const noexcept {
  return std::max<uint16_t>(1, typed.types[flatPort].width);
}

float ControlEval::Graph::inputDefault(NodeIndex n, uint32_t i) const noexcept {
  const Node& node = p.node(n);
  if (node.kind != NodeKind::Atom) return 0.0f;
  const AtomSignature& s = atomSignature(node.atom);
  if (s.inputs.empty()) return 0.0f;
  const PortSpec& spec = s.inputs[std::min<size_t>(i, s.inputs.size() - 1)];  // (a variadic atom's repeats: the last)
  if (spec.type.kind == Kind::Bool) return dsp::boolMask(spec.defaultValue != 0.0f);
  return spec.defaultValue;
}

uint8_t ControlEval::Graph::rateOf(NodeIndex n) const noexcept {
  if (n >= typed.origin.size() || typed.origin[n].region == 0) return 0;
  const Node& node = p.node(n);
  static const AtomId osUp = atomId("oversample_up"), osDown = atomId("oversample_down");
  if (node.kind == NodeKind::Atom && (node.atom == osUp || node.atom == osDown)) return 0;
  const uint16_t r = typed.origin[n].region;
  const uint32_t f = r <= typed.oversample.size() ? typed.oversample[r - 1u].factor : 1u;
  return static_cast<uint8_t>(std::min<uint32_t>(kRates - 1u, static_cast<uint32_t>(std::countr_zero(std::max(f, 1u)))));
}

bool ControlEval::Graph::computes(NodeIndex n) const noexcept {
  const Node& node = p.node(n);
  if (node.kind != NodeKind::Atom || setOut[n] != 0) return false;
  const std::string_view name = atomSignature(node.atom).name;
  return name == "curve" || name == "channel" || name == "pack" || pureKernel(node.atom);
}

uint32_t ControlEval::Graph::resolve(ResponseEnd e, bool& valid, uint16_t& w) const noexcept {
  valid = e.node < p.nodeCount();
  if (!valid) return kNone;
  const Node& n = p.node(e.node);
  if (e.input) {
    valid = e.port < n.numInputs;
    if (!valid) return kNone;
    const uint32_t d = driverPort(e.node, e.port);
    w = width(d != kNone ? d : p.flatInput(e.node, e.port));
    return d;
  }
  valid = e.port < n.numOutputs;
  if (!valid) return kNone;
  const uint32_t f = p.flatOutput(e.node, e.port);
  w = width(f);
  return f;
}

void ControlEval::Graph::signalFrom(uint32_t src, std::vector<uint8_t>& sig) const {
  const uint32_t nodes = p.nodeCount();
  const NodeIndex srcNode = owner[src];
  sig.assign(p.portCount(), 0);
  std::vector<uint8_t> seen(nodes, 0);
  std::vector<NodeIndex> work;
  sig[src] = 1;
  for (WireIndex w : adj.outgoing(srcNode))
    if (p.flatOutput(srcNode, p.wire(w).from.port) == src) work.push_back(p.wire(w).to.node);
  while (!work.empty()) {
    const NodeIndex n = work.back();
    work.pop_back();
    if (n == srcNode || seen[n] != 0 || setOut[n] != 0) continue;  // (a set port's node: its value is given)
    seen[n] = 1;
    for (uint32_t o = 0; o < p.node(n).numOutputs; ++o) sig[p.flatOutput(n, o)] = 1;
    for (WireIndex w : adj.outgoing(n)) work.push_back(p.wire(w).to.node);
  }
}

void ControlEval::Graph::relevantTo(uint32_t sink, uint32_t src, const std::vector<uint8_t>& sig, std::vector<uint8_t>& rel,
                                    std::vector<uint8_t>& relNode) const {
  const NodeIndex srcNode = owner[src];
  rel.assign(p.portCount(), 0);
  relNode.assign(p.nodeCount(), 0);
  std::vector<NodeIndex> work;
  rel[sink] = 1;
  if (owner[sink] != srcNode) work.push_back(owner[sink]);
  while (!work.empty()) {
    const NodeIndex n = work.back();
    work.pop_back();
    if (relNode[n] != 0) continue;
    relNode[n] = 1;
    for (uint32_t i = 0; i < p.node(n).numInputs; ++i) {
      const uint32_t d = driverPort(n, i);
      if (d == kNone || sig[d] == 0 || rel[d] != 0) continue;
      rel[d] = 1;
      if (owner[d] != srcNode) work.push_back(owner[d]);
    }
  }
}

void ControlEval::beginControls(const Graph& g) {
  ctrlOff_.assign(g.p.portCount(), kNone);
  ctrlKnown_.assign(g.p.portCount(), 0);
  ctrlVal_.clear();
  ops_.clear();
  ctrlOuts_.clear();
  ctrlIns_.clear();
  paramNode_.clear();
  paramValue_.clear();
  curveNode_.clear();
  setPort_.clear();
}

void ControlEval::needControl(Graph& g, NodeIndex root, uint32_t source) {
  const Patch& p = g.p;
  const NodeIndex sourceNode = source != kNone ? g.owner[source] : kNone;
  auto emit = [&](NodeIndex n) {
    const Node& node = p.node(n);
    CtrlOp op;
    op.atom = node.atom;
    op.node = n;
    op.rate = g.rateOf(n);
    if (n == sourceNode) {  // StaticTransfer: the varied value (its inputs are not evaluated)
      op.kind = OpKind::Source;
      op.setOutput = static_cast<uint16_t>(source - p.flatOutput(n, 0));
    } else if (g.setOut[n] != 0) {  // TX.mod-bands: a set port (its inputs are not evaluated)
      op.kind = OpKind::Set;
      op.index = static_cast<uint32_t>(setPort_.size());
      op.setOutput = static_cast<uint16_t>(g.setOut[n] - 1u);
      setPort_.push_back(p.flatOutput(n, op.setOutput));
    } else
      switch (node.kind) {
        case NodeKind::Const: {
          op.kind = OpKind::Const;
          const Constant& c = p.constant(n);
          op.constant = c.kind == Kind::Bool ? dsp::boolMask(c.literal != 0.0) : static_cast<float>(c.value());
          break;
        }
        case NodeKind::Param: {
          op.kind = OpKind::Param;
          op.index = static_cast<uint32_t>(paramNode_.size());
          const ParamDecl& d = p.paramDecl(n);
          paramNode_.push_back(n);
          paramValue_.push_back(static_cast<float>(d.defaultValue.value()));
          op.constant = d.kind == Kind::Bool ? 1.0f : 0.0f;  // Param: 1 = a bool (written 0 / 1, read as a mask)
          break;
        }
        case NodeKind::Atom: {
          const std::string_view name = atomSignature(node.atom).name;
          if (name == "sr") op.kind = OpKind::Sr;
          else if (name == "curve") {
            op.kind = OpKind::Curve;
            op.index = static_cast<uint32_t>(curveNode_.size());
            curveNode_.push_back(n);
          } else if (name == "channel") op.kind = OpKind::Channel;
          else if (name == "pack") op.kind = OpKind::Pack;
          else if (pureKernel(node.atom) && node.numInputs <= detail::kMaxFoldPorts &&
                   node.numOutputs <= detail::kMaxFoldPorts)
            op.kind = OpKind::Fold;
          break;
        }
        default: break;  // Input, Unknown, Equation …: unknown
      }
    op.inBegin = static_cast<uint32_t>(ctrlIns_.size());
    if (op.kind != OpKind::Unknown && op.kind != OpKind::Set && op.kind != OpKind::Source)
      for (uint32_t i = 0; i < node.numInputs; ++i) {
        InSrc s;
        s.port = g.driverPort(n, i);
        s.def = g.inputDefault(n, i);
        s.width = s.port != kNone ? g.width(s.port) : 1;
        ctrlIns_.push_back(s);
      }
    op.inCount = static_cast<uint32_t>(ctrlIns_.size()) - op.inBegin;
    op.outBegin = static_cast<uint32_t>(ctrlOuts_.size());
    for (uint32_t o = 0; o < node.numOutputs; ++o) op.width = std::max(op.width, g.width(p.flatOutput(n, o)));
    for (uint32_t o = 0; o < node.numOutputs; ++o) {  // every output holds op.width channels
      const uint32_t f = p.flatOutput(n, o);
      ctrlOff_[f] = static_cast<uint32_t>(ctrlVal_.size());
      ctrlVal_.resize(ctrlVal_.size() + op.width, 0.0f);
      ctrlOuts_.push_back(f);
    }
    op.outCount = node.numOutputs;
    ops_.push_back(op);
  };
  if (g.state[root] != 0) return;
  g.stack.push_back({root, 0});
  g.state[root] = 1;
  while (!g.stack.empty()) {
    auto& [n, next] = g.stack.back();
    if (n != sourceNode && g.computes(n) && next < p.node(n).numInputs) {
      const uint32_t d = g.driverPort(n, next++);
      if (d != kNone && g.state[g.owner[d]] == 0) {
        g.state[g.owner[d]] = 1;
        g.stack.push_back({g.owner[d], 0});
      }
      continue;
    }
    const NodeIndex done = n;
    g.state[done] = 2;
    g.stack.pop_back();
    emit(done);
  }
}

void ControlEval::endControls() {
  curve_.assign(curveNode_.size(), dsp::Curve{});
  curveSet_.assign(curveNode_.size(), 0);
  setValue_.assign(setPort_.size(), 0.0f);
  setKnown_.assign(setPort_.size(), 0);
}

bool ControlEval::readControl(const InSrc& s, uint32_t channel, float& v) const noexcept {
  if (s.port == kNone) {
    v = s.def;
    return true;
  }
  const uint32_t off = ctrlOff_[s.port];
  if (off == kNone || ctrlKnown_[s.port] == 0) return false;
  v = ctrlVal_[off + (s.width == 1 ? 0u : std::min<uint32_t>(channel, s.width - 1u))];
  return true;
}

void ControlEval::runControl(double sampleRate, uint32_t first, float sourceValue) noexcept {
  for (uint32_t oi = first; oi < ops_.size(); ++oi) {
    const CtrlOp& op = ops_[oi];
    bool known = true;
    const uint32_t f0 = ctrlOuts_[op.outBegin];
    switch (op.kind) {
      case OpKind::Const:
        for (uint32_t k = 0; k < op.outCount; ++k)
          std::fill_n(ctrlVal_.begin() + ctrlOff_[ctrlOuts_[op.outBegin + k]], op.width, op.constant);
        break;
      case OpKind::Param: {
        const float v = op.constant != 0.0f ? dsp::boolMask(paramValue_[op.index] != 0.0f) : paramValue_[op.index];
        std::fill_n(ctrlVal_.begin() + ctrlOff_[f0], op.width, v);
        break;
      }
      case OpKind::Sr:  // (inside an oversample region: n·sr)
        std::fill_n(ctrlVal_.begin() + ctrlOff_[f0], op.width, static_cast<float>(sampleRate * static_cast<double>(1u << op.rate)));
        break;
      case OpKind::Curve: {
        float convert = 1.0f;
        known = curveSet_[op.index] != 0 && (op.inCount < 2 || readControl(ctrlIns_[op.inBegin + 1], 0, convert));
        for (uint16_t c = 0; known && c < op.width; ++c) {
          float x = 0.0f;
          known = readControl(ctrlIns_[op.inBegin], c, x);
          const dsp::Curve& cv = curve_[op.index];
          ctrlVal_[ctrlOff_[f0] + c] = static_cast<float>(convert != 0.0f ? dsp::evalCurve(cv, x) : dsp::evalCurveUnits(cv, x));
        }
        break;
      }
      case OpKind::Channel: {
        float idx = 0.0f, x = 0.0f;
        known = op.inCount >= 1 && (op.inCount < 2 || readControl(ctrlIns_[op.inBegin + 1], 0, idx));
        const uint32_t i = std::isfinite(idx) && idx > 0.0f ? static_cast<uint32_t>(idx) : 0u;
        known = known && readControl(ctrlIns_[op.inBegin], i, x);
        ctrlVal_[ctrlOff_[f0]] = x;
        break;
      }
      case OpKind::Pack: {
        uint32_t c = 0;
        for (uint32_t k = 0; known && k < op.inCount; ++k) {
          const InSrc& s = ctrlIns_[op.inBegin + k];
          for (uint16_t j = 0; known && j < s.width && c < op.width; ++j, ++c) known = readControl(s, j, ctrlVal_[ctrlOff_[f0] + c]);
        }
        break;
      }
      case OpKind::Fold: {
        float in[detail::kMaxFoldPorts], out[detail::kMaxFoldPorts];
        for (uint16_t c = 0; known && c < op.width; ++c) {
          for (uint32_t k = 0; known && k < op.inCount; ++k) known = readControl(ctrlIns_[op.inBegin + k], c, in[k]);
          known = known && detail::foldPureAtom(op.atom, in, op.inCount, out, op.outCount);
          for (uint32_t k = 0; known && k < op.outCount; ++k) {
            const uint32_t f = ctrlOuts_[op.outBegin + k];
            ctrlVal_[ctrlOff_[f] + c] = out[k];
          }
        }
        break;
      }
      case OpKind::Set:
      case OpKind::Source: {  // the given value on its output; the node's others are unknown
        known = op.kind == OpKind::Source || setKnown_[op.index] != 0;
        const uint32_t f = ctrlOuts_[op.outBegin + op.setOutput];
        std::fill_n(ctrlVal_.begin() + ctrlOff_[f], op.width, op.kind == OpKind::Source ? sourceValue : setValue_[op.index]);
        for (uint32_t k = 0; k < op.outCount; ++k) ctrlKnown_[ctrlOuts_[op.outBegin + k]] = known && k == op.setOutput ? 1 : 0;
        continue;
      }
      case OpKind::Unknown: known = false; break;
    }
    for (uint32_t k = 0; k < op.outCount; ++k) ctrlKnown_[ctrlOuts_[op.outBegin + k]] = known ? 1 : 0;
  }
}

}  // namespace pq::graph
