// The sample atoms (SM.2; semantics in pq/dsp/atoms.h "Tables", data in pq/dsp/sample_slot.h): sample_read_linear /
// _cubic, sample_pos_linear / _cubic, sample_slice, sample_scrub — every one reads a SampleSlot.

#include <cmath>
#include <cstring>

#include "kernels_common.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/sample_slot.h"

namespace pq::dsp::detail {
namespace {

constexpr float k2pm24 = 5.9604644775390625e-08f;  // 2^-24
constexpr float kHalfPi = 1.5707963267948966f;

using Entry = SampleSlot::Entry;

// ---- interpolation --------------------------------------------------------------------------------------------

template <class S, bool kCubic>
inline typename S::F interp4(typename S::F xm1, typename S::F x0, typename S::F x1, typename S::F x2,
                             typename S::F fr) noexcept {  // RT-safe
  if constexpr (kCubic) {
    const auto half = S::set(0.5f);
    const auto c1 = S::mul(half, S::sub(x1, xm1));
    const auto c2 = S::add(S::fnma(S::set(2.5f), x0, xm1), S::fnma(half, x2, S::add(x1, x1)));
    const auto c3 = S::fma(half, S::sub(x2, xm1), S::mul(S::set(1.5f), S::sub(x0, x1)));
    return S::fma(S::fma(S::fma(c3, fr, c2), fr, c1), fr, x0);
  } else {
    (void)xm1;
    (void)x2;
    return S::fma(fr, S::sub(x1, x0), x0);
  }
}

// Taps i − 1 … i + 2 of `data` (unclamped: i in [0, length − 1], the guards hold the rest).
template <class S, bool kCubic>
inline typename S::F sampleInterp(const float* data, typename S::I i, typename S::F fr) noexcept {  // RT-safe
  const auto x0 = S::gather(data, i);
  const auto x1 = S::gather(data, S::addi(i, S::seti(1)));
  if constexpr (kCubic) {
    const auto xm1 = S::gather(data, S::subi(i, S::seti(1)));
    const auto x2 = S::gather(data, S::addi(i, S::seti(2)));
    return interp4<S, kCubic>(xm1, x0, x1, x2, fr);
  } else {
    return interp4<S, kCubic>(x0, x0, x1, x1, fr);
  }
}

// The tap positions of a read at sample i (M1 review L3, SM.2 ping-pong): while i is inside the loop, the taps past the
// seam come from the other end of the loop (forward lanes: i + 1, i + 2 ≥ loopEnd wrap back by the loop length, i − 1 <
// loopStart forward) or are mirrored at the turning points (ping-pong lanes: t > loopEnd − 1 → 2(loopEnd − 1) − t, t <
// loopStart → 2·loopStart − t), so a loop plays seamlessly; then clamped to [−1, length] (the zero guards).
struct Taps {
  enum Mode : int { None = 0, Loop = 1, PingPong = 2 };
};
template <class S>
inline typename S::I clampTap(typename S::I t, typename S::I length) noexcept {  // RT-safe
  return S::mini(S::maxi(t, S::seti(-1)), length);
}
template <class S>
struct LoopTaps {
  using I = typename S::I;
  I inLoop, pp, loopStart, lastIn, loopLen, length;
  I up(I t) const noexcept {  // a tap above i
    const I past = S::andi(inLoop, S::gti(t, lastIn));
    const I wrapped = S::selecti(pp, S::subi(S::addi(lastIn, lastIn), t), S::subi(t, loopLen));
    return S::selecti(past, wrapped, t);
  }
  I down(I t) const noexcept {  // a tap below i
    const I before = S::andi(inLoop, S::gti(loopStart, t));
    const I wrapped = S::selecti(pp, S::subi(S::addi(loopStart, loopStart), t), S::addi(t, loopLen));
    return S::selecti(before, wrapped, t);
  }
};
template <class S, bool kCubic, int kTap>
inline typename S::F sampleInterpLoop(const float* data, typename S::I i, typename S::F fr,
                                      const LoopTaps<S>& lt) noexcept {  // RT-safe
  using I = typename S::I;
  const I one = S::seti(1);
  const auto x0 = S::gather(data, i);
  const auto x1 = S::gather(data, clampTap<S>(lt.up(S::addi(i, one)), lt.length));
  if constexpr (kCubic) {
    const auto xm1 = S::gather(data, clampTap<S>(lt.down(S::subi(i, one)), lt.length));
    I t2 = lt.up(S::addi(i, S::seti(2)));
    if constexpr (kTap == Taps::Loop) t2 = lt.up(t2);  // a one-sample loop wraps i + 2 twice
    else t2 = lt.down(t2);                              // (a mirror past both ends of a one-sample loop)
    const auto x2 = S::gather(data, clampTap<S>(t2, lt.length));
    return interp4<S, kCubic>(xm1, x0, x1, x2, fr);
  } else {
    return interp4<S, kCubic>(x0, x0, x1, x1, fr);
  }
}

// ---- 32.32 fixed point ----------------------------------------------------------------------------------------------

// Splits x (clamped to ±2^20) into a 32.32 fixed-point value: integer part floor(x) and floor(fraction·2^32), i.e.
// floor(x·2^32) exactly (bits below 2^-32 are dropped). For x ≥ 0 or x ≤ −0.5 the fraction x − floor(x) is an exact
// float and is converted in two exact 16-bit halves (AVX2 has no float → uint32 conversion). For −0.5 < x < 0 the
// fraction 1 + x needs more than 24 bits: it used to round, up to 1.0 for x ≥ −2^−25, and a tiny negative rate then
// moved the playhead a whole sample backwards per sample (M1 review M11a). There x·2^32 is an exact float in (−2^31,
// 0), and floor(x·2^32) as int32 is the fraction mod 2^32 (with hi = −1).
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

// A start position (frames, possibly beyond ±2^20: a long sample) as 32.32: floor and the fraction's top bits.
template <class S>
inline void toFixedWide(typename S::F x, typename S::I& hi, typename S::I& lo) noexcept {  // RT-safe
  x = S::min(S::max(S::band(S::eq(x, x), x), S::set(-1.0e9f)), S::set(2.0e9f));  // NaN → 0
  const auto fl = S::floor(x);
  hi = S::truncToInt(fl);
  const auto f16 = S::mul(S::sub(x, fl), S::set(65536.0f));
  const auto f16i = S::floor(f16);
  const auto lo16 = S::truncToInt(S::mul(S::sub(f16, f16i), S::set(65536.0f)));
  lo = S::addi(S::template slli<16>(S::truncToInt(f16i)), lo16);
}

// e mod len for 0 ≤ e ≤ 2^20 + 1 and len ≥ 1 (len given as int32; a loop longer than 2^31 wraps negative and is
// treated as "longer than e"). Clamping len to 2^22 > e does not change the result, and keeps both operands exact in
// float; the float quotient is at most one above floor(e/len) (rounding is monotonic and integers are exact), which
// one correction fixes.
template <class S>
inline typename S::I smallMod(typename S::I e, typename S::I len) noexcept {  // RT-safe
  const auto cap = S::seti(1 << 22);
  const auto l = S::selecti(S::gti(len, S::seti(0)), S::mini(len, cap), cap);
  const auto q = S::truncToInt(S::floor(S::div(S::toFloat(e), S::toFloat(l))));
  auto m = S::subi(e, S::muli(q, l));
  m = S::addi(m, S::andi(S::gti(S::seti(0), m), l));
  return m;
}

template <class S>
inline typename S::I clampedFloor(typename S::F x) noexcept {  // RT-safe
  return S::truncToInt(S::floor(S::min(S::max(x, S::set(-1048576.0f * 1024.0f)), S::set(2147483520.0f))));
}

// ---- sample_read ----------------------------------------------------------------------------------------------------

// The block-constant values of the lanes, from the inputs and each lane's entry (its length, its file rate): positions
// in frames (units 1: × the lane's length), fades in frames (units 1: seconds × the lane's file rate).
template <class S>
struct LaneConsts {
  using F = typename S::F;
  using I = typename S::I;
  I startHi, startLo, loopStart, loopEnd, loopOn, loopLen, lastIn, endEff, length, lastIndex;
  F startValid, scale, fadeF, fadeInv, fiInv, foInv, endF, startF, posMul;
  I pp;

