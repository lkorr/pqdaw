// Circuit-diagram layout (T4.3): layered placement, channel routing, simple routing of hand-placed layouts, layout
// carry-over across text edits. See pq/graph/layout.h for the algorithm overview.

#include "pq/graph/layout.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include "pq/graph/patch_file.h"

namespace pq::graph {

namespace {

float snapTo(float v, float grid) noexcept { return grid > 0.0f ? std::round(v / grid) * grid : v; }

// One element of a layer: a node, or a dummy (a wire passing through the layer, zero height).
struct Item {
  NodeIndex node = kNone;
  uint32_t layer = 0;
  float w = 0.0f, h = 0.0f;
  float y = 0.0f;       // top
  uint32_t order = 0;   // index in its layer
};

// A connection between items of adjacent layers (a in layer L, b in layer L + 1) or of one layer (attachments of a
// reversed chain to its source / target), with the pin offsets it attaches at.
struct Link {
  uint32_t a, b;
  float pa, pb;
};

// One net's vertical segment in a channel: its terminals on the channel's left side (layer L's right edge) and right
// side (layer L + 1's left edge).
struct Group {
  std::vector<float> left, right;
  float lo = 0.0f, hi = 0.0f;
  uint32_t track = 0;
};

// The pieces of one net (an output pin and the wires it drives).
struct Net {
  NodeIndex node;
  uint16_t port;
  std::vector<WireIndex> wires;
  uint32_t fwdFirst = 0;              // layer of fwd[0]
  std::vector<uint32_t> fwd;          // dummy items of the forward chain, layers fwdFirst, fwdFirst + 1, …
  uint32_t revFirst = 0;              // layer of rev[0]
  std::vector<uint32_t> rev;          // dummy items of the reversed chain, layers revFirst … source layer
  // Channel groups (index into Layouter::groups_) by channel slot (channel c → slot c + 1), for routing.
  std::unordered_map<uint32_t, uint32_t> groupOf;
};

class Layouter {
public:
  Layouter(Patch& p, const LayoutGeometry& g, const AutoLayoutOptions& o) : p_(p), g_(g), o_(o) {}

  AutoLayoutStats run() {
    const uint32_t n = p_.nodeCount();
    if (n == 0) return stats_;
    adj_ = p_.adjacency();
    findFeedback();
    assignLayers();
    buildItems();
    orderLayers();
    placeVertically();
    route();
    write();
    return stats_;
  }

private:
  // ---- 1. feedback edges -----------------------------------------------------------------------------------------
  // First, inside every strongly connected component, the wires into a delayed input (z1.x, delay.x, phasor.hz: the
  // atom's own memory) are the feedback: the delay then sits at the start of its loop, the loop's wire runs back into
  // it — the way DSP diagrams draw a z⁻¹. Then a DFS from the sources cuts whatever cycles remain.
  void findFeedback() {
    const uint32_t n = p_.nodeCount();
    back_.assign(p_.wireCount(), 0);
    const std::vector<uint32_t> scc = components();
    // Per delay inside a loop: cut its loop inputs or its loop outputs, whichever are fewer (ties: the outputs, so the
    // delay follows what feeds it and its output runs back: y = z1(x + y·k) reads x → + → z⁻¹ with ·k fed back; the
    // SVF's integrators, whose output feeds three loop nodes, sit at the start of their loops instead).
    for (NodeIndex v = 0; v < n; ++v) {
      const Node& nd = p_.node(v);
      if (nd.kind != NodeKind::Atom) continue;
      const AtomSignature& sig = atomSignature(nd.atom);
      uint32_t ins = 0, outs = 0;
      for (uint32_t i = 0; i < nd.numInputs; ++i) {
        const WireIndex w = adj_.driver[p_.flatInput(v, i)];
        if (w != kNone && sig.inputs[std::min<size_t>(i, sig.inputs.size() - 1)].delayed && scc[p_.wire(w).from.node] == scc[v])
          ++ins;
      }
      if (ins == 0) continue;
      for (const WireIndex w : adj_.outgoing(v)) outs += scc[p_.wire(w).to.node] == scc[v] ? 1u : 0u;
      if (outs > 0 && outs <= ins) {
        for (const WireIndex w : adj_.outgoing(v))
          if (scc[p_.wire(w).to.node] == scc[v] && !back_[w]) {
            back_[w] = 1;
            ++stats_.reversed;
          }
      } else {
        for (uint32_t i = 0; i < nd.numInputs; ++i) {
          const WireIndex w = adj_.driver[p_.flatInput(v, i)];
          if (w != kNone && sig.inputs[std::min<size_t>(i, sig.inputs.size() - 1)].delayed &&
              scc[p_.wire(w).from.node] == scc[v] && !back_[w]) {
            back_[w] = 1;
            ++stats_.reversed;
          }
        }
      }
    }
    std::vector<uint8_t> state(n, 0);  // 0 new, 1 on stack, 2 done
    std::vector<std::pair<NodeIndex, uint32_t>> stack;
    // Sources first (nodes without driven inputs), so loops are cut where the signal re-enters them.
    std::vector<NodeIndex> starts;
    for (NodeIndex v = 0; v < n; ++v)
      if (inDegree(v) == 0) starts.push_back(v);
    for (NodeIndex v = 0; v < n; ++v)
      if (inDegree(v) != 0) starts.push_back(v);
    for (const NodeIndex s : starts) {
      if (state[s]) continue;
      state[s] = 1;
      stack.emplace_back(s, 0);
      while (!stack.empty()) {
        auto& [v, next] = stack.back();
        const auto out = adj_.outgoing(v);
        if (next >= out.size()) {
          state[v] = 2;
          stack.pop_back();
          continue;
        }
        const WireIndex w = out[next++];
        if (back_[w]) continue;
        const NodeIndex t = p_.wire(w).to.node;
        if (state[t] == 1) {
          back_[w] = 1;
          ++stats_.reversed;
        } else if (state[t] == 0) {
          state[t] = 1;
          stack.emplace_back(t, 0);
        }
      }
    }
  }

  // Strongly connected component of every node (iterative Tarjan over the wires).
  std::vector<uint32_t> components() const {
    const uint32_t n = p_.nodeCount();
    std::vector<uint32_t> index(n, kNone), low(n, 0), comp(n, kNone);
    std::vector<uint8_t> onStack(n, 0);
    std::vector<NodeIndex> stack;
    std::vector<std::pair<NodeIndex, uint32_t>> call;
    uint32_t counter = 0, comps = 0;
    for (NodeIndex s = 0; s < n; ++s) {
      if (index[s] != kNone) continue;
      call.emplace_back(s, 0);
      index[s] = low[s] = counter++;
      stack.push_back(s);
      onStack[s] = 1;
      while (!call.empty()) {
        const NodeIndex v = call.back().first;
        const auto out = adj_.outgoing(v);
        if (call.back().second < out.size()) {
          const NodeIndex t = p_.wire(out[call.back().second++]).to.node;
          if (index[t] == kNone) {
            index[t] = low[t] = counter++;
            stack.push_back(t);
            onStack[t] = 1;
            call.emplace_back(t, 0);
          } else if (onStack[t]) {
            low[v] = std::min(low[v], index[t]);
          }
          continue;
        }
        if (low[v] == index[v]) {
          for (;;) {
            const NodeIndex u = stack.back();
            stack.pop_back();
            onStack[u] = 0;
            comp[u] = comps;
            if (u == v) break;
          }
          ++comps;
        }
        call.pop_back();
        if (!call.empty()) low[call.back().first] = std::min(low[call.back().first], low[v]);
      }
    }
    return comp;
  }

  uint32_t inDegree(NodeIndex v) const noexcept {
    uint32_t d = 0;
    for (uint32_t i = 0; i < p_.node(v).numInputs; ++i) d += adj_.driver[p_.flatInput(v, i)] != kNone ? 1u : 0u;
    return d;
  }

  // ---- 2. layers -------------------------------------------------------------------------------------------------
  void assignLayers() {
    const uint32_t n = p_.nodeCount();
    layer_.assign(n, 0);
    // Longest path over the forward edges (Kahn's order): the earliest layer of every node.
    std::vector<uint32_t> indeg(n, 0);
    for (WireIndex w = 0; w < p_.wireCount(); ++w)
      if (!back_[w]) ++indeg[p_.wire(w).to.node];
    std::vector<NodeIndex> order;
    order.reserve(n);
    for (NodeIndex v = 0; v < n; ++v)
      if (indeg[v] == 0) order.push_back(v);
    for (size_t qi = 0; qi < order.size(); ++qi) {
      const NodeIndex v = order[qi];
      for (const WireIndex w : adj_.outgoing(v)) {
        if (back_[w]) continue;
        const NodeIndex t = p_.wire(w).to.node;
        layer_[t] = std::max(layer_[t], layer_[v] + 1);
        if (--indeg[t] == 0) order.push_back(t);
      }
    }
    auto isOut = [&](NodeIndex v) {
      const NodeKind k = p_.node(v).kind;
      return k == NodeKind::Output || k == NodeKind::Export;
    };
    // Outputs and exports in one last column.
    uint32_t last = 0;
    bool anyOut = false;
    for (NodeIndex v = 0; v < n; ++v) {
      if (isOut(v)) anyOut = true;
      else last = std::max(last, layer_[v]);
    }
    if (anyOut)
      for (NodeIndex v = 0; v < n; ++v)
        if (isOut(v)) layer_[v] = last + 1;
    // Then as late as possible: every node right before its earliest consumer (a constant or param next to what it
    // scales, a branch of the signal next to where it rejoins), so few wires span several layers. Sinks other than
    // outputs keep their earliest layer; `in` declarations stay in the first column.
    for (size_t qi = order.size(); qi-- > 0;) {
      const NodeIndex v = order[qi];
      if (isOut(v)) continue;
      uint32_t first = UINT32_MAX;
      for (const WireIndex w : adj_.outgoing(v))
        if (!back_[w]) first = std::min(first, layer_[p_.wire(w).to.node]);
      if (first == UINT32_MAX) continue;  // a sink
      if (p_.node(v).kind != NodeKind::Input) layer_[v] = std::max(layer_[v], first - 1);
    }
    // Compact empty layers.
    uint32_t maxL = 0;
    for (NodeIndex v = 0; v < n; ++v) maxL = std::max(maxL, layer_[v]);
    std::vector<uint32_t> used(maxL + 1, 0), remap(maxL + 1, 0);
    for (NodeIndex v = 0; v < n; ++v) used[layer_[v]] = 1;
    uint32_t next = 0;
    for (uint32_t l = 0; l <= maxL; ++l) remap[l] = used[l] ? next++ : next;
    for (NodeIndex v = 0; v < n; ++v) layer_[v] = remap[layer_[v]];
    layerCount_ = next;
    stats_.layers = layerCount_;
  }

  // ---- 3. items, dummies, links ----------------------------------------------------------------------------------
  uint32_t addDummy(uint32_t layer) {
    Item it;
    it.layer = layer;
    items_.push_back(it);
    ++stats_.dummies;
    return static_cast<uint32_t>(items_.size() - 1);
  }

  void buildItems() {
    const uint32_t n = p_.nodeCount();
    items_.resize(n);
    for (NodeIndex v = 0; v < n; ++v) {
      items_[v].node = v;
      items_[v].layer = layer_[v];
      items_[v].w = g_.shape[v].w;
      items_[v].h = g_.shape[v].h;
    }
    // Nets in (node, port) order.
    std::vector<std::vector<WireIndex>> byPort(p_.portCount());
    for (WireIndex w = 0; w < p_.wireCount(); ++w) {
      const Wire& wr = p_.wire(w);
      byPort[p_.flatOutput(wr.from.node, wr.from.port)].push_back(w);
    }
    for (NodeIndex v = 0; v < n; ++v)
      for (uint16_t k = 0; k < p_.node(v).numOutputs; ++k) {
        auto& ws = byPort[p_.flatOutput(v, k)];
        if (ws.empty()) continue;
        Net net;
        net.node = v;
        net.port = k;
        net.wires = std::move(ws);
        nets_.push_back(std::move(net));
      }
    for (uint32_t ni = 0; ni < nets_.size(); ++ni) {
      Net& net = nets_[ni];
      const uint32_t ls = layer_[net.node];
      const float ps = g_.outPin(p_, net.node, net.port);
      uint32_t maxF = 0, minR = UINT32_MAX;
      for (const WireIndex w : net.wires) {
        const uint32_t lt = layer_[p_.wire(w).to.node];
        if (lt > ls) maxF = std::max(maxF, lt);
        else minR = std::min(minR, lt);
      }
      // Forward chain: dummies in layers ls + 1 … maxF − 1.
      net.fwdFirst = ls + 1;
      for (uint32_t l = ls + 1; l < maxF; ++l) net.fwd.push_back(addDummy(l));
      // Reversed chain: dummies in layers minR … ls.
      if (minR != UINT32_MAX) {
        net.revFirst = minR;
        for (uint32_t l = minR; l <= ls; ++l) net.rev.push_back(addDummy(l));
      }
      // Links. Forward chain: source → fwd[0] → … ; each forward target from the chain's element one layer before it.
      auto fwdAt = [&](uint32_t l) -> std::pair<uint32_t, float> {  // the chain element in layer l (l ≥ ls)
        if (l == ls) return {net.node, ps};
        return {net.fwd[l - net.fwdFirst], 0.0f};
      };
      for (uint32_t i = 0; i < net.fwd.size(); ++i) {
        const auto [a, pa] = fwdAt(net.fwdFirst + i - 1);
        links_.push_back({a, net.fwd[i], pa, 0.0f});
      }
      for (const WireIndex w : net.wires) {
        const Wire& wr = p_.wire(w);
        const uint32_t lt = layer_[wr.to.node];
        const float pt = g_.inPin(p_, wr.to.node, wr.to.port);
        if (lt > ls) {
          const auto [a, pa] = fwdAt(lt - 1);
          links_.push_back({a, wr.to.node, pa, pt});
        } else {
          // Reversed: from the chain element in layer lt − 1 (if any) and, in the same layer, the element at lt.
          const uint32_t here = net.rev[lt - net.revFirst];
          if (lt > net.revFirst) links_.push_back({net.rev[lt - 1 - net.revFirst], wr.to.node, 0.0f, pt});
          attach_.push_back({here, wr.to.node, 0.0f, pt});
        }
      }
      if (!net.rev.empty()) {
        for (uint32_t i = 0; i + 1 < net.rev.size(); ++i) links_.push_back({net.rev[i], net.rev[i + 1], 0.0f, 0.0f});
        attach_.push_back({net.rev.back(), net.node, 0.0f, ps});
      }
    }
    // Layers, in node order then dummies in creation order.
    layers_.assign(layerCount_, {});
    for (uint32_t i = 0; i < items_.size(); ++i) layers_[items_[i].layer].push_back(i);
    for (auto& L : layers_)
      for (uint32_t k = 0; k < L.size(); ++k) items_[L[k]].order = k;
    // Adjacency of the links per item (for barycentres and placement).
    nbr_.assign(items_.size(), {});
    for (uint32_t li = 0; li < links_.size(); ++li) {
      nbr_[links_[li].a].push_back(li);
      nbr_[links_[li].b].push_back(li);
    }
    nbrAttach_.assign(items_.size(), {});
    for (uint32_t ai = 0; ai < attach_.size(); ++ai) {
      nbrAttach_[attach_[ai].a].push_back(ai);
      nbrAttach_[attach_[ai].b].push_back(ai);
    }
  }

  // ---- 4. crossing minimisation ----------------------------------------------------------------------------------
  // Position of an item's attachment point within its layer's order: the item's index plus where the pin sits on it.
  double posOf(uint32_t item, float pin) const noexcept {
    const Item& it = items_[item];
    const double f = it.h > 0.0f ? std::clamp(static_cast<double>(pin) / static_cast<double>(it.h), 0.0, 1.0) : 0.5;
    return static_cast<double>(it.order) + f;
  }

