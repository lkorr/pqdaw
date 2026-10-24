// The optimiser (T1.4). See pq/graph/optimise.h for the passes and the invariants they keep.
//
// Working form: the TypedPatch's nodes copied into flat arrays (one entry per node, per in-port, per out-port), with
// in-ports holding the out-port that drives them. Rewrites never edit wires: replacing a value forwards its
// out-port to another one (fwd_, resolved with path compression when read), so a rewrite is O(1) and consumers
// pick it up when they are next visited. Nodes created here are appended. emit() builds a fresh, compact Patch.
//
// Simplification rules (all value-exact in float32 with FTZ/DAZ, i.e. allowed without nnan/ninf/nsz; the one
// approximate-function rewrite is marked afn):
//   x + (−0) → x            x + (+0) → x if x is never −0      x − (+0) → x      x − (−0) → x if x is never −0
//   x · 1, x / 1 → x        x · (−1), x / (−1) → −x            x / ±2^k → x · ±2^−k (1/c exact and normal)
//   x · (±0) → ±0 if x is finite and its sign bit is clear (x ≥ +0), so the product's sign is the zero's
//   pow(x, 1) → x           pow(x, 2) → x · x (afn: x·x is the correctly rounded square; the CRT pow is within a
//                           few ulp of it)
//   −(−x) → x   !!x → x     abs(abs x) → abs x     abs(−x) → abs x     min/max(x, x) → x
//   x && true, x || false, x ^^ false → x;  x && false → false;  x || true → true;  x ^^ true → !x;
//   x && x, x || x → x;  x ^^ x → false
//   select(literal, a, b) → a or b;  select(c, a, a) → a;  mux(literal i, …) → in[clamp(floor i)] (NaN → in0)
//   channel(x, i) → x for a mono x;  channel(pack(…), literal i) → the pack input holding channel i, if it is mono
//   voice_*(x) → x for a global x (the registry: "a global x passes through unchanged")
// "Finite"/"sign bit clear" come from a small forward analysis (Facts): literals, and atoms whose range is known
// (phasor, gate_to_real, abs, clip with finite literal bounds, …). Params and engine inputs are never assumed finite.

#include "pq/graph/optimise.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "pq/core/denormal_guard.h"
#include "pq/dsp/atoms.h"
#include "pq/graph/kernel_map.h"

namespace pq::graph {

namespace {

using namespace atom_flags;

uint64_t hashMix(uint64_t h, uint64_t v) noexcept {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 31;
  h *= 0x94D049BB133111EBull;
  h ^= h >> 29;
  return h;
}

Rate maxRate(Rate a, Rate b) noexcept { return a < b ? b : a; }

// What is known about a real value at every sample.
struct Facts {
  bool finite = false;     // never ±inf or NaN
  bool signClear = false;  // the sign bit is clear whenever the value is not NaN (so it is never −0)
};

constexpr uint32_t kMaxFoldPorts = 64;

// Value-numbering table: 64-bit structural hash → node, open addressing with linear probing. Cleared every sweep;
// a hash collision between different nodes only costs a missed merge (callers compare the nodes).
class HashTable {
public:
  void reset(size_t expected) {
    size_t cap = 16;
    while (cap < expected * 2) cap *= 2;
    keys_.assign(cap, 0);
    vals_.assign(cap, kNone);
    size_ = 0;
  }
  uint32_t find(uint64_t h) const {
    const size_t mask = keys_.size() - 1;
    for (size_t i = static_cast<size_t>(h) & mask;; i = (i + 1) & mask) {
      if (vals_[i] == kNone) return kNone;
      if (keys_[i] == h) return vals_[i];
    }
  }
  // The node already stored under h, or kNone after storing n.
  uint32_t findOrInsert(uint64_t h, uint32_t n) {
    if ((size_ + 1) * 2 > keys_.size()) grow();
    const size_t mask = keys_.size() - 1;
    for (size_t i = static_cast<size_t>(h) & mask;; i = (i + 1) & mask) {
      if (vals_[i] == kNone) {
        keys_[i] = h;
        vals_[i] = n;
        ++size_;
        return kNone;
      }
      if (keys_[i] == h) return vals_[i];
    }
  }

private:
  void grow() {
    std::vector<uint64_t> k = std::move(keys_);
    std::vector<uint32_t> v = std::move(vals_);
    keys_.assign(k.size() * 2, 0);
    vals_.assign(k.size() * 2, kNone);
    size_ = 0;
    for (size_t i = 0; i < k.size(); ++i)
      if (v[i] != kNone) findOrInsert(k[i], v[i]);
  }
  std::vector<uint64_t> keys_;
  std::vector<uint32_t> vals_;
  size_t size_ = 0;
};

struct WNode {
  NodeKind kind = NodeKind::Atom;
  AtomId atom = 0;
  uint16_t nIn = 0, nOut = 0;
  uint32_t firstIn = 0;   // into drv_
  uint32_t firstOut = 0;  // into the out-port arrays
  NodeIndex src = kNone;  // node of the input patch; kNone for nodes created here
  uint32_t payload = kNone;  // Const: index into consts_; Unknown/Equation: solve block of the input patch
  uint64_t key = 0;
  NodeOrigin origin;
  bool alive = true;
  bool replaced = false;  // every output forwarded: skipped by the sweep, removed by DCE
};

struct Atoms {
  AtomId add, sub, mul, div, neg, pow, abs, min, max, muladd, and_, or_, xor_, not_, select, mux, channel, pack, sr;
  AtomId phasor, gateToReal, noise, sineHz, sine, sin, tanh, fastTanh, sign, counter, floor, frac, clip,
      exp, sqrt, fastExp, fastSqrt, voiceSum;
  Atoms() {
    add = atomId("add"); sub = atomId("sub"); mul = atomId("mul"); div = atomId("div"); neg = atomId("neg");
    pow = atomId("pow"); abs = atomId("abs"); min = atomId("min"); max = atomId("max"); muladd = atomId("muladd");
    and_ = atomId("and"); or_ = atomId("or"); xor_ = atomId("xor"); not_ = atomId("not");
    select = atomId("select"); mux = atomId("mux"); channel = atomId("channel"); pack = atomId("pack");
    sr = atomId("sr"); phasor = atomId("phasor"); gateToReal = atomId("gate_to_real"); noise = atomId("noise");
    sineHz = atomId("sine_hz"); sine = atomId("sine"); sin = atomId("sin");
    tanh = atomId("tanh"); fastTanh = atomId("fast_tanh"); sign = atomId("sign"); counter = atomId("counter");
    floor = atomId("floor"); frac = atomId("frac"); clip = atomId("clip"); exp = atomId("exp");
    sqrt = atomId("sqrt"); fastExp = atomId("fast_exp"); fastSqrt = atomId("fast_sqrt");
    voiceSum = atomId("voice_sum");
  }
};

const Atoms& atoms() {
  static const Atoms a;
  return a;
}

// Index of the registry spec that input i of a node of `sig` uses (the repeated spec for variadic inputs).
uint32_t specIndex(const AtomSignature& sig, uint32_t i) noexcept {
  const auto n = static_cast<uint32_t>(sig.inputs.size());
  return i < n ? i : n - 1;
}

bool isPure(const AtomSignature& sig) noexcept { return !sig.has(kStateful) && !sig.has(kSideEffect); }

bool isVoiceReducer(const AtomSignature& sig) noexcept {
  return sig.category == AtomCategory::Structure && sig.name.starts_with("voice_");
}

// Runs a stateless kernel for one frame on one lane: the value a param-rate evaluation of the node produces at
// runtime (kernel.h: "a param-rate computation is the same kernel run with frames = 1"), with FTZ/DAZ as on the
// audio thread.
bool evalKernel(dsp::Atom k, const float* in, uint32_t nIn, float* out, uint32_t nOut) {
  const dsp::KernelFn fn = dsp::findKernel(k, 1, 0);
  if (!fn) return false;
  struct alignas(32) Slot {
    float v[dsp::kPatternFloats];
  };
  Slot ib[kMaxFoldPorts], ob[kMaxFoldPorts];
  const float* ip[kMaxFoldPorts];
  float* op[kMaxFoldPorts];
  for (uint32_t i = 0; i < nIn; ++i) {
    std::fill(std::begin(ib[i].v), std::end(ib[i].v), in[i]);
    ip[i] = ib[i].v;
  }
  for (uint32_t j = 0; j < nOut; ++j) op[j] = ob[j].v;
  dsp::KernelArgs a;
  a.in = ip;
  a.out = op;
  a.frames = 1;
  a.numIn = static_cast<uint16_t>(nIn);
  a.numOut = static_cast<uint16_t>(nOut);
  a.splMask = 0;
  {
    DenormalGuard guard;
    fn(a);
  }
  for (uint32_t j = 0; j < nOut; ++j) out[j] = ob[j].v[0];
  return true;
}

class Optimiser {
public:
  Optimiser(TypedPatch& tp, const OptimiseOptions& o, OptimiseStats& s) : tp_(tp), p_(tp.patch), opt_(o), st_(s) {}

  Result<TypedPatch> run() {
    build();
    st_.nodesBefore = p_.nodeCount();
    if (opt_.lowerDurations && !tp_.durationsInSamples) lowerDurations();
    const uint32_t cap = std::max<uint32_t>(opt_.maxPasses, 1);
    for (uint32_t pass = 0; pass < cap; ++pass) {
      const uint32_t changes = sweep();
      eliminateDead();
      ++st_.passes;
      if (changes == 0) {
        st_.converged = true;
        break;
      }
    }
    refineRates();
    if (opt_.fuseMulAdd && fuse() > 0) eliminateDead();
    if (Status s = checkChannels(); !s) return s.error();
    TypedPatch out = emit();
    st_.nodesAfter = out.patch.nodeCount();
    return out;
  }

private:
  // ---- working form -----------------------------------------------------------------------------------------------

  void build() {
    const uint32_t n = p_.nodeCount();
    nodes_.reserve(n + n / 4 + 8);
    names_.reserve(n + n / 4 + 8);
    uint32_t nIn = 0, nOut = 0;
    for (NodeIndex v = 0; v < n; ++v) {
      const Node& nd = p_.node(v);
      WNode w;
      w.kind = nd.kind;
      w.atom = nd.atom;
      w.nIn = nd.numInputs;
      w.nOut = nd.numOutputs;
      w.firstIn = nIn;
      w.firstOut = nOut;
      w.src = v;
      w.key = v < tp_.stateKey.size() ? tp_.stateKey[v] : 0;
      w.origin = v < tp_.origin.size() ? tp_.origin[v] : NodeOrigin{};
      if (nd.kind == NodeKind::Const) {
        w.payload = static_cast<uint32_t>(consts_.size());
        consts_.push_back(p_.constant(v));
      } else if (nd.kind == NodeKind::Unknown || nd.kind == NodeKind::Equation) {
        w.payload = nd.payload;
      }
      nIn += nd.numInputs;
      nOut += nd.numOutputs;
      nodes_.push_back(w);
      names_.push_back({p_.nodeName(v), nd.nameOutput});
    }
    drv_.assign(nIn, kNone);
    origWire_.assign(nIn, kNone);
    outT_.resize(nOut);
    outOwner_.resize(nOut);
    fwd_.assign(nOut, kNone);
    facts_.assign(nOut, Facts{});
    for (NodeIndex v = 0; v < n; ++v) {
      const WNode& w = nodes_[v];
      for (uint32_t j = 0; j < w.nOut; ++j) {
        outOwner_[w.firstOut + j] = v;
        outT_[w.firstOut + j] = tp_.types[p_.flatOutput(v, j)];
      }
    }
    for (WireIndex wi = 0; wi < p_.wireCount(); ++wi) {
      const Wire& wr = p_.wire(wi);
      if (wr.from.node >= n || wr.to.node >= n) continue;
      if (wr.from.port >= nodes_[wr.from.node].nOut || wr.to.port >= nodes_[wr.to.node].nIn) continue;
      const uint32_t in = nodes_[wr.to.node].firstIn + wr.to.port;
      if (drv_[in] != kNone) continue;  // the first driver wins (as in Patch::adjacency)
      drv_[in] = nodes_[wr.from.node].firstOut + wr.from.port;
      origWire_[in] = wi;
    }
    origDrv_ = drv_;
    cse_.reset(n);
    solveMembers_.assign(p_.solveCount(), {});
    for (NodeIndex v = 0; v < n; ++v)
      if ((nodes_[v].kind == NodeKind::Unknown || nodes_[v].kind == NodeKind::Equation) &&
          nodes_[v].payload < solveMembers_.size())
        solveMembers_[nodes_[v].payload].push_back(v);
  }

  uint32_t resolve(uint32_t o) {
    if (o == kNone) return kNone;
    uint32_t r = o;
    while (fwd_[r] != kNone) r = fwd_[r];
    while (fwd_[o] != kNone) {  // path compression
      const uint32_t next = fwd_[o];
      fwd_[o] = r;
      o = next;
    }
    return r;
  }

  // The (resolved) driver of input i of node n, or kNone.
  uint32_t in(uint32_t n, uint32_t i) {
    uint32_t& d = drv_[nodes_[n].firstIn + i];
    d = resolve(d);
    return d;
  }

  uint32_t owner(uint32_t o) const { return outOwner_[o]; }
  const AtomSignature& sigOf(AtomId a) const noexcept { return reg_.get(a); }  // no static-init guard per call
  uint32_t out(uint32_t n, uint32_t j = 0) const { return nodes_[n].firstOut + j; }

  uint32_t addNode(NodeKind kind, AtomId atom, std::span<const uint32_t> drivers, uint32_t nOut,
                   const PortType& outType, uint64_t key, const NodeOrigin& origin) {
    WNode w;
    w.kind = kind;
    w.atom = atom;
    w.nIn = static_cast<uint16_t>(drivers.size());
    w.nOut = static_cast<uint16_t>(nOut);
    w.firstIn = static_cast<uint32_t>(drv_.size());
    w.firstOut = static_cast<uint32_t>(outT_.size());
    w.key = key;
    w.origin = origin;
    const auto id = static_cast<uint32_t>(nodes_.size());
    for (uint32_t d : drivers) {
      drv_.push_back(d);
      origDrv_.push_back(kNone);
      origWire_.push_back(kNone);
    }
    for (uint32_t j = 0; j < nOut; ++j) {
      outT_.push_back(outType);
      outOwner_.push_back(id);
      fwd_.push_back(kNone);
      facts_.push_back({});
    }
    nodes_.push_back(w);
    names_.push_back({});
    return id;
  }

  // A literal out-port: an existing literal with the same value (this sweep's CSE table), or a new Const node.
  uint32_t literal(const Constant& c, uint64_t key, const NodeOrigin& origin) {
    const uint64_t h = constHash(c);
    if (const uint32_t m = cse_.find(h); m != kNone) {
      if (nodes_[m].alive && !nodes_[m].replaced && nodes_[m].kind == NodeKind::Const && sameConst(consts_[nodes_[m].payload], c))
        return out(m);
    }
    const uint32_t n = addNode(NodeKind::Const, 0, {}, 1, portType(c.kind, Rate::Const, Poly::Global, kWidthMono),
                               key, origin);
    nodes_[n].payload = static_cast<uint32_t>(consts_.size());
    consts_.push_back(c);
    facts_[out(n)] = constFacts(c);
    cse_.findOrInsert(h, n);
    return out(n);
  }

  // Forwards out-port o to t. Refused if that would change the value's kind, width or poly (pack concatenates
  // widths, and voice reducers treat a global value differently from N equal voices), raise its rate, or make a
  // value its own replacement.
  bool replace(uint32_t o, uint32_t t) {
    t = resolve(t);
    if (t == kNone || t == o) return false;
    const PortType& a = outT_[o];
    const PortType& b = outT_[t];
    if (a.kind != b.kind || a.width != b.width || a.poly != b.poly || b.rate > a.rate) return false;
    fwd_[o] = t;
    // Keep the name (debugging, tests) if the surviving value has none.
    const uint32_t from = owner(o), to = owner(t);
    const uint16_t named = names_[from].output;  // the output the name refers to (kWholeNode: the node = output 0)
    if (!names_[from].name.empty() && names_[to].name.empty() &&
        (nodes_[to].kind == NodeKind::Atom || nodes_[to].kind == NodeKind::Const) &&
        o == out(from, named == kWholeNode ? 0u : named)) {
      names_[to].name = std::move(names_[from].name);
      names_[to].output = nodes_[to].nOut == 1 ? kWholeNode : static_cast<uint16_t>(t - nodes_[to].firstOut);
      names_[from].name.clear();
    }
    return true;
  }

  // Replaces the single output of n; marks n replaced on success.
  bool replaceNode(uint32_t n, uint32_t t) {
    if (!replace(out(n), t)) return false;
    nodes_[n].replaced = true;
    return true;
  }

  // Turns n into another atom on (a prefix of) its input slots. The output keeps its type (same kind).
  void rewrite(uint32_t n, AtomId atom, std::initializer_list<uint32_t> drivers) {
    WNode& w = nodes_[n];
    w.atom = atom;
    uint32_t i = 0;
    for (uint32_t d : drivers) drv_[w.firstIn + i++] = d;
    w.nIn = static_cast<uint16_t>(i);
  }

  // ---- literals and facts ---------------------------------------------------------------------------------------

  static bool sameConst(const Constant& a, const Constant& b) noexcept {
    return a.kind == b.kind && std::bit_cast<uint64_t>(a.value()) == std::bit_cast<uint64_t>(b.value());
  }
  static uint64_t constHash(const Constant& c) noexcept {
    return hashMix(hashMix(0xC0C0C0ull, static_cast<uint64_t>(c.kind)), std::bit_cast<uint64_t>(c.value()));
  }
  static Facts constFacts(const Constant& c) noexcept {
    if (c.kind == Kind::Bool) return {};
    const auto f = static_cast<float>(c.value());
    return {std::isfinite(f), !std::signbit(f)};
  }

  // The literal driving input i of n (a Const node, or the default of an unconnected optional input), as the
  // float the kernel would see. False if the input is computed.
  bool lit(uint32_t n, uint32_t i, float& v) {
    const uint32_t d = in(n, i);
    if (d == kNone) {
      const WNode& w = nodes_[n];
      if (w.kind != NodeKind::Atom) return false;
      const AtomSignature& sig = sigOf(w.atom);
      const PortSpec& ps = sig.inputs[specIndex(sig, i)];
      if (!ps.optional) return false;
      v = ps.type.kind == Kind::Bool ? dsp::boolMask(ps.defaultValue != 0.0f) : ps.defaultValue;
      return true;
    }
    const WNode& m = nodes_[owner(d)];
    if (m.kind != NodeKind::Const) return false;
    const Constant& c = consts_[m.payload];
    v = c.kind == Kind::Bool ? dsp::boolMask(c.literal != 0.0) : static_cast<float>(c.value());
    return true;
  }
  bool litIs(uint32_t n, uint32_t i, float x) {
    float v;
    return lit(n, i, v) && std::bit_cast<uint32_t>(v) == std::bit_cast<uint32_t>(x);
  }
  bool litBool(uint32_t n, uint32_t i, bool& b) {
    float v;
    if (!lit(n, i, v)) return false;
    b = dsp::maskIsTrue(v);
    return true;
  }
  Facts factsOf(uint32_t d) const { return d == kNone ? Facts{} : facts_[d]; }
  // Facts of input i, including the literal default of an unconnected input.
  Facts inFacts(uint32_t n, uint32_t i) {
    const uint32_t d = in(n, i);
    if (d != kNone) return facts_[d];
    float v;
    if (!lit(n, i, v)) return {};
    return {std::isfinite(v), !std::signbit(v)};
  }

  Facts atomFacts(uint32_t n) {
    const A& A_ = a_;
    const WNode& w = nodes_[n];
    const AtomId at = w.atom;
    auto f = [&](uint32_t i) { return inFacts(n, i); };
    if (at == A_.phasor || at == A_.gateToReal) return {true, true};
    if (at == A_.noise || at == A_.sineHz || at == A_.sign || at == A_.counter) return {true, false};
    // fast_sin is not here: its result is only specified for |x| ≤ 65536 (the dsp atom clamps it to [−1, 1],
    // but the fact list promises only what the atoms' contracts promise; M1 review M5).
    if (at == A_.sine || at == A_.sin || at == A_.tanh || at == A_.fastTanh || at == A_.floor)
      return {f(0).finite, false};
    if (at == A_.frac) return {f(0).finite, f(0).finite};
    if (at == A_.abs) return {f(0).finite, true};
    if (at == A_.exp || at == A_.fastExp) return {false, true};
    if (at == A_.sqrt || at == A_.fastSqrt) return {false, f(0).signClear};
    if (at == A_.clip) {
      const Facts lo = f(1), hi = f(2);
      return {lo.finite && hi.finite, lo.finite && hi.finite && lo.signClear && hi.signClear};
    }
    if (at == A_.min || at == A_.max) return {f(0).finite && f(1).finite, f(0).signClear && f(1).signClear};
    if (at == A_.add || at == A_.mul || at == A_.div) return {false, f(0).signClear && f(1).signClear};
    if (at == A_.muladd) return {false, f(0).signClear && f(1).signClear && f(2).signClear};
    if (at == A_.select) return {f(1).finite && f(2).finite, f(1).signClear && f(2).signClear};
    if (at == A_.mux || at == A_.pack) {
      Facts r{true, true};
      for (uint32_t i = at == A_.mux ? 1u : 0u; i < w.nIn; ++i) {
        const Facts x = f(i);
        r.finite &= x.finite;
        r.signClear &= x.signClear;
      }
      return r;
    }
    if (at == A_.channel) return f(0);
    const AtomSignature& sig = sigOf(at);
    if (isVoiceReducer(sig) && at != A_.voiceSum) return f(0);  // one voice's value (a sum may overflow)
    return {};
  }

  // ---- pass 0: durations ------------------------------------------------------------------------------------------

  uint32_t srPort(const NodeOrigin& origin) {
    if (srPort_ == kNone) {
      const uint32_t n = addNode(NodeKind::Atom, a_.sr, {}, 1, portType(Kind::Real, Rate::Const, Poly::Global, kWidthMono),
                                 hashMix(0x5352ull, 0), origin);
      srPort_ = out(n);
    }
    return srPort_;
  }

  void lowerDurations() {
    const auto n0 = static_cast<uint32_t>(nodes_.size());
    for (uint32_t n = 0; n < n0; ++n) {
      if (nodes_[n].kind != NodeKind::Atom) continue;
      const KernelMapping& km = kernelMapping(nodes_[n].atom);
      if (km.lowering != Lowering::Kernel || km.args.empty()) continue;
      for (uint32_t i = 0; i < nodes_[n].nIn; ++i) {
        const ArgUse use = km.args[std::min<size_t>(i, km.args.size() - 1)].use;
        if (use != ArgUse::InputSeconds && use != ArgUse::InitMaxDelaySeconds) continue;
        const uint32_t p = nodes_[n].firstIn + i;
        uint32_t d = resolve(drv_[p]);
        const bool smp = d != kNone && nodes_[owner(d)].kind == NodeKind::Const &&
                         consts_[nodes_[owner(d)].payload].unit == Unit::smp;
        if (use == ArgUse::InitMaxDelaySeconds) {
          // Builder computes ceil(max · sr): give it seconds. Unknown sample rate: left in samples (see header).
          if (smp && opt_.sampleRate > 0.0) {
            const double frames = consts_[nodes_[owner(d)].payload].value();
            drv_[p] = literal(Constant::real(frames / opt_.sampleRate, Unit::s), hashMix(nodes_[n].key, 0xD1A0 + i),
                              nodes_[n].origin);
            ++st_.durations;
          }
          continue;
        }
        if (smp) {  // already samples
          ++st_.durations;
          continue;
        }
        if (d == kNone) {
          const AtomSignature& sig = sigOf(nodes_[n].atom);
          const PortSpec& ps = sig.inputs[specIndex(sig, i)];
          if (ps.defaultValue == 0.0f) continue;  // 0 s = 0 samples
          d = literal(Constant::real(ps.defaultValue), hashMix(nodes_[n].key, 0xDEF0 + i), nodes_[n].origin);
        }
        const PortType& dt = outT_[d];
        const PortType t = portType(Kind::Real, maxRate(dt.rate, Rate::Const), dt.poly, dt.width);
        const uint32_t ins[2] = {d, srPort(nodes_[n].origin)};
        const uint32_t m = addNode(NodeKind::Atom, a_.mul, ins, 1, t, hashMix(nodes_[n].key, 0xD0D0 + i), nodes_[n].origin);
        drv_[p] = out(m);
        ++st_.durations;
      }
    }
    tp_.durationsInSamples = true;
  }

  // ---- order --------------------------------------------------------------------------------------------------------

  // Topological order of the alive nodes (drivers first; edges into delayed inputs, e.g. z1.x, are feedback and
  // ignored). Nodes on or downstream of any other cycle are appended at the end with cyclic_ set.
  void computeOrder() {
    const auto n = static_cast<uint32_t>(nodes_.size());
    order_.clear();
    order_.reserve(n);
    cyclic_.assign(n, 0);
    indeg_.assign(n, 0);
    consBegin_.assign(n + 1, 0);
    auto counted = [&](uint32_t v, uint32_t i) -> uint32_t {
      const uint32_t d = in(v, i);
      if (d == kNone || !nodes_[owner(d)].alive) return kNone;
      if (nodes_[v].kind == NodeKind::Atom) {
        const AtomSignature& sig = sigOf(nodes_[v].atom);
        if (!sig.inputs.empty() && sig.inputs[specIndex(sig, i)].delayed) return kNone;
      }
      return owner(d);
    };
    for (uint32_t v = 0; v < n; ++v) {
      if (!nodes_[v].alive) continue;
      for (uint32_t i = 0; i < nodes_[v].nIn; ++i)
        if (const uint32_t u = counted(v, i); u != kNone) {
          ++indeg_[v];
          ++consBegin_[u + 1];
        }
    }
    for (uint32_t v = 0; v < n; ++v) consBegin_[v + 1] += consBegin_[v];
    cons_.resize(consBegin_[n]);
    fill_.assign(consBegin_.begin(), consBegin_.end() - 1);
    for (uint32_t v = 0; v < n; ++v) {
      if (!nodes_[v].alive) continue;
      for (uint32_t i = 0; i < nodes_[v].nIn; ++i)
        if (const uint32_t u = counted(v, i); u != kNone) cons_[fill_[u]++] = v;
    }
    for (uint32_t v = 0; v < n; ++v)
      if (nodes_[v].alive && indeg_[v] == 0) order_.push_back(v);
    for (size_t h = 0; h < order_.size(); ++h) {
      const uint32_t v = order_[h];
      for (uint32_t c = consBegin_[v]; c < consBegin_[v + 1]; ++c)
        if (--indeg_[cons_[c]] == 0) order_.push_back(cons_[c]);
    }
    if (order_.size() < n) {
      for (uint32_t v = 0; v < n; ++v)
        if (nodes_[v].alive && indeg_[v] != 0) {
          cyclic_[v] = 1;
          order_.push_back(v);
        }
    }
  }

  // ---- pass 1: sweep ------------------------------------------------------------------------------------------------

  uint32_t sweep() {
    computeOrder();
    facts_.assign(outT_.size(), Facts{});
    cse_.reset(nodes_.size());
    const uint32_t before = st_.folded + st_.simplified + st_.merged;
    for (size_t k = 0; k < order_.size(); ++k) {
      const uint32_t n = order_[k];
      if (!nodes_[n].alive || nodes_[n].replaced) continue;
      switch (nodes_[n].kind) {
        case NodeKind::Const: {
          const Constant& c = consts_[nodes_[n].payload];
          facts_[out(n)] = constFacts(c);
          const uint64_t h = constHash(c);
          if (const uint32_t m = cse_.findOrInsert(h, n); m != kNone) {
            if (nodes_[m].kind == NodeKind::Const && sameConst(consts_[nodes_[m].payload], c) && replaceNode(n, out(m)))
              ++st_.merged;
          }
          break;
        }
        case NodeKind::Atom: visitAtom(n); break;
        default: break;
      }
    }
    return st_.folded + st_.simplified + st_.merged - before;
  }

  void visitAtom(uint32_t n) {
    const AtomSignature& sig = sigOf(nodes_[n].atom);
    for (uint32_t i = 0; i < nodes_[n].nIn; ++i) in(n, i);  // resolve
    if (!isPure(sig)) {
      if (!cyclic_[n] && nodes_[n].nOut > 0) facts_[out(n)] = atomFacts(n);
      return;
    }
    if (fold(n)) return;
    if (simplify(n)) return;
    if (nodes_[n].nOut > 0 && !cyclic_[n]) facts_[out(n)] = atomFacts(n);
    cse(n);
  }

  bool fold(uint32_t n) {
    const WNode& w = nodes_[n];
    if (w.atom == a_.sr) {
      if (opt_.sampleRate <= 0.0) return false;
      if (!replaceNode(n, literal(Constant::real(opt_.sampleRate), hashMix(w.key, 0xF01D), w.origin))) return false;
      ++st_.folded;
      return true;
    }
    if (w.nIn > kMaxFoldPorts || w.nOut > kMaxFoldPorts || w.nOut == 0) return false;
    float ins[kMaxFoldPorts], outs[kMaxFoldPorts];
    for (uint32_t i = 0; i < w.nIn; ++i)  // first: most nodes have a computed input and stop here
      if (!lit(n, i, ins[i])) return false;
    const KernelMapping& km = kernelMapping(w.atom);
    if (km.lowering != Lowering::Kernel) return false;
    const dsp::KernelInfo& ki = dsp::kernelInfo(km.kernel);
    if (ki.resource != dsp::Resource::None || ki.splRequired != 0 || ki.stateSize != 0) return false;
    for (const ArgMap& am : km.args)
      if (am.use != ArgUse::Input) return false;
    if (!evalKernel(km.kernel, ins, w.nIn, outs, w.nOut)) return false;
    // Literals are finite (Patch::validate); an inf/NaN result stays a computation (same value at runtime).
    for (uint32_t j = 0; j < w.nOut; ++j)
      if (outT_[out(n, j)].kind == Kind::Real && !std::isfinite(outs[j])) return false;
    bool all = true;
    for (uint32_t j = 0; j < nodes_[n].nOut; ++j) {
      const uint32_t o = out(n, j);
      const Constant c = outT_[o].kind == Kind::Bool ? Constant::boolean(dsp::maskIsTrue(outs[j]))
                                                     : Constant::real(static_cast<double>(outs[j]));
      const uint32_t lp = literal(c, hashMix(nodes_[n].key, 0xF01D + j), nodes_[n].origin);
      all &= replace(o, lp);
    }
    if (!all) return false;  // partially folded (refused by a type guard): the node stays
    nodes_[n].replaced = true;
    ++st_.folded;
    return true;
  }

  bool simplified(bool done) {
    if (done) ++st_.simplified;
    return done;
  }

  // True if n was replaced; a rewrite in place returns false (the node then goes on to CSE as its new self).
  bool simplify(uint32_t n) {
    const A& A_ = a_;
    const AtomId at = nodes_[n].atom;
    const uint32_t a = nodes_[n].nIn > 0 ? in(n, 0) : kNone;
    const uint32_t b = nodes_[n].nIn > 1 ? in(n, 1) : kNone;
    const auto ownerAtom = [&](uint32_t d, AtomId want) {
      return d != kNone && nodes_[owner(d)].kind == NodeKind::Atom && nodes_[owner(d)].atom == want &&
             !nodes_[owner(d)].replaced;
    };
    const float kPosZero = 0.0f, kNegZero = -0.0f;

    if (at == A_.add) {
      for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t x = k == 0 ? a : b;
        if (x == kNone) continue;
        if (litIs(n, 1 - k, kNegZero) || (litIs(n, 1 - k, kPosZero) && factsOf(x).signClear))
          if (replaceNode(n, x)) return simplified(true);
      }
      return false;
    }
    if (at == A_.sub) {
      if (a != kNone && (litIs(n, 1, kPosZero) || (litIs(n, 1, kNegZero) && factsOf(a).signClear)))
        return simplified(replaceNode(n, a));
      return false;
    }
    if (at == A_.mul) {
      for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t x = k == 0 ? a : b;
        if (x == kNone) continue;
        if (litIs(n, 1 - k, 1.0f) && replaceNode(n, x)) return simplified(true);
        if (litIs(n, 1 - k, -1.0f)) {
          rewrite(n, A_.neg, {x});
          ++st_.simplified;
          return simplify(n);
        }
        float z;
        if (lit(n, 1 - k, z) && z == 0.0f && factsOf(x).finite && factsOf(x).signClear) {
          const uint32_t zp = in(n, 1 - k);
          const uint32_t lp = zp != kNone ? zp : literal(Constant::real(z), hashMix(nodes_[n].key, 0x2E50), nodes_[n].origin);
          if (replaceNode(n, lp)) return simplified(true);
        }
      }
      return false;
    }
    if (at == A_.div) {
      float c;
      if (a == kNone || !lit(n, 1, c)) return false;
      if (c == 1.0f) return simplified(replaceNode(n, a));
      if (c == -1.0f) {
        rewrite(n, A_.neg, {a});
        ++st_.simplified;
        return simplify(n);
      }
      int e = 0;
      const float m = std::frexp(c, &e);
      const float r = 1.0f / c;
      if ((m == 0.5f || m == -0.5f) && std::isnormal(c) && std::isnormal(r)) {
        rewrite(n, A_.mul, {a, literal(Constant::real(r), hashMix(nodes_[n].key, 0x1DC0), nodes_[n].origin)});
        ++st_.simplified;
        return false;
      }
      return false;
    }
    if (at == A_.pow) {
      if (a == kNone) return false;
      if (litIs(n, 1, 1.0f)) return simplified(replaceNode(n, a));
      if (litIs(n, 1, 2.0f)) {  // afn
        rewrite(n, A_.mul, {a, a});
        ++st_.simplified;
        return false;
      }
      return false;
    }
    if (at == A_.neg || at == A_.not_) {
      if (ownerAtom(a, at)) return simplified(replaceNode(n, in(owner(a), 0)));
      return false;
    }
    if (at == A_.abs) {
      if (ownerAtom(a, A_.abs)) return simplified(replaceNode(n, a));
      if (ownerAtom(a, A_.neg) && in(owner(a), 0) != kNone) {
        rewrite(n, A_.abs, {in(owner(a), 0)});
        ++st_.simplified;
        return simplify(n);
      }
      return false;
    }
    if (at == A_.min || at == A_.max) {
      if (a != kNone && a == b) return simplified(replaceNode(n, a));
      return false;
    }
    if (at == A_.and_ || at == A_.or_ || at == A_.xor_) {
      if (a != kNone && a == b) {
        if (at == A_.xor_)
          return simplified(replaceNode(n, literal(Constant::boolean(false), hashMix(nodes_[n].key, 0xB00), nodes_[n].origin)));
        return simplified(replaceNode(n, a));
      }
      for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t x = k == 0 ? a : b;
        bool c;
        if (x == kNone || !litBool(n, 1 - k, c)) continue;
        const bool identity = at == A_.and_ ? c : !c;  // x && true, x || false, x ^^ false
        if (identity) return simplified(replaceNode(n, x));
        if (at == A_.xor_) {  // x ^^ true
          rewrite(n, A_.not_, {x});
          ++st_.simplified;
          return simplify(n);
        }
        // x && false → false, x || true → true
        if (replaceNode(n, literal(Constant::boolean(c), hashMix(nodes_[n].key, 0xB01), nodes_[n].origin)))
          return simplified(true);
      }
      return false;
    }
    if (at == A_.select) {
      const uint32_t x = in(n, 1), y = in(n, 2);
      bool c;
      if (litBool(n, 0, c)) {
        const uint32_t pick = c ? x : y;
        return pick != kNone && simplified(replaceNode(n, pick));
      }
      if (x != kNone && x == y) return simplified(replaceNode(n, x));
      return false;
    }
    if (at == A_.mux) {
      float idx;
      if (!lit(n, 0, idx)) return false;
      const uint32_t count = nodes_[n].nIn - 1u;
      uint32_t k = 0;
      if (!std::isnan(idx)) {
        const float f = std::floor(idx);
        k = f <= 0.0f ? 0u : f >= static_cast<float>(count - 1) ? count - 1 : static_cast<uint32_t>(f);
      }
      const uint32_t pick = in(n, 1 + k);
      return pick != kNone && simplified(replaceNode(n, pick));
    }
    if (at == A_.channel) {
      if (a == kNone) return false;
      if (outT_[a].width == kWidthMono) return simplified(replaceNode(n, a));
      float idx;
      if (!ownerAtom(a, A_.pack) || !lit(n, 1, idx) || !(idx >= 0.0f) || idx != std::floor(idx)) return false;
      const uint32_t pk = owner(a);
      uint32_t base = 0;
      for (uint32_t i = 0; i < nodes_[pk].nIn; ++i) {
        const uint32_t x = in(pk, i);
        const uint32_t w = x == kNone ? 1u : outT_[x].width;
        if (static_cast<float>(base) == idx && w == 1 && x != kNone) return simplified(replaceNode(n, x));
        base += w;
        if (static_cast<float>(base) > idx) break;
      }
      return false;
    }
    const AtomSignature& sig = sigOf(at);
    if (isVoiceReducer(sig) && a != kNone && outT_[a].poly == Poly::Global) return simplified(replaceNode(n, a));
    return false;
  }

  void cse(uint32_t n) {
    const WNode& w = nodes_[n];
    if (w.nOut == 0) return;
    const AtomSignature& sig = sigOf(w.atom);
    uint32_t d0 = w.nIn > 0 ? in(n, 0) : kNone, d1 = w.nIn > 1 ? in(n, 1) : kNone;
    const bool swap = sig.has(kCommutative) && w.atom != a_.min && w.atom != a_.max;
    if (swap && d1 < d0) std::swap(d0, d1);
    uint64_t h = hashMix(hashMix(0xA70Aull, w.atom), (static_cast<uint64_t>(w.nIn) << 16) | w.nOut);
    h = hashMix(h, d0);
    h = hashMix(h, d1);
    for (uint32_t i = 2; i < w.nIn; ++i) h = hashMix(h, in(n, i));
    const uint32_t m = cse_.findOrInsert(h, n);
    if (m == kNone) return;
    const WNode& v = nodes_[m];
    if (!v.alive || v.replaced || v.kind != NodeKind::Atom || v.atom != w.atom || v.nIn != w.nIn || v.nOut != w.nOut)
      return;
    uint32_t e0 = v.nIn > 0 ? in(m, 0) : kNone, e1 = v.nIn > 1 ? in(m, 1) : kNone;
    if (swap && e1 < e0) std::swap(e0, e1);
    if (e0 != d0 || e1 != d1) return;
    for (uint32_t i = 2; i < w.nIn; ++i)
      if (in(m, i) != in(n, i)) return;
    for (uint32_t j = 0; j < w.nOut; ++j)
      if (!replace(out(n, j), out(m, j))) return;
    nodes_[n].replaced = true;
    ++st_.merged;
  }

  // ---- DCE ----------------------------------------------------------------------------------------------------------

  void eliminateDead() {
    const auto n = static_cast<uint32_t>(nodes_.size());
    live_.assign(n, 0);
    stack_.clear();
    std::vector<uint8_t> solveLive(solveMembers_.size(), 0);
    auto mark = [&](uint32_t v) {
      if (live_[v] || !nodes_[v].alive) return;
      live_[v] = 1;
      stack_.push_back(v);
    };
    for (uint32_t v = 0; v < n; ++v) {
      const WNode& w = nodes_[v];
      if (!w.alive) continue;
      const bool root = w.kind == NodeKind::Output || w.kind == NodeKind::Export || w.kind == NodeKind::Param ||
                        w.kind == NodeKind::Input ||
                        (w.kind == NodeKind::Atom && sigOf(w.atom).has(kSideEffect));
      if (root) mark(v);
    }
    while (!stack_.empty()) {
      const uint32_t v = stack_.back();
      stack_.pop_back();
      const WNode& w = nodes_[v];
      if ((w.kind == NodeKind::Unknown || w.kind == NodeKind::Equation) && w.payload < solveLive.size() &&
          !solveLive[w.payload]) {
        solveLive[w.payload] = 1;
        for (uint32_t m : solveMembers_[w.payload]) mark(m);
      }
      for (uint32_t i = 0; i < w.nIn; ++i)
        if (const uint32_t d = in(v, i); d != kNone) mark(owner(d));
    }
    for (uint32_t v = 0; v < n; ++v)
      if (nodes_[v].alive && !live_[v]) {
        nodes_[v].alive = false;
        if (!nodes_[v].replaced) ++st_.removed;
      }
  }

  // ---- pass 2: rate refinement ------------------------------------------------------------------------------------

  void refineRates() {
    computeOrder();
    for (const uint32_t n : order_) {
      const WNode& w = nodes_[n];
      if (cyclic_[n] || w.kind != NodeKind::Atom || w.nIn == 0) continue;
      const AtomSignature& sig = sigOf(w.atom);
      if (!isPure(sig)) continue;
      Rate r = Rate::Const;
      for (uint32_t i = 0; i < w.nIn; ++i)
        if (const uint32_t d = in(n, i); d != kNone) r = maxRate(r, outT_[d].rate);
      for (uint32_t j = 0; j < w.nOut; ++j) {
        const Rate declared = sig.outputs[std::min<size_t>(j, sig.outputs.size() - 1)].type.rate;
        const Rate rj = declared == Rate::Infer ? r : maxRate(r, declared);
        PortType& t = outT_[out(n, j)];
        if (rj < t.rate) {
          t.rate = rj;
          ++st_.ratesLowered;
        }
      }
    }
  }

  // ---- pass 3: fusion -----------------------------------------------------------------------------------------------

  uint32_t fuse() {
    const auto n0 = static_cast<uint32_t>(nodes_.size());
    std::vector<uint32_t> uses(outT_.size(), 0);
    for (uint32_t v = 0; v < n0; ++v)
      if (nodes_[v].alive)
        for (uint32_t i = 0; i < nodes_[v].nIn; ++i)
          if (const uint32_t d = in(v, i); d != kNone) ++uses[d];
    uint32_t fused = 0;
    for (uint32_t v = 0; v < n0; ++v) {
      if (!nodes_[v].alive || nodes_[v].replaced || nodes_[v].kind != NodeKind::Atom || nodes_[v].atom != a_.add)
        continue;
      const uint32_t o = out(v);
      for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t m = in(v, k), c = in(v, 1 - k);
        if (m == kNone || c == kNone || uses[m] != 1) continue;
        const uint32_t mn = owner(m);
        if (nodes_[mn].kind != NodeKind::Atom || nodes_[mn].atom != a_.mul || nodes_[mn].replaced) continue;
        if (!(outT_[m] == outT_[o])) continue;  // a hoisted (lower-rate) product stays a separate op
        const uint32_t x = in(mn, 0), y = in(mn, 1);
        if (x == kNone || y == kNone) continue;
        const uint32_t ins[3] = {x, y, c};
        const uint32_t f = addNode(NodeKind::Atom, a_.muladd, ins, 1, outT_[o], nodes_[v].key, nodes_[v].origin);
        if (!replaceNode(v, out(f))) {
          nodes_[f].alive = false;
          continue;
        }
        uses.resize(outT_.size(), 0);
        ++fused;
        ++st_.fused;
        break;
      }
    }
    return fused;
  }

  // ---- checks -------------------------------------------------------------------------------------------------------

  Status checkChannels() {
    for (uint32_t n = 0; n < nodes_.size(); ++n) {
      const WNode& w = nodes_[n];
      if (!w.alive || w.replaced || w.kind != NodeKind::Atom || w.atom != a_.channel || w.nIn < 2) continue;
      const uint32_t x = in(n, 0), i = in(n, 1);
      if (x == kNone || i == kNone || outT_[x].width <= kWidthMono || nodes_[owner(i)].kind != NodeKind::Const) continue;
      const double v = consts_[nodes_[owner(i)].payload].value();
      const uint16_t width = outT_[x].width;
      if (!(v >= 0.0 && v < width && v == std::floor(v))) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", v);
        const std::string what = w.src != kNone ? p_.describe(w.src) : std::string("a channel select");
        return Error{ErrorCode::Type, "patch '" + p_.name() + "': channel index " + buf + " is not a channel of a " +
                                          std::to_string(width) + "-channel signal: " + what,
                     {}};
      }
    }
    return Ok{};
  }

  // ---- emit ---------------------------------------------------------------------------------------------------------

  TypedPatch emit() {
    TypedPatch r;
    Patch& q = r.patch;
    q.setName(p_.name());
    std::vector<uint32_t> solveMap(p_.solveCount(), kNone);
    for (uint32_t n = 0; n < nodes_.size(); ++n) {
      const WNode& w = nodes_[n];
      if (w.alive && (w.kind == NodeKind::Unknown || w.kind == NodeKind::Equation) && w.payload < solveMap.size() &&
          solveMap[w.payload] == kNone)
        solveMap[w.payload] = 0;
    }
    for (uint32_t s = 0; s < solveMap.size(); ++s)
      if (solveMap[s] != kNone) solveMap[s] = q.addSolve(p_.solves()[s].iterations);

    const auto n = static_cast<uint32_t>(nodes_.size());
    std::vector<NodeIndex> newIdx(n, kNone);
    uint32_t alive = 0;
    for (uint32_t v = 0; v < n; ++v) alive += nodes_[v].alive;
    r.stateKey.reserve(alive);
    r.origin.reserve(alive);
    for (uint32_t v = 0; v < n; ++v) {
      const WNode& w = nodes_[v];
      if (!w.alive) continue;
      NodeIndex m = kNone;
      switch (w.kind) {
        case NodeKind::Atom: {
          const AtomSignature& sig = sigOf(w.atom);
          uint16_t repeat = 0;
          if (sig.has(kVariadicInputs)) repeat = static_cast<uint16_t>(w.nIn - sig.fixedInputs());
          if (sig.has(kVariadicOutputs)) repeat = static_cast<uint16_t>(w.nOut - sig.fixedOutputs());
          m = q.addAtom(w.atom, repeat);
          q.setNodeName(m, std::move(names_[v].name), names_[v].output);
          break;
        }
        case NodeKind::Const:
          m = q.addConst(consts_[w.payload]);
          q.setNodeName(m, std::move(names_[v].name), names_[v].output);
          break;
        case NodeKind::Param: m = q.addParam(names_[v].name, p_.paramDecl(w.src)); break;
        case NodeKind::Input: m = q.addInput(names_[v].name, p_.outputType(w.src, 0)); break;
        case NodeKind::Output: m = q.addOutput(names_[v].name, p_.inputType(w.src, 0)); break;
        case NodeKind::Export: m = q.addExport(names_[v].name); break;
        case NodeKind::Unknown: m = q.addUnknown(names_[v].name, solveMap[w.payload]); break;
        case NodeKind::Equation: m = q.addEquation(solveMap[w.payload]); break;
        case NodeKind::Subcircuit: continue;  // never in a TypedPatch
      }
      if (w.src != kNone) q.setLayout(m, p_.layout(w.src));
      newIdx[v] = m;
      r.stateKey.push_back(w.key);
      r.origin.push_back(w.origin);
    }
    for (uint32_t v = 0; v < n; ++v) {
      const WNode& w = nodes_[v];
      if (!w.alive) continue;
      for (uint32_t i = 0; i < w.nIn; ++i) {
        const uint32_t d = in(v, i);
        if (d == kNone) continue;
        const uint32_t u = owner(d);
        const WireIndex wi = q.connect(PortRef{newIdx[u], static_cast<uint16_t>(d - nodes_[u].firstOut)},
                                       PortRef{newIdx[v], static_cast<uint16_t>(i)});
        const uint32_t p = w.firstIn + i;
        if (origWire_[p] != kNone && origDrv_[p] == d && !p_.wireVia(origWire_[p]).empty())
          q.setWireVia(wi, std::vector<Point2>(p_.wireVia(origWire_[p]).begin(), p_.wireVia(origWire_[p]).end()));
      }
    }
    for (const LayoutComment& c : p_.comments()) q.addComment(c);

    r.types.assign(q.portCount(), PortType{});
    for (uint32_t v = 0; v < n; ++v) {
      const WNode& w = nodes_[v];
      if (!w.alive) continue;
      const NodeIndex m = newIdx[v];
      for (uint32_t j = 0; j < w.nOut; ++j) r.types[q.flatOutput(m, j)] = outT_[out(v, j)];
      for (uint32_t i = 0; i < w.nIn; ++i) {
        const uint32_t d = in(v, i);
        r.types[q.flatInput(m, i)] =
            d != kNone ? outT_[d]
                       : portType(q.inputType(m, i).kind == Kind::Bool ? Kind::Bool : Kind::Real, Rate::Const,
                                  Poly::Global, kWidthMono);
      }
    }
    r.instances = std::move(tp_.instances);
    r.durationsInSamples = tp_.durationsInSamples;
    return r;
  }

  // ---- state --------------------------------------------------------------------------------------------------------

  using A = Atoms;
  struct Name {
    std::string name;
    uint16_t output = kWholeNode;
  };

  TypedPatch& tp_;
  const Patch& p_;
  const OptimiseOptions& opt_;
  OptimiseStats& st_;
  const Atoms& a_ = atoms();
  const AtomRegistry& reg_ = AtomRegistry::builtin();

  std::vector<WNode> nodes_;
  std::vector<Name> names_;
  std::vector<Constant> consts_;
  std::vector<uint32_t> drv_;       // per in-port: driving out-port (possibly forwarded) or kNone
  std::vector<uint32_t> origDrv_;   // per in-port: the driver in the input patch (wire routing is kept for those)
  std::vector<WireIndex> origWire_; // per in-port: its wire in the input patch
  std::vector<PortType> outT_;      // per out-port
  std::vector<uint32_t> outOwner_;  // per out-port
  std::vector<uint32_t> fwd_;       // per out-port: replacement or kNone
  std::vector<Facts> facts_;        // per out-port, valid during a sweep
  std::vector<std::vector<uint32_t>> solveMembers_;
  uint32_t srPort_ = kNone;

  std::vector<uint32_t> order_, indeg_, consBegin_, cons_, fill_, stack_;
  std::vector<uint8_t> cyclic_, live_;
  HashTable cse_;
};

}  // namespace

Result<TypedPatch> optimise(TypedPatch patch, const OptimiseOptions& options, OptimiseStats* stats) {
  OptimiseStats local;
  OptimiseStats& st = stats ? *stats : local;
  st = OptimiseStats{};
  return Optimiser(patch, options, st).run();
}

}  // namespace pq::graph
