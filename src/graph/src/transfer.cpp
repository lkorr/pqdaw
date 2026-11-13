// DY.3: the static transfer curve of a memoryless circuit (pq/graph/response.h, StaticTransfer).

#include <algorithm>
#include <limits>

#include "pq/graph/response.h"

namespace pq::graph {

void StaticTransfer::prepare(const TypedPatch& typed, ResponseEnd source, ResponseEnd sink, std::span<const uint32_t> settable) {
  *this = StaticTransfer{};
  const Patch& p = typed.patch;
  if (typed.types.size() != p.portCount()) return;
  Graph g(typed);
  bool srcOk = false, sinkOk = false;
  uint16_t srcWidth = 1;
  const uint32_t srcPort = g.resolve(source, srcOk, srcWidth);
  sinkPort_ = g.resolve(sink, sinkOk, sinkWidth_);
  if (!srcOk || !sinkOk || srcPort == kNone || sinkPort_ == kNone) return;  // Unavailable
  if (srcPort == sinkPort_) {
    identity_ = true;
    prepared_ = ResponseKind::Exact;
    return;
  }
  g.cut(settable, g.owner[srcPort]);
  // Every node between the source and the sink must be computed from its inputs (memoryless): a stateful atom there
  // (z1, a filter, `follow`) has no static curve.
  std::vector<uint8_t> sig, rel, relNode;
  g.signalFrom(srcPort, sig);
  if (sig[sinkPort_] != 0) {
    g.relevantTo(sinkPort_, srcPort, sig, rel, relNode);
    for (NodeIndex n = 0; n < p.nodeCount(); ++n)
      if (relNode[n] != 0 && !g.computes(n)) return;  // Unavailable
  }
  beginControls(g);
  needControl(g, g.owner[sinkPort_], srcPort);
  endControls();
  // CPU: the ops that do not depend on the source (params, their curves, what is computed from them only) run once per
  // evaluate; only the rest run per point. A stable partition keeps the dependency order (a source-independent op
  // reads only source-independent ones).
  std::vector<uint8_t> dep(p.portCount(), 0), opDep(ops_.size(), 0);
  for (size_t i = 0; i < ops_.size(); ++i) {
    const CtrlOp& op = ops_[i];
    bool d = op.kind == OpKind::Source;
    for (uint32_t k = 0; !d && k < op.inCount; ++k) {
      const uint32_t port = ctrlIns_[op.inBegin + k].port;
      d = port != kNone && dep[port] != 0;
    }
    opDep[i] = d ? 1 : 0;
    for (uint32_t k = 0; d && k < op.outCount; ++k) dep[ctrlOuts_[op.outBegin + k]] = 1;
  }
  std::vector<CtrlOp> ordered;
  ordered.reserve(ops_.size());
  for (int pass = 0; pass < 2; ++pass)
    for (size_t i = 0; i < ops_.size(); ++i)
      if (opDep[i] == pass) ordered.push_back(ops_[i]);
  firstDependent_ = static_cast<uint32_t>(std::count(opDep.begin(), opDep.end(), uint8_t{0}));
  ops_ = std::move(ordered);
  prepared_ = ResponseKind::Exact;
}

ResponseKind StaticTransfer::evaluate(std::span<const float> xs, double sampleRate, std::span<float> ys) noexcept {
  ++evaluations_;
  const size_t need = xs.size() * sinkWidth_;
  if (ys.size() < need) return ResponseKind::Unavailable;
  auto fail = [&] {
    std::fill_n(ys.begin(), need, std::numeric_limits<float>::quiet_NaN());
    return ResponseKind::Unavailable;
  };
  if (prepared_ == ResponseKind::Unavailable) return fail();
  if (identity_) {
    for (size_t k = 0; k < xs.size(); ++k) std::fill_n(ys.begin() + static_cast<std::ptrdiff_t>(k * sinkWidth_), sinkWidth_, xs[k]);
    return ResponseKind::Exact;
  }
  for (size_t k = 0; k < xs.size(); ++k) {
    runControl(sampleRate, k == 0 ? 0u : firstDependent_, xs[k]);
    if (!controlKnown(sinkPort_)) return fail();
    for (uint16_t j = 0; j < sinkWidth_; ++j) ys[k * sinkWidth_ + j] = controlValue(sinkPort_, j);
  }
  return ResponseKind::Exact;
}

}  // namespace pq::graph
