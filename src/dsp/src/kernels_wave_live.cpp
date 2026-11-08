// wavetable_live and wavetable_shape (TMM.8): the Wavetable's live warp / fold / sync path and Shape mode with ADAA.
// Semantics: pq/dsp/wave_live.h; slots and swaps: pq/dsp/frame_table_slot.h (as kernels_frame_table.cpp).
//
// CPU: one acquire load of the slot per call, then per frame of 8 lanes (S8; the 1-lane form runs the same steps on
// lane 0 of an SSE register, S1, bit-identical per lane): the controls (a param-rate one ramps: one FMA), the sync
// product, the warp, the level pair (4 gathers), two reads per tap (Mirror: two taps), the fold (two sines, only
// when fold can be non-zero). With corrections, one vector add / load / store on the ring per frame and a scalar
// pass only on the frames where a lane has an event (a few per cycle). Shape: four antiderivative reads and the
// ADAA division per frame, a value read only when a lane falls back to the midpoint.

#include <immintrin.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <type_traits>

#include "frame_table_ops.h"
#include "frame_table_simd.h"
#include "kernels_common.h"
#include "pq/core/voice_tap.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_slot.h"
#include "pq/dsp/wave_live.h"

namespace pq::dsp {

namespace {

constexpr uint32_t kD = kLiveCorrHalf;
constexpr uint32_t kRing = 2 * kD;
constexpr uint32_t kP = kLiveResidualSteps;
constexpr double kPi = std::numbers::pi;
static_assert(kRing == 8, "the ring is 8 slots");

// ---- Residual tables (static initialisation, never on the audio thread) ----------------------------------------
struct Residuals {
  float r1[kRing][kP + 1];  // [k + kD][m]: offset k + m/kP
  float r2[kRing][kP + 1];
};

double besselI0(double x) noexcept {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 64; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-18 * sum) break;
  }
  return sum;
}

Residuals makeResiduals() noexcept {
  Residuals r{};
  constexpr double kBeta = 9.5, kFc = 0.495;
  constexpr uint32_t kSub = 16;                // integration steps per table step
  constexpr uint32_t kN = 2 * kD * kP * kSub;  // fine steps over [−D, D]
  const double h = 1.0 / (kP * kSub);
  const double i0b = besselI0(kBeta);
  auto kern = [&](double t) {
    const double q = t / kD;
    const double w = q * q < 1.0 ? besselI0(kBeta * std::sqrt(1.0 - q * q)) / i0b : 0.0;
    const double a = 2.0 * kPi * kFc * t;
    return 2.0 * kFc * (t == 0.0 ? 1.0 : std::sin(a) / a) * w;
  };
  // Simpson over each fine step (midpoint included): K(t) = ∫_{−D}^{t} k, then normalised so K(D) = 1.
  static double K[kN + 1];  // static storage: built once, before main
  K[0] = 0.0;
  double prev = kern(-static_cast<double>(kD));
  for (uint32_t i = 1; i <= kN; ++i) {
    const double t1 = -static_cast<double>(kD) + i * h;
    const double cur = kern(t1);
    K[i] = K[i - 1] + h / 6.0 * (prev + 4.0 * kern(t1 - 0.5 * h) + cur);
    prev = cur;
  }
  const double total = K[kN];
  for (uint32_t i = 0; i <= kN; ++i) K[i] /= total;
  // r1 = K − H (H(0) = 1: a sample at the event is after it); r2 = ∫ r1 (trapezoid; r1 is smooth on each side).
  static double R2[kN + 1];
  R2[0] = 0.0;
  const uint32_t zero = kD * kP * kSub;  // index of t = 0
  for (uint32_t i = 1; i <= kN; ++i) {
    const double a = K[i - 1] - (i - 1 >= zero ? 1.0 : 0.0);
    const double b = K[i] - (i > zero ? 1.0 : 0.0);  // left limit at i == zero
    R2[i] = R2[i - 1] + 0.5 * h * (a + b);
  }
  for (uint32_t k = 0; k < kRing; ++k)
    for (uint32_t m = 0; m <= kP; ++m) {
      const uint32_t i = (k * kP + m) * kSub;
      const bool right = static_cast<int32_t>(k) - static_cast<int32_t>(kD) >= 0;  // offsets ≥ 0: after the event
      r.r1[k][m] = static_cast<float>(K[i] - (right ? 1.0 : 0.0));
      r.r2[k][m] = static_cast<float>(R2[i]);
    }
  return r;
}

const Residuals kResiduals = makeResiduals();

// ---- Shared steps -----------------------------------------------------------------------------------------------
using simd::S1;
using simd::S8;

template <class S>
__forceinline typename S::I gatherU(const uint32_t* base, typename S::I idx) noexcept {
  return S::asI(S::gather(reinterpret_cast<const float*>(base), idx));
}

template <class S>
struct Lv {
  typename S::I info, rich;
  typename S::F w;  // weight of the richer level
};

// The level pair for a speed (≥ 0; NaN reads as 0). RT-safe.
template <class S>
__forceinline Lv<S> levelsFor(const FrameTableView& t, typename S::F speed) noexcept {
  const auto b = detail::ft::levelBucket<S>(speed);
  const auto sp = S::max(speed, S::zero());
  const auto top = S::gather(t.bucketTop, b);
  const auto inv = S::gather(t.bucketInvSpan, b);
  const auto w = S::min(S::max(S::mul(S::sub(top, sp), inv), S::zero()), S::set(1.0f));
  return {gatherU<S>(t.bucketInfo, b), gatherU<S>(t.bucketRich, b), w};
}

template <class S>
struct Pick {
  typename S::I off, step;
  typename S::F w;
  bool blend;
};

template <class S>
__forceinline Pick<S> pickFrames(const FrameTableView& t, typename S::F u) noexcept {
  if (t.frames <= 1) return {S::seti(0), S::seti(0), S::zero(), false};
  typename S::I k;
  typename S::F w;
  detail::ft::framePosition<S>(t, u, k, w);
  const auto stride = S::seti(static_cast<int32_t>(t.frameStride));
  const auto moving = S::ne(w, S::zero());
  return {S::muli(k, stride), S::andi(S::asI(moving), stride), w, S::any(moving)};
}

// One read at one level (blended between frames when the pick blends). RT-safe.
template <class S>
__forceinline typename S::F readAt(const float* data, typename S::I info, typename S::F phase,
                                   const Pick<S>& pk) noexcept {
  typename S::I idx;
  typename S::F x;
  detail::ft::splitPhase<S>(info, phase, pk.off, idx, x);
  typename S::F r[6];
  detail::ft::rows6(data, idx, r);
  if (pk.blend) {
    typename S::F q[6];
    detail::ft::rows6(data, S::addi(idx, pk.step), q);
    for (int i = 0; i < 6; ++i) r[i] = S::fma(pk.w, S::sub(q[i], r[i]), r[i]);
  }
  return detail::ft::poly6(r, x);
}

// A read crossfaded over the level pair. RT-safe.
template <class S>
__forceinline typename S::F readPair(const float* data, const Lv<S>& lv, typename S::F phase, const Pick<S>& pk) noexcept {
  const auto v = readAt<S>(data, lv.info, phase, pk);
  if (!S::any(S::gt(lv.w, S::zero()))) return v;
  const auto vr = readAt<S>(data, lv.rich, phase, pk);
  return S::fma(lv.w, S::sub(vr, v), v);
}

// A control's value at frame f: audio rate as it is; param rate ramped from the previous block's value (a fresh
// lane: the value). Clamped to [lo, hi] (NaN → lo).
template <class S>
__forceinline typename S::F control(const float* p, bool spl, const float* prev, typename S::F primed, uint32_t f,
                                    float rampT, float lo, float hi) noexcept {
  typename S::F v;
  if (spl) {
    const auto tgt = S::load(p);
    const auto pv = S::load(prev);
    v = S::select(primed, S::fma(S::sub(tgt, pv), S::set(rampT), pv), tgt);
  } else {
    v = S::load(p + static_cast<size_t>(f) * S::kW);
  }
  return S::min(S::max(v, S::set(lo)), S::set(hi));
}

// ---- Double evaluation (the corrections and the tests' reference) ---------------------------------------------
struct PickD {
  std::size_t off = 0, step = 0;
  double w = 0;
};

PickD pickD(const FrameTableView& t, float position) noexcept {
  PickD p;
  if (t.frames <= 1) return p;
  __m128i ki;
  __m128 wf;
  detail::ft::framePosition<S1>(t, _mm_set_ss(position), ki, wf);
  p.off = static_cast<std::size_t>(_mm_cvtsi128_si32(ki)) * t.frameStride;
  p.w = _mm_cvtss_f32(wf);
  p.step = p.w != 0.0 ? t.frameStride : 0;
  return p;
}

// Value and d/dphase (per cycle) of one level in double.
double readLevelD(const float* data, const PickD& pk, uint32_t info, double phase, double* slope) noexcept {
  const std::size_t off = info & detail::ft::kInfoOffsetMask;
  const double len = static_cast<double>(info >> 17);
  double ph = phase - std::floor(phase);
  if (!(ph >= 0.0) || ph >= 1.0) ph = 0.0;
  const double pos = ph * len;
  const double fi = std::floor(pos);
  const double z = pos - fi - 0.5;
  const float* a = data + pk.off + off + static_cast<std::ptrdiff_t>(fi);
  double y[6];
  for (int q = 0; q < 6; ++q) {
    const double va = a[q - 2];
    y[q] = pk.step != 0 ? va + pk.w * (static_cast<double>(a[q - 2 + pk.step]) - va) : va;
  }
  const double e[3] = {y[2] + y[3], y[1] + y[4], y[0] + y[5]};
  const double o[3] = {y[2] - y[3], y[1] - y[4], y[0] - y[5]};
  double c[6];
  for (int n = 0; n < 6; ++n) {
    c[n] = 0;
    for (int d = 0; d < 3; ++d) c[n] += static_cast<double>(detail::ft::kInterp[d][n]) * (n % 2 ? o[d] : e[d]);
  }
  double v = 0, dv = 0;
  for (int n = 5; n >= 0; --n) {
    dv = dv * z + v;
    v = v * z + c[n];
  }
  if (slope) *slope = dv * len;
  return v;
}

double readPairD(const float* data, const PickD& pk, const LiveLevels& lv, double phase, double* slope) noexcept {
  double s0 = 0, s1 = 0;
  const double v0 = readLevelD(data, pk, lv.info, phase, &s0);
  if (lv.wRich <= 0.0f) {
    if (slope) *slope = s0;
    return v0;
  }
  const double v1 = readLevelD(data, pk, lv.rich, phase, &s1);
  const double w = lv.wRich;
  if (slope) *slope = s0 + w * (s1 - s0);
  return v0 + w * (v1 - v0);
}

using Taps = LiveTaps;

// The warp's taps at ψ ∈ [0, 1] with their slopes; side −1 / +1 picks the left / right branch at a breakpoint.
Taps tapsAt(WarpMode mode, double psi, double warp, int side) noexcept {
  Taps t;
  t.a = t.b = psi;
  if (warp == 0.0) return t;
  switch (mode) {
    case WarpMode::Bend: {
      const double c = bendCurve(warp);
      const double den = c + (1.0 - c) * psi;
      t.a = psi / den;
      t.da = c / (den * den);
      t.dadw = 2.0 * std::numbers::ln2 * c * psi * (1.0 - psi) / (den * den);
      break;
    }
    case WarpMode::Mirror: {
      const bool lo = side < 0 ? psi <= 0.5 : psi < 0.5;
      const bool fwd = lo == (warp > 0.0);
      t.b = fwd ? psi : 1.0 - psi;
      t.db = fwd ? 1.0 : -1.0;
      t.m = std::fabs(warp);
      break;
    }
    case WarpMode::PulseWidth: {
      const double pw = pulseWidth(warp);
      const bool lo = side < 0 ? psi <= pw : psi < pw;
      t.da = lo ? 0.5 / pw : 0.5 / (1.0 - pw);
      t.a = lo ? psi * t.da : 0.5 + (psi - pw) * t.da;
      const double dadpw = lo ? -psi / (2.0 * pw * pw) : (psi - 1.0) / (2.0 * (1.0 - pw) * (1.0 - pw));
      t.dadw = dadpw * -(0.5 - kPulseMinWidth);
      break;
    }
  }
  return t;
}

double clampD(double v, double lo, double hi) noexcept { return v >= lo ? (v <= hi ? v : hi) : lo; }

}  // namespace

float liveHeadroom(uint32_t factor) noexcept {
  return factor >= 16 ? 4.0f : factor >= 8 ? 3.0f : factor >= 4 ? 2.0f : 1.0f;
}

uint32_t liveFactor(float stateRate, const FrameTableView& t) noexcept {
  if (!(t.sampleRate > 0.0f) || !(stateRate > 0.0f)) return 1;
  const float r = stateRate / t.sampleRate;
  return r >= 12.0f ? 16u : r >= 6.0f ? 8u : r >= 3.0f ? 4u : r >= 1.5f ? 2u : 1u;
}

LiveLevels liveLevels(const FrameTableView& t, float speed) noexcept {  // RT-safe
  const Lv<S1> lv = levelsFor<S1>(t, _mm_set_ss(speed));
  return {static_cast<uint32_t>(_mm_cvtsi128_si32(lv.info)), static_cast<uint32_t>(_mm_cvtsi128_si32(lv.rich)),
          _mm_cvtss_f32(lv.w)};
}

double liveRead(const FrameTableView& t, float position, const LiveLevels& lv, double phase, double* slope) noexcept {
  return readPairD(t.data, pickD(t, position), lv, phase, slope);
}

LiveTaps liveTaps(WarpMode mode, double psi, double warp, int side) noexcept {
  return tapsAt(mode, psi, clampD(warp, -1.0, 1.0), side);
}

double liveSpeedScale(double fold) noexcept { return kLiveBandScale * (1.0 + kFoldMaxTheta * clampD(fold, 0.0, 1.0)); }

float liveRepIncrement(const FrameTableView& t, float baseInc) noexcept {
  return _mm_cvtss_f32(S1::gather(t.bucketTop, detail::ft::levelBucket<S1>(_mm_set_ss(baseInc))));
}

double liveValueAtPsi(const FrameTableView& t, float position, const LiveControls& c, double psi, double inc,
                      int side, double* dydpsi, double speedA, double speedB) noexcept {
  const double warp = clampD(c.warp, -1.0, 1.0), fold = clampD(c.fold, 0.0, 1.0), sync = clampD(c.sync, 0.0, 1.0);
  const double s = 1.0 + (kSyncMaxRatio - 1.0) * sync;
  const Taps tp = tapsAt(c.mode, psi, warp, side);
  const PickD pk = pickD(t, position);
  double xa = 0, xb = 0;
  const double lvInc = liveRepIncrement(t, static_cast<float>(inc)) * liveSpeedScale(fold);
  if (speedA < 0.0) speedA = lvInc * s * std::fabs(tp.da);
  if (speedB < 0.0) speedB = lvInc * s;
  const double va = readPairD(t.data, pk, liveLevels(t, static_cast<float>(speedA)), tp.a, &xa);
  double v = va, dv = xa * tp.da;
  if (tp.m != 0.0) {
    const double vb = readPairD(t.data, pk, liveLevels(t, static_cast<float>(speedB)), tp.b, &xb);
    v = (1.0 - tp.m) * va + tp.m * vb;
    dv = (1.0 - tp.m) * xa * tp.da + tp.m * xb * tp.db;
  }
  const double theta = kFoldMaxTheta * fold;
  double y = v, dy = dv;
  if (theta != 0.0) {
    const double norm = theta < 0.5 * kPi ? 1.0 / std::sin(theta) : 1.0;
    y = std::sin(theta * v) * norm;
    dy = theta * std::cos(theta * v) * norm * dv;
  }
  if (dydpsi) *dydpsi = dy;
  return y;
}

double liveValue(const FrameTableView& t, float position, const LiveControls& c, double phase, double inc, int side,
                 double* dydphi) noexcept {
  const double s = 1.0 + (kSyncMaxRatio - 1.0) * clampD(c.sync, 0.0, 1.0);
  const double p = phase * s;
  double psi = p - std::floor(p);
  if (side < 0 && psi == 0.0 && p > 0.0) psi = 1.0;  // the left limit at a sub-period boundary
  double g = 0;
  const double y = liveValueAtPsi(t, position, c, psi, inc, side, &g);
  if (dydphi) *dydphi = g * s;
  return y;
}

double liveMirrorCrossing(const FrameTableView& t, float position, const LiveControls& c, double psi, bool toPositive,
                          double speedA, double speedB) noexcept {
  const PickD pk = pickD(t, position);
  const double xa = readPairD(t.data, pk, liveLevels(t, static_cast<float>(speedA)), psi, nullptr);
  const LiveLevels lb = liveLevels(t, static_cast<float>(speedB));
  const double bPos = 0.5 - std::fabs(psi - 0.5), bNeg = 0.5 + std::fabs(psi - 0.5);
  const double xbp = readPairD(t.data, pk, lb, bPos, nullptr), xbn = readPairD(t.data, pk, lb, bNeg, nullptr);
  const double theta = kFoldMaxTheta * clampD(c.fold, 0.0, 1.0);
  double fp = 1.0;  // fold′ at v = x(a) (the mix is 0 at the crossing)
  if (theta != 0.0) fp = theta * std::cos(theta * xa) * (theta < 0.5 * kPi ? 1.0 / std::sin(theta) : 1.0);
  return (toPositive ? 1.0 : -1.0) * (xbp + xbn - 2.0 * xa) * fp;
}

void liveResidual(int32_t k, double d, double* r1, double* r2) noexcept {
  if (k < -static_cast<int32_t>(kD) || k >= static_cast<int32_t>(kD) || !(d >= 0.0)) {
    *r1 = *r2 = 0.0;
    return;
  }
  const double x = (d > 1.0 ? 1.0 : d) * kP;
  uint32_t m = static_cast<uint32_t>(x);
  if (m >= kP) m = kP - 1;
  const double fr = x - m;
  const uint32_t row = static_cast<uint32_t>(k + static_cast<int32_t>(kD));
  *r1 = kResiduals.r1[row][m] + fr * (kResiduals.r1[row][m + 1] - kResiduals.r1[row][m]);
  *r2 = kResiduals.r2[row][m] + fr * (kResiduals.r2[row][m + 1] - kResiduals.r2[row][m]);
}

double shapeCurve(const FrameTableView& t, float position, const LiveLevels& lv, double u) noexcept {
  return readPairD(t.data, pickD(t, position), lv, clampD(u, 0.0, 1.0), nullptr);
}

namespace {

// The static live read for the bake (wave_live.h liveRenderStatic): the kernel's steps without state, 8 phases at a
// time, frame `frame` of `t`, levels from the representative base increment.
template <WarpMode M>
void renderStaticT(const FrameTableView& t, uint32_t frame, const LiveControls& c, float incRep, const float* phase,
                   uint32_t n, float* out) noexcept {
  using S = S8;
  using F = S::F;
  const Pick<S> pk{S::seti(static_cast<int32_t>(frame * t.frameStride)), S::seti(0), S::zero(), false};
  const F half = S::set(0.5f);
  const F warp = S::set(static_cast<float>(clampD(c.warp, -1.0, 1.0)));
  const float foldV = static_cast<float>(clampD(c.fold, 0.0, 1.0));
  const F s = S::set(static_cast<float>(1.0 + (kSyncMaxRatio - 1.0) * clampD(c.sync, 0.0, 1.0)));
  const F theta = S::set(foldV * static_cast<float>(kFoldMaxTheta));
  const F scale = S::set(static_cast<float>(liveSpeedScale(foldV)));
  const F rate = S::mul(S::set(incRep), s);
  for (uint32_t i = 0; i < n; i += 8) {
    alignas(32) float ph[8];
    for (uint32_t l = 0; l < 8; ++l) ph[l] = phase[i + l < n ? i + l : n - 1];
    const F phi = S::load(ph);
    const F prod = S::mul(phi, s);
    const F err = S::fma(phi, s, S::neg(prod));
    F psi = S::add(S::sub(prod, S::floor(prod)), err);
    psi = S::sub(psi, S::floor(psi));
    F ta = psi, tb = psi, mix = S::zero(), slopeA = S::set(1.0f);
    if constexpr (M == WarpMode::Bend) {
      const F cc = S::exactExp(S::mul(warp, S::set(static_cast<float>(-2.0 * std::numbers::ln2))));
      const F den = S::fma(S::sub(S::set(1.0f), cc), psi, cc);
      ta = S::div(psi, den);
      slopeA = S::mul(cc, S::div(S::set(1.0f), S::mul(den, den)));
    } else if constexpr (M == WarpMode::Mirror) {
      const F fwd = S::bnot(S::bxor(S::lt(psi, half), S::gt(warp, S::zero())));
      tb = S::select(fwd, psi, S::sub(S::set(1.0f), psi));
      mix = S::abs(warp);
    } else {
      const F pw = S::fma(warp, S::set(static_cast<float>(-(0.5 - kPulseMinWidth))), half);
      const F lo = S::lt(psi, pw);
      slopeA = S::select(lo, S::div(half, pw), S::div(half, S::sub(S::set(1.0f), pw)));
      ta = S::select(lo, S::mul(psi, slopeA), S::fma(S::sub(psi, pw), slopeA, half));
    }
    F v = readPair<S>(t.data, levelsFor<S>(t, S::mul(S::mul(rate, slopeA), scale)), ta, pk);
    if constexpr (M == WarpMode::Mirror)
      if (S::any(S::ne(mix, S::zero()))) {
        const F vb = readPair<S>(t.data, levelsFor<S>(t, S::mul(rate, scale)), tb, pk);
        v = S::fma(mix, S::sub(vb, v), v);
      }
    F y = v;
    if (foldV != 0.0f) {
      const F norm = foldV * kFoldMaxTheta < 0.5 * kPi ? S::div(S::set(1.0f), S::exactSin(theta)) : S::set(1.0f);
      y = S::mul(S::exactSin(S::mul(theta, v)), norm);
    }
    alignas(32) float res[8];
    S::store(res, y);
    for (uint32_t l = 0; l < 8 && i + l < n; ++l) out[i + l] = res[l];
  }
}

}  // namespace

void liveRenderStatic(const FrameTableView& t, uint32_t frame, const LiveControls& c, float incRep,
                      const float* phase, uint32_t n, float* out) noexcept {  // RT-safe
  switch (c.warp == 0.0 ? WarpMode::Bend : c.mode) {
    case WarpMode::Bend: renderStaticT<WarpMode::Bend>(t, frame, c, incRep, phase, n, out); break;
    case WarpMode::Mirror: renderStaticT<WarpMode::Mirror>(t, frame, c, incRep, phase, n, out); break;
    default: renderStaticT<WarpMode::PulseWidth>(t, frame, c, incRep, phase, n, out); break;
  }
}

namespace detail {
namespace {

// ---- wavetable_live ---------------------------------------------------------------------------------------------
constexpr uint32_t kFadeChunk = kFrameTableFadeChunk;

struct LiveArgs {
  const float* in[5];  // phase, position, warp, fold, sync (at the run's first frame)
  uint32_t spl;        // Spl bits of in[0..4]
  uint32_t f0;         // the run's first frame in the block
  uint32_t total;      // the block's frames (the param ramps' length)
  bool last;           // the run ends the block: the controls' block values become the previous ones
  bool foldOn;         // fold may be non-zero this block
  float os;            // the region's factor (the base increment = os × the increment here)
};

// Stamps one event's residuals around the current slot. RT-safe.
inline void stampEvent(WavetableLiveState& st, uint32_t lane, double d, double jump, double kink) noexcept {
  if (!(d >= 0.0)) return;
  if (d > 1.0) d = 1.0;
  for (int32_t k = -static_cast<int32_t>(kD); k < static_cast<int32_t>(kD); ++k) {
    double r1, r2;
    liveResidual(k, d, &r1, &r2);
    float& slot = st.ring[(st.ringPos + static_cast<uint32_t>(k + static_cast<int32_t>(kRing))) & (kRing - 1)][lane];
    slot += static_cast<float>(jump * r1 + kink * r2);
  }
}

// The events of one lane between its previous and its current sample (wave_live.h "Corrections"). RT-safe.
void laneEvents(const FrameTableView& t, WavetableLiveState& st, uint32_t lane, WarpMode mode, float phi,
                float prevPhi, float psi, float prevPsi, bool wrapped, float s, float warp, float fold, float inc,
                float position, float psiRate, float speedA, float speedB, float prevSpeedA,
                float prevSpeedB, float dW, float psiRate0) noexcept {
  const LiveControls c{mode, warp, fold, (static_cast<double>(s) - 1.0) / (kSyncMaxRatio - 1.0)};
  const double sd = s;
  const double psiRateIn = psiRate;  // ψ per sample
  if (mode == WarpMode::Mirror && dW != 0.0f && (warp > 0.0f) != (warp - dW > 0.0f)) {
    // The warp crossed 0 (between the previous sample and this one): a corner in the mix.
    const double d = static_cast<double>(warp) / static_cast<double>(dW);
    stampEvent(st, lane, d, 0.0, static_cast<double>(dW) * liveMirrorCrossing(t, position, c, psi, warp > 0.0f, speedA, speedB));
  }
  // TMM.18: φ moves forwards or backwards (a negative frequency): the sign of psiRate0 = ±inc·s, ψ's rate at φ = 0.
  // ψ is handled in φ's direction and, as before for a forward φ, a sample where ψ moves against it (a sync falling
  // faster than φ·s rises) has no events. An event at a breakpoint is the step from the value just BEFORE it in time
  // (read at the previous sample's levels) to the value just AFTER it (this sample's levels): forwards from the left
  // limit to the right one, backwards from the right limit to the left one. The slope jump is per sample of time,
  // (g_after − g_before)·rate with the signed rate; the times since the events are distances in ψ over |rate|.
  // Backwards is the forward case mirrored in time: at φ's wrap the forward case takes ψ's rate on the φ ≈ 0 side
  // (after the reset), so the backward one takes it there too (before the reset: psiRate0, without φ·Δs).
  // tests/dsp/test_wave_live.cpp: a reversed phase at static controls gives the forward spectrum within −130 dB.
  if (!(inc > 0.0f) || psiRate0 == 0.0f) return;
  const bool fwd = psiRate0 > 0.0f;
  const bool backWrap = wrapped && !fwd;
  const double rate = backWrap ? static_cast<double>(psiRate0) : psiRateIn;  // ψ per sample
  if (!(fwd ? rate > 0.0 : rate < 0.0)) return;
  const double ar = std::fabs(rate);
  auto stepAt = [&](double before, int sideBefore, double after, int sideAfter, double& jump) {
    double gb = 0, ga = 0;
    const double yb = liveValueAtPsi(t, position, c, before, inc, sideBefore, &gb, prevSpeedA, prevSpeedB);
    const double ya = liveValueAtPsi(t, position, c, after, inc, sideAfter, &ga, speedA, speedB);
    jump = ya - yb;
    return (ga - gb) * rate;
  };
  // A kink at k crossed in the direction of motion.
  auto kinkStep = [&](double k, double& jump) { return fwd ? stepAt(k, -1, k, +1, jump) : stepAt(k, +1, k, -1, jump); };
  // The kink inside a sub-period (Mirror: ½, while the second tap is in; Pulse width: w).
  double kinkAt = -1.0;
  if (mode == WarpMode::Mirror && warp != 0.0f) kinkAt = 0.5;
  if (mode == WarpMode::PulseWidth && warp != 0.0f) kinkAt = pulseWidth(warp);
  const double dp = static_cast<double>(psi), dpp = static_cast<double>(prevPsi);
  if (wrapped) {
    // A sync reset at φ's wrap (also the plain cycle wrap when s = 1): d = time since it, in samples.
    double end = sd - std::floor(sd);  // ψ as φ → 1 (its left limit)
    if (end == 0.0) end = 1.0;
    double jump = 0, j2 = 0;
    if (phi < prevPhi) {  // forwards: ψ ran dpp → end, reset to 0, ran on to dp
      const double dphi = static_cast<double>(phi) + 1.0 - static_cast<double>(prevPhi);
      const double d = dphi > 0.0 ? static_cast<double>(phi) / dphi : 0.0;
      const double kink = stepAt(end, -1, 0.0, +1, jump);
      stampEvent(st, lane, d, jump, kink);
      if (kinkAt > 0.0) {
        if (dp >= kinkAt) {
          const double kk = kinkStep(kinkAt, j2);
          stampEvent(st, lane, (dp - kinkAt) / ar, j2, kk);
        }
        if (dpp < kinkAt && kinkAt <= end) {
          const double kk = kinkStep(kinkAt, j2);
          stampEvent(st, lane, d + (end - kinkAt) / ar, j2, kk);
        }
      }
    } else {  // backwards: ψ ran dpp → 0, jumped to end, ran down to dp
      const double dphi = static_cast<double>(prevPhi) + 1.0 - static_cast<double>(phi);
      const double d = dphi > 0.0 ? (1.0 - static_cast<double>(phi)) / dphi : 0.0;
      const double kink = stepAt(0.0, +1, end, -1, jump);
      stampEvent(st, lane, d, jump, kink);
      if (kinkAt > 0.0) {
        if (dp < kinkAt && kinkAt < end) {
          const double kk = kinkStep(kinkAt, j2);
          stampEvent(st, lane, (kinkAt - dp) / ar, j2, kk);
        }
        if (dpp >= kinkAt) {
          const double kk = kinkStep(kinkAt, j2);
          stampEvent(st, lane, d + kinkAt / ar, j2, kk);
        }
      }
    }
    return;
  }
  if (fwd ? dp < dpp : dp > dpp) {
    // ψ wrapped (a sub-period boundary): its two ends' slopes.
    double j = 0;
    if (fwd) {
      const double span = dp + 1.0 - dpp;
      const double kw = stepAt(1.0, -1, 0.0, +1, j);
      stampEvent(st, lane, span > 0.0 ? dp / span : 0.0, j, kw);
      if (kinkAt > 0.0) {
        if (dp >= kinkAt) {
          const double kk = kinkStep(kinkAt, j);
          stampEvent(st, lane, (dp - kinkAt) / ar, j, kk);
        }
        if (dpp < kinkAt) {
          const double kk = kinkStep(kinkAt, j);
          stampEvent(st, lane, (dp + 1.0 - kinkAt) / ar, j, kk);
        }
      }
    } else {
      const double span = dpp + 1.0 - dp;
      const double kw = stepAt(0.0, +1, 1.0, -1, j);
      stampEvent(st, lane, span > 0.0 ? (1.0 - dp) / span : 0.0, j, kw);
      if (kinkAt > 0.0) {
        if (dp < kinkAt) {
          const double kk = kinkStep(kinkAt, j);
          stampEvent(st, lane, (kinkAt - dp) / ar, j, kk);
        }
        if (dpp >= kinkAt) {
          const double kk = kinkStep(kinkAt, j);
          stampEvent(st, lane, (kinkAt + 1.0 - dp) / ar, j, kk);
        }
      }
    }
    return;
  }
  if (kinkAt > 0.0 && (fwd ? (dpp < kinkAt && dp >= kinkAt) : (dpp >= kinkAt && dp < kinkAt))) {
    double j = 0;
    const double kk = kinkStep(kinkAt, j);
    stampEvent(st, lane, (dp - kinkAt) / (dp - dpp), j, kk);
  }
}

template <class S, WarpMode M, bool kCorr>
void liveRun(const FrameTableView& t, WavetableLiveState& st, const LiveArgs& a, uint32_t n, float* out) noexcept {
  using F = typename S::F;
  constexpr uint32_t W = S::kW;
  const bool posSpl = splBit(a.spl, 1);
  const Pick<S> pkSpl = posSpl ? pickFrames<S>(t, S::load(a.in[1])) : Pick<S>{};
  const F half = S::set(0.5f);
  {
    // A lane without a previous sample ramps from its first value (its previous-block values are not its own).
    const F primed0 = S::load(st.primed);
    for (int c = 0; c < 3; ++c)
      if (splBit(a.spl, 2 + c)) S::store(st.prevCtl[c], S::select(primed0, S::load(st.prevCtl[c]), S::load(a.in[2 + c])));
  }
  for (uint32_t f = 0; f < n; ++f) {
    const size_t o = static_cast<size_t>(f) * W;
    const F primed = S::load(st.primed);
    const F phi = S::load(a.in[0] + (splBit(a.spl, 0) ? 0 : o));
    const F prevPhi = S::load(st.prevPhase);
    const float rampT = static_cast<float>(a.f0 + f + 1) / static_cast<float>(a.total);
    const F warp = control<S>(a.in[2], splBit(a.spl, 2), st.prevCtl[0], primed, f, rampT, -1.0f, 1.0f);
    const F sync = control<S>(a.in[4], splBit(a.spl, 4), st.prevCtl[2], primed, f, rampT, 0.0f, 1.0f);
    // The increment estimate (as the readers): the smaller of the last two wrapped |Δφ|.
    const F dl = detail::ft::phaseDelta<S>(phi, prevPhi);
    const F inc = S::min(dl, S::load(st.prevDelta));
    S::store(st.prevDelta, dl);
    // TMM.18: the direction φ moves in (a negative frequency runs it backwards): the sign of this sample's wrapped Δφ.
    // The speeds below are signed with it, so ψ′'s two parts (e·s and φ·Δs) add with their true signs.
    const F d0 = S::sub(phi, prevPhi);
    const F dir = S::select(S::lt(S::sub(d0, S::round(d0)), S::zero()), S::set(-1.0f), S::set(1.0f));
    const F incDir = S::mul(inc, dir);
    // ψ = frac(φ·s) with the product's rounding error kept.
    const F s = S::fma(sync, S::set(static_cast<float>(kSyncMaxRatio - 1.0)), S::set(1.0f));
    const F prod = S::mul(phi, s);
    const F err = S::fma(phi, s, S::neg(prod));
    F sub = S::floor(prod);
    F psi = S::add(S::sub(prod, sub), err);
    const F fl = S::floor(psi);
    sub = S::add(sub, fl);
    psi = S::sub(psi, fl);
    // The warp's taps, their slope a′ = ∂a/∂ψ and ∂a/∂warp.
    F ta = psi, tb = psi, mix = S::zero(), slopeA = S::set(1.0f), dadw = S::zero();
    if constexpr (M == WarpMode::Bend) {
      const F c = S::exactExp(S::mul(warp, S::set(static_cast<float>(-2.0 * std::numbers::ln2))));
      const F den = S::fma(S::sub(S::set(1.0f), c), psi, c);
      ta = S::div(psi, den);
      const F inv2 = S::div(S::set(1.0f), S::mul(den, den));
      slopeA = S::mul(c, inv2);
      dadw = S::mul(S::mul(S::set(static_cast<float>(2.0 * std::numbers::ln2)), slopeA),
                    S::mul(psi, S::sub(S::set(1.0f), psi)));
    } else if constexpr (M == WarpMode::Mirror) {
      const F lo = S::lt(psi, half);
      const F fwd = S::bnot(S::bxor(lo, S::gt(warp, S::zero())));
      tb = S::select(fwd, psi, S::sub(S::set(1.0f), psi));
      mix = S::abs(warp);
    } else {
      const F pw = S::fma(warp, S::set(static_cast<float>(-(0.5 - kPulseMinWidth))), half);
      const F lo = S::lt(psi, pw);
      slopeA = S::select(lo, S::div(half, pw), S::div(half, S::sub(S::set(1.0f), pw)));
      ta = S::select(lo, S::mul(psi, slopeA), S::fma(S::sub(psi, pw), slopeA, half));
      // ∂a/∂pw = −ψ/(2pw²) below w, (ψ − 1)/(2(1 − pw)²) above; ∂pw/∂warp = −15/32.
      const F dadpw = S::select(lo, S::neg(S::mul(S::mul(psi, slopeA), S::mul(slopeA, S::set(2.0f)))),
                                S::mul(S::sub(psi, S::set(1.0f)), S::mul(S::mul(slopeA, slopeA), S::set(2.0f))));
      dadw = S::mul(dadpw, S::set(static_cast<float>(-(0.5 - kPulseMinWidth))));
    }
    // ψ per sample: φ's increment times s, plus what a moving s adds (φ·Δs), and the warp's own motion (∂a/∂w·Δw).
    const F dS = S::select(primed, S::sub(s, S::load(st.prevS)), S::zero());
    const F dW = S::select(primed, S::sub(warp, S::load(st.prevW)), S::zero());
    S::store(st.prevS, s);
    S::store(st.prevW, warp);
    const F rate = S::fma(phi, dS, S::mul(incDir, s));  // signed: ψ per sample
    F fold = S::zero(), theta = S::zero();
    // The read's band (wave_live.h "Level choice"): base-rate speeds from the representative increment of the base
    // level, scaled by kLiveBandScale and the fold's spread (1 + θ).
    F lvScale = S::set(static_cast<float>(kLiveBandScale));
    if (a.foldOn) {
      fold = control<S>(a.in[3], splBit(a.spl, 3), st.prevCtl[1], primed, f, rampT, 0.0f, 1.0f);
      theta = S::mul(fold, S::set(static_cast<float>(kFoldMaxTheta)));
      lvScale = S::mul(lvScale, S::add(S::set(1.0f), theta));
    }
    const F osF = S::set(a.os);
    const F incRep = S::gather(t.bucketTop, detail::ft::levelBucket<S>(S::mul(inc, osF)));
    const F rateRep = S::fma(S::mul(phi, dS), osF, S::mul(S::mul(incRep, dir), s));  // base-rate ψ per sample (signed)
    // A tap's level speed: |a′·ψ′ + ∂a/∂w·Δw| (analytic: smooth except where a′ jumps, which the corrections
    // handle), scaled by the band headroom and the fold's spread.
    const F speedA = S::mul(S::abs(S::fma(S::mul(dadw, dW), osF, S::mul(rateRep, slopeA))), lvScale);
    const Lv<S> lvA = levelsFor<S>(t, speedA);
    const Pick<S> pk = posSpl ? pkSpl : pickFrames<S>(t, S::load(a.in[1] + o));
    F v = readPair<S>(t.data, lvA, ta, pk);
    F speedB = S::zero();
    if constexpr (M == WarpMode::Mirror) {
      speedB = S::mul(S::abs(rateRep), lvScale);
      if (S::any(S::ne(mix, S::zero()))) {
        const F vb = readPair<S>(t.data, levelsFor<S>(t, speedB), tb, pk);
        v = S::fma(mix, S::sub(vb, v), v);
      }
    }
    F y = v;
    if (a.foldOn) {
      const F soft = S::lt(theta, S::set(static_cast<float>(0.5 * kPi)));
      const F norm = S::select(soft, S::div(S::set(1.0f), S::exactSin(theta)), S::set(1.0f));
      const F folded = S::mul(S::exactSin(S::mul(theta, v)), norm);
      y = S::select(S::eq(theta, S::zero()), v, folded);
    }
    if constexpr (kCorr) {
      float* slot = st.ring[st.ringPos];
      S::store(slot, S::add(S::load(slot), y));
      // Events: φ wrapped, ψ wrapped (sub-period count changed), or a kink crossed (the lane handler decides).
      // TMM.18: either way (a backward φ wraps from near 0 to near 1).
      const F wrapped = S::band(primed, S::gt(S::abs(d0), S::set(0.5f)));
      const F prevSub = S::load(st.prevSub);
      F ev = S::bor(wrapped, S::band(primed, S::ne(sub, prevSub)));
      if constexpr (M != WarpMode::Bend) {
        const F prevPsi = S::load(st.prevPsi);
        const F k = M == WarpMode::Mirror ? half : S::fma(warp, S::set(static_cast<float>(-(0.5 - kPulseMinWidth))), half);
        const F on = M == WarpMode::Mirror ? S::ne(mix, S::zero()) : S::ne(warp, S::zero());
        // Crossed upwards (forward) or downwards (TMM.18: backward).
        ev = S::bor(ev, S::band(S::band(primed, on), S::bor(S::band(S::lt(prevPsi, k), S::ge(psi, k)),
                                                             S::band(S::ge(prevPsi, k), S::lt(psi, k)))));
        if constexpr (M == WarpMode::Mirror)  // the warp crossed 0
          ev = S::bor(ev, S::band(primed, S::bxor(S::gt(warp, S::zero()), S::gt(S::sub(warp, dW), S::zero()))));
      }
      if (S::any(ev)) {
        alignas(32) float m[8], vPhi[8], vPrevPhi[8], vPsi[8], vPrevPsi[8], vWrap[8], vS[8], vWarp[8], vFold[8], vInc[8],
            vPos[8], vSa[8], vSb[8];
        alignas(32) float vRate[8], vPsa[8], vPsb[8], vDw[8], vDir[8];
        S::store(vDw, dW);
        S::store(vDir, S::mul(incDir, s));  // ψ's rate at φ = 0 (its sign: φ's direction)
        S::store(vSa, speedA);
        S::store(vSb, speedB);
        S::store(vRate, rate);
        S::store(vPsa, S::load(st.prevSpA));
        S::store(vPsb, S::load(st.prevSpB));
        S::store(m, ev);
        S::store(vPhi, phi);
        S::store(vPrevPhi, prevPhi);
        S::store(vPsi, psi);
        S::store(vPrevPsi, S::load(st.prevPsi));
        S::store(vWrap, wrapped);
        S::store(vS, s);
        S::store(vWarp, warp);
        S::store(vFold, fold);
        S::store(vInc, inc);
        S::store(vPos, S::load(a.in[1] + (posSpl ? 0 : o)));
        for (uint32_t l = 0; l < W; ++l)
          if (std::bit_cast<uint32_t>(m[l]) != 0)
            laneEvents(t, st, l, M, vPhi[l], vPrevPhi[l], vPsi[l], vPrevPsi[l], std::bit_cast<uint32_t>(vWrap[l]) != 0,
                       vS[l], vWarp[l], vFold[l], vInc[l], vPos[l], vRate[l], vSa[l], vSb[l], vPsa[l], vPsb[l], vDw[l],
                       vDir[l]);
      }
      float* outSlot = st.ring[(st.ringPos + kD) & (kRing - 1)];
      S::store(out + o, S::load(outSlot));
      S::store(outSlot, S::zero());
      st.ringPos = (st.ringPos + 1) & (kRing - 1);
      S::store(st.prevSub, sub);
      S::store(st.prevSpA, speedA);
      S::store(st.prevSpB, speedB);
    } else {
      S::store(out + o, y);
    }
    S::store(st.prevPsi, psi);
    S::store(st.prevPhase, phi);
    S::store(st.primed, S::ones());
  }
  if (a.last && n > 0) {
    // The value a param-rate control ramps from next block. AU.fold: also after an audio-rate block (the compiler's
    // param ramp, TX.smooth, feeds a moving param per sample, then the param itself again once it rests): the last
    // frame's value, so the next param-rate block continues from where the ramp ended instead of jumping back to the
    // value before it (a click at the end of every knob move or lane step on a live control).
    for (int c = 0; c < 3; ++c)
      S::store(st.prevCtl[c], S::load(a.in[2 + c] + (splBit(a.spl, 2 + c) ? 0 : static_cast<size_t>(n - 1) * W)));
  }
}

// n frames, crossfaded while the state's fade runs (AU.fold: frame_table_slot.h "Publication", chunk by chunk).
template <class S, WarpMode M, bool kCorr>
void liveSpan(const FrameTableSlot::Entry& newest, WavetableLiveState& s, const LiveArgs& a0, uint32_t n,
              float* out) noexcept {  // RT-safe
  constexpr uint32_t W = S::kW;
  const uint32_t fadeFrames = frameTableFadeFrames(s.sampleRate > 0.0f ? s.sampleRate : newest.view.sampleRate);
  uint32_t f0 = 0;
  while (f0 < n) {
    const FrameTableFade fd = frameTableFade(&newest, s, fadeFrames);
    LiveArgs a = a0;
    for (int i = 0; i < 5; ++i)
      if (!splBit(a.spl, static_cast<uint32_t>(i))) a.in[i] += static_cast<size_t>(f0) * W;
    a.f0 += f0;
    float* o = out + static_cast<size_t>(f0) * W;
    if (fd.from == nullptr) {
      liveRun<S, M, kCorr>(fd.to->view, s, a, n - f0, o);
      return;
    }
    uint32_t m = n - f0 < kFadeChunk ? n - f0 : kFadeChunk;
    if (s.fadeLeft < m) m = s.fadeLeft;
    alignas(32) float old[kFadeChunk * 8];
    WavetableLiveState so = s;  // the old table's run on a copy (its corrections and history are discarded)
    LiveArgs ao = a;
    ao.last = false;
    a.last = a0.last && f0 + m == n;
    liveRun<S, M, kCorr>(fd.from->view, so, ao, m, old);
    liveRun<S, M, kCorr>(fd.to->view, s, a, m, o);
    frameTableFadeMix(o, old, m, W, fadeFrames, s.fadeLeft);
    s.fadeLeft -= m;
    f0 += m;
  }
}

bool allZero(const float* p, uint32_t n) noexcept {
  for (uint32_t i = 0; i < n; ++i)
    if (p[i] != 0.0f) return false;
  return true;
}


template <uint32_t L>
void wavetableLiveK(const KernelArgs& k) noexcept {  // RT-safe
  using S = std::conditional_t<L == 8, S8, S1>;
  if (k.frames == 0) return;
  auto& s = *static_cast<WavetableLiveState*>(k.state);
  const auto& slot = *static_cast<const FrameTableSlot*>(k.resource);
  const FrameTableSlot::Entry& e = *slot.acquire();
  LiveArgs a;
  for (int i = 0; i < 5; ++i) a.in[i] = k.in[i];
  a.spl = k.splMask;
  a.f0 = 0;
  a.total = k.frames;
  a.last = true;
  // Fold costs two sines per sample: skipped while it is a param that is 0 now and was 0 before.
  a.foldOn = !(splBit(k.splMask, 3) && allZero(k.in[3], L) && allZero(s.prevCtl[1], L));
  const uint32_t os = liveFactor(s.sampleRate, e.view);
  a.os = static_cast<float>(os);
  const float modeF = k.in[5][0];
  const uint32_t mode = modeF >= 1.5f ? 2u : modeF >= 0.5f ? 1u : 0u;
  const bool corr = k.in[6][0] != 0.0f;
  float* out = k.out[0];
  switch (mode * 2 + (corr ? 1u : 0u)) {
    case 0: liveSpan<S, WarpMode::Bend, false>(e, s, a, k.frames, out); break;
    case 1: liveSpan<S, WarpMode::Bend, true>(e, s, a, k.frames, out); break;
    case 2: liveSpan<S, WarpMode::Mirror, false>(e, s, a, k.frames, out); break;
    case 3: liveSpan<S, WarpMode::Mirror, true>(e, s, a, k.frames, out); break;
    case 4: liveSpan<S, WarpMode::PulseWidth, false>(e, s, a, k.frames, out); break;
    default: liveSpan<S, WarpMode::PulseWidth, true>(e, s, a, k.frames, out); break;
  }
  VoiceTap* tap = slot.tap();
  if (!tap->wanted()) return;
  // The tap samples at the base rate: inside an oversample(n) region, every n-th frame.
  float position[8];
  const bool posSpl = splBit(k.splMask, 1);
  const float* lastPos = posSpl ? k.in[1] : k.in[1] + static_cast<size_t>(k.frames - 1) * L;
  for (uint32_t l = 0; l < L; ++l) position[l] = lastPos[l];
  float inc[8];
  for (uint32_t l = 0; l < L; ++l) inc[l] = s.prevDelta[l] * static_cast<float>(os);
  const bool phSpl = splBit(k.splMask, 0);
  tap->write(s.group, L, k.in[0], phSpl ? 0u : L * os, phSpl ? k.frames : k.frames / os, true, position, inc);
}

// ---- wavetable_shape --------------------------------------------------------------------------------------------
struct ShapeArgs {
  const float* x;
  const float* pos;
  bool xSpl, posSpl;
  float decay;
  float headroom;
};

// G's difference parts at the level pair: c₀·Δcu + ΔP + T(0)·(Δu − Δcu), from the frame's DC and edge.
template <class S>
__forceinline typename S::F edgeValue(const FrameTableView& t, typename S::I level, const Pick<S>& pk,
                                      typename S::I frameK) noexcept {
  const auto idx = S::addi(S::muli(frameK, S::seti(static_cast<int32_t>(t.levels))), level);
  auto e = S::gather(t.levelEdge, idx);
  if (pk.blend) {  // only lanes that blend step to frame k + 1 (a lane at k = F − 1 has none)
    const auto moving = S::asI(S::ne(pk.w, S::zero()));
    const auto e1 = S::gather(t.levelEdge, S::addi(idx, S::andi(moving, S::seti(static_cast<int32_t>(t.levels)))));
    e = S::fma(pk.w, S::sub(e1, e), e);
  }
  return e;
}

template <class S>
void shapeRun(const FrameTableView& t, WavetableShapeState& st, const ShapeArgs& a, uint32_t n, float* out) noexcept {
  using F = typename S::F;
  using I = typename S::I;
  constexpr uint32_t W = S::kW;
  const F half = S::set(0.5f), one = S::set(1.0f), zero = S::zero();
  auto pickAt = [&](uint32_t f, I& frameK) {
    const F u = S::load(a.pos + (a.posSpl ? 0 : static_cast<size_t>(f) * W));
    if (t.frames <= 1) {
      frameK = S::seti(0);
      return Pick<S>{S::seti(0), S::seti(0), S::zero(), false};
    }
    F w;
    detail::ft::framePosition<S>(t, u, frameK, w);
    const auto stride = S::seti(static_cast<int32_t>(t.frameStride));
    const auto moving = S::ne(w, S::zero());
    // A lane resting on the last frame has k = F − 1 and no k + 1 (w = 0): the edge gather must not step past it.
    return Pick<S>{S::muli(frameK, stride), S::andi(S::asI(moving), stride), w, S::any(moving)};
  };
  for (uint32_t f = 0; f < n; ++f) {
    const size_t o = static_cast<size_t>(f) * W;
    const F primed = S::load(st.primed);
    const F x = S::load(a.x + (a.xSpl ? 0 : o));
    const F u1 = S::fma(x, half, half);
    const F u0 = S::select(primed, S::load(st.prevU), u1);
    // NaN input: read as u = 0.5 (x = 0), never through a gather index.
    const F u1s = S::select(S::eq(u1, u1), u1, half);
    const F u0s = S::select(S::eq(u0, u0), u0, half);
    const F cu1 = S::min(S::max(u1s, zero), one);
    const F cu0 = S::min(S::max(u0s, zero), one);
    const F dcu = S::sub(cu1, cu0);
    const F hold = S::max(S::abs(dcu), S::mul(S::load(st.hold), S::set(a.decay)));
    S::store(st.hold, hold);
    const F lvSpeed = S::mul(hold, S::set(a.headroom));  // the band headroom, as wavetable_live
    const Lv<S> lv = levelsFor<S>(t, lvSpeed);
    I frameK;
    const Pick<S> pk = pickAt(f, frameK);
    const F du = S::sub(u1s, u0s);
    const F useAdaa = S::band(primed, S::ge(S::abs(du), S::set(kShapeAdaaMin)));
    F y = zero;
    if (S::any(useAdaa)) {
      // ΔP at both levels, the frame's DC, T(0) at both levels.
      const F p1 = readAt<S>(t.adData, lv.info, cu1, pk), p0 = readAt<S>(t.adData, lv.info, cu0, pk);
      F dP = S::sub(p1, p0);
      const I lvl = gatherU<S>(t.bucketLevelIndex, detail::ft::levelBucket<S>(lvSpeed));
      F e0 = edgeValue<S>(t, lvl, pk, frameK);
      if (S::any(S::gt(lv.w, zero))) {
        const F q1 = readAt<S>(t.adData, lv.rich, cu1, pk), q0 = readAt<S>(t.adData, lv.rich, cu0, pk);
        dP = S::fma(lv.w, S::sub(S::sub(q1, q0), dP), dP);
        const I rl = S::maxi(S::subi(lvl, S::seti(1)), S::seti(0));
        const F er = edgeValue<S>(t, rl, pk, frameK);
        e0 = S::fma(lv.w, S::sub(er, e0), e0);
      }
      F c0 = S::gather(t.frameDc, frameK);
      if (pk.blend) {
        const F c1 = S::gather(t.frameDc, S::addi(frameK, S::andi(S::asI(S::ne(pk.w, zero)), S::seti(1))));
        c0 = S::fma(pk.w, S::sub(c1, c0), c0);
      }
      const F dG = S::fma(c0, dcu, S::fma(e0, S::sub(du, dcu), dP));
      y = S::div(dG, S::select(useAdaa, du, one));
    }
    if (S::any(S::bnot(useAdaa))) {
      const F mid = S::min(S::max(S::mul(S::add(u0s, u1s), half), zero), one);
      const F ym = readPair<S>(t.data, lv, mid, pk);
      y = S::select(useAdaa, y, ym);
    }
    S::store(out + o, y);
    S::store(st.prevU, u1s);
    S::store(st.primed, S::ones());
  }
}

// n frames, crossfaded while the state's fade runs (AU.fold: frame_table_slot.h "Publication", chunk by chunk). A
// table built without the antiderivative (not Shape mode's: the engine's default, one still being built for the mode)
// is not read: silence rather than what is not there; a fade from one is skipped.
template <class S>
void shapeSpan(const FrameTableSlot::Entry& newest, WavetableShapeState& s, const ShapeArgs& a0, uint32_t n,
               float* out) noexcept {  // RT-safe
  constexpr uint32_t W = S::kW;
  const uint32_t fadeFrames = frameTableFadeFrames(s.sampleRate > 0.0f ? s.sampleRate : newest.view.sampleRate);
  uint32_t f0 = 0;
  while (f0 < n) {
    FrameTableFade fd = frameTableFade(&newest, s, fadeFrames);
    if (fd.from != nullptr && (fd.from->view.adData == nullptr || fd.to->view.adData == nullptr)) {
      s.fadeLeft = 0;
      continue;
    }
    ShapeArgs a = a0;
    if (!a.xSpl) a.x += static_cast<size_t>(f0) * W;
    if (!a.posSpl) a.pos += static_cast<size_t>(f0) * W;
    float* o = out + static_cast<size_t>(f0) * W;
    if (fd.from == nullptr) {
      if (fd.to->view.adData == nullptr) std::memset(o, 0, sizeof(float) * (n - f0) * W);
      else shapeRun<S>(fd.to->view, s, a, n - f0, o);
      return;
    }
    uint32_t m = n - f0 < kFadeChunk ? n - f0 : kFadeChunk;
    if (s.fadeLeft < m) m = s.fadeLeft;
    alignas(32) float old[kFadeChunk * 8];
    WavetableShapeState so = s;
    shapeRun<S>(fd.from->view, so, a, m, old);
    shapeRun<S>(fd.to->view, s, a, m, o);
    frameTableFadeMix(o, old, m, W, fadeFrames, s.fadeLeft);
    s.fadeLeft -= m;
    f0 += m;
  }
}

template <uint32_t L>
void wavetableShapeK(const KernelArgs& k) noexcept {  // RT-safe
  using S = std::conditional_t<L == 8, S8, S1>;
  if (k.frames == 0) return;
  auto& s = *static_cast<WavetableShapeState*>(k.state);
  const auto& slot = *static_cast<const FrameTableSlot*>(k.resource);
  const FrameTableSlot::Entry& e = *slot.acquire();
  float* out = k.out[0];
  ShapeArgs a;
  a.x = k.in[0];
  a.pos = k.in[1];
  a.xSpl = splBit(k.splMask, 0);
  a.posSpl = splBit(k.splMask, 1);
  a.decay = s.sampleRate > 0.0f ? static_cast<float>(std::exp(-1.0 / (kShapeHoldSeconds * s.sampleRate))) : 0.999f;
  const uint32_t os = liveFactor(s.sampleRate, e.view);
  a.headroom = liveHeadroom(os);
  shapeSpan<S>(e, s, a, k.frames, out);
  VoiceTap* tap = slot.tap();
  if (!tap->wanted()) return;
  float position[8];
  const float* lastPos = a.posSpl ? k.in[1] : k.in[1] + static_cast<size_t>(k.frames - 1) * L;
  for (uint32_t l = 0; l < L; ++l) position[l] = lastPos[l];
  // The dots sit at the input's table position u = clamp((x + 1)/2) (no wrap), sampled at the base rate.
  float inc[8];
  for (uint32_t l = 0; l < L; ++l) inc[l] = s.hold[l] * static_cast<float>(os);
  if (a.xSpl) {
    float u[8];
    for (uint32_t l = 0; l < L; ++l) u[l] = std::fmin(std::fmax(std::fma(a.x[l], 0.5f, 0.5f), 0.0f), 1.0f);
    tap->write(s.group, L, u, 0u, k.frames / os, false, position, inc);
    return;
  }
  constexpr uint32_t kChunk = 64;
  alignas(32) float u[kChunk * L];
  const uint32_t base = k.frames / os;
  for (uint32_t done = 0; done < base;) {
    const uint32_t m = base - done < kChunk ? base - done : kChunk;
    for (uint32_t f = 0; f < m; ++f)
      for (uint32_t l = 0; l < L; ++l) {
        const float x = a.x[(static_cast<size_t>(done + f) * os) * L + l];
        u[f * L + l] = std::fmin(std::fmax(std::fma(x, 0.5f, 0.5f), 0.0f), 1.0f);
      }
    tap->write(s.group, L, u, L, m, false, position, inc);
    done += m;
  }
}

}  // namespace

KernelFn findWaveLiveKernel(Atom a, uint32_t lanes, uint32_t) noexcept {
  if (lanes != 1 && lanes != 8) return nullptr;
  switch (a) {
    case Atom::WavetableLive: return lanes == 1 ? &wavetableLiveK<1> : &wavetableLiveK<8>;
    case Atom::WavetableShape: return lanes == 1 ? &wavetableShapeK<1> : &wavetableShapeK<8>;
    default: return nullptr;
  }
}

}  // namespace detail
}  // namespace pq::dsp
