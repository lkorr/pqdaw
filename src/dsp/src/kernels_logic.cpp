// Logic atoms (SPEC §7.2): bool lanes as masks. Semantics are documented in pq/dsp/atoms.h.
//
// Stateful logic has a per-sample recurrence per lane, so it runs one frame per iteration: one __m256 (8 voices)
// for L = 8, one SSE lane for L = 1. Each body is written once over the ops struct S.

#include "kernels_common.h"

namespace pq::dsp::detail {
namespace {

#define PQ_BINARY(Name, expr)                                                                     \
  struct Name {                                                                                   \
    static constexpr uint32_t kArity = 2;                                                         \
    template <class S>                                                                            \
    static typename S::F f(typename S::F a, typename S::F b) noexcept { /* RT-safe */ return expr; } \
  };

PQ_BINARY(AndOp, S::band(a, b))
PQ_BINARY(OrOp, S::bor(a, b))
PQ_BINARY(XorOp, S::bxor(a, b))
PQ_BINARY(NandOp, S::bnot(S::band(a, b)))
PQ_BINARY(NorOp, S::bnot(S::bor(a, b)))
PQ_BINARY(GtOp, S::gt(a, b))
PQ_BINARY(LtOp, S::lt(a, b))
PQ_BINARY(GeOp, S::ge(a, b))
PQ_BINARY(LeOp, S::le(a, b))
PQ_BINARY(EqOp, S::eq(a, b))
PQ_BINARY(NeOp, S::ne(a, b))
#undef PQ_BINARY

struct NotOp {
  static constexpr uint32_t kArity = 1;
  template <class S>
  static typename S::F f(typename S::F a) noexcept { return S::bnot(a); }  // RT-safe
};
struct GateToRealOp {
  static constexpr uint32_t kArity = 1;
  template <class S>
  static typename S::F f(typename S::F a) noexcept { return S::band(a, S::set(1.0f)); }  // RT-safe
};

// mux(index, in0 … in{n−1}): variadic, so the Spl choice of the data inputs is read from KernelArgs::splMask at
// run time: input k is read at `p + (i & m_k)` with m_k = 0 for a Spl pattern (whose 8 floats are what both the
// vector path and, for L = 1, the uniform scalar tail need) and ~0 for a block. One AND per input per vector.
struct Mux {
  static constexpr uint32_t kSpec = 1;  // index
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    const uint32_t n = a.numIn > 1 ? static_cast<uint32_t>(a.numIn) - 1u : 0u;
    if (n == 0) return;
    const uint32_t count = a.frames * S::kW;
    const uint32_t vecEnd = count & ~7u;
    const float* const* data = a.in + 1;
    float* out = a.out[0];
    const In<S8, splBit(SM, 0)> idx(a.in[0]);
    auto off = [&](uint32_t k, uint32_t i) noexcept { return splBit(a.splMask, k + 1) ? 0u : i; };
    for (uint32_t i = 0; i < vecEnd; i += 8) {
      const S8::F x = idx.at(i >> 3);
      S8::F r = S8::load(data[0] + off(0, i));
      for (uint32_t k = 1; k < n; ++k)
        r = S8::select(S8::ge(x, S8::set(static_cast<float>(k))), S8::load(data[k] + off(k, i)), r);
      S8::store(out + i, r);
    }
    for (uint32_t i = vecEnd; i < count; ++i) {
      const S1::F x = S1::load(splBit(SM, 0) ? a.in[0] : a.in[0] + i);
      S1::F r = S1::load(data[0] + off(0, i));
      for (uint32_t k = 1; k < n; ++k)
        r = S1::select(S1::ge(x, S1::set(static_cast<float>(k))), S1::load(data[k] + off(k, i)), r);
      S1::store(out + i, r);
    }
  }
};

// demux(index, x) → out0 … out{n−1}.
struct Demux {
  static constexpr uint32_t kSpec = 2;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    const uint32_t n = a.numOut;
    if (n == 0) return;
    const uint32_t count = a.frames * S::kW;
    const uint32_t vecEnd = count & ~7u;
    const float top = static_cast<float>(n - 1);
    const In<S8, splBit(SM, 0)> idx(a.in[0]);
    const In<S8, splBit(SM, 1)> xin(a.in[1]);
    for (uint32_t i = 0; i < vecEnd; i += 8) {
      // max(floor, 0) returns 0 for NaN (second operand); then clamp to n − 1.
      const S8::F c = S8::min(S8::max(S8::floor(idx.at(i >> 3)), S8::zero()), S8::set(top));
      const S8::F x = xin.at(i >> 3);
      for (uint32_t k = 0; k < n; ++k)
        S8::store(a.out[k] + i, S8::band(S8::eq(c, S8::set(static_cast<float>(k))), x));
    }
    for (uint32_t i = vecEnd; i < count; ++i) {
      const S1::F c = S1::min(S1::max(S1::floor(S1::load(splBit(SM, 0) ? a.in[0] : a.in[0] + i)), S1::zero()),
                              S1::set(top));
      const S1::F x = S1::load(splBit(SM, 1) ? a.in[1] : a.in[1] + i);
      for (uint32_t k = 0; k < n; ++k)
        S1::store(a.out[k] + i, S1::band(S1::eq(c, S1::set(static_cast<float>(k))), x));
    }
  }
};

// ---- Stateful logic -----------------------------------------------------------------------------------------

struct Schmitt {
  static constexpr uint32_t kSpec = 3;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<SchmittState*>(a.state);
    const In<S, splBit(SM, 0)> x(a.in[0]);
    const In<S, splBit(SM, 1)> lo(a.in[1]);
    const In<S, splBit(SM, 2)> hi(a.in[2]);
    float* out = a.out[0];
    auto q = S::load(st.q);
    for (uint32_t f = 0; f < a.frames; ++f) {
      const auto v = x.at(f);
      q = S::select(S::gt(v, hi.at(f)), S::ones(), S::andnot(S::lt(v, lo.at(f)), q));
      S::store(out + f * S::kW, q);
    }
    S::store(st.q, q);
  }
};

enum class EdgeKind { Rise, Fall, Change };

template <EdgeKind K>
struct Edge {
  static constexpr uint32_t kSpec = 1;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<EdgeState*>(a.state);
    const In<S, splBit(SM, 0)> b(a.in[0]);
    float* out = a.out[0];
    auto prev = S::load(st.prev);
    for (uint32_t f = 0; f < a.frames; ++f) {
      const auto v = b.at(f);
      typename S::F r;
      if constexpr (K == EdgeKind::Rise) r = S::andnot(prev, v);
      else if constexpr (K == EdgeKind::Fall) r = S::andnot(v, prev);
      else r = S::bxor(v, prev);
      S::store(out + f * S::kW, r);
      prev = v;
    }
    S::store(st.prev, prev);
  }
};

struct SrFlipFlop {
  static constexpr uint32_t kSpec = 2;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<FlipFlopState*>(a.state);
    const In<S, splBit(SM, 0)> s(a.in[0]);
    const In<S, splBit(SM, 1)> r(a.in[1]);
    float* out = a.out[0];
    auto q = S::load(st.q);
    for (uint32_t f = 0; f < a.frames; ++f) {
      q = S::andnot(r.at(f), S::bor(s.at(f), q));
      S::store(out + f * S::kW, q);
    }
    S::store(st.q, q);
  }
};

template <bool kToggle>
struct ClockedFlipFlop {  // d (kToggle = false) and t (kToggle = true): inputs (data, clk)
  static constexpr uint32_t kSpec = 2;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<FlipFlopState*>(a.state);
    const In<S, splBit(SM, 0)> d(a.in[0]);
    const In<S, splBit(SM, 1)> clk(a.in[1]);
    float* out = a.out[0];
    auto q = S::load(st.q);
    auto prev = S::load(st.prevClk);
    for (uint32_t f = 0; f < a.frames; ++f) {
      const auto c = clk.at(f);
      const auto edge = S::andnot(prev, c);
      if constexpr (kToggle) q = S::bxor(q, S::band(edge, d.at(f)));
      else q = S::select(edge, d.at(f), q);
      prev = c;
      S::store(out + f * S::kW, q);
    }
    S::store(st.q, q);
    S::store(st.prevClk, prev);
  }
};

// Valid integer count from a real input: round(x) when 1 ≤ x < 2^31, else `fallback` (NaN → fallback).
template <class S>
inline typename S::I countFrom(typename S::F x, int32_t fallback) noexcept {  // RT-safe
  const auto valid = S::band(S::ge(x, S::set(1.0f)), S::lt(x, S::set(2147483648.0f)));
  return S::selecti(S::asI(valid), S::roundToInt(x), S::seti(fallback));
}

struct Counter {
  static constexpr uint32_t kSpec = 4;  // up, down, reset, n
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<CounterState*>(a.state);
    const In<S, splBit(SM, 0)> up(a.in[0]);
    const In<S, splBit(SM, 1)> down(a.in[1]);
    const In<S, splBit(SM, 2)> reset(a.in[2]);
    const In<S, splBit(SM, 3)> nIn(a.in[3]);
    float* out = a.out[0];
    auto count = S::loadi(st.count);
    auto pu = S::load(st.prevUp);
    auto pd = S::load(st.prevDown);
    const auto zero = S::seti(0);
    for (uint32_t f = 0; f < a.frames; ++f) {
      const auto u = up.at(f);
      const auto d = down.at(f);
      const auto eu = S::asI(S::andnot(pu, u));  // −1 on an up edge
      const auto ed = S::asI(S::andnot(pd, d));
      pu = u;
      pd = d;
      count = S::addi(S::subi(count, eu), ed);
      const auto n = countFrom<S>(nIn.at(f), 0);
      const auto wrap = S::gti(n, zero);
      const auto nm1 = S::subi(n, S::seti(1));
      count = S::selecti(S::andi(wrap, S::gti(count, nm1)), zero, count);
      count = S::selecti(S::andi(wrap, S::gti(zero, count)), nm1, count);
      count = S::selecti(S::asI(reset.at(f)), zero, count);
      S::store(out + f * S::kW, S::toFloat(count));
    }
    S::storei(st.count, count);
    S::store(st.prevUp, pu);
    S::store(st.prevDown, pd);
  }
};

struct ClockDiv {
  static constexpr uint32_t kSpec = 2;  // clk, n
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<ClockDivState*>(a.state);
    const In<S, splBit(SM, 0)> clk(a.in[0]);
    const In<S, splBit(SM, 1)> nIn(a.in[1]);
    float* out = a.out[0];
    auto k = S::loadi(st.k);
    auto prev = S::load(st.prevClk);
    const auto zero = S::seti(0);
    for (uint32_t f = 0; f < a.frames; ++f) {
      const auto c = clk.at(f);
      const auto edge = S::asI(S::andnot(prev, c));
      prev = c;
      const auto n = countFrom<S>(nIn.at(f), 1);
      auto next = S::addi(k, S::seti(1));
      next = S::selecti(S::gti(n, next), next, zero);  // k + 1 ≥ n → 0
      k = S::selecti(edge, next, k);
      S::store(out + f * S::kW, S::band(c, S::asF(S::eqi(k, zero))));
    }
    S::storei(st.k, k);
    S::store(st.prevClk, prev);
  }
};

struct Pulse {
  static constexpr uint32_t kSpec = 1;  // period
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<PulseState*>(a.state);
    const In<S, splBit(SM, 0)> period(a.in[0]);
    float* out = a.out[0];
    auto r = S::loadi(st.remaining);
    const auto zero = S::seti(0);
    for (uint32_t f = 0; f < a.frames; ++f) {
      const auto fire = S::eqi(r, zero);
      const auto p = countFrom<S>(period.at(f), 0);  // invalid → 0 → next value −1 (one-shot)
      // r > 0 → r − 1 (adding the −1 mask); r == 0 → p − 1; r < 0 stays.
      const auto dec = S::addi(r, S::gti(r, zero));
      r = S::selecti(fire, S::subi(p, S::seti(1)), dec);
      S::store(out + f * S::kW, S::asF(fire));
    }
    S::storei(st.remaining, r);
  }
};

struct Timer {
  static constexpr uint32_t kSpec = 2;  // trig, duration
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<TimerState*>(a.state);
    const In<S, splBit(SM, 0)> trig(a.in[0]);
    const In<S, splBit(SM, 1)> dur(a.in[1]);
    float* out = a.out[0];
    auto r = S::loadi(st.remaining);
    auto prev = S::load(st.prevTrig);
    const auto zero = S::seti(0);
    for (uint32_t f = 0; f < a.frames; ++f) {
      const auto t = trig.at(f);
      const auto edge = S::asI(S::andnot(prev, t));
      prev = t;
      // N = round(duration) clamped to [0, 2^31 − 1]; NaN → 0. min() first so huge values clamp before
      // conversion; max(·, 0) returns 0 for NaN.
      const auto d = S::max(S::min(dur.at(f), S::set(2147483520.0f)), S::zero());
      r = S::selecti(edge, S::roundToInt(d), r);
      const auto on = S::gti(r, zero);
      S::store(out + f * S::kW, S::asF(on));
      r = S::addi(r, on);  // −1 while positive
    }
    S::storei(st.remaining, r);
    S::store(st.prevTrig, prev);
  }
};

}  // namespace

KernelFn findLogicKernel(Atom a, uint32_t lanes, uint32_t m) noexcept {
  switch (a) {
    case Atom::And: return pick<Ew<AndOp>>(lanes, m);
    case Atom::Or: return pick<Ew<OrOp>>(lanes, m);
    case Atom::Xor: return pick<Ew<XorOp>>(lanes, m);
    case Atom::Nand: return pick<Ew<NandOp>>(lanes, m);
    case Atom::Nor: return pick<Ew<NorOp>>(lanes, m);
    case Atom::Not: return pick<Ew<NotOp>>(lanes, m);
    case Atom::Gt: return pick<Ew<GtOp>>(lanes, m);
    case Atom::Lt: return pick<Ew<LtOp>>(lanes, m);
    case Atom::Ge: return pick<Ew<GeOp>>(lanes, m);
    case Atom::Le: return pick<Ew<LeOp>>(lanes, m);
    case Atom::Eq: return pick<Ew<EqOp>>(lanes, m);
    case Atom::Ne: return pick<Ew<NeOp>>(lanes, m);
    case Atom::GateToReal: return pick<Ew<GateToRealOp>>(lanes, m);
    case Atom::Mux: return pick<Mux>(lanes, m);
    case Atom::Demux: return pick<Demux>(lanes, m);
    case Atom::Schmitt: return pick<Schmitt>(lanes, m);
    case Atom::Rise: return pick<Edge<EdgeKind::Rise>>(lanes, m);
    case Atom::Fall: return pick<Edge<EdgeKind::Fall>>(lanes, m);
    case Atom::Change: return pick<Edge<EdgeKind::Change>>(lanes, m);
    case Atom::SrFlipFlop: return pick<SrFlipFlop>(lanes, m);
    case Atom::DFlipFlop: return pick<ClockedFlipFlop<false>>(lanes, m);
    case Atom::TFlipFlop: return pick<ClockedFlipFlop<true>>(lanes, m);
    case Atom::Counter: return pick<Counter>(lanes, m);
    case Atom::ClockDiv: return pick<ClockDiv>(lanes, m);
    case Atom::Pulse: return pick<Pulse>(lanes, m);
    case Atom::Timer: return pick<Timer>(lanes, m);
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail
