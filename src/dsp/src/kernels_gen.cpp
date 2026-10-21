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
    // max(hz, −half) returns −half for NaN. At ±sr/2 the increment is ±2^31, the same phase step either way.
    const auto h = S::min(S::max(hz, S::set(-st.halfSr)), S::set(st.halfSr));
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

template <class S>
inline typename S::F wavetableRead(const WavetableView& v, typename S::F p, typename S::F prev) noexcept {  // RT-safe
  // |phase increment| in cycles, wrapped to [0, 0.5].
  auto d = S::sub(p, prev);
  d = S::abs(S::sub(d, S::round(d)));
  // level = ceil(log2(N·inc)) from the float bits: exponent, +1 if the mantissa is non-zero.
  const auto bits = S::asI(S::mul(d, S::set(v.sizeF)));
  auto lvl = S::subi(S::template srli<23>(bits), S::seti(127));
  const auto mant = S::andi(bits, S::seti(0x7FFFFF));
  lvl = S::subi(lvl, S::xori(S::eqi(mant, S::seti(0)), S::seti(-1)));  // + 1 where mantissa ≠ 0
  lvl = S::mini(S::maxi(lvl, S::seti(0)), S::seti(static_cast<int32_t>(v.levels) - 1));
  // position in the level
  const auto pos = S::mul(S::sub(p, S::floor(p)), S::set(v.sizeF));
  const auto i = S::truncToInt(pos);
  const auto fr = S::sub(pos, S::toFloat(i));
  const auto base = S::addi(S::muli(lvl, S::seti(static_cast<int32_t>(v.stride))),
                            S::andi(i, S::seti(static_cast<int32_t>(v.size) - 1)));
  const auto y0 = S::gather(v.data, base);
  const auto y1 = S::gather(v.data, S::addi(base, S::seti(1)));
  return S::fma(fr, S::sub(y1, y0), y0);
}

struct WavetableK {
  static constexpr uint32_t kSpec = 1;  // phase
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<WavetableState*>(a.state);
    const auto& v = *static_cast<const WavetableView*>(a.resource);
    float* out = a.out[0];
    if constexpr (S::kW == 1 && !splBit(SM, 0)) {
      // Mono: the only "state" is the previous input sample, so vectorise over time with a shifted load.
      const float* p = a.in[0];
      const uint32_t vecEnd = a.frames & ~7u;
      if (vecEnd > 0) {
        alignas(32) float first[8];
        first[0] = st.prevPhase[0];
        std::memcpy(first + 1, p, 7 * sizeof(float));
        S8::store(out, wavetableRead<S8>(v, S8::load(p), S8::load(first)));
        for (uint32_t i = 8; i < vecEnd; i += 8)
          S8::store(out + i, wavetableRead<S8>(v, S8::load(p + i), S8::load(p + i - 1)));
      }
      for (uint32_t i = vecEnd; i < a.frames; ++i) {
        const auto prev = i == 0 ? S1::load(st.prevPhase) : S1::load(p + i - 1);
        S1::store(out + i, wavetableRead<S1>(v, S1::load(p + i), prev));
      }
      if (a.frames > 0) st.prevPhase[0] = p[a.frames - 1];
    } else {
      const In<S, splBit(SM, 0)> phase(a.in[0]);
      auto prev = S::load(st.prevPhase);
      for (uint32_t f = 0; f < a.frames; ++f) {
        const auto p = phase.at(f);
        S::store(out + f * S::kW, wavetableRead<S>(v, p, prev));
        prev = p;
      }
      S::store(st.prevPhase, prev);
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

// Splits x (clamped to ±2^20) into a 32.32 fixed-point value: integer part floor(x) and fraction·2^32.
// Exact for the fraction bits a float can hold down to 2^-32 (the fraction of a float is exact; it is
// converted in two exact 16-bit halves because AVX2 has no float → uint32 conversion).
template <class S>
inline void toFixed(typename S::F x, typename S::I& hi, typename S::I& lo) noexcept {  // RT-safe
  x = S::min(S::max(x, S::set(-1048576.0f)), S::set(1048576.0f));  // NaN → −2^20
  const auto fl = S::floor(x);
  hi = S::truncToInt(fl);
  const auto f16 = S::mul(S::sub(x, fl), S::set(65536.0f));
  const auto f16i = S::floor(f16);
  const auto lo16 = S::truncToInt(S::mul(S::sub(f16, f16i), S::set(65536.0f)));
  lo = S::addi(S::template slli<16>(S::truncToInt(f16i)), lo16);
}

template <bool kCubic>
struct SampleReadK {
  static constexpr uint32_t kSpec = 2;  // rate, reset (start, loopStart, loopEnd: Spl only)
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
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
    const auto clampedFloor = [](typename S::F x) noexcept {
      return S::truncToInt(S::floor(S::min(S::max(x, S::set(-1048576.0f * 1024.0f)), S::set(2147483520.0f))));
    };
    const I loopStart = clampedFloor(S::load(a.in[3]));
    const I loopEnd = clampedFloor(S::load(a.in[4]));
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
      S::store(out + f * S::kW, S::band(active, sampleInterp<S, kCubic>(v.data, i, fr)));
      S::store(playing + f * S::kW, active);
      // Advance the 32.32 playhead: unsigned carry out of the fraction via a sign-flipped signed compare.
      if constexpr (!splBit(SM, 0)) toFixed<S>(rate.at(f), incHi, incLo);
      const I newFrac = S::addi(frac, incLo);
      const I carry = S::gti(S::xori(frac, signBit), S::xori(newFrac, signBit));  // −1 on carry
      I next = S::subi(S::addi(index, incHi), carry);
      // Loops: crossing loopEnd forwards / loopStart backwards (masks are −1 where true).
      const I crossFwd = S::andi(S::andi(loopOn, S::gti(loopEnd, index)), S::gti(next, S::subi(loopEnd, S::seti(1))));
      const I crossBwd = S::andi(S::andi(loopOn, S::gti(index, S::subi(loopStart, S::seti(1)))), S::gti(loopStart, next));
      next = S::selecti(crossFwd, S::subi(next, loopLen), next);
      next = S::selecti(crossBwd, S::addi(next, loopLen), next);
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
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail
