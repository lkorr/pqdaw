// Generator and table atoms (SPEC §7.2): phasor, sine_hz, wavetable, noise, shaper (SM.2: the sample atoms are in
// kernels_sample.cpp).
// Semantics are documented in pq/dsp/atoms.h and pq/dsp/tables.h.

#include <cstring>

#include "kernels_common.h"
#include "pq/dsp/tables.h"

namespace pq::dsp::detail {
namespace {

constexpr float kTwoPiOver2p32 = 1.46291807926715968e-09f;  // 2π / 2^32
constexpr float k2pm24 = 5.9604644775390625e-08f;           // 2^-24
constexpr float k2pm23 = 1.1920928955078125e-07f;           // 2^-23

// ---- phasor / sine_hz ---------------------------------------------------------------------------------------

template <class S>
inline typename S::F phaseToUnit(typename S::I ph) noexcept {  // RT-safe: top 24 bits → [0, 1), exact
  return S::mul(S::toFloat(S::template srli<8>(ph)), S::set(k2pm24));
}
template <class S>
inline typename S::F phaseToSine(typename S::I ph) noexcept {  // RT-safe: phase as signed → [−π, π) → sin
  return S::fastSin(S::mul(S::toFloat(ph), S::set(kTwoPiOver2p32)));
}

template <bool kSine>
struct PhasorK {
  static constexpr uint32_t kSpec = 2;  // hz, reset
  template <class S>
  static typename S::F emit(typename S::I ph) noexcept {
    if constexpr (kSine) return phaseToSine<S>(ph);
    else return phaseToUnit<S>(ph);
  }
  template <class S>
  static typename S::I increment(typename S::F hz, const PhasorState& st) noexcept {
    // NaN hz → 0 Hz (an ordered self-compare masks it to +0; without it max(NaN, −half) gave −sr/2, a full-scale
    // Nyquist square: M1 review L3). At ±sr/2 the increment is ±2^31, the same phase step either way.
    const auto h = S::min(S::max(S::band(S::eq(hz, hz), hz), S::set(-st.halfSr)), S::set(st.halfSr));
    return S::roundMulToInt(h, st.incScale);
  }

  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<PhasorState*>(a.state);
    const In<S, splBit(SM, 0)> hz(a.in[0]);
    const In<S, splBit(SM, 1)> reset(a.in[1]);
    float* out = a.out[0];
    const bool resetActive = splBit(SM, 1) ? S::any(reset.at(0)) : blockAny(a.in[1], a.frames * S::kW);

    if constexpr (S::kW == 1 && splBit(SM, 0)) {
      if (!resetActive) {
        // Mono, constant frequency, no reset: no recurrence left, so vectorise over time:
        // phase(t) = phase0 + inc·t (mod 2^32), 8 samples per __m256.
        const uint32_t inc = static_cast<uint32_t>(_mm_cvtsi128_si32(increment<S1>(hz.at(0), st)));
        const uint32_t ph0 = st.phase[0];
        const uint32_t vecEnd = a.frames & ~7u;
        auto phv = S8::addi(S8::seti(static_cast<int32_t>(ph0)),
                            S8::muli(S8::seti(static_cast<int32_t>(inc)), S8::laneIndex()));
        const auto step = S8::seti(static_cast<int32_t>(inc * 8u));
        for (uint32_t i = 0; i < vecEnd; i += 8) {
          S8::store(out + i, emit<S8>(phv));
          phv = S8::addi(phv, step);
        }
        for (uint32_t i = vecEnd; i < a.frames; ++i)
          S1::store(out + i, emit<S1>(S1::seti(static_cast<int32_t>(ph0 + inc * i))));
        st.phase[0] = ph0 + inc * a.frames;
        return;
      }
    }

    if constexpr (S::kW == 1 && !splBit(SM, 0)) {
      if (!resetActive) {
        // Mono, audio-rate frequency, no reset: 8 increments at once, then an in-register exclusive prefix sum
        // gives the 8 phases (still exact integer arithmetic, mod 2^32).
        uint32_t ph0 = st.phase[0];
        const uint32_t vecEnd = a.frames & ~7u;
        for (uint32_t i = 0; i < vecEnd; i += 8) {
          const auto inc = increment<S8>(S8::load(a.in[0] + i), st);
          // Inclusive scan within each 128-bit half, then carry the low half's total into the high half.
          auto scan = _mm256_add_epi32(inc, _mm256_slli_si256(inc, 4));
          scan = _mm256_add_epi32(scan, _mm256_slli_si256(scan, 8));
          const auto lowTotal = _mm256_permutevar8x32_epi32(scan, _mm256_set1_epi32(3));
          scan = _mm256_add_epi32(scan, _mm256_blend_epi32(_mm256_setzero_si256(), lowTotal, 0xF0));
          const auto phases = _mm256_add_epi32(S8::seti(static_cast<int32_t>(ph0)), _mm256_sub_epi32(scan, inc));
          S8::store(out + i, emit<S8>(phases));
          ph0 += static_cast<uint32_t>(_mm256_extract_epi32(scan, 7));
        }
        for (uint32_t i = vecEnd; i < a.frames; ++i) {
          S1::store(out + i, emit<S1>(S1::seti(static_cast<int32_t>(ph0))));
          ph0 += static_cast<uint32_t>(_mm_cvtsi128_si32(increment<S1>(S1::load(a.in[0] + i), st)));
        }
        st.phase[0] = ph0;
        return;
      }
    }

    auto ph = S::loadu32(st.phase);
    const auto zero = S::seti(0);
    if constexpr (splBit(SM, 0)) {
      const auto inc = increment<S>(hz.at(0), st);
      if (resetActive) {
        for (uint32_t f = 0; f < a.frames; ++f) {
          ph = S::selecti(S::asI(reset.at(f)), zero, ph);
          S::store(out + f * S::kW, emit<S>(ph));
          ph = S::addi(ph, inc);
        }
      } else {
        for (uint32_t f = 0; f < a.frames; ++f) {
          S::store(out + f * S::kW, emit<S>(ph));
          ph = S::addi(ph, inc);
        }
      }
    } else {
      if (resetActive) {
        for (uint32_t f = 0; f < a.frames; ++f) {
          ph = S::selecti(S::asI(reset.at(f)), zero, ph);
          S::store(out + f * S::kW, emit<S>(ph));
          ph = S::addi(ph, increment<S>(hz.at(f), st));
        }
      } else {
        for (uint32_t f = 0; f < a.frames; ++f) {
          S::store(out + f * S::kW, emit<S>(ph));
          ph = S::addi(ph, increment<S>(hz.at(f), st));
        }
      }
    }
    S::storeu32(st.phase, ph);
  }
};

// ---- wavetable ------------------------------------------------------------------------------------------------

// |p − prev| in cycles, wrapped to [0, 0.5]; NaN → 0 (max returns its second operand for NaN).
template <class S>
inline typename S::F phaseDelta(typename S::F p, typename S::F prev) noexcept {  // RT-safe
  auto d = S::sub(p, prev);
  d = S::abs(S::sub(d, S::round(d)));
  return S::max(d, S::zero());
}

// One table read with the mip crossfade of tables.h. `inc` = increment estimate in cycles (≥ 0, not NaN).
// kSkip: test whether any lane needs the lower level before gathering it (worth it when the lanes are consecutive
// samples of one pitch; voice lanes almost never all sit at t = 1, so there the test is pure overhead).
template <class S, bool kSkip>
inline typename S::F wavetableRead(const WavetableView& v, typename S::F p, typename S::F inc) noexcept {  // RT-safe
  // u = 2·log2(N·inc) from the float bits: bits·2^−23 − 127 = e + (m − 1) ≈ log2(N·inc) (0.086 octave low at most).
  const auto x = S::mul(inc, S::set(v.sizeF));
  const auto u = S::fma(S::toFloat(S::asI(x)), S::set(2.384185791015625e-07f), S::set(-254.0f));  // 2^-22
  const auto uf = S::floor(u);
  const auto fu = S::sub(u, uf);
  const auto t = S::min(S::add(fu, fu), S::set(1.0f));
  const auto k = S::truncToInt(uf);
  const auto top = S::seti(static_cast<int32_t>(v.levels) - 1);
  const auto lo = S::mini(S::maxi(k, S::seti(0)), top);
  const auto hi = S::mini(S::maxi(S::addi(k, S::seti(1)), S::seti(0)), top);
  // position in the level
  const auto pos = S::mul(S::sub(p, S::floor(p)), S::set(v.sizeF));
  const auto i = S::truncToInt(pos);
  const auto fr = S::sub(pos, S::toFloat(i));
  const auto stride = S::seti(static_cast<int32_t>(v.stride));
  const auto off = S::andi(i, S::seti(static_cast<int32_t>(v.size) - 1));
  const auto b0 = S::addi(S::muli(lo, stride), off);
  const auto b1 = S::addi(S::muli(hi, stride), off);
  typename S::F y0, y1, z0, z1;
  S::gatherPair(v.data, b1, z0, z1);
  const auto b = S::fma(fr, S::sub(z1, z0), z0);
  // Half of every half-octave reads the upper level alone (t = 1): with kSkip, skip the lower level's gathers when
  // no lane needs it (a steady mono pitch, vectorised over time, half the time). Lanes at t = 1 return b exactly
  // either way (fma(1, b − a, a) can differ from b by rounding), so the result does not depend on the lane mix.
  const auto upperOnly = S::ge(t, S::set(1.0f));
  if constexpr (kSkip)
    if (!S::any(S::bnot(upperOnly))) return b;
  S::gatherPair(v.data, b0, y0, y1);
  const auto a = S::fma(fr, S::sub(y1, y0), y0);
  return S::select(upperOnly, b, S::fma(t, S::sub(b, a), a));
}

struct WavetableK {
  static constexpr uint32_t kSpec = 1;  // phase
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<WavetableState*>(a.state);
    const auto& v = *static_cast<const WavetableView*>(a.resource);
    float* out = a.out[0];
    if constexpr (S::kW == 1 && !splBit(SM, 0)) {
      // Mono: the only state is the previous phase and delta, so vectorise over time: 8 deltas from a shifted
      // load, and the 8 previous deltas by rotating them one lane up (lane 0 takes the last delta before).
      const float* p = a.in[0];
      const uint32_t vecEnd = a.frames & ~7u;
      __m256 dLast = _mm256_set1_ps(st.prevDelta[0]);  // broadcast
      if (vecEnd > 0) {
        alignas(32) float first[8];
        first[0] = st.prevPhase[0];
        std::memcpy(first + 1, p, 7 * sizeof(float));
        const __m256i rot = _mm256_setr_epi32(7, 0, 1, 2, 3, 4, 5, 6);
        const __m256i last = _mm256_set1_epi32(7);
        for (uint32_t i = 0; i < vecEnd; i += 8) {
          const auto ph = S8::load(p + i);
          const auto d = phaseDelta<S8>(ph, i == 0 ? S8::load(first) : S8::load(p + i - 1));
          const auto dPrev = _mm256_blend_ps(_mm256_permutevar8x32_ps(d, rot), dLast, 0x01);
          S8::store(out + i, wavetableRead<S8, true>(v, ph, S8::min(d, dPrev)));
          dLast = _mm256_permutevar8x32_ps(d, last);
        }
      }
      auto dPrev1 = _mm256_castps256_ps128(dLast);
      for (uint32_t i = vecEnd; i < a.frames; ++i) {
        const auto prev = i == 0 ? S1::load(st.prevPhase) : S1::load(p + i - 1);
        const auto ph = S1::load(p + i);
        const auto d = phaseDelta<S1>(ph, prev);
        S1::store(out + i, wavetableRead<S1, true>(v, ph, S1::min(d, dPrev1)));
        dPrev1 = d;
      }
      if (a.frames > 0) {
        st.prevPhase[0] = p[a.frames - 1];
        S1::store(st.prevDelta, dPrev1);
      }
    } else {
      const In<S, splBit(SM, 0)> phase(a.in[0]);
      auto prev = S::load(st.prevPhase);
      auto prevD = S::load(st.prevDelta);
      for (uint32_t f = 0; f < a.frames; ++f) {
        const auto p = phase.at(f);
        const auto d = phaseDelta<S>(p, prev);
        S::store(out + f * S::kW, wavetableRead<S, S::kW == 1>(v, p, S::min(d, prevD)));
        prev = p;
        prevD = d;
      }
      S::store(st.prevPhase, prev);
      S::store(st.prevDelta, prevD);
    }
  }
};

// ---- noise ------------------------------------------------------------------------------------------------------

