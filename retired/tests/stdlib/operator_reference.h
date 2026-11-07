#pragma once

// Hand-written C++ reference of Operator (stdlib/devices/operator.pqp) and its multi-stage envelope
// (stdlib/envelopes/msenv.pqp), T4.12; tests and benchmarks only, never shipped. The same float32 formulas as the
// patches, written the way a DSP programmer would (one voice at a time, state in members), with fused multiply-adds
// where the optimiser fuses a·b + c (stdlib_reference.h's convention). The algorithm routing is written out per
// algorithm from the table in operator.pqp's header (a switch, not the patch's routing tables), so a routing test
// against it checks the patch's tables independently.
//
// What the reference leaves out: Noise White (per-lane random; tested statistically) and the LFO's S&H / Noise
// shapes. Everything else — every algorithm, waveform (the band-limited tables read exactly as the wavetable kernel
// reads them), envelope mode, feedback, pitch envelope, LFO, filter type, slope and circuit model (T4.12-fix1: Auto
// Filter's reference sections devref::AfNlSvf / AfLadder, the engine's Newton steps), glide, spread, velocity and key
// sensitivity, volume and pan — is here.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include "device_reference.h"
#include "pq/core/fast_math.h"
#include "pq/dsp/tables.h"
#include "stdlib_reference.h"

namespace opref {

using stdref::fma_;
using devref::sinePhase;

constexpr float kSr = 48000.0f;

inline float frac(float x) { return std::min(x - std::floor(x), 0.99999994f); }  // the frac atom (kBelowOne)
inline float clipf(float x, float lo, float hi) { return std::min(std::max(x, lo), hi); }
// fast_cents_to_hz(c, ref) = ref · fastExp2(c · (1/1200))
inline float fastCentsToHz(float c, float ref) { return ref * pq::fastExp2(c * (1.0f / 1200.0f)); }
// cents_to_hz (exact): ref · 2^(c/1200) in double, rounded once.
inline float centsToHz(float c, float ref) {
  return static_cast<float>(static_cast<double>(ref) * std::exp2(static_cast<double>(c) / 1200.0));
}
// lin(): a dB knob's gain with −70 dB read as −inf.
inline float lin(float x) { return x < 0.00031623f ? 0.0f : x; }
inline float db(double d) { return static_cast<float>(std::pow(10.0, d / 20.0)); }

// ---- the wavetable atom, one lane (pq/dsp kernels_gen.cpp WavetableK, voice path: both mip levels read) ----------
struct WavetableLane {
  float prev = 0.0f, prevD = 0.0f;
  static float read(const pq::dsp::WavetableView& v, float p, float inc) {
    const float x = inc * v.sizeF;
    const float u = fma_(static_cast<float>(std::bit_cast<int32_t>(x)), 2.384185791015625e-07f, -254.0f);
    const float uf = std::floor(u);
    const float fu = u - uf;
    const float t = std::min(fu + fu, 1.0f);
    const int32_t k = static_cast<int32_t>(uf);
    const int32_t top = static_cast<int32_t>(v.levels) - 1;
    const int32_t lo = std::min(std::max(k, 0), top);
    const int32_t hi = std::min(std::max(k + 1, 0), top);
    const float pos = (p - std::floor(p)) * v.sizeF;
    const int32_t i = static_cast<int32_t>(pos);
    const float fr = pos - static_cast<float>(i);
    const int32_t off = i & (static_cast<int32_t>(v.size) - 1);
    const float* d = v.data;
    const size_t b0 = static_cast<size_t>(lo) * v.stride + off, b1 = static_cast<size_t>(hi) * v.stride + off;
    const float bv = fma_(fr, d[b1 + 1] - d[b1], d[b1]);
    const float av = fma_(fr, d[b0 + 1] - d[b0], d[b0]);
    return t >= 1.0f ? bv : fma_(t, bv - av, av);
  }
  float tick(const pq::dsp::WavetableView& v, float p) {
    float dd = p - prev;
    dd = std::fabs(dd - std::nearbyint(dd));
    dd = std::max(dd, 0.0f);
    const float y = read(v, p, std::min(dd, prevD));
    prev = p;
    prevD = dd;
    return y;
  }
};

// ---- msenv -------------------------------------------------------------------------------------------------------
struct EnvParams {
  float a = 0.0f, d = 0.6f, r = 0.05f, init = 0.0f, peak = 1.0f, sus = 1.0f, end = 0.0f, rep = 1.0f;
};

struct MsEnv {
  int mode = 0;
  bool soft = true;
  // state
  float lvl = 0.0f;
  bool att = false, prevGate = false, settled = false;
  bool riseSettledPrev = false;  // rise(settled)
  int32_t timerRemaining = 0;
  bool timerPrevTrig = false, fallPrev = false;
  stdref::Phasor beatPhasor;
  float beatPhPrev = 0.0f, songPhPrev = 0.0f;
  bool held4 = false;  // the Trigger mode's flip-flop

  struct Coef {
    float rate, dcoef, rcoef, init, peak, sus, end, durSamples, beatHz, rep;
  };
  static Coef coef(const EnvParams& p, float timeScale, float bpm) {
    Coef c;
    const float a = p.a * timeScale, d = p.d * timeScale, r = p.r * timeScale;
    c.rate = std::max(std::fabs(p.peak - p.init), 0.001f) / std::max(a * kSr, 1.0f);
    c.dcoef = stdref::vexp(-9.210340371976184f / std::max(d * kSr, 1.0f));
    c.rcoef = stdref::vexp(-9.210340371976184f / std::max(r * kSr, 1.0f));
    c.init = p.init;
    c.peak = p.peak;
    c.sus = p.sus;
    c.end = p.end;
    c.durSamples = p.rep * kSr;
    c.beatHz = bpm / (60.0f * p.rep);
    c.rep = p.rep;
    return c;
  }

  // `beat` = transport.beat at this sample. Returns the level; `done` as the patch's output.
  float tick(bool gate, const Coef& c, float beat, bool& done) {
    const bool start0 = gate && !prevGate;
    prevGate = gate;
    // loopTrig = fall(timer(rise(settled), rep)) with settled = the z1 output (previous sample)
    const bool riseS = settled && !riseSettledPrev;
    riseSettledPrev = settled;
    if (riseS && !timerPrevTrig) {
      const float dd = std::min(std::max(c.durSamples, 0.0f), 2147483520.0f);
      timerRemaining = static_cast<int32_t>(std::nearbyint(dd));
    }
    timerPrevTrig = riseS;
    const bool timerOn = timerRemaining > 0;
    if (timerOn) --timerRemaining;
    const bool loopTrig = fallPrev && !timerOn;
    fallPrev = timerOn;
    // beatTrig: phasor(bpm / (60 · rep), reset: start0) < its previous value
    const float bph = beatPhasor.tick(c.beatHz, start0, kSr);
    const bool beatTrig = bph < beatPhPrev;
    beatPhPrev = bph;
    const float sph = frac(beat / c.rep);
    const bool syncTrig = sph < songPhPrev;
    songPhPrev = sph;

    bool held = gate, start = start0;
    if (mode == 4) {
      held4 = !settled && (start0 || held4);
      held = held4;
    } else if (mode == 3) {
      start = start0 || (held && syncTrig);
    } else if (mode == 2) {
      start = start0 || (held && beatTrig);
    } else if (mode == 1) {
      start = start0 || (held && loopTrig);
    }
    const float prev = lvl;
    const float b = soft ? (start0 ? std::max(c.init, prev) : prev) : (start0 ? c.init : prev);
    const bool inAtt = held && (start || att);
    const float dist = c.peak - b;
    const bool reach = std::fabs(dist) <= c.rate;
    const float aNext = reach ? c.peak : b + clipf(dist, -c.rate, c.rate);
    const float next = held ? (inAtt ? aNext : fma_(b - c.sus, c.dcoef, c.sus)) : fma_(b - c.end, c.rcoef, c.end);
    att = inAtt && !reach;
    settled = held && !att && std::fabs(next - c.sus) <= 0.001f;
    lvl = next;
    done = !held && next < 0.0001f;
    return next;
  }
};

// ---- Operator ---------------------------------------------------------------------------------------------------
struct Config {
  int algorithm = 1;
  std::array<bool, 4> on{true, false, false, false};
  std::array<int, 4> wave{0, 0, 0, 0};
  std::array<int, 4> loop{0, 0, 0, 0};
  bool pitchEnvOn = false;
  int pitchLoop = 0;
  bool lfoOn = false;
  int lfoWave = 0;  // 0..4 (S&H and Noise are not in the reference)
  bool lfoRetrig = true;
  bool filterOn = false;
  int filterType = 0, filterSlope = 0, filterLoop = 0;
  int filterCircuit = 0;  // 0 Clean, 1 OSR, 2 MS2, 3 SMP, 4 PRD (T4.12-fix1)
  bool glideOn = false;
  bool spreadOn = false;
};

struct OpParams {
  float coarse = 1.0f, den = 1.0f, fine = 0.0f;
  bool fixed = false;
  float freq = 1000.0f;
  float level = 1.0f, vel = 0.0f, key = 0.0f;
  EnvParams env{0.0f, 0.6f, 0.05f, db(-70), 1.0f, 1.0f, db(-70), 1.0f};
  bool penvTo = true, lfoTo = true;
  std::array<float, 16> u{1.0f};
};

struct Params {
  std::array<OpParams, 4> op{};
  float feedback = 0.0f;
  float penvAmount = 0.0f;
  EnvParams penv{0.0f, 0.3f, 0.05f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
  float lfoRate = 1.0f;
  bool lfoSync = false;
  float lfoBeats = 1.0f, lfoAmount = 1.0f, lfoPitch = 100.0f, lfoFilter = 0.0f;
  float filterFreq = 2000.0f, filterRes = 0.2f, morph = 0.0f, filterKey = 0.0f, filterVel = 0.0f, fenvAmount = 0.0f;
  float filterDrive = 1.0f;  // linear (the dB knob's value)
  EnvParams fenv{0.0f, 0.6f, 0.05f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
  float volume = db(-12), pan = 0.0f, transpose = 0.0f, time = 1.0f, spread = 0.0f, glideTime = 0.1f;
  Params() {
    op[1].level = op[2].level = op[3].level = db(-70);
  }
};

// The voice's engine signals at one sample.
struct VoiceIn {
  float hz = 0.0f, cents = 0.0f, velocity = 1.0f;
  bool gate = false, trigger = false;
};

struct Out {
  float l = 0.0f, r = 0.0f;
  bool done = false;
  std::array<float, 4> op{};
  float lfo = 0.0f, penv = 0.0f, fenv = 0.0f;
};

// One channel's four operators (op_bank).
struct Bank {
  std::array<stdref::Phasor, 4> ph{};
  std::array<WavetableLane, 4> wt{};
  float wD1 = 0.0f, wD2 = 0.0f;

  static float wave(int w, float p, float nmax, const std::array<float, 16>& u, WavetableLane& wt) {
    using pq::dsp::builtinWavetable;
    auto table = [&](uint32_t k) { return wt.tick(*builtinWavetable(k), p); };
    switch (w) {
      case 0: return sinePhase(p);
      case 1: return std::floor(fma_(sinePhase(p), 7.0f, 0.5f)) * 0.14285714285714285f;
      case 2: return std::floor(fma_(sinePhase(p), 127.0f, 0.5f)) * 0.007874015748031496f;
      case 3: case 4: case 5: case 6: case 7: case 8: case 9: return table(static_cast<uint32_t>(w - 3));
      case 10: return 1.0f - 2.0f * frac(p);
      case 11: case 12: case 13: case 14: case 15: case 16: case 17: return table(static_cast<uint32_t>(w - 11 + 7));
      case 18: return frac(p) < 0.5f ? 1.0f : -1.0f;
      case 19: return table(14);
      case 20: return table(15);
      case 22: {
        const float s = sinePhase(p);
        const float c2 = 2.0f * sinePhase(p + 0.25f);
        std::array<float, 17> sh{};
        sh[1] = s;
        sh[2] = c2 * s;
        for (int h = 3; h <= 16; ++h) sh[h] = c2 * sh[h - 1] - sh[h - 2];
        float y = 0.0f;
        for (int h = 1; h <= 16; ++h) y += nmax > static_cast<float>(h) ? u[h - 1] * sh[h] : 0.0f;
        return y;
      }
      default: return 0.0f;
    }
  }

  // hz, amp per operator; returns the carriers' sum; o = each operator's output.
  float tick(const Config& cfg, const Params& prm, const std::array<float, 4>& hz, const std::array<float, 4>& amp,
             std::array<float, 4>& o) {
    auto wv = [&](int i, float p) {
      const float nmax = 21600.0f / std::max(std::fabs(hz[i]), 1.0f);
      return wave(cfg.wave[i], p, nmax, prm.op[i].u, wt[i]);
    };
    const bool onA = cfg.on[0], onB = cfg.on[1], onC = cfg.on[2], onD = cfg.on[3];
    // D: feedback
    const float pD = fma_(prm.feedback * 0.25f, wD1 + wD2, ph[3].tick(hz[3], false, kSr));
    const float wD = wv(3, pD);
    wD2 = wD1;
    wD1 = wD;
    o[3] = wD * amp[3];
    const int alg = cfg.algorithm;
    // Modulators per algorithm (operator.pqp's table), accumulated in the order B, C, D.
    auto add = [](float acc, bool on, float m) { return on ? fma_(0.5f, m, acc) : acc; };
    // C
    float pC = ph[2].tick(hz[2], false, kSr);
    const bool dToC = alg == 1 || alg == 3 || alg == 6 || alg == 7 || alg == 9 || alg == 10;
    pC = add(pC, dToC && onD, o[3]);
    o[2] = wv(2, pC) * amp[2];
    // B
    float pB = ph[1].tick(hz[1], false, kSr);
    const bool cToB = alg == 1 || alg == 2 || alg == 4 || alg == 6 || alg == 8;
    const bool dToB = alg == 2 || alg == 8 || alg == 10;
    pB = add(pB, cToB && onC, o[2]);
    pB = add(pB, dToB && onD, o[3]);
    o[1] = wv(1, pB) * amp[1];
    // A
    float pA = ph[0].tick(hz[0], false, kSr);
    const bool bToA = alg == 1 || alg == 2 || alg == 3 || alg == 4 || alg == 5 || alg == 7;
    const bool cToA = alg == 3 || alg == 5;
    const bool dToA = alg == 4 || alg == 5 || alg == 10;
    pA = add(pA, bToA && onB, o[1]);
    pA = add(pA, cToA && onC, o[2]);
    pA = add(pA, dToA && onD, o[3]);
    o[0] = wv(0, pA) * amp[0];
    // Carriers
    const bool carB = alg == 6 || alg == 8 || alg == 9 || alg == 10 || alg == 11;
    const bool carC = alg == 7 || alg == 9 || alg == 10 || alg == 11;
    const bool carD = alg == 11;
    float y = onA ? o[0] : 0.0f;
    if (carB && onB) y += o[1];
    if (carC && onC) y += o[2];
    if (carD && onD) y += o[3];
    for (int i = 0; i < 4; ++i)
      if (!cfg.on[static_cast<size_t>(i)]) o[static_cast<size_t>(i)] = 0.0f;
    return y;
  }
};

// op_filter: Clean = two svf_core_fast stages (the first only at 24 dB, Butterworth), the type's response; OSR / MS2 /
// SMP = Auto Filter's nonlinear SVF sections (devref::AfNlSvf: the same float formulas and Newton steps as
// stdlib/filters/circuit.pqp) in the second stage, damping k; PRD = devref::AfLadder with feedback 2·(2 − k).
struct Filter {
  devref::FastSvfCore s1, s2;
  devref::AfNlSvf nl;
  devref::AfLadder lad;
  struct Weights {
    float lp, bp, hp, n;
  };
  static Weights weights(int type, float morph) {
    const float mt = morph * 4.0f;
    const float wLP = std::max(0.0f, 1.0f - mt) + std::max(0.0f, mt - 3.0f);
    const float wBP = std::max(0.0f, 1.0f - std::fabs(mt - 1.0f));
    const float wHP = std::max(0.0f, 1.0f - std::fabs(mt - 2.0f));
    const float wN = std::max(0.0f, 1.0f - std::fabs(mt - 3.0f));
    switch (type) {
      case 0: return {1, 0, 0, 0};
      case 1: return {0, 0, 1, 0};
      case 2: return {0, 1, 0, 0};
      case 3: return {0, 0, 0, 1};
      default: return {wLP, wBP, wHP, wN};
    }
  }
  static float response(int type, float x, float band, float low, float k, float morph) {
    const Weights w = weights(4, morph);
    const float kb = k * band;
    const float hp = x - kb - low, notch = x - kb;
    switch (type) {
      case 0: return low;
      case 1: return hp;
      case 2: return kb;
      case 3: return notch;
      default: return fma_(w.n, notch, fma_(w.hp, hp, fma_(w.bp, kb, w.lp * low)));
    }
  }
  float tick(int type, int slope, int circuit, float x, float g, float k, float morph, float d) {
    if (circuit == 4) {
      const Weights w = weights(type, morph);
      const float hn = w.hp + w.n;
      float c[5];
      if (slope == 1) {
        c[0] = hn;
        c[1] = -4.0f * hn;
        c[2] = 4.0f * w.bp + 6.0f * w.hp + 8.0f * w.n;
        c[3] = -8.0f * w.bp - 4.0f * w.hp - 8.0f * w.n;
        c[4] = w.lp + 4.0f * w.bp + w.hp + 4.0f * w.n;
      } else {
        c[0] = hn;
        c[1] = 2.0f * w.bp - 2.0f * hn;
        c[2] = w.lp - 2.0f * w.bp + w.hp + 2.0f * w.n;
        c[3] = 0.0f;
        c[4] = 0.0f;
      }
      const float fb = 2.0f * (2.0f - k);
      c[4] = fma_(fb, w.lp + w.n, c[4]);  // the passband compensation (M4 review M12)
      float tap[5];
      lad.tick(x, g / (1.0f + g), fb, d, 1, false, tap);
      return fma_(c[4], tap[4], fma_(c[3], tap[3], fma_(c[2], tap[2], fma_(c[1], tap[1], c[0] * tap[0]))));
    }
    float xi = x;
    if (slope == 1) {
      float band, low;
      constexpr float kB = 1.4142135623730951f;
      s1.tick(x, devref::FastSvfCore::coef(g, kB), band, low);
      xi = response(type, x, band, low, kB, morph);
    }
    float band = 0.0f, low = 0.0f;
    switch (circuit) {
      case 1: nl.osr(xi, g, k, d, band, low); break;
      case 2: nl.ms2(xi, g, k, d, 2, false, band, low); break;
      case 3: nl.smp(xi, g, k, d, 2, false, band, low); break;
      default: s2.tick(xi, devref::FastSvfCore::coef(g, k), band, low); break;
    }
    return response(type, xi, band, low, k, morph);
  }
};

struct Voice {
  std::array<MsEnv, 4> env{};
  MsEnv penv, fenv;
  Bank bankL, bankS, bankR;
  Filter filtL, filtR;
  stdref::Phasor lfoPh;
  float gStart = 0.0f, gRamp = 0.0f;
  bool prevTrigger = false;  // samplehold's edge
};

// Every voice of one run, sample by sample (glide reads the notes of all voices).
struct Operator {
  Config cfg;
  Params prm;
  std::vector<Voice> voices;
  // glide's global state
  float newest = 0.0f, prevC = 0.0f;
  bool srffPlayed = false, played = false;

  Operator(const Config& c, const Params& p, size_t numVoices) : cfg(c), prm(p), voices(numVoices) {
    for (Voice& v : voices) {
      for (int i = 0; i < 4; ++i) {
        v.env[static_cast<size_t>(i)].mode = cfg.loop[static_cast<size_t>(i)];
        v.env[static_cast<size_t>(i)].soft = true;
      }
      v.penv.mode = cfg.pitchLoop;
      v.penv.soft = false;
      v.fenv.mode = cfg.filterLoop;
      v.fenv.soft = false;
    }
  }

  static EnvParams ampEnv(const EnvParams& e) {
    EnvParams r = e;
    r.init = lin(e.init);
    r.peak = lin(e.peak);
    r.sus = lin(e.sus);
    r.end = lin(e.end);
    return r;
  }

  // One sample for every voice: `in[v]` the voice's signals, `beat` / `bpm` the transport at this sample.
  void tick(const std::vector<VoiceIn>& in, float beat, float bpm, std::vector<Out>& out) {
    const size_t n = voices.size();
    out.resize(n);
    // ---- glide's global part (voice_sum over the voices' note starts) ----
    float sumN = 0.0f, sumC = 0.0f;
    for (size_t v = 0; v < n; ++v) {
      const float kc = clipf(in[v].cents, -9600.0f, 9600.0f);
      sumN += in[v].trigger ? 1.0f : 0.0f;
      sumC += in[v].trigger ? kc : 0.0f;
    }
    const float newestPrev = newest;
    const float cur = sumN > 0.5f ? sumC / sumN : newestPrev;
    const float prevNote = sumN > 0.5f ? newestPrev : prevC;
    const bool playedNow = played;
    srffPlayed = srffPlayed || sumN > 0.5f;
    played = srffPlayed;
    newest = cur;
    prevC = prevNote;

    const float tF = centsToHz(prm.transpose, 1.0f);
    const float gStep = 1.0f / std::max(prm.glideTime * kSr, 1.0f);
    std::array<MsEnv::Coef, 4> ec;
    for (size_t i = 0; i < 4; ++i) ec[i] = MsEnv::coef(ampEnv(prm.op[i].env), prm.time, bpm);
    const MsEnv::Coef pc = MsEnv::coef(prm.penv, prm.time, bpm);
    const MsEnv::Coef fcf = MsEnv::coef(prm.fenv, prm.time, bpm);
    const float spC = prm.spread * 10.0f;

    for (size_t v = 0; v < n; ++v) {
      Voice& vc = voices[v];
      const VoiceIn& vi = in[v];
      Out& o = out[v];
      const float kc = clipf(vi.cents, -9600.0f, 9600.0f);
      const float vel = vi.velocity;
      const float base0 = vi.hz * tF;
      // glide
      const bool edge = vi.trigger && !vc.prevTrigger;
      vc.prevTrigger = vi.trigger;
      if (edge) vc.gStart = playedNow ? prevNote - kc : 0.0f;
      vc.gRamp = vi.trigger ? 1.0f : std::max(vc.gRamp - gStep, 0.0f);
      const float glideC = vc.gStart * vc.gRamp;
      const float base = cfg.glideOn ? fastCentsToHz(glideC, base0) : base0;
      // envelopes
      std::array<float, 4> e{};
      std::array<bool, 4> ed{};
      for (size_t i = 0; i < 4; ++i) {
        bool d = false;
        e[i] = vc.env[i].tick(vi.gate, ec[i], beat, d);
        ed[i] = d;
      }
      bool dummy = false;
      const float penv = vc.penv.tick(vi.gate, pc, beat, dummy);
      const float fenv = vc.fenv.tick(vi.gate, fcf, beat, dummy);
      // LFO
      const float lfoHz = prm.lfoSync ? bpm / (60.0f * prm.lfoBeats) : prm.lfoRate;
      const float lp = vc.lfoPh.tick(lfoHz, cfg.lfoRetrig && vi.trigger, kSr);
      float raw = 0.0f;
      switch (cfg.lfoWave) {
        case 0: raw = sinePhase(lp); break;
        case 1: raw = lp < 0.5f ? 1.0f : -1.0f; break;
        case 2: raw = 4.0f * std::fabs(frac(lp + 0.75f) - 0.5f) - 1.0f; break;
        case 3: raw = 2.0f * frac(lp + 0.5f) - 1.0f; break;
        case 4: raw = 1.0f - 2.0f * frac(lp + 0.5f); break;
        default: break;
      }
      const float lfoSig = raw * prm.lfoAmount;
      // pitch per operator
      const bool modOn = cfg.pitchEnvOn || cfg.lfoOn;
      std::array<float, 4> hz{}, c{}, amp{};
      for (size_t i = 0; i < 4; ++i) {
        const OpParams& op = prm.op[i];
        const float ratio = fma_(op.fine, 0.001f, op.coarse < 0.5f ? 0.5f : op.coarse) / op.den;
        hz[i] = op.fixed ? op.freq : base * ratio;
        const float pe = op.penvTo ? penv * prm.penvAmount : 0.0f;
        const float lf = op.lfoTo ? lfoSig * prm.lfoPitch : 0.0f;
        c[i] = cfg.pitchEnvOn && cfg.lfoOn ? pe + lf : cfg.pitchEnvOn ? pe : lf;
        const float lv = lin(op.level) * std::max(0.0f, fma_(op.vel, vel - 1.0f, 1.0f));
        amp[i] = e[i] * lv * fastCentsToHz(std::min(op.key * kc, 0.0f), 1.0f);  // attenuation only (M4 review M11)
      }
      std::array<float, 4> hm{};
      for (size_t i = 0; i < 4; ++i) hm[i] = modOn ? fastCentsToHz(c[i], hz[i]) : hz[i];
      std::array<float, 4> ol{}, os{}, orr{};
      const float yBank = vc.bankL.tick(cfg, prm, hm, amp, ol);
      float monoL = yBank, monoR = yBank;
      if (cfg.spreadOn) {
        std::array<float, 4> hs{}, hr{};
        for (size_t i = 0; i < 4; ++i) {
          hs[i] = modOn ? fastCentsToHz(c[i] - spC, hz[i]) : fastCentsToHz(-spC, hz[i]);
          hr[i] = modOn ? fastCentsToHz(c[i] + spC, hz[i]) : fastCentsToHz(spC, hz[i]);
        }
        monoL = vc.bankS.tick(cfg, prm, hs, amp, os);
        monoR = vc.bankR.tick(cfg, prm, hr, amp, orr);
      }
      // filter
      float yL = monoL, yR = cfg.spreadOn ? monoR : monoL;
      if (cfg.filterOn) {
        const float fc0 = fma_(fenv, prm.fenvAmount, prm.filterKey * kc) + prm.filterVel * vel;
        const float fcC = cfg.lfoOn ? fma_(lfoSig, prm.lfoFilter, fc0) : fc0;
        const float fc = fastCentsToHz(fcC, prm.filterFreq);
        const float g = pq::fastTan(3.141592653589793f * clipf(fc, 10.0f, 0.49f * kSr) / kSr);
        const float k = 2.0f - 1.98f * prm.filterRes;
        yL = vc.filtL.tick(cfg.filterType, cfg.filterSlope, cfg.filterCircuit, monoL, g, k, prm.morph, prm.filterDrive);
        if (cfg.spreadOn)
          yR = vc.filtR.tick(cfg.filterType, cfg.filterSlope, cfg.filterCircuit, monoR, g, k, prm.morph, prm.filterDrive);
        else yR = yL;
      }
      const float vol = lin(prm.volume);
      o.l = yL * (vol * std::min(1.0f, 1.0f - prm.pan));
      o.r = yR * (vol * std::min(1.0f, 1.0f + prm.pan));
      // done: every carrier that is on has released
      const int alg = cfg.algorithm;
      const bool carB = alg == 6 || alg == 8 || alg == 9 || alg == 10 || alg == 11;
      const bool carC = alg == 7 || alg == 9 || alg == 10 || alg == 11;
      const bool carD = alg == 11;
      bool d = cfg.on[0] ? ed[0] : !vi.gate;
      if (cfg.on[1] && carB) d = d && ed[1];
      if (cfg.on[2] && carC) d = d && ed[2];
      if (cfg.on[3] && carD) d = d && ed[3];
      o.done = d;
      o.op = cfg.spreadOn ? os : ol;
      o.lfo = lfoSig;
      o.penv = penv;
      o.fenv = fenv;
    }
  }
};

}  // namespace opref
