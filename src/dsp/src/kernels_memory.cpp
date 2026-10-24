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

// z1 split for feedback loops scheduled per sample: read (emit the stored value) at the start of the sample,
// write (store the input) at the end.
struct Z1Read {
  static constexpr uint32_t kSpec = 0;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    const auto& st = *static_cast<const Z1State*>(a.state);
    for (uint32_t f = 0; f < a.frames; ++f) std::memcpy(a.out[0] + f * S::kW, st.last, S::kW * sizeof(float));
  }
};
struct Z1Write {
  static constexpr uint32_t kSpec = 1;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    if (a.frames == 0) return;
    auto& st = *static_cast<Z1State*>(a.state);
    const float* x = splBit(SM, 0) ? a.in[0] : a.in[0] + static_cast<size_t>(a.frames - 1) * S::kW;
    std::memcpy(st.last, x, S::kW * sizeof(float));
  }
};

enum class Interp { Linear, Cubic, Allpass };

// Minimum delay per read type: every tap must be at least one sample old (so `x` is a delayed input and a
// feedback loop through a delay line is legal): linear ≥ 1, cubic ≥ 2 (its newest tap is one sample newer than
// the interpolated pair), allpass ≥ 1.5 (fraction kept in [0.5, 1.5) with the newest tap ≥ 1 sample old).
template <Interp K>
constexpr float kMinDelay = K == Interp::Linear ? 1.0f : K == Interp::Cubic ? 2.0f : 1.5f;

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

// One delayed read. `pos` = ring frame of the sample being produced (its own input is never read); `rix`
// maps a frame position to a ring float index (lane-interleaved for voice groups, plain for mono).
// kAge (8-lane rings with a lane younger than the longest tap, DelayState::start): a tap `k` frames old reads 0
// where k > age (frames of the lane's valid history), bit for bit what a zeroed ring would give.
template <class S, Interp K, bool kAge = false, class RingIndex>
inline typename S::F delayTap(const float* ring, typename S::I pos, typename S::F dIn, float maxDelay,
                              RingIndex rix, typename S::F& y1, typename S::I age = {}) noexcept {  // RT-safe
  const auto one = S::seti(1);
  // Tap `k` frames old (k as a vector of distances): valid iff k ≤ age.
  auto tap = [&](typename S::I k) noexcept {
    const auto x = S::gather(ring, rix(S::subi(pos, k)));
    if constexpr (kAge) return S::andnot(S::asF(S::gti(k, age)), x);
    else return x;
  };
  // max(d, min) returns min for NaN.
  const auto d = S::min(S::max(dIn, S::set(kMinDelay<K>)), S::set(maxDelay));
  const auto fl = S::floor(d);
  auto fr = S::sub(d, fl);
  auto n = S::truncToInt(fl);
  if constexpr (K == Interp::Linear) {
    const auto x0 = tap(n);
    const auto x1 = tap(S::addi(n, one));
    return S::fma(fr, S::sub(x1, x0), x0);
  } else if constexpr (K == Interp::Cubic) {
    const auto xm1 = tap(S::subi(n, one));
    const auto x0 = tap(n);
    const auto x1 = tap(S::addi(n, one));
    const auto x2 = tap(S::addi(n, S::seti(2)));
    return hermite<S>(xm1, x0, x1, x2, fr);
  } else {
    // First-order allpass y = c·(x0 − y1) + x1, c = (1 − Δ)/(1 + Δ), with Δ kept in [0.5, 1.5): if fr < 0.5,
    // borrow one sample from the integer part.
    const auto low = S::lt(fr, S::set(0.5f));
    fr = S::add(fr, S::band(low, S::set(1.0f)));
    n = S::addi(n, S::asI(low));  // −1 where low
    const auto x0 = tap(n);
    const auto x1 = tap(S::addi(n, one));
    const auto c = S::div(S::sub(S::set(1.0f), fr), S::add(S::set(1.0f), fr));
    const auto y = S::fma(c, S::sub(x0, y1), x1);
    y1 = y;
    return y;
  }
}

// 8-lane rings (DelayState::start): the smallest writePos − start over the lanes, and the clamp that keeps every
// lane's history ≤ capacity frames (so the uint32 difference never wraps; history beyond the ring is gone anyway).
inline uint32_t youngestAge(const DelayState& st, uint32_t w) noexcept {  // RT-safe
  const __m256i age = _mm256_sub_epi32(_mm256_set1_epi32(static_cast<int32_t>(w)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(st.start)));
  __m128i m = _mm_min_epu32(_mm256_castsi256_si128(age), _mm256_extracti128_si256(age, 1));
  m = _mm_min_epu32(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(1, 0, 3, 2)));
  m = _mm_min_epu32(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(2, 3, 0, 1)));
  return static_cast<uint32_t>(_mm_cvtsi128_si32(m));
}
inline void clampStart(DelayState& st) noexcept {  // RT-safe
  const __m256i w = _mm256_set1_epi32(static_cast<int32_t>(st.writePos));
  __m256i* sp = reinterpret_cast<__m256i*>(st.start);
  const __m256i age = _mm256_min_epu32(_mm256_sub_epi32(w, _mm256_loadu_si256(sp)),
                                       _mm256_set1_epi32(static_cast<int32_t>(st.capacity)));
  _mm256_storeu_si256(sp, _mm256_sub_epi32(w, age));
}

// Delay line kernels. The atom (kRead && kWrite) writes then reads each frame; the split kernels, for feedback
// loops scheduled per sample (atoms.h), do one each. Inputs: atom (x, delay); read (delay); write (x).
template <Interp K, bool kRead, bool kWrite>
struct Delay {
  static constexpr uint32_t kSpec = kRead && kWrite ? 2 : 1;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<DelayState*>(a.state);
    constexpr uint32_t L = S::kW;
    constexpr uint32_t kD = kWrite ? 1 : 0;  // input index of the delay time
    if (st.lanes != L) {  // state initialised for the other lane count: never touch the ring
      if constexpr (kRead) std::memset(a.out[0], 0, static_cast<size_t>(a.frames) * L * sizeof(float));
      return;
    }
    float* ring = st.ring();
    uint32_t w = st.writePos;
    uint32_t f = 0;
    if constexpr (!kRead) {
      const In<S, splBit(SM, 0)> x(a.in[0]);
      for (; f < a.frames; ++f, ++w) S::store(ring + static_cast<size_t>(w & st.mask) * L, x.at(f));
      st.writePos = w;
      if constexpr (L == 8) clampStart(st);
    } else {
      float* out = a.out[0];
      auto y1 = S::load(st.apY1);
      if constexpr (S::kW == 1 && kWrite && K != Interp::Allpass) {
        // Mono linear/cubic reads have no recurrence and every tap is ≥ 1 sample old, so a chunk of 8 inputs
        // can be written first and the 8 outputs read with gathers, one frame per AVX2 lane (the ring has 10
        // frames of headroom for the samples written ahead). Allpass keeps its per-sample recurrence (y1).
        const auto mask8 = S8::seti(static_cast<int32_t>(st.mask));
        const auto rix8 = [mask8](S8::I p) noexcept { return S8::andi(p, mask8); };
        S8::F unusedY1 = S8::zero();
        for (; f + 8 <= a.frames; f += 8, w += 8) {
          const uint32_t wpos = w & st.mask;
          if constexpr (splBit(SM, 0)) {
            if (wpos + 8 <= st.capacity) S8::store(ring + wpos, S8::set(a.in[0][0]));
            else for (uint32_t k = 0; k < 8; ++k) ring[(w + k) & st.mask] = a.in[0][0];
          } else {
            if (wpos + 8 <= st.capacity) S8::store(ring + wpos, S8::load(a.in[0] + f));
            else for (uint32_t k = 0; k < 8; ++k) ring[(w + k) & st.mask] = a.in[0][f + k];
          }
          const auto dIn = splBit(SM, kD) ? S8::set(a.in[kD][0]) : S8::load(a.in[kD] + f);
          const auto pos = S8::addi(S8::seti(static_cast<int32_t>(w)), S8::laneIndex());
          S8::store(out + f, delayTap<S8, K>(ring, pos, dIn, st.maxDelay, rix8, unusedY1));
        }
      }
      const auto mask = S::seti(static_cast<int32_t>(st.mask));
      const auto lane = S::laneIndex();
      const auto rix = [mask, lane](typename S::I p) noexcept { return ringIndex<S>(p, mask, lane); };
      const In<S, splBit(SM, kD)> delay(a.in[kD]);
      if constexpr (L == 8) {
        // A lane younger than the longest tap (floor(maxDelay) + 2 frames; after its O(1) reset): taps older than
        // its history read 0 (age = w − start per lane, see delayTap). Only the first frames after a note-on.
        if (youngestAge(st, w) < static_cast<uint32_t>(st.maxDelay) + 2u) {
          const auto start = S::loadi(reinterpret_cast<const int32_t*>(st.start));
          for (; f < a.frames; ++f, ++w) {
            if constexpr (kWrite) {
              const In<S, splBit(SM, 0)> x(a.in[0]);
              S::store(ring + static_cast<size_t>(w & st.mask) * L, x.at(f));
            }
            const auto pos = S::seti(static_cast<int32_t>(w));
            S::store(out + f * L, delayTap<S, K, true>(ring, pos, delay.at(f), st.maxDelay, rix, y1, S::subi(pos, start)));
          }
        }
      }
      // Steady state (and mono rings): every tap is inside the history.
      for (; f < a.frames; ++f, ++w) {
        if constexpr (kWrite) {
          const In<S, splBit(SM, 0)> x(a.in[0]);
          S::store(ring + static_cast<size_t>(w & st.mask) * L, x.at(f));
        }
        const auto pos = S::seti(static_cast<int32_t>(w));
        S::store(out + f * L, delayTap<S, K>(ring, pos, delay.at(f), st.maxDelay, rix, y1));
      }
      if constexpr (kWrite) {
        st.writePos = w;
        if constexpr (L == 8) clampStart(st);
      }
      S::store(st.apY1, y1);
    }
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
    case Atom::Z1Read: return pick<Z1Read>(lanes, m);
    case Atom::Z1Write: return pick<Z1Write>(lanes, m);
    case Atom::DelayLinear: return pick<Delay<Interp::Linear, true, true>>(lanes, m);
    case Atom::DelayCubic: return pick<Delay<Interp::Cubic, true, true>>(lanes, m);
    case Atom::DelayAllpass: return pick<Delay<Interp::Allpass, true, true>>(lanes, m);
    case Atom::DelayReadLinear: return pick<Delay<Interp::Linear, true, false>>(lanes, m);
    case Atom::DelayReadCubic: return pick<Delay<Interp::Cubic, true, false>>(lanes, m);
    case Atom::DelayReadAllpass: return pick<Delay<Interp::Allpass, true, false>>(lanes, m);
    case Atom::DelayWrite: return pick<Delay<Interp::Linear, false, true>>(lanes, m);
    case Atom::SampleHold: return pick<SampleHold>(lanes, m);
    case Atom::Latch: return pick<Latch>(lanes, m);
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail
