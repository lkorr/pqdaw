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
  stdref::SvfCore<float> ap, h1, h2, l1, l2;
  stdref::DcBlock<float> dcL, dcR;
  UtilityConf conf;
  float sr = 48000.0f;
  float out[2] = {}, mid = 0.0f, side = 0.0f, bass = 0.0f;
  bool wantBass = true;  // compute the `bass` export (tests); a benchmark reading only `out` turns it off
  // Param-rate values (prepare(): once per block, as the compiled Control region).
  float direct[4] = {}, split[6] = {}, k = 0.0f, rc = 0.0f;
  stdref::SvfCore<float>::Coef bc{};

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
  void bassCoef(float bassFreq) { bc = stdref::SvfCore<float>::coef(stdref::prewarp(bassFreq, sr), 1.4142135623730951f); }

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

}  // namespace devref
