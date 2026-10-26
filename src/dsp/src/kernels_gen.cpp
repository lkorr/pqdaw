// Generator and table atoms (SPEC §7.2): phasor, sine_hz, wavetable, noise, shaper, sampleRead.
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

// ---- shaper and sample_pos (stateless table reads, vectorised over time for L = 1) ---------------------------

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

template <class S, bool kCubic>
inline typename S::F sampleInterp(const float* data, typename S::I i, typename S::F fr) noexcept {  // RT-safe
  const auto x0 = S::gather(data, i);
  const auto x1 = S::gather(data, S::addi(i, S::seti(1)));
  if constexpr (kCubic) {
    const auto xm1 = S::gather(data, S::subi(i, S::seti(1)));
    const auto x2 = S::gather(data, S::addi(i, S::seti(2)));
    const auto half = S::set(0.5f);
    const auto c1 = S::mul(half, S::sub(x1, xm1));
    const auto c2 = S::add(S::fnma(S::set(2.5f), x0, xm1), S::fnma(half, x2, S::add(x1, x1)));
    const auto c3 = S::fma(half, S::sub(x2, xm1), S::mul(S::set(1.5f), S::sub(x0, x1)));
    return S::fma(S::fma(S::fma(c3, fr, c2), fr, c1), fr, x0);
  } else {
    return S::fma(fr, S::sub(x1, x0), x0);
  }
}

// sample_read's interpolation inside a loop (M1 review L3): while the playhead's sample i is in [loopStart, loopEnd),
// the taps past the seam come from the other end of the loop (i + 1, i + 2 ≥ loopEnd wrap back by the loop length;
// the cubic's i − 1 < loopStart wraps forward), so a loop plays seamlessly instead of reading the samples stored
// after loopEnd. Taps are then clamped to [−1, length] (the zero guards: a loop reaching past the data reads 0
// there, as an unlooped read does), which also keeps every gather inside the guarded buffer whatever the loop
// points are. Two wraps for i + 2 cover a one-sample loop.
// One wrap of a tap past the loop end (lanes inside the loop only), and the clamp to the zero guards.
template <class S>
inline typename S::I wrapTapDown(typename S::I t, typename S::I inLoop, typename S::I lastIn,
                                 typename S::I loopLen) noexcept {  // RT-safe
  return S::selecti(S::andi(inLoop, S::gti(t, lastIn)), S::subi(t, loopLen), t);
}
template <class S>
inline typename S::I clampTap(typename S::I t, typename S::I length) noexcept {  // RT-safe
  return S::mini(S::maxi(t, S::seti(-1)), length);
}

template <class S, bool kCubic>
inline typename S::F sampleInterpLoop(const float* data, typename S::I i, typename S::F fr, typename S::I loopOn,
                                      typename S::I loopStart, typename S::I loopEnd, typename S::I loopLen,
                                      typename S::I length) noexcept {  // RT-safe
  using I = typename S::I;
  const I one = S::seti(1);
  const I inLoop = S::andi(loopOn, S::andi(S::gti(i, S::subi(loopStart, one)), S::gti(loopEnd, i)));
  const I lastIn = S::subi(loopEnd, one);
  const auto x0 = S::gather(data, i);
  const auto x1 = S::gather(data, clampTap<S>(wrapTapDown<S>(S::addi(i, one), inLoop, lastIn, loopLen), length));
  if constexpr (kCubic) {
    I tm1 = S::subi(i, one);
    tm1 = S::selecti(S::andi(inLoop, S::gti(loopStart, tm1)), S::addi(tm1, loopLen), tm1);
    const auto xm1 = S::gather(data, clampTap<S>(tm1, length));
    const I t2 = wrapTapDown<S>(wrapTapDown<S>(S::addi(i, S::seti(2)), inLoop, lastIn, loopLen), inLoop, lastIn, loopLen);
    const auto x2 = S::gather(data, clampTap<S>(t2, length));
    const auto half = S::set(0.5f);
    const auto c1 = S::mul(half, S::sub(x1, xm1));
    const auto c2 = S::add(S::fnma(S::set(2.5f), x0, xm1), S::fnma(half, x2, S::add(x1, x1)));
    const auto c3 = S::fma(half, S::sub(x2, xm1), S::mul(S::set(1.5f), S::sub(x0, x1)));
    return S::fma(S::fma(S::fma(c3, fr, c2), fr, c1), fr, x0);
  } else {
    return S::fma(fr, S::sub(x1, x0), x0);
  }
}

template <class S, bool kCubic>
inline typename S::F samplePosRead(const SampleView& v, typename S::F pos) noexcept {  // RT-safe
  const auto valid = S::band(S::ge(pos, S::zero()), S::lt(pos, S::toFloat(S::seti(v.length))));
  const auto fl = S::floor(pos);
  // Clamp the index into [0, length − 1] so the gather stays inside the guarded buffer (NaN → 0).
  const auto i = S::mini(S::maxi(S::truncToInt(fl), S::seti(0)), S::seti(v.length - 1));
  const auto y = sampleInterp<S, kCubic>(v.data, i, S::sub(pos, fl));
  return S::band(valid, y);
}

struct ShaperOp {
  using View = ShaperView;
  template <class S>
  static typename S::F f(const View& v, typename S::F x) noexcept { return shaperRead<S>(v, x); }
};
template <bool kCubic>
struct SamplePosOp {
  using View = SampleView;
  template <class S>
  static typename S::F f(const View& v, typename S::F x) noexcept { return samplePosRead<S, kCubic>(v, x); }
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

// ---- sample_read --------------------------------------------------------------------------------------------

// Splits x (clamped to ±2^20) into a 32.32 fixed-point value: integer part floor(x) and floor(fraction·2^32), i.e.
// floor(x·2^32) exactly (bits below 2^-32 are dropped). For x ≥ 0 or x ≤ −0.5 the fraction x − floor(x) is an
// exact float and is converted in two exact 16-bit halves (AVX2 has no float → uint32 conversion). For
// −0.5 < x < 0 the fraction 1 + x needs more than 24 bits: it used to round, up to 1.0 for x ≥ −2^−25, and a
// tiny negative rate then moved the playhead a whole sample backwards per sample (M1 review M11a). There
// x·2^32 is an exact float in (−2^31, 0), and floor(x·2^32) as int32 is the fraction mod 2^32 (with hi = −1).
template <class S>
inline void toFixed(typename S::F x, typename S::I& hi, typename S::I& lo) noexcept {  // RT-safe
  x = S::min(S::max(x, S::set(-1048576.0f)), S::set(1048576.0f));  // NaN → −2^20
  const auto fl = S::floor(x);
  hi = S::truncToInt(fl);
  const auto f16 = S::mul(S::sub(x, fl), S::set(65536.0f));
  const auto f16i = S::floor(f16);
  const auto lo16 = S::truncToInt(S::mul(S::sub(f16, f16i), S::set(65536.0f)));
  lo = S::addi(S::template slli<16>(S::truncToInt(f16i)), lo16);
  const auto smallNeg = S::band(S::lt(x, S::zero()), S::gt(x, S::set(-0.5f)));
  lo = S::selecti(S::asI(smallNeg), S::truncToInt(S::floor(S::mul(x, S::set(4294967296.0f)))), lo);
}

// e mod len for 0 ≤ e ≤ 2^20 + 1 and len ≥ 1 (len given as int32; a loop longer than 2^31 wraps negative and is
// treated as "longer than e"). Clamping len to 2^22 > e does not change the result, and keeps both operands exact
// in float; the float quotient is at most one above floor(e/len) (rounding is monotonic and integers are exact),
// which one correction fixes.
template <class S>
inline typename S::I smallMod(typename S::I e, typename S::I len) noexcept {  // RT-safe
  const auto cap = S::seti(1 << 22);
  const auto l = S::selecti(S::gti(len, S::seti(0)), S::mini(len, cap), cap);
  const auto q = S::truncToInt(S::floor(S::div(S::toFloat(e), S::toFloat(l))));
  auto m = S::subi(e, S::muli(q, l));
  m = S::addi(m, S::andi(S::gti(S::seti(0), m), l));
  return m;
}

template <bool kCubic>
struct SampleReadK {
  static constexpr uint32_t kSpec = 2;  // rate, reset (start, loopStart, loopEnd: Spl only)
  // Two frame loops: with the loop-seam taps (some lane loops) and without (none does), chosen per block, so an
  // unlooped read pays nothing for the seam handling.
  template <class S>
  static typename S::I clampedFloor(typename S::F x) noexcept {  // RT-safe
    return S::truncToInt(S::floor(S::min(S::max(x, S::set(-1048576.0f * 1024.0f)), S::set(2147483520.0f))));
  }
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    if (S::any(S::asF(S::gti(clampedFloor<S>(S::load(a.in[4])), clampedFloor<S>(S::load(a.in[3]))))))
      runFrames<S, SM, true>(a);
    else
      runFrames<S, SM, false>(a);
  }
  template <class S, uint32_t SM, bool kLoop>
  static void runFrames(const KernelArgs& a) noexcept {  // RT-safe
    using I = typename S::I;
    auto& st = *static_cast<SampleReadState*>(a.state);
    const auto& v = *static_cast<const SampleView*>(a.resource);
    const In<S, splBit(SM, 0)> rate(a.in[0]);
    const In<S, splBit(SM, 1)> reset(a.in[1]);
    float* out = a.out[0];
    float* playing = a.out[1];

    // Block-constant parameters.
    I startHi, startLo;
    toFixed<S>(S::load(a.in[2]), startHi, startLo);
    const I loopStart = clampedFloor<S>(S::load(a.in[3]));
    const I loopEnd = clampedFloor<S>(S::load(a.in[4]));
    const I loopOn = S::gti(loopEnd, loopStart);
    const I loopLen = S::subi(loopEnd, loopStart);
    const I length = S::seti(v.length);
    const I zero = S::seti(0);
    const I lastIndex = S::seti(v.length - 1);
    const I signBit = S::seti(static_cast<int32_t>(0x80000000u));
    const auto startValid = S::band(S::asF(S::gti(startHi, S::seti(-1))), S::asF(S::gti(length, startHi)));
    const bool resetActive = splBit(SM, 1) ? S::any(reset.at(0)) : blockAny(a.in[1], a.frames * S::kW);

    I index = S::loadi(st.index);
    I frac = S::loadu32(st.frac);
    auto active = S::load(st.active);
    I incHi = zero, incLo = zero;
    if constexpr (splBit(SM, 0)) toFixed<S>(rate.at(0), incHi, incLo);

    for (uint32_t f = 0; f < a.frames; ++f) {
      if (resetActive) {
        const auto r = reset.at(f);
        const I ri = S::asI(r);
        index = S::selecti(ri, startHi, index);
        frac = S::selecti(ri, startLo, frac);
        active = S::select(r, startValid, active);
      }
      // Read (index clamped for memory safety; inactive lanes output 0).
      const I i = S::mini(S::maxi(index, zero), lastIndex);
      const auto fr = S::mul(S::toFloat(S::template srli<8>(frac)), S::set(k2pm24));
      if constexpr (kLoop)
        S::store(out + f * S::kW,
                 S::band(active, sampleInterpLoop<S, kCubic>(v.data, i, fr, loopOn, loopStart, loopEnd, loopLen, length)));
      else
        S::store(out + f * S::kW, S::band(active, sampleInterp<S, kCubic>(v.data, i, fr)));
      S::store(playing + f * S::kW, active);
      // Advance the 32.32 playhead: unsigned carry out of the fraction via a sign-flipped signed compare.
      if constexpr (!splBit(SM, 0)) toFixed<S>(rate.at(f), incHi, incLo);
      const I newFrac = S::addi(frac, incLo);
      const I carry = S::gti(S::xori(frac, signBit), S::xori(newFrac, signBit));  // −1 on carry
      I next = S::subi(S::addi(index, incHi), carry);
      // Loops: crossing loopEnd forwards / loopStart backwards (masks are −1 where true) wraps into the loop
      // modulo its length: next = loopStart + (next − loopEnd) mod len forwards, loopEnd − 1 − (loopStart − 1 −
      // next) mod len backwards. (A single ±len jump left the loop for good when |rate| > len: M1 review M11b.)
      const I crossFwd = S::andi(S::andi(loopOn, S::gti(loopEnd, index)), S::gti(next, S::subi(loopEnd, S::seti(1))));
      const I crossBwd = S::andi(S::andi(loopOn, S::gti(index, S::subi(loopStart, S::seti(1)))), S::gti(loopStart, next));
      const I cross = S::xori(crossFwd, crossBwd);  // never both (fwd needs next ≥ loopEnd > loopStart > next)
      if (S::any(S::asF(cross))) {
        const I one = S::seti(1);
        const I e = S::selecti(crossFwd, S::subi(next, loopEnd), S::subi(S::subi(loopStart, one), next));
        const I m = smallMod<S>(e, loopLen);
        const I wrapped = S::selecti(crossFwd, S::addi(loopStart, m), S::subi(S::subi(loopEnd, one), m));
        next = S::selecti(cross, wrapped, next);
      }
      // Leaving [0, length) stops playback; a stopped playhead stays where it was.
      const auto inRange = S::asF(S::andi(S::gti(next, S::seti(-1)), S::gti(length, next)));
      const I adv = S::asI(active);
      index = S::selecti(adv, next, index);
      frac = S::selecti(adv, newFrac, frac);
      active = S::band(active, inRange);
    }
    S::storei(st.index, index);
    S::storeu32(st.frac, frac);
    S::store(st.active, active);
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
    case Atom::SamplePosLinear: return pick<TableEw<SamplePosOp<false>>>(lanes, m);
    case Atom::SamplePosCubic: return pick<TableEw<SamplePosOp<true>>>(lanes, m);
    case Atom::SampleReadLinear: return pick<SampleReadK<false>>(lanes, m);
    case Atom::SampleReadCubic: return pick<SampleReadK<true>>(lanes, m);
    case Atom::TuningHz: return findTuningKernel(lanes);
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail
