#pragma once

// Tests only (T1.7): a slow reference evaluator for TypedPatches — a sample-by-sample walk of the GRAPH (not the
// Schedule) in double precision, written from the documented semantics (pq/dsp/atoms.h, pq/graph/atoms.h,
// schedule.h for solve blocks and reducers), to check the interpreter.
//
// What it models:
//   - every atom used by tests/graph/cases/ and tests/engine/patches/, multichannel signals (element-wise atoms
//     broadcast a mono input; channel/pack), voices (V voices, every voice node evaluated per voice; reducers
//     combine voices 0..V−1), solve blocks (Newton, same iteration count, exact Jacobian by tiny central differences
//     in double), sends (bus sums), transport, engine inputs;
//   - signals are float32 (SPEC §7.1): each atom is evaluated in double from its inputs and its result is stored
//     rounded to float32, like a block buffer. So exact operations (+ − × ÷ √, comparisons, floor …) agree bit for bit
//     with the kernels and a discontinuous atom (floor, comparisons, counters) sees the same input; transcendental
//     atoms differ by a few float ulp. Inside a solve block's Newton iteration values stay double (the root is then
//     rounded once), so the reference solves the equations, not the interpreter's float arithmetic.
//   - fast_ atoms are evaluated EXACTLY (std:: in double). Their documented bounds (pq/core/fast_math.h, ≤ 1e-6
//     relative/absolute in the ranges used here) are far inside the comparison tolerance, so no special case is
//     needed; a test that needs the approximation itself would compare against those bounds.
// Things the reference takes from outside rather than re-deriving: the noise seed of each node (the compiler mixes
// the state key into it; the test reads it from the Schedule's StateLayout) and the resource tables (the same table
// data the program is given; wavetables are read with the documented mip-level choice and linear interpolation, in
// float like the signal: an FFT-built table holds −6e−20 where an exact sine has 0, and `sign`/`floor` of it differ).
// Not modelled: clipPlayer (silence, as in the engine until the warp engine exists), event-rate ports.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pq/dsp/kernel.h"
#include "pq/dsp/tables.h"
#include "pq/engine/program_image.h"
#include "pq/graph/infer.h"
#include "pq/graph/kernel_map.h"

namespace refeval {

using namespace pq::graph;

inline constexpr double kPi = 3.14159265358979323846;

struct ShaperSpec {
  std::vector<float> points;  // ≥ 2
  float lo = -1.0f, hi = 1.0f;
};

struct Stimulus {
  double sampleRate = 48000.0;
  uint32_t voices = 0;  // V (lanes 0..V−1 of groups 0..)
  // Values the engine provides (bools as 0/1). Param-rate signals must be constant over time.
  std::function<double(uint32_t ordinal, uint32_t ch, uint32_t voice, uint64_t t)> root;
  std::function<double(uint32_t signal, uint32_t voice, uint64_t t)> voiceSignal;  // pq::engine::VoiceSignal
  std::function<double(uint32_t ch, uint64_t t)> trackIn, sidechainIn;
  std::function<double(NodeIndex param)> param;  // raw value before clamping (NaN = default)
  pq::engine::TransportSnapshot transport;      // at t = 0; beat advances by beatsPerFrame per sample
  uint32_t lastVoice = pq::engine::kNoVoice;
  std::vector<const pq::dsp::WavetableView*> wavetables;  // by table index (required for every wavetable atom)
  std::vector<ShaperSpec> shapers;                // by table index (missing: identity on [−1, 1])
  std::vector<std::vector<float>> samples;        // by sample index (missing: one silent frame)
  std::function<uint32_t(NodeIndex, uint16_t ch)> noiseSeed;
};

// Atom names compared by a compile-time hash (the evaluator compares a name against ~90 literals per atom per
// sample; string compares made the dev-build tests slow).
constexpr uint32_t fnv1a(const char* s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
  return h;
}
struct Lit {
  uint32_t h, n;
  template <size_t N>
  consteval Lit(const char (&s)[N]) : h(fnv1a(s, N - 1)), n(static_cast<uint32_t>(N - 1)) {}
};
struct AName {
  std::string_view s;
  uint32_t h = 0;
  AName() = default;
  explicit AName(std::string_view v) : s(v), h(fnv1a(v.data(), v.size())) {}
  bool operator==(Lit l) const { return h == l.h && s.size() == l.n; }
  bool starts_with(std::string_view p) const { return s.starts_with(p); }
};

inline uint32_t mixSeed(uint32_t seed, uint32_t lane) {  // as pq/dsp atoms.cpp (the lane seeds of noise)
  uint32_t x = seed ^ (0x9E3779B9u * (lane + 1u));
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
inline uint32_t lowbias32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

class Evaluator {
public:
  Evaluator(const TypedPatch& tp, Stimulus st) : tp_(tp), p_(tp.patch), st_(std::move(st)) {
    sr_ = st_.sampleRate;
    V_ = st_.voices;
    adj_ = p_.adjacency();
    const uint32_t N = p_.nodeCount();
    name_.resize(N);
    nodeVoices_.assign(N, 1);
    width_.assign(N, 1);
    for (NodeIndex n = 0; n < N; ++n) {
      const Node& nd = p_.node(n);
      if (nd.kind == NodeKind::Atom) name_[n] = AName(atomSignature(nd.atom).name);
      Poly py = Poly::Global;
      uint16_t w = 1;
      for (uint32_t o = 0; o < nd.numOutputs; ++o) {
        py = std::max(py, tp.outputType(n, o).poly);
        w = std::max(w, tp.outputType(n, o).width);
      }
      if (nd.numOutputs == 0 || nd.kind == NodeKind::Equation)
        for (uint32_t i = 0; i < nd.numInputs; ++i) {
          py = std::max(py, tp.inputType(n, i).poly);
          w = std::max(w, tp.inputType(n, i).width);
        }
      if (nd.kind == NodeKind::Atom && name_[n].starts_with("voice_")) py = Poly::Global;  // reducers: global out
      nodeVoices_[n] = py == Poly::Voice ? std::max<uint32_t>(V_, 1) : 1;
      if (py == Poly::Voice && V_ == 0) nodeVoices_[n] = 0;
      width_[n] = std::max<uint16_t>(w, 1);
      if (nd.kind == NodeKind::Input) inputOrdinal_[n] = static_cast<uint32_t>(inputOrdinal_.size());
    }
    // Value storage per output port: width × voices.
    portOff_.assign(p_.portCount(), 0);
    size_t off = 0;
    for (NodeIndex n = 0; n < N; ++n)
      for (uint32_t o = 0; o < p_.node(n).numOutputs; ++o) {
        const uint32_t fo = p_.flatOutput(n, o);
        portOff_[fo] = off;
        off += static_cast<size_t>(std::max<uint16_t>(tp.types[fo].width, 1)) * std::max<uint32_t>(portVoices(fo), 1);
      }
    vals_.assign(off + 1, 0.0);
    state_.resize(N);
    for (NodeIndex n = 0; n < N; ++n) state_[n].assign(static_cast<size_t>(width_[n]) * std::max<uint32_t>(nodeVoices_[n], 1), St{});
    initStates();
    order();
  }

  bool ok() const { return ok_; }
  const std::string& why() const { return why_; }

  // One sample.
  void step(uint64_t t) {
    t_ = t;
    sends_.clear();
    for (const Item& it : items_) {
      if (it.solve == kNone) evalNode(it.node);
      else solveItem(it.solve);
    }
    for (NodeIndex n = 0; n < p_.nodeCount(); ++n) update(n);
  }

  // Value of a sink (`out`/`export`) node's input channel `ch` for voice v (0 for global sinks), as stored (float).
  double sink(NodeIndex n, uint32_t ch, uint32_t v) const { return in(n, 0, ch, v); }
  bool sinkIsBool(NodeIndex n) const { return tp_.inputType(n, 0).kind == Kind::Bool; }
  // Send bus contents at the current sample: (bus, channel) → sum.
  const std::map<std::pair<uint32_t, uint32_t>, double>& sends() const { return sends_; }

private:
  struct St {
    double a = 0, b = 0, c = 0;
    int64_t i = 0, j = 0;
    uint32_t u = 0, frac = 0;
    bool active = true;
    std::vector<float> hist;
  };
  struct Item {
    NodeIndex node = kNone;
    uint32_t solve = kNone;
  };
  struct SolveInfo {
    std::vector<NodeIndex> unknowns, equations, body;
    uint32_t iterations = 1;
  };

  uint32_t portVoices(uint32_t flatOut) const {
    return tp_.types[flatOut].poly == Poly::Voice ? V_ : 1u;
  }

  // ---- values ----
  double getVal(NodeIndex n, uint32_t o, uint32_t ch, uint32_t v) const {
    const uint32_t fo = p_.flatOutput(n, o);
    const uint32_t w = std::max<uint16_t>(tp_.types[fo].width, 1);
    const uint32_t vv = std::max<uint32_t>(portVoices(fo), 1);
    const uint32_t c = std::min(ch, w - 1);
    const uint32_t vi = tp_.types[fo].poly == Poly::Voice ? std::min(v, vv - 1) : 0;
    return vals_[portOff_[fo] + static_cast<size_t>(c) * vv + vi];
  }
  void set(NodeIndex n, uint32_t o, uint32_t ch, uint32_t v, double x) {
    const uint32_t fo = p_.flatOutput(n, o);
    if (ch >= std::max<uint16_t>(tp_.types[fo].width, 1)) return;
    if (tp_.types[fo].kind == Kind::Bool) x = x != 0.0 ? 1.0 : 0.0;
    else if (round_) x = static_cast<double>(static_cast<float>(x));
    const uint32_t vv = std::max<uint32_t>(portVoices(fo), 1);
    const uint32_t vi = tp_.types[fo].poly == Poly::Voice ? std::min(v, vv - 1) : 0;
    vals_[portOff_[fo] + static_cast<size_t>(ch) * vv + vi] = x;
  }
  NodeIndex driverNode(NodeIndex n, uint32_t i) const {
    const WireIndex w = adj_.driver[p_.flatInput(n, i)];
    return w == kNone ? kNone : p_.wire(w).from.node;
  }
  // Input i of node n, channel ch, voice v.
  double in(NodeIndex n, uint32_t i, uint32_t ch, uint32_t v) const {
    if (i >= p_.node(n).numInputs) return 0.0;
    const WireIndex w = adj_.driver[p_.flatInput(n, i)];
    if (w != kNone) {
      const PortRef f = p_.wire(w).from;
      return getVal(f.node, f.port, ch, v);
    }
    if (p_.node(n).kind == NodeKind::Atom) {
      const AtomSignature& sig = atomSignature(p_.node(n).atom);
      const PortSpec& ps = sig.inputs[std::min<size_t>(i, sig.inputs.size() - 1)];
      if (tp_.inputType(n, i).kind == Kind::Bool) return ps.defaultValue != 0.0f ? 1.0 : 0.0;
      return static_cast<double>(ps.defaultValue);
    }
    return 0.0;
  }
  double constIn(NodeIndex n, uint32_t i) const {  // a const-rate input (state init, resource index)
    const NodeIndex d = driverNode(n, i);
    if (d == kNone) return in(n, i, 0, 0);
    if (p_.node(d).kind == NodeKind::Const) {
      const Constant& c = p_.constant(d);
      if (c.unit == Unit::smp && kernelMapping(p_.node(n).atom).args.size() > i &&
          kernelMapping(p_.node(n).atom).args[i].use == ArgUse::InitMaxDelaySeconds)
        return c.value() / sr_;
      return c.value();
    }
    if (p_.node(d).kind == NodeKind::Atom && atomSignature(p_.node(d).atom).name == "sr") return sr_;
    return getVal(d, 0, 0, 0);
  }
  St& stateOf(NodeIndex n, uint32_t ch, uint32_t v) {
    const uint32_t vv = std::max<uint32_t>(nodeVoices_[n], 1);
    return state_[n][static_cast<size_t>(ch) * vv + std::min(v, vv - 1)];
  }
  bool isDelayedInput(NodeIndex n, uint32_t i) const {
    if (p_.node(n).kind != NodeKind::Atom) return false;
    const AtomSignature& sig = atomSignature(p_.node(n).atom);
    return !sig.inputs.empty() && sig.inputs[std::min<size_t>(i, sig.inputs.size() - 1)].delayed;
  }
  // Duration inputs in seconds (before optimise()) are converted here.
  double samplesOf(NodeIndex n, uint32_t i, uint32_t ch, uint32_t v) const {
    const double x = in(n, i, ch, v);
    if (tp_.durationsInSamples) return x;
    const KernelMapping& m = kernelMapping(p_.node(n).atom);
    if (i < m.args.size() && m.args[i].use == ArgUse::InputSeconds) {
      const NodeIndex d = driverNode(n, i);
      if (d != kNone && p_.node(d).kind == NodeKind::Const && p_.constant(d).unit == Unit::smp) return x;
      return static_cast<double>(static_cast<float>(x * sr_));
    }
    return x;
  }

  // ---- setup ----
  void initStates() {
    for (NodeIndex n = 0; n < p_.nodeCount(); ++n) {
      const AName& a = name_[n];
      for (St& s : state_[n]) {
        if (a == "z1") {
          const double init = constIn(n, 1);
          s.a = tp_.outputType(n, 0).kind == Kind::Bool ? (init != 0.0 ? 1.0 : 0.0) : static_cast<float>(init);
        }
        if (a == "clockdiv") s.i = -1;
        if (a.starts_with("sample_read")) s.active = true;
      }
      if (a == "delay" || a == "delay_cubic" || a == "delay_allpass") {
        const double maxSec = constIn(n, 2);
        double frames = std::ceil(maxSec * sr_);
        frames = frames < 2 ? 2 : frames > (1 << 30) ? (1 << 30) : frames;
        maxDelay_[n] = static_cast<float>(frames);
      }
    }
  }

  void order() {
    const uint32_t N = p_.nodeCount();
    // Solve blocks: unknowns, equations, body = reach(unknowns) ∩ coreach(equations) over same-sample edges.
    solves_.assign(p_.solveCount(), {});
    for (uint32_t s = 0; s < p_.solveCount(); ++s) solves_[s].iterations = p_.solves()[s].iterations;
    for (NodeIndex n = 0; n < N; ++n) {
      const Node& nd = p_.node(n);
      if (nd.kind == NodeKind::Unknown) solves_[nd.payload].unknowns.push_back(n);
      if (nd.kind == NodeKind::Equation) solves_[nd.payload].equations.push_back(n);
    }
    itemOf_.assign(N, kNone);
    for (uint32_t s = 0; s < solves_.size(); ++s) {
      SolveInfo& si = solves_[s];
      std::vector<uint8_t> fwd(N, 0), bwd(N, 0);
      std::vector<NodeIndex> q(si.unknowns.begin(), si.unknowns.end());
      for (NodeIndex u : q) fwd[u] = 1;
      while (!q.empty()) {
        const NodeIndex n = q.back();
        q.pop_back();
        for (WireIndex w : adj_.outgoing(n)) {
          const Wire& wr = p_.wire(w);
          if (isDelayedInput(wr.to.node, wr.to.port) || fwd[wr.to.node]) continue;
          fwd[wr.to.node] = 1;
          q.push_back(wr.to.node);
        }
      }
      q.assign(si.equations.begin(), si.equations.end());
      for (NodeIndex e : q) bwd[e] = 1;
      while (!q.empty()) {
        const NodeIndex n = q.back();
        q.pop_back();
        for (uint32_t i = 0; i < p_.node(n).numInputs; ++i) {
          const NodeIndex d = driverNode(n, i);
          if (d == kNone || bwd[d] || isDelayedInput(n, i)) continue;
          bwd[d] = 1;
          q.push_back(d);
        }
      }
      for (NodeIndex n = 0; n < N; ++n) {
        if (!(fwd[n] && bwd[n])) continue;
        itemOf_[n] = s;
        const NodeKind k = p_.node(n).kind;
        if (k != NodeKind::Unknown && k != NodeKind::Equation) si.body.push_back(n);
      }
      for (NodeIndex n : si.unknowns) itemOf_[n] = s;
      for (NodeIndex n : si.equations) itemOf_[n] = s;
    }
    // Kahn over items (nodes; solve blocks contracted), same-sample edges only; lowest index first.
    auto itemId = [&](NodeIndex n) { return itemOf_[n] == kNone ? n : N + itemOf_[n]; };
    const uint32_t numItems = N + static_cast<uint32_t>(solves_.size());
    std::vector<uint32_t> indeg(numItems, 0);
    std::vector<std::vector<uint32_t>> succ(numItems);
    for (NodeIndex n = 0; n < N; ++n)
      for (uint32_t i = 0; i < p_.node(n).numInputs; ++i) {
        const NodeIndex d = driverNode(n, i);
        if (d == kNone || isDelayedInput(n, i)) continue;
        const uint32_t a = itemId(d), b = itemId(n);
        if (a == b) continue;
        succ[a].push_back(b);
        ++indeg[b];
      }
    std::vector<uint8_t> used(numItems, 0);
    for (NodeIndex n = 0; n < N; ++n) used[itemId(n)] = 1;
    std::vector<uint32_t> ready;
    for (uint32_t it = 0; it < numItems; ++it)
      if (used[it] && indeg[it] == 0) ready.push_back(it);
    std::vector<uint32_t> seq;
    while (!ready.empty()) {
      const auto mn = std::min_element(ready.begin(), ready.end());
      const uint32_t it = *mn;
      ready.erase(mn);
      seq.push_back(it);
      for (uint32_t b : succ[it])
        if (--indeg[b] == 0) ready.push_back(b);
    }
    uint32_t usedCount = 0;
    for (uint8_t u : used) usedCount += u;
    if (seq.size() != usedCount) {
      ok_ = false;
      why_ = "delay-free cycle";
      return;
    }
    for (uint32_t it : seq) {
      Item x;
      if (it < N) x.node = it;
      else x.solve = it - N;
      items_.push_back(x);
    }
    // Body order: topological among the body (unknowns are sources).
    for (SolveInfo& si : solves_) {
      std::vector<NodeIndex> out;
      std::vector<uint8_t> inBody(N, 0), done(N, 0);
      for (NodeIndex b : si.body) inBody[b] = 1;
      bool progress = true;
      while (out.size() < si.body.size() && progress) {
        progress = false;
        for (NodeIndex b : si.body) {
          if (done[b]) continue;
          bool readyNode = true;
          for (uint32_t i = 0; i < p_.node(b).numInputs; ++i) {
            const NodeIndex d = driverNode(b, i);
            if (d != kNone && inBody[d] && !done[d] && !isDelayedInput(b, i)) readyNode = false;
          }
          if (!readyNode) continue;
          done[b] = 1;
          out.push_back(b);
          progress = true;
        }
      }
      si.body = out;
    }
  }

  // ---- evaluation ----
  void evalNode(NodeIndex n) {
    const uint32_t vn = nodeVoices_[n];
    for (uint32_t v = 0; v < vn; ++v) evalVoice(n, v);
  }

  void evalVoice(NodeIndex n, uint32_t v) {
    const Node& nd = p_.node(n);
    const uint32_t W = width_[n];
    switch (nd.kind) {
      case NodeKind::Const: {
        const Constant& c = p_.constant(n);
        for (uint32_t ch = 0; ch < W; ++ch) set(n, 0, ch, v, c.kind == Kind::Bool ? (c.value() != 0 ? 1 : 0) : static_cast<float>(c.value()));
        return;
      }
      case NodeKind::Param: {
        const ParamDecl& d = p_.paramDecl(n);
        double x = st_.param ? st_.param(n) : NAN;
        const double def = static_cast<float>(d.defaultValue.value());
        if (x != x) x = def;
        if (d.kind == Kind::Bool) x = x != 0 ? 1 : 0;
        else {
          x = static_cast<float>(x);
          if (d.hasRange) {
            const double lo = static_cast<float>(d.rangeMin.value()), hi = static_cast<float>(d.rangeMax.value());
            if (lo <= hi) x = x < lo ? lo : x > hi ? hi : x;
          }
        }
        for (uint32_t ch = 0; ch < W; ++ch) set(n, 0, ch, v, x);
        return;
      }
      case NodeKind::Input: {
        if (driverNode(n, 0) != kNone) {
          for (uint32_t ch = 0; ch < W; ++ch) set(n, 0, ch, v, in(n, 0, ch, v));
          return;
        }
        const bool b = tp_.outputType(n, 0).kind == Kind::Bool;
        for (uint32_t ch = 0; ch < W; ++ch) {
          const double x = st_.root ? st_.root(inputOrdinal_[n], ch, v, t_) : 0.0;
          set(n, 0, ch, v, b ? (x != 0 ? 1 : 0) : x);
        }
        return;
      }
      case NodeKind::Atom: break;
      default: return;  // out/export (read through sink()), unknown/equation (solveItem)
    }
    const AName& a = name_[n];
    // Structural / engine atoms.
    if (a == "channel") {
      const uint32_t xw = std::max<uint16_t>(tp_.inputType(n, 0).width, 1);
      const double idx = constIn(n, 1);
      const uint32_t c = xw == 1 ? 0 : static_cast<uint32_t>(std::clamp(idx, 0.0, xw - 1.0));
      set(n, 0, 0, v, in(n, 0, c, v));
      return;
    }
    if (a == "pack") {
      uint32_t ch = 0;
      for (uint32_t i = 0; i < nd.numInputs; ++i) {
        const uint32_t iw = std::max<uint16_t>(tp_.inputType(n, i).width, 1);
        for (uint32_t c = 0; c < iw; ++c) set(n, 0, ch++, v, in(n, i, c, v));
      }
      return;
    }
    if (a.starts_with("voice_")) {
      for (uint32_t ch = 0; ch < W; ++ch) set(n, 0, ch, 0, reduce(n, ch));
      return;
    }
    if (a.starts_with("voice.")) {
      static constexpr std::string_view kSig[pq::engine::kVoiceSignalCount] = {
          "voice.hz", "voice.cents", "voice.gate", "voice.trigger", "voice.velocity", "voice.release",
          "voice.ratio", "voice.harmonic", "voice.unisonCount", "voice.index", "voice.degree", "voice.equave"};
      for (uint32_t k = 0; k < pq::engine::kVoiceSignalCount; ++k)
        if (a.s == kSig[k]) set(n, 0, 0, v, st_.voiceSignal ? st_.voiceSignal(k, v, t_) : 0.0);
      return;
    }
    if (a.starts_with("transport.")) {
      const pq::engine::TransportSnapshot& tr = st_.transport;
      const double beat = tr.beat + static_cast<double>(t_) * tr.beatsPerFrame;
      double x = 0;
      if (a == "transport.bpm") x = tr.bpm;
      else if (a == "transport.beat") x = beat;
      else if (a == "transport.barPhase") {
        const double bars = (beat - tr.barStartBeat) / tr.beatsPerBar;
        float ph = static_cast<float>(bars - std::floor(bars));
        x = ph < 1.0f ? ph : 0.99999994f;
      } else if (a == "transport.playing") x = tr.playing ? 1 : 0;
      set(n, 0, 0, v, x);
      return;
    }
    if (a == "track.in" || a == "sidechain.in") {
      for (uint32_t ch = 0; ch < W; ++ch) {
        const auto& f = a == "track.in" ? st_.trackIn : st_.sidechainIn;
        set(n, 0, ch, v, f ? f(ch, t_) : 0.0);
      }
      return;
    }
    if (a == "clipPlayer") {
      for (uint32_t ch = 0; ch < W; ++ch) set(n, 0, ch, v, 0.0);
      return;
    }
    if (a == "sr") {
      set(n, 0, 0, v, sr_);
      return;
    }
    if (a == "send") {
      const uint32_t bus = static_cast<uint32_t>(constIn(n, 1));
      for (uint32_t ch = 0; ch < W; ++ch) sends_[{bus, ch}] += in(n, 0, ch, v);
      return;
    }
    for (uint32_t ch = 0; ch < W; ++ch) evalAtom(n, a, ch, v);
  }

  static double b01(bool x) { return x ? 1.0 : 0.0; }

  void evalAtom(NodeIndex n, const AName& a, uint32_t ch, uint32_t v) {
    auto I = [&](uint32_t i) { return in(n, i, ch, v); };
    const double x = I(0), y = p_.node(n).numInputs > 1 ? I(1) : 0.0, z = p_.node(n).numInputs > 2 ? I(2) : 0.0;
    double r = 0.0;
    if (a == "add") r = x + y;
    else if (a == "sub") r = x - y;
    else if (a == "mul") r = x * y;
    else if (a == "div") r = x / y;
    else if (a == "mod") {  // a result that rounds to b in float is 0 (pq/dsp/atoms.h)
      r = x - y * std::floor(x / y);
      if (static_cast<float>(r) == static_cast<float>(y)) r = 0.0;
    }
    else if (a == "neg") r = -x;
    else if (a == "min") r = x < y ? x : y;
    else if (a == "max") r = x > y ? x : y;
    else if (a == "clip") {
      const double m = x > y ? x : y;
      r = m < z ? m : z;
    } else if (a == "abs") r = std::fabs(x);
    else if (a == "sign") r = x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0;
    else if (a == "floor") r = std::floor(x);
    else if (a == "frac") {
      const float f = static_cast<float>(x - std::floor(x));
      r = f < 1.0f ? f : 0.99999994f;
    } else if (a == "tan" || a == "fast_tan") r = std::tan(x);
    else if (a == "tanh" || a == "fast_tanh") r = std::tanh(x);
    else if (a == "exp" || a == "fast_exp") r = std::exp(x);
    else if (a == "log" || a == "fast_log") r = std::log(x);
    else if (a == "pow") r = std::pow(x, y);
    else if (a == "fast_pow") r = x < 0 ? NAN : x == 0 ? (y > 0 ? 0.0 : std::pow(x, y)) : std::exp2(y * std::log2(x));
    else if (a == "sqrt" || a == "fast_sqrt") r = std::sqrt(x);
    else if (a == "sin" || a == "fast_sin") r = std::sin(x);
    else if (a == "muladd") r = x * y + z;
    else if (a == "cents_to_hz" || a == "fast_cents_to_hz") r = y * std::exp2(x / 1200.0);
    else if (a == "hz_to_cents" || a == "fast_hz_to_cents") r = 1200.0 * std::log2(x / y);
    else if (a == "db_to_gain" || a == "fast_db_to_gain") r = std::pow(10.0, x / 20.0);
    else if (a == "gain_to_db" || a == "fast_gain_to_db") r = 20.0 * std::log10(x);
    else if (a == "select") r = x != 0.0 ? y : z;
    else if (a == "lerp") r = x + z * (y - x);
    else if (a == "gate_to_real") r = b01(x != 0.0);
    else if (a == "lt") r = b01(x < y);
    else if (a == "le") r = b01(x <= y);
    else if (a == "gt") r = b01(x > y);
    else if (a == "ge") r = b01(x >= y);
    else if (a == "eq") r = b01(x == y);
    else if (a == "ne") r = b01(!(x == y));
    else if (a == "and") r = b01(x != 0 && y != 0);
    else if (a == "or") r = b01(x != 0 || y != 0);
    else if (a == "xor") r = b01((x != 0) != (y != 0));
    else if (a == "nand") r = b01(!(x != 0 && y != 0));
    else if (a == "nor") r = b01(!(x != 0 || y != 0));
    else if (a == "not") r = b01(x == 0);
    else if (a == "mux") {
      const uint32_t k = static_cast<uint32_t>(p_.node(n).numInputs) - 1u;
      double idx = std::isnan(x) ? 0.0 : std::floor(x);
      idx = idx < 0 ? 0 : idx > k - 1 ? k - 1 : idx;
      r = I(1 + static_cast<uint32_t>(idx));
    } else if (a == "demux") {
      const uint32_t k = p_.node(n).numOutputs;
      double c = std::isnan(x) ? 0.0 : std::floor(x);
      c = c < 0 ? 0 : c > k - 1 ? k - 1 : c;
      for (uint32_t o = 0; o < k; ++o) set(n, o, ch, v, o == static_cast<uint32_t>(c) ? y : 0.0);
      return;
    } else if (a == "schmitt") {
      St& s = stateOf(n, ch, v);
      s.a = x > z ? 1.0 : x < y ? 0.0 : s.a;
      r = s.a;
    } else if (a == "rise" || a == "fall" || a == "change") {
      St& s = stateOf(n, ch, v);
      const bool now = x != 0, prev = s.a != 0;
      r = a == "rise" ? b01(now && !prev) : a == "fall" ? b01(!now && prev) : b01(now != prev);
      s.a = b01(now);
    } else if (a == "srff") {
      St& s = stateOf(n, ch, v);
      s.a = y != 0 ? 0.0 : x != 0 ? 1.0 : s.a;
      r = s.a;
    } else if (a == "dff" || a == "tff") {
      St& s = stateOf(n, ch, v);
      const bool edge = y != 0 && s.b == 0;
      if (a == "dff") s.a = edge ? b01(x != 0) : s.a;
      else if (edge && x != 0) s.a = b01(s.a == 0);
      s.b = b01(y != 0);
      r = s.a;
    } else if (a == "counter") {
      St& s = stateOf(n, ch, v);
      const bool u = x != 0, d = y != 0;
      const bool eu = u && s.a == 0, ed = d && s.b == 0;
      s.a = b01(u);
      s.b = b01(d);
      s.i += (eu ? 1 : 0) - (ed ? 1 : 0);
      const int64_t wrap = countFrom(I(3), 0);
      if (wrap > 0) {
        if (s.i > wrap - 1) s.i = 0;
        if (s.i < 0) s.i = wrap - 1;
      }
      if (z != 0) s.i = 0;
      r = static_cast<double>(s.i);
    } else if (a == "clockdiv") {
      St& s = stateOf(n, ch, v);
      const bool c = x != 0;
      const bool edge = c && s.a == 0;
      s.a = b01(c);
      const int64_t k = countFrom(y, 1);
      if (edge) {
        const int64_t next = s.i + 1;
        s.i = k > next ? next : 0;
      }
      r = b01(c && s.i == 0);
    } else if (a == "pulse") {
      St& s = stateOf(n, ch, v);
      const bool fire = s.i == 0;
      const int64_t per = countFrom(samplesOf(n, 0, ch, v), 0);
      s.i = fire ? per - 1 : s.i > 0 ? s.i - 1 : s.i;
      r = b01(fire);
    } else if (a == "timer") {
      St& s = stateOf(n, ch, v);
      const bool tr = x != 0;
      const bool edge = tr && s.a == 0;
      s.a = b01(tr);
      double d = samplesOf(n, 1, ch, v);
      d = std::isnan(d) ? 0.0 : std::clamp(d, 0.0, 2147483520.0);
      if (edge) s.i = static_cast<int64_t>(std::nearbyint(static_cast<float>(d)));
      r = b01(s.i > 0);
      if (s.i > 0) --s.i;
    } else if (a == "z1") {
      r = stateOf(n, ch, v).a;
    } else if (a == "delay" || a == "delay_cubic" || a == "delay_allpass") {
      r = delayRead(n, a, ch, v, samplesOf(n, 1, ch, v));
    } else if (a == "samplehold") {
      St& s = stateOf(n, ch, v);
      const bool tr = y != 0;
      if (tr && s.b == 0) s.a = x;
      s.b = b01(tr);
      r = s.a;
    } else if (a == "latch") {
      St& s = stateOf(n, ch, v);
      if (y != 0) s.a = x;
      r = s.a;
    } else if (a == "phasor" || a == "sine_hz") {
      St& s = stateOf(n, ch, v);
      if (y != 0) s.u = 0;
      r = a == "phasor" ? static_cast<double>(s.u >> 8) * 0x1p-24
                        : std::sin(2 * kPi * static_cast<double>(static_cast<int32_t>(s.u)) * 0x1p-32);
    } else if (a == "sine") {
      r = std::sin(2 * kPi * (x - std::nearbyint(x)));
    } else if (a == "wavetable") {
      // pq/dsp/tables.h: increment estimate = the smaller of the last two |Δphase| (wrapped to [0, 0.5]), u =
      // 2·log2(N·inc) from the float bits, levels floor(u) and floor(u) + 1 (clamped) crossfaded by min(2·frac(u),
      // 1), each read with linear interpolation. Evaluated in float like the signal it reads, on the same table
      // data the program is given.
      const uint32_t tbl = static_cast<uint32_t>(constIn(n, 1));
      if (tbl >= st_.wavetables.size() || st_.wavetables[tbl] == nullptr) {
        ok_ = false;
        why_ = "no wavetable " + std::to_string(tbl);
        return;
      }
      const pq::dsp::WavetableView& tv = *st_.wavetables[tbl];
      St& s = stateOf(n, ch, v);
      const float p = static_cast<float>(x), prev = static_cast<float>(s.a);
      float d = p - prev;
      d = std::fabs(d - std::nearbyint(d));
      if (std::isnan(d)) d = 0.0f;
      const float inc = std::min(d, static_cast<float>(s.b));
      s.b = d;
      const double u = 2.0 * (std::bit_cast<uint32_t>(inc * tv.sizeF) * 0x1p-23 - 127.0);
      const double k = std::floor(u);
      const double t = std::min(2.0 * (u - k), 1.0);
      const int32_t top = static_cast<int32_t>(tv.levels) - 1;
      const int32_t lo = std::clamp<int32_t>(static_cast<int32_t>(k), 0, top);
      const int32_t hi = std::clamp<int32_t>(static_cast<int32_t>(k) + 1, 0, top);
      const float pos = (p - std::floor(p)) * tv.sizeF;
      const auto i = static_cast<int32_t>(pos);
      const float fr = pos - static_cast<float>(i);
      const auto read = [&](int32_t lvl) {
        const size_t base = static_cast<size_t>(lvl) * tv.stride + (static_cast<uint32_t>(i) & (tv.size - 1));
        const double y0 = tv.data[base], y1 = tv.data[base + 1];
        return y0 + static_cast<double>(fr) * (y1 - y0);
      };
      const double ra = read(lo);
      r = ra + t * (read(hi) - ra);
      s.a = p;
    } else if (a == "noise") {
      const uint32_t seed = st_.noiseSeed ? st_.noiseSeed(n, static_cast<uint16_t>(ch)) : 0u;
      const bool voice = tp_.outputType(n, 0).poly == Poly::Voice;
      const uint32_t lane = voice ? v % 8 : 0, group = voice ? v / 8 : 0;
      const uint32_t h = lowbias32(static_cast<uint32_t>(t_) + mixSeed(seed + group * 0x9E3779B9u, lane));
      r = static_cast<double>(static_cast<int32_t>(h) >> 8) * 0x1p-23;
    } else if (a == "shaper") {
      const uint32_t tbl = static_cast<uint32_t>(constIn(n, 1));
      ShaperSpec def;
      def.points = {-1.0f, 1.0f};
      const ShaperSpec& sp = tbl < st_.shapers.size() && st_.shapers[tbl].points.size() >= 2 ? st_.shapers[tbl] : def;
      const double P = static_cast<double>(sp.points.size());
      const double scale = static_cast<float>((P - 1) / (static_cast<double>(sp.hi) - sp.lo));
      double tt = (x - sp.lo) * scale;
      tt = std::isnan(tt) ? 0.0 : std::clamp(tt, 0.0, P - 1);
      const uint32_t i = std::min(static_cast<uint32_t>(tt), static_cast<uint32_t>(P) - 2);
      const double fr = tt - i;
      r = sp.points[i] + fr * (static_cast<double>(sp.points[i + 1]) - sp.points[i]);
    } else if (a == "sample_pos_linear" || a == "sample_pos_cubic") {
      const std::vector<float>& d = sampleData(static_cast<uint32_t>(constIn(n, 0)));
      const double pos = I(1);
      const auto len = static_cast<int64_t>(d.size());
      const bool valid = pos >= 0 && pos < static_cast<double>(len);
      const double fl = std::floor(pos);
      const int64_t i = std::clamp<int64_t>(std::isnan(fl) ? 0 : static_cast<int64_t>(fl), 0, len - 1);
      const double y2 = interp(d, i, pos - fl, a == "sample_pos_cubic");
      r = valid ? y2 : 0.0;
    } else if (a == "sample_read_linear" || a == "sample_read_cubic") {
      St& s = stateOf(n, ch, v);
      const std::vector<float>& d = sampleData(static_cast<uint32_t>(constIn(n, 0)));
      const auto len = static_cast<int64_t>(d.size());
      if (I(2) != 0) {  // reset (level)
        int64_t hi;
        uint32_t lo;
        toFixed(I(3), hi, lo);
        s.i = hi;
        s.frac = lo;
        s.active = hi >= 0 && hi < len;
      }
      const int64_t i = std::clamp<int64_t>(s.i, 0, len - 1);
      const double fr = static_cast<double>(s.frac >> 8) * 0x1p-24;
      set(n, 0, ch, v, s.active ? interp(d, i, fr, a == "sample_read_cubic") : 0.0);
      set(n, 1, ch, v, b01(s.active));
      return;
    } else {
      ok_ = false;
      why_ = "unknown atom " + std::string(a.s);
    }
    set(n, 0, ch, v, r);
  }

  static int64_t countFrom(double x, int64_t fallback) {
    const float f = static_cast<float>(x);
    if (!(f >= 1.0f && f < 2147483648.0f)) return fallback;
    return static_cast<int64_t>(std::nearbyint(f));
  }

  const std::vector<float>& sampleData(uint32_t idx) const {
    static const std::vector<float> silent{0.0f};
    return idx < st_.samples.size() && !st_.samples[idx].empty() ? st_.samples[idx] : silent;
  }
  static double at(const std::vector<float>& d, int64_t i) { return i >= 0 && i < static_cast<int64_t>(d.size()) ? d[i] : 0.0; }
  static double hermite(double xm1, double x0, double x1, double x2, double t) {
    const double c1 = 0.5 * (x1 - xm1);
    const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
    const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
  }
  static double interp(const std::vector<float>& d, int64_t i, double fr, bool cubic) {
    if (cubic) return hermite(at(d, i - 1), at(d, i), at(d, i + 1), at(d, i + 2), fr);
    return at(d, i) + fr * (at(d, i + 1) - at(d, i));
  }
  static void toFixed(double x, int64_t& hi, uint32_t& lo) {
    const float xf = static_cast<float>(x);
    double c = std::isnan(xf) ? -1048576.0 : std::clamp<double>(xf, -1048576.0, 1048576.0);
    const double fl = std::floor(c);
    hi = static_cast<int64_t>(fl);
    lo = static_cast<uint32_t>(std::floor((c - fl) * 4294967296.0));
  }
  static int64_t clampedFloor(double x) {
    const float xf = static_cast<float>(x);
    const double c = std::isnan(xf) ? -1073741824.0 : std::clamp<double>(xf, -1073741824.0, 2147483520.0);
    return static_cast<int64_t>(std::floor(c));
  }

  double delayRead(NodeIndex n, const AName& a, uint32_t ch, uint32_t v, double dIn) {
    St& s = stateOf(n, ch, v);
    const double minD = a == "delay" ? 1.0 : a == "delay_cubic" ? 2.0 : 1.5;
    const double maxD = maxDelay_[n];
    const float df = static_cast<float>(dIn);
    double d = std::isnan(df) ? minD : std::clamp<double>(df, minD, maxD);
    double fl = std::floor(d);
    double fr = static_cast<float>(d - fl);
    auto nIdx = static_cast<int64_t>(fl);
    const auto t = static_cast<int64_t>(t_);
    auto x = [&](int64_t k) -> double { return k >= 0 && k < static_cast<int64_t>(s.hist.size()) ? s.hist[k] : 0.0; };
    if (a == "delay") {
      const double x0 = x(t - nIdx), x1 = x(t - nIdx - 1);
      return x0 + fr * (x1 - x0);
    }
    if (a == "delay_cubic") return hermite(x(t - nIdx + 1), x(t - nIdx), x(t - nIdx - 1), x(t - nIdx - 2), fr);
    if (fr < 0.5) {
      fr += 1.0;
      nIdx -= 1;
    }
    const double x0 = x(t - nIdx), x1 = x(t - nIdx - 1);
    const double c = (1.0 - fr) / (1.0 + fr);
    const double y = static_cast<float>(c * (x0 - s.c) + x1);
    s.c = y;
    return y;
  }

  double reduce(NodeIndex n, uint32_t ch) {
    const AName& a = name_[n];
    const bool voiceIn = tp_.inputType(n, 0).poly == Poly::Voice;
    if (!voiceIn) return in(n, 0, ch, 0);
    if (V_ == 0) return 0.0;
    const bool isBool = tp_.inputType(n, 0).kind == Kind::Bool;
    if (a == "voice_last") return st_.lastVoice < V_ ? in(n, 0, ch, st_.lastVoice) : 0.0;
    if (isBool) {
      bool any = false, all = true;
      for (uint32_t v = 0; v < V_; ++v) {
        const bool x = in(n, 0, ch, v) != 0;
        any = any || x;
        all = all && x;
      }
      return a == "voice_lowest" ? b01(all) : b01(any);
    }
    double acc = a == "voice_highest" ? -INFINITY : a == "voice_lowest" ? INFINITY : 0.0;
    for (uint32_t v = 0; v < V_; ++v) {
      const double x = in(n, 0, ch, v);
      if (a == "voice_sum") acc += x;
      else if (a == "voice_highest") acc = x > acc ? x : acc;
      else if (a == "voice_lowest") acc = x < acc ? x : acc;
      else if (a == "voice_loudest") acc = std::fabs(x) > std::fabs(acc) ? x : acc;
    }
    return acc;
  }

  // End of the sample: state the next sample reads through a delayed input.
  void update(NodeIndex n) {
    if (p_.node(n).kind != NodeKind::Atom) return;
    const AName& a = name_[n];
    const bool z = a == "z1", del = a == "delay" || a == "delay_cubic" || a == "delay_allpass";
    const bool ph = a == "phasor" || a == "sine_hz", sr = a.starts_with("sample_read");
    if (!z && !del && !ph && !sr) return;
    for (uint32_t v = 0; v < nodeVoices_[n]; ++v)
      for (uint32_t ch = 0; ch < width_[n]; ++ch) {
        St& s = stateOf(n, ch, v);
        if (z) s.a = in(n, 0, ch, v);
        else if (del) s.hist.push_back(static_cast<float>(in(n, 0, ch, v)));
        else if (ph) {
          const float halfSr = static_cast<float>(0.5 * sr_);
          float hz = static_cast<float>(in(n, 0, ch, v));
          hz = std::isnan(hz) ? 0.0f : std::clamp(hz, -halfSr, halfSr);
          const double incD = std::nearbyint(static_cast<double>(hz) * (4294967296.0 / sr_));
          s.u += static_cast<uint32_t>(static_cast<int64_t>(incD));
        } else {
          const std::vector<float>& d = sampleData(static_cast<uint32_t>(constIn(n, 0)));
          const auto len = static_cast<int64_t>(d.size());
          int64_t incHi;
          uint32_t incLo;
          toFixed(in(n, 1, ch, v), incHi, incLo);
          const uint64_t sum = static_cast<uint64_t>(s.frac) + incLo;
          const uint32_t newFrac = static_cast<uint32_t>(sum);
          int64_t next = s.i + incHi + static_cast<int64_t>(sum >> 32);
          const int64_t ls = clampedFloor(in(n, 4, ch, v)), le = clampedFloor(in(n, 5, ch, v));
          const bool loopOn = le > ls;
          const bool fwd = loopOn && s.i < le && next > le - 1;
          const bool bwd = loopOn && s.i > ls - 1 && next < ls;
          if (fwd) next = ls + (next - le) % (le - ls);
          if (bwd) next = le - 1 - (ls - 1 - next) % (le - ls);
          const bool inRange = next >= 0 && next < len;
          if (s.active) {
            s.i = next;
            s.frac = newFrac;
          }
          s.active = s.active && inRange;
        }
      }
  }

  // ---- solve blocks: Newton in double with the same iteration count ----
  void solveItem(uint32_t si) {
    SolveInfo& S = solves_[si];
    const NodeIndex first = S.unknowns.front();
    const uint32_t vn = nodeVoices_[first];
    for (uint32_t v = 0; v < vn; ++v) solveVoice(S, v);
  }

  void bodyEval(const SolveInfo& S, uint32_t v) {
    for (NodeIndex b : S.body) evalVoice(b, v);
  }

  void solveVoice(SolveInfo& S, uint32_t v) {
    // Unknown channels → flat list.
    struct U {
      NodeIndex n;
      uint32_t ch;
    };
    std::vector<U> us, es;
    for (NodeIndex u : S.unknowns)
      for (uint32_t ch = 0; ch < width_[u]; ++ch) us.push_back({u, ch});
    for (NodeIndex e : S.equations)
      for (uint32_t ch = 0; ch < width_[e]; ++ch) es.push_back({e, ch});
    const size_t n = us.size();
    if (es.size() != n) {
      ok_ = false;
      why_ = "solve block with unequal counts";
      return;
    }
    std::vector<double> u(n), r0(n), rp(n), rm(n), J(n * n);
    for (size_t j = 0; j < n; ++j) {
      const NodeIndex un = us[j].n;
      if (driverNode(un, 0) != kNone) u[j] = in(un, 0, us[j].ch, v);
      else u[j] = stateOf(un, us[j].ch, v).a;
    }
    const bool saveRound = round_;
    round_ = false;
    auto write = [&]() {
      for (size_t j = 0; j < n; ++j) set(us[j].n, 0, us[j].ch, v, u[j]);
    };
    auto res = [&](std::vector<double>& r) {
      for (size_t i = 0; i < n; ++i) r[i] = in(es[i].n, 0, es[i].ch, v) - in(es[i].n, 1, es[i].ch, v);
    };
    write();
    for (uint32_t it = 0; it < S.iterations; ++it) {
      bodyEval(S, v);
      res(r0);
      for (size_t j = 0; j < n; ++j) {
        const double saved = u[j];
        const double h = 1e-7 * std::max(std::fabs(saved), 1.0);
        u[j] = saved + h;
        write();
        bodyEval(S, v);
        res(rp);
        u[j] = saved - h;
        write();
        bodyEval(S, v);
        res(rm);
        u[j] = saved;
        write();
        for (size_t i = 0; i < n; ++i) J[i * n + j] = (rp[i] - rm[i]) / (2 * h);
      }
      // Gaussian elimination with partial pivoting.
      std::vector<double> M(n * (n + 1));
      for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) M[i * (n + 1) + j] = J[i * n + j];
        M[i * (n + 1) + n] = r0[i];
      }
      bool good = true;
      const size_t w = n + 1;
      for (size_t c = 0; c < n && good; ++c) {
        size_t piv = c;
        for (size_t i = c + 1; i < n; ++i)
          if (std::fabs(M[i * w + c]) > std::fabs(M[piv * w + c])) piv = i;
        if (!(std::fabs(M[piv * w + c]) > 0.0)) {
          good = false;
          break;
        }
        if (piv != c)
          for (size_t j = 0; j < w; ++j) std::swap(M[c * w + j], M[piv * w + j]);
        for (size_t i = c + 1; i < n; ++i) {
          const double f = M[i * w + c] / M[c * w + c];
          for (size_t j = c; j < w; ++j) M[i * w + j] -= f * M[c * w + j];
        }
      }
      if (good) {
        for (size_t i = n; i-- > 0;) {
          double s = M[i * w + n];
          for (size_t j = i + 1; j < n; ++j) s -= M[i * w + j] * M[j * w + n];
          M[i * w + n] = s / M[i * w + i];
        }
        for (size_t i = 0; i < n; ++i)
          if (!std::isfinite(u[i] - M[i * w + n])) good = false;
        if (good)
          for (size_t i = 0; i < n; ++i) u[i] -= M[i * w + n];
      }
      write();
    }
    round_ = saveRound;
    // The solution, rounded once; the body evaluated with it (values read after the block).
    for (size_t j = 0; j < n; ++j) u[j] = static_cast<float>(u[j]);
    write();
    bodyEval(S, v);
    for (size_t j = 0; j < n; ++j)
      if (driverNode(us[j].n, 0) == kNone) stateOf(us[j].n, us[j].ch, v).a = u[j];
  }

  const TypedPatch& tp_;
  const Patch& p_;
  Stimulus st_;
  double sr_ = 48000.0;
  uint32_t V_ = 0;
  uint64_t t_ = 0;
  bool ok_ = true;
  bool round_ = true;
  std::string why_;
  Adjacency adj_;
  std::vector<AName> name_;
  std::vector<uint32_t> nodeVoices_;
  std::vector<uint16_t> width_;
  std::map<NodeIndex, uint32_t> inputOrdinal_;
  std::map<NodeIndex, float> maxDelay_;
  std::vector<size_t> portOff_;
  std::vector<double> vals_;
  std::vector<std::vector<St>> state_;
  std::vector<SolveInfo> solves_;
  std::vector<uint32_t> itemOf_;
  std::vector<Item> items_;
  std::map<std::pair<uint32_t, uint32_t>, double> sends_;
};

}  // namespace refeval
