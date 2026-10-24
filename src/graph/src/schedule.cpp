// The scheduler (T1.5). pq/graph/schedule.h describes what it produces and how a backend runs it; this file is the
// algorithm, in passes over flat arrays indexed by NodeIndex:
//   analyse   node classes (free source / alias / op / solve member / sink), poly, rate, width, the edge CSR
//   tarjan    SCCs (iterative), cyclic flags
//   cycles    per cyclic SCC: voice/global check, delay-free cycles (error, or z1 insertion + restart in visual mode)
//   solves    per solve block: body = reach(unknowns) ∩ coreach(equations) over same-sample edges, and its checks
//   stages    voice/global stage per SCC; control vs audio class; loop sub-block size
//   order     Kahn over the SCC DAG with priority (stage, class, sub-block, LIFO); inside cyclic SCCs, Kahn over the
//             SCC with solve bodies contracted
//   values    every node output channel → an Operand (virtual values for op outputs, aliases resolved)
//   emit      ops, regions, stages, broadcasts, fills, split writes, solve blocks; uses recorded per op position
//   buffers   liveness with loop extension, linear-scan interval reuse with in-place ops
//   state     layout keyed by stateKey/channel/arena with tags, stable against a previous layout
// Everything is O(nodes + wires) except the priority queues (log factor) and the per-solve reachability.

#include "pq/graph/schedule.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <functional>
#include <queue>
#include <string>
#include <unordered_map>

#include "pq/graph/kernel_map.h"

namespace pq::graph {

namespace {

constexpr uint64_t mix64(uint64_t x) noexcept {  // splitmix64 finaliser
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

enum EdgeFlag : uint8_t {
  kEdgeNormal,   // same-sample dependency
  kEdgeSplit,    // into z1.x / delay*.x: cut inside a feedback SCC (split kernels)
  kEdgeDelayed,  // into another delayed input (phasor.hz, sine_hz.hz, sample_read rate): no split kernel
  kEdgeSolve,    // implicit equation → unknown of one solve block
};

enum class NK : uint8_t {
  Free,      // no op, no inputs: const, param, engine-connected `in`, engine sources, sr
  Alias,     // no op: channel, pack, `in` with a default, a reducer of a global value
  Op,        // kernel atoms, voice reducers, send
  Unknown,   // solve block members
  Equation,
  Sink,      // `out`, `export`
};

struct Edge {
  NodeIndex to = kNone;
  uint16_t input = 0;
  uint8_t flag = kEdgeNormal;
};

struct SolveInfo {
  std::vector<NodeIndex> unknowns, equations, body;  // body: in emission order once ordered
};

// A virtual value: one channel of an op output (or an unknown, a reducer accumulator, a fill, a broadcast).
struct Val {
  bool pattern = false;
  uint8_t lanes = 1;
  Poly poly = Poly::Global;
  bool perGroup = false;
  uint32_t defStage = 0;
  uint32_t def = kNone;   // op position
  // Liveness end in half steps: 2·p = last read BY op p (op p may write its output over it in place, element-wise);
  // 2·p + 1 = last read AFTER op p and before op p + 1 (op p must not overwrite it; its buffer is free from op p + 1
  // on). Reads after an op: the solver's lhs/rhs/unknown reads after a solve body, the engine's voice-output reads
  // at the end of a voice stage, and the next iteration of a loop (voice group, Loop sub-block, Newton evaluation)
  // re-reading a value defined before the loop.
  uint32_t live = 0;
  uint32_t buffer = kNone;
  NodeIndex node = kNone; // the node whose op defines it (for dumps)
};

// A kernel input of an op: which registry input of the node feeds kernel input k.
struct KIn {
  uint32_t reg;
  uint32_t k;
};

struct Use {
  uint32_t val;
  uint32_t pos;
  uint32_t region;
  uint32_t solve;  // kNone outside a solve body
  bool after;      // read after op `pos` has run (the solver's reads after a body), not by it
};

struct PendingState {
  uint64_t key;
  uint16_t channel;
  Poly arena;
  StateTag tag;
  uint32_t size;
  dsp::StateInit init;
  NodeIndex node;
};

bool isSplittable(dsp::Atom k) noexcept {
  return k == dsp::Atom::Z1 || k == dsp::Atom::DelayLinear || k == dsp::Atom::DelayCubic ||
         k == dsp::Atom::DelayAllpass;
}

dsp::Atom readKernel(dsp::Atom k) noexcept {
  switch (k) {
    case dsp::Atom::Z1: return dsp::Atom::Z1Read;
    case dsp::Atom::DelayLinear: return dsp::Atom::DelayReadLinear;
    case dsp::Atom::DelayCubic: return dsp::Atom::DelayReadCubic;
    default: return dsp::Atom::DelayReadAllpass;
  }
}

dsp::Atom writeKernel(dsp::Atom k) noexcept { return k == dsp::Atom::Z1 ? dsp::Atom::Z1Write : dsp::Atom::DelayWrite; }

uint32_t specIndex(const AtomSignature& sig, uint32_t i) noexcept {
  const auto n = static_cast<uint32_t>(sig.inputs.size());
  return i < n ? i : n - 1;
}

uint32_t roundUpStage(int64_t s, Poly p) noexcept {
  if (s < 0) s = 0;
  const int64_t parity = p == Poly::Voice ? 1 : 0;
  if ((s & 1) != parity) ++s;
  return static_cast<uint32_t>(s);
}

uint64_t operandKey(const Operand& o) noexcept { return (static_cast<uint64_t>(o.kind) << 32) | o.index; }

constexpr uint32_t kItemFree = 0, kItemControl = 1, kItemBlock = 2, kItemLoop = 3;
constexpr uint32_t kMaxSubBlock = 1u << 20;

class Scheduler {
public:
  Scheduler(const TypedPatch& tp, const ScheduleOptions& o) : opt_(o), tp_(&tp) {}

  ScheduleResult run() {
    S().sampleRate = opt_.sampleRate;
    if (!(opt_.sampleRate > 0.0) || !std::isfinite(opt_.sampleRate)) {
      reportPlain(ErrorCode::InvalidArgument, "the sample rate must be positive");
      return std::move(res_);
    }
    if (tp_->types.size() != tp_->patch.portCount() || tp_->stateKey.size() != tp_->patch.nodeCount()) {
      reportPlain(ErrorCode::InvalidArgument, "the TypedPatch is incomplete (types/stateKey do not match the patch)");
      return std::move(res_);
    }
    // Delay-free cycles: each visual-mode pass inserts one z1 per offending SCC and starts over; every insertion
    // removes a cycle without creating a delay-free one, so this ends within the wire count.
    const uint32_t maxPasses = tp_->patch.wireCount() + 2;
    for (uint32_t pass = 0;; ++pass) {
      analyse();
      if (!res_.ok()) return std::move(res_);
      tarjan();
      std::vector<std::pair<NodeIndex, uint16_t>> inserts;
      checkCycles(inserts);
      if (!res_.ok()) return std::move(res_);
      if (inserts.empty()) break;
      if (pass >= maxPasses) {
        reportPlain(ErrorCode::Unknown, "scheduler: delay-free cycles remain after inserting delays");
        return std::move(res_);
      }
      applyInserts(inserts);
    }
    markSplits();
    checkSolves();
    if (!res_.ok()) return std::move(res_);
    assignStages();
    order();
    if (!res_.ok()) return std::move(res_);
    assignTables();
    assignValues();
    if (!res_.ok()) return std::move(res_);
    collectBroadcasts();
    emit();
    if (!res_.ok()) return std::move(res_);
    finishStructure();
    outputs();
    allocateBuffers();
    layoutState();
    if (work_) S().rewritten = work_;
    return std::move(res_);
  }

private:
  Schedule& S() noexcept { return res_.schedule; }
  const Patch& P() const noexcept { return tp_->patch; }

  // ---- diagnostics ------------------------------------------------------------------------------------------
  Diagnostic makeDiag(NodeIndex n, ErrorCode code, std::string msg) const {
    msg = "patch '" + P().name() + "': " + msg;
    const SourceText& src = opt_.source;
    if (src.text.empty()) {
      Diagnostic d;
      d.message = std::move(msg);
      d.code = code;
      return d;
    }
    NodeIndex rn = n;
    if (n != kNone && n < tp_->origin.size()) {
      uint32_t inst = tp_->origin[n].instance;
      rn = tp_->origin[n].node;
      while (inst != 0 && inst < tp_->instances.size()) {
        rn = tp_->instances[inst].callNode;
        inst = tp_->instances[inst].parent;
      }
    }
    SourceSpan span;
    if (rn != kNone && rn < src.nodeSpans.size()) span = src.nodeSpans[rn];
    return makeDiagnostic(src.text, src.fileName, span, std::move(msg), code);
  }
  void report(NodeIndex n, ErrorCode code, std::string msg) {
    if (res_.diagnostics.size() >= kMaxDiagnostics) return;
    res_.diagnostics.push_back(makeDiag(n, code, std::move(msg)));
  }
  void reportPlain(ErrorCode code, std::string msg) { report(kNone, code, std::move(msg)); }

  // Short node label for messages and dumps: 'name', or atom#index.
  std::string label(NodeIndex n) const {
    const std::string& name = P().nodeName(n);
    if (!name.empty()) return "'" + name + "'";
    const Node& nd = P().node(n);
    if (nd.kind == NodeKind::Atom) return std::string(atomSignature(nd.atom).name) + "#" + std::to_string(n);
    return std::string(toText(nd.kind)) + "#" + std::to_string(n);
  }

  // ---- analyse ----------------------------------------------------------------------------------------------
  uint8_t edgeFlag(NodeIndex v, uint32_t input) const noexcept {
    const Node& nd = P().node(v);
    if (nd.kind != NodeKind::Atom) return kEdgeNormal;
    const AtomSignature& sig = atomSignature(nd.atom);
    if (sig.inputs.empty() || !sig.inputs[specIndex(sig, input)].delayed) return kEdgeNormal;
    const KernelMapping& m = kernelMapping(nd.atom);
    if (m.lowering == Lowering::Kernel && isSplittable(m.kernel) && input == 0) return kEdgeSplit;
    return kEdgeDelayed;
  }

  NodeIndex driverNode(NodeIndex n, uint32_t i) const noexcept {
    const WireIndex w = adj_.driver[P().flatInput(n, i)];
    return w == kNone ? kNone : P().wire(w).from.node;
  }

  void analyse() {
    const Patch& p = P();
    const uint32_t N = p.nodeCount();
    adj_ = p.adjacency();
    nk_.assign(N, NK::Free);
    poly_.assign(N, Poly::Global);
    rate_.assign(N, Rate::Const);
    width_.assign(N, 1);
    reducer_.assign(N, 0);
    stateful_.assign(N, 0);
    solves_.assign(p.solveCount(), {});
    bool eventReported = false;
    for (const PortType& t : tp_->types) {
      if (t.rate == Rate::Event && !eventReported) {
        reportPlain(ErrorCode::Unsupported, "event-rate ports cannot be scheduled (v0 has no event values)");
        eventReported = true;
      }
    }
    bool secondsReported = false;
    for (NodeIndex n = 0; n < N; ++n) {
      const Node& nd = p.node(n);
      Rate r = Rate::Const;
      Poly py = Poly::Global;
      uint16_t w = 1;
      for (uint32_t o = 0; o < nd.numOutputs; ++o) {
        const PortType& t = tp_->outputType(n, o);
        r = std::max(r, t.rate);
        py = std::max(py, t.poly);
        w = std::max(w, t.width);
      }
      if (nd.numOutputs == 0) {
        for (uint32_t i = 0; i < nd.numInputs; ++i) {
          const PortType& t = tp_->inputType(n, i);
          r = std::max(r, t.rate);
          py = std::max(py, t.poly);
          w = std::max(w, t.width);
        }
      }
      NK k = NK::Free;
      switch (nd.kind) {
        case NodeKind::Const:
        case NodeKind::Param: k = NK::Free; break;
        case NodeKind::Input: k = driverNode(n, 0) != kNone ? NK::Alias : NK::Free; break;
        case NodeKind::Output:
        case NodeKind::Export: k = NK::Sink; break;
        case NodeKind::Unknown:
        case NodeKind::Equation:
          k = nd.kind == NodeKind::Unknown ? NK::Unknown : NK::Equation;
          if (nd.payload < solves_.size()) {
            (k == NK::Unknown ? solves_[nd.payload].unknowns : solves_[nd.payload].equations).push_back(n);
          } else {
            report(n, ErrorCode::InvalidArgument, label(n) + " belongs to a nonexistent solve block");
          }
          break;
        case NodeKind::Subcircuit:
          report(n, ErrorCode::InvalidArgument, label(n) + " is a subcircuit: schedule the flattened patch (infer())");
          break;
        case NodeKind::Atom: {
          const AtomSignature& sig = atomSignature(nd.atom);
          const KernelMapping& m = kernelMapping(nd.atom);
          switch (m.lowering) {
            case Lowering::Kernel:
              k = NK::Op;
              stateful_[n] = dsp::kernelInfo(m.kernel).stateSize > 0 ? 1 : 0;
              if (!tp_->durationsInSamples && !secondsReported) {
                for (const ArgMap& a : m.args) {
                  if (a.use == ArgUse::InputSeconds) {
                    report(n, ErrorCode::InvalidArgument,
                           "durations are still in seconds: run optimise() before schedule() (" + label(n) + ")");
                    secondsReported = true;
                    break;
                  }
                }
              }
              break;
            case Lowering::Structural: k = NK::Alias; break;
            case Lowering::Constant: k = NK::Free; break;
            case Lowering::Engine:
              if (sig.name.starts_with("voice_")) {
                const bool voiceIn = nd.numInputs > 0 && tp_->inputType(n, 0).poly == Poly::Voice;
                k = voiceIn ? NK::Op : NK::Alias;
                reducer_[n] = voiceIn ? 1 : 0;
                if (voiceIn) {
                  py = Poly::Voice;  // placement: inside the voice stage of its input
                  r = tp_->inputType(n, 0).rate;
                }
              } else if (sig.has(atom_flags::kSideEffect)) {
                k = NK::Op;  // send
                r = Rate::Audio;
              } else {
                k = NK::Free;
              }
              break;
          }
          break;
        }
      }
      if (k == NK::Equation) {
        w = 1;
        for (uint32_t i = 0; i < nd.numInputs; ++i) w = std::max(w, tp_->inputType(n, i).width);
      }
      nk_[n] = k;
      poly_[n] = py;
      rate_[n] = r;
      width_[n] = w;
    }
    // Edge CSR (successors).
    const auto wires = p.wires();
    std::vector<uint32_t> count(N + 1, 0);
    for (const Wire& wr : wires)
      if (wr.from.node < N && wr.to.node < N) ++count[wr.from.node];
    for (const SolveInfo& s : solves_)
      for (NodeIndex e : s.equations) count[e] += static_cast<uint32_t>(s.unknowns.size());
    succBegin_.assign(N + 1, 0);
    for (NodeIndex n = 0; n < N; ++n) succBegin_[n + 1] = succBegin_[n] + count[n];
    succ_.assign(succBegin_[N], Edge{});
    std::vector<uint32_t> fill(succBegin_.begin(), succBegin_.end() - 1);
    for (const Wire& wr : wires) {
      if (wr.from.node >= N || wr.to.node >= N) continue;
      succ_[fill[wr.from.node]++] = Edge{wr.to.node, wr.to.port, edgeFlag(wr.to.node, wr.to.port)};
    }
    for (const SolveInfo& s : solves_)
      for (NodeIndex e : s.equations)
        for (NodeIndex u : s.unknowns) succ_[fill[e]++] = Edge{u, 0, kEdgeSolve};
  }

  // ---- tarjan (iterative) -----------------------------------------------------------------------------------
  void tarjan() {
    const uint32_t N = P().nodeCount();
    std::vector<uint32_t> index(N, kNone), low(N, 0);
    std::vector<uint8_t> onStack(N, 0);
    std::vector<NodeIndex> stack;
    struct Frame {
      NodeIndex n;
      uint32_t e;
    };
    std::vector<Frame> call;
    sccOf_.assign(N, kNone);
    sccBegin_.clear();
    sccNodes_.clear();
    sccBegin_.push_back(0);
    uint32_t next = 0;
    for (NodeIndex root = 0; root < N; ++root) {
      if (index[root] != kNone) continue;
      call.push_back({root, succBegin_[root]});
      index[root] = low[root] = next++;
      stack.push_back(root);
      onStack[root] = 1;
      while (!call.empty()) {
        Frame& f = call.back();
        if (f.e < succBegin_[f.n + 1]) {
          const NodeIndex v = succ_[f.e++].to;
          if (index[v] == kNone) {
            index[v] = low[v] = next++;
            stack.push_back(v);
            onStack[v] = 1;
            call.push_back({v, succBegin_[v]});
          } else if (onStack[v]) {
            low[f.n] = std::min(low[f.n], index[v]);
          }
          continue;
        }
        const NodeIndex n = f.n;
        call.pop_back();
        if (!call.empty()) low[call.back().n] = std::min(low[call.back().n], low[n]);
        if (low[n] == index[n]) {
          const auto c = static_cast<uint32_t>(sccBegin_.size() - 1);
          NodeIndex m;
          do {
            m = stack.back();
            stack.pop_back();
            onStack[m] = 0;
            sccOf_[m] = c;
            sccNodes_.push_back(m);
          } while (m != n);
          sccBegin_.push_back(static_cast<uint32_t>(sccNodes_.size()));
        }
      }
    }
    // Members in ascending node order (determinism), cyclic flags.
    const auto numScc = static_cast<uint32_t>(sccBegin_.size() - 1);
    cyclic_.assign(numScc, 0);
    for (uint32_t c = 0; c < numScc; ++c) {
      if (sccBegin_[c + 1] - sccBegin_[c] > 1) {
        std::sort(sccNodes_.begin() + sccBegin_[c], sccNodes_.begin() + sccBegin_[c + 1]);
        cyclic_[c] = 1;
      } else {
        const NodeIndex n = sccNodes_[sccBegin_[c]];
        for (uint32_t e = succBegin_[n]; e < succBegin_[n + 1]; ++e)
          if (succ_[e].to == n) cyclic_[c] = 1;
      }
    }
  }

  std::span<const NodeIndex> members(uint32_t c) const noexcept {
    return {sccNodes_.data() + sccBegin_[c], sccBegin_[c + 1] - sccBegin_[c]};
  }

  // ---- delay-free cycles --------------------------------------------------------------------------------------
  // A cycle inside SCC c over edges with flag ≤ maxFlag (kEdgeNormal: same-sample edges; kEdgeDelayed: also the
  // delayed inputs without split kernels). Returns the cycle's edges (from, to, input) in order, or empty.
  struct CycleEdge {
    NodeIndex from, to;
    uint16_t input;
  };
  std::vector<CycleEdge> findCycle(uint32_t c, bool withDelayed) {
    auto allowed = [&](const Edge& e) {
      if (sccOf_[e.to] != c) return false;
      return e.flag == kEdgeNormal || (withDelayed && e.flag == kEdgeDelayed);
    };
    for (NodeIndex m : members(c)) color_[m] = 0;
    std::vector<DfsFrame>& st = dfs_;
    st.clear();
    for (NodeIndex root : members(c)) {
      if (color_[root] != 0) continue;
      st.push_back({root, succBegin_[root]});
      color_[root] = 1;
      while (!st.empty()) {
        DfsFrame& f = st.back();
        if (f.e < succBegin_[f.n + 1]) {
          const Edge& e = succ_[f.e++];
          if (!allowed(e)) continue;
          if (color_[e.to] == 1) {
            // Back edge f.n → e.to: the cycle is the stack from e.to up to f.n, closed by this edge.
            std::vector<CycleEdge> cyc;
            size_t k = st.size();
            while (k > 0 && st[k - 1].n != e.to) --k;
            // Each frame's cursor is one past the edge that pushed the next frame.
            for (size_t j = k; j < st.size(); ++j)
              cyc.push_back({st[j - 1].n, st[j].n, succ_[st[j - 1].e - 1].input});
            cyc.push_back({f.n, e.to, e.input});
            // Start at the lowest node index (source order), so messages do not depend on the DFS root.
            const auto lowest = std::min_element(cyc.begin(), cyc.end(),
                                                 [](const CycleEdge& x, const CycleEdge& y) { return x.from < y.from; });
            std::rotate(cyc.begin(), lowest, cyc.end());
            for (NodeIndex m : members(c)) color_[m] = 0;
            return cyc;
          }
          if (color_[e.to] == 0) {
            color_[e.to] = 1;
            st.push_back({e.to, succBegin_[e.to]});
          }
          continue;
        }
        color_[f.n] = 2;
        st.pop_back();
      }
    }
    return {};
  }

  std::string cycleText(const std::vector<CycleEdge>& cyc) const {
    std::string s;
    const size_t shown = std::min<size_t>(cyc.size(), 8);
    for (size_t i = 0; i < shown; ++i) s += label(cyc[i].from) + " → ";
    if (shown < cyc.size()) s += "… → ";
    s += label(cyc.front().from);
    return s;
  }

  void checkCycles(std::vector<std::pair<NodeIndex, uint16_t>>& inserts) {
    const auto numScc = static_cast<uint32_t>(sccBegin_.size() - 1);
    color_.assign(P().nodeCount(), 0);
    for (uint32_t c = 0; c < numScc; ++c) {
      if (!cyclic_[c]) continue;
      // Voice/global: a loop through a reducer would need every voice group and the global part to alternate per
      // sample.
      bool cross = false;
      const NodeIndex first = members(c).front();
      for (NodeIndex m : members(c))
        if (reducer_[m] || poly_[m] != poly_[first]) cross = true;
      if (cross) {
        report(first, ErrorCode::Unsupported,
               "feedback loop through " + label(first) +
                   " crosses the voice/global boundary (a voice reducer inside a loop); reduce outside the loop");
        continue;
      }
      std::vector<CycleEdge> cyc = findCycle(c, false);
      bool delayedOnly = false;
      if (cyc.empty()) {
        cyc = findCycle(c, true);
        delayedOnly = true;
      }
      if (cyc.empty()) continue;
      // Report at the first named node of the cycle (best source position), else where the cycle closes.
      NodeIndex at = cyc.back().to;
      for (const CycleEdge& e : cyc)
        if (!P().nodeName(e.from).empty()) {
          at = e.from;
          break;
        }
      std::string what;
      if (!delayedOnly) {
        what = "delay-free feedback loop " + cycleText(cyc);
      } else {
        std::string via;
        for (const CycleEdge& e : cyc)
          if (edgeFlag(e.to, e.input) == kEdgeDelayed) {
            via = "input '" + std::string(P().inputName(e.to, e.input)) + "' of " + label(e.to);
            break;
          }
        what = "feedback loop " + cycleText(cyc) + " whose only delay is " + via +
               ", which has no split kernel for per-sample scheduling";
      }
      if (opt_.visualMode) {
        const CycleEdge& e = cyc.back();
        inserts.emplace_back(e.to, e.input);
        res_.schedule.warnings.push_back(
            makeDiag(e.to, ErrorCode::Type,
                     what + ": inserted a one-sample delay (z1) before input '" +
                         std::string(P().inputName(e.to, e.input)) + "' of " + label(e.to)));
      } else {
        report(at, ErrorCode::Type,
               what + (delayedOnly ? "; put a z1(…) into the loop"
                                   : " (a loop needs a z1 or a delay line, or a solve block for an implicit equation)"));
      }
    }
  }

  // Visual mode: a z1 in front of each (consumer, input); then the rates downstream are raised to audio where the
  // z1 made them so (a param-rate cycle becomes an audio-rate loop).
  void applyInserts(const std::vector<std::pair<NodeIndex, uint16_t>>& inserts) {
    if (!work_) {
      work_ = std::make_shared<TypedPatch>(*tp_);
      tp_ = work_.get();
    }
    TypedPatch& t = *work_;
    Patch& p = t.patch;
    static const AtomId z1Id = atomId("z1");
    std::vector<NodeIndex> raised;
    for (const auto& [consumer, input] : inserts) {
      WireIndex w = kNone;
      for (WireIndex i = 0; i < p.wireCount(); ++i)
        if (p.wire(i).to == PortRef{consumer, input}) w = i;
      if (w == kNone) continue;
      const PortRef from = p.wire(w).from;
      const PortType driverType = t.types[p.flatOutput(from.node, from.port)];
      p.disconnect(w);
      const NodeIndex z = p.addAtom(z1Id);
      t.types.resize(p.portCount());
      PortType out = driverType;
      out.rate = Rate::Audio;
      t.types[p.flatInput(z, 0)] = driverType;
      t.types[p.flatInput(z, 1)] = portType(Kind::Real, Rate::Const, Poly::Global, kWidthMono);
      t.types[p.flatOutput(z, 0)] = out;
      p.connect(from, PortRef{z, 0});
      p.connect(PortRef{z, 0}, PortRef{consumer, input});
      t.types[p.flatInput(consumer, input)] = out;
      t.stateKey.push_back(mix64(t.stateKey[consumer] ^ mix64(0x7A31u + input)));
      NodeOrigin o = consumer < t.origin.size() ? t.origin[consumer] : NodeOrigin{};
      o.what = NodeOrigin::What::Rewrite;
      t.origin.push_back(o);
      res_.schedule.insertedDelays.push_back({z, consumer, input});
      if (driverType.rate != Rate::Audio) raised.push_back(consumer);
    }
    // Rate propagation: pure kernel/structural atoms take the max of their input rates.
    if (raised.empty()) return;
    const Adjacency adj = p.adjacency();
    while (!raised.empty()) {
      const NodeIndex n = raised.back();
      raised.pop_back();
      const Node& nd = p.node(n);
      if (nd.kind != NodeKind::Atom) continue;
      const KernelMapping& m = kernelMapping(nd.atom);
      const bool pure = (m.lowering == Lowering::Kernel && dsp::kernelInfo(m.kernel).stateSize == 0) ||
                        m.lowering == Lowering::Structural;
      if (!pure) continue;
      Rate r = Rate::Const;
      for (uint32_t i = 0; i < nd.numInputs; ++i) r = std::max(r, t.types[p.flatInput(n, i)].rate);
      for (uint32_t o = 0; o < nd.numOutputs; ++o) {
        PortType& ot = t.types[p.flatOutput(n, o)];
        if (ot.rate >= r) continue;
        ot.rate = r;
        for (WireIndex w : adj.outgoing(n)) {
          const Wire& wr = p.wire(w);
          if (wr.from.port != o) continue;
          t.types[p.flatInput(wr.to.node, wr.to.port)].rate = r;
          raised.push_back(wr.to.node);
        }
      }
    }
  }

  // Which z1 / delay-line nodes of each feedback SCC are split (read … write). T1b.3: when cutting only the delay
  // lines already leaves the SCC acyclic, its z1s stay ordinary (fused) kernels inside the loop: every cycle then
  // passes through a delay line, so the loop runs in sub-blocks of the delay's lower bound instead of per sample
  // (a z1 on such a cycle only adds age). E.g. a comb whose loop holds an FIR made of z1 taps. SCCs with solve
  // blocks, or with a cycle made of z1s alone, split everything as before.
  bool isZ1(NodeIndex n) const noexcept {
    const Node& nd = P().node(n);
    return nd.kind == NodeKind::Atom && kernelMapping(nd.atom).lowering == Lowering::Kernel &&
           kernelMapping(nd.atom).kernel == dsp::Atom::Z1;
  }
  bool acyclicCuttingDelayLinesOnly(uint32_t c) {
    bool anyDelayCut = false;
    for (NodeIndex m : members(c))
      if (nk_[m] == NK::Unknown || nk_[m] == NK::Equation) return false;
    auto kept = [&](const Edge& e) { return sccOf_[e.to] == c && (e.flag != kEdgeSplit || isZ1(e.to)); };
    std::vector<uint32_t> deg(P().nodeCount(), 0);
    for (NodeIndex m : members(c))
      for (uint32_t e = succBegin_[m]; e < succBegin_[m + 1]; ++e) {
        const Edge& ed = succ_[e];
        if (kept(ed)) ++deg[ed.to];
        else if (sccOf_[ed.to] == c) anyDelayCut = true;
      }
    if (!anyDelayCut) return false;
    std::vector<NodeIndex> ready;
    for (NodeIndex m : members(c))
      if (deg[m] == 0) ready.push_back(m);
    size_t done = 0;
    while (!ready.empty()) {
      const NodeIndex m = ready.back();
      ready.pop_back();
      ++done;
      for (uint32_t e = succBegin_[m]; e < succBegin_[m + 1]; ++e)
        if (kept(succ_[e]) && --deg[succ_[e].to] == 0) ready.push_back(succ_[e].to);
    }
    return done == members(c).size();
  }
  void markSplits() {
    split_.assign(P().nodeCount(), 0);
    const auto numScc = static_cast<uint32_t>(sccBegin_.size() - 1);
    std::vector<uint8_t> delayOnly(numScc, 0);
    for (uint32_t c = 0; c < numScc; ++c)
      if (cyclic_[c]) delayOnly[c] = acyclicCuttingDelayLinesOnly(c) ? 1 : 0;
    for (NodeIndex u = 0; u < P().nodeCount(); ++u)
      for (uint32_t e = succBegin_[u]; e < succBegin_[u + 1]; ++e) {
        const Edge& ed = succ_[e];
        if (ed.flag == kEdgeSplit && sccOf_[u] == sccOf_[ed.to] && cyclic_[sccOf_[u]] &&
            !(delayOnly[sccOf_[u]] && isZ1(ed.to)))
          split_[ed.to] = 1;
      }
  }

  // ---- solve blocks ---------------------------------------------------------------------------------------
  void checkSolves() {
    const uint32_t N = P().nodeCount();
    bodyOf_.assign(N, kNone);
    std::vector<uint8_t> fwd(N), bwd(N);
    std::vector<NodeIndex> queue;
    for (uint32_t s = 0; s < solves_.size(); ++s) {
      SolveInfo& si = solves_[s];
      if (si.unknowns.empty() && si.equations.empty()) continue;
      if (si.unknowns.empty() || si.unknowns.size() != si.equations.size()) {
        report(si.unknowns.empty() ? si.equations.front() : si.unknowns.front(), ErrorCode::InvalidArgument,
               "solve block #" + std::to_string(s) + " has " + std::to_string(si.unknowns.size()) +
                   " unknowns but " + std::to_string(si.equations.size()) + " equations");
        continue;
      }
      std::fill(fwd.begin(), fwd.end(), uint8_t{0});
      std::fill(bwd.begin(), bwd.end(), uint8_t{0});
      queue.assign(si.unknowns.begin(), si.unknowns.end());
      for (NodeIndex u : si.unknowns) fwd[u] = 1;
      while (!queue.empty()) {
        const NodeIndex n = queue.back();
        queue.pop_back();
        for (uint32_t e = succBegin_[n]; e < succBegin_[n + 1]; ++e) {
          const Edge& ed = succ_[e];
          if (ed.flag != kEdgeNormal || fwd[ed.to]) continue;
          fwd[ed.to] = 1;
          queue.push_back(ed.to);
        }
      }
      queue.assign(si.equations.begin(), si.equations.end());
      for (NodeIndex q : si.equations) bwd[q] = 1;
      while (!queue.empty()) {
        const NodeIndex n = queue.back();
        queue.pop_back();
        for (uint32_t i = 0; i < P().node(n).numInputs; ++i) {
          const NodeIndex d = driverNode(n, i);
          if (d == kNone || bwd[d] || edgeFlag(n, i) != kEdgeNormal) continue;
          bwd[d] = 1;
          queue.push_back(d);
        }
      }
      bool bad = false;
      for (NodeIndex u : si.unknowns)
        if (!bwd[u]) {
          report(u, ErrorCode::InvalidArgument,
                 "unknown " + label(u) + " is not read by any equation of its solve block (within the same sample)");
          bad = true;
        }
      for (NodeIndex q : si.equations)
        if (!fwd[q]) {
          report(q, ErrorCode::InvalidArgument,
                 "an equation of solve block #" + std::to_string(s) + " does not depend on any of its unknowns");
          bad = true;
        }
      if (bad) continue;
      si.body.clear();
      for (NodeIndex n = 0; n < N; ++n) {
        if (!fwd[n] || !bwd[n]) continue;
        if (bodyOf_[n] != kNone && bodyOf_[n] != s) {
          report(n, ErrorCode::InvalidArgument,
                 label(n) + " is part of two solve blocks (#" + std::to_string(bodyOf_[n]) + " and #" +
                     std::to_string(s) + "): solve them as one block");
          bad = true;
          continue;
        }
        bodyOf_[n] = s;
        si.body.push_back(n);
        if (nk_[n] == NK::Op && stateful_[n] && !split_[n])
          report(n, ErrorCode::InvalidArgument,
                 "stateful " + label(n) + " is inside the equations of solve block #" + std::to_string(s) +
                     " (it would advance once per Newton iteration); move it out of the equations");
        if (nk_[n] == NK::Op && reducer_[n])
          report(n, ErrorCode::Unsupported, "voice reducer " + label(n) + " inside a solve block");
      }
      for (NodeIndex u : si.unknowns) {
        const NodeIndex d = driverNode(u, 0);
        if (d != kNone && fwd[d] && bwd[d])
          report(u, ErrorCode::InvalidArgument,
                 "the initial guess of unknown " + label(u) + " depends on its own solve block");
      }
    }
  }

  // ---- stages, classes, sub-blocks ----------------------------------------------------------------------------
  bool isOpish(NodeIndex n) const noexcept { return nk_[n] == NK::Op || nk_[n] == NK::Unknown || nk_[n] == NK::Equation; }

  void assignStages() {
    const uint32_t N = P().nodeCount();
    const auto numScc = static_cast<uint32_t>(sccBegin_.size() - 1);
    sccStage_.assign(numScc, -1);
    sccClass_.assign(numScc, kItemFree);
    sccSub_.assign(numScc, 0);
    outStage_.assign(N, -1);
    ctl_.assign(N, 0);
    // Tarjan emits SCCs in reverse topological order.
    for (uint32_t c = numScc; c-- > 0;) {
      const auto mem = members(c);
      bool anyOp = false, anyNonFree = false;
      Poly py = Poly::Global;
      for (NodeIndex m : mem) {
        if (isOpish(m)) anyOp = true;
        if (nk_[m] != NK::Free) {
          anyNonFree = true;
          py = poly_[m];
        }
      }
      // Control: param/const rate, not stateful, and everything it reads (outside the SCC) is control too.
      bool ctl = true;
      int64_t stage = py == Poly::Voice ? 1 : 0;
      for (NodeIndex m : mem) {
        if (rate_[m] > Rate::Param) ctl = false;
        if (nk_[m] == NK::Op && stateful_[m]) ctl = false;
        for (uint32_t i = 0; i < P().node(m).numInputs; ++i) {
          const NodeIndex u = driverNode(m, i);
          if (u == kNone || sccOf_[u] == c) continue;
          if (!ctl_[u]) ctl = false;
          if (nk_[u] == NK::Free) continue;
          stage = std::max<int64_t>(stage, roundUpStage(outStage_[u], py));
        }
      }
      for (NodeIndex m : mem) ctl_[m] = ctl ? 1 : 0;
      if (!anyNonFree) {
        sccStage_[c] = -1;
        sccClass_[c] = kItemFree;
        continue;
      }
      sccStage_[c] = static_cast<int32_t>(stage);
      for (NodeIndex m : mem) outStage_[m] = static_cast<int32_t>(stage + (reducer_[m] ? 1 : 0));
      if (!anyOp) sccClass_[c] = kItemFree;
      else if (ctl) sccClass_[c] = kItemControl;
      else if (cyclic_[c]) sccClass_[c] = kItemLoop;
      else sccClass_[c] = kItemBlock;
      if (sccClass_[c] == kItemLoop) sccSub_[c] = subBlockOf(c);
    }
  }

  // Lower bound of an output's value (−inf if unknown), for delay times (samples). Computed in float32 with the
  // runtime's operations (constants and `sr` are float constants, params are clamped to float(rangeMin), mul/add
  // round to nearest, muladd is an fma), so it is the smallest value the program can actually produce: a bound in
  // double could round above it (M1 review M10: 20.75 ms · 48 kHz = 996 in double, 995.99994 at runtime). An add
  // of a product may be contracted into an fma by the JIT; both roundings are considered.
  float lowerBound(NodeIndex n, int depth) const {
    constexpr float kUnknown = -std::numeric_limits<float>::infinity();
    if (depth > 16) return kUnknown;
    const Node& nd = P().node(n);
    switch (nd.kind) {
      case NodeKind::Const: return static_cast<float>(P().constant(n).value());
      case NodeKind::Param: {
        const ParamDecl& d = P().paramDecl(n);
        return opt_.trustParamRanges && d.kind == Kind::Real && d.hasRange ? static_cast<float>(d.rangeMin.value())
                                                                          : kUnknown;
      }
      case NodeKind::Atom: break;
      default: return kUnknown;
    }
    const std::string_view name = atomSignature(nd.atom).name;
    if (name == "sr") return static_cast<float>(opt_.sampleRate);
    auto driver = [&](uint32_t i) -> NodeIndex {
      const WireIndex w = adj_.driver[P().flatInput(n, i)];
      return w == kNone ? kNone : P().wire(w).from.node;
    };
    auto in = [&](uint32_t i) -> float {
      const NodeIndex d = driver(i);
      if (d == kNone) {
        const AtomSignature& sig = atomSignature(nd.atom);
        return static_cast<float>(sig.inputs[specIndex(sig, i)].defaultValue);
      }
      return lowerBound(d, depth + 1);
    };
    if (name == "mul") {
      const float a = in(0), b = in(1);
      return a >= 0 && b >= 0 ? a * b : kUnknown;
    }
    if (name == "add") {
      const float a = in(0), b = in(1);
      float r = a + b;
      // The fma a contracted mul + add would compute instead (non-negative factors only, as for mul).
      for (uint32_t i = 0; i < 2; ++i) {
        const NodeIndex d = driver(i);
        if (d == kNone || P().node(d).kind != NodeKind::Atom || atomSignature(P().node(d).atom).name != "mul") continue;
        auto din = [&](uint32_t j) -> float {
          const WireIndex w = adj_.driver[P().flatInput(d, j)];
          if (w == kNone) {
            const AtomSignature& sig = atomSignature(P().node(d).atom);
            return static_cast<float>(sig.inputs[specIndex(sig, j)].defaultValue);
          }
          return lowerBound(P().wire(w).from.node, depth + 2);
        };
        const float x = din(0), y = din(1);
        if (x >= 0 && y >= 0) r = std::min(r, std::fma(x, y, i == 0 ? b : a));
      }
      return std::isnan(r) ? kUnknown : r;
    }
    if (name == "max") return std::max(in(0), in(1));
    // a − constant (T1b.3: `time − 1/sr`): the runtime's float subtraction is monotonic in a.
    if (name == "sub") {
      const NodeIndex d = driver(1);
      if (d == kNone || P().node(d).kind != NodeKind::Const) return kUnknown;
      const float r = in(0) - static_cast<float>(P().constant(d).value());
      return std::isnan(r) ? kUnknown : r;
    }
    if (name == "muladd") {
      const float a = in(0), b = in(1), c = in(2);
      const float r = std::fma(a, b, c);
      return a >= 0 && b >= 0 && !std::isnan(r) ? r : kUnknown;
    }
    return kUnknown;
  }

  // Frames per loop iteration: 1 with any split z1 or a solve; else the smallest guaranteed tap age of the split
  // delay lines, evaluated exactly as the read kernel does in float32 (pq/dsp kernels_memory.cpp delayTap:
  // d = min(max(time, minimum), max frames); linear newest tap = ⌊d⌋, cubic ⌊d⌋ − 1, allpass ⌊d⌋ − 1 when the
  // fraction is below 0.5, else ⌊d⌋).
  uint32_t subBlockOf(uint32_t c) const {
    uint32_t sub = kMaxSubBlock;
    for (NodeIndex m : members(c)) {
      if (nk_[m] == NK::Unknown || nk_[m] == NK::Equation) return 1;
      if (!split_[m]) continue;
      const Node& nd = P().node(m);
      const dsp::Atom k = kernelMapping(nd.atom).kernel;
      if (k == dsp::Atom::Z1) return 1;
      const WireIndex tw = adj_.driver[P().flatInput(m, 1)];
      float lb = -std::numeric_limits<float>::infinity();
      if (tw != kNone) lb = lowerBound(P().wire(tw).from.node, 0);
      const float minD = k == dsp::Atom::DelayLinear ? 1.0f : k == dsp::Atom::DelayCubic ? 2.0f : 1.5f;
      float d = std::isfinite(lb) ? std::max(lb, minD) : minD;
      d = std::min(d, static_cast<float>(maxDelayFramesOf(m)));
      const float fl = std::floor(d);
      const float age = k == dsp::Atom::DelayLinear ? fl
                        : k == dsp::Atom::DelayCubic ? fl - 1.0f
                                                     : (d - fl < 0.5f ? fl - 1.0f : fl);
      const uint32_t a = !(age >= 1.0f) ? 1u : age > static_cast<float>(kMaxSubBlock) ? kMaxSubBlock
                                                                                      : static_cast<uint32_t>(age);
      sub = std::min(sub, a);
    }
    return sub == kMaxSubBlock ? 1 : sub;
  }

  // ---- order --------------------------------------------------------------------------------------------------
  // Ready SCCs are taken by (stage, class) and then most recently readied first: depth-first, so a value is
  // consumed soon after it is produced and few buffers are live. Block and loop items are one class here: preferring
  // every ready block item over any loop would pull independent branches forward past the loops that consume them.
  struct Ready {
    int32_t stage;
    uint32_t cls;
    uint32_t seq;
    uint32_t scc;
  };
  struct ReadyLess {  // priority_queue keeps the "largest" on top: invert for (stage, cls) min, seq max
    bool operator()(const Ready& a, const Ready& b) const noexcept {
      if (a.stage != b.stage) return a.stage > b.stage;
      if (a.cls != b.cls) return a.cls > b.cls;
      return a.seq < b.seq;
    }
  };

  void order() {
    const uint32_t N = P().nodeCount();
    const auto numScc = static_cast<uint32_t>(sccBegin_.size() - 1);
    std::vector<uint32_t> indeg(numScc, 0);
    for (NodeIndex u = 0; u < N; ++u)
      for (uint32_t e = succBegin_[u]; e < succBegin_[u + 1]; ++e)
        if (sccOf_[succ_[e].to] != sccOf_[u]) ++indeg[sccOf_[succ_[e].to]];
    std::priority_queue<Ready, std::vector<Ready>, ReadyLess> ready;
    uint32_t seq = 0;
    auto push = [&](uint32_t c) { ready.push({sccStage_[c], std::min(sccClass_[c], kItemBlock), seq++, c}); };
    for (uint32_t c = numScc; c-- > 0;)
      if (indeg[c] == 0) push(c);
    sccOrder_.clear();
    sccOrder_.reserve(numScc);
    while (!ready.empty()) {
      const uint32_t c = ready.top().scc;
      ready.pop();
      sccOrder_.push_back(c);
      for (NodeIndex u : members(c))
        for (uint32_t e = succBegin_[u]; e < succBegin_[u + 1]; ++e) {
          const uint32_t d = sccOf_[succ_[e].to];
          if (d != c && --indeg[d] == 0) push(d);
        }
    }
    // Steps: nodes in order; cyclic SCCs expanded by their internal order (solve bodies contiguous).
    steps_.clear();
    steps_.reserve(N);
    itemMark_.assign(N + solves_.size(), 0);
    itemDeg_.assign(N + solves_.size(), 0);
    for (uint32_t c : sccOrder_) {
      if (!cyclic_[c]) {
        steps_.push_back({members(c).front(), c});
        continue;
      }
      internalOrder(c);
    }
  }

  uint32_t itemOf(NodeIndex n) const noexcept {
    return bodyOf_[n] != kNone ? P().nodeCount() + bodyOf_[n] : n;
  }

  // Kahn over the members of cyclic SCC c on same-sample edges (split and solve edges removed; the input edge of an
  // unsplit z1 is an ordinary dependency), solve bodies
  // contracted to one item; then each body in its own Kahn order (its unknowns first). Min node index first.
  void internalOrder(uint32_t c) {
    const uint32_t N = P().nodeCount();
    std::vector<uint32_t>& items = items_;
    items.clear();
    for (NodeIndex m : members(c)) {
      const uint32_t it = itemOf(m);
      if (!itemMark_[it]) {
        itemMark_[it] = 1;
        items.push_back(it);
      }
    }
    for (uint32_t it : items) itemMark_[it] = 0;
    auto kahn = [&](bool withDelayed) {
      auto inLoop = [&](const Edge& e) {
        return sccOf_[e.to] == c && (e.flag == kEdgeNormal || (withDelayed && e.flag == kEdgeDelayed) ||
                                     (e.flag == kEdgeSplit && !split_[e.to]));  // an unsplit z1 (markSplits)
      };
      for (uint32_t it : items) itemDeg_[it] = 0;
      for (NodeIndex m : members(c))
        for (uint32_t e = succBegin_[m]; e < succBegin_[m + 1]; ++e) {
          const Edge& ed = succ_[e];
          if (!inLoop(ed)) continue;
          const uint32_t a = itemOf(m), b = itemOf(ed.to);
          if (a != b) ++itemDeg_[b];
        }
      // Min-heap of ready items on a reused vector.
      std::vector<uint32_t>& heap = heap_;
      heap.clear();
      auto pushReady = [&](uint32_t it) {
        heap.push_back(it);
        std::push_heap(heap.begin(), heap.end(), std::greater<>());
      };
      for (uint32_t it : items)
        if (itemDeg_[it] == 0) pushReady(it);
      std::vector<uint32_t>& seq = seq_;
      seq.clear();
      auto release = [&](NodeIndex m) {
        for (uint32_t e = succBegin_[m]; e < succBegin_[m + 1]; ++e) {
          const Edge& ed = succ_[e];
          if (!inLoop(ed)) continue;
          const uint32_t a = itemOf(m), b = itemOf(ed.to);
          if (a != b && --itemDeg_[b] == 0) pushReady(b);
        }
      };
      while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), std::greater<>());
        const uint32_t it = heap.back();
        heap.pop_back();
        seq.push_back(it);
        if (it < N) release(it);
        else
          for (NodeIndex m : solves_[it - N].body) release(m);
      }
      return seq.size();
    };
    const size_t done = kahn(true);
    const std::vector<uint32_t>& seq = seq_;  // kahn(false) below only runs on the error path
    if (done != items.size()) {
      // A cycle through a contracted solve body. Without the delayed inputs that have no split kernel it would be
      // schedulable: the loop's only delay is such an input (like checkCycles' second case, but through a solve).
      uint32_t solveIt = kNone;
      for (uint32_t it : items)
        if (it >= N && std::find(seq.begin(), seq.end(), it) == seq.end()) solveIt = it;
      if (kahn(false) == items.size()) {
        std::string via;
        for (NodeIndex m : members(c))
          for (uint32_t e = succBegin_[m]; e < succBegin_[m + 1] && via.empty(); ++e)
            if (succ_[e].flag == kEdgeDelayed && sccOf_[succ_[e].to] == c)
              via = "input '" + std::string(P().inputName(succ_[e].to, succ_[e].input)) + "' of " + label(succ_[e].to);
        const NodeIndex at = solveIt != kNone ? solves_[solveIt - N].unknowns.front() : members(c).front();
        report(at, ErrorCode::Type,
               "feedback loop through solve block #" + std::to_string(solveIt != kNone ? solveIt - N : 0) +
                   " whose only delay is " + via + ", which has no split kernel for per-sample scheduling; put a "
                   "z1(…) into the loop");
        return;
      }
      if (solveIt != kNone) {
        report(solves_[solveIt - N].unknowns.front(), ErrorCode::InvalidArgument,
               "solve block #" + std::to_string(solveIt - N) +
                   " and another solve block depend on each other within one sample: write them as one block");
        return;
      }
      report(members(c).front(), ErrorCode::Unknown, "scheduler: no order for the loop at " + label(members(c).front()));
      return;
    }
    for (uint32_t it : seq) {
      if (it < N) {
        steps_.push_back({it, c});
        continue;
      }
      SolveInfo& si = solves_[it - N];
      orderBody(si);
      for (NodeIndex m : si.body) steps_.push_back({m, c});
    }
  }

  void orderBody(SolveInfo& si) {
    const uint32_t s = static_cast<uint32_t>(&si - solves_.data());
    auto inBody = [&](const Edge& e) {
      return bodyOf_[e.to] == s && (e.flag == kEdgeNormal || e.flag == kEdgeDelayed);
    };
    for (NodeIndex m : si.body) itemDeg_[m] = 0;
    for (NodeIndex m : si.body)
      for (uint32_t e = succBegin_[m]; e < succBegin_[m + 1]; ++e)
        if (inBody(succ_[e])) ++itemDeg_[succ_[e].to];
    std::priority_queue<NodeIndex, std::vector<NodeIndex>, std::greater<>> ready;
    for (NodeIndex m : si.body)
      if (itemDeg_[m] == 0) ready.push(m);
    std::vector<NodeIndex> out;
    out.reserve(si.body.size());
    while (!ready.empty()) {
      const NodeIndex m = ready.top();
      ready.pop();
      out.push_back(m);
      for (uint32_t e = succBegin_[m]; e < succBegin_[m + 1]; ++e)
        if (inBody(succ_[e]) && --itemDeg_[succ_[e].to] == 0) ready.push(succ_[e].to);
    }
    si.body = std::move(out);
  }

  // ---- tables: params, root inputs ------------------------------------------------------------------------
  void assignTables() {
    const uint32_t N = P().nodeCount();
    paramSlot_.assign(N, kNone);
    inputOrdinal_.assign(N, kNone);
    uint32_t inputs = 0;
    for (NodeIndex n = 0; n < N; ++n) {
      const Node& nd = P().node(n);
      if (nd.kind == NodeKind::Input) inputOrdinal_[n] = inputs++;
      if (nd.kind != NodeKind::Param) continue;
      const ParamDecl& d = P().paramDecl(n);
      ParamSlot ps;
      ps.node = n;
      ps.key = tp_->stateKey[n];
      ps.kind = d.kind;
      ps.defaultValue = static_cast<float>(d.defaultValue.value());
      ps.hasRange = d.hasRange;
      if (d.hasRange) {
        ps.rangeMin = static_cast<float>(d.rangeMin.value());
        ps.rangeMax = static_cast<float>(d.rangeMax.value());
      }
      paramSlot_[n] = static_cast<uint32_t>(S().params.size());
      S().params.push_back(ps);
    }
  }

  // ---- values ---------------------------------------------------------------------------------------------
  Operand constOperand(float v, Kind k) {
    const uint64_t key = (static_cast<uint64_t>(k) << 32) | std::bit_cast<uint32_t>(v);
    const auto [it, inserted] = constIndex_.try_emplace(key, static_cast<uint32_t>(S().constants.size()));
    if (inserted) S().constants.push_back({v, k});
    return {Operand::Kind::Const, it->second};
  }
  Operand constOperandOf(double v, Kind k) {
    if (k == Kind::Bool) return constOperand(dsp::boolMask(v != 0.0), Kind::Bool);
    return constOperand(static_cast<float>(v), Kind::Real);
  }

  Operand newVal(bool pattern, uint8_t lanes, Poly poly) {
    Val v;
    v.pattern = pattern;
    v.lanes = pattern ? uint8_t{8} : lanes;
    v.poly = poly;
    vals_.push_back(v);
    return {Operand::Kind::Buffer, static_cast<uint32_t>(vals_.size() - 1)};
  }

  Operand external(External e) {
    S().externals.push_back(e);
    return {Operand::Kind::External, static_cast<uint32_t>(S().externals.size() - 1)};
  }

  Operand outOperand(NodeIndex n, uint32_t o, uint32_t ch) {
    const uint32_t fo = P().flatOutput(n, o);
    const uint32_t base = outBase_[fo];
    if (base == kNone) {
      report(n, ErrorCode::Unknown, "scheduler: " + label(n) + " is read before it is computed");
      return {};
    }
    const uint32_t w = std::max<uint32_t>(1, tp_->types[fo].width);
    return outOps_[base + std::min(ch, w - 1)];
  }