  static LaneConsts make(const KernelArgs& a, I len, F fileRate, F rateScale, bool units) noexcept {
    LaneConsts c;
    const F lenF = S::toFloat(len);
    const F posScale = units ? lenF : S::set(1.0f);
    const F timeScale = units ? fileRate : S::set(1.0f);
    const F start = S::mul(S::load(a.in[2]), posScale);
    toFixedWide<S>(start, c.startHi, c.startLo);
    c.startF = start;
    c.loopStart = clampedFloor<S>(S::mul(S::load(a.in[3]), posScale));
    c.loopEnd = clampedFloor<S>(S::mul(S::load(a.in[4]), posScale));
    c.loopOn = S::gti(c.loopEnd, c.loopStart);
    c.loopLen = S::subi(c.loopEnd, c.loopStart);
    c.lastIn = S::subi(c.loopEnd, S::seti(1));
    c.length = len;
    c.lastIndex = S::subi(len, S::seti(1));
    c.endEff = S::mini(clampedFloor<S>(S::mul(S::load(a.in[7]), posScale)), len);
    c.endF = S::toFloat(c.endEff);
    c.pp = S::andi(c.loopOn, S::asI(S::gt(S::load(a.in[6]), S::set(0.5f))));
    // The loop crossfade: forward loops only, at most the loop's length and the frames before loopStart.
    const I fade = S::maxi(clampedFloor<S>(S::mul(S::load(a.in[5]), timeScale)), S::seti(0));
    const I fmax = S::mini(c.loopLen, S::maxi(c.loopStart, S::seti(0)));
    const I fe = S::andi(S::andi(c.loopOn, S::xori(c.pp, S::seti(-1))), S::mini(fade, fmax));
    c.fadeF = S::toFloat(fe);
    // (The divisions only where a lane needs them: the per-call setup is a visible part of a param-rate read.)
    c.fadeInv = S::zero();
    if (S::any(S::gt(c.fadeF, S::zero())))
      c.fadeInv = S::select(S::gt(c.fadeF, S::zero()), S::div(S::set(kHalfPi), S::max(c.fadeF, S::set(1.0f))), S::zero());
    // Fade in / out (from start, to the end): the inverse lengths (0 → none: gain 1).
    const F fi = S::mul(S::load(a.in[8]), timeScale), fo = S::mul(S::load(a.in[9]), timeScale);
    c.fiInv = c.foInv = S::zero();
    if (S::any(S::bor(S::gt(fi, S::zero()), S::gt(fo, S::zero())))) {
      c.fiInv = S::select(S::gt(fi, S::zero()), S::div(S::set(1.0f), S::max(fi, S::set(1e-30f))), S::zero());
      c.foInv = S::select(S::gt(fo, S::zero()), S::div(S::set(1.0f), S::max(fo, S::set(1e-30f))), S::zero());
    }
    c.startValid = S::band(S::asF(S::gti(c.startHi, S::seti(-1))), S::asF(S::gti(c.endEff, c.startHi)));
    c.scale = rateScale;
    c.posMul = units ? S::div(S::set(1.0f), S::max(lenF, S::set(1.0f))) : S::set(1.0f);
    return c;
  }
  static LaneConsts pick(F m, const LaneConsts& a, const LaneConsts& b) noexcept {  // m ? a : b
    LaneConsts c;
    const I mi = S::asI(m);
    auto si = [&](I x, I y) { return S::selecti(mi, x, y); };
    auto sf = [&](F x, F y) { return S::select(m, x, y); };
    c.startHi = si(a.startHi, b.startHi), c.startLo = si(a.startLo, b.startLo), c.loopStart = si(a.loopStart, b.loopStart);
    c.loopEnd = si(a.loopEnd, b.loopEnd), c.loopOn = si(a.loopOn, b.loopOn), c.loopLen = si(a.loopLen, b.loopLen);
    c.lastIn = si(a.lastIn, b.lastIn), c.endEff = si(a.endEff, b.endEff), c.length = si(a.length, b.length);
    c.lastIndex = si(a.lastIndex, b.lastIndex), c.pp = si(a.pp, b.pp);
    c.startValid = sf(a.startValid, b.startValid), c.scale = sf(a.scale, b.scale), c.fadeF = sf(a.fadeF, b.fadeF);
    c.fadeInv = sf(a.fadeInv, b.fadeInv), c.fiInv = sf(a.fiInv, b.fiInv), c.foInv = sf(a.foInv, b.foInv);
    c.endF = sf(a.endF, b.endF), c.startF = sf(a.startF, b.startF), c.posMul = sf(a.posMul, b.posMul);
    return c;
  }
};

// The playhead (32.32 in level-0 frames) in frames of the 2^L× decimated copy.
// Returns the fraction; the index is iL = index >> L (arithmetic).
template <class S, int L>
inline typename S::I levelFrac(typename S::I index, typename S::I frac) noexcept {
  const auto low = S::subi(index, S::template slli<L>(S::template srai<L>(index)));
  return S::addi(S::template slli<32 - L>(low), S::template srli<L>(frac));
}

// One entry the call reads and the lanes it serves.
struct Group {
  const Entry* e;
  uint32_t level;  // mip level of this call's reads
  uint32_t lanes;  // bit l: lane l reads this entry
};

template <class S>
inline typename S::I laneBitsToMask(uint32_t bits) noexcept {  // RT-safe: bit l → lane l all ones
  alignas(32) int32_t m[8];
  for (uint32_t l = 0; l < S::kW; ++l) m[l] = ((bits >> l) & 1u) != 0 ? -1 : 0;
  return S::loadi(m);
}

template <bool kCubic>
struct SampleReadK {
  static constexpr uint32_t kSpec = 2;  // rate, reset (the rest: Spl only)
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    using I = typename S::I;
    // Three frame loops: plain (no lane loops), forward loops (the seam taps), ping-pong (mirrored taps and the turns),
    // chosen per block, so an unlooped read pays nothing for the seam handling.
    const I loopStart = clampedFloor<S>(S::load(a.in[3]));
    const I loopEnd = clampedFloor<S>(S::load(a.in[4]));
    const bool units = a.in[10][0] >= 0.5f;
    const bool anyLoop = units ? S::any(S::lt(S::load(a.in[3]), S::load(a.in[4])))
                               : S::any(S::asF(S::gti(loopEnd, loopStart)));
    if (!anyLoop) runFrames<S, SM, Taps::None>(a);
    else if (S::any(S::gt(S::load(a.in[6]), S::set(0.5f)))) runFrames<S, SM, Taps::PingPong>(a);
    else runFrames<S, SM, Taps::Loop>(a);
  }

  // A decimated copy: position / 2^L, taps clamped to its frames (no loop seam handling at these rates).
  // (Not inlined: MSVC 17.14's optimiser crashed, C1001, inlining it into the frame loop; a mip read is the rare path.)
  template <class S>
  __declspec(noinline) static typename S::F readLevel(const float* data, int32_t length, uint32_t level, typename S::I index,
                                 typename S::I frac) noexcept {  // RT-safe
    using I = typename S::I;
    const I iL = level == 1 ? S::template srai<1>(index) : level == 2 ? S::template srai<2>(index) : S::template srai<3>(index);
    const I fL = level == 1 ? levelFrac<S, 1>(index, frac) : level == 2 ? levelFrac<S, 2>(index, frac) : levelFrac<S, 3>(index, frac);
    const auto fr = S::mul(S::toFloat(S::template srli<8>(fL)), S::set(k2pm24));
    const I i = S::mini(S::maxi(iL, S::seti(0)), S::seti(length - 1));
    return sampleInterp<S, kCubic>(data, i, fr);
  }

  // The read at the playhead from one entry at one level.
  template <class S, int kTap>
  static typename S::F read(const float* data, const SampleLevelView* lv, uint32_t level, typename S::I index,
                            typename S::I frac, const LaneConsts<S>& c, typename S::I loopOnNow) noexcept {
    using I = typename S::I;
    if (level == 0) {
      const I i = S::mini(S::maxi(index, S::seti(0)), c.lastIndex);
      const auto fr = S::mul(S::toFloat(S::template srli<8>(frac)), S::set(k2pm24));
      if constexpr (kTap == Taps::None) {
        (void)loopOnNow;
        return sampleInterp<S, kCubic>(data, i, fr);
      } else {
        const I inLoop = S::andi(loopOnNow, S::andi(S::gti(i, S::subi(c.loopStart, S::seti(1))), S::gti(c.loopEnd, i)));
        const LoopTaps<S> lt{inLoop, kTap == Taps::PingPong ? c.pp : S::seti(0), c.loopStart, c.lastIn, c.loopLen, c.length};
        return sampleInterpLoop<S, kCubic, kTap>(data, i, fr, lt);
      }
    }
    return readLevel<S>(data, lv->length, level, index, frac);
  }

  // One group's read (both channels), with the loop crossfade where a lane is inside it.
  template <class S, int kTap>
  static void readGroup(const Group& gr, typename S::I index, typename S::I frac, const LaneConsts<S>& c, bool stereo,
                        bool fadeOn, typename S::F& l, typename S::F& rr) noexcept {  // RT-safe
    using F = typename S::F;
    using I = typename S::I;
    const I zero = S::seti(0);
    const SampleLevelView* lv = &gr.e->level[gr.level];
    l = read<S, kTap>(lv->ch[0], lv, gr.level, index, frac, c, c.loopOn);
    rr = stereo && gr.e->channels > 1 ? read<S, kTap>(lv->ch[1], lv, gr.level, index, frac, c, c.loopOn) : l;
    if (!fadeOn) return;
    // The loop crossfade: inside the last `fade` frames before loopEnd.
    const I i = S::mini(S::maxi(index, zero), c.lastIndex);
    const F fr = S::mul(S::toFloat(S::template srli<8>(frac)), S::set(k2pm24));
    const F x = S::add(S::toFloat(S::subi(i, S::subi(c.loopEnd, S::truncToInt(c.fadeF)))), fr);
    const F inZone = S::band(S::band(S::asF(c.loopOn), S::gt(c.fadeF, S::zero())),
                             S::band(S::ge(x, S::zero()), S::asF(S::gti(c.loopEnd, i))));
    if (!S::any(inZone)) return;
    const I j = S::subi(index, c.loopLen);
    const F u = S::mul(x, c.fadeInv);                       // [0, π/2)
    const F gOut = S::fastSin(S::sub(S::set(kHalfPi), u));  // cos
    const F gIn = S::fastSin(u);
    const F l2 = read<S, Taps::None>(lv->ch[0], lv, gr.level, j, frac, c, zero);
    l = S::select(inZone, S::fma(gOut, l, S::mul(gIn, l2)), l);
    if (stereo && gr.e->channels > 1) {
      const F r2 = read<S, Taps::None>(lv->ch[1], lv, gr.level, j, frac, c, zero);
      rr = S::select(inZone, S::fma(gOut, rr, S::mul(gIn, r2)), rr);
    } else {
      rr = l;
    }
  }

  template <class S, uint32_t SM, int kTap, bool kStereo>
  static void runSimple(const KernelArgs& a, SampleReadState& st, const float* dl, const float* dr,
                        const LaneConsts<S>& cc, uint32_t newGen, bool resetActive) noexcept {  // RT-safe
    using F = typename S::F;
    using I = typename S::I;
    constexpr uint32_t W = S::kW;
    const In<S, splBit(SM, 0)> rate(a.in[0]);
    const In<S, splBit(SM, 1)> reset(a.in[1]);
    float* outL = a.out[0];
    float* playing = a.out[1];
    float* outR = a.out[2];
    float* posOut = a.out[3];
    const I startHi = cc.startHi, startLo = cc.startLo, loopStart = cc.loopStart, loopEnd = cc.loopEnd;
    const I loopOn = cc.loopOn, loopLen = cc.loopLen, lastIn = cc.lastIn, endEff = cc.endEff, length = cc.length;
    const I lastIndex = cc.lastIndex, pp = cc.pp;
    const F startValid = cc.startValid, scale = cc.scale, posMul = cc.posMul;
    const I zero = S::seti(0), one = S::seti(1), minusOne = S::seti(-1);
    const I signBit = S::seti(static_cast<int32_t>(0x80000000u));
    I index = S::loadi(st.index);
    I frac = S::loadu32(st.frac);
    F active = S::load(st.active);
    F back = S::load(st.back);
    I gen = S::loadu32(st.gen);
    const I newGenV = S::seti(static_cast<int32_t>(newGen));
    I incHi = zero, incLo = zero;
    if constexpr (splBit(SM, 0)) toFixed<S>(S::mul(rate.at(0), scale), incHi, incLo);
    for (uint32_t f = 0; f < a.frames; ++f) {
      if (resetActive) {
        const auto r = reset.at(f);
        const I ri = S::asI(r);
        gen = S::selecti(ri, newGenV, gen);
        index = S::selecti(ri, startHi, index);
        frac = S::selecti(ri, startLo, frac);
        active = S::select(r, startValid, active);
        if constexpr (kTap == Taps::PingPong) back = S::select(r, S::zero(), back);
      }
      const I i = S::mini(S::maxi(index, zero), lastIndex);
      const F fr = S::mul(S::toFloat(S::template srli<8>(frac)), S::set(k2pm24));
      F yl, yr;
      if constexpr (kTap == Taps::None) {
        yl = sampleInterp<S, kCubic>(dl, i, fr);
        yr = kStereo ? sampleInterp<S, kCubic>(dr, i, fr) : yl;
      } else {
        const I inLoop = S::andi(loopOn, S::andi(S::gti(i, S::subi(loopStart, one)), S::gti(loopEnd, i)));
        const LoopTaps<S> lt{inLoop, kTap == Taps::PingPong ? pp : zero, loopStart, lastIn, loopLen, length};
        yl = sampleInterpLoop<S, kCubic, kTap>(dl, i, fr, lt);
        yr = kStereo ? sampleInterpLoop<S, kCubic, kTap>(dr, i, fr, lt) : yl;
      }
      S::store(outL + f * W, S::band(active, yl));
      S::store(outR + f * W, S::band(active, yr));
      S::store(playing + f * W, active);
      S::store(posOut + f * W, S::mul(S::add(S::toFloat(index), fr), posMul));
      if constexpr (!splBit(SM, 0)) toFixed<S>(S::mul(rate.at(f), scale), incHi, incLo);
      I dHi = incHi, dLo = incLo;
      if constexpr (kTap == Taps::PingPong) {
        const I bi = S::asI(back);
        dHi = S::selecti(bi, S::addi(S::subi(zero, incHi), S::xori(S::eqi(incLo, zero), minusOne)), incHi);
        dLo = S::selecti(bi, S::subi(zero, incLo), incLo);
      }
      const I newFrac = S::addi(frac, dLo);
      const I carry = S::gti(S::xori(frac, signBit), S::xori(newFrac, signBit));
      I next = S::subi(S::addi(index, dHi), carry);
      I nextFrac = newFrac;
      if constexpr (kTap != Taps::None) {
        const I crossFwd = S::andi(S::andi(loopOn, S::gti(loopEnd, index)), S::gti(next, lastIn));
        const I crossBwd = S::andi(S::andi(loopOn, S::gti(index, S::subi(loopStart, one))), S::gti(loopStart, next));
        const I cross = S::xori(crossFwd, crossBwd);
        if (S::any(S::asF(cross))) {
          const I e = S::selecti(crossFwd, S::subi(next, loopEnd), S::subi(S::subi(loopStart, one), next));
          const I m = smallMod<S>(e, loopLen);
          I wrapped = S::selecti(crossFwd, S::addi(loopStart, m), S::subi(lastIn, m));
          I wrappedFrac = nextFrac;
          if constexpr (kTap == Taps::PingPong) {
            const I b2 = S::selecti(crossFwd, S::addi(lastIn, lastIn), S::addi(loopStart, loopStart));
            I refl = S::addi(S::subi(b2, next), S::xori(S::eqi(nextFrac, zero), minusOne));
            refl = S::mini(S::maxi(refl, loopStart), lastIn);
            wrapped = S::selecti(pp, refl, wrapped);
            wrappedFrac = S::selecti(pp, S::subi(zero, nextFrac), nextFrac);
            back = S::select(S::asF(S::andi(cross, pp)), S::bxor(back, S::asF(minusOne)), back);
          }
          next = S::selecti(cross, wrapped, next);
          nextFrac = S::selecti(cross, wrappedFrac, nextFrac);
        }
      }
      const auto inRange = S::asF(S::andi(S::gti(next, minusOne), S::gti(endEff, next)));
      const I adv = S::asI(active);
      index = S::selecti(adv, next, index);
      frac = S::selecti(adv, nextFrac, frac);
      active = S::band(active, inRange);
    }
    S::storei(st.index, index);
    S::storeu32(st.frac, frac);
    S::store(st.active, active);
    S::store(st.back, back);
    S::storeu32(st.gen, gen);
  }

  template <class S, uint32_t SM, int kTap>
  static void runFrames(const KernelArgs& a) noexcept {  // RT-safe
    using F = typename S::F;
    using I = typename S::I;
    constexpr uint32_t W = S::kW;
    auto& st = *static_cast<SampleReadState*>(a.state);
    const auto& slot = *static_cast<const SampleSlot*>(a.resource);
    const Entry* newest = slot.acquire();
    const uint32_t newGen = static_cast<uint32_t>(newest->generation);
    const In<S, splBit(SM, 0)> rate(a.in[0]);
    const In<S, splBit(SM, 1)> reset(a.in[1]);
    const bool units = a.in[10][0] >= 0.5f;
    float* outL = a.out[0];
    float* playing = a.out[1];
    float* outR = a.out[2];
    float* posOut = a.out[3];

    // The entries the lanes play (a lane whose entry is gone stops), one group each; group 0 is the newest.
    Group groups[W + 1];
    uint32_t nGroups = 1;
    groups[0] = {newest, 0, 0};
    alignas(32) int32_t lenA[8];
    alignas(32) float rateA[8], scaleA[8];
    alignas(32) float activeA[8];
    S::store(activeA, S::load(st.active));
    // Every lane on the newest entry (the common case): one compare instead of the per-lane walk.
    const bool allNewest = !S::any(S::bnot(S::asF(S::eqi(S::loadu32(st.gen), S::seti(static_cast<int32_t>(newGen))))));
    if (allNewest) groups[0].lanes = (1u << W) - 1u;
    for (uint32_t l = 0; l < W && !allNewest; ++l) {
      const Entry* e = newest;
      uint32_t g = 0;
      if (st.gen[l] != newGen) {
        if (!maskIsTrue(activeA[l])) {
          st.gen[l] = newGen;  // idle: it takes the newest at its next trigger anyway
        } else if ((e = SampleSlot::find(newest, st.gen[l])) == nullptr) {
          activeA[l] = 0.0f;  // its entry was retired (more than kSampleSlotMaxEntries swaps): silent
          st.gen[l] = newGen;
          e = newest;
        } else {
          for (g = 1; g < nGroups && groups[g].e != e; ++g) {}
          if (g == nGroups) groups[nGroups++] = {e, 0, 0};
          e->touched.store(1, std::memory_order_relaxed);  // still read: not retired yet
        }
      }
      groups[g].lanes |= 1u << l;
      lenA[l] = e->level[0].length;
      rateA[l] = static_cast<float>(e->fileRate);
      scaleA[l] = e->rateScale;
    }
    auto active = S::load(activeA);
    const LaneConsts<S> cur =
        allNewest ? LaneConsts<S>::make(a, S::seti(newest->level[0].length), S::set(static_cast<float>(newest->fileRate)),
                                        S::set(newest->rateScale), units)
                  : LaneConsts<S>::make(a, S::loadi(lenA), S::load(rateA), S::load(scaleA), units);
    const LaneConsts<S> fresh =
        nGroups == 1 ? cur
                     : LaneConsts<S>::make(a, S::seti(newest->level[0].length), S::set(static_cast<float>(newest->fileRate)),
                                           S::set(newest->rateScale), units);
    LaneConsts<S> c = cur;
    // Mip levels: per group, from the slowest |rate| of its playing lanes at the call's first frame (a faster lane
    // than that aliases a little more; a slower one is never dulled).
    {
      alignas(32) float r0[8];
      S::store(r0, S::abs(S::mul(rate.at(0), cur.scale)));
      for (uint32_t g = 0; g < nGroups; ++g) {
        if (groups[g].e->levels == 0) continue;  // (level 0)
        float slowest = 1e30f;
        for (uint32_t l = 0; l < W; ++l)
          if (((groups[g].lanes >> l) & 1u) != 0 && maskIsTrue(activeA[l])) slowest = std::fmin(slowest, r0[l]);
        groups[g].level = slowest < 1e29f ? mipLevelFor(slowest, groups[g].e->levels) : 0u;
      }
    }
    I gmask[W + 1];
    for (uint32_t g = 0; g < nGroups; ++g) gmask[g] = laneBitsToMask<S>(groups[g].lanes);
    const bool gainOn = S::any(S::bor(S::gt(cur.fiInv, S::zero()), S::gt(cur.foInv, S::zero()))) ||
                        (nGroups > 1 && S::any(S::bor(S::gt(fresh.fiInv, S::zero()), S::gt(fresh.foInv, S::zero()))));
    const bool fadeOn = kTap == Taps::Loop &&
                        (S::any(S::gt(cur.fadeF, S::zero())) || (nGroups > 1 && S::any(S::gt(fresh.fadeF, S::zero()))));
    const bool resetActive = splBit(SM, 1) ? S::any(reset.at(0)) : blockAny(a.in[1], a.frames * W);
    bool stereo = false;
    for (uint32_t g = 0; g < nGroups; ++g) stereo = stereo || groups[g].e->channels > 1;
    // The common case — every lane on one entry, read at the sample itself, no loop fade, no fades in / out — runs a
    // loop with every constant in registers (the general loop below costs ≈ 2× it: QUESTIONS Q?-SM.2-a, bench).
    if (nGroups == 1 && groups[0].level == 0 && !gainOn && !fadeOn) {
      S::store(st.active, active);
      const SampleLevelView& lv = newest->level[0];
      if (stereo) runSimple<S, SM, kTap, true>(a, st, lv.ch[0], lv.ch[1], cur, newGen, resetActive);
      else runSimple<S, SM, kTap, false>(a, st, lv.ch[0], lv.ch[0], cur, newGen, resetActive);
      return;
    }

    I index = S::loadi(st.index);
    I frac = S::loadu32(st.frac);
    F back = S::load(st.back);
    I gen = S::loadu32(st.gen);
    const I newGenV = S::seti(static_cast<int32_t>(newGen));
    const I zero = S::seti(0);
    const I one = S::seti(1);
    const I signBit = S::seti(static_cast<int32_t>(0x80000000u));
    I incHi = zero, incLo = zero;
    if constexpr (splBit(SM, 0)) toFixed<S>(S::mul(rate.at(0), c.scale), incHi, incLo);

    for (uint32_t f = 0; f < a.frames; ++f) {
      if (resetActive) {
        const auto r = reset.at(f);
        if (S::any(r)) {
          const I ri = S::asI(r);
          if (nGroups > 1) {  // a triggered lane moves to the newest entry
            c = LaneConsts<S>::pick(r, fresh, c);
            gmask[0] = S::selecti(ri, S::seti(-1), gmask[0]);
            for (uint32_t g = 1; g < nGroups; ++g) gmask[g] = S::selecti(ri, zero, gmask[g]);
            if constexpr (splBit(SM, 0)) toFixed<S>(S::mul(rate.at(0), c.scale), incHi, incLo);
          }
          gen = S::selecti(ri, newGenV, gen);
          index = S::selecti(ri, c.startHi, index);
          frac = S::selecti(ri, c.startLo, frac);
          active = S::select(r, c.startValid, active);
          back = S::select(r, S::zero(), back);
        }
      }
      // Read (inactive lanes output 0).
      F yl, yr;
      readGroup<S, kTap>(groups[0], index, frac, c, stereo, fadeOn, yl, yr);
      for (uint32_t g = 1; g < nGroups; ++g) {
        F l2, r2;
        readGroup<S, kTap>(groups[g], index, frac, c, stereo, fadeOn, l2, r2);
        const F m = S::asF(gmask[g]);
        yl = S::select(m, l2, yl);
        yr = S::select(m, r2, yr);
      }
      const auto fr = S::mul(S::toFloat(S::template srli<8>(frac)), S::set(k2pm24));
      const F x = S::add(S::toFloat(index), fr);
      if (gainOn) {  // fade in from start, fade out to the end (the sample's own time)
        const F gi = S::select(S::gt(c.fiInv, S::zero()), S::mul(S::sub(x, c.startF), c.fiInv), S::set(1.0f));
        const F go = S::select(S::gt(c.foInv, S::zero()), S::mul(S::sub(c.endF, x), c.foInv), S::set(1.0f));
        const F g = S::max(S::min(S::min(gi, go), S::set(1.0f)), S::zero());
        yl = S::mul(yl, g);
        yr = S::mul(yr, g);
      }
      S::store(outL + f * W, S::band(active, yl));
      S::store(outR + f * W, S::band(active, yr));  // mono: the left read, copied
      S::store(playing + f * W, active);
      S::store(posOut + f * W, S::mul(x, c.posMul));
      // Advance the 32.32 playhead: unsigned carry out of the fraction via a sign-flipped signed compare.
      if constexpr (!splBit(SM, 0)) toFixed<S>(S::mul(rate.at(f), c.scale), incHi, incLo);
      I dHi = incHi, dLo = incLo;
      if constexpr (kTap == Taps::PingPong) {  // a backwards ping-pong lane moves by −rate
        const I nLo = S::subi(zero, incLo);
        const I borrow = S::xori(S::eqi(incLo, zero), S::seti(-1));  // −1 where lo ≠ 0
        const I negHi = S::addi(S::subi(zero, incHi), borrow);
        const I bi = S::asI(back);
        dHi = S::selecti(bi, negHi, incHi);
        dLo = S::selecti(bi, nLo, incLo);
      }
      const I newFrac = S::addi(frac, dLo);
      const I carry = S::gti(S::xori(frac, signBit), S::xori(newFrac, signBit));  // −1 on carry
      I next = S::subi(S::addi(index, dHi), carry);
      I nextFrac = newFrac;
      if constexpr (kTap != Taps::None) {
        // Loops: crossing loopEnd forwards / loopStart backwards (masks are −1 where true). Forward lanes wrap into the
        // loop modulo its length: next = loopStart + (next − loopEnd) mod len forwards, loopEnd − 1 − (loopStart − 1 −
        // next) mod len backwards (a single ±len jump left the loop for good when |rate| > len: M1 review M11b).
        // Ping-pong lanes reflect at loopEnd − 1 and at loopStart (x → 2b − x) and turn; a reflection that would leave
        // the loop again (|rate| > its length) is clamped into it.
        const I crossFwd = S::andi(S::andi(c.loopOn, S::gti(c.loopEnd, index)), S::gti(next, c.lastIn));
        const I crossBwd = S::andi(S::andi(c.loopOn, S::gti(index, S::subi(c.loopStart, one))), S::gti(c.loopStart, next));
        const I cross = S::xori(crossFwd, crossBwd);  // never both (fwd needs next ≥ loopEnd > loopStart > next)
        if (S::any(S::asF(cross))) {
          const I e = S::selecti(crossFwd, S::subi(next, c.loopEnd), S::subi(S::subi(c.loopStart, one), next));
          const I m = smallMod<S>(e, c.loopLen);
          I wrapped = S::selecti(crossFwd, S::addi(c.loopStart, m), S::subi(c.lastIn, m));
          I wrappedFrac = nextFrac;
          if constexpr (kTap == Taps::PingPong) {
            const I b2 = S::selecti(crossFwd, S::addi(c.lastIn, c.lastIn), S::addi(c.loopStart, c.loopStart));
            const I fracNz = S::xori(S::eqi(nextFrac, zero), S::seti(-1));  // −1 where frac ≠ 0
            I refl = S::addi(S::subi(b2, next), fracNz);                     // 2b − next − (frac ≠ 0)
            refl = S::mini(S::maxi(refl, c.loopStart), c.lastIn);
            const I reflFrac = S::subi(zero, nextFrac);
            const I ppc = S::andi(cross, c.pp);
            wrapped = S::selecti(c.pp, refl, wrapped);
            wrappedFrac = S::selecti(c.pp, reflFrac, nextFrac);
            back = S::select(S::asF(ppc), S::bxor(back, S::asF(S::seti(-1))), back);
          }
          next = S::selecti(cross, wrapped, next);
          nextFrac = S::selecti(cross, wrappedFrac, nextFrac);
        }
      }
      // Leaving [0, end) stops playback; a stopped playhead stays where it was.
      const auto inRange = S::asF(S::andi(S::gti(next, S::seti(-1)), S::gti(c.endEff, next)));
      const I adv = S::asI(active);
      index = S::selecti(adv, next, index);
      frac = S::selecti(adv, nextFrac, frac);
      active = S::band(active, inRange);
    }
    S::storei(st.index, index);
    S::storeu32(st.frac, frac);
    S::store(st.active, active);
    S::store(st.back, back);
    S::storeu32(st.gen, gen);
  }
};

// ---- sample_pos: a stateless read of the newest entry's left channel at a frame position --------------------------

template <bool kCubic>
struct SamplePosK {
  static constexpr uint32_t kSpec = 1;
  template <class S>
  static typename S::F f(const SampleLevelView& v, typename S::F pos) noexcept {  // RT-safe
    const auto valid = S::band(S::ge(pos, S::zero()), S::lt(pos, S::toFloat(S::seti(v.length))));
    const auto fl = S::floor(pos);
    // Clamp the index into [0, length − 1] so the gather stays inside the guarded buffer (NaN → 0).
    const auto i = S::mini(S::maxi(S::truncToInt(fl), S::seti(0)), S::seti(v.length - 1));
    const auto y = sampleInterp<S, kCubic>(v.ch[0], i, S::sub(pos, fl));
    return S::band(valid, y);
  }
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    const SampleLevelView& v = static_cast<const SampleSlot*>(a.resource)->acquire()->level[0];
    const uint32_t count = a.frames * S::kW;
    const uint32_t vecEnd = count & ~7u;
    float* out = a.out[0];
    const In<S8, splBit(SM, 0)> x(a.in[0]);
    for (uint32_t i = 0; i < vecEnd; i += 8) S8::store(out + i, f<S8>(v, x.at(i >> 3)));
    for (uint32_t i = vecEnd; i < count; ++i)
      S1::store(out + i, f<S1>(v, S1::load(splBit(SM, 0) ? a.in[0] : a.in[0] + i)));
  }
};

// ---- sample_slice: a key's slice of the newest entry --------------------------------------------------------------

inline void sliceOf(const Entry& e, float key, bool units, float& start, float& end) noexcept {  // RT-safe
  start = end = 0.0f;
  if (!(std::fabs(key) < 1.0e6f)) return;  // NaN, huge
  const int32_t n = static_cast<int32_t>(std::lround(key)) - 36;
  const int32_t len = e.level[0].length;
  if (n < 0 || len <= 0) return;
  if (e.sliceCount == 0) {
    if (n == 0) end = static_cast<float>(len);  // no slices: the whole sample is slice 0
  } else if (static_cast<uint32_t>(n) < e.sliceCount) {
    start = e.slices[n];
    end = static_cast<uint32_t>(n) + 1 < e.sliceCount ? e.slices[n + 1] : static_cast<float>(len);
  }
  if (units) {
    const float inv = 1.0f / static_cast<float>(len);
    start *= inv;
    end *= inv;
  }
}

struct SampleSliceK {
  static constexpr uint32_t kSpec = 2;  // key, units (a const: its first value is read either way)
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    const Entry& e = *static_cast<const SampleSlot*>(a.resource)->acquire();
    const bool units = a.in[1][0] >= 0.5f;
    constexpr uint32_t W = S::kW;
    if constexpr (splBit(SM, 0)) {  // param rate: one lookup per lane, filled over the block
      float s[8], en[8];
      for (uint32_t l = 0; l < W; ++l) sliceOf(e, a.in[0][l], units, s[l], en[l]);
      for (uint32_t f = 0; f < a.frames; ++f)
        for (uint32_t l = 0; l < W; ++l) {
          a.out[0][f * W + l] = s[l];
          a.out[1][f * W + l] = en[l];
        }
    } else {
      for (uint32_t i = 0; i < a.frames * W; ++i) sliceOf(e, a.in[0][i], units, a.out[0][i], a.out[1][i]);
    }
  }
};

// ---- sample_scrub: a read at a position 0 … 1 of the newest entry (stereo), the mip level from its slope ----------

template <bool kCubic>
struct SampleScrubK {
  static constexpr uint32_t kSpec = 1;  // pos
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    using F = typename S::F;
    using I = typename S::I;
    constexpr uint32_t W = S::kW;
    auto& st = *static_cast<SampleScrubState*>(a.state);
    const Entry& e = *static_cast<const SampleSlot*>(a.resource)->acquire();
    const In<S, splBit(SM, 0)> pos(a.in[0]);
    const int32_t len = e.level[0].length;
    const F lenF = S::set(static_cast<float>(len));
    // The level from the block's fastest |Δpos| · length (frames per sample).
    F prev = S::load(st.prev);
    float slope = 0.0f;
    {
      alignas(32) float d[8];
      const F p1 = a.frames > 1 ? pos.at(a.frames - 1) : pos.at(0);
      S::store(d, S::abs(S::mul(S::sub(p1, prev), lenF)));
      for (uint32_t l = 0; l < W; ++l)
        if (std::isfinite(d[l])) slope = std::fmax(slope, d[l] / static_cast<float>(a.frames > 0 ? a.frames : 1));
    }
    const uint32_t level = mipLevelFor(slope, e.levels);
    const SampleLevelView& lv = e.level[level];
    const float levelScale = 1.0f / static_cast<float>(1u << level);
    const F maxPos = S::set(static_cast<float>(len > 0 ? len - 1 : 0));
    for (uint32_t f = 0; f < a.frames; ++f) {
      const F p = pos.at(f);
      const F ok = S::band(S::eq(p, p), S::band(S::ge(p, S::zero()), S::le(p, S::set(1.0f))));
      const F x = S::mul(S::min(S::max(S::mul(S::band(ok, p), lenF), S::zero()), maxPos), S::set(levelScale));
      const F fl = S::floor(x);
      const I i = S::mini(S::maxi(S::truncToInt(fl), S::seti(0)), S::seti(lv.length > 0 ? lv.length - 1 : 0));
      const F fr = S::sub(x, fl);
      const F yl = S::band(ok, sampleInterp<S, kCubic>(lv.ch[0], i, fr));
      const F yr = e.channels > 1 ? S::band(ok, sampleInterp<S, kCubic>(lv.ch[1], i, fr)) : yl;
      S::store(a.out[0] + f * W, len > 0 ? yl : S::zero());
      S::store(a.out[1] + f * W, len > 0 ? yr : S::zero());
      prev = p;
    }
    S::store(st.prev, S::band(S::eq(prev, prev), prev));
  }
};

}  // namespace

KernelFn findSampleKernel(Atom a, uint32_t lanes, uint32_t m) noexcept {
  switch (a) {
    case Atom::SamplePosLinear: return pick<SamplePosK<false>>(lanes, m);
    case Atom::SamplePosCubic: return pick<SamplePosK<true>>(lanes, m);
    case Atom::SampleReadLinear: return pick<SampleReadK<false>>(lanes, m);
    case Atom::SampleReadCubic: return pick<SampleReadK<true>>(lanes, m);
    case Atom::SampleSlice: return pick<SampleSliceK>(lanes, m);
    case Atom::SampleScrub: return pick<SampleScrubK<true>>(lanes, m);
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail
