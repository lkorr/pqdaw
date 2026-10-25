// Structural equality of patches (see Patch::operator== in patch.h).
//
// Both patches are put in a canonical node order, then compared position by position; wires are compared as sorted
// lists of canonical endpoints. The canonical order depends only on structure, never on NodeIds or dense order:
//   1. declarations in their declared order: params, then ins, then outs, then exports;
//   2. solve members: unknowns, then equations, each in their order;
//   3. other named nodes, sorted by name;
//   4. depth-first (pre-order) from each of the above through input wires (input 0 first), which reaches every
//      anonymous node that feeds something named, declared or solved;
//   5. what is left is anonymous dead code (nothing named or declared consumes it; text cannot express this, but
//      editors and passes can produce it). Its sinks (nodes with no consumers) are sorted by a structural hash —
//      a Merkle hash over input wires, where an already-ordered driver contributes its canonical position — and
//      walked depth-first in that order;
//   6. anything still left (dead feedback loops with no sink), in index order.
// If the comparison succeeds, the order pairs every node with an equal node and every wire with an equal wire, so
// the patches are isomorphic: equality never reports a false "equal". Equal patches produce corresponding orders
// because steps 1–5 use only compared properties; the exceptions, which can only make equal patches compare
// unequal, are 64-bit hash collisions and symmetric ties in step 5, and dead sink-less loops in step 6.
//
// Cost: O(nodes + wires·log wires), a few flat arrays.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <vector>

#include "pq/graph/patch.h"

namespace pq::graph {

namespace {

constexpr uint64_t mix(uint64_t h, uint64_t v) noexcept {  // splitmix64 finaliser over a running hash
  uint64_t z = h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

uint64_t hashString(uint64_t h, std::string_view s) noexcept {
  h = mix(h, s.size());
  for (char c : s) h = mix(h, static_cast<unsigned char>(c));
  return h;
}

// Hash of everything sameNode() compares (anonymous dead nodes are atoms, consts or subcircuits; their params and
// subcircuit port lists are covered by kind/name/port types closely enough for ordering purposes).
uint64_t labelHash(const Patch& p, NodeIndex v) noexcept {
  const Node& nd = p.node(v);
  uint64_t h = mix(static_cast<uint64_t>(nd.kind), nd.atom);
  h = mix(h, (static_cast<uint64_t>(nd.numInputs) << 32) | (static_cast<uint64_t>(nd.numOutputs) << 16) | nd.nameOutput);
  h = mix(h, nd.shared ? 1u : 0u);
  for (uint32_t k = 0, e = static_cast<uint32_t>(nd.numInputs) + nd.numOutputs; k < e; ++k) {
    const PortType& t = p.portTypes()[nd.firstPort + k];
    h = mix(h, (static_cast<uint64_t>(t.kind) << 40) | (static_cast<uint64_t>(t.rate) << 32) |
                   (static_cast<uint64_t>(t.poly) << 24) | t.width);
  }
  if (nd.kind == NodeKind::Const) {
    const Constant& c = p.constant(v);
    h = mix(h, std::bit_cast<uint64_t>(c.literal));
    h = mix(h, (static_cast<uint64_t>(c.kind) << 8) | static_cast<uint64_t>(c.unit));
  } else if (nd.kind == NodeKind::Subcircuit) {
    h = hashString(h, p.subcircuit(v).patch);
  }
  const NodeLayout& l = p.layout(v);
  if (l.placed) h = mix(mix(h, std::bit_cast<uint32_t>(l.x)), std::bit_cast<uint32_t>(l.y));
  return h;
}

std::vector<NodeIndex> canonicalOrder(const Patch& p, const Adjacency& adj) {
  const uint32_t n = p.nodeCount();
  std::vector<NodeIndex> order;
  order.reserve(n);
  std::vector<uint8_t> seen(n, 0);
  std::vector<NodeIndex> stack;

  auto walk = [&](NodeIndex root) {
    stack.push_back(root);
    while (!stack.empty()) {
      const NodeIndex v = stack.back();
      stack.pop_back();
      if (seen[v]) continue;
      seen[v] = 1;
      order.push_back(v);
      const Node& nd = p.node(v);
      for (uint32_t i = nd.numInputs; i-- > 0;) {
        const WireIndex w = adj.driver[nd.firstPort + i];
        if (w != kNone) stack.push_back(p.wire(w).from.node);
      }
    }
  };

  std::vector<NodeIndex> roots;
  roots.reserve(n);
  constexpr NodeKind kRootKinds[] = {NodeKind::Param, NodeKind::Input,   NodeKind::Output,
                                     NodeKind::Export, NodeKind::Unknown, NodeKind::Equation};
  for (NodeKind k : kRootKinds)
    for (NodeIndex v = 0; v < n; ++v)
      if (p.node(v).kind == k) roots.push_back(v);
  const size_t namedBegin = roots.size();
  for (NodeIndex v = 0; v < n; ++v) {
    const NodeKind k = p.node(v).kind;
    const bool isRootKind = std::find(std::begin(kRootKinds), std::end(kRootKinds), k) != std::end(kRootKinds);
    if (!isRootKind && !p.nodeName(v).empty()) roots.push_back(v);
  }
  std::sort(roots.begin() + static_cast<std::ptrdiff_t>(namedBegin), roots.end(),
            [&](NodeIndex a, NodeIndex b) { return p.nodeName(a) < p.nodeName(b); });
  for (NodeIndex r : roots) walk(r);
  if (order.size() == n) return order;

  // Step 5: Merkle hashes of the leftover nodes over their input wires, by iterative post-order DFS. A driver that
  // is already ordered contributes its canonical position; a driver on the current DFS path (a dead loop)
  // contributes a fixed marker.
  std::vector<uint32_t> rank(n, kNone);
  for (uint32_t k = 0; k < order.size(); ++k) rank[order[k]] = k;
  std::vector<uint64_t> hash(n, 0);
  std::vector<uint8_t> state(n, 0);  // 0 new, 1 on path, 2 done
  std::vector<std::pair<NodeIndex, uint32_t>> dfs;  // (node, next input)
  auto driverOf = [&](NodeIndex v, uint32_t i) {
    const WireIndex w = adj.driver[p.node(v).firstPort + i];
    return w == kNone ? PortRef{kNone, 0} : p.wire(w).from;
  };
  for (NodeIndex root = 0; root < n; ++root) {
    if (seen[root] || state[root]) continue;
    dfs.emplace_back(root, 0);
    state[root] = 1;
    while (!dfs.empty()) {
      auto& [v, next] = dfs.back();
      if (next < p.node(v).numInputs) {
        const NodeIndex d = driverOf(v, next++).node;
        if (d != kNone && !seen[d] && state[d] == 0) {
          state[d] = 1;
          dfs.emplace_back(d, 0);
        }
        continue;
      }
      uint64_t h = labelHash(p, v);
      for (uint32_t i = 0; i < p.node(v).numInputs; ++i) {
        const auto [d, port] = driverOf(v, i);  // PortRef{node, port}
        if (d == kNone) h = mix(h, 0x6E6F6E65ull);                     // unconnected
        else if (seen[d]) h = mix(mix(h, 0x72616E6Bull + rank[d]), port);  // ordered: its position
        else if (state[d] == 1) h = mix(mix(h, 0x6C6F6F70ull), port);     // dead loop back edge
        else h = mix(mix(h, hash[d]), port);
      }
      hash[v] = h;
      state[v] = 2;
      dfs.pop_back();
    }
  }
  std::vector<NodeIndex> sinks;
  for (NodeIndex v = 0; v < n; ++v)
    if (!seen[v] && adj.outgoing(v).empty()) sinks.push_back(v);
  std::sort(sinks.begin(), sinks.end(),
            [&](NodeIndex x, NodeIndex y) { return hash[x] != hash[y] ? hash[x] < hash[y] : x < y; });
  for (NodeIndex v : sinks) walk(v);
  // Step 6.
  for (NodeIndex v = 0; v < n; ++v) walk(v);
  return order;
}

bool sameNode(const Patch& a, NodeIndex i, const Patch& b, NodeIndex j) {
  const Node& x = a.node(i);
  const Node& y = b.node(j);
  if (x.kind != y.kind || x.numInputs != y.numInputs || x.numOutputs != y.numOutputs ||
      x.nameOutput != y.nameOutput || x.shared != y.shared)
    return false;
  if (a.nodeName(i) != b.nodeName(j) || !(a.layout(i) == b.layout(j))) return false;
  const auto ta = a.portTypes().subspan(x.firstPort, x.numInputs + x.numOutputs);
  const auto tb = b.portTypes().subspan(y.firstPort, y.numInputs + y.numOutputs);
  if (!std::equal(ta.begin(), ta.end(), tb.begin(), tb.end())) return false;
  switch (x.kind) {
    case NodeKind::Atom: return x.atom == y.atom;
    case NodeKind::Const: return a.constant(i) == b.constant(j);
    case NodeKind::Param: return a.paramDecl(i) == b.paramDecl(j);
    case NodeKind::Subcircuit: return a.subcircuit(i) == b.subcircuit(j);
    case NodeKind::Unknown:
    case NodeKind::Equation: return x.payload == y.payload;  // solve blocks are compared in order
    case NodeKind::Input:
    case NodeKind::Output:
    case NodeKind::Export: return true;
  }
  return false;
}

// (from rank, from port, to rank, to port, wire index); out-of-range endpoints keep their raw index offset past n
// so that broken patches still compare deterministically.
using WireKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, WireIndex>;

std::vector<WireKey> wireKeys(const Patch& p, const std::vector<uint32_t>& rank) {
  const uint32_t n = p.nodeCount();
  std::vector<WireKey> keys;
  keys.reserve(p.wireCount());
  auto r = [&](NodeIndex v) { return v < n ? rank[v] : v; };
  for (WireIndex w = 0; w < p.wireCount(); ++w) {
    const Wire& wr = p.wire(w);
    keys.emplace_back(r(wr.from.node), wr.from.port, r(wr.to.node), wr.to.port, w);
  }
  std::sort(keys.begin(), keys.end(), [](const WireKey& x, const WireKey& y) {
    return std::tie(std::get<0>(x), std::get<1>(x), std::get<2>(x), std::get<3>(x)) <
           std::tie(std::get<0>(y), std::get<1>(y), std::get<2>(y), std::get<3>(y));
  });
  return keys;
}

}  // namespace

bool operator==(const Patch& a, const Patch& b) {
  if (a.name() != b.name() || a.nodeCount() != b.nodeCount() || a.wireCount() != b.wireCount() ||
      a.portCount() != b.portCount())
    return false;
  if (!std::ranges::equal(a.solves(), b.solves()) || !std::ranges::equal(a.comments(), b.comments())) return false;

  const std::vector<NodeIndex> oa = canonicalOrder(a, a.adjacency());
  const std::vector<NodeIndex> ob = canonicalOrder(b, b.adjacency());
  const uint32_t n = a.nodeCount();
  std::vector<uint32_t> ra(n), rb(n);
  for (uint32_t k = 0; k < n; ++k) {
    if (!sameNode(a, oa[k], b, ob[k])) return false;
    ra[oa[k]] = k;
    rb[ob[k]] = k;
  }

  const std::vector<WireKey> wa = wireKeys(a, ra), wb = wireKeys(b, rb);
  for (size_t k = 0; k < wa.size(); ++k) {
    if (std::get<0>(wa[k]) != std::get<0>(wb[k]) || std::get<1>(wa[k]) != std::get<1>(wb[k]) ||
        std::get<2>(wa[k]) != std::get<2>(wb[k]) || std::get<3>(wa[k]) != std::get<3>(wb[k]))
      return false;
    if (!std::ranges::equal(a.wireVia(std::get<4>(wa[k])), b.wireVia(std::get<4>(wb[k])))) return false;
  }
  return true;
}

}  // namespace pq::graph