  Operand defaultOperand(NodeIndex n, uint32_t i) {
    const Node& nd = P().node(n);
    if (nd.kind == NodeKind::Atom) {
      const AtomSignature& sig = atomSignature(nd.atom);
      const PortSpec& spec = sig.inputs[specIndex(sig, i)];
      const Kind k = tp_->inputType(n, i).kind == Kind::Bool ? Kind::Bool : Kind::Real;
      return constOperandOf(spec.defaultValue, k);
    }
    return constOperand(0.0f, Kind::Real);
  }

  Operand inputOperand(NodeIndex n, uint32_t i, uint32_t ch) {
    const WireIndex w = adj_.driver[P().flatInput(n, i)];
    if (w == kNone) return defaultOperand(n, i);
    const PortRef from = P().wire(w).from;
    return outOperand(from.node, from.port, ch);
  }

  void setOut(NodeIndex n, uint32_t o, std::span<const Operand> ops) {
    outBase_[P().flatOutput(n, o)] = static_cast<uint32_t>(outOps_.size());
    outOps_.insert(outOps_.end(), ops.begin(), ops.end());
  }

  // A constant-valued input (state init, resources): a literal, a default, or `sr`; else an error.
  bool constInput(NodeIndex n, uint32_t i, double& v, Unit* unit = nullptr) {
    const WireIndex w = adj_.driver[P().flatInput(n, i)];
    if (unit) *unit = Unit::None;
    if (w == kNone) {
      const AtomSignature& sig = atomSignature(P().node(n).atom);
      v = sig.inputs[specIndex(sig, i)].defaultValue;
      return true;
    }
    const NodeIndex d = P().wire(w).from.node;
    const Node& dn = P().node(d);
    if (dn.kind == NodeKind::Const) {
      v = P().constant(d).value();
      if (unit) *unit = P().constant(d).unit;
      return true;
    }
    if (dn.kind == NodeKind::Atom && kernelMapping(dn.atom).lowering == Lowering::Constant) {
      v = opt_.sampleRate;
      return true;
    }
    report(n, ErrorCode::Type,
           "input '" + std::string(P().inputName(n, i)) + "' of " + label(n) +
               " must be a constant after optimisation (it sizes or selects state/tables)");
    return false;
  }

  void assignValues() {
    const Patch& p = P();
    outBase_.assign(p.portCount(), kNone);
    outOps_.clear();
    vals_.clear();
    std::vector<Operand> tmp;
    for (const Step& st : steps_) {
      const NodeIndex n = st.node;
      const Node& nd = p.node(n);
      const uint16_t w = width_[n];
      switch (nk_[n]) {
        case NK::Sink:
        case NK::Equation: break;
        case NK::Unknown: {
          tmp.clear();
          for (uint32_t ch = 0; ch < w; ++ch)
            tmp.push_back(newVal(ctl_[n] != 0, poly_[n] == Poly::Voice ? 8 : 1, poly_[n]));
          setOut(n, 0, tmp);
          break;
        }
        case NK::Free: {
          for (uint32_t o = 0; o < nd.numOutputs; ++o) {
            tmp.clear();
            const PortType& t = tp_->outputType(n, o);
            for (uint32_t ch = 0; ch < std::max<uint16_t>(1, t.width); ++ch) {
              switch (nd.kind) {
                case NodeKind::Const: {
                  const Constant& c = p.constant(n);
                  tmp.push_back(constOperandOf(c.value(), c.kind));
                  break;
                }
                case NodeKind::Param: tmp.push_back({Operand::Kind::Param, paramSlot_[n]}); break;
                case NodeKind::Input: {
                  External e;
                  e.source = External::Source::RootInput;
                  e.index = inputOrdinal_[n];
                  e.channel = static_cast<uint16_t>(ch);
                  e.rate = t.rate;
                  e.poly = t.poly;
                  e.node = n;
                  tmp.push_back(external(e));
                  break;
                }
                default: {  // engine source or sr
                  if (kernelMapping(nd.atom).lowering == Lowering::Constant) {
                    tmp.push_back(constOperandOf(opt_.sampleRate, Kind::Real));
                    break;
                  }
                  External e;
                  e.source = External::Source::Engine;
                  e.atom = nd.atom;
                  e.channel = static_cast<uint16_t>(ch);
                  e.rate = t.rate;
                  e.poly = t.poly;
                  e.node = n;
                  tmp.push_back(external(e));
                  break;
                }
              }
            }
            setOut(n, o, tmp);
          }
          break;
        }
        case NK::Alias: {
          tmp.clear();
          const uint16_t ow = std::max<uint16_t>(1, tp_->outputType(n, 0).width);
          {
            const std::string_view name = nd.kind == NodeKind::Atom ? atomSignature(nd.atom).name : std::string_view{};
            if (name == "channel") {
              double idx = 0;
              if (!constInput(n, 1, idx)) return;
              const uint32_t xw = std::max<uint32_t>(1, tp_->inputType(n, 0).width);
              const uint32_t c = xw == 1 ? 0 : static_cast<uint32_t>(std::clamp(idx, 0.0, xw - 1.0));
              tmp.push_back(inputOperand(n, 0, c));
            } else if (name == "pack") {
              for (uint32_t i = 0; i < nd.numInputs; ++i) {
                const uint32_t iw = std::max<uint32_t>(1, tp_->inputType(n, i).width);
                for (uint32_t c = 0; c < iw; ++c) tmp.push_back(inputOperand(n, i, c));
              }
              tmp.resize(ow, tmp.empty() ? constOperand(0.0f, Kind::Real) : tmp.back());
            } else {  // `in` with a default, a reducer of a global value: the input as is
              for (uint32_t c = 0; c < ow; ++c) tmp.push_back(inputOperand(n, 0, c));
            }
          }
          setOut(n, 0, tmp);
          break;
        }
        case NK::Op: {
          if (reducer_[n]) {
            tmp.clear();
            const uint16_t ow = std::max<uint16_t>(1, tp_->outputType(n, 0).width);
            for (uint32_t c = 0; c < ow; ++c) tmp.push_back(newVal(ctl_[n] != 0, 1, Poly::Global));
            setOut(n, 0, tmp);
            break;
          }
          for (uint32_t o = 0; o < nd.numOutputs; ++o) {
            tmp.clear();
            const uint16_t ow = std::max<uint16_t>(1, tp_->outputType(n, o).width);
            for (uint32_t c = 0; c < ow; ++c)
              tmp.push_back(newVal(ctl_[n] != 0, poly_[n] == Poly::Voice ? 8 : 1, poly_[n]));
            setOut(n, o, tmp);
          }
          break;
        }
      }
      if (!res_.ok()) return;
    }
  }

  // ---- broadcasts (global audio → voice ops) -------------------------------------------------------------
  bool isPatternLike(const Operand& o) const noexcept {
    switch (o.kind) {
      case Operand::Kind::Param:
      case Operand::Kind::Const: return true;
      case Operand::Kind::External: return S_const().externals[o.index].rate <= Rate::Param;
      case Operand::Kind::Buffer: return vals_[o.index].pattern;
      default: return false;
    }
  }
  const Schedule& S_const() const noexcept { return res_.schedule; }

  bool isGlobalBlock(const Operand& o) const noexcept {
    if (o.kind == Operand::Kind::External) {
      const External& e = S_const().externals[o.index];
      return e.rate == Rate::Audio && e.poly == Poly::Global;
    }
    if (o.kind == Operand::Kind::Buffer) return !vals_[o.index].pattern && vals_[o.index].lanes == 1;
    return false;
  }

  void collectBroadcasts() {
    needs_.clear();
    auto need = [&](uint32_t stage, const Operand& o) {
      if (!isGlobalBlock(o)) return;
      auto& v = needs_[stage];
      if (std::find(v.begin(), v.end(), o) == v.end()) v.push_back(o);
    };
    for (const Step& st : steps_) {
      const NodeIndex n = st.node;
      const uint32_t cls = sccClass_[st.scc];
      if (cls < kItemBlock || poly_[n] != Poly::Voice || reducer_[n]) continue;
      const auto stage = static_cast<uint32_t>(sccStage_[st.scc]);
      const Node& nd = P().node(n);
      if (nk_[n] == NK::Op || nk_[n] == NK::Equation) {
        for (uint32_t i = 0; i < nd.numInputs; ++i)
          for (uint32_t ch = 0; ch < width_[n]; ++ch) need(stage, inputOperand(n, i, ch));
      } else if (nk_[n] == NK::Unknown && driverNode(n, 0) != kNone) {
        for (uint32_t ch = 0; ch < width_[n]; ++ch) need(stage, inputOperand(n, 0, ch));
      }
    }
  }

  // ---- emission -----------------------------------------------------------------------------------------------
  void openRegion(uint32_t stage, RegionKind kind, uint32_t sub) {
    Schedule& s = S();
    if (!s.regions.empty()) {
      const Region& r = s.regions.back();
      if (r.stage == stage && r.kind == kind && r.subBlock == sub) return;
    }
    if (s.stages.empty() || s.stages.back().index != stage) {
      if ((stage & 1) && !broadcastDone_.count(stage)) {
        broadcastDone_.insert({stage, uint8_t{1}});
        emitBroadcasts(stage);
        if (!s.regions.empty()) {
          const Region& r = s.regions.back();
          if (r.stage == stage && r.kind == kind && r.subBlock == sub) return;
        }
      }
      if (s.stages.empty() || s.stages.back().index != stage) {
        Stage sg;
        sg.poly = (stage & 1) ? Poly::Voice : Poly::Global;
        sg.index = stage;
        sg.firstRegion = static_cast<uint32_t>(s.regions.size());
        sg.firstOp = static_cast<uint32_t>(s.ops.size());
        s.stages.push_back(sg);
      }
    }
    Region r;
    r.kind = kind;
    r.poly = (stage & 1) ? Poly::Voice : Poly::Global;
    r.stage = stage;
    r.firstOp = static_cast<uint32_t>(s.ops.size());
    r.subBlock = sub;
    r.firstSolve = static_cast<uint32_t>(s.solves.size());
    s.regions.push_back(r);
    regionStage_.push_back(static_cast<uint32_t>(s.stages.size() - 1));
  }

  void emitBroadcasts(uint32_t voiceStage) {
    auto it = needs_.find(voiceStage);
    if (it == needs_.end() || it->second.empty()) return;
    openRegion(voiceStage - 1, RegionKind::Block, 0);
    for (const Operand& o : it->second) {
      if (broadcastOf_.count(operandKey(o))) continue;
      const Operand out = newVal(false, 8, Poly::Global);
      Op op;
      op.kind = OpKind::Broadcast;
      op.lanes = 8;
      op.node = o.kind == Operand::Kind::External ? S().externals[o.index].node
                : o.kind == Operand::Kind::Buffer   ? vals_[o.index].node
                                                    : kNone;
      pushOp(op, {&o, 1}, {&out, 1});
      broadcastOf_[operandKey(o)] = out;
    }
  }

  uint32_t curRegion() const noexcept { return static_cast<uint32_t>(S_const().regions.size() - 1); }

  void use(const Operand& o, uint32_t pos, bool after = false) {
    if (o.kind == Operand::Kind::Buffer) uses_.push_back({o.index, pos, curRegion(), curSolve_, after});
  }

  uint32_t pushOp(Op op, std::span<const Operand> ins, std::span<const Operand> outs) {
    Schedule& s = S();
    const auto pos = static_cast<uint32_t>(s.ops.size());
    op.firstOperand = static_cast<uint32_t>(s.operands.size());
    op.numIn = static_cast<uint16_t>(ins.size());
    op.numOut = static_cast<uint16_t>(outs.size());
    s.operands.insert(s.operands.end(), ins.begin(), ins.end());
    s.operands.insert(s.operands.end(), outs.begin(), outs.end());
    s.ops.push_back(op);
    for (const Operand& o : ins) use(o, pos);
    for (const Operand& o : outs)
      if (o.kind == Operand::Kind::Buffer && vals_[o.index].def == kNone) {
        vals_[o.index].def = pos;
        vals_[o.index].defStage = s.regions.back().stage;
        vals_[o.index].node = op.node;
      }
    return pos;
  }

  // An operand for an input of an op with `lanes` lanes in the current region: Spl (pattern) or Blk, with a fill
  // (pattern → block) when the kernel input cannot be Spl, or the broadcast copy of a global block for a voice op.
  Operand adapt(const Operand& o, uint8_t lanes, bool control, bool splOk, bool& spl, NodeIndex node) {
    spl = false;
    if (control) return o;
    if (isPatternLike(o)) {
      if (splOk) {
        spl = true;
        return o;
      }
      const Operand out = newVal(false, lanes, lanes == 8 ? Poly::Voice : Poly::Global);
      Op f;
      f.kind = OpKind::Kernel;
      f.kernel = dsp::Atom::Const;
      f.lanes = lanes;
      f.splMask = 1;
      f.node = node;
      pushOp(f, {&o, 1}, {&out, 1});
      return out;
    }
    if (lanes == 8 && isGlobalBlock(o)) {
      auto it = broadcastOf_.find(operandKey(o));
      if (it == broadcastOf_.end()) {
        report(node, ErrorCode::Unknown, "scheduler: missing broadcast for an input of " + label(node));
        return o;
      }
      return it->second;
    }
    return o;
  }

  uint32_t stateEntry(NodeIndex n, uint16_t ch, StateTag tag, uint32_t size, const dsp::StateInit& init, Poly arena) {
    PendingState ps{tp_->stateKey[n], ch, arena, tag, size, init, n};
    pending_.push_back(ps);
    return static_cast<uint32_t>(pending_.size() - 1);
  }

  // Kernel atom node n: one op per channel (a read now and a write queued for the loop's end when split).
  void emitKernel(NodeIndex n, bool control) {
    const Node& nd = P().node(n);
    const KernelMapping& m = kernelMapping(nd.atom);
    const dsp::KernelInfo& info = dsp::kernelInfo(m.kernel);
    const uint8_t lanes = control || poly_[n] == Poly::Voice ? 8 : 1;
    const bool isSplit = split_[n] != 0;
    // Registry inputs → kernel inputs (in kernel order) / init / resource.
    kins_.clear();
    double seed = 0, maxDelay = 0, z1Init = 0;
    uint32_t resource = kNone;
    for (uint32_t i = 0; i < nd.numInputs; ++i) {
      if (m.args.empty()) break;
      const uint32_t ai = std::min<uint32_t>(i, static_cast<uint32_t>(m.args.size()) - 1);
      const ArgMap& a = m.args[ai];
      switch (a.use) {
        case ArgUse::Input:
        case ArgUse::InputSeconds: kins_.push_back({i, a.kernelInput + (i - ai)}); break;
        case ArgUse::Resource: {
          double v = 0;
          if (!constInput(n, i, v)) return;
          resource = v >= 0 && v < 4294967295.0 ? static_cast<uint32_t>(v) : 0;
          break;
        }
        case ArgUse::InitSeed:
          if (!constInput(n, i, seed)) return;
          break;
        case ArgUse::InitMaxDelaySeconds:
          if (!constInput(n, i, maxDelay)) return;  // reports a non-constant `max`
          break;
        case ArgUse::InitZ1:
          if (!constInput(n, i, z1Init)) return;
          break;
      }
    }
    std::sort(kins_.begin(), kins_.end(), [](const KIn& a, const KIn& b) { return a.k < b.k; });
    // A split read takes every kernel input but x (registry input 0). Built once per node: every channel's read
    // op needs the same list (M1 review H2: compacting kins_ in place inside the channel loop gave channel 1 and
    // up the inputs (time, time)).
    kreads_.clear();
    if (isSplit)
      for (const KIn& k : kins_)
        if (k.reg != 0) kreads_.push_back(k);
    for (uint32_t ch = 0; ch < width_[n]; ++ch) {
      uint32_t stateIdx = kNone;
      if (info.stateSize > 0) {
        dsp::StateInit init;
        init.sampleRate = opt_.sampleRate;
        init.lanes = lanes;
        if (m.kernel == dsp::Atom::DelayLinear || m.kernel == dsp::Atom::DelayCubic ||
            m.kernel == dsp::Atom::DelayAllpass) {
          init.maxDelayFrames = maxDelayFramesOf(n);
          // A delay ring is at most dsp::kMaxDelayRingBytes (M1 review H3: a larger `max` overflowed the state
          // size and the kernel wrote out of bounds). Rejected here, never clamped silently.
          const uint32_t limit = dsp::maxDelayFramesLimit(lanes);
          if (init.maxDelayFrames == kNone || init.maxDelayFrames > limit) {
            char minutes[32];
            std::snprintf(minutes, sizeof minutes, "%.1f", static_cast<double>(limit) / opt_.sampleRate / 60.0);
            report(n, ErrorCode::InvalidArgument,
                   "the maximum delay of " + label(n) + " must be between 0 and " + std::to_string(limit) +
                       " samples (" + minutes + " min at " + std::to_string(static_cast<int64_t>(opt_.sampleRate)) +
                       " Hz) for a " + (lanes == 8 ? "voice" : "mono") + " delay line, whose ring is at most 1 GiB");
            return;
          }
        }
        if (m.kernel == dsp::Atom::Noise)
          init.seed = static_cast<uint32_t>(mix64(tp_->stateKey[n] ^ mix64(ch))) +
                      static_cast<uint32_t>(static_cast<int64_t>(seed));
        if (m.kernel == dsp::Atom::Z1)
          init.z1Init = tp_->outputType(n, 0).kind == Kind::Bool ? dsp::boolMask(z1Init != 0.0)
                                                                 : static_cast<float>(z1Init);
        StateTag tag;
        tag.kernel = static_cast<uint16_t>(m.kernel);
        tag.lanes = lanes;
        tag.extra = init.maxDelayFrames;
        // ≤ sizeof(DelayState) + kMaxDelayRingBytes after the check above, so it fits the uint32 entry size.
        stateIdx = stateEntry(n, static_cast<uint16_t>(ch), tag, static_cast<uint32_t>(dsp::stateBytes(m.kernel, init)), init,
                              lanes == 8 ? Poly::Voice : Poly::Global);
      }
      outs_.clear();
      for (uint32_t o = 0; o < nd.numOutputs; ++o) outs_.push_back(outOperand(n, o, ch));
      Op op;
      op.kind = OpKind::Kernel;
      op.atom = nd.atom;
      op.lanes = lanes;
      op.channel = static_cast<uint16_t>(ch);
      op.node = n;
      if (!isSplit) {
        op.kernel = m.kernel;
        op.state = stateIdx;
        op.resource = info.resource != dsp::Resource::None ? resource : kNone;
        pushKernelOp(op, kins_, outs_, control);
        continue;
      }
      // Split: read now (z1: no input; delay: its time input), write (input x) at the end of the loop body.
      op.kernel = readKernel(m.kernel);
      op.state = stateIdx;
      pushKernelOp(op, kreads_, outs_, control);
      op.kernel = writeKernel(m.kernel);
      writes_.push_back(op);
    }
  }

  // An op whose inputs are the given registry inputs of op.node (in kernel order): adapts each operand (Spl/Blk,
  // fill, broadcast), sets the Spl mask and checks that the kernel exists.
  void pushKernelOp(Op op, std::span<const KIn> which, std::span<const Operand> outs, bool control) {
    const dsp::KernelInfo& ki = dsp::kernelInfo(op.kernel);
    const size_t mark = insStack_.size();
    uint32_t mask = 0;
    for (uint32_t j = 0; j < which.size(); ++j) {
      const bool splOk = ki.variadicIn || (((ki.splSpecMask | ki.splRequired) >> j) & 1u) != 0;
      bool spl = false;
      const Operand o = adapt(inputOperand(op.node, which[j].reg, op.channel), op.lanes, control, splOk, spl, op.node);
      insStack_.push_back(o);
      if (spl) mask |= 1u << j;
    }
    op.splMask = mask;
    if (dsp::findKernel(op.kernel, op.lanes, mask) == nullptr)
      report(op.node, ErrorCode::Unsupported,
             "no " + std::string(ki.name) + " kernel for " + std::to_string(op.lanes) + " lanes with Spl mask " +
                 std::to_string(mask) + " (" + label(op.node) + ")");
    pushOp(op, std::span<const Operand>(insStack_.data() + mark, insStack_.size() - mark), outs);
    insStack_.resize(mark);
  }

  // The split writes of the loop just emitted (input: registry input 0, x).
  void emitWrites() {
    static constexpr KIn kX[] = {{0, 0}};
    for (const Op& w : writes_) pushKernelOp(w, kX, {}, false);
    writes_.clear();
  }

  uint32_t maxDelayFramesOf(NodeIndex n) const {
    double v = 0;
    Unit u = Unit::None;
    const uint32_t maxInput = 2;
    if (!constInputQuiet(n, maxInput, v, u)) return 0;
    const double frames = u == Unit::smp ? std::ceil(v) : std::ceil(v * opt_.sampleRate);
    if (!(frames >= 0) || frames > static_cast<double>(1u << 30)) return kNone;
    return static_cast<uint32_t>(frames);
  }

  bool constInputQuiet(NodeIndex n, uint32_t i, double& v, Unit& u) const {
    const WireIndex w = adj_.driver[P().flatInput(n, i)];
    u = Unit::None;
    if (w == kNone) {
      const AtomSignature& sig = atomSignature(P().node(n).atom);
      v = sig.inputs[specIndex(sig, i)].defaultValue;
      return true;
    }
    const NodeIndex d = P().wire(w).from.node;
    if (P().node(d).kind == NodeKind::Const) {
      v = P().constant(d).value();
      u = P().constant(d).unit;
      return true;
    }
    if (P().node(d).kind == NodeKind::Atom && kernelMapping(P().node(d).atom).lowering == Lowering::Constant) {
      v = opt_.sampleRate;
      return true;
    }
    return false;
  }

  void emitNode(NodeIndex n, bool control) {
    if (nk_[n] != NK::Op) return;
    const Node& nd = P().node(n);
    if (reducer_[n]) {
      for (uint32_t ch = 0; ch < width_[n]; ++ch) {
        const Operand in = inputOperand(n, 0, ch);
        const Operand out = outOperand(n, 0, ch);
        Op op;
        op.kind = OpKind::Reduce;
        op.atom = nd.atom;
        op.lanes = 8;
        op.channel = static_cast<uint16_t>(ch);
        op.splMask = isPatternLike(in) && !control ? 1u : 0u;
        op.node = n;
        op.valueKind = tp_->inputType(n, 0).kind == Kind::Bool ? Kind::Bool : Kind::Real;
        pushOp(op, {&in, 1}, {&out, 1});
        reduceVals_.push_back(out.index);
      }
      return;
    }
    if (kernelMapping(nd.atom).lowering == Lowering::Engine) {  // send
      double bus = 0;
      if (nd.numInputs > 1 && !constInput(n, 1, bus)) return;
      for (uint32_t ch = 0; ch < width_[n]; ++ch) {
        const Operand in = inputOperand(n, 0, ch);
        Op op;
        op.kind = OpKind::Send;
        op.atom = nd.atom;
        op.lanes = poly_[n] == Poly::Voice ? 8 : 1;
        op.channel = static_cast<uint16_t>(ch);
        op.splMask = isPatternLike(in) ? 1u : 0u;
        op.resource = bus >= 0 && bus < 4294967295.0 ? static_cast<uint32_t>(bus) : 0;
        op.node = n;
        pushOp(op, {&in, 1}, {});
      }
      return;
    }
    emitKernel(n, control);
  }

  void emitSolve(uint32_t s, bool control) {
    Schedule& sch = S();
    SolveInfo& si = solves_[s];
    SolveBlock sb;
    sb.solve = s;
    sb.iterations = P().solves()[s].iterations;
    sb.region = curRegion();
    sb.firstOp = static_cast<uint32_t>(sch.ops.size());
    sb.firstUnknown = static_cast<uint32_t>(sch.unknowns.size());
    sb.firstEquation = static_cast<uint32_t>(sch.equations.size());
    const auto solveIdx = static_cast<uint32_t>(sch.solves.size());
    sch.solves.push_back(sb);
    ++sch.regions.back().numSolves;
    curSolve_ = solveIdx;
    for (NodeIndex u : si.unknowns) {
      for (uint32_t ch = 0; ch < width_[u]; ++ch) {
        SolveUnknown su;
        su.node = u;
        su.channel = static_cast<uint16_t>(ch);
        su.value = outOperand(u, 0, ch);
        if (su.value.kind == Operand::Kind::Buffer) {
          vals_[su.value.index].def = sb.firstOp;
          vals_[su.value.index].defStage = sch.regions.back().stage;
        }
        if (driverNode(u, 0) != kNone) {
          bool spl = false;
          su.init = adapt(inputOperand(u, 0, ch), poly_[u] == Poly::Voice ? 8 : 1, control, true, spl, u);
          use(su.init, sb.firstOp);
        } else {
          StateTag tag;
          tag.kernel = StateTag::kSolveGuess;
          tag.lanes = poly_[u] == Poly::Voice ? 8 : 1;
          dsp::StateInit init;
          init.sampleRate = opt_.sampleRate;
          init.lanes = tag.lanes;
          su.state = stateEntry(u, static_cast<uint16_t>(ch), tag, dsp::kStateAlign, init,
                                tag.lanes == 8 ? Poly::Voice : Poly::Global);
        }
        sch.unknowns.push_back(su);
      }
    }
    // Body ops (unknowns and equations have none). Split reads in a body only read state; writes go to the loop end.
    for (NodeIndex m : si.body)
      if (nk_[m] == NK::Op) emitNode(m, control);
    SolveBlock& b = sch.solves[solveIdx];
    b.numOps = static_cast<uint32_t>(sch.ops.size()) - b.firstOp;
    const uint32_t end = b.numOps ? b.firstOp + b.numOps - 1 : b.firstOp;
    for (NodeIndex q : si.equations) {
      for (uint32_t ch = 0; ch < width_[q]; ++ch) {
        SolveEquation se;
        se.node = q;
        se.channel = static_cast<uint16_t>(ch);
        bool spl = false;
        const uint8_t lanes = poly_[q] == Poly::Voice ? 8 : 1;
        se.lhs = adapt(inputOperand(q, 0, ch), lanes, control, true, spl, q);
        se.rhs = adapt(inputOperand(q, 1, ch), lanes, control, true, spl, q);
        // The solver reads both sides after the body has run (residuals), not inside it.
        use(se.lhs, end, true);
        use(se.rhs, end, true);
        sch.equations.push_back(se);
      }
    }
    // The unknowns are written by the solver before every body evaluation and read back after it (the next
    // evaluation's guess, the perturbed Jacobian columns): no body op may overwrite one, even at its last read.
    for (uint32_t k = b.firstUnknown; k < sch.unknowns.size(); ++k) use(sch.unknowns[k].value, end, true);
    b.numUnknowns = static_cast<uint32_t>(sch.unknowns.size()) - b.firstUnknown;
    b.numEquations = static_cast<uint32_t>(sch.equations.size()) - b.firstEquation;
    curSolve_ = kNone;
  }

  void emit() {
    Schedule& sch = S();
    sch.ops.reserve(steps_.size() + steps_.size() / 4);
    sch.operands.reserve(steps_.size() * 3);
    uses_.reserve(steps_.size() * 2);
    pending_.reserve(steps_.size() / 2);
    size_t i = 0;
    while (i < steps_.size()) {
      const uint32_t c = steps_[i].scc;
      size_t j = i;
      while (j < steps_.size() && steps_[j].scc == c) ++j;
      const uint32_t cls = sccClass_[c];
      if (cls == kItemFree) {
        i = j;
        continue;
      }
      const auto stage = static_cast<uint32_t>(sccStage_[c]);
      const RegionKind kind = cls == kItemControl ? RegionKind::Control
                              : cls == kItemLoop  ? RegionKind::Loop
                                                  : RegionKind::Block;
      openRegion(stage, kind, kind == RegionKind::Loop ? sccSub_[c] : 0);
      const bool control = kind == RegionKind::Control;
      for (size_t k = i; k < j; ++k) {
        const NodeIndex n = steps_[k].node;
        if (bodyOf_[n] != kNone) {
          // The whole body is emitted at its first node; skip the rest of it.
          const uint32_t s = bodyOf_[n];
          emitSolve(s, control);
          k += solves_[s].body.size() - 1;
          continue;
        }
        emitNode(n, control);
        if (!res_.ok()) return;
      }
      emitWrites();
      if (!res_.ok()) return;
      i = j;
    }
  }

  void finishStructure() {
    Schedule& s = S();
    const auto numOps = static_cast<uint32_t>(s.ops.size());
    for (size_t r = 0; r < s.regions.size(); ++r) {
      const uint32_t end = r + 1 < s.regions.size() ? s.regions[r + 1].firstOp : numOps;
      s.regions[r].numOps = end - s.regions[r].firstOp;
    }
    for (size_t g = 0; g < s.stages.size(); ++g) {
      const uint32_t endR = g + 1 < s.stages.size() ? s.stages[g + 1].firstRegion : static_cast<uint32_t>(s.regions.size());
      s.stages[g].numRegions = endR - s.stages[g].firstRegion;
      const uint32_t endO = g + 1 < s.stages.size() ? s.stages[g + 1].firstOp : numOps;
      s.stages[g].numOps = endO - s.stages[g].firstOp;
    }
    // Reducer accumulators are initialised before their stage's group loop.
    for (uint32_t v : reduceVals_) {
      const uint32_t st = vals_[v].def;
      // Find the stage containing the reduce op.
      for (const Stage& sg : s.stages)
        if (st >= sg.firstOp && st < sg.firstOp + sg.numOps) {
          vals_[v].def = sg.firstOp;
          break;
        }
    }
  }

  void outputs() {
    Schedule& s = S();
    for (NodeIndex n = 0; n < P().nodeCount(); ++n) {
      if (nk_[n] != NK::Sink) continue;
      const Node& nd = P().node(n);
      if (nd.numInputs == 0 || adj_.driver[P().flatInput(n, 0)] == kNone) continue;
      const uint16_t w = std::max<uint16_t>(1, tp_->inputType(n, 0).width);
      for (uint32_t ch = 0; ch < w; ++ch) {
        OutputRef o;
        o.node = n;
        o.isExport = nd.kind == NodeKind::Export;
        o.channel = static_cast<uint16_t>(ch);
        o.value = inputOperand(n, 0, ch);
        o.poly = tp_->inputType(n, 0).poly;
        if (o.poly == Poly::Voice) {
          if (o.value.kind == Operand::Kind::Buffer && (vals_[o.value.index].defStage & 1)) {
            o.stage = vals_[o.value.index].defStage;
          } else {
            o.stage = lastVoiceStage();
          }
        }
        s.outputs.push_back(o);
      }
    }
  }

  uint32_t lastVoiceStage() {
    Schedule& s = S();
    for (size_t g = s.stages.size(); g-- > 0;)
      if (s.stages[g].poly == Poly::Voice) return s.stages[g].index;
    // No voice stage: add an empty one at the end (the engine reads voice externals there).
    Stage sg;
    sg.poly = Poly::Voice;
    sg.index = s.stages.empty() ? 1 : s.stages.back().index + 1;  // the last stage is global (even) here
    sg.firstRegion = static_cast<uint32_t>(s.regions.size());
    sg.firstOp = static_cast<uint32_t>(s.ops.size());
    s.stages.push_back(sg);
    return sg.index;
  }

  // ---- buffers ------------------------------------------------------------------------------------------------
  void allocateBuffers() {
    Schedule& s = S();
    const auto numOps = static_cast<uint32_t>(s.ops.size());
    // Half-step positions (Val::live): 2·p is "read by op p", 2·p + 1 "read after op p". kForever: after every op.
    const uint32_t kForever = 2 * (numOps + 1);
    for (Val& v : vals_) {
      if (v.def == kNone) v.def = 0;  // never written (cannot happen for emitted values); keep it harmless
      v.live = 2 * v.def;
    }
    auto stageOfIndex = [&](uint32_t stageIdx) -> const Stage* {
      for (const Stage& sg : s.stages)
        if (sg.index == stageIdx) return &sg;
      return nullptr;
    };
    // After the last op of [first, first + n): the value survives the whole range (a loop's next iteration, or a
    // reader at its end, sees it). An empty range is read before its first op, i.e. after op first − 1.
    auto afterRange = [](uint32_t first, uint32_t n) -> uint32_t {
      return n ? 2 * (first + n - 1) + 1 : (first ? 2 * first - 1 : 0);
    };
    for (const Use& u : uses_) {
      Val& v = vals_[u.val];
      uint32_t q = 2 * u.pos + (u.after ? 1u : 0u);
      const Region& r = s.regions[u.region];
      const Stage& sg = s.stages[regionStage_[u.region]];
      // A value defined before a loop and read inside it is read again by the loop's next iteration: it lives past
      // the loop's last op (so that op cannot take it in place either).
      if (sg.poly == Poly::Voice && v.def < sg.firstOp && sg.numOps) q = std::max(q, afterRange(sg.firstOp, sg.numOps));
      if (r.kind == RegionKind::Loop && v.def < r.firstOp && r.numOps) q = std::max(q, afterRange(r.firstOp, r.numOps));
      if (u.solve != kNone) {
        const SolveBlock& b = s.solves[u.solve];
        if (v.def < b.firstOp && b.numOps) q = std::max(q, afterRange(b.firstOp, b.numOps));
      }
      if (v.poly == Poly::Voice && r.stage != v.defStage) v.perGroup = true;
      v.live = std::max(v.live, q);
    }
    // Reducer accumulators: initialised before their stage's group loop and combined into by every group, so live
    // over the whole stage even when nothing reads the result.
    for (uint32_t vi : reduceVals_) {
      Val& v = vals_[vi];
      for (const Stage& sg : s.stages)
        if (sg.numOps && v.def >= sg.firstOp && v.def < sg.firstOp + sg.numOps)
          v.live = std::max(v.live, afterRange(sg.firstOp, sg.numOps));
    }
    // Outputs: the engine reads a voice output at the end of its stage, per group (after the stage's last op), and a
    // global one after the last stage.
    for (const OutputRef& o : s.outputs) {
      if (o.value.kind != Operand::Kind::Buffer) continue;
      Val& v = vals_[o.value.index];
      if (o.poly == Poly::Voice) {
        if (v.poly == Poly::Voice && o.stage != v.defStage) v.perGroup = true;
        const Stage* sg = stageOfIndex(o.stage);
        v.live = std::max(v.live, sg ? afterRange(sg->firstOp, sg->numOps) : kForever);
      } else {
        v.live = kForever;
      }
    }
    // Linear scan over op positions 0 … numOps (values may be defined at numOps: an empty solve body at the end).
    // A value whose live end is 2·p or 2·p + 1 dies at p: its buffer is free for values defined at p + 1 and later.
    const auto numVals = static_cast<uint32_t>(vals_.size());
    auto deathOf = [&](const Val& v) { return std::min(v.live / 2, numOps + 1); };
    std::vector<uint32_t> defBegin(numOps + 3, 0), lastBegin(numOps + 3, 0);
    for (const Val& v : vals_) {
      ++defBegin[std::min(v.def, numOps + 1) + 1];
      ++lastBegin[deathOf(v) + 1];
    }
    for (uint32_t p = 0; p + 1 < defBegin.size(); ++p) {
      defBegin[p + 1] += defBegin[p];
      lastBegin[p + 1] += lastBegin[p];
    }
    std::vector<uint32_t> byDef(numVals), byLast(numVals);
    {
      std::vector<uint32_t> fd(defBegin.begin(), defBegin.end() - 1), fl(lastBegin.begin(), lastBegin.end() - 1);
      for (uint32_t v = 0; v < numVals; ++v) {
        byDef[fd[std::min(vals_[v].def, numOps + 1)]++] = v;
        byLast[fl[deathOf(vals_[v])]++] = v;
      }
    }
    std::vector<uint32_t> freeList[3];
    std::vector<uint8_t> handed(numVals, 0);
    auto cls = [](const Val& v) { return v.pattern ? 0 : v.lanes == 8 ? 2 : 1; };
    auto fresh = [&](const Val& v) {
      Buffer b;
      b.cls = v.pattern ? Buffer::Class::Pattern : Buffer::Class::Block;
      b.lanes = v.lanes;
      b.perGroup = v.perGroup;
      s.buffers.push_back(b);
      return static_cast<uint32_t>(s.buffers.size() - 1);
    };
    for (uint32_t p = 0; p <= numOps + 1; ++p) {
      // In place: output 0 of an element-wise kernel takes the buffer of an input that dies here.
      if (p < numOps) {
        const Op& op = s.ops[p];
        if (op.kind == OpKind::Kernel && op.numOut > 0 && dsp::kernelInfo(op.kernel).inPlaceOk) {
          const Operand out0 = s.operands[op.firstOperand + op.numIn];
          if (opt_.shareBuffers && out0.kind == Operand::Kind::Buffer && !vals_[out0.index].perGroup &&
              vals_[out0.index].buffer == kNone) {
            Val& o = vals_[out0.index];
            for (uint32_t j = 0; j < op.numIn; ++j) {
              const Operand in = s.operands[op.firstOperand + j];
              if (in.kind != Operand::Kind::Buffer || in.index == out0.index) continue;
              Val& iv = vals_[in.index];
              // Only an input whose last read is BY this op (live = 2·p). One read after it (live = 2·p + 1: a
              // loop's next iteration, the solver after a body, the engine after a voice stage) keeps its buffer
              // (T1.7: a broadcast at a voice stage's end; M1 review C1/C2: solve operands and voice outputs).
              if (iv.live != 2 * p || iv.perGroup || iv.buffer == kNone || handed[in.index] || cls(iv) != cls(o))
                continue;
              o.buffer = iv.buffer;
              handed[in.index] = 1;
              break;
            }
          }
        }
      }
      for (uint32_t k = defBegin[p]; k < defBegin[p + 1]; ++k) {
        Val& v = vals_[byDef[k]];
        if (v.buffer != kNone) continue;
        if (v.perGroup) {
          v.buffer = fresh(v);
          continue;
        }
        auto& fl = freeList[cls(v)];
        if (opt_.shareBuffers && !fl.empty()) {
          v.buffer = fl.back();
          fl.pop_back();
        } else {
          v.buffer = fresh(v);
        }
      }
      for (uint32_t k = lastBegin[p]; k < lastBegin[p + 1]; ++k) {
        const uint32_t vi = byLast[k];
        Val& v = vals_[vi];
        if (v.perGroup || handed[vi] || v.buffer == kNone) continue;
        freeList[cls(v)].push_back(v.buffer);
      }
    }
    auto fix = [&](Operand& o) {
      if (o.kind == Operand::Kind::Buffer) o.index = vals_[o.index].buffer;
    };
    for (Operand& o : s.operands) fix(o);
    for (SolveUnknown& u : s.unknowns) {
      fix(u.value);
      fix(u.init);
    }
    for (SolveEquation& e : s.equations) {
      fix(e.lhs);
      fix(e.rhs);
    }
    for (OutputRef& o : s.outputs) fix(o.value);
  }

  // ---- state layout -------------------------------------------------------------------------------------------
  static uint64_t slotId(uint64_t key, uint16_t ch, Poly arena) noexcept {
    return mix64(key ^ mix64((static_cast<uint64_t>(ch) << 1) | (arena == Poly::Voice ? 1u : 0u)));
  }

  void layoutState() {
    StateLayout& L = S().state;
    L.entries.clear();
    L.entries.reserve(pending_.size());
    for (const PendingState& ps : pending_) {
      StateEntry e;
      e.key = ps.key;
      e.channel = ps.channel;
      e.arena = ps.arena;
      e.tag = ps.tag;
      e.size = (ps.size + dsp::kStateAlign - 1) / dsp::kStateAlign * dsp::kStateAlign;
      e.init = ps.init;
      e.node = ps.node;
      L.entries.push_back(e);  // in pending order: Op::state indexes them 1:1
    }
    // Each arena's total must fit its uint32 offsets with room for the stable layout's holes: reject (naming the
    // node whose state crosses the cap) rather than let an offset wrap and two states overlap.
    uint64_t total[2] = {0, 0};
    for (const StateEntry& e : L.entries) {
      uint64_t& t = total[e.arena == Poly::Voice];
      t += e.size;
      if (t > kMaxArenaBytes) {
        report(e.node, ErrorCode::Overflow,
               "the patch's " + std::string(e.arena == Poly::Voice ? "per-voice-group" : "global") +
                   " state exceeds 2 GiB at " + label(e.node) + " (delay-line `max` values too large)");
        return;
      }
    }
    // Unique (key, channel, arena): a repeated key (a hash collision, or a hand-built patch) is re-keyed
    // deterministically (the later entry in op order) so lookups and transfers stay unambiguous.
    std::vector<std::pair<uint64_t, uint32_t>> ids(L.entries.size());
    for (uint32_t round = 1; round < 64; ++round) {
      for (uint32_t i = 0; i < L.entries.size(); ++i) {
        const StateEntry& e = L.entries[i];
        ids[i] = {slotId(e.key, e.channel, e.arena), i};
      }
      std::sort(ids.begin(), ids.end());
      bool dup = false;
      for (size_t i = 1; i < ids.size(); ++i)
        if (ids[i].first == ids[i - 1].first) {
          StateEntry& e = L.entries[std::max(ids[i].second, ids[i - 1].second)];
          e.key = mix64(e.key + round);
          dup = true;
        }
      if (!dup) break;
    }
    bool laid = false;
    if (opt_.previous && !opt_.previous->entries.empty()) laid = layoutStable(L, *opt_.previous);
    if (!laid) layoutFresh(L);
  }

  // Per-arena state cap (2 GiB: one maximal delay ring is 1 GiB). The fresh layout's offsets then fit uint32; the
  // stable layout computes in uint64 and falls back to a fresh layout if its end would pass the cap.
  static constexpr uint64_t kMaxArenaBytes = uint64_t{1} << 31;

  static uint32_t round64(uint32_t x) noexcept { return (x + 63u) & ~63u; }

  void layoutFresh(StateLayout& L) {
    uint32_t off[2] = {0, 0};
    for (StateEntry& e : L.entries) {
      uint32_t& o = off[e.arena == Poly::Voice];
      e.offset = o;
      o += e.size;
    }
    L.globalBytes = round64(off[0]);
    L.voiceBytes = round64(off[1]);
  }

  // Keep the offsets of unchanged entries; place the rest first-fit into the holes between them, then at the end.
  bool layoutStable(StateLayout& L, const StateLayout& prev) {
    std::vector<std::pair<uint64_t, uint32_t>> byKey;  // sorted (slot id, previous entry)
    byKey.reserve(prev.entries.size());
    for (uint32_t i = 0; i < prev.entries.size(); ++i) {
      const StateEntry& e = prev.entries[i];
      byKey.emplace_back(slotId(e.key, e.channel, e.arena), i);
    }
    std::sort(byKey.begin(), byKey.end());
    struct Range {
      uint64_t begin, end;
    };
    std::vector<Range> used[2];
    std::vector<uint8_t> kept(L.entries.size(), 0);
    uint64_t live[2] = {0, 0};
    for (size_t i = 0; i < L.entries.size(); ++i) {
      StateEntry& e = L.entries[i];
      const int a = e.arena == Poly::Voice;
      live[a] += e.size;
      const uint64_t id = slotId(e.key, e.channel, e.arena);
      const auto it = std::lower_bound(byKey.begin(), byKey.end(), std::pair<uint64_t, uint32_t>{id, 0});
      if (it == byKey.end() || it->first != id) continue;
      const StateEntry& pe = prev.entries[it->second];
      if (!(pe.tag == e.tag) || pe.size != e.size || pe.arena != e.arena) continue;
      e.offset = pe.offset;
      kept[i] = 1;
      used[a].push_back({e.offset, uint64_t{e.offset} + e.size});
    }
    uint64_t end[2] = {0, 0};
    std::vector<Range> holes[2];
    for (int a = 0; a < 2; ++a) {
      auto& u = used[a];
      std::sort(u.begin(), u.end(), [](const Range& x, const Range& y) { return x.begin < y.begin; });
      uint64_t at = 0;
      for (const Range& r : u) {
        if (r.begin > at) holes[a].push_back({at, r.begin});
        at = std::max(at, r.end);
      }
      end[a] = at;
    }
    for (size_t i = 0; i < L.entries.size(); ++i) {
      if (kept[i]) continue;
      StateEntry& e = L.entries[i];
      const int a = e.arena == Poly::Voice;
      bool placed = false;
      for (Range& h : holes[a]) {
        if (h.end - h.begin < e.size) continue;
        e.offset = static_cast<uint32_t>(h.begin);  // a hole lies below a kept entry: < 2^32
        h.begin += e.size;
        placed = true;
        break;
      }
      if (!placed) {
        if (end[a] + e.size > kMaxArenaBytes) {  // would pass the cap: lay out afresh (≤ live ≤ the cap)
          L.compacted = true;
          return false;
        }
        e.offset = static_cast<uint32_t>(end[a]);
        end[a] += e.size;
      }
    }
    // Compact when holes dominate.
    for (int a = 0; a < 2; ++a)
      if (end[a] > 2 * live[a] + 4096) {
        L.compacted = true;
        return false;
      }
    L.globalBytes = round64(static_cast<uint32_t>(end[0]));
    L.voiceBytes = round64(static_cast<uint32_t>(end[1]));
    return true;
  }

  // ---- state ----------------------------------------------------------------------------------------------
  struct Step {
    NodeIndex node;
    uint32_t scc;
  };
  struct DfsFrame {
    NodeIndex n;
    uint32_t e;
  };

  const ScheduleOptions& opt_;
  const TypedPatch* tp_;
  std::shared_ptr<TypedPatch> work_;
  ScheduleResult res_;

  Adjacency adj_;
  std::vector<NK> nk_;
  std::vector<Poly> poly_;
  std::vector<Rate> rate_;
  std::vector<uint16_t> width_;
  std::vector<uint8_t> reducer_, stateful_, split_, color_, ctl_;
  std::vector<uint32_t> succBegin_;
  std::vector<Edge> succ_;
  std::vector<uint32_t> sccOf_, sccBegin_, sccNodes_;
  std::vector<uint8_t> cyclic_;
  std::vector<SolveInfo> solves_;
  std::vector<uint32_t> bodyOf_;
  std::vector<int32_t> sccStage_, outStage_;
  std::vector<uint32_t> sccClass_, sccSub_;
  std::vector<uint32_t> sccOrder_;
  std::vector<Step> steps_;
  std::vector<uint8_t> itemMark_;
  std::vector<uint32_t> itemDeg_;
  std::vector<uint32_t> paramSlot_, inputOrdinal_;
  std::unordered_map<uint64_t, uint32_t> constIndex_;
  std::vector<uint32_t> outBase_;
  std::vector<Operand> outOps_;
  std::vector<Val> vals_;
  std::unordered_map<uint32_t, std::vector<Operand>> needs_;
  std::unordered_map<uint32_t, uint8_t> broadcastDone_;
  std::unordered_map<uint64_t, Operand> broadcastOf_;
  std::vector<uint32_t> regionStage_;
  std::vector<Use> uses_;
  uint32_t curSolve_ = kNone;
  std::vector<Op> writes_;
  std::vector<DfsFrame> dfs_;
  std::vector<uint32_t> items_, heap_, seq_;
  std::vector<KIn> kins_, kreads_;
  std::vector<Operand> outs_, insStack_;
  std::vector<uint32_t> reduceVals_;
  std::vector<PendingState> pending_;
};

}  // namespace

const StateEntry* StateLayout::find(uint64_t key, uint16_t channel, Poly arena) const noexcept {
  for (const StateEntry& e : entries)
    if (e.key == key && e.channel == channel && e.arena == arena) return &e;
  return nullptr;
}

uint32_t Schedule::blockBuffers() const noexcept {
  uint32_t n = 0;
  for (const Buffer& b : buffers) n += b.cls == Buffer::Class::Block;
  return n;
}

uint32_t Schedule::patternBuffers() const noexcept {
  uint32_t n = 0;
  for (const Buffer& b : buffers) n += b.cls == Buffer::Class::Pattern;
  return n;
}

ScheduleResult scheduleDetailed(const TypedPatch& patch, const ScheduleOptions& options) {
  return Scheduler(patch, options).run();
}

Result<Schedule> schedule(const TypedPatch& patch, const ScheduleOptions& options) {
  ScheduleResult r = scheduleDetailed(patch, options);
  if (r.ok()) return std::move(r.schedule);
  Error e;
  e.code = r.diagnostics.front().code;
  e.location = r.diagnostics.front().location;
  for (size_t i = 0; i < r.diagnostics.size(); ++i) {
    if (i) e.message += '\n';
    e.message += options.source.text.empty() ? r.diagnostics[i].message
                                             : formatDiagnostic(options.source.text, r.diagnostics[i]);
  }
  return e;
}

}  // namespace pq::graph
