// Structural diff of a re-parsed patch against the previous parse: NodeIds survive text edits (id_match.h).

#include "pq/graph/id_match.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <string_view>
#include <utility>
#include <vector>

namespace pq::graph {

namespace {

constexpr uint64_t mix(uint64_t h, uint64_t v) noexcept {  // splitmix64 finaliser over a running hash
  uint64_t z = h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

uint64_t hashString(uint64_t h, std::string_view s) noexcept {  // FNV-1a, then one mix
  uint64_t f = 0xCBF29CE484222325ull;
  for (char c : s) f = (f ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
  return mix(h, f ^ s.size());
}

// One patch and its matching state.
struct Side {
  const Patch& p;
  Adjacency adj;
  std::vector<uint64_t> nameHash;      // per node (0 = unnamed)
  std::vector<NodeIndex> nameTable;    // open addressing over the named nodes (kNone = empty), power-of-2 size
  std::vector<uint32_t> match;         // partner on the other side, or kNone
  std::vector<uint64_t> strongHash, weakHash;  // label hashes (names excluded)

  explicit Side(const Patch& patch) : p(patch), adj(patch.adjacency()) {
    const uint32_t n = p.nodeCount();
    match.assign(n, kNone);
    strongHash.resize(n);
    weakHash.resize(n);
    nameHash.assign(n, 0);
    nameTable.assign(std::bit_ceil(2u * n + 2u), kNone);
    const size_t mask = nameTable.size() - 1;
    for (NodeIndex v = 0; v < n; ++v) {
      if (!p.nodeName(v).empty()) {
        nameHash[v] = hashString(0x4E414D45ull, p.nodeName(v)) | 1u;
        size_t k = nameHash[v] & mask;
        while (nameTable[k] != kNone) k = (k + 1) & mask;
        nameTable[k] = v;
      }
      const Node& nd = p.node(v);
      uint64_t w = static_cast<uint64_t>(nd.kind) * 0x9E3779B97F4A7C15ull;
      if (nd.kind == NodeKind::Subcircuit) w = hashString(w, p.subcircuit(v).patch);
      weakHash[v] = w;
      uint64_t s = w ^ ((static_cast<uint64_t>(nd.numInputs) << 48) | (static_cast<uint64_t>(nd.numOutputs) << 32) | nd.atom);
      if (nd.kind == NodeKind::Const) {
        const Constant& c = p.constant(v);
        s = mix(s, std::bit_cast<uint64_t>(c.literal) ^ ((static_cast<uint64_t>(c.kind) << 8) | static_cast<uint64_t>(c.unit)));
      }
      strongHash[v] = mix(s, 0x5354ull);
    }
  }

  // The node named `s` (whose name hash is h), or kNone.
  NodeIndex findName(std::string_view s, uint64_t h) const noexcept {
    const size_t mask = nameTable.size() - 1;
    for (size_t k = h & mask; nameTable[k] != kNone; k = (k + 1) & mask)
      if (nameHash[nameTable[k]] == h && p.nodeName(nameTable[k]) == s) return nameTable[k];
    return kNone;
  }
  PortRef driver(NodeIndex v, uint32_t i) const noexcept {
    const WireIndex w = adj.driver[p.flatInput(v, i)];
    return w == kNone ? PortRef{kNone, 0} : p.wire(w).from;
  }
};

enum class Mode : uint8_t { Strong, Weak };

class Matcher {
public:
  Matcher(const Patch& previous, const Patch& next) : A_(previous), B_(next) {}

  IdMatchStats run() {
    stats_.nodes = B_.p.nodeCount();
    stats_.previousNodes = A_.p.nodeCount();
    pairByName();
    settle(false);
    for (int round = 0; round < kHashPasses && pairByHash(false) > 0; ++round) settle(false);
    settle(true);
    for (int round = 0; round < kHashPasses && pairByHash(false) > 0; ++round) settle(true);
    if (pairByHash(true) > 0) settle(true);
    return stats_;
  }

  // The ids for `next`: its partner's id, or a fresh one above every id `previous` handed out.
  std::vector<NodeId> ids() const {
    std::vector<NodeId> out(B_.p.nodeCount());
    uint32_t fresh = std::max<uint32_t>(A_.p.nextId().value, 1);
    for (NodeIndex b = 0; b < out.size(); ++b)
      out[b] = B_.match[b] != kNone ? A_.p.node(B_.match[b]).id : NodeId{fresh++};
    return out;
  }

private:
  static constexpr int kHashPasses = 8;
  static constexpr int kHashRounds = 4;
  enum class Why : uint8_t { Name, Structure, Hash };

  Side A_, B_;
  std::vector<NodeIndex> strongQ_, weakQ_;  // paired A nodes, processed once per mode
  size_t strongHead_ = 0, weakHead_ = 0;
  IdMatchStats stats_;
  std::vector<std::pair<uint64_t, NodeIndex>> scratchA_, scratchB_;
  std::vector<uint64_t> colA_, colB_, tmp_;

  // ---- labels ----------------------------------------------------------------------------------------------
  bool sameLabel(NodeIndex a, NodeIndex b) const noexcept {
    if (A_.strongHash[a] != B_.strongHash[b]) return false;
    const Node& x = A_.p.node(a);
    const Node& y = B_.p.node(b);
    if (x.kind != y.kind || x.numInputs != y.numInputs || x.numOutputs != y.numOutputs) return false;
    switch (x.kind) {
      case NodeKind::Atom: return x.atom == y.atom;
      case NodeKind::Const: return A_.p.constant(a) == B_.p.constant(b);
      case NodeKind::Subcircuit: return A_.p.subcircuit(a).patch == B_.p.subcircuit(b).patch;
      default: return true;
    }
  }
  bool weakLabel(NodeIndex a, NodeIndex b) const noexcept {
    const Node& x = A_.p.node(a);
    const Node& y = B_.p.node(b);
    if (x.kind != y.kind) return false;
    return x.kind != NodeKind::Subcircuit || A_.p.subcircuit(a).patch == B_.p.subcircuit(b).patch;
  }
  bool compatible(Mode m, NodeIndex a, NodeIndex b) const noexcept {
    return m == Mode::Strong ? sameLabel(a, b) : weakLabel(a, b);
  }
  // Names never contradict a pair: equal, or a name that disappeared paired with one that appeared (a rename).
  bool namesAllow(NodeIndex a, NodeIndex b) const noexcept {
    const std::string& na = A_.p.nodeName(a);
    const std::string& nb = B_.p.nodeName(b);
    if (na == nb) return true;
    return (na.empty() || B_.findName(na, A_.nameHash[a]) == kNone) && (nb.empty() || A_.findName(nb, B_.nameHash[b]) == kNone);
  }
  bool unpaired(NodeIndex a, NodeIndex b) const noexcept { return A_.match[a] == kNone && B_.match[b] == kNone; }
  // Every node of the smaller patch is paired: nothing is left to pair.
  bool done() const noexcept { return stats_.matched == std::min(A_.p.nodeCount(), B_.p.nodeCount()); }

  void pair(NodeIndex a, NodeIndex b, Why why) {
    A_.match[a] = b;
    B_.match[b] = a;
    strongQ_.push_back(a);
    weakQ_.push_back(a);
    ++stats_.matched;
    if (!sameLabel(a, b)) ++stats_.weak;
    else if (why == Why::Name) ++stats_.byName;
    else if (why == Why::Hash) ++stats_.byHash;
    else ++stats_.byStructure;
  }

  // Ports of a pair with the same label correspond by index (the same atom, or the same kind with the same port
  // count; a subcircuit's callee may have reordered its ports, so subcircuits always go by name).
  bool portsByIndex(NodeIndex a, NodeIndex b) const noexcept {
    return A_.p.node(a).kind != NodeKind::Subcircuit && sameLabel(a, b);
  }
  // b's port that corresponds to a's input (output) port i: the same index when the names agree (variadic ports
  // share a name, so equal atoms map by index), else the port of b with a's port name.
  uint32_t corrInput(NodeIndex a, uint32_t i, NodeIndex b) const {
    const std::string_view name = A_.p.inputName(a, i);
    if (i < B_.p.node(b).numInputs && B_.p.inputName(b, i) == name) return i;
    return B_.p.findInput(b, name).value_or(kNone);
  }
  uint32_t corrOutput(NodeIndex a, uint32_t i, NodeIndex b) const {
    const std::string_view name = A_.p.outputName(a, i);
    if (i < B_.p.node(b).numOutputs && B_.p.outputName(b, i) == name) return i;
    return B_.p.findOutput(b, name).value_or(kNone);
  }

  // ---- step 1 ----------------------------------------------------------------------------------------------
  void pairByName() {
    for (NodeIndex b = 0; b < B_.p.nodeCount(); ++b) {
      const std::string& name = B_.p.nodeName(b);
      if (name.empty()) continue;
      const NodeIndex a = A_.findName(name, B_.nameHash[b]);
      if (a != kNone && unpaired(a, b) && weakLabel(a, b)) pair(a, b, Why::Name);
    }
    // Equations have no names: the k-th equation of solve block s pairs with the k-th of block s.
    std::vector<std::pair<uint32_t, NodeIndex>> ea, eb;
    for (NodeIndex a = 0; a < A_.p.nodeCount(); ++a)
      if (A_.p.node(a).kind == NodeKind::Equation) ea.emplace_back(A_.p.node(a).payload, a);
    for (NodeIndex b = 0; b < B_.p.nodeCount(); ++b)
      if (B_.p.node(b).kind == NodeKind::Equation) eb.emplace_back(B_.p.node(b).payload, b);
    std::stable_sort(ea.begin(), ea.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    std::stable_sort(eb.begin(), eb.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (size_t i = 0, j = 0; i < ea.size() && j < eb.size();) {
      if (ea[i].first < eb[j].first) ++i;
      else if (eb[j].first < ea[i].first) ++j;
      else pair(ea[i++].second, eb[j++].second, Why::Name);
    }
  }

  // ---- steps 2 and 4 ---------------------------------------------------------------------------------------
  void settle(bool weak) {
    while (true) {
      while (strongHead_ < strongQ_.size()) expand(strongQ_[strongHead_++], Mode::Strong);
      if (!weak || weakHead_ == weakQ_.size()) return;
      expand(weakQ_[weakHead_++], Mode::Weak);
    }
  }

  void expand(NodeIndex a, Mode m) {
    if (done()) return;
    const NodeIndex b = A_.match[a];
    const Why why = Why::Structure;
    const bool byIndex = portsByIndex(a, b);
    // Down: input i's driver ↔ the corresponding input's driver.
    for (uint32_t i = 0; i < A_.p.node(a).numInputs; ++i) {
      const PortRef da = A_.driver(a, i);
      if (da.node == kNone || A_.match[da.node] != kNone) continue;
      const uint32_t j = byIndex ? i : corrInput(a, i, b);
      if (j == kNone) continue;
      const PortRef db = B_.driver(b, j);
      if (db.node == kNone || B_.match[db.node] != kNone) continue;
      if (!compatible(m, da.node, db.node)) continue;
      const uint32_t port = portsByIndex(da.node, db.node) ? da.port : corrOutput(da.node, da.port, db.node);
      if (port == db.port && namesAllow(da.node, db.node)) pair(da.node, db.node, why);
    }
    // Up: consumer edges of output o, keyed by (consumer label, input port); keys unique on both sides pair.
    for (uint32_t o = 0; o < A_.p.node(a).numOutputs; ++o) {
      const uint32_t ob = byIndex ? o : corrOutput(a, o, b);
      if (ob == kNone) continue;
      collectConsumers(A_, a, o, m, scratchA_);
      if (scratchA_.empty()) continue;
      collectConsumers(B_, b, ob, m, scratchB_);
      if (scratchB_.empty()) continue;
      size_t i = 0, j = 0;
      while (i < scratchA_.size() && j < scratchB_.size()) {
        const uint64_t ka = scratchA_[i].first, kb = scratchB_[j].first;
        if (ka < kb) {
          while (i < scratchA_.size() && scratchA_[i].first == ka) ++i;
          continue;
        }
        if (kb < ka) {
          while (j < scratchB_.size() && scratchB_[j].first == kb) ++j;
          continue;
        }
        size_t ie = i, je = j;
        while (ie < scratchA_.size() && scratchA_[ie].first == ka) ++ie;
        while (je < scratchB_.size() && scratchB_[je].first == ka) ++je;
        if (ie - i == 1 && je - j == 1) {
          const NodeIndex ca = scratchA_[i].second, cb = scratchB_[j].second;
          if (unpaired(ca, cb) && compatible(m, ca, cb) && namesAllow(ca, cb)) pair(ca, cb, why);
        }
        i = ie;
        j = je;
      }
    }
  }

  // Unpaired consumers of (v, output o) as (key, node), sorted by key. Strong keys: the label and the input
  // index; weak keys: the kind (+ callee) and the input's name.
  static void collectConsumers(const Side& s, NodeIndex v, uint32_t o, Mode m,
                               std::vector<std::pair<uint64_t, NodeIndex>>& out) {
    out.clear();
    for (WireIndex w : s.adj.outgoing(v)) {
      const Wire& wr = s.p.wire(w);
      if (wr.from.port != o) continue;
      const NodeIndex c = wr.to.node;
      if (s.match[c] != kNone) continue;
      const uint64_t key = m == Mode::Strong ? mix(s.strongHash[c], wr.to.port)
                                             : hashString(s.weakHash[c], s.p.inputName(c, wr.to.port));
      out.emplace_back(key, c);
    }
    std::sort(out.begin(), out.end());
  }

  // ---- step 3 ----------------------------------------------------------------------------------------------
  // Hashes over inputs (paired nodes hashed as their pair, fixed); pairs nodes whose hash is unique among the
  // unpaired nodes of both sides (with `ties`, equal-hash groups pair in index order). Returns the pairs made.
  uint32_t pairByHash(bool ties) {
    const uint32_t before = stats_.matched;
    if (done()) return 0;
    hashInputs(A_, false, colA_);
    hashInputs(B_, true, colB_);
    auto gather = [](const Side& s, const std::vector<uint64_t>& col, std::vector<std::pair<uint64_t, NodeIndex>>& out) {
      out.clear();
      for (NodeIndex v = 0; v < s.p.nodeCount(); ++v)
        if (s.match[v] == kNone) out.emplace_back(col[v], v);
      std::sort(out.begin(), out.end());
    };
    gather(A_, colA_, scratchA_);
    gather(B_, colB_, scratchB_);
    size_t i = 0, j = 0;
    while (i < scratchA_.size() && j < scratchB_.size()) {
      const uint64_t ka = scratchA_[i].first, kb = scratchB_[j].first;
      if (ka != kb) {
        if (ka < kb) ++i;
        else ++j;
        continue;
      }
      size_t ie = i, je = j;
      while (ie < scratchA_.size() && scratchA_[ie].first == ka) ++ie;
      while (je < scratchB_.size() && scratchB_[je].first == ka) ++je;
      if (ties || (ie - i == 1 && je - j == 1)) {
        for (size_t x = i, y = j; x < ie && y < je; ++x, ++y) {
          const NodeIndex a = scratchA_[x].second, b = scratchB_[y].second;
          if (unpaired(a, b) && sameLabel(a, b) && namesAllow(a, b)) pair(a, b, Why::Hash);
        }
      }
      i = ie;
      j = je;
    }
    return stats_.matched - before;
  }

  void hashInputs(const Side& s, bool isNext, std::vector<uint64_t>& col) {
    const uint32_t n = s.p.nodeCount();
    col.resize(n);
    for (NodeIndex v = 0; v < n; ++v) {
      const uint32_t partner = s.match[v];
      col[v] = partner == kNone ? s.strongHash[v] : mix(0x9A17ull, isNext ? partner : v);  // a pair: A's index
    }
    for (int r = 0; r < kHashRounds; ++r) {
      tmp_ = col;
      for (NodeIndex v = 0; v < n; ++v) {
        if (s.match[v] != kNone) continue;
        uint64_t h = col[v];
        for (uint32_t i = 0; i < s.p.node(v).numInputs; ++i) {
          const PortRef d = s.driver(v, i);
          h = d.node == kNone ? mix(h, 0x6E6F6E65ull) : mix(mix(h, col[d.node]), d.port);
        }
        tmp_[v] = h;
      }
      col.swap(tmp_);
    }
  }
};

}  // namespace

IdMatchStats inheritNodeIds(Patch& next, const Patch& previous) {
  Matcher m(previous, next);
  const IdMatchStats stats = m.run();
  const std::vector<NodeId> ids = m.ids();
  [[maybe_unused]] const bool ok = next.setNodeIds(ids);
  assert(ok && "inheritNodeIds: ids are distinct by construction");
  return stats;
}

IdMatchStats inheritNodeIds(PatchFile& next, const PatchFile& previous) {
  IdMatchStats total;
  for (Patch& p : next.patches)
    if (const Patch* prev = previous.find(p.name())) total += inheritNodeIds(p, *prev);
  return total;
}

}  // namespace pq::graph
