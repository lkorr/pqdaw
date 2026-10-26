#pragma once

// Hand-written C++ references of the stock devices in stdlib/devices/ (T4.9 Utility + Saturator, T4.10 EQ Eight;
// tests and benchmarks only, never shipped): the same float32 formulas as the .pqp files, written the way a DSP
// programmer would (state in locals, one loop over the samples, every band / allpass of a sample interleaved), so
// the tests can hold the compiled devices to them and bench/jit/bench_devices.cpp can compare their cost.
// Fused multiply-adds sit where the optimiser fuses (a·b + c at equal rates, stdlib_reference.h's convention).

#include <algorithm>
#include <cmath>
#include <cstdint>

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

// ---- Saturator (stdlib/devices/saturator.pqp) ----------------------------------------------------------------------
struct SatParams {
  float drive = 1.0f, output = 1.0f, dryWet = 1.0f;  // linear
  float base = 1.0f, colorFreq = 1000.0f, colorWidth = 0.7f, depth = 1.0f;
  float wsDrive = 0.5f, wsLin = 0.5f, wsCurve = 0.5f, wsDamp = 0.0f, wsDepth = 0.0f, wsPeriod = 0.0f;
};
struct SatConf {
  int type = 3, hq = 1;
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
  Oversampler os[2];
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
  // saturator would specialise it).
  template <int kType>
  void processT(const float (&x)[2]) {
    for (int ch = 0; ch < 2; ++ch) {
      float pre = x[ch];
      if (conf.color) {
        pre = shelf[ch].tick(pre, shelfC).lowshelf;
        pre = bell[ch].tick(pre, bellC).bell;
      }
      const float u = pre * p.drive;
      driven[ch] = u;
      const float w = os[ch].tick(u, conf.hq, [&](float v) { return shape<kType>(v); });
      wet[ch] = w;
      out[ch] = fma_(p.dryWet, w - x[ch], x[ch]);
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
  constexpr float f1 = 1.3870398453221475f;
  switch (type) {
    case kLC48: c = {w, f1 * kc, 1, -f1 * kc, -1}; break;
    case kLC12: c = {w, kc, 1, -kc, -1}; break;
    case kLS: c = {w / sA, kb, 1, kb * (A - 1), ge - 1}; break;
    case kBell: c = {w, kb / A, 1, kb / A * (ge - 1), 0}; break;
    case kNotch: c = {w, kc, 1, -kc, 0}; break;
    case kHS: c = {w * sA, kb, ge, kb * (1 - A) * A, 1 - ge}; break;
    case kHC12: c = {w, kc, 0, 0, 1}; break;
    default: c = {w, f1 * kc, 0, 0, 1}; break;
  }
  c.k2 = 1.1758756024193588f * kc;
  c.k3 = 0.7856949583871023f * kc;
  c.k4 = 0.2758993792829431f * kc;
  return c;
}

// One section (eq8_sec): y = m0·x + m1·band + m2·low, fused as the optimiser fuses it: with m0 the literal 1
// (every type but the high shelf) m1·band + x is one multiply-add, else m0·x + m1·band; then m2·low + that.
struct EqSec {
  FastSvfCore core;
  float tick(float x, const FastSvfCore::Coef& c, bool m0One, float m0, float m1, float m2) {
    float band, low;
    core.tick(x, c, band, low);
    const float v = m0One ? fma_(m1, band, x) : fma_(m0, x, m1 * band);
    return fma_(m2, low, v);
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

}  // namespace devref