  // Crossings between links of layer L and L + 1 (inversions after sorting by the left position).
  uint64_t crossingsBetween(uint32_t L) {
    pairs_.clear();
    for (const uint32_t a : layers_[L])
      for (const uint32_t li : nbr_[a]) {
        const Link& k = links_[li];
        if (k.a == a && items_[k.b].layer == L + 1) pairs_.emplace_back(posOf(k.a, k.pa), posOf(k.b, k.pb));
      }
    std::sort(pairs_.begin(), pairs_.end());
    seq_.resize(pairs_.size());
    for (size_t i = 0; i < pairs_.size(); ++i) seq_[i] = pairs_[i].second;
    return inversions(0, seq_.size());
  }

  // Merge-sort inversion count of seq_[b, e) (strict: equal values do not count).
  uint64_t inversions(size_t b, size_t e) {
    if (e - b < 2) return 0;
    const size_t m = b + (e - b) / 2;
    uint64_t c = inversions(b, m) + inversions(m, e);
    tmp_.clear();
    size_t i = b, j = m;
    while (i < m && j < e) {
      if (seq_[j] < seq_[i]) {
        c += m - i;
        tmp_.push_back(seq_[j++]);
      } else {
        tmp_.push_back(seq_[i++]);
      }
    }
    while (i < m) tmp_.push_back(seq_[i++]);
    while (j < e) tmp_.push_back(seq_[j++]);
    std::copy(tmp_.begin(), tmp_.end(), seq_.begin() + static_cast<std::ptrdiff_t>(b));
    return c;
  }

  uint64_t totalCrossings() {
    uint64_t c = 0;
    for (uint32_t L = 0; L + 1 < layerCount_; ++L) c += crossingsBetween(L);
    return c;
  }

  // Reorders layer L by the barycentre of its neighbours in layer `from` (L − 1 or L + 1); attachments in the same
  // layer pull too (a reversed chain stays next to its ends).
  void sortLayer(uint32_t L, uint32_t from) {
    auto& layer = layers_[L];
    bary_.resize(layer.size());
    for (size_t k = 0; k < layer.size(); ++k) {
      const uint32_t it = layer[k];
      double sum = 0.0;
      uint32_t cnt = 0;
      for (const uint32_t li : nbr_[it]) {
        const Link& lk = links_[li];
        const uint32_t other = lk.a == it ? lk.b : lk.a;
        if (items_[other].layer != from) continue;
        sum += posOf(other, lk.a == it ? lk.pb : lk.pa);
        ++cnt;
      }
      for (const uint32_t ai : nbrAttach_[it]) {
        const Link& lk = attach_[ai];
        const uint32_t other = lk.a == it ? lk.b : lk.a;
        sum += static_cast<double>(items_[other].order) + 0.5;
        ++cnt;
      }
      bary_[k] = {cnt ? sum / cnt : static_cast<double>(items_[it].order) + 0.5, it};
    }
    std::stable_sort(bary_.begin(), bary_.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (size_t k = 0; k < layer.size(); ++k) {
      layer[k] = bary_[k].second;
      items_[layer[k]].order = static_cast<uint32_t>(k);
    }
  }

  void orderLayers() {
    // Initial order: a DFS-like order from the first layer keeps related nodes together; node index (text order) is a
    // good start already, and dummies follow their source.
    for (uint32_t L = 1; L < layerCount_; ++L) sortLayer(L, L - 1);
    uint64_t best = totalCrossings();
    stats_.crossingsInitial = best;
    std::vector<std::vector<uint32_t>> bestLayers = layers_;
    for (uint32_t s = 0; s < o_.orderSweeps && best > 0; ++s) {
      if (s % 2 == 0)
        for (uint32_t L = layerCount_ - 1; L-- > 0;) sortLayer(L, L + 1);
      else
        for (uint32_t L = 1; L < layerCount_; ++L) sortLayer(L, L - 1);
      const uint64_t c = totalCrossings();
      if (c < best) {
        best = c;
        bestLayers = layers_;
      }
    }
    layers_ = std::move(bestLayers);
    for (auto& L : layers_)
      for (uint32_t k = 0; k < L.size(); ++k) items_[L[k]].order = k;
    stats_.crossings = best;
  }

  // ---- 5. vertical placement ---------------------------------------------------------------------------------------
  float gapBetween(uint32_t a, uint32_t b) const noexcept {
    return (items_[a].node == kNone || items_[b].node == kNone) ? o_.dummyGap : o_.nodeGap;
  }

  void placeLayer(uint32_t L) {
    const auto& layer = layers_[L];
    const size_t m = layer.size();
    if (m == 0) return;
    // Desired top of each item: where its links' other ends would make its wires straight.
    target_.resize(m);
    weight_.resize(m);
    offset_.resize(m);
    float off = 0.0f;
    for (size_t k = 0; k < m; ++k) {
      const uint32_t it = layer[k];
      double sum = 0.0, cnt = 0.0;
      auto pull = [&](const Link& lk) {
        const bool isA = lk.a == it;
        const uint32_t other = isA ? lk.b : lk.a;
        const float own = isA ? lk.pa : lk.pb, theirs = isA ? lk.pb : lk.pa;
        // A wire passing through layers pulls hardest: long wires are straight (fewer jogs).
        const double wgt = (items_[it].node == kNone && items_[other].node == kNone) ? 4.0 : 1.0;
        sum += wgt * static_cast<double>(items_[other].y + theirs - own);
        cnt += wgt;
      };
      for (const uint32_t li : nbr_[it]) pull(links_[li]);
      for (const uint32_t ai : nbrAttach_[it]) pull(attach_[ai]);
      offset_[k] = off;
      target_[k] = (cnt > 0.0 ? static_cast<float>(sum / cnt) : items_[it].y) - off;
      weight_[k] = static_cast<float>(std::max(cnt, 1.0));
      if (k + 1 < m) off += items_[it].h + gapBetween(it, layer[k + 1]);
    }
    // Pool adjacent violators: z non-decreasing, minimising Σ w (z − target)².
    blocks_.clear();
    for (size_t k = 0; k < m; ++k) {
      Block b{target_[k] * weight_[k], weight_[k], 1};
      while (!blocks_.empty() && blocks_.back().mean() > b.mean()) {
        b.sum += blocks_.back().sum;
        b.w += blocks_.back().w;
        b.count += blocks_.back().count;
        blocks_.pop_back();
      }
      blocks_.push_back(b);
    }
    size_t k = 0;
    for (const Block& b : blocks_)
      for (uint32_t i = 0; i < b.count; ++i, ++k) items_[layer[k]].y = b.mean() + offset_[k];
  }

  struct Block {
    float sum, w;
    uint32_t count;
    float mean() const noexcept { return sum / w; }
  };

  void placeVertically() {
    for (auto& L : layers_) {  // stacked
      float y = 0.0f;
      for (size_t k = 0; k < L.size(); ++k) {
        items_[L[k]].y = y;
        if (k + 1 < L.size()) y += items_[L[k]].h + gapBetween(L[k], L[k + 1]);
      }
    }
    for (uint32_t s = 0; s < o_.placeSweeps; ++s) {
      if (s % 2 == 0)
        for (uint32_t L = 0; L < layerCount_; ++L) placeLayer(L);
      else
        for (uint32_t L = layerCount_; L-- > 0;) placeLayer(L);
    }
    // Grid, then the minimum gaps again (snapping may close one by up to a grid step), then y ≥ 0.
    float minY = 0.0f;
    bool first = true;
    for (auto& L : layers_) {
      for (size_t k = 0; k < L.size(); ++k) {
        Item& it = items_[L[k]];
        it.y = snapTo(it.y, o_.grid);
        if (k > 0) {
          const Item& prev = items_[L[k - 1]];
          const float lo = prev.y + prev.h + gapBetween(L[k - 1], L[k]);
          if (it.y < lo) it.y = lo;
        }
        if (first || it.y < minY) minY = it.y;
        first = false;
      }
    }
    for (Item& it : items_) it.y -= minY;
  }

  // ---- 6. routing --------------------------------------------------------------------------------------------------
  float pinY(uint32_t item, float pin) const noexcept { return items_[item].y + pin; }

  uint32_t groupFor(Net& net, int32_t channel) {
    const uint32_t slot = static_cast<uint32_t>(channel + 1);
    auto it = net.groupOf.find(slot);
    if (it != net.groupOf.end()) return it->second;
    groups_.emplace_back();
    groupChannel_.push_back(slot);
    const uint32_t gi = static_cast<uint32_t>(groups_.size() - 1);
    net.groupOf.emplace(slot, gi);
    return gi;
  }

  static void addTerminal(std::vector<float>& v, float y) {
    if (std::find(v.begin(), v.end(), y) == v.end()) v.push_back(y);
  }

  void route() {
    // Terminals of every net's group in every channel it crosses. Channel c lies between layers c and c + 1
    // (c = −1: left of layer 0; c = last: right of the last layer).
    for (Net& net : nets_) {
      const uint32_t ls = layer_[net.node];
      const float ys = pinY(net.node, g_.outPin(p_, net.node, net.port));
      auto fwdY = [&](uint32_t l) { return l == ls ? ys : items_[net.fwd[l - net.fwdFirst]].y; };
      for (uint32_t i = 0; i < net.fwd.size(); ++i) {
        Group& g = groups_[groupFor(net, static_cast<int32_t>(net.fwdFirst + i) - 1)];
        addTerminal(g.left, fwdY(net.fwdFirst + i - 1));
        addTerminal(g.right, items_[net.fwd[i]].y);
      }
      for (const WireIndex w : net.wires) {
        const Wire& wr = p_.wire(w);
        const uint32_t lt = layer_[wr.to.node];
        const float yt = pinY(wr.to.node, g_.inPin(p_, wr.to.node, wr.to.port));
        if (lt > ls) {
          Group& g = groups_[groupFor(net, static_cast<int32_t>(lt) - 1)];
          addTerminal(g.left, fwdY(lt - 1));
          addTerminal(g.right, yt);
        } else {
          Group& g = groups_[groupFor(net, static_cast<int32_t>(lt) - 1)];
          addTerminal(g.right, items_[net.rev[lt - net.revFirst]].y);
          addTerminal(g.right, yt);
          if (lt > net.revFirst) addTerminal(g.left, items_[net.rev[lt - 1 - net.revFirst]].y);
        }
      }
      if (!net.rev.empty()) {
        Group& g = groups_[groupFor(net, static_cast<int32_t>(ls))];  // the U-turn right of the source
        addTerminal(g.left, ys);
        addTerminal(g.left, items_[net.rev.back()].y);
        for (uint32_t i = 0; i + 1 < net.rev.size(); ++i) {
          Group& h = groups_[groupFor(net, static_cast<int32_t>(net.revFirst + i))];
          addTerminal(h.left, items_[net.rev[i]].y);
          addTerminal(h.right, items_[net.rev[i + 1]].y);
        }
      }
    }
    // Tracks per channel.
    const uint32_t slots = layerCount_ + 1;
    tracks_.assign(slots, 0);
    std::vector<std::vector<uint32_t>> bySlot(slots);
    for (uint32_t gi = 0; gi < groups_.size(); ++gi) {
      Group& g = groups_[gi];
      g.lo = g.hi = g.left.empty() ? g.right.front() : g.left.front();
      for (const float y : g.left) g.lo = std::min(g.lo, y), g.hi = std::max(g.hi, y);
      for (const float y : g.right) g.lo = std::min(g.lo, y), g.hi = std::max(g.hi, y);
      if (g.hi > g.lo) bySlot[groupChannel_[gi]].push_back(gi);
    }
    for (uint32_t s = 0; s < slots; ++s) assignTracks(bySlot[s], tracks_[s]);
    for (uint32_t s = 0; s < slots; ++s) stats_.tracks += tracks_[s];
  }

  // Cost of placing a left of b in one channel: b's left stubs crossing a's vertical, a's right stubs crossing b's.
  static uint32_t pairCost(const Group& a, const Group& b) noexcept {
    uint32_t c = 0;
    for (const float y : b.left) c += (y >= a.lo && y <= a.hi) ? 1u : 0u;
    for (const float y : a.right) c += (y >= b.lo && y <= b.hi) ? 1u : 0u;
    return c;
  }

  bool overlaps(const Group& a, const Group& b) const noexcept {
    const float sep = o_.trackSpacing * 0.5f;
    return a.lo <= b.hi + sep && b.lo <= a.hi + sep;
  }

  void assignTracks(std::vector<uint32_t>& gs, uint32_t& trackCount) {
    trackCount = 0;
    if (gs.empty()) return;
    // Initial key: wires going down ordered by their start, wires going up after them by theirs (fewest stub
    // crossings between two parallel jogs), then each group inserted where it costs least against the ones before.
    auto key = [&](uint32_t gi) {
      const Group& g = groups_[gi];
      float l = 0.0f, r = 0.0f;
      for (const float y : g.left) l += y;
      for (const float y : g.right) r += y;
      l = g.left.empty() ? g.lo : l / static_cast<float>(g.left.size());
      r = g.right.empty() ? g.hi : r / static_cast<float>(g.right.size());
      return r >= l ? std::pair<int, float>{0, -l} : std::pair<int, float>{1, l};
    };
    std::stable_sort(gs.begin(), gs.end(), [&](uint32_t a, uint32_t b) { return key(a) < key(b); });
    std::vector<uint32_t> seq;
    seq.reserve(gs.size());
    for (const uint32_t g : gs) {
      const Group& G = groups_[g];
      int64_t cost = 0;
      for (const uint32_t h : seq)
        if (overlaps(G, groups_[h])) cost += pairCost(G, groups_[h]);
      int64_t bestCost = cost;
      size_t bestPos = 0;
      for (size_t p = 0; p < seq.size(); ++p) {
        const Group& H = groups_[seq[p]];
        if (overlaps(G, H)) cost += static_cast<int64_t>(pairCost(H, G)) - static_cast<int64_t>(pairCost(G, H));
        if (cost < bestCost) {  // strictly better: ties keep the earlier (key) position
          bestCost = cost;
          bestPos = p + 1;
        }
      }
      seq.insert(seq.begin() + static_cast<std::ptrdiff_t>(bestPos), g);
    }
    // Tracks keep the order among overlapping groups: each takes the track after the last overlapping one before it.
    for (size_t i = 0; i < seq.size(); ++i) {
      Group& G = groups_[seq[i]];
      uint32_t t = 0;
      for (size_t j = 0; j < i; ++j) {
        const Group& H = groups_[seq[j]];
        if (overlaps(G, H)) t = std::max(t, H.track + 1);
      }
      G.track = t;
      trackCount = std::max(trackCount, t + 1);
    }
  }

  // ---- write positions and routes --------------------------------------------------------------------------------
  float channelTrackX(uint32_t slot, uint32_t track) const noexcept {
    return chanX_[slot] + o_.layerGap + static_cast<float>(track) * o_.trackSpacing;
  }

  void write() {
    const uint32_t slots = layerCount_ + 1;
    // Columns: widest node per layer; channels: tracks + a gap each side (the outer channels only when used).
    std::vector<float> colW(layerCount_, 0.0f);
    for (const Item& it : items_) colW[it.layer] = std::max(colW[it.layer], it.w);
    auto chanW = [&](uint32_t s) {
      const bool outer = s == 0 || s == slots - 1;
      if (tracks_[s] == 0) return outer ? 0.0f : 2.0f * o_.layerGap;
      return 2.0f * o_.layerGap + static_cast<float>(tracks_[s] - 1) * o_.trackSpacing;
    };
    chanX_.assign(slots, 0.0f);
    colX_.assign(layerCount_, 0.0f);
    float x = 0.0f;
    for (uint32_t s = 0; s < slots; ++s) {
      chanX_[s] = x;
      x += chanW(s);
      if (s < layerCount_) {
        x = snapTo(x, o_.grid);
        colX_[s] = x;
        x += snapTo(colW[s], o_.grid);
      }
    }
    // Nodes: centred in their column.
    float maxY = 0.0f;
    for (NodeIndex v = 0; v < p_.nodeCount(); ++v) {
      const Item& it = items_[v];
      NodeLayout l;
      l.placed = true;
      l.x = snapTo(colX_[it.layer] + (colW[it.layer] - it.w) * 0.5f, o_.grid);
      l.y = it.y;
      p_.setLayout(v, l);
      maxY = std::max(maxY, it.y + it.h);
    }
    stats_.width = x;
    stats_.height = maxY;
    // Wires.
    std::vector<Point2> pts;
    for (Net& net : nets_) {
      const uint32_t ls = layer_[net.node];
      const float ys = pinY(net.node, g_.outPin(p_, net.node, net.port));
      auto fwdY = [&](uint32_t l) { return l == ls ? ys : items_[net.fwd[l - net.fwdFirst]].y; };
      auto trackX = [&](int32_t channel) {
        const uint32_t slot = static_cast<uint32_t>(channel + 1);
        return channelTrackX(slot, groups_[net.groupOf.at(slot)].track);
      };
      auto jog = [&](int32_t channel, float y0, float y1) {
        if (y0 == y1) return;  // straight through the channel
        const float tx = trackX(channel);
        pts.push_back({tx, y0});
        pts.push_back({tx, y1});
      };
      for (const WireIndex w : net.wires) {
        const Wire& wr = p_.wire(w);
        const uint32_t lt = layer_[wr.to.node];
        const float yt = pinY(wr.to.node, g_.inPin(p_, wr.to.node, wr.to.port));
        pts.clear();
        if (lt > ls) {
          for (uint32_t c = ls; c < lt; ++c) jog(static_cast<int32_t>(c), fwdY(c), c + 1 == lt ? yt : fwdY(c + 1));
        } else {
          const float yr = items_[net.rev.back()].y;
          // U-turn right of the source, then leftwards through the chain.
          {
            const float tx = trackX(static_cast<int32_t>(ls));
            pts.push_back({tx, ys});
            pts.push_back({tx, yr});
          }
          for (uint32_t l = ls; l-- > lt;) {
            // channel l lies between layer l and l + 1: from the chain at l + 1 to the chain at l
            jog(static_cast<int32_t>(l), items_[net.rev[l + 1 - net.revFirst]].y, items_[net.rev[l - net.revFirst]].y);
          }
          {
            const float tx = trackX(static_cast<int32_t>(lt) - 1);
            pts.push_back({tx, items_[net.rev[lt - net.revFirst]].y});
            pts.push_back({tx, yt});
          }
        }
        simplify(pts);
        p_.setWireVia(w, pts);
      }
    }
  }

  // Drops repeated points and points in the middle of a straight run.
  static void simplify(std::vector<Point2>& pts) {
    std::vector<Point2> out;
    out.reserve(pts.size());
    for (const Point2& q : pts) {
      if (!out.empty() && out.back() == q) continue;
      if (out.size() >= 2) {
        const Point2& a = out[out.size() - 2];
        const Point2& b = out.back();
        if ((a.x == b.x && b.x == q.x) || (a.y == b.y && b.y == q.y)) {
          out.back() = q;
          continue;
        }
      }
      out.push_back(q);
    }
    pts = std::move(out);
  }

  Patch& p_;
  const LayoutGeometry& g_;
  const AutoLayoutOptions& o_;
  AutoLayoutStats stats_;
  Adjacency adj_;
  std::vector<uint8_t> back_;
  std::vector<uint32_t> layer_;
  uint32_t layerCount_ = 0;
  std::vector<Item> items_;
  std::vector<std::vector<uint32_t>> layers_;
  std::vector<Net> nets_;
  std::vector<Link> links_, attach_;
  std::vector<std::vector<uint32_t>> nbr_, nbrAttach_;
  std::vector<Group> groups_;
  std::vector<uint32_t> groupChannel_;  // group → channel slot
  std::vector<uint32_t> tracks_;        // per slot
  std::vector<float> chanX_, colX_;
  // scratch
  std::vector<std::pair<double, double>> pairs_;
  std::vector<double> seq_, tmp_;
  std::vector<std::pair<double, uint32_t>> bary_;
  std::vector<float> target_, weight_, offset_;
  std::vector<Block> blocks_;
};

struct Box {
  float x0, y0, x1, y1;
};

Box boxOf(const Patch& p, const LayoutGeometry& g, NodeIndex v) noexcept {
  const NodeLayout& l = p.layout(v);
  return {l.x, l.y, l.x + g.shape[v].w, l.y + g.shape[v].h};
}

}  // namespace

LayoutGeometry defaultGeometry(const Patch& p) {
  LayoutGeometry g;
  g.shape.resize(p.nodeCount());
  g.inPinY.assign(p.portCount(), 0.0f);
  g.outPinY.assign(p.portCount(), 0.0f);
  for (NodeIndex v = 0; v < p.nodeCount(); ++v) {
    const Node& nd = p.node(v);
    const uint32_t rows = std::max<uint32_t>(1, std::max<uint32_t>(nd.numInputs, nd.numOutputs));
    g.shape[v] = {48.0f, 16.0f * static_cast<float>(rows) + 8.0f};
    for (uint32_t i = 0; i < nd.numInputs; ++i) g.inPinY[p.flatInput(v, i)] = 12.0f + 16.0f * static_cast<float>(i);
    for (uint32_t i = 0; i < nd.numOutputs; ++i) g.outPinY[p.flatOutput(v, i)] = 12.0f + 16.0f * static_cast<float>(i);
  }
  return g;
}

AutoLayoutStats autoLayout(Patch& p, const LayoutGeometry& geometry, const AutoLayoutOptions& options) {
  return Layouter(p, geometry, options).run();
}

bool fullyPlaced(const Patch& p) noexcept {
  for (NodeIndex v = 0; v < p.nodeCount(); ++v)
    if (!p.layout(v).placed) return false;
  return true;
}

uint32_t routeUnrouted(Patch& p, const LayoutGeometry& g, float stub) {
  uint32_t routed = 0;
  for (WireIndex w = 0; w < p.wireCount(); ++w) {
    if (!p.wireVia(w).empty()) continue;
    const Wire& wr = p.wire(w);
    const NodeLayout& a = p.layout(wr.from.node);
    const NodeLayout& b = p.layout(wr.to.node);
    const float sx = a.x + g.shape[wr.from.node].w, sy = a.y + g.outPin(p, wr.from.node, wr.from.port);
    const float tx = b.x, ty = b.y + g.inPin(p, wr.to.node, wr.to.port);
    std::vector<Point2> via;
    if (sy == ty && tx > sx) continue;  // a straight wire needs no points
    if (tx - sx >= 2.0f * stub) {
      const float mx = std::round((sx + tx) * 0.5f);
      via = {{mx, sy}, {mx, ty}};
    } else {
      const float below = std::max(a.y + g.shape[wr.from.node].h, b.y + g.shape[wr.to.node].h) + stub;
      via = {{sx + stub, sy}, {sx + stub, below}, {tx - stub, below}, {tx - stub, ty}};
    }
    p.setWireVia(w, std::move(via));
    ++routed;
  }
  return routed;
}

uint32_t completeLayout(Patch& p, const LayoutGeometry& g, const AutoLayoutOptions& options) {
  const uint32_t n = p.nodeCount();
  uint32_t placedBefore = 0;
  for (NodeIndex v = 0; v < n; ++v) placedBefore += p.layout(v).placed ? 1u : 0u;
  if (placedBefore == 0) {
    autoLayout(p, g, options);
    return n;
  }
  if (placedBefore == n) {
    routeUnrouted(p, g);
    return 0;
  }
  Patch tidy = p;
  autoLayout(tidy, g, options);
  std::vector<NodeIndex> fixed;
  for (NodeIndex v = 0; v < n; ++v)
    if (p.layout(v).placed) fixed.push_back(v);
  uint32_t added = 0;
  for (NodeIndex v = 0; v < n; ++v) {
    if (p.layout(v).placed) continue;
    NodeLayout l = tidy.layout(v);
    for (bool moved = true; moved;) {
      moved = false;
      for (const NodeIndex f : fixed) {
        const Box b = boxOf(p, g, f);
        const float x1 = l.x + g.shape[v].w, y1 = l.y + g.shape[v].h;
        if (l.x < b.x1 + options.nodeGap && b.x0 < x1 + options.nodeGap && l.y < b.y1 + options.nodeGap &&
            b.y0 < y1 + options.nodeGap) {
          l.y = snapTo(b.y1 + options.nodeGap, options.grid);
          moved = true;
        }
      }
    }
    p.setLayout(v, l);
    fixed.push_back(v);
    ++added;
  }
  routeUnrouted(p, g);
  return added;
}

uint32_t carryLayout(Patch& next, const Patch& previous) {
  uint32_t placed = 0;
  std::vector<uint8_t> kept(next.nodeCount(), 0);  // node has the position its previous counterpart had
  for (NodeIndex v = 0; v < next.nodeCount(); ++v) {
    const auto pv = previous.indexOf(next.node(v).id);
    if (!pv) continue;
    const NodeLayout& old = previous.layout(*pv);
    if (!next.layout(v).placed && old.placed) {
      next.setLayout(v, old);
      ++placed;
    }
    kept[v] = next.layout(v) == old ? 1 : 0;
  }
  // Routing points of wires whose ends both kept their positions.
  std::unordered_map<uint64_t, WireIndex> prevByTarget;  // (target id, port) → wire
  for (WireIndex w = 0; w < previous.wireCount(); ++w) {
    const Wire& wr = previous.wire(w);
    prevByTarget.emplace((static_cast<uint64_t>(previous.node(wr.to.node).id.value) << 16) | wr.to.port, w);
  }
  for (WireIndex w = 0; w < next.wireCount(); ++w) {
    if (!next.wireVia(w).empty()) continue;
    const Wire& wr = next.wire(w);
    if (!kept[wr.from.node] || !kept[wr.to.node]) continue;
    const auto it = prevByTarget.find((static_cast<uint64_t>(next.node(wr.to.node).id.value) << 16) | wr.to.port);
    if (it == prevByTarget.end()) continue;
    const Wire& pw = previous.wire(it->second);
    if (previous.node(pw.from.node).id != next.node(wr.from.node).id || pw.from.port != wr.from.port) continue;
    const auto via = previous.wireVia(it->second);
    if (!via.empty()) next.setWireVia(w, std::vector<Point2>(via.begin(), via.end()));
  }
  return placed;
}

uint32_t carryLayout(PatchFile& next, const PatchFile& previous) {
  uint32_t placed = 0;
  for (Patch& p : next.patches)
    if (const Patch* old = previous.find(p.name())) placed += carryLayout(p, *old);
  return placed;
}

}  // namespace pq::graph