// lowbias32 (C. Wellons' hash prospector): a bijective 32-bit mixer with very low avalanche bias.
template <class S>
inline typename S::I lowbias32(typename S::I x) noexcept {  // RT-safe
  x = S::xori(x, S::template srli<16>(x));
  x = S::muli(x, S::seti(0x7feb352d));
  x = S::xori(x, S::template srli<15>(x));
  x = S::muli(x, S::seti(static_cast<int32_t>(0x846ca68bu)));
  x = S::xori(x, S::template srli<16>(x));
  return x;
}
template <class S>
inline typename S::F hashToUniform(typename S::I h) noexcept {  // RT-safe: [−1, 1) in steps of 2^-23
  return S::mul(S::toFloat(S::template srai<8>(h)), S::set(k2pm23));
}

struct NoiseK {
  static constexpr uint32_t kSpec = 0;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<NoiseState*>(a.state);
    float* out = a.out[0];
    const uint32_t c0 = st.counter;
    if constexpr (S::kW == 1) {
      const uint32_t vecEnd = a.frames & ~7u;
      auto c = S8::addi(S8::seti(static_cast<int32_t>(c0 + st.seed[0])), S8::laneIndex());
      for (uint32_t i = 0; i < vecEnd; i += 8) {
        S8::store(out + i, hashToUniform<S8>(lowbias32<S8>(c)));
        c = S8::addi(c, S8::seti(8));
      }
      for (uint32_t i = vecEnd; i < a.frames; ++i)
        S1::store(out + i, hashToUniform<S1>(lowbias32<S1>(S1::seti(static_cast<int32_t>(c0 + st.seed[0] + i)))));
    } else {
      auto c = S::addi(S::loadu32(st.seed), S::seti(static_cast<int32_t>(c0)));
      for (uint32_t f = 0; f < a.frames; ++f) {
        S::store(out + f * S::kW, hashToUniform<S>(lowbias32<S>(c)));
        c = S::addi(c, S::seti(1));
      }
    }
    st.counter = c0 + a.frames;
  }
};

// ---- shaper (a stateless table read, vectorised over time for L = 1) ----------------------------------------

template <class S>
inline typename S::F shaperRead(const ShaperView& v, typename S::F x) noexcept {  // RT-safe
  // max(t, 0) returns 0 for NaN.
  auto t = S::min(S::max(S::mul(S::sub(x, S::set(v.lo)), S::set(v.scale)), S::zero()), S::set(v.maxPos));
  const auto i = S::mini(S::truncToInt(t), S::seti(static_cast<int32_t>(v.segments) - 1));
  const auto fr = S::sub(t, S::toFloat(i));
  const auto y0 = S::gather(v.data, i);
  const auto y1 = S::gather(v.data, S::addi(i, S::seti(1)));
  return S::fma(fr, S::sub(y1, y0), y0);
}

struct ShaperOp {
  using View = ShaperView;
  template <class S>
  static typename S::F f(const View& v, typename S::F x) noexcept { return shaperRead<S>(v, x); }
};
template <class Op>
struct TableEw {
  static constexpr uint32_t kSpec = 1;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    const auto& v = *static_cast<const typename Op::View*>(a.resource);
    const uint32_t count = a.frames * S::kW;
    const uint32_t vecEnd = count & ~7u;
    float* out = a.out[0];
    const In<S8, splBit(SM, 0)> x(a.in[0]);
    for (uint32_t i = 0; i < vecEnd; i += 8) S8::store(out + i, Op::template f<S8>(v, x.at(i >> 3)));
    for (uint32_t i = vecEnd; i < count; ++i)
      S1::store(out + i, Op::template f<S1>(v, S1::load(splBit(SM, 0) ? a.in[0] : a.in[0] + i)));
  }
};

}  // namespace

KernelFn findGeneratorKernel(Atom a, uint32_t lanes, uint32_t m) noexcept {
  switch (a) {
    case Atom::Phasor: return pick<PhasorK<false>>(lanes, m);
    case Atom::SineHz: return pick<PhasorK<true>>(lanes, m);
    case Atom::Wavetable: return pick<WavetableK>(lanes, m);
    case Atom::Noise: return pick<NoiseK>(lanes, m);
    default: return nullptr;
  }
}

KernelFn findTableKernel(Atom a, uint32_t lanes, uint32_t m) noexcept {
  switch (a) {
    case Atom::Shaper: return pick<TableEw<ShaperOp>>(lanes, m);
    case Atom::TuningHz: return findTuningKernel(lanes);
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail
