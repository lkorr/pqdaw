// Memory atoms (SPEC §7.2): z1, delay lines (linear / cubic Hermite / allpass reads), sample&hold, latch.
// Semantics are documented in pq/dsp/atoms.h.

#include <cstring>

#include "kernels_common.h"

namespace pq::dsp::detail {
namespace {

// z1: a one-frame shift, so it is a copy for both lane counts (vectorised by memcpy).
struct Z1 {
  static constexpr uint32_t kSpec = 1;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    if (a.frames == 0) return;
    auto& st = *static_cast<Z1State*>(a.state);
    constexpr uint32_t L = S::kW;
    float* out = a.out[0];
    const float* x = a.in[0];
    std::memcpy(out, st.last, L * sizeof(float));
    if constexpr (splBit(SM, 0)) {
      for (uint32_t f = 1; f < a.frames; ++f) std::memcpy(out + f * L, x, L * sizeof(float));
      std::memcpy(st.last, x, L * sizeof(float));
    } else {
      std::memcpy(out + L, x, (a.frames - 1) * L * sizeof(float));
      std::memcpy(st.last, x + (a.frames - 1) * L, L * sizeof(float));
    }
  }
};

enum class Interp { Linear, Cubic, Allpass };

// Ring-buffer offsets: frame `pos` (wrapped by mask) of this lane. The ring is frame-major with L lanes.
template <class S>
inline typename S::I ringIndex(typename S::I pos, typename S::I mask, typename S::I lane) noexcept {  // RT-safe
  if constexpr (S::kW == 8) return S::addi(S::template slli<3>(S::andi(pos, mask)), lane);
  else return S::andi(pos, mask);
}

// 4-point, 3rd-order Hermite (Catmull-Rom): interpolates between x0 (t = 0) and x1 (t = 1).
template <class S>
inline typename S::F hermite(typename S::F xm1, typename S::F x0, typename S::F x1, typename S::F x2,
                             typename S::F t) noexcept {  // RT-safe
  const auto half = S::set(0.5f);
  const auto c1 = S::mul(half, S::sub(x1, xm1));
  const auto c2 = S::add(S::fnma(S::set(2.5f), x0, xm1), S::fnma(half, x2, S::add(x1, x1)));
  const auto c3 = S::fma(half, S::sub(x2, xm1), S::mul(S::set(1.5f), S::sub(x0, x1)));
  return S::fma(S::fma(S::fma(c3, t, c2), t, c1), t, x0);
}

template <Interp K>
struct Delay {
  static constexpr uint32_t kSpec = 2;  // x, delay
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<DelayState*>(a.state);
    constexpr uint32_t L = S::kW;
    float* out = a.out[0];
    if (st.lanes != L) {  // state initialised for the other lane count: never touch the ring
      std::memset(out, 0, static_cast<size_t>(a.frames) * L * sizeof(float));
      return;
    }
    float* ring = st.ring();
    const In<S, splBit(SM, 0)> x(a.in[0]);
    const In<S, splBit(SM, 1)> delay(a.in[1]);
    const float minDelay = K == Interp::Linear ? 0.0f : K == Interp::Cubic ? 1.0f : 0.5f;
    const auto dMin = S::set(minDelay);
    const auto dMax = S::set(st.maxDelay);
    const auto mask = S::seti(static_cast<int32_t>(st.mask));
    const auto lane = S::laneIndex();
    const auto one = S::seti(1);
    uint32_t w = st.writePos;
    auto y1 = S::load(st.apY1);
    for (uint32_t f = 0; f < a.frames; ++f, ++w) {
      S::store(ring + static_cast<size_t>(w & st.mask) * L, x.at(f));
      // max(d, dMin) returns dMin for NaN.
      const auto d = S::min(S::max(delay.at(f), dMin), dMax);
      const auto fl = S::floor(d);
      auto fr = S::sub(d, fl);
      auto n = S::truncToInt(fl);
      const auto wv = S::seti(static_cast<int32_t>(w));
      typename S::F y;
      if constexpr (K == Interp::Linear) {
        const auto p0 = S::subi(wv, n);
        const auto x0 = S::gather(ring, ringIndex<S>(p0, mask, lane));
        const auto x1 = S::gather(ring, ringIndex<S>(S::subi(p0, one), mask, lane));
        y = S::fma(fr, S::sub(x1, x0), x0);
      } else if constexpr (K == Interp::Cubic) {
        const auto p0 = S::subi(wv, n);
        const auto xm1 = S::gather(ring, ringIndex<S>(S::addi(p0, one), mask, lane));
        const auto x0 = S::gather(ring, ringIndex<S>(p0, mask, lane));
        const auto x1 = S::gather(ring, ringIndex<S>(S::subi(p0, one), mask, lane));
        const auto x2 = S::gather(ring, ringIndex<S>(S::subi(p0, S::seti(2)), mask, lane));
        y = hermite<S>(xm1, x0, x1, x2, fr);
      } else {
        // Keep the allpass fraction in [0.5, 1.5): if fr < 0.5, borrow one sample from the integer part.
        const auto low = S::lt(fr, S::set(0.5f));
        fr = S::add(fr, S::band(low, S::set(1.0f)));
        n = S::addi(n, S::asI(low));  // −1 where low
        const auto p0 = S::subi(wv, n);
        const auto x0 = S::gather(ring, ringIndex<S>(p0, mask, lane));
        const auto x1 = S::gather(ring, ringIndex<S>(S::subi(p0, one), mask, lane));
        const auto c = S::div(S::sub(S::set(1.0f), fr), S::add(S::set(1.0f), fr));
        y = S::fma(c, S::sub(x0, y1), x1);
        y1 = y;
      }
      S::store(out + f * L, y);
    }
    st.writePos = w;
    S::store(st.apY1, y1);
  }
};

struct SampleHold {
  static constexpr uint32_t kSpec = 2;  // x, trig
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<SampleHoldState*>(a.state);
    const In<S, splBit(SM, 0)> x(a.in[0]);
    const In<S, splBit(SM, 1)> trig(a.in[1]);
    float* out = a.out[0];
    auto held = S::load(st.held);
    auto prev = S::load(st.prevTrig);
    for (uint32_t f = 0; f < a.frames; ++f) {
      const auto t = trig.at(f);
      held = S::select(S::andnot(prev, t), x.at(f), held);
      prev = t;
      S::store(out + f * S::kW, held);
    }
    S::store(st.held, held);
    S::store(st.prevTrig, prev);
  }
};

struct Latch {
  static constexpr uint32_t kSpec = 2;  // x, enable
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<LatchState*>(a.state);
    const In<S, splBit(SM, 0)> x(a.in[0]);
    const In<S, splBit(SM, 1)> en(a.in[1]);
    float* out = a.out[0];
    auto held = S::load(st.held);
    for (uint32_t f = 0; f < a.frames; ++f) {
      held = S::select(en.at(f), x.at(f), held);
      S::store(out + f * S::kW, held);
    }
    S::store(st.held, held);
  }
};

}  // namespace

KernelFn findMemoryKernel(Atom a, uint32_t lanes, uint32_t m) noexcept {
  switch (a) {
    case Atom::Z1: return pick<Z1>(lanes, m);
    case Atom::DelayLinear: return pick<Delay<Interp::Linear>>(lanes, m);
    case Atom::DelayCubic: return pick<Delay<Interp::Cubic>>(lanes, m);
    case Atom::DelayAllpass: return pick<Delay<Interp::Allpass>>(lanes, m);
    case Atom::SampleHold: return pick<SampleHold>(lanes, m);
    case Atom::Latch: return pick<Latch>(lanes, m);
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail
