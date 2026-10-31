#pragma once

// Hand-written C++ references of the stock devices in stdlib/devices/ (T4.9 Utility + Saturator, T4.10 EQ Eight;
// tests and benchmarks only, never shipped): the same float32 formulas as the .pqp files, written the way a DSP
// programmer would (state in locals, one loop over the samples, every band / allpass of a sample interleaved), so
// the tests can hold the compiled devices to them and bench/jit/bench_devices.cpp can compare their cost.
// Fused multiply-adds sit where the optimiser fuses (a·b + c at equal rates, stdlib_reference.h's convention).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "pq/core/fast_math.h"
#include "stdlib_reference.h"

namespace devref {

using stdref::fma_;

inline float fastTanh(float x) { return pq::fastTanh(x); }
inline float fastSin(float x) { return pq::fastSin(x); }
// The `sine` atom (pq/dsp kernels_math.cpp): fastSin((phase − round(phase))·2π), round to nearest even.
inline float sinePhase(float ph) { return pq::fastSin((ph - std::nearbyint(ph)) * stdref::kTwoPi); }

// ---- svf_core_fast (stdlib/filters/svf_fast.pqp): svf_core's filter, the state update two FMAs deep ------------
struct FastSvfCore {
  float ic1 = 0.0f, ic2 = 0.0f;
  struct Coef {
    float d1, b1, b2;  // 2·a1 − 2, 2·a2, 2·a3
  };
  static Coef coef(float g, float k) {
    Coef c;
    const float gk = g + k;
    const float a1 = 1.0f / fma_(g, gk, 1.0f);
    c.d1 = -2.0f * g * gk * a1;
    c.b1 = 2.0f * g * a1;
    c.b2 = c.b1 * g;
    return c;
  }
  void tick(float x, const Coef& c, float& band, float& low) {
    const float v = x - ic2;
    const float n1 = fma_(c.b1, v, fma_(c.d1, ic1, ic1));
    const float n2 = fma_(c.b2, v, fma_(c.b1, ic1, ic2));
    band = 0.5f * (ic1 + n1);
    low = 0.5f * (ic2 + n2);
    ic1 = n1;
    ic2 = n2;
  }
};

// ---- Utility (stdlib/devices/utility.pqp) --------------------------------------------------------------------------
struct UtilityParams {
  float chan = 0.0f;  // 0 Stereo, 1 Left, 2 Right, 3 Swap
  bool invL = false, invR = false;
  float width = 1.0f;
  bool msMode = false;
  float ms = 0.0f;
  bool mono = false;
  float bassFreq = 120.0f;
  float balance = 0.0f;
  float gain = 1.0f;  // linear
  bool mute = false;
};
struct UtilityConf {
  bool bassMono = false, audition = false, dc = false;
};

struct Utility {
  float ed[4] = {}, es[6] = {}, td[4] = {}, ts[6] = {};  // smoothing distances and the previous targets
  bool started = false;  // the first sample starts at the targets (M4 review L10)
  FastSvfCore ap, h1, h2, l1, l2;
  stdref::DcBlock<float> dcL, dcR;
  UtilityConf conf;
  float sr = 48000.0f;
  float out[2] = {}, mid = 0.0f, side = 0.0f, bass = 0.0f;
  bool wantBass = true;  // compute the `bass` export (tests); a benchmark reading only `out` turns it off
  // Param-rate values (prepare(): once per block, as the compiled Control region).
  float direct[4] = {}, split[6] = {}, k = 0.0f, rc = 0.0f;
  FastSvfCore::Coef bc{};

  void prepare(const UtilityParams& p) {
    const int ci = static_cast<int>(std::clamp(std::floor(p.chan), 0.0f, 3.0f));
    const float cLL = ci <= 1 ? 1.0f : 0.0f, cLR = ci >= 2 ? 1.0f : 0.0f;
    const float cRL = ci == 1 || ci == 3 ? 1.0f : 0.0f, cRR = ci == 0 || ci == 2 ? 1.0f : 0.0f;
    const float pL = p.invL ? -1.0f : 1.0f, pR = p.invR ? -1.0f : 1.0f;
    const float m = p.mono ? 1.0f : p.msMode ? std::min(1.0f, 1.0f - p.ms) : 1.0f;
    const float s = p.mono ? 0.0f : p.msMode ? std::min(1.0f, 1.0f + p.ms) : p.width;
    const float mL = 0.5f * m * (pL * cLL + pR * cRL), mR = 0.5f * m * (pL * cLR + pR * cRR);
    const float sL = 0.5f * s * (pL * cLL - pR * cRL), sR = 0.5f * s * (pL * cLR - pR * cRR);
    const float g = p.mute || p.gain <= 0.00031622776f ? 0.0f : p.gain;
    const float oL = g * std::min(1.0f, 1.0f - p.balance), oR = g * std::min(1.0f, 1.0f + p.balance);
    k = stdref::vexp(-1.0f / (0.002f * sr));
    rc = stdref::DcBlock<float>::coef(5.0f, sr);
    const float d[4] = {oL * (mL + sL), oL * (mR + sR), oR * (mL - sL), oR * (mR - sR)};
    const float sp[6] = {mL, mR, sL, sR, oL, oR};
    std::copy(d, d + 4, direct);
    std::copy(sp, sp + 6, split);
    bassCoef(p.bassFreq);
  }
  void bassCoef(float bassFreq) { bc = FastSvfCore::coef(stdref::prewarp(bassFreq, sr), 1.4142135623730951f); }

  // One sample with the prepared coefficients.
  void process(float L, float R) {
    if (!started) {
      std::copy(direct, direct + 4, td);
      std::copy(split, split + 6, ts);
      started = true;
    }
    if (conf.dc) {
      L = dcL.tick(L, rc);
      R = dcR.tick(R, rc);
    }
    if (!conf.bassMono) {
      float sd[4];
      for (int i = 0; i < 4; ++i) {
        ed[i] = fma_(ed[i], k, (td[i] - direct[i]) * k);
        td[i] = direct[i];
        sd[i] = direct[i] + ed[i];
      }
      out[0] = fma_(sd[0], L, sd[1] * R);
      out[1] = fma_(sd[2], L, sd[3] * R);
      mid = 0.5f * (out[0] + out[1]);
      side = 0.5f * (out[0] - out[1]);
      bass = 0.0f;
      return;
    }
    float ss[6];
    for (int i = 0; i < 6; ++i) {
      es[i] = fma_(es[i], k, (ts[i] - split[i]) * k);
      ts[i] = split[i];
      ss[i] = split[i] + es[i];
    }
    const float M = fma_(ss[0], L, ss[1] * R), S = fma_(ss[2], L, ss[3] * R);
    const float o4 = ss[4], o5 = ss[5];
    float band, low;
    ap.tick(M, bc, band, low);
    const float Mp = M - 2.8284271247461903f * band;
    h1.tick(S, bc, band, low);
    const float S1 = S - 1.4142135623730951f * band - low;
    h2.tick(S1, bc, band, low);
    const float Sp = S1 - 1.4142135623730951f * band - low;
    mid = M;
    side = S;
    if (conf.audition || wantBass) {
      float l1b, l1l, l2b, l2l;
      l1.tick(M, bc, l1b, l1l);
      l2.tick(l1l, bc, l2b, l2l);
      bass = l2l;
    }
    if (conf.audition) {
      out[0] = o4 * bass;
      out[1] = o5 * bass;
    } else {
      out[0] = o4 * (Mp + Sp);
      out[1] = o5 * (Mp - Sp);
    }
  }

  // prepare + process (tests: params per sample).
  void tick(float L, float R, const UtilityParams& p) {
    prepare(p);
    process(L, R);
  }
};

// ---- oversampling stages (stdlib/util/oversample.pqp) --------------------------------------------------------------
// One first-order allpass (c + z⁻¹)/(1 + c·z⁻¹): y = −c·y1 + (c·x + x1).
struct Ap {
  float x1 = 0.0f, y1 = 0.0f;
  float tick(float x, float c) {
    const float y = fma_(-c, y1, fma_(c, x, x1));
    x1 = x;
    y1 = y;
    return y;
  }
};
// A chain of allpasses (one path of a half-band).
template <int N>
struct ApChain {
  Ap a[N];
  float tick(float x, const float (&c)[N]) {
    for (int i = 0; i < N; ++i) x = a[i].tick(x, c[i]);
    return x;
  }
};
// Stage 1 (6 coefficients), 2 (3), 3 (2); path 0 = even-indexed coefficients.
inline constexpr float kHb1P0[3] = {0.044181909085223188f, 0.33074416170770549f, 0.70205285207742341f};
inline constexpr float kHb1P1[3] = {0.1641888597263377f, 0.51508694250895914f, 0.89488964220359257f};
inline constexpr float kHb2P0[2] = {0.063787836824294675f, 0.66854879318358884f};
inline constexpr float kHb2P1[1] = {0.26678096249714506f};
inline constexpr float kHb3P0[1] = {0.11030120991226221f};
inline constexpr float kHb3P1[1] = {0.5367721236663745f};

// A half-band stage: up (one sample in, two out) and down (two in, one out) at its low rate.
template <int N0, int N1>
struct HbUp {
  ApChain<N0> p0;
  ApChain<N1> p1;
  void tick(float x, const float (&c0)[N0], const float (&c1)[N1], float& e, float& o) {
    e = p0.tick(x, c0);
    o = p1.tick(x, c1);
  }
};
template <int N0, int N1>
struct HbDown {
  ApChain<N0> p0;
  ApChain<N1> p1;
  float tick(float e, float o, const float (&c0)[N0], const float (&c1)[N1]) {
    const float a = p0.tick(o, c0);
    const float b = p1.tick(e, c1);
    return 0.5f * (a + b);
  }
};

// Oversampling by `factor` (1, 2, 4, 8) around a per-sample function: up → f on every high-rate sample → down.
// The stages run their allpasses through the samples of one base sample in time order, as the subcircuits do.
struct Oversampler {
  HbUp<3, 3> up1;
  HbDown<3, 3> down1;
  HbUp<2, 1> up2;
  HbDown<2, 1> down2;
  HbUp<1, 1> up3;
  HbDown<1, 1> down3;
  template <class F>
  float tick(float x, int factor, F&& f) {
    if (factor <= 1) return f(x);
    float s[2];
    up1.tick(x, kHb1P0, kHb1P1, s[0], s[1]);
    if (factor == 2) return down1.tick(f(s[0]), f(s[1]), kHb1P0, kHb1P1);
    float q[4];
    for (int i = 0; i < 2; ++i) up2.tick(s[i], kHb2P0, kHb2P1, q[2 * i], q[2 * i + 1]);
    float r[4];
    if (factor == 4) {
      for (int i = 0; i < 4; ++i) r[i] = f(q[i]);
    } else {
      float o[8];
      for (int i = 0; i < 4; ++i) up3.tick(q[i], kHb3P0, kHb3P1, o[2 * i], o[2 * i + 1]);
      for (int i = 0; i < 8; ++i) o[i] = f(o[i]);
      for (int i = 0; i < 4; ++i) r[i] = down3.tick(o[2 * i], o[2 * i + 1], kHb3P0, kHb3P1);
    }
    float d[2];
    for (int i = 0; i < 2; ++i) d[i] = down2.tick(r[2 * i], r[2 * i + 1], kHb2P0, kHb2P1);
    return down1.tick(d[0], d[1], kHb1P0, kHb1P1);
  }
};

// Up- and down-sampling chains of the three stages, run separately (the Saturator's dry/wet mix inside the
// oversampled region, M4 review M10: two up chains (the colour EQ's output, the dry input) and two down chains (the
// mix, the `wet` export)).
struct OsUp {
  HbUp<3, 3> up1;
  HbUp<2, 1> up2;
  HbUp<1, 1> up3;
  // x → `factor` samples in time order (factor 2, 4, 8).
  void tick(float x, int factor, float* o) {
    float s[2];
    up1.tick(x, kHb1P0, kHb1P1, s[0], s[1]);
    if (factor == 2) {
      o[0] = s[0], o[1] = s[1];
      return;
    }
    float q[4];
    for (int i = 0; i < 2; ++i) up2.tick(s[i], kHb2P0, kHb2P1, q[2 * i], q[2 * i + 1]);
    if (factor == 4) {
      for (int i = 0; i < 4; ++i) o[i] = q[i];
      return;
    }
    for (int i = 0; i < 4; ++i) up3.tick(q[i], kHb3P0, kHb3P1, o[2 * i], o[2 * i + 1]);
  }
};
struct OsDown {
  HbDown<3, 3> down1;
  HbDown<2, 1> down2;
  HbDown<1, 1> down3;
  float tick(const float* in, int factor) {
    float r[4], d[2];
    if (factor == 2) return down1.tick(in[0], in[1], kHb1P0, kHb1P1);
    if (factor == 4) {
      for (int i = 0; i < 4; ++i) r[i] = in[i];
    } else {
      for (int i = 0; i < 4; ++i) r[i] = down3.tick(in[2 * i], in[2 * i + 1], kHb3P0, kHb3P1);
    }
    for (int i = 0; i < 2; ++i) d[i] = down2.tick(r[2 * i], r[2 * i + 1], kHb2P0, kHb2P1);
    return down1.tick(d[0], d[1], kHb1P0, kHb1P1);
  }
};

// ---- Saturator (stdlib/devices/saturator.pqp) ----------------------------------------------------------------------
struct SatParams {
  float drive = 1.0f, output = 1.0f, dryWet = 1.0f;  // linear
  float base = 1.0f, colorFreq = 1000.0f, colorWidth = 0.7f, depth = 1.0f;
  float wsDrive = 0.5f, wsLin = 0.5f, wsCurve = 0.5f, wsDamp = 0.0f, wsDepth = 0.0f, wsPeriod = 0.0f;
};
struct SatConf {
  int type = 0, hq = 1;  // Analog Clip (M4 review L11: Live's default)
  bool color = false, softClip = false;
};

// The curve of `type` at v (sat_shape without output gain and soft clip).
inline float satCurve(int type, float v, const SatParams& p) {
  switch (type) {
    case 0: {
      const float ca = std::clamp(v, -1.5f, 1.5f);
      return ca - 0.14814814814814814f * ca * ca * ca;
    }
    case 1: return sinePhase(std::clamp(v * 0.15915494309189535f, -0.25f, 0.25f));
    case 2: {
      const float av = std::fabs(v);
      return v * (av + 2.0f) / (fma_(v, v, av) + 1.0f);
    }
    case 3: return fastTanh(v);
    case 4: {
      const float v2 = v * v;
      return v / std::sqrt(std::sqrt(fma_(v2, v2, 1.0f)));
    }
    case 5: return sinePhase(v * 0.15915494309189535f);
    case 6: return std::clamp(v, -1.0f, 1.0f);
    case 7: {
      const float t = std::clamp(v, -1.0f, 1.0f);
      const float wf = fma_(7.5f, p.wsPeriod, 0.5f);
      const float wg = fma_(0.01f * p.wsDamp, p.wsDamp, 1e-30f);
      const float shape = p.wsLin * v + p.wsCurve * (1.5f * t - 0.5f * t * t * t) + p.wsDepth * sinePhase(wf * t);
      const float gate = (v * v + 1e-30f) / (v * v + wg);
      return fma_(p.wsDrive, shape * gate - v, v);
    }
    default: {  // the built-in identity shaper on [−1, 1] (two points), as the `shaper` atom reads it
      const float t = std::min(std::max((v + 1.0f) * 0.5f, 0.0f), 1.0f);
      return fma_(t, 2.0f, -1.0f);
    }
  }
}

struct Saturator {
  SatConf conf;
  float sr = 48000.0f;
  OsUp upP[2], upX[2];    // the colour EQ's output; the dry input (with colour only: else upP's samples)
  OsDown downY[2], downW[2];  // the mix; the `wet` export
  stdref::Svf<float> shelf[2], bell[2];
  stdref::Svf<float>::Coef shelfC{}, bellC{};
  SatParams p{};
  float out[2] = {}, driven[2] = {}, wet[2] = {};

  void prepare(const SatParams& q) {
    p = q;
    if (conf.color) {
      shelfC = stdref::Svf<float>::coef(200.0f, 0.7071067811865476f, p.base, sr);
      bellC = stdref::Svf<float>::coef(p.colorFreq, p.colorWidth, p.depth, sr);
    }
  }
  template <int kType>
  float shape(float v) const {
    const float g = satCurve(kType, v, p) * p.output;
    return conf.softClip ? fastTanh(g) : g;
  }
  // One stereo sample with the prepared params (the curve type as a template argument, as a hand-written
  // saturator would specialise it). Drive, curve, output and the dry/wet mix on every (oversampled) sample.
  template <int kType>
  void processT(const float (&x)[2]) {
    for (int ch = 0; ch < 2; ++ch) {
      float pre = x[ch];
      if (conf.color) {
        pre = shelf[ch].tick(pre, shelfC).lowshelf;
        pre = bell[ch].tick(pre, bellC).bell;
      }
      driven[ch] = pre * p.drive;
      if (conf.hq <= 1) {
        const float w = shape<kType>(driven[ch]);
        wet[ch] = w;
        out[ch] = fma_(p.dryWet, w - x[ch], x[ch]);
        continue;
      }
      float ps[8], ds[8], ws[8], ys[8];
      upP[ch].tick(pre, conf.hq, ps);
      if (conf.color) upX[ch].tick(x[ch], conf.hq, ds);
      else std::copy(ps, ps + 8, ds);
      for (int i = 0; i < conf.hq; ++i) {
        ws[i] = shape<kType>(ps[i] * p.drive);
        ys[i] = fma_(p.dryWet, ws[i] - ds[i], ds[i]);
      }
      out[ch] = downY[ch].tick(ys, conf.hq);
      wet[ch] = downW[ch].tick(ws, conf.hq);
    }
  }
  void process(const float (&x)[2]) {
    switch (conf.type) {
      case 0: processT<0>(x); break;
      case 1: processT<1>(x); break;
      case 2: processT<2>(x); break;
      case 3: processT<3>(x); break;
      case 4: processT<4>(x); break;
      case 5: processT<5>(x); break;
      case 6: processT<6>(x); break;
      case 7: processT<7>(x); break;
      default: processT<8>(x); break;
    }
  }
  void tick(const float (&x)[2], const SatParams& q) {
    prepare(q);
    process(x);
  }
};

// ---- EQ Eight (stdlib/devices/eq_eight.pqp) -------------------------------------------------------------------------
// The `pow` atom: SVML powf (lane 0 of the 8-wide call, as the 1-lane kernel).
inline float vpow(float a, float b) { return _mm256_cvtss_f32(_mm256_pow_ps(_mm256_set1_ps(a), _mm256_set1_ps(b))); }

enum EqType { kLC48 = 0, kLC12, kLS, kBell, kNotch, kHS, kHC12, kHC48 };
struct EqBand {
  float freq = 1000.0f, gain = 1.0f, q = 0.7071f;  // gain linear
};
struct EqCoef {
  float g, k, m0, m1, m2, k2, k3, k4;
};
// eq8_coef: one band's section coefficients at os·sr.
inline EqCoef eqCoef(int type, float os, const EqBand& b, bool adaptive, float scale, float sr) {
  const float ge = vpow(b.gain, scale);
  const float A = std::sqrt(ge);
  const float qe = adaptive ? b.q * std::sqrt(std::max(ge, 1.0f / ge)) : b.q;
  const float w = stdref::vtan(3.141592653589793f * std::clamp(b.freq, 0.0f, 0.49f * sr) / (os * sr));
  const float kc = 1.0f / std::max(b.q, 0.01f), kb = 1.0f / std::max(qe, 0.01f), sA = std::sqrt(A);
  EqCoef c{};
  // The 48s (M4 review H3): sections 1–3 at the Butterworth-8 dampings, section 4 carries q.
  constexpr float f1 = 1.9615705608064609f;
  switch (type) {
    case kLC48: c = {w, f1, 1, -f1, -1}; break;
    case kLC12: c = {w, kc, 1, -kc, -1}; break;
    case kLS: c = {w / sA, kb, 1, kb * (A - 1), ge - 1}; break;
    case kBell: c = {w, kb / A, 1, kb / A * (ge - 1), 0}; break;
    case kNotch: c = {w, kc, 1, -kc, 0}; break;
    case kHS: c = {w * sA, kb, ge, kb * (1 - A) * A, 1 - ge}; break;
    case kHC12: c = {w, kc, 0, 0, 1}; break;
    default: c = {w, f1, 0, 0, 1}; break;
  }
  c.k2 = 1.6629392246050905f;
  c.k3 = 1.1111404660392046f;
  c.k4 = 0.27589937928294295f * kc;
  return c;
}

// One section (eq8_sec): y = m0·x + m1·band + m2·low as the smoothed gains compute it (M4 review M13; settled):
// m2·low + (m1·band + m0·x), each a multiply-add (m0·x is x for the literal m0 = 1; a literal 0 term is left out,
// which is the same value).
struct EqSec {
  FastSvfCore core;
  float tick(float x, const FastSvfCore::Coef& c, bool, float m0, float m1, float m2) {
    float band, low;
    core.tick(x, c, band, low);
    return fma_(low, m2, fma_(band, m1, x * m0));
  }
};

// One band (eq8_band; the 2× band is the same section ticks on both oversampled samples).
struct EqBandState {
  EqSec sec[4];
  // Prepared section coefficients (param-rate: once per block).
  FastSvfCore::Coef cc[4]{};
  float mix[4][3]{};
  void prepare(int type, const EqCoef& c) {
    const bool hp = type == kLC48;
    cc[0] = FastSvfCore::coef(c.g, c.k);
    mix[0][0] = c.m0, mix[0][1] = c.m1, mix[0][2] = c.m2;
    const float ks[3] = {c.k2, c.k3, c.k4};
    for (int i = 0; i < (type == kLC48 || type == kHC48 ? 3 : 0); ++i) {
      cc[i + 1] = FastSvfCore::coef(c.g, ks[i]);
      mix[i + 1][0] = hp ? 1.0f : 0.0f;
      mix[i + 1][1] = hp ? -ks[i] : 0.0f;
      mix[i + 1][2] = hp ? -1.0f : 1.0f;
    }
  }
  float tick(int type, float x) {
    float y = sec[0].tick(x, cc[0], type != kHS && type != kHC12 && type != kHC48, mix[0][0], mix[0][1], mix[0][2]);
    if (type == kLC48 || type == kHC48)
      for (int i = 1; i < 4; ++i) y = sec[i].tick(y, cc[i], type == kLC48, mix[i][0], mix[i][1], mix[i][2]);
    return y;
  }
};

struct EqConf {
  int mode = 0, hq = 1;
  int type[2][8] = {{0, 2, 3, 3, 3, 3, 5, 7}, {0, 2, 3, 3, 3, 3, 5, 7}};
  bool on[2][8] = {{false, true, true, true, true, true, true, false}, {false, true, true, true, true, true, true, false}};
};
struct EqParams {
  EqBand band[2][8];
  bool adaptive = false;
  float scale = 1.0f, outGain = 1.0f;
  EqParams() {
    const float f[8] = {30, 100, 250, 1000, 2500, 5000, 10000, 18000};
    for (int s = 0; s < 2; ++s)
      for (int i = 0; i < 8; ++i) band[s][i].freq = f[i];
  }
};

// One chain of eight bands over one channel (eq8_chain).
struct EqChain {
  EqBandState band[8];
  HbUp<3, 3> up;
  HbDown<3, 3> down;
  float bandOut[8] = {};
  void prepare(const EqConf& conf, int set, const EqParams& p, float sr) {
    for (int i = 0; i < 8; ++i)
      if (conf.on[set][i])
        band[i].prepare(conf.type[set][i], eqCoef(conf.type[set][i], static_cast<float>(conf.hq), p.band[set][i], p.adaptive, p.scale, sr));
  }
  float tick(const EqConf& conf, int set, float x) {
    if (conf.hq <= 1) {
      for (int i = 0; i < 8; ++i) {
        if (conf.on[set][i]) x = band[i].tick(conf.type[set][i], x);
        bandOut[i] = x;
      }
      return x;
    }
    float s0, s1;
    up.tick(x, kHb1P0, kHb1P1, s0, s1);
    for (int i = 0; i < 8; ++i) {
      if (conf.on[set][i]) {
        s0 = band[i].tick(conf.type[set][i], s0);
        s1 = band[i].tick(conf.type[set][i], s1);
      }
      bandOut[i] = s0;
    }
    return down.tick(s0, s1, kHb1P0, kHb1P1);
  }
};

struct EqEight {
  EqConf conf;
  float sr = 48000.0f;
  EqChain chain[2];  // stereo: A on L (0) and R (1); L/R, M/S: A (0), B (1)
  float out[2] = {}, a[2] = {}, b = 0.0f, band[8][2] = {};
  void prepare(const EqParams& p) {
    chain[0].prepare(conf, 0, p, sr);
    chain[1].prepare(conf, conf.mode == 0 ? 0 : 1, p, sr);
  }
  void process(float L, float R, float outGain) {
    if (conf.mode == 0) {
      a[0] = chain[0].tick(conf, 0, L);
      a[1] = chain[1].tick(conf, 0, R);
      b = 0.0f;
      out[0] = a[0] * outGain;
      out[1] = a[1] * outGain;
      for (int i = 0; i < 8; ++i) band[i][0] = chain[0].bandOut[i], band[i][1] = chain[1].bandOut[i];
      return;
    }
    const float xa = conf.mode == 2 ? 0.5f * (L + R) : L, xb = conf.mode == 2 ? 0.5f * (L - R) : R;
    const float ca = chain[0].tick(conf, 0, xa), cb = chain[1].tick(conf, 1, xb);
    a[0] = a[1] = ca;
    b = cb;
    if (conf.mode == 2) {
      out[0] = (ca + cb) * outGain;
      out[1] = (ca - cb) * outGain;
    } else {
      out[0] = ca * outGain;
      out[1] = cb * outGain;
    }
    for (int i = 0; i < 8; ++i) band[i][0] = band[i][1] = chain[0].bandOut[i];
  }
  void tick(float L, float R, const EqParams& p) {
    prepare(p);
    process(L, R, p.outGain);
  }
};

// ---- Auto Filter (stdlib/devices/auto_filter.pqp) -----------------------------------------------------------------
// The device's float32 formulas, per sample, both channels interleaved. Structural settings come from a `Conf`
// type: AfRuntime (tests: every combination at run time) or AfFixed<…> (benchmarks: the hand-written, specialised
// filter a DSP programmer would write; every `if` on the configuration folds away). Param-rate values are prepared
// once per block (prepare), as the compiled Control region does; with the LFO or the follower on, the cutoff and
// the coefficients that depend on it run per sample. The nonlinear models' Newton iterations follow the engine's
// solver (pq/engine/newton.h): r = lhs − rhs and its exact derivative in float, Δ = r / J in double.
inline constexpr float kLog2e = 1.44269504088896341f;
inline float fastExp(float x) { return pq::fastExp2(x * kLog2e); }
inline float fastTan(float x) { return pq::fastTan(x); }
inline float fracBelowOne(float x) { return std::min(x - std::floor(x), 0.99999994f); }

struct AfParams {
  float freq = 1000.0f, res = 0.3f, drive = 1.0f, morph = 0.0f, dj = 0.0f;
  float lfoAmount = 0.0f, lfoRate = 1.0f;
  bool lfoSync = false;
  float lfoBeats = 1.0f, lfoOffset = 0.0f, lfoPhase = 0.0f, lfoSpin = 0.0f;
  float envAmount = 0.0f, attack = 0.006f, release = 0.2f, scGain = 1.0f, scMix = 1.0f;
  float dryWet = 1.0f, output = 1.0f;
};

// Run-time configuration (tests).
struct AfRuntime {
  int type = 0, circuit = 0, slope = 0, lfoShape = 0;
  bool lfo = false, env = false, scOn = false;
  int iterations = 2;        // Newton iterations of MS2 / SMP (the device: 2)
  int ladderIterations = 1;  // ... of PRD (the device: 1)
  bool prevGuess = false;  // start Newton from the previous sample's solution instead of the linear one
};
// Compile-time configuration (benchmarks).
template <int kType, int kCircuit, int kSlope, bool kLfo, bool kEnv, int kShape = 0>
struct AfFixed {
  static constexpr int type = kType, circuit = kCircuit, slope = kSlope, lfoShape = kShape, iterations = 2, ladderIterations = 1;
  static constexpr bool lfo = kLfo, env = kEnv, scOn = false, prevGuess = false;
};

// The svf_core_fast step with its derived coefficients (af_svf).
struct AfSvfCoef {
  float d1, b1, b2;
  static AfSvfCoef make(float g, float k) {
    const float gk = g + k;
    const float a1 = 1.0f / fma_(g, gk, 1.0f);
    const float b1 = 2.0f * g * a1;
    return {-2.0f * g * gk * a1, b1, b1 * g};
  }
};
struct AfSvf {
  float ic1 = 0.0f, ic2 = 0.0f;
  void tick(float x, const AfSvfCoef& c, float& band, float& low) {
    const float v = x - ic2;
    const float n1 = fma_(c.b1, v, fma_(c.d1, ic1, ic1));
    const float n2 = fma_(c.b2, v, fma_(c.b1, ic1, ic2));
    band = 0.5f * (ic1 + n1);
    low = 0.5f * (ic2 + n2);
    ic1 = n1;
    ic2 = n2;
  }
};

// One Newton solve of u = rhs(u) (the device's form `u == rhs`): `f(u, drhs)` returns rhs and its derivative.
template <class F>
inline float afNewton(float u, int iterations, F&& f) {
  for (int it = 0; it < iterations; ++it) {
    float drhs = 0.0f;
    const float rhs = f(u, drhs);
    const float r = u - rhs;
    const float J = 1.0f - drhs;
    const double d = static_cast<double>(r) / static_cast<double>(J);
    const float nu = static_cast<float>(static_cast<double>(u) - d);
    if (std::isfinite(d) && std::isfinite(nu)) u = nu;
  }
  return u;
}

// The nonlinear SVF sections (af_osr, af_ms2, af_smp): states s1 (band), s2 (low).
struct AfNlSvf {
  float s1 = 0.0f, s2 = 0.0f, last = 0.0f;
  void finish(float g, float bp, float& band, float& low) {
    const float lp = fma_(g, bp, s2);
    s1 = 2.0f * bp - s1;
    s2 = 2.0f * lp - s2;
    band = bp;
    low = lp;
  }
  void osr(float x, float g, float k, float d, float& band, float& low) {
    const float t = 1.0f / d;
    const float xs = fastTanh(d * x) / d;
    const float a = fma_(g, g + k, 1.0f);
    const float c = fma_(g, xs - s2, s1);
    const float bl = c / a;
    const float sg = bl > t ? 1.0f : bl < -t ? -1.0f : 0.0f;
    const float gd = 4.0f * g;
    const float bp = fma_(gd * t, sg, c) / fma_(gd, std::fabs(sg), a);
    finish(g, bp, band, low);
  }
  void ms2(float x, float g, float k, float d, int iterations, bool prevGuess, float& band, float& low) {
    const float kr = 2.0f - k;
    const float bl = fma_(g, x - s2, s1) / fma_(g, g + k, 1.0f);
    const float g2 = g + 2.0f;
    const float bp = last = afNewton(prevGuess ? last : bl, iterations, [&](float u, float& dr) {
      const float T = fastTanh(d * u);
      const float inner = ((x - g2 * u) - s2) + (kr * T) / d;
      const float dT = d * (1.0f - T * T);
      dr = (-g2 + (dT * kr) / d) * g;
      return fma_(g, inner, s1);
    });
    finish(g, bp, band, low);
  }
  void smp(float x, float g, float k, float d, int iterations, bool prevGuess, float& band, float& low) {
    const float kr = 2.0f - k;
    const float xs = fastTanh(d * x) / d;
    const float bl = fma_(g, xs - s2, s1) / fma_(g, g + k, 1.0f);
    const float g2 = g + 2.0f;
    const float bp = last = afNewton(prevGuess ? last : bl, iterations, [&](float u, float& dr) {
      const float T = fastTanh(d * u);
      const float inner = ((xs - g2 * u) - s2) + (kr * fma_(0.25f * T, T, T)) / d;
      const float dT = d * (1.0f - T * T);
      dr = (-g2 + ((dT + (dT * T * 0.25f + 0.25f * T * dT)) * kr) / d) * g;
      return fma_(g, inner, s1);
    });
    finish(g, bp, band, low);
  }
};

// The ladder (af_ladder): stage states s[0..3]; writes the taps u, y1 … y4.
struct AfLadder {
  float s[4] = {}, last = 0.0f;
  void tick(float x, float G, float k, float d, int iterations, bool prevGuess, float (&tap)[5]) {
    const float H = 1.0f - G;
    const float G2 = G * G, G4 = G2 * G2;
    const float S = fma_(H, s[3], fma_(fma_(H, s[2], fma_(H * s[0], G, H * s[1]) * G), G, 0.0f));
    const float lin = fma_(G4, x, S) / fma_(k, G4, 1.0f);
    const float y4 = last = afNewton(prevGuess ? last : lin, iterations, [&](float v, float& dr) {
      const float T = fastTanh(d * (x - k * v));
      dr = (((-k * d) * (1.0f - T * T)) / d) * G4;
      return fma_(G4, T / d, S);
    });
    const float u = fastTanh(d * (x - k * y4)) / d;
    const float y1 = fma_(G, u, H * s[0]);
    const float y2 = fma_(G, y1, H * s[1]);
    const float y3 = fma_(G, y2, H * s[2]);
    s[0] = 2.0f * y1 - s[0];
    s[1] = 2.0f * y2 - s[1];
    s[2] = 2.0f * y3 - s[2];
    s[3] = 2.0f * y4 - s[3];
    tap[0] = u;
    tap[1] = y1;
    tap[2] = y2;
    tap[3] = y3;
    tap[4] = y4;
  }
};

// With AfFixed every configuration test is a constant: that is the point (they fold away), so C4127 is off here.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4127)
#endif
template <class Conf>
struct AutoFilter {
  Conf conf{};
  float sr = 48000.0f;
  double beat0 = 0.0, beatsPerFrame = 2.0 / 48000.0;  // transport (120 BPM)
  uint64_t t = 0;

  // ---- state, per channel ----
  stdref::EnvFollow<float> follower;
  stdref::Phasor lfoPh[2];
  stdref::Smooth<float> lfoSmooth[2];
  AfSvf secA[2], secB[2], nlp1[2], nlp2[2], vow[2][3];
  AfNlSvf nl[2];
  AfLadder lad[2];
  stdref::Comb<float> comb[2] = {stdref::Comb<float>(0.05f, 48000.0f, 1), stdref::Comb<float>(0.05f, 48000.0f, 1)};
  stdref::Phasor rsPh[2];
  float rsPrev[2] = {}, rsHeld[2] = {};
  bool rsPrevTrig[2] = {};

  // ---- prepared (param rate) ----
  AfParams p{};
  float wLP = 1, wBP = 0, wHP = 0, wN = 0, k12 = 0, kB = 0, kbB = 0, kL = 0, d = 1, ca = 0, cr = 0, base = 0;
  float c[5] = {};  // ladder pole mix
  float djw = 0, fb = 0, combScale = 1, vk = 0, vf[3] = {};
  // ---- outputs ----
  float out[2] = {}, cutoff[2] = {}, lfo[2] = {}, envelope = 0;

  bool mod() const { return conf.lfo || conf.env; }
  // af_kclean: Clean's damping (one resonance law with the nonlinear models up to 90 %, M4 review M12).
  static float kClean(float res) {
    return res < 0.9f ? 1.4142135623730951f * (1.0f - res) : 0.1414213562373095f * fastExp(-5.545177444479562f * (res - 0.9f));
  }
  static float pick(int i, std::initializer_list<float> v) { return v.begin()[std::clamp<int>(i, 0, static_cast<int>(v.size()) - 1)]; }

  void prepare(const AfParams& q) {
    p = q;
    const int type = conf.type;
    const float pm = 4.0f * p.morph;
    const float hLP = std::max(0.0f, 1.0f - std::fabs(pm)) + std::max(0.0f, pm - 3.0f);
    const float hBP = std::max(0.0f, 1.0f - std::fabs(pm - 1.0f));
    const float hHP = std::max(0.0f, 1.0f - std::fabs(pm - 2.0f));
    const float hN = std::max(0.0f, 1.0f - std::fabs(pm - 3.0f));
    const float djHP = p.dj > 0.0f ? 1.0f : 0.0f;
    wLP = pick(type, {1, 0, 0, 0, hLP, 1 - djHP});
    wBP = pick(type, {0, 0, 1, 0, hBP, 0});
    wHP = pick(type, {0, 1, 0, 0, hHP, djHP});
    wN = pick(type, {0, 0, 0, 1, hN, 0});
    const int model = type > 4 ? 0 : conf.circuit;
    d = conf.circuit >= 1 ? p.drive : 1.0f;
    const bool clean = model == 0;
    k12 = clean ? kClean(p.res) : 1.4142135623730951f * (1.0f - p.res);
    kB = conf.slope >= 1 ? 0.541196100146197f * k12 : k12;
    kbB = clean ? kB : std::max(kB, 0.25f);
    kL = 4.0f * p.res;
    const float hn = wHP + wN;
    if (conf.slope >= 1) {
      c[0] = hn;
      c[1] = -4.0f * hn;
      c[2] = 4.0f * wBP + 6.0f * wHP + 8.0f * wN;
      c[3] = -8.0f * wBP - 4.0f * wHP - 8.0f * wN;
      c[4] = wLP + 4.0f * wBP + wHP + 4.0f * wN;
    } else {
      c[0] = hn;
      c[1] = 2.0f * wBP - 2.0f * hn;
      c[2] = wLP - 2.0f * wBP + wHP + 2.0f * wN;
      c[3] = 0.0f;
      c[4] = 0.0f;
    }
    c[4] = fma_(kL, wLP + wN, c[4]);  // PRD's passband compensation (M4 review M12)
    ca = stdref::EnvFollow<float>::coef(p.attack, sr);
    cr = stdref::EnvFollow<float>::coef(p.release, sr);
    const float djc = p.dj < 0.0f ? 20000.0f * fastExp(6.684611727667927f * p.dj) : 15.0f * fastExp(6.907755278982137f * p.dj);
    base = type == 5 ? djc : p.freq;
    djw = std::min(1.0f, 20.0f * std::fabs(p.dj));
    fb = 0.792f * p.res;
    combScale = std::sqrt(1.0f - fb * fb);
    const float q4 = 4.0f * p.morph;
    const int i = static_cast<int>(std::min(std::floor(q4), 3.0f));
    const float f = q4 - static_cast<float>(i);
    const float F1[5] = {730, 530, 270, 570, 300}, F2[5] = {1090, 1840, 2290, 840, 870}, F3[5] = {2440, 2480, 3010, 2410, 2240};
    vf[0] = fma_(f, F1[i + 1] - F1[i], F1[i]);
    vf[1] = fma_(f, F2[i + 1] - F2[i], F2[i]);
    vf[2] = fma_(f, F3[i + 1] - F3[i], F3[i]);
    vk = 1.0f / fma_(9.0f, p.res, 3.0f);
    modStatic = modFor(std::clamp(base, 10.0f, 0.49f * sr));
  }

  float lfoShape(float ph) const {
    switch (conf.lfoShape) {
      case 0: return sinePhase(ph);
      case 1: return 4.0f * std::fabs(fracBelowOne(ph + 0.75f) - 0.5f) - 1.0f;
      case 2: return 2.0f * fracBelowOne(ph + 0.5f) - 1.0f;
      case 3: return 1.0f - 2.0f * fracBelowOne(ph + 0.5f);
      default: return ph < 0.5f ? 1.0f : -1.0f;  // 4 square (5 random is not reproduced: the noise atom's seeds)
    }
  }

  // One SVF-family section of `model` (af_sec) with its output mix.
  float section(int ch, AfSvf& lin, int model, float x, float g, float k, float kb, const AfSvfCoef& cc) {
    float band = 0.0f, low = 0.0f;
    switch (model) {
      case 0: lin.tick(x, cc, band, low); break;
      case 1: nl[ch].osr(x, g, k, d, band, low); break;
      case 2: nl[ch].ms2(x, g, k, d, conf.iterations, conf.prevGuess, band, low); break;
      default: nl[ch].smp(x, g, k, d, conf.iterations, conf.prevGuess, band, low); break;
    }
    const float hn = wHP + wN;
    return fma_(wLP - wHP, low, fma_(fma_(kb, wBP, -(k * hn)), band, hn * x));
  }

  // The cutoff-dependent coefficients of one channel at one sample.
  struct ChMod {
    float fc = 0, g = 0, G = 0, g2 = 0;
    AfSvfCoef cA{}, cB{}, cv[3]{};
  };
  ChMod modStatic{};  // the unmodulated cutoff's (prepare)

  ChMod modFor(float fc) const {
    ChMod m;
    m.fc = fc;
    if (conf.type == 6 || conf.type == 7 || conf.type == 9) return vowelOnly(m);  // these read fc (vowel: its own g's)
    m.g = fastTan(3.141592653589793f * fc / sr);
    m.G = m.g / (1.0f + m.g);
    if (conf.type <= 5) {
      if (conf.slope >= 1) m.cA = AfSvfCoef::make(m.g, 1.8477590650225735f);
      m.cB = AfSvfCoef::make(m.g, kB);
    } else if (conf.type == 8) {
      m.g2 = fastTan(3.141592653589793f * std::min(2.0f * fc, 0.49f * sr) / sr);
    }
    return m;
  }
  ChMod vowelOnly(ChMod m) const {
    if (conf.type == 9) {
      const float sh = std::sqrt(m.fc * 0.001f);
      for (int i = 0; i < 3; ++i) m.cv[i] = AfSvfCoef::make(fastTan(3.141592653589793f * std::clamp(vf[i] * sh, 20.0f, 0.45f * sr) / sr), vk);
    }
    return m;
  }

  // One channel's filter at one sample.
  float filterOne(int ch, float xi, const ChMod& m) {
    const int type = conf.type;
    const int model = type > 4 ? 0 : conf.circuit;
    float w = 0.0f;
    if (type <= 5) {
      float filt;
      if (model == 4) {
        float tap[5];
        lad[ch].tick(xi, m.G, kL, d, conf.ladderIterations, conf.prevGuess, tap);
        filt = fma_(c[4], tap[4], fma_(c[3], tap[3], fma_(c[2], tap[2], fma_(c[1], tap[1], c[0] * tap[0]))));
      } else {
        float in = xi;
        if (conf.slope >= 1) in = section(ch, secA[ch], 0, xi, m.g, 1.8477590650225735f, 1.8477590650225735f, m.cA);
        filt = section(ch, secB[ch], model, in, m.g, kB, kbB, m.cB);
      }
      w = type == 5 ? fma_(djw, filt - xi, xi) : filt;
    } else if (type == 6) {
      const float T = 1.0f / std::clamp(m.fc, 20.0f, 10000.0f);
      w = comb[ch].tick(xi, T * sr, fb) * combScale;
    } else if (type == 7) {
      const float ph = rsPh[ch].tick(std::min(2.0f * m.fc, 0.45f * sr), false, sr);
      const bool trig = ph < rsPrev[ch];
      rsPrev[ch] = ph;
      if (trig && !rsPrevTrig[ch]) rsHeld[ch] = xi;
      rsPrevTrig[ch] = trig;
      w = rsHeld[ch];
    } else if (type == 8) {
      const float k = kClean(p.res);
      float band, low;
      nlp1[ch].tick(xi, AfSvfCoef::make(m.g, k), band, low);
      const float nt = xi - k * band;
      nlp2[ch].tick(nt, AfSvfCoef::make(m.g2, 1.4142135623730951f), band, low);
      w = low;
    } else {
      float b[3];
      for (int i = 0; i < 3; ++i) {
        float low;
        vow[ch][i].tick(xi, m.cv[i], b[i], low);
      }
      w = vk * fma_(0.25f, b[2], fma_(0.5f, b[1], b[0]));
    }
    return fma_(p.dryWet, w - xi, xi) * p.output;
  }

  // One stereo sample (tests: everything per sample).
  void process(const float (&x)[2], const float (&sc)[2]) {
    float oct[2] = {0.0f, 0.0f};
    if (conf.env) {
      float det[2] = {x[0], x[1]};
      if (conf.scOn)
        for (int ch = 0; ch < 2; ++ch) det[ch] = fma_(p.scMix, sc[ch] * p.scGain - x[ch], x[ch]);
      envelope = follower.tick(0.5f * (det[0] + det[1]), ca, cr, false);
    }
    if (conf.lfo) {
      const float beat = static_cast<float>(beat0 + static_cast<double>(t) * beatsPerFrame);
      const float fl = lfoPh[0].tick(p.lfoRate, false, sr);
      const float fr = lfoPh[1].tick(p.lfoRate * (1.0f + p.lfoSpin), false, sr);
      const float sl = fracBelowOne(beat / p.lfoBeats);
      const float sR = fracBelowOne(beat * (1.0f + p.lfoSpin) / p.lfoBeats);
      const float pl = fracBelowOne((p.lfoSync ? sl : fl) + p.lfoOffset);
      const float pr = fracBelowOne(((p.lfoSync ? sR : fr) + p.lfoOffset) + p.lfoPhase);
      lfo[0] = lfoShape(pl);
      lfo[1] = lfoShape(pr);
      if (conf.lfoShape >= 2) {  // the shapes with jumps: a 0.5 ms one-pole
        const float sm = stdref::Smooth<float>::coef(0.0005f, sr);
        lfo[0] = lfoSmooth[0].tick(lfo[0], sm);
        lfo[1] = lfoSmooth[1].tick(lfo[1], sm);
      }
      const float a4 = p.lfoAmount * 4.0f;
      oct[0] = a4 * lfo[0];
      oct[1] = a4 * lfo[1];
    }
    if (conf.env) {
      const float e8 = p.envAmount * 8.0f * envelope;
      oct[0] += e8;
      oct[1] += e8;
    }
    for (int ch = 0; ch < 2; ++ch) {
      const ChMod m = mod() ? modFor(std::clamp(base * fastExp(0.6931471805599453f * oct[ch]), 10.0f, 0.49f * sr)) : modStatic;
      cutoff[ch] = m.fc;
      out[ch] = filterOne(ch, x[ch], m);
    }
    ++t;
  }

  // A block (benchmarks: how a hand-written filter is structured): the modulation and the cutoff-dependent
  // coefficients computed vectorised over the block first (8 samples per AVX2 op; the follower's recursion, the
  // phasors' integer phases and the LFO smoother scalar), then one loop over the frames running both channels'
  // filters. Free-running LFO, no sidechain. Writes outLR[2·i + ch]. Not RT code (allocates its buffers once).
  void processBlock(const float* xL, const float* xR, uint32_t n, float* outLR) {
    if (!mod()) {
      for (uint32_t i = 0; i < n; ++i) {
        outLR[2 * i] = filterOne(0, xL[i], modStatic);
        outLR[2 * i + 1] = filterOne(1, xR[i], modStatic);
      }
      t += n;
      return;
    }
    const uint32_t n8 = (n + 7) & ~7u;
    if (blk_.size() < 22u * n8 + 8) blk_.assign(22u * n8 + 8, 0.0f);
    float* base0 = blk_.data() + ((8 - (reinterpret_cast<uintptr_t>(blk_.data()) / 4) % 8) % 8);  // 32-byte aligned
    auto buf = [&](uint32_t k) { return base0 + static_cast<size_t>(k) * n8; };
    float* oct[2] = {buf(0), buf(1)};
    float* tmp = buf(2);
    for (uint32_t i = 0; i < 2 * n8; ++i) base0[i] = 0.0f;
    if (conf.env) {
      const float e8 = p.envAmount * 8.0f;
      for (uint32_t i = 0; i < n; ++i) {
        envelope = follower.tick(0.5f * (xL[i] + xR[i]), ca, cr, false);
        oct[0][i] = oct[1][i] = e8 * envelope;
      }
    }
    if (conf.lfo) {
      const float rates[2] = {p.lfoRate, p.lfoRate * (1.0f + p.lfoSpin)};
      const float offs[2] = {p.lfoOffset, p.lfoOffset + p.lfoPhase};
      const __m256 a4 = _mm256_set1_ps(p.lfoAmount * 4.0f);
      for (int ch = 0; ch < 2; ++ch) {
        const float half = sr * 0.5f, h = std::min(std::max(rates[ch], -half), half);
        const uint32_t inc = static_cast<uint32_t>(static_cast<int64_t>(std::nearbyint(static_cast<double>(h) * (4294967296.0 / sr))));
        uint32_t ph = lfoPh[ch].phase;
        for (uint32_t i = 0; i < n; ++i) {
          tmp[i] = static_cast<float>(ph >> 8) * 5.9604644775390625e-08f;
          ph += inc;
        }
        lfoPh[ch].phase = ph;
        const __m256 off = _mm256_set1_ps(offs[ch]), below = _mm256_set1_ps(0.99999994f);
        for (uint32_t i = 0; i < n8; i += 8) {
          __m256 q = _mm256_add_ps(_mm256_load_ps(tmp + i), off);
          q = _mm256_min_ps(_mm256_sub_ps(q, _mm256_floor_ps(q)), below);
          __m256 v;
          if (conf.lfoShape == 0) {
            const __m256 r = _mm256_sub_ps(q, _mm256_round_ps(q, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
            v = pq::fastSin(_mm256_mul_ps(r, _mm256_set1_ps(stdref::kTwoPi)));
          } else if (conf.lfoShape == 1) {
            __m256 u = _mm256_add_ps(q, _mm256_set1_ps(0.75f));
            u = _mm256_min_ps(_mm256_sub_ps(u, _mm256_floor_ps(u)), below);
            const __m256 a = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), _mm256_sub_ps(u, _mm256_set1_ps(0.5f)));
            v = _mm256_sub_ps(_mm256_mul_ps(_mm256_set1_ps(4.0f), a), _mm256_set1_ps(1.0f));
          } else if (conf.lfoShape == 4) {
            v = _mm256_blendv_ps(_mm256_set1_ps(-1.0f), _mm256_set1_ps(1.0f), _mm256_cmp_ps(q, _mm256_set1_ps(0.5f), _CMP_LT_OQ));
          } else {
            __m256 u = _mm256_add_ps(q, _mm256_set1_ps(0.5f));
            u = _mm256_min_ps(_mm256_sub_ps(u, _mm256_floor_ps(u)), below);
            v = _mm256_sub_ps(_mm256_mul_ps(_mm256_set1_ps(2.0f), u), _mm256_set1_ps(1.0f));
            if (conf.lfoShape == 3) v = _mm256_sub_ps(_mm256_setzero_ps(), v);
          }
          _mm256_store_ps(tmp + i, v);
        }
        if (conf.lfoShape >= 2) {
          const float sm = stdref::Smooth<float>::coef(0.0005f, sr);
          for (uint32_t i = 0; i < n; ++i) tmp[i] = lfoSmooth[ch].tick(tmp[i], sm);
        }
        for (uint32_t i = 0; i < n8; i += 8)
          _mm256_store_ps(oct[ch] + i, _mm256_fmadd_ps(a4, _mm256_load_ps(tmp + i), _mm256_load_ps(oct[ch] + i)));
      }
    }
    // Cutoff → coefficients, vectorised: fc, g, G and the main section's (d1, b1, b2).
    float* fcv[2] = {buf(3), buf(4)};
    float* gv[2] = {buf(5), buf(6)};
    float* Gv[2] = {buf(7), buf(8)};
    float* cB[2][3] = {{buf(9), buf(10), buf(11)}, {buf(12), buf(13), buf(14)}};
    float* cA[2][3] = {{buf(15), buf(16), buf(17)}, {buf(18), buf(19), buf(20)}};
    const __m256 ln2l2e = _mm256_set1_ps(0.6931471805599453f * kLog2e), vb = _mm256_set1_ps(base), lo = _mm256_set1_ps(10.0f),
                 hi = _mm256_set1_ps(0.49f * sr), wv = _mm256_set1_ps(3.141592653589793f / sr), one = _mm256_set1_ps(1.0f),
                 kb = _mm256_set1_ps(kB), ka = _mm256_set1_ps(1.8477590650225735f), m2 = _mm256_set1_ps(-2.0f), two = _mm256_set1_ps(2.0f);
    for (int ch = 0; ch < 2; ++ch)
      for (uint32_t i = 0; i < n8; i += 8) {
        const __m256 e = pq::fastExp2(_mm256_mul_ps(_mm256_load_ps(oct[ch] + i), ln2l2e));
        const __m256 f = _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(vb, e), lo), hi);
        const __m256 g = pq::fastTan(_mm256_mul_ps(f, wv));
        _mm256_store_ps(fcv[ch] + i, f);
        _mm256_store_ps(gv[ch] + i, g);
        _mm256_store_ps(Gv[ch] + i, _mm256_div_ps(g, _mm256_add_ps(one, g)));
        const __m256 gk = _mm256_add_ps(g, kb);
        const __m256 a1 = _mm256_div_ps(one, _mm256_fmadd_ps(g, gk, one));
        const __m256 b1 = _mm256_mul_ps(_mm256_mul_ps(two, g), a1);
        _mm256_store_ps(cB[ch][0] + i, _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(m2, g), gk), a1));
        _mm256_store_ps(cB[ch][1] + i, b1);
        _mm256_store_ps(cB[ch][2] + i, _mm256_mul_ps(b1, g));
        if (conf.slope >= 1) {  // the 24 dB pre-section
          const __m256 gka = _mm256_add_ps(g, ka);
          const __m256 aa = _mm256_div_ps(one, _mm256_fmadd_ps(g, gka, one));
          const __m256 ba = _mm256_mul_ps(_mm256_mul_ps(two, g), aa);
          _mm256_store_ps(cA[ch][0] + i, _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(m2, g), gka), aa));
          _mm256_store_ps(cA[ch][1] + i, ba);
          _mm256_store_ps(cA[ch][2] + i, _mm256_mul_ps(ba, g));
        }
      }
    // The Clean-only types' coefficient sets (comb, resampling, notch + lowpass, vowel) per sample through modFor.
    const bool perSample = conf.type > 5;
    for (uint32_t i = 0; i < n; ++i)
      for (int ch = 0; ch < 2; ++ch) {
        ChMod m;
        if (perSample) {
          m = modFor(fcv[ch][i]);
        } else {
          m.fc = fcv[ch][i];
          m.g = gv[ch][i];
          m.G = Gv[ch][i];
          m.cB = {cB[ch][0][i], cB[ch][1][i], cB[ch][2][i]};
          if (conf.slope >= 1) m.cA = {cA[ch][0][i], cA[ch][1][i], cA[ch][2][i]};
        }
        outLR[2 * i + ch] = filterOne(ch, ch ? xR[i] : xL[i], m);
      }
    t += n;
  }

private:
  std::vector<float> blk_;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace devref
