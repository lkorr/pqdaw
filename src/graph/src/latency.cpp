// T6.2: reported latency of a typed patch (pq/graph/latency.h) and the atoms' latency rules (latency_rules.h).
#include "pq/graph/latency.h"

#include <algorithm>
#include <cmath>

#include "latency_rules.h"
#include "pq/dsp/oversample.h"

namespace pq::graph {

namespace latency_rules {

double frames(const LatencyArgs& a) noexcept {
  if (a.numInputs < 2 || a.known[1] == 0 || !(a.value[1] >= 0.5)) return 0.0;  // below half a sample: a wire
  return a.value[1] / static_cast<double>(std::max(a.factor, 1u));
}

double converter(const LatencyArgs& a) noexcept {
  if (a.numInputs < 2 || a.known[1] == 0) return 0.0;
  return 0.5 * dsp::os::roundTripLatency(dsp::os::sanitizeFactor(static_cast<float>(a.value[1])));
}

}  // namespace latency_rules

std::vector<double> pathLatency(const TypedPatch& typed) {
  const Patch& p = typed.patch;
  const uint32_t n = p.nodeCount();
  std::vector<double> lat(n, 0.0);
  if (n == 0) return lat;
  const Adjacency adj = p.adjacency();
  std::vector<double> value;
  std::vector<uint8_t> known;
  // What node v itself reports (its atom's rule; the inputs that are literals).
  auto own = [&](NodeIndex v) -> double {
    const Node& nd = p.node(v);
    if (nd.kind != NodeKind::Atom) return 0.0;
    const AtomSignature& sig = atomSignature(nd.atom);
    if (sig.latency == nullptr) return 0.0;
    value.assign(nd.numInputs, 0.0);
    known.assign(nd.numInputs, 0);
    for (uint32_t i = 0; i < nd.numInputs; ++i) {
      const WireIndex w = adj.driver[p.flatInput(v, i)];
      if (w == kNone) continue;
      const NodeIndex d = p.wire(w).from.node;
      if (p.node(d).kind != NodeKind::Const) continue;
      value[i] = p.constant(d).value();
      known[i] = 1;
    }
    LatencyArgs a;
    a.numInputs = nd.numInputs;
    a.value = value.data();
    a.known = known.data();
    a.factor = v < typed.origin.size() ? typed.factorOf(typed.origin[v].region) : 1u;
    return std::max(0.0, sig.latency(a));
  };
  // The latest of v's drivers (those in `comp` skipped when given).
  auto arriving = [&](NodeIndex v, const std::vector<uint32_t>* compOf, uint32_t comp) {
    double in = 0.0;
    for (uint32_t i = 0; i < p.node(v).numInputs; ++i)
      if (const WireIndex w = adj.driver[p.flatInput(v, i)]; w != kNone) {
        const NodeIndex d = p.wire(w).from.node;
        if (compOf == nullptr || (*compOf)[d] != comp) in = std::max(in, lat[d]);
      }
    return in;
  };

  // The strongly connected components (iterative Tarjan over the driver wires), emitted sinks first: evaluated in
  // reverse, every component's drivers outside it are known.
  auto driving = [&](WireIndex w) {
    const Wire& wr = p.wire(w);
    return adj.driver[p.flatInput(wr.to.node, wr.to.port)] == w;  // the input's driver (first wins)
  };
  constexpr uint32_t kUnseen = 0xFFFFFFFFu;
  std::vector<uint32_t> idx(n, kUnseen), low(n, 0);
  std::vector<uint8_t> onStack(n, 0);
  std::vector<NodeIndex> stack, members;
  std::vector<uint32_t> compEnd;  // members[compEnd[k-1] .. compEnd[k]) is component k
  struct Frame {
    NodeIndex v;
    uint32_t edge;
  };
  std::vector<Frame> call;
  members.reserve(n);
  uint32_t counter = 0;
  for (NodeIndex root = 0; root < n; ++root) {
    if (idx[root] != kUnseen) continue;
    idx[root] = low[root] = counter++;
    stack.push_back(root);
    onStack[root] = 1;
    call.push_back({root, 0});
    while (!call.empty()) {
      const NodeIndex v = call.back().v;
      const auto out = adj.outgoing(v);
      if (call.back().edge < out.size()) {
        const WireIndex w = out[call.back().edge++];
        if (!driving(w)) continue;
        const NodeIndex t = p.wire(w).to.node;
        if (idx[t] == kUnseen) {
          idx[t] = low[t] = counter++;
          stack.push_back(t);
          onStack[t] = 1;
          call.push_back({t, 0});
        } else if (onStack[t]) {
          low[v] = std::min(low[v], idx[t]);
        }
        continue;
      }
      call.pop_back();
      if (!call.empty()) low[call.back().v] = std::min(low[call.back().v], low[v]);
      if (low[v] != idx[v]) continue;
      NodeIndex m = kNone;
      do {
        m = stack.back();
        stack.pop_back();
        onStack[m] = 0;
        members.push_back(m);
      } while (m != v);
      compEnd.push_back(static_cast<uint32_t>(members.size()));
    }
  }
  std::vector<uint32_t> compOf(n, 0);
  for (uint32_t k = 0; k < compEnd.size(); ++k)
    for (uint32_t i = k == 0 ? 0u : compEnd[k - 1]; i < compEnd[k]; ++i) compOf[members[i]] = k;
  for (size_t k = compEnd.size(); k-- > 0;) {
    const uint32_t begin = k == 0 ? 0u : compEnd[k - 1], end = compEnd[k];
    if (end - begin == 1) {  // a node off any cycle (a self-loop's own value counts as 0)
      const NodeIndex v = members[begin];
      lat[v] = arriving(v, &compOf, static_cast<uint32_t>(k)) + own(v);
      continue;
    }
    // A cycle: every member is reachable from every entry, and going round adds nothing.
    double entry = 0.0;
    for (uint32_t i = begin; i < end; ++i) entry = std::max(entry, arriving(members[i], &compOf, static_cast<uint32_t>(k)));
    for (uint32_t i = begin; i < end; ++i) lat[members[i]] = entry + own(members[i]);
  }
  return lat;
}

double arrivalLatency(const TypedPatch& typed, std::span<const double> latency, NodeIndex node) {
  const Patch& p = typed.patch;
  if (node >= p.nodeCount() || node >= latency.size()) return 0.0;
  return latency[node];  // an out / export has no latency of its own: what arrives is its driver's
}

}  // namespace pq::graph
