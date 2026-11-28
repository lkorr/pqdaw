// FD.1: interval analysis of a TypedPatch's values (pq/graph/value_range.h has the contract and the rules).

#include "pq/graph/value_range.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "pq/graph/atoms.h"
#include "pq/graph/kernel_map.h"

namespace pq::graph {

namespace {

using R = ValueRange;
constexpr float kInf = std::numeric_limits<float>::infinity();
// Approximated sources of nominal range [−1, 1] (value_range.h): fast_sin is within 1.5e−7 (pq/core/fast_math.h).
constexpr float kUnit = 1.000001f;
constexpr uint32_t kMaxDepth = 200;      // expression depth past which a value is unknown (recursion bound)
constexpr uint32_t kMaxAliasHops = 1024;

// A NaN endpoint (inf − inf) is that side unknown; a denormal endpoint moves out to zero's side (FTZ / DAZ).
R norm(R r) noexcept {
  if (std::isnan(r.lo)) r.lo = -kInf;
  if (std::isnan(r.hi)) r.hi = kInf;
  if (r.lo > 0.0f && r.lo < FLT_MIN) r.lo = 0.0f;
  if (r.hi < 0.0f && r.hi > -FLT_MIN) r.hi = 0.0f;
  if (r.lo > r.hi) return {};
  return r;
}
R point(float v) noexcept { return std::isnan(v) ? R{} : norm({v, v}); }
R hull(R a, R b) noexcept { return {std::min(a.lo, b.lo), std::max(a.hi, b.hi)}; }
R negR(R a) noexcept { return {-a.hi, -a.lo}; }
bool finite(R a) noexcept { return std::isfinite(a.lo) && std::isfinite(a.hi); }

// Products at the corners. 0 · x is 0 for every finite x, so a zero endpoint against an infinite one is 0.
float mulc(float a, float b) noexcept { return a == 0.0f || b == 0.0f ? 0.0f : a * b; }
float fmac(float x, float y, float c) noexcept { return x == 0.0f || y == 0.0f ? c : std::fma(x, y, c); }

R mulR(R a, R b) noexcept {
  const float p[4] = {mulc(a.lo, b.lo), mulc(a.lo, b.hi), mulc(a.hi, b.lo), mulc(a.hi, b.hi)};
  return norm({std::min({p[0], p[1], p[2], p[3]}), std::max({p[0], p[1], p[2], p[3]})});
}

// fma(x, y, c): x · y + c with one rounding. Monotonic in c; for a fixed c its extremes over a box of (x, y) are at
// the corners (the exact value is bilinear, the rounding monotonic).
R fmaR(R x, R y, R c) noexcept {
  const float xs[2] = {x.lo, x.hi}, ys[2] = {y.lo, y.hi};
  float lo = kInf, hi = -kInf;
  for (float xc : xs)
    for (float yc : ys) {
      const float l = fmac(xc, yc, c.lo), h = fmac(xc, yc, c.hi);
      lo = std::isnan(l) ? -kInf : std::min(lo, l);
      hi = std::isnan(h) ? kInf : std::max(hi, h);
    }
  return norm({lo, hi});
}

R divR(R a, R b) noexcept {
  if (!(b.lo > 0.0f || b.hi < 0.0f)) return {};  // the divisor may be 0
  const float as[2] = {a.lo, a.hi}, bs[2] = {b.lo, b.hi};
  float lo = kInf, hi = -kInf;
  for (float ac : as)
    for (float bc : bs) {
      const float q = ac == 0.0f ? 0.0f : ac / bc;
      if (std::isnan(q)) return {};  // inf / inf
      lo = std::min(lo, q);
      hi = std::max(hi, q);
    }
  return norm({lo, hi});
}

R absR(R a) noexcept {
  if (a.lo >= 0.0f) return a;
  if (a.hi <= 0.0f) return negR(a);
  return {0.0f, std::max(-a.lo, a.hi)};
}
R minR(R a, R b) noexcept { return {std::min(a.lo, b.lo), std::min(a.hi, b.hi)}; }
R maxR(R a, R b) noexcept { return {std::max(a.lo, b.lo), std::max(a.hi, b.hi)}; }

float below(double v) noexcept {  // the largest float ≤ v
  float f = static_cast<float>(v);
  if (static_cast<double>(f) > v) f = std::nextafter(f, -kInf);
  return f;
}
float above(double v) noexcept {
  float f = static_cast<float>(v);
  if (static_cast<double>(f) < v) f = std::nextafter(f, kInf);
  return f;
}

struct Ids {
  AtomId add = atomId("add"), sub = atomId("sub"), mul = atomId("mul"), div = atomId("div"), neg = atomId("neg");
  AtomId abs = atomId("abs"), min = atomId("min"), max = atomId("max"), clip = atomId("clip");
  AtomId muladd = atomId("muladd"), lerp = atomId("lerp"), select = atomId("select");
  AtomId phasor = atomId("phasor"), frac = atomId("frac"), gateToReal = atomId("gate_to_real"), sign = atomId("sign");
  AtomId channel = atomId("channel"), pack = atomId("pack");
  // Nominally [−1, 1].
  AtomId unit[8] = {atomId("sine"),     atomId("sine_hz"), atomId("tanh"),      atomId("fast_tanh"),
                    atomId("sin"),      atomId("fast_sin"), atomId("noise"),    atomId("noise_step")};
};
const Ids& ids() {
  static const Ids k;
  return k;
}

uint32_t specIndex(const AtomSignature& sig, uint32_t i) noexcept {
  const auto n = static_cast<uint32_t>(sig.inputs.size());
  return i < n ? i : n - 1;
}

}  // namespace

// An operand of a sum: its range and, when it is a product the JIT may contract into the sum's fma, the factors.
struct ValueRanges::Term {
  R r;
  bool product = false;
  R x, y;
};

ValueRanges::ValueRanges(const TypedPatch& tp, const Adjacency& adj, const ValueRangeOptions& options)
    : tp_(&tp), adj_(&adj), opt_(options) {}

// The value input i of node n reads at channel ch, followed through the structural nodes exactly as the scheduler
// resolves operands (schedule.cpp assignValues: `in` with a driver, channel, pack).
ValueRanges::Src ValueRanges::source(NodeIndex n, uint32_t i, uint32_t ch) const {
  const Patch& p = tp_->patch;
  for (uint32_t hop = 0;; ++hop) {
    const WireIndex w = adj_->driver[p.flatInput(n, i)];
    if (w == kNone) {
      Src s;
      const Node& nd = p.node(n);
      if (nd.kind == NodeKind::Atom) {
        const AtomSignature& sig = atomSignature(nd.atom);
        if (!sig.inputs.empty()) s.value = static_cast<float>(sig.inputs[specIndex(sig, i)].defaultValue);
      }
      return s;
    }
    const PortRef from = p.wire(w).from;
    const NodeIndex d = from.node;
    const uint32_t width = std::max<uint32_t>(1, tp_->types[p.flatOutput(d, from.port)].width);
    ch = std::min(ch, width - 1);
    const Node& dn = p.node(d);
    const Src here{d, from.port, ch, 0.0f};
    if (hop >= kMaxAliasHops) return here;
    if (dn.kind == NodeKind::Input && dn.numInputs > 0 && adj_->driver[p.flatInput(d, 0)] != kNone) {
      n = d, i = 0;
      continue;
    }
    if (dn.kind != NodeKind::Atom) return here;
    if (dn.atom == ids().channel && dn.numInputs >= 2) {
      const WireIndex iw = adj_->driver[p.flatInput(d, 1)];
      double idx = 0.0;
      if (iw == kNone) {
        const AtomSignature& sig = atomSignature(dn.atom);
        idx = sig.inputs[specIndex(sig, 1)].defaultValue;
      } else if (p.node(p.wire(iw).from.node).kind == NodeKind::Const) {
        idx = p.constant(p.wire(iw).from.node).value();
      } else {
        return here;  // not a constant index: the scheduler reports it
      }
      const uint32_t xw = std::max<uint32_t>(1, tp_->inputType(d, 0).width);
      n = d, i = 0;
      ch = xw == 1 || !(idx > 0.0) ? 0u : static_cast<uint32_t>(std::min(idx, xw - 1.0));
      continue;
    }
    if (dn.atom == ids().pack && dn.numInputs > 0) {
      uint32_t at = ch, pick = dn.numInputs - 1, pickCh = 0;
      for (uint32_t j = 0; j < dn.numInputs; ++j) {
        const uint32_t iw = std::max<uint32_t>(1, tp_->inputType(d, j).width);
        if (at < iw) {
          pick = j, pickCh = at;
          break;
        }
        at -= iw;
        pickCh = iw - 1;  // past the inputs: the last channel written (the scheduler repeats it)
      }
      n = d, i = pick, ch = pickCh;
      continue;
    }
    return here;
  }
}

ValueRange ValueRanges::of(const Src& s) {
  if (s.node == kNone) return point(s.value);
  return output(s.node, s.port, s.ch);
}

ValueRange ValueRanges::input(NodeIndex n, uint32_t i, uint32_t ch) { return of(source(n, i, ch)); }

ValueRange ValueRanges::output(NodeIndex n, uint32_t port, uint32_t ch) {
  const Patch& p = tp_->patch;
  if (n >= p.nodeCount() || port >= p.node(n).numOutputs) return {};
  const uint32_t fo = p.flatOutput(n, port);
  const uint32_t width = std::max<uint32_t>(1, tp_->types[fo].width);
  ch = std::min(ch, width - 1);
  const uint64_t key = (static_cast<uint64_t>(fo) << 16) | (ch & 0xFFFFu);
  if (const auto it = memo_.find(key); it != memo_.end()) return it->second;
  if (depth_ >= kMaxDepth) return {};
  memo_.emplace(key, R{});  // in progress: a value that depends on itself is unknown
  ++depth_;
  const R r = eval(n, port, ch);
  --depth_;
  memo_[key] = r;
  return r;
}

ValueRanges::Term ValueRanges::term(NodeIndex n, uint32_t i, uint32_t ch, bool negate) {
  const Patch& p = tp_->patch;
  Term t;
  Src s = source(n, i, ch);
  t.r = of(s);
  if (negate) t.r = negR(t.r);
  auto isAtom = [&](const Src& x, AtomId a) {
    return x.node != kNone && p.node(x.node).kind == NodeKind::Atom && p.node(x.node).atom == a;
  };
  if (isAtom(s, ids().neg)) {
    s = source(s.node, 0, s.ch);
    negate = !negate;
  }
  if (isAtom(s, ids().mul)) {
    t.product = true;
    t.x = input(s.node, 0, s.ch);
    t.y = input(s.node, 1, s.ch);
    if (negate) t.x = negR(t.x);
  }
  return t;
}

ValueRange ValueRanges::eval(NodeIndex n, uint32_t port, uint32_t ch) {
  const Patch& p = tp_->patch;
  const Node& nd = p.node(n);
  if (tp_->outputType(n, port).kind != Kind::Real) return {};
  switch (nd.kind) {
    case NodeKind::Const: {
      const Constant& c = p.constant(n);
      return c.kind == Kind::Real ? point(static_cast<float>(c.value())) : R{};
    }
    case NodeKind::Param: {
      const ParamDecl& d = p.paramDecl(n);
      if (!opt_.trustParamRanges || d.kind != Kind::Real || !d.hasRange) return {};
      const float lo = static_cast<float>(d.rangeMin.value()), hi = static_cast<float>(d.rangeMax.value());
      return lo <= hi ? norm({lo, hi}) : R{};
    }
    case NodeKind::Atom: break;
    default: return {};
  }
  const Ids& k = ids();
  const AtomId a = nd.atom;
  if (kernelMapping(a).lowering == Lowering::Constant) {  // sr
    const uint32_t factor = n < tp_->origin.size() ? tp_->factorOf(tp_->origin[n].region) : 1u;
    return point(static_cast<float>(opt_.sampleRate * factor));
  }
  if (port != 0) return {};
  auto in = [&](uint32_t i) { return input(n, i, ch); };
  // A sum of two operands, either of which may be a product contracted into it.
  auto sum = [&](const Term& x, const Term& y) {
    R r = norm({x.r.lo + y.r.lo, x.r.hi + y.r.hi});
    if (x.product) r = hull(r, fmaR(x.x, x.y, y.r));
    if (y.product) r = hull(r, fmaR(y.x, y.y, x.r));
    return r;
  };
  if (a == k.add) return sum(term(n, 0, ch, false), term(n, 1, ch, false));
  if (a == k.sub) return sum(term(n, 0, ch, false), term(n, 1, ch, true));  // a − b = a + (−b), the same rounding
  if (a == k.mul) return mulR(in(0), in(1));
  if (a == k.div) return divR(in(0), in(1));
  if (a == k.neg) return negR(in(0));
  if (a == k.abs) return absR(in(0));
  if (a == k.min) return minR(in(0), in(1));
  if (a == k.max) return maxR(in(0), in(1));
  if (a == k.clip) return minR(maxR(in(0), in(1)), in(2));  // min(max(x, lo), hi), as the kernel
  if (a == k.muladd) return fmaR(in(0), in(1), in(2));
  if (a == k.select) return hull(in(1), in(2));
  if (a == k.lerp) {
    // The kernel: fma(t, b − a, a). By intervals (exact for single values) …
    const R A = in(0), B = in(1), T = in(2);
    R r = fmaR(T, sum(term(n, 1, ch, false), term(n, 0, ch, true)), A);
    // … and, for t in [0, 1], the hull of a and b: the true value lies between them, the computed one within 2^−22 of
    // the largest magnitude (two roundings, of b − a and of the fma). Both hold, so their intersection does.
    if (T.lo >= 0.0f && T.hi <= 1.0f && finite(A) && finite(B)) {
      const R h = hull(A, B);
      const double pad = std::ldexp(std::max(std::fabs(static_cast<double>(h.lo)), std::fabs(static_cast<double>(h.hi))), -20);
      const R w = norm({below(static_cast<double>(h.lo) - pad), above(static_cast<double>(h.hi) + pad)});
      r = {std::max(r.lo, w.lo), std::min(r.hi, w.hi)};
      if (r.lo > r.hi) r = w;  // cannot happen (both contain the value); never return an empty range
    }
    return r;
  }
  if (a == k.phasor || a == k.frac || a == k.gateToReal) return {0.0f, 1.0f};
  if (a == k.sign) return {-1.0f, 1.0f};
  for (const AtomId u : k.unit)
    if (a == u) return {-kUnit, kUnit};
  return {};
}

}  // namespace pq::graph
