// EQ.2: the analytic frequency response of a typed graph (pq/graph/response.h).

#include "pq/graph/response.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

#include "fold.h"
#include "response_rules.h"
#include "pq/dsp/kernel.h"
#include "pq/graph/kernel_map.h"

namespace pq::graph {

namespace {

using Cx = std::complex<double>;

uint16_t widthOf(const TypedPatch& t, uint32_t flatPort) noexcept {
  return std::max<uint16_t>(1, t.types[flatPort].width);
}

// The default an unconnected input reads (its spec's; a variadic atom's repeats use the last spec), in the kernels'
// domain (bools as masks).
float inputDefault(const Patch& p, NodeIndex n, uint32_t i) noexcept {
  const Node& node = p.node(n);
  if (node.kind != NodeKind::Atom) return 0.0f;
  const AtomSignature& s = atomSignature(node.atom);
  if (s.inputs.empty()) return 0.0f;
  const PortSpec& spec = s.inputs[std::min<size_t>(i, s.inputs.size() - 1)];
  if (spec.type.kind == Kind::Bool) return dsp::boolMask(spec.defaultValue != 0.0f);
  return spec.defaultValue;
}

// Whether registry input `i` of `atom` is a duration the language writes in seconds (the kernel takes samples).
bool isDuration(AtomId atom, uint32_t i) noexcept {
  const KernelMapping& km = kernelMapping(atom);
  if (km.args.empty()) return false;
  const ArgUse u = km.args[std::min<size_t>(i, km.args.size() - 1)].use;
  return u == ArgUse::InputSeconds || u == ArgUse::InitMaxDelaySeconds;
}

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

void LinearResponse::prepare(const TypedPatch& typed, ResponseEnd source, ResponseEnd sink) {
  *this = LinearResponse{};
  const Patch& p = typed.patch;
  durationsInSamples_ = typed.durationsInSamples;
  const uint32_t nodes = p.nodeCount(), ports = p.portCount();
  if (typed.types.size() != ports) return;
  const Adjacency adj = p.adjacency();
  std::vector<NodeIndex> owner(ports, kNone);
  for (NodeIndex n = 0; n < nodes; ++n)
    for (uint32_t o = 0; o < p.node(n).numOutputs; ++o) owner[p.flatOutput(n, o)] = n;
  auto driverPort = [&](NodeIndex n, uint32_t i) -> uint32_t {
    const WireIndex w = adj.driver[p.flatInput(n, i)];
    if (w == kNone) return kNone;
    const Wire& wr = p.wire(w);
    return p.flatOutput(wr.from.node, wr.from.port);
  };
  // The ends as flat output ports. `valid` false: out of range.
  auto resolve = [&](ResponseEnd e, bool& valid, uint16_t& width) -> uint32_t {
    valid = e.node < nodes;
    if (!valid) return kNone;
    const Node& n = p.node(e.node);
    if (e.input) {
      valid = e.port < n.numInputs;
      if (!valid) return kNone;
      const uint32_t d = driverPort(e.node, e.port);
      width = widthOf(typed, d != kNone ? d : p.flatInput(e.node, e.port));
      return d;
    }
    valid = e.port < n.numOutputs;
    if (!valid) return kNone;
    const uint32_t f = p.flatOutput(e.node, e.port);
    width = widthOf(typed, f);
    return f;
  };
  bool srcOk = false, sinkOk = false;
  const uint32_t srcPort = resolve(source, srcOk, srcWidth_);
  const uint32_t sinkPort = resolve(sink, sinkOk, sinkWidth_);
  if (!srcOk || !sinkOk || srcPort == kNone) return;  // Unavailable
  if (srcPort == sinkPort) {
    identity_ = true;
    prepared_ = ResponseKind::Exact;
    return;
  }
  const NodeIndex srcNode = owner[srcPort];

  // ---- signal: forward from the source (its own inputs cut) ----
  std::vector<uint8_t> sig(ports, 0), seen(nodes, 0);
  std::vector<NodeIndex> work;
  sig[srcPort] = 1;
  for (WireIndex w : adj.outgoing(srcNode))
    if (p.flatOutput(srcNode, p.wire(w).from.port) == srcPort) work.push_back(p.wire(w).to.node);
  while (!work.empty()) {
    const NodeIndex n = work.back();
    work.pop_back();
    if (n == srcNode || seen[n] != 0) continue;
    seen[n] = 1;
    for (uint32_t o = 0; o < p.node(n).numOutputs; ++o) sig[p.flatOutput(n, o)] = 1;
    for (WireIndex w : adj.outgoing(n)) work.push_back(p.wire(w).to.node);
  }
  if (sinkPort == kNone || sig[sinkPort] == 0) {
    zero_ = true;
    prepared_ = ResponseKind::Exact;
    return;
  }

  // ---- relevance: back from the sink through signal ports ----
  std::vector<uint8_t> rel(ports, 0), relNode(nodes, 0);
  rel[sinkPort] = 1;
  if (owner[sinkPort] != srcNode) work.push_back(owner[sinkPort]);
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
  for (NodeIndex n = 0; n < nodes; ++n) {
    if (relNode[n] == 0) continue;
    const Node& node = p.node(n);
    if (node.kind != NodeKind::Atom || atomSignature(node.atom).response == nullptr) return;  // Unavailable
  }
  // TX.group-oversample: the rate each node runs at (log2 of its oversample region's factor; 0: the base rate). The
  // converters are evaluated at the base rate (their rules are the half-band cascades at the base omega).
  const AtomId osUp = atomId("oversample_up"), osDown = atomId("oversample_down");
  auto rateOf = [&](NodeIndex n) -> uint8_t {
    if (n >= typed.origin.size() || typed.origin[n].region == 0) return 0;
    const Node& node = p.node(n);
    if (node.kind == NodeKind::Atom && (node.atom == osUp || node.atom == osDown)) return 0;
    const uint16_t r = typed.origin[n].region;
    const uint32_t f = r <= typed.oversample.size() ? typed.oversample[r - 1u].factor : 1u;
    return static_cast<uint8_t>(std::min<uint32_t>(kRates - 1u, static_cast<uint32_t>(std::countr_zero(std::max(f, 1u)))));
  };

  // ---- unknowns: one per (relevant port, channel) ----
  std::vector<uint32_t> varBase(ports, kNone);
  uint32_t vars = 0;
  for (uint32_t f = 0; f < ports; ++f)
    if (rel[f] != 0) {
      varBase[f] = vars;
      vars += widthOf(typed, f);
    }
  for (uint16_t s = 0; s < srcWidth_; ++s) srcVar_.push_back(varBase[srcPort] + s);
  for (uint16_t j = 0; j < sinkWidth_; ++j) sinkVar_.push_back(varBase[sinkPort] + std::min<uint32_t>(j, widthOf(typed, sinkPort) - 1u));

  // ---- control values: what relevant nodes read besides the signal, in dependency order ----
  ctrlOff_.assign(ports, kNone);
  ctrlKnown_.assign(ports, 0);
  std::vector<uint8_t> ctrlState(nodes, 0);  // 0 new, 1 open, 2 emitted
  std::vector<std::pair<NodeIndex, uint32_t>> stack;
  auto addCtrlOp = [&](NodeIndex n) {
    const Node& node = p.node(n);
    CtrlOp op;
    op.atom = node.atom;
    op.rate = rateOf(n);
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
    op.inBegin = static_cast<uint32_t>(ins_.size());
    if (op.kind != OpKind::Unknown)
      for (uint32_t i = 0; i < node.numInputs; ++i) {
        InSrc s;
        s.port = driverPort(n, i);
        s.def = inputDefault(p, n, i);
        s.width = s.port != kNone ? widthOf(typed, s.port) : 1;
        ins_.push_back(s);
      }
    op.inCount = static_cast<uint32_t>(ins_.size()) - op.inBegin;
    op.outBegin = static_cast<uint32_t>(ctrlOuts_.size());
    for (uint32_t o = 0; o < node.numOutputs; ++o) op.width = std::max(op.width, widthOf(typed, p.flatOutput(n, o)));
    for (uint32_t o = 0; o < node.numOutputs; ++o) {  // every output holds op.width channels
      const uint32_t f = p.flatOutput(n, o);
      ctrlOff_[f] = static_cast<uint32_t>(ctrlVal_.size());
      ctrlVal_.resize(ctrlVal_.size() + op.width, 0.0f);
      ctrlOuts_.push_back(f);
    }
    op.outCount = node.numOutputs;
    ops_.push_back(op);
  };
  // Whether a node's inputs are evaluated (it is computed from them, so they are needed too).
  auto computes = [&](NodeIndex n) {
    const Node& node = p.node(n);
    if (node.kind != NodeKind::Atom) return false;
    const std::string_view name = atomSignature(node.atom).name;
    return name == "curve" || name == "channel" || name == "pack" || pureKernel(node.atom);
  };
  auto needCtrl = [&](NodeIndex root) {
    if (ctrlState[root] != 0) return;
    stack.push_back({root, 0});
    ctrlState[root] = 1;
    while (!stack.empty()) {
      auto& [n, next] = stack.back();
      if (computes(n) && next < p.node(n).numInputs) {
        const uint32_t d = driverPort(n, next++);
        if (d != kNone && ctrlState[owner[d]] == 0) {
          ctrlState[owner[d]] = 1;
          stack.push_back({owner[d], 0});
        }
        continue;
      }
      ctrlState[n] = 2;
      addCtrlOp(n);
      stack.pop_back();
    }
  };
  for (NodeIndex n = 0; n < nodes; ++n) {
    if (relNode[n] == 0) continue;
    for (uint32_t i = 0; i < p.node(n).numInputs; ++i) {
      const uint32_t d = driverPort(n, i);
      if (d != kNone && sig[d] == 0) needCtrl(owner[d]);
    }
  }
  // An `smp` literal wired straight into a duration is samples already (optimise()'s rule, QUESTIONS Q35).
  auto smpLiteral = [&](uint32_t port) {
    if (port == kNone) return false;
    const NodeIndex o = owner[port];
    return p.node(o).kind == NodeKind::Const && p.constant(o).unit == Unit::smp;
  };

  // ---- cells and terms ----
  std::vector<std::pair<uint32_t, Term>> edges;  // (unknown, term)
  uint32_t argCount = 0;
  size_t cacheCount = 0;  // doubles
  gain_.assign(kRates, Cx{});  // gain_[r]: z at the current point at rate r (an oversample region; the z1 terms)
  for (NodeIndex n = 0; n < nodes; ++n) {
    if (relNode[n] == 0) continue;
    const Node& node = p.node(n);
    const AtomSignature& s = atomSignature(node.atom);
    const bool isChannel = s.name == "channel", isPack = s.name == "pack";
    const uint32_t inBegin = static_cast<uint32_t>(ins_.size());
    for (uint32_t i = 0; i < node.numInputs; ++i) {
      InSrc src;
      src.port = driverPort(n, i);
      src.def = inputDefault(p, n, i);
      src.width = src.port != kNone ? widthOf(typed, src.port) : 1;
      src.signal = src.port != kNone && sig[src.port] != 0 ? 1 : 0;
      src.toSamples = isDuration(node.atom, i) && !durationsInSamples_ && !smpLiteral(src.port) ? 1 : 0;
      ins_.push_back(src);
    }
    uint16_t width = 1;
    for (uint32_t o = 0; o < node.numOutputs; ++o) width = std::max(width, widthOf(typed, p.flatOutput(n, o)));
    // `channel`'s index: a const (infer()); read from its literal.
    uint32_t channelIndex = 0;
    if (isChannel) {
      const uint32_t d = node.numInputs > 1 ? driverPort(n, 1) : kNone;
      if (d != kNone && p.node(owner[d]).kind != NodeKind::Const) return;  // Unavailable: not a literal index
      channelIndex = d != kNone ? static_cast<uint32_t>(std::max(0.0, p.constant(owner[d]).value())) : 0u;
    }
    for (uint16_t c = 0; c < width; ++c) {
      Cell cell;
      cell.rule = s.response;
      cell.rate = rateOf(n);
      usedRates_ |= static_cast<uint8_t>(1u << cell.rate);
      cell.inBegin = inBegin;
      cell.inCount = node.numInputs;
      cell.numOutputs = node.numOutputs;
      cell.channel = c;
      cell.gainBase = static_cast<uint32_t>(gain_.size());
      gain_.resize(gain_.size() + static_cast<size_t>(node.numOutputs) * node.numInputs);
      cell.argBase = argCount;
      argCount += node.numInputs;
      cell.cacheBase = static_cast<uint32_t>(cacheCount);  // EQ.1b: the rule's scratch for one evaluate
      cacheCount += (s.responseCacheBytes + sizeof(double) - 1) / sizeof(double);
      // CPU: z1 is the commonest atom whose gain depends on the frequency; its term reads the one shared slot that
      // holds z (set once per point) instead of calling the rule per point.
      cell.zOnly = s.response == &rules::z1;
      cells_.push_back(cell);
      for (uint32_t o = 0; o < node.numOutputs; ++o) {
        const uint32_t f = p.flatOutput(n, o);
        if (rel[f] == 0 || c >= widthOf(typed, f)) continue;
        const uint32_t v = varBase[f] + c;
        uint32_t packOffset = 0;
        for (uint32_t i = 0; i < node.numInputs; ++i) {
          const InSrc& in = ins_[inBegin + i];
          uint32_t ic;
          if (isPack) {
            const uint32_t lo = packOffset;
            packOffset += in.width;
            if (c < lo || c >= packOffset) continue;
            ic = c - lo;
          } else if (isChannel) {
            if (i != 0) continue;
            ic = in.width == 1 ? 0u : std::min<uint32_t>(channelIndex, in.width - 1u);
          } else {
            ic = in.width == 1 ? 0u : std::min<uint32_t>(c, in.width - 1u);
          }
          if (in.signal == 0) continue;
          edges.push_back({v, Term{varBase[in.port] + ic, cell.zOnly ? uint32_t{cell.rate} : cell.gainBase + o * node.numInputs + i}});
        }
      }
    }
  }
  // CSR by unknown.
  termBegin_.assign(vars + 1, 0);
  for (const auto& e : edges) ++termBegin_[e.first + 1];
  for (uint32_t v = 0; v < vars; ++v) termBegin_[v + 1] += termBegin_[v];
  terms_.resize(edges.size());
  {
    std::vector<uint32_t> fill(termBegin_.begin(), termBegin_.end() - 1);
    for (const auto& e : edges) terms_[fill[e.first]++] = e.second;
  }

  // ---- strongly connected components (iterative Tarjan; emitted dependencies first) ----
  constexpr uint32_t kUnset = kNone;
  std::vector<uint32_t> index(vars, kUnset), low(vars, 0), tstack;
  std::vector<uint8_t> onStack(vars, 0);
  std::vector<std::pair<uint32_t, uint32_t>> call;  // (unknown, next term)
  uint32_t counter = 0;
  sccOf_.assign(vars, 0);
  localOf_.assign(vars, 0);
  sccBegin_.push_back(0);
  uint32_t maxLoop = 0;
  for (uint32_t root = 0; root < vars; ++root) {
    if (index[root] != kUnset) continue;
    call.push_back({root, termBegin_[root]});
    index[root] = low[root] = counter++;
    tstack.push_back(root);
    onStack[root] = 1;
    while (!call.empty()) {
      auto& [v, t] = call.back();
      if (t < termBegin_[v + 1]) {
        const uint32_t u = terms_[t++].from;
        if (index[u] == kUnset) {
          index[u] = low[u] = counter++;
          tstack.push_back(u);
          onStack[u] = 1;
          call.push_back({u, termBegin_[u]});
        } else if (onStack[u] != 0) {
          low[v] = std::min(low[v], index[u]);
        }
        continue;
      }
      const uint32_t done = v;
      call.pop_back();
      if (!call.empty()) low[call.back().first] = std::min(low[call.back().first], low[done]);
      if (low[done] != index[done]) continue;
      const uint32_t comp = static_cast<uint32_t>(sccBegin_.size()) - 1u;
      uint32_t w;
      do {
        w = tstack.back();
        tstack.pop_back();
        onStack[w] = 0;
        sccOf_[w] = comp;
        localOf_[w] = static_cast<uint32_t>(sccVars_.size()) - sccBegin_.back();
        sccVars_.push_back(w);
      } while (w != done);
      sccBegin_.push_back(static_cast<uint32_t>(sccVars_.size()));
      const uint32_t size = sccBegin_.back() - sccBegin_[comp];
      bool loop = size > 1;
      if (!loop)
        for (uint32_t k = termBegin_[done]; k < termBegin_[done + 1]; ++k) loop |= terms_[k].from == done;
      sccLoop_.push_back(loop ? 1 : 0);
      if (loop) maxLoop = std::max(maxLoop, size);
    }
  }

  valRe_.assign(static_cast<size_t>(vars) * srcWidth_ * kBatch, 0.0);
  valIm_.assign(valRe_.size(), 0.0);
  gainRe_.assign(gain_.size() * kBatch, 0.0);
  gainIm_.assign(gainRe_.size(), 0.0);
  mat_.assign(static_cast<size_t>(maxLoop) * (maxLoop + srcWidth_), Cx{});
  dynamicCells_.assign(cells_.size(), 0);
  argValue_.assign(argCount, 0.0f);
  cache_.assign(cacheCount, 0.0);
  argKnown_.assign(argCount, 0);
  argSignal_.assign(argCount, 0);
  curve_.resize(curveNode_.size());
  curveSet_.assign(curveNode_.size(), 0);
  prepared_ = ResponseKind::Exact;
}

bool LinearResponse::readControl(const InSrc& s, uint32_t channel, float& v) const noexcept {
  if (s.port == kNone) {
    v = s.def;
    return true;
  }
  const uint32_t off = ctrlOff_[s.port];
  if (off == kNone || ctrlKnown_[s.port] == 0) return false;
  v = ctrlVal_[off + (s.width == 1 ? 0u : std::min<uint32_t>(channel, s.width - 1u))];
  return true;
}

void LinearResponse::runControl(double sampleRate) noexcept {
  for (const CtrlOp& op : ops_) {
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
        known = curveSet_[op.index] != 0 && (op.inCount < 2 || readControl(ins_[op.inBegin + 1], 0, convert));
        for (uint16_t c = 0; known && c < op.width; ++c) {
          float x = 0.0f;
          known = readControl(ins_[op.inBegin], c, x);
          const dsp::Curve& cv = curve_[op.index];
          ctrlVal_[ctrlOff_[f0] + c] = static_cast<float>(convert != 0.0f ? dsp::evalCurve(cv, x) : dsp::evalCurveUnits(cv, x));
        }
        break;
      }
      case OpKind::Channel: {
        float idx = 0.0f, x = 0.0f;
        known = op.inCount >= 1 && (op.inCount < 2 || readControl(ins_[op.inBegin + 1], 0, idx));
        const uint32_t i = std::isfinite(idx) && idx > 0.0f ? static_cast<uint32_t>(idx) : 0u;
        known = known && readControl(ins_[op.inBegin], i, x);
        ctrlVal_[ctrlOff_[f0]] = x;
        break;
      }
      case OpKind::Pack: {
        uint32_t c = 0;
        for (uint32_t k = 0; known && k < op.inCount; ++k) {
          const InSrc& s = ins_[op.inBegin + k];
          for (uint16_t j = 0; known && j < s.width && c < op.width; ++j, ++c) known = readControl(s, j, ctrlVal_[ctrlOff_[f0] + c]);
        }
        break;
      }
      case OpKind::Fold: {
        float in[detail::kMaxFoldPorts], out[detail::kMaxFoldPorts];
        for (uint16_t c = 0; known && c < op.width; ++c) {
          for (uint32_t k = 0; known && k < op.inCount; ++k) known = readControl(ins_[op.inBegin + k], c, in[k]);
          known = known && detail::foldPureAtom(op.atom, in, op.inCount, out, op.outCount);
          for (uint32_t k = 0; known && k < op.outCount; ++k) {
            const uint32_t f = ctrlOuts_[op.outBegin + k];
            ctrlVal_[ctrlOff_[f] + c] = out[k];
          }
        }
        break;
      }
      case OpKind::Unknown: known = false; break;
    }
    for (uint32_t k = 0; k < op.outCount; ++k) ctrlKnown_[ctrlOuts_[op.outBegin + k]] = known ? 1 : 0;
  }
}

ResponseKind LinearResponse::firstCall(uint32_t c, const Point& pt, double sampleRate, bool& dynamic) noexcept {
  const Cell& cell = cells_[c];
  const double cellRate = sampleRate * static_cast<double>(1u << cell.rate);  // (a region's durations count its samples)
  for (uint32_t i = 0; i < cell.inCount; ++i) {
    const InSrc& s = ins_[cell.inBegin + i];
    argSignal_[cell.argBase + i] = s.signal;
    float v = 0.0f;
    const bool known = s.signal == 0 && readControl(s, cell.channel, v);
    argKnown_[cell.argBase + i] = known ? 1 : 0;
    argValue_[cell.argBase + i] = known && s.toSamples != 0 ? static_cast<float>(v * cellRate) : v;
  }
  std::fill_n(gain_.data() + cell.gainBase, static_cast<size_t>(cell.numOutputs) * cell.inCount, Cx{});
  return recall(c, pt, sampleRate, dynamic, true);
}

ResponseKind LinearResponse::recall(uint32_t c, const Point& pt, double sampleRate, bool& dynamic, bool fresh) noexcept {
  const Cell& cell = cells_[c];
  Cx* g = gain_.data() + cell.gainBase;
  ResponseArgs a;
  a.omega = pt.omega[cell.rate];
  a.z = pt.z[cell.rate];
  a.ejw1 = pt.ejw1[cell.rate];
  a.sampleRate = sampleRate * static_cast<double>(1u << cell.rate);
  a.numInputs = cell.inCount;
  a.numOutputs = cell.numOutputs;
  a.value = argValue_.data() + cell.argBase;
  a.known = argKnown_.data() + cell.argBase;
  a.signal = argSignal_.data() + cell.argBase;
  a.gain = g;
  a.cache = cache_.data() + cell.cacheBase;
  a.fresh = fresh;
  const ResponseKind k = cell.rule(a);
  dynamic = a.dependsOnOmega;
  return k;
}

void LinearResponse::solveBatch(uint32_t first, uint32_t count, std::span<Cx> out) noexcept {
  constexpr uint32_t P = kBatch;
  const uint32_t W = srcWidth_;
  const uint32_t comps = static_cast<uint32_t>(sccBegin_.size()) - 1u;
  const uint32_t srcBase = srcVar_.front();
  double* vr = valRe_.data();
  double* vi = valIm_.data();
  const double* gr = gainRe_.data();
  const double* gi = gainIm_.data();
  for (uint32_t comp = 0; comp < comps; ++comp) {
    const uint32_t b = sccBegin_[comp], e = sccBegin_[comp + 1];
    if (sccLoop_[comp] == 0) {
      // CPU: points are the lanes (struct of arrays, kBatch doubles per value), so each term is one vectorised
      // complex multiply-add over the batch.
      const uint32_t v = sccVars_[b];
      for (uint32_t s = 0; s < W; ++s) {
        const double inj = v == srcBase + s ? 1.0 : 0.0;
        alignas(64) double yr[P], yi[P];  // local accumulators: no aliasing with the inputs, kept in registers
        for (uint32_t p = 0; p < P; ++p) {
          yr[p] = inj;
          yi[p] = 0.0;
        }
        for (uint32_t t = termBegin_[v]; t < termBegin_[v + 1]; ++t) {
          const double* ar = gr + static_cast<size_t>(terms_[t].gain) * P;
          const double* ai = gi + static_cast<size_t>(terms_[t].gain) * P;
          const double* xr = vr + (static_cast<size_t>(terms_[t].from) * W + s) * P;
          const double* xi = vi + (static_cast<size_t>(terms_[t].from) * W + s) * P;
          for (uint32_t p = 0; p < P; ++p) {
            yr[p] += ar[p] * xr[p] - ai[p] * xi[p];
            yi[p] += ar[p] * xi[p] + ai[p] * xr[p];
          }
        }
        std::copy_n(yr, P, vr + (static_cast<size_t>(v) * W + s) * P);
        std::copy_n(yi, P, vi + (static_cast<size_t>(v) * W + s) * P);
      }
      continue;
    }
    // A loop: (I − A) y = r over its k unknowns, r from what is already known; per point.
    const uint32_t k = e - b, cols = k + W;
    for (uint32_t p = 0; p < count; ++p) {
      Cx* m = mat_.data();
      std::fill_n(m, static_cast<size_t>(k) * cols, Cx{});
      for (uint32_t a = 0; a < k; ++a) {
        Cx* row = m + static_cast<size_t>(a) * cols;
        row[a] = 1.0;
        const uint32_t v = sccVars_[b + a];
        for (uint32_t t = termBegin_[v]; t < termBegin_[v + 1]; ++t) {
          const uint32_t u = terms_[t].from;
          const size_t gs = static_cast<size_t>(terms_[t].gain) * P + p;
          const Cx g{gr[gs], gi[gs]};
          if (sccOf_[u] == comp) {
            row[localOf_[u]] -= g;
          } else {
            for (uint32_t s = 0; s < W; ++s) {
              const size_t xs = (static_cast<size_t>(u) * W + s) * P + p;
              row[k + s] += g * Cx{vr[xs], vi[xs]};
            }
          }
        }
      }
      bool singular = false;
      for (uint32_t col = 0; col < k && !singular; ++col) {
        uint32_t piv = col;
        double best = std::abs(m[static_cast<size_t>(col) * cols + col]);
        for (uint32_t r = col + 1; r < k; ++r) {
          const double a = std::abs(m[static_cast<size_t>(r) * cols + col]);
          if (a > best) {
            best = a;
            piv = r;
          }
        }
        if (!(best > 1e-300)) {
          singular = true;
          break;
        }
        if (piv != col)
          std::swap_ranges(m + static_cast<size_t>(piv) * cols, m + static_cast<size_t>(piv + 1) * cols,
                           m + static_cast<size_t>(col) * cols);
        const Cx* prow = m + static_cast<size_t>(col) * cols;
        const Cx inv = 1.0 / prow[col];
        for (uint32_t r = col + 1; r < k; ++r) {
          Cx* row = m + static_cast<size_t>(r) * cols;
          const Cx f = row[col] * inv;
          if (f == Cx{}) continue;
          for (uint32_t j = col; j < cols; ++j) row[j] -= f * prow[j];
        }
      }
      for (uint32_t a = k; a-- > 0;) {
        const uint32_t v = sccVars_[b + a];
        const Cx* row = m + static_cast<size_t>(a) * cols;
        for (uint32_t s = 0; s < W; ++s) {
          Cx y{std::numeric_limits<double>::infinity(), 0.0};
          if (!singular) {
            Cx acc = row[k + s];
            for (uint32_t j = a + 1; j < k; ++j) {
              const size_t xs = (static_cast<size_t>(sccVars_[b + j]) * W + s) * P + p;
              acc -= row[j] * Cx{vr[xs], vi[xs]};
            }
            y = acc / row[a];
          }
          const size_t ys = (static_cast<size_t>(v) * W + s) * P + p;
          vr[ys] = y.real();
          vi[ys] = y.imag();
        }
      }
    }
  }
  const uint32_t S = sinkWidth_;
  for (uint32_t p = 0; p < count; ++p)
    for (uint32_t s = 0; s < W; ++s)
      for (uint32_t j = 0; j < S; ++j) {
        const size_t xs = (static_cast<size_t>(sinkVar_[j]) * W + s) * P + p;
        out[(static_cast<size_t>(first + p) * W + s) * S + j] = Cx{vr[xs], vi[xs]};
      }
}

void LinearResponse::storeGains(uint32_t c, uint32_t p) noexcept {
  const Cell& cell = cells_[c];
  const uint32_t n = cell.numOutputs * cell.inCount;
  for (uint32_t i = 0; i < n; ++i) {
    const Cx g = gain_[cell.gainBase + i];
    gainRe_[static_cast<size_t>(cell.gainBase + i) * kBatch + p] = g.real();
    gainIm_[static_cast<size_t>(cell.gainBase + i) * kBatch + p] = g.imag();
  }
}

ResponseKind LinearResponse::evaluate(std::span<const double> hz, double sampleRate, std::span<Cx> out) noexcept {
  const size_t per = static_cast<size_t>(srcWidth_) * sinkWidth_;
  const size_t need = hz.size() * per;
  if (out.size() < need) return ResponseKind::Unavailable;
  auto fill = [&](Cx v) { std::fill_n(out.begin(), need, v); };
  const Cx nan{std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
  if (prepared_ == ResponseKind::Unavailable) {
    fill(nan);
    return ResponseKind::Unavailable;
  }
  if (zero_ || identity_) {
    fill(Cx{});
    if (identity_)
      for (size_t k = 0; k < hz.size(); ++k)
        for (uint32_t s = 0; s < std::min(srcWidth_, sinkWidth_); ++s)
          out[k * per + static_cast<size_t>(s) * sinkWidth_ + s] = 1.0;
    return ResponseKind::Exact;
  }
  if (hz.empty()) return prepared_;
  runControl(sampleRate);
  const double toOmega = 2.0 * std::numbers::pi / sampleRate;
  ResponseKind status = ResponseKind::Exact;
  dynamicCount_ = 0;
  // Each rate's omega, z and e^jw - 1 at a point (rate r: an oversample region at x2^r sees omega / 2^r).
  auto pointAt = [&](double w, Point& pt) {
    for (uint32_t r = 0; r < kRates; ++r) {
      if ((usedRates_ >> r & 1u) == 0) continue;
      const double wr = w / static_cast<double>(1u << r);
      pt.omega[r] = wr;
      pt.z[r] = std::polar(1.0, -wr);
      pt.ejw1[r] = dsp::svfEjwMinus1(wr);
    }
  };
  Point p0;
  pointAt(hz[0] * toOmega, p0);
  for (uint32_t c = 0; c < cells_.size(); ++c) {
    bool dyn = false;
    status = worse(status, firstCall(c, p0, sampleRate, dyn));
    if (cells_[c].zOnly) continue;
    if (dyn) dynamicCells_[dynamicCount_++] = c;
    else
      for (uint32_t p = 0; p < kBatch; ++p) storeGains(c, p);  // static: the same gains at every point
  }
  if (status == ResponseKind::Unavailable) {
    fill(nan);
    return status;
  }
  const uint32_t n = static_cast<uint32_t>(hz.size());
  for (uint32_t first = 0; first < n; first += kBatch) {
    const uint32_t count = std::min(kBatch, n - first);
    for (uint32_t p = 0; p < kBatch; ++p) {
      const uint32_t k = first + std::min(p, count - 1u);  // a partial batch repeats its last point
      Point pt;
      if (k == 0) pt = p0;
      else pointAt(hz[k] * toOmega, pt);  // (e^jw - 1 once per point and rate: the filters' rules share it)
      for (uint32_t r = 0; r < kRates; ++r) {  // slots 0 ... kRates - 1: z at each rate (the z1 terms)
        gainRe_[static_cast<size_t>(r) * kBatch + p] = pt.z[r].real();
        gainIm_[static_cast<size_t>(r) * kBatch + p] = pt.z[r].imag();
      }
      bool dyn = false;
      for (uint32_t d = 0; d < dynamicCount_; ++d) {
        status = worse(status, recall(dynamicCells_[d], pt, sampleRate, dyn));
        storeGains(dynamicCells_[d], p);
      }
    }
    solveBatch(first, count, out);
  }
  if (status == ResponseKind::Unavailable) fill(nan);
  return status;
}

ResponseKind linearResponse(const TypedPatch& typed, ResponseEnd source, ResponseEnd sink, std::span<const double> hz,
                            double sampleRate, std::vector<Cx>& out, std::span<const std::pair<NodeIndex, float>> params) {
  LinearResponse r;
  r.prepare(typed, source, sink);
  for (const auto& [node, v] : params)
    for (uint32_t i = 0; i < r.paramNodes().size(); ++i)
      if (r.paramNodes()[i] == node) r.setParam(i, v);
  out.assign(hz.size() * r.sourceChannels() * r.sinkChannels(), Cx{});
  return r.evaluate(hz, sampleRate, out);
}

}  // namespace pq::graph
