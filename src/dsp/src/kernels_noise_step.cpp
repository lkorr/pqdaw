// NZ.1: the noise_step atom (semantics: pq/dsp/atoms.h "Generators"; the colour design: pq/dsp/noise_step.h).
//
// One function per lane count; every input's rate is read at run time (KernelArgs::splMask), so a call costs per frame
// only what its active step sources need:
//   - first the QUICK paths (quickPath: HOLD — no lane steps or glides in the call — and WHITE — every frame white
//     uniform — when every input is a pattern its caches already hold); else the general path, out of line:
//   - a per-call setup: the keys of a Spl seed, the increments of a Spl rate and the colour design of a Spl colour,
//     each cached in the state per lane (recomputed only for a lane whose input changed bitwise);
//   - frame 0 alone, with every source checked, when a Spl trig / reset / sync cell has an edge there or a lane has not
//     drawn yet (a restarted voice);
//   - then one of: EVERY FRAME (every lane steps every frame, no sparse density, no steps: the draws — vectorised over
//     time for one lane —, the colour cascade, the fold; white uniform is exactly the `noise` atom's loop), the SCAN
//     loop (a Blk trig or reset that rises somewhere in the call, a sync beat: each frame checks them) or the RATE loop
//     (only the rate's phase; with a Spl rate whose periods are ≥ 32 frames it fills whole runs between steps, or runs
//     the glide while one moves).
// A step's work (the draw, the colour cascade, fold, density, steps, the glide's start) runs only on frames where some
// lane steps. Everything a frame computes depends only on the frame's inputs and the state, never on where a call
// starts: any split of the frames into calls gives the same bits (tests/dsp/test_noise_step.cpp). The loops keep the
// state in locals (no member traffic per frame).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "kernels_common.h"
#include "pq/dsp/noise_step.h"

namespace pq::dsp::detail {
namespace {

enum Input : uint32_t { kRate, kTrig, kBeat, kGrid, kReset, kSmooth, kColour, kDensity, kSteps, kSeed, kDist, kLanes };

constexpr float k2pm23 = 1.1920928955078125e-07f;  // 2^-23
constexpr float k2pm24 = 5.9604644775390625e-08f;  // 2^-24
constexpr uint32_t kGaussSalt = 0x6A09E667u, kDensitySalt = 0xBB67AE85u, kSeedMul = 0x9E3779B9u;
constexpr double k2p32 = 4294967296.0;

// lowbias32 (as the noise atom's): a bijective 32-bit mixer.
template <class S>
inline typename S::I mix(typename S::I x) noexcept {  // RT-safe
  x = S::xori(x, S::template srli<16>(x));
  x = S::muli(x, S::seti(0x7feb352d));
  x = S::xori(x, S::template srli<15>(x));
  x = S::muli(x, S::seti(static_cast<int32_t>(0x846ca68bu)));
  x = S::xori(x, S::template srli<16>(x));
  return x;
}
inline uint32_t mix1(uint32_t x) noexcept {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

template <class S>
inline typename S::F uniformOf(typename S::I h) noexcept {  // [−1, 1) in steps of 2^-23
  return S::mul(S::toFloat(S::template srai<8>(h)), S::set(k2pm23));
}
template <class S>
inline typename S::F gaussOf(typename S::I h0, typename S::I h1) noexcept {  // Irwin–Hall 4 of 16-bit halves, σ = 1/3
  const auto lo = S::seti(0xFFFF);
  auto s = S::addi(S::template srli<16>(h0), S::andi(h0, lo));
  s = S::addi(s, S::addi(S::template srli<16>(h1), S::andi(h1, lo)));
  return S::mul(S::toFloat(S::subi(s, S::seti(131070))), S::set(kNoiseGaussScale));
}
// The draw at counter offset n (n = t − origin) for keys k: uniform, or Gaussian where gm (all lanes when allG).
template <class S>
inline typename S::F drawOf(typename S::I k, typename S::I n, bool anyG, bool allG, typename S::F gm) noexcept {
  const auto h0 = mix<S>(S::addi(k, n));
  auto v = uniformOf<S>(h0);
  if (anyG) {
    const auto g = gaussOf<S>(h0, mix<S>(S::addi(S::xori(k, S::seti(static_cast<int32_t>(kGaussSalt))), n)));
    v = allG ? g : S::select(gm, g, v);
  }
  return v;
}

template <class S>
inline typename S::F foldOf(typename S::F v) noexcept {  // noiseFold, lane-wise, branch-free
  const auto u = S::add(v, S::set(1.0f));
  const auto m = S::sub(u, S::mul(S::set(4.0f), S::floor(S::mul(u, S::set(0.25f)))));
  const auto r = S::sub(S::set(1.0f), S::abs(S::sub(m, S::set(2.0f))));
  return S::select(S::gt(S::abs(v), S::set(1.0f)), r, v);
}

// After the cascade: lanes of a colour > 0 (pos) are bounded by their scale already (the clip only absorbs rounding);
// the others are folded (pq/dsp/noise_step.h, Q244).
template <class S>
inline typename S::F rangeOf(typename S::F v, typename S::F pos, bool anyPos) noexcept {
  const auto f = foldOf<S>(v);
  if (!anyPos) return f;
  return S::select(pos, S::min(S::max(v, S::set(-1.0f)), S::set(1.0f)), f);
}

// The sync cell floor(beat / grid), nudged up by 2^-21 of its size: beat and grid are floats, so a grid line that falls
// exactly on a frame (a triplet's, at most tempos) can come out a rounding below its integer; the nudge (above the
// quotient's worst rounding, 1.5 · 2^-23) puts it on its frame.
template <class S>
inline typename S::F cellOf(typename S::F beatV, typename S::F gridV) noexcept {
  const auto r = S::div(beatV, gridV);
  return S::floor(S::fma(S::abs(r), S::set(4.76837158203125e-07f), r));
}

// round(rate · 2^32 / sr) per lane as uint32, clamped to [0, 2^32 − 2] (NaN, ≤ 0: 0), in double.
template <class S>
inline typename S::I incOf(typename S::F r, double scale) noexcept {
  if constexpr (S::kW == 8) {
    auto half = [&](__m128 x) {
      __m256d d = _mm256_mul_pd(_mm256_cvtps_pd(x), _mm256_set1_pd(scale));
      d = _mm256_min_pd(_mm256_max_pd(d, _mm256_setzero_pd()), _mm256_set1_pd(4294967294.0));  // max(NaN, 0) = 0
      return _mm_xor_si128(_mm256_cvtpd_epi32(_mm256_sub_pd(d, _mm256_set1_pd(2147483648.0))), _mm_set1_epi32(INT32_MIN));
    };
    return _mm256_inserti128_si256(_mm256_castsi128_si256(half(_mm256_castps256_ps128(r))), half(_mm256_extractf128_ps(r, 1)), 1);
  } else {
    __m128d d = _mm_mul_sd(_mm_cvtss_sd(_mm_setzero_pd(), r), _mm_set_sd(scale));
    d = _mm_min_sd(_mm_max_sd(d, _mm_setzero_pd()), _mm_set_sd(4294967294.0));
    return _mm_xor_si128(_mm_cvtpd_epi32(_mm_sub_sd(d, _mm_set_sd(2147483648.0))), _mm_set1_epi32(INT32_MIN));
  }
}

// Whether every lane's bits of `a` equal `b`'s (cache checks).
template <class S>
inline bool sameBits(typename S::F a, typename S::F b) noexcept {
  return !S::any(S::bnot(S::asF(S::eqi(S::asI(a), S::asI(b)))));
}

// An input read at frame f: a Blk buffer (stride kW) or a Spl pattern (stride 0).
template <class S>
struct Rd {
  const float* p = nullptr;
  uint32_t stride = 0;
  typename S::F at(uint32_t f) const noexcept { return S::load(p + static_cast<size_t>(f) * stride); }
  bool spl() const noexcept { return stride == 0; }
};

template <class S>
struct Kernel {
  using F = typename S::F;
  using I = typename S::I;
  static constexpr uint32_t W = S::kW;

  // The state carried across frames: locals of the loops (load / store once per call).
  struct Regs {
    I phase, origin, lastStep;
    F hasLast, started, prevTrig, prevReset, prevCell, from, to, u, du, out;
    F w[5];
  };
  // What a call holds constant (or a Blk input's last value: rate, seed, colour).
  struct Call {
    I key, inc;
    F every, rateOn, periodRate;
    F z[4], gain, white, gaussM, pos;  // gain: the cascade's input scale (a colour > 0: its bound, a Gaussian's too)
    bool allWhite = true, anyGauss = false, allGauss = false, mayFold = false, anyPos = false;
    bool densOff = true, stepsOff = true, smoothOff = true;
  };

  const KernelArgs& a;
  NoiseStepState& st;
  float* out;
  Rd<S> rate, trig, beat, grid, reset, smooth, colour, density, steps, seed;
  float srF = 48000.0f;
  double incScale = k2p32 / 48000.0;
  Call c;

  explicit Kernel(const KernelArgs& args) noexcept
      : a(args), st(*static_cast<NoiseStepState*>(args.state)), out(args.out[0]) {
    // (field by field: a {pointer, stride} temporary copied as one 16-byte load stalls on its two stores, 10× a call)
    auto rd = [&](Rd<S>& r, uint32_t i) {
      r.p = a.in[i];
      r.stride = ((a.splMask >> i) & 1u) ? 0u : W;
    };
    rd(rate, kRate), rd(trig, kTrig), rd(beat, kBeat), rd(grid, kGrid), rd(reset, kReset);
    rd(smooth, kSmooth), rd(colour, kColour), rd(density, kDensity), rd(steps, kSteps), rd(seed, kSeed);
    srF = st.sampleRate > 0.0f ? st.sampleRate : 48000.0f;
    incScale = k2p32 / static_cast<double>(srF);
  }

  Regs load() const noexcept {
    Regs r;
    r.phase = S::loadu32(st.phase), r.origin = S::loadu32(st.origin), r.lastStep = S::loadu32(st.lastStep);
    r.hasLast = S::load(st.hasLast), r.started = S::load(st.started), r.prevTrig = S::load(st.prevTrig);
    r.prevReset = S::load(st.prevReset), r.prevCell = S::load(st.prevCell);
    r.from = S::load(st.from), r.to = S::load(st.to), r.u = S::load(st.u), r.du = S::load(st.du), r.out = S::load(st.out);
    for (uint32_t k = 0; k < 5; ++k) r.w[k] = S::load(st.w[k]);
    return r;
  }
  void save(const Regs& r) noexcept {
    S::storeu32(st.phase, r.phase), S::storeu32(st.origin, r.origin), S::storeu32(st.lastStep, r.lastStep);
    S::store(st.hasLast, r.hasLast), S::store(st.started, r.started), S::store(st.prevTrig, r.prevTrig);
    S::store(st.prevReset, r.prevReset), S::store(st.prevCell, r.prevCell);
    S::store(st.from, r.from), S::store(st.to, r.to), S::store(st.u, r.u), S::store(st.du, r.du), S::store(st.out, r.out);
    for (uint32_t k = 0; k < 5; ++k) S::store(st.w[k], r.w[k]);
  }

  // The keys of seed values `sv` per lane (cached by the seed's bits and the lanes const): the lane's stream (lanes
  // seeded apart: its seed and the voice group's; lanes 0: the node's for every lane) mixed with round(seed).
  I keysFor(F sv) noexcept {
    const F lanesOn = S::ge(S::load(a.in[kLanes]), S::set(0.5f));
    const F tag = S::bxor(sv, S::band(S::bnot(lanesOn), S::asF(S::seti(0x5A5A5A5A))));  // lanes 0: other bits
    if (sameBits<S>(tag, S::load(st.keyTag))) return S::loadu32(st.keys);
    alignas(32) float s[8], on[8];
    S::store(s, sv), S::store(on, lanesOn);
    for (uint32_t l = 0; l < W; ++l) {
      const float x = s[l] == s[l] ? std::clamp(s[l], -1e9f, 1e9f) : 0.0f;
      const auto si = static_cast<int32_t>(std::floor(x + 0.5f));
      const uint32_t lane = std::bit_cast<uint32_t>(on[l]) != 0u ? mix1(st.seed[l] ^ st.group) : st.base;
      st.keys[l] = mix1(lane ^ (static_cast<uint32_t>(si) * kSeedMul));
    }
    S::store(st.keyTag, tag);
    return S::loadu32(st.keys);
  }

  // The colour design for colour values `cv` (lanes whose value changed bitwise are designed again).
  void colourFor(F cv) noexcept {
    if (!sameBits<S>(cv, S::load(st.colourKey))) {
      alignas(32) float v[8], key[8];
      S::store(v, cv), S::store(key, S::load(st.colourKey));
      for (uint32_t l = 0; l < W; ++l) {
        if (std::bit_cast<uint32_t>(v[l]) == std::bit_cast<uint32_t>(key[l])) continue;
        const NoiseColourDesign d = noiseColourDesign(v[l]);
        for (uint32_t k = 0; k < 4; ++k) st.z[k][l] = d.z[k];
        st.gain[l] = d.gain * d.bound;  // colours > 0: the output stays in [−1, 1] (bound 1 otherwise)
        st.pos[l] = boolMask(!d.white && v[l] > 0.0f);
        st.white[l] = boolMask(d.white);
        st.colourKey[l] = v[l];
      }
    }
    for (uint32_t k = 0; k < 4; ++k) c.z[k] = S::load(st.z[k]);
    c.white = S::load(st.white), c.pos = S::load(st.pos);
    c.anyPos = S::any(c.pos);
    // A Gaussian lane of a colour > 0 is scaled by 1 / its draw's bound as well.
    c.gain = S::mul(S::load(st.gain), S::select(S::band(c.pos, c.gaussM), S::set(1.0f / kNoiseGaussBound), S::set(1.0f)));
    c.allWhite = !S::any(S::bnot(c.white));
  }

  // The rate's increment (cached by the rate's bits for a Spl rate), its "every frame" and "runs" masks, its period.
  void rateFor(F r, bool cache) noexcept {
    const F ok = S::gt(r, S::zero());  // NaN, ≤ 0: off
    c.every = S::band(ok, S::ge(r, S::set(srF)));
    c.rateOn = ok;
    c.periodRate = S::max(S::div(S::set(srF), r), S::set(1.0f));  // only read where rateOn
    if (cache && sameBits<S>(r, S::load(st.rateKey))) {
      c.inc = S::loadu32(st.inc);
      return;
    }
    c.inc = incOf<S>(r, incScale);
    if (cache) S::storeu32(st.inc, c.inc), S::store(st.rateKey, r);
  }

  // Frames [f0, f1). kScan: trig / reset / sync checked per frame. kRateBlk: the rate read per frame.
  template <bool kScan, bool kRateBlk>
  __declspec(noinline) void frames(Regs& R0, uint32_t f0, uint32_t f1) noexcept {
    I rPhase = R0.phase, rOrigin = R0.origin, rLastStep = R0.lastStep;
    F rHasLast = R0.hasLast, rStarted = R0.started, rPrevTrig = R0.prevTrig, rPrevReset = R0.prevReset,
      rPrevCell = R0.prevCell, rFrom = R0.from, rTo = R0.to, rU = R0.u, rDu = R0.du, rOut = R0.out;
    F w0 = R0.w[0], w1 = R0.w[1], w2 = R0.w[2], w3 = R0.w[3], w4 = R0.w[4];
    // (the state in plain locals: kept in registers through the loops and lambdas)
    const Call C = c;  // (copies: the loops' values live in registers, not behind `this`)
    I key = C.key, inc = C.inc;
    F every = C.every, rateOn = C.rateOn, periodRate = C.periodRate;
    float* const o = out;
    const uint32_t t0 = st.counter;
    const F signBit = S::asF(S::seti(static_cast<int32_t>(0x80000000u)));
    const F one = S::set(1.0f), zero = S::zero();
    const F nanV = S::set(std::numeric_limits<float>::quiet_NaN());
    const F p0 = S::set(kNoiseColourPoles[0]), p1 = S::set(kNoiseColourPoles[1]), p2 = S::set(kNoiseColourPoles[2]),
            p3 = S::set(kNoiseColourPoles[3]);
    bool allWhite = C.allWhite, mayFold = C.mayFold;
    F z0 = C.z[0], z1 = C.z[1], z2 = C.z[2], z3 = C.z[3], gain = C.gain, white = C.white, pos = C.pos;
    bool anyPos = C.anyPos;

    // One step on lanes `m` at frame f (counter t): draw, colour, fold, density, steps, the glide's start.
    auto step = [&](F m, uint32_t f, uint32_t t) {
      if (!seed.spl()) key = keysFor(seed.at(f));
      if (!colour.spl()) {
        colourFor(colour.at(f));
        allWhite = c.allWhite, mayFold = !allWhite || C.anyGauss;
        z0 = c.z[0], z1 = c.z[1], z2 = c.z[2], z3 = c.z[3], gain = c.gain, white = c.white, pos = c.pos;
        anyPos = c.anyPos;
      }
      const I tv = S::seti(static_cast<int32_t>(t));
      const I n = S::subi(tv, rOrigin);
      F v = drawOf<S>(key, n, C.anyGauss, C.allGauss, C.gaussM);
      if (!allWhite) {
        const F mc = S::andnot(white, m);  // coloured lanes that step
        const F x = S::mul(v, gain);
        const F y0 = S::fma(p0, w1, S::fnma(z0, w0, x));
        const F y1 = S::fma(p1, w2, S::fnma(z1, w1, y0));
        const F y2 = S::fma(p2, w3, S::fnma(z2, w2, y1));
        const F y3 = S::fma(p3, w4, S::fnma(z3, w3, y2));
        w0 = S::select(mc, x, w0), w1 = S::select(mc, y0, w1), w2 = S::select(mc, y1, w2);
        w3 = S::select(mc, y2, w3), w4 = S::select(mc, y3, w4);
        v = S::select(white, v, y3);
      }
      if (mayFold) v = rangeOf<S>(v, pos, anyPos);
      // The lane's period: its rate's, else measured since its previous step (unknown before one).
      const F known = S::bor(rateOn, rHasLast);
      const F period = S::select(rateOn, periodRate, S::toFloat(S::subi(tv, rLastStep)));
      // Density: keep with probability density · period / sr (top of the range, or an unknown period: always).
      F keep = S::ones();
      if (!C.densOff) {
        const F dens = density.at(f);
        keep = S::bor(S::ge(dens, S::set(kNoiseDensityContinuous)), S::bnot(known));
        const I hd = mix<S>(S::addi(S::xori(key, S::seti(static_cast<int32_t>(kDensitySalt))), n));
        const F u01 = S::mul(S::toFloat(S::template srli<8>(hd)), S::set(k2pm24));
        const F q = S::mul(S::mul(S::max(dens, zero), period), S::set(1.0f / srF));
        keep = S::bor(keep, S::lt(u01, q));
        v = S::band(keep, v);  // a dropped step outputs exactly 0
      }
      // Steps: N levels across [−1, 1] (N = 1: 0); a dropped step stays 0.
      if (!C.stepsOff) {
        const F nLev = S::round(steps.at(f));
        const F quant = S::band(keep, S::ge(nLev, one));
        const F nm1 = S::max(S::sub(nLev, one), one);
        const F k = S::round(S::mul(S::mul(S::add(v, one), S::set(0.5f)), nm1));
        const F lv = S::sub(S::mul(k, S::div(S::set(2.0f), nm1)), one);
        const F q = S::select(S::ge(nLev, S::set(2.0f)), S::min(S::max(lv, S::set(-1.0f)), one), zero);
        v = S::select(quant, q, v);
      }
      // The glide: from where the output is to the new value over smooth · period (0, or no period yet: a jump, the
      // value at once — u = 1 here is what the frame's first advance, min(0 + du, 1), gives for du ≥ 1).
      F duNew = S::set(2.0f);
      if (!C.smoothOff) {
        const F s = S::min(S::max(smooth.at(f), zero), one);  // NaN → 0
        const F glides = S::band(S::band(known, S::gt(s, zero)), S::lt(period, S::set(3.0e9f)));
        duNew = S::select(glides, S::div(one, S::mul(s, period)), S::set(2.0f));
      }
      rFrom = S::select(m, rOut, rFrom);
      rTo = S::select(m, v, rTo);
      rU = S::select(m, S::select(S::ge(duNew, one), one, zero), rU);
      rDu = S::select(m, duNew, rDu);
      rLastStep = S::selecti(S::asI(m), tv, rLastStep);
      rHasLast = S::bor(rHasLast, m);
    };
    // The output of a frame (the glide, or the held value).
    auto emit = [&]() -> F {
      if (!S::any(S::lt(rU, one))) return rTo;
      rU = S::min(S::add(rU, rDu), one);
      const F sm = S::mul(S::mul(rU, rU), S::fnma(S::set(2.0f), rU, S::set(3.0f)));  // u²(3 − 2u)
      return S::fnma(S::sub(rTo, rFrom), S::sub(one, sm), rTo);
    };
    // Frames [f, end) hold `to` (no lane glides, none steps).
    auto fill = [&](uint32_t f, uint32_t end) {
      if (f >= end) return;
      rOut = rTo;
      if constexpr (W == 8) {
        for (; f < end; ++f) S::store(o + static_cast<size_t>(f) * 8, rTo);
      } else {
        const __m256 v = _mm256_broadcastss_ps(rTo);
        for (; f + 8 <= end; f += 8) _mm256_storeu_ps(o + f, v);
        for (; f < end; ++f) S::store(o + f, rTo);
      }
    };

    bool runs = false;
    if constexpr (!kScan && !kRateBlk)  // run skipping: no lane steps every frame, every period ≥ 32 frames
      runs = !S::any(every) && !S::any(S::bnot(S::asF(S::eqi(S::template srli<27>(inc), S::seti(0)))));
    for (uint32_t f = f0; f < f1;) {
      if constexpr (!kScan && !kRateBlk) {
        if (runs) {
          // Frames before the first carry of any lane: phase + j·inc stays ≤ 2^32 − 1 for j ≤ (2^32 − 1 − phase) / inc
          // (exact in double: integers below 2^32).
          alignas(32) uint32_t ph[8], iv[8];
          S::storeu32(ph, rPhase), S::storeu32(iv, inc);
          uint32_t m = f1 - f;
          for (uint32_t l = 0; l < W; ++l)
            if (iv[l] != 0u) {
              const double room = static_cast<double>(0xFFFFFFFFu - ph[l]);
              if (room < static_cast<double>(iv[l]) * m) m = static_cast<uint32_t>(std::floor(room / static_cast<double>(iv[l])));
            }
          if (m > 0) {
            for (uint32_t l = 0; l < W; ++l) ph[l] += iv[l] * m;
            rPhase = S::loadu32(ph);
            const uint32_t end = f + m;
            for (; f < end && S::any(S::lt(rU, one)); ++f) S::store(o + static_cast<size_t>(f) * W, rOut = emit());
            fill(f, end);
            f = end;
            continue;
          }
        }
      }
      const uint32_t t = t0 + f;
      F stepM = zero;
      // A lane's first frame (a fresh state, a voice's note-on) draws at once, its stream where the counter is; a
      // rising reset also restarts the stream (origin = t) and clears the colour history.
      const F first = S::bnot(rStarted);
      F restart = zero, trigE = zero;
      if constexpr (kScan) {
        const F rs = reset.at(f), tr = trig.at(f);
        restart = S::andnot(rPrevReset, rs);
        stepM = trigE = S::andnot(rPrevTrig, tr);
        rPrevReset = rs, rPrevTrig = tr;
        const F g = grid.at(f);
        const F gOk = S::gt(g, zero);
        const F cell = S::select(gOk, cellOf<S>(beat.at(f), g), nanV);
        stepM = S::bor(stepM, S::band(gOk, S::ne(cell, rPrevCell)));
        rPrevCell = cell;
        if (S::any(restart)) {
          rOrigin = S::selecti(S::asI(restart), S::seti(static_cast<int32_t>(t)), rOrigin);
          w0 = S::andnot(restart, w0), w1 = S::andnot(restart, w1), w2 = S::andnot(restart, w2), w3 = S::andnot(restart, w3), w4 = S::andnot(restart, w4);
          rHasLast = S::andnot(restart, rHasLast);
        }
      }
      const F begin = S::bor(first, restart);
      rStarted = S::ones();
      if constexpr (kRateBlk) {
        rateFor(rate.at(f), false);
        inc = c.inc, every = c.every, rateOn = c.rateOn, periodRate = c.periodRate;
      }
      // The rate's phase: a carry is a step; a (re)start, a trig or every-frame stepping puts it at 0 (a trig is a
      // new value now, the rate's next one a period later: a voice's note-on starts its rate there).
      const I next = S::addi(rPhase, inc);
      const F carry = S::asF(S::gti(S::asI(S::bxor(S::asF(rPhase), signBit)), S::asI(S::bxor(S::asF(next), signBit))));
      rPhase = S::selecti(S::asI(S::bor(S::bor(begin, trigE), every)), S::seti(0), next);
      stepM = S::bor(stepM, S::bor(begin, S::band(rateOn, S::bor(carry, every))));
      if (S::any(stepM)) step(stepM, f, t);
      rOut = emit();
      S::store(o + static_cast<size_t>(f) * W, rOut);
      ++f;
    }
    if constexpr (kRateBlk) c.inc = inc;
    c.key = key;
    R0.phase = rPhase, R0.origin = rOrigin, R0.lastStep = rLastStep;
    R0.hasLast = rHasLast, R0.started = rStarted, R0.prevTrig = rPrevTrig, R0.prevReset = rPrevReset;
    R0.prevCell = rPrevCell, R0.from = rFrom, R0.to = rTo, R0.u = rU, R0.du = rDu, R0.out = rOut;
    R0.w[0] = w0, R0.w[1] = w1, R0.w[2] = w2, R0.w[3] = w3, R0.w[4] = w4;
  }

  // EVERY FRAME: every lane steps every frame (a Spl rate ≥ sr), no sparse density, no steps, no Blk seed / colour, no
  // edges in the call. The draws (for one lane: 8 frames per vector), the colour cascade (frame by frame: a
  // recursion), the fold. Leaves the state as the general loop does (each frame a jump to its value).
  __declspec(noinline) void everyFrame(Regs& R0, uint32_t f0) noexcept {
    I rPhase = R0.phase, rOrigin = R0.origin, rLastStep = R0.lastStep;
    F rHasLast = R0.hasLast, rStarted = R0.started, rPrevTrig = R0.prevTrig, rPrevReset = R0.prevReset,
      rPrevCell = R0.prevCell, rFrom = R0.from, rTo = R0.to, rU = R0.u, rDu = R0.du, rOut = R0.out;
    F w0 = R0.w[0], w1 = R0.w[1], w2 = R0.w[2], w3 = R0.w[3], w4 = R0.w[4];
    const uint32_t t0 = st.counter, n = a.frames;
    if (f0 >= n) return;
    const bool coloured = !c.allWhite, fold = c.mayFold;
    if constexpr (W == 1) {
      const uint32_t base = static_cast<uint32_t>(_mm_cvtsi128_si32(c.key)) - static_cast<uint32_t>(_mm_cvtsi128_si32(rOrigin)) + t0;
      const uint32_t salt = static_cast<uint32_t>(_mm_cvtsi128_si32(c.key)) ^ kGaussSalt;
      const uint32_t gbase = salt - static_cast<uint32_t>(_mm_cvtsi128_si32(rOrigin)) + t0;
      const bool gauss = c.anyGauss;  // one lane: all or none
      const __m128 z0 = c.z[0], z1 = c.z[1], z2 = c.z[2], z3 = c.z[3], g = c.gain, pos1 = c.pos;
      const bool posOn = c.anyPos;
      const __m128 q0 = _mm_set1_ps(kNoiseColourPoles[0]), q1 = _mm_set1_ps(kNoiseColourPoles[1]),
                   q2 = _mm_set1_ps(kNoiseColourPoles[2]), q3 = _mm_set1_ps(kNoiseColourPoles[3]);
      auto run8 = [&](uint32_t i, uint32_t count) {
        const auto lanesIdx = S8::laneIndex();
        const auto h0 = mix<S8>(S8::addi(S8::seti(static_cast<int32_t>(base + i)), lanesIdx));
        __m256 v = uniformOf<S8>(h0);
        if (gauss) v = gaussOf<S8>(h0, mix<S8>(S8::addi(S8::seti(static_cast<int32_t>(gbase + i)), lanesIdx)));
        if (coloured) {  // the recursion frame by frame, each output folded and stored as it comes
          alignas(32) float x[8];
          S8::store(x, v);
          for (uint32_t k = 0; k < count; ++k) {
            const __m128 xs = S1::mul(_mm_set_ss(x[k]), g);
            const __m128 y0 = S1::fma(q0, w1, S1::fnma(z0, w0, xs));
            const __m128 y1 = S1::fma(q1, w2, S1::fnma(z1, w1, y0));
            const __m128 y2 = S1::fma(q2, w3, S1::fnma(z2, w2, y1));
            const __m128 y3 = S1::fma(q3, w4, S1::fnma(z3, w3, y2));
            w0 = xs, w1 = y0, w2 = y1, w3 = y2, w4 = y3;
            S1::store(out + i + k, rangeOf<S1>(y3, pos1, posOn));  // (coloured: fold or clip always)
          }
          return;
        }
        if (fold) v = foldOf<S8>(v);
        if (count == 8) {
          S8::store(out + i, v);
        } else {
          alignas(32) float tmp[8];
          S8::store(tmp, v);
          std::memcpy(out + i, tmp, count * sizeof(float));
        }
      };
      uint32_t i = f0;
      for (; i + 8 <= n; i += 8) run8(i, 8);
      if (i < n) run8(i, n - i);
    } else {
      const F mc = S::bnot(c.white), white = c.white, gain = c.gain, z0 = c.z[0], z1 = c.z[1], z2 = c.z[2], z3 = c.z[3];
      const F posM = c.pos;
      const bool posOn = c.anyPos;
      const F gm = c.gaussM;
      const I key = c.key;
      const bool anyG = c.anyGauss, allG = c.allGauss;
      const F p0 = S::set(kNoiseColourPoles[0]), p1 = S::set(kNoiseColourPoles[1]), p2 = S::set(kNoiseColourPoles[2]),
              p3 = S::set(kNoiseColourPoles[3]);
      float* const o = out;
      for (uint32_t f = f0; f < n; ++f) {
        const I nn = S::subi(S::seti(static_cast<int32_t>(t0 + f)), rOrigin);
        F v = drawOf<S>(key, nn, anyG, allG, gm);
        if (coloured) {
          const F x = S::mul(v, gain);
          const F y0 = S::fma(p0, w1, S::fnma(z0, w0, x));
          const F y1 = S::fma(p1, w2, S::fnma(z1, w1, y0));
          const F y2 = S::fma(p2, w3, S::fnma(z2, w2, y1));
          const F y3 = S::fma(p3, w4, S::fnma(z3, w3, y2));
          w0 = S::select(mc, x, w0), w1 = S::select(mc, y0, w1), w2 = S::select(mc, y1, w2);
          w3 = S::select(mc, y2, w3), w4 = S::select(mc, y3, w4);
          v = S::select(white, v, y3);
        }
        if (fold) v = rangeOf<S>(v, posM, posOn);
        S::store(o + static_cast<size_t>(f) * 8, v);
      }
    }
    // As the general loop leaves it: the last frame's step jumped (or glided within the frame) to its value.
    rFrom = n - f0 >= 2 ? S::load(out + static_cast<size_t>(n - 2) * W) : rOut;
    rTo = rOut = S::load(out + static_cast<size_t>(n - 1) * W);
    F du = S::set(2.0f);
    if (!c.smoothOff) {
      const F s = S::min(S::max(smooth.at(n - 1), S::zero()), S::set(1.0f));
      du = S::select(S::gt(s, S::zero()), S::div(S::set(1.0f), S::mul(s, c.periodRate)), S::set(2.0f));
    }
    rDu = du;
    rU = S::set(1.0f);
    rLastStep = S::seti(static_cast<int32_t>(t0 + n - 1));
    rHasLast = S::ones();
    rPhase = S::seti(0);
    R0.phase = rPhase, R0.origin = rOrigin, R0.lastStep = rLastStep;
    R0.hasLast = rHasLast, R0.started = rStarted, R0.prevTrig = rPrevTrig, R0.prevReset = rPrevReset;
    R0.prevCell = rPrevCell, R0.from = rFrom, R0.to = rTo, R0.u = rU, R0.du = rDu, R0.out = rOut;
    R0.w[0] = w0, R0.w[1] = w1, R0.w[2] = w2, R0.w[3] = w3, R0.w[4] = w4;
  }

  void general() noexcept {
    const uint32_t n = a.frames;
    const F dist = S::load(a.in[kDist]);  // consts (Spl, per lane like every pattern): 0 uniform, 1 gaussian
    c.gaussM = S::ge(dist, S::set(0.5f));
    c.anyGauss = S::any(c.gaussM);
    c.allGauss = !S::any(S::bnot(c.gaussM));
    c.key = keysFor(seed.at(0));
    colourFor(colour.at(0));  // (a Blk colour: the first frame's here; each step designs its own frame's)
    c.mayFold = !c.allWhite || c.anyGauss;
    rateFor(rate.at(0), rate.spl());
    // Spl inputs that switch a step's stages off for the whole call.
    c.densOff = density.spl() && !S::any(S::lt(density.at(0), S::set(kNoiseDensityContinuous)));
    c.stepsOff = steps.spl() && !S::any(S::ge(S::round(steps.at(0)), S::set(1.0f)));
    c.smoothOff = smooth.spl() && !S::any(S::gt(smooth.at(0), S::zero()));
    Regs R = load();
    if (n == 0) {
      save(R);
      return;
    }
    // Which sources need a per-frame check; a Spl one can only change at frame 0. A Blk trig / reset that stays low
    // over the call has no edge in it (voice.trigger: almost every call) and leaves its previous level low.
    const F gOn = S::gt(grid.at(0), S::zero());
    const bool gridLive = !grid.spl() || S::any(gOn);
    const bool trigQuiet = trig.spl() || !blockAny(trig.p, n * W);
    const bool resetQuiet = reset.spl() || !blockAny(reset.p, n * W);
    const bool scan = !trigQuiet || !resetQuiet || (gridLive && !beat.spl());
    bool first = S::any(S::bnot(R.started));
    if (!scan) {
      first = first || S::any(S::andnot(R.prevTrig, trig.at(0))) || S::any(S::andnot(R.prevReset, reset.at(0)));
      if (gridLive) first = first || S::any(S::band(gOn, S::ne(cellOf<S>(beat.at(0), grid.at(0)), R.prevCell)));
    }
    const bool rateBlk = !rate.spl();
    uint32_t f = 0;
    if (first && !scan) {
      rateBlk ? frames<true, true>(R, 0, 1) : frames<true, false>(R, 0, 1);
      f = 1;
    }
    if (f < n) {
      const bool every = !scan && !rateBlk && seed.spl() && colour.spl() && c.densOff && c.stepsOff &&
                         !S::any(S::bnot(c.every));
      if (every) everyFrame(R, f);
      else if (scan) rateBlk ? frames<true, true>(R, f, n) : frames<true, false>(R, f, n);
      else rateBlk ? frames<false, true>(R, f, n) : frames<false, false>(R, f, n);
    }
    if (!scan) {  // the Spl edge sources' values stand for every frame of the call (a quiet Blk one: low)
      R.prevTrig = trig.at(0), R.prevReset = reset.at(0);
      R.prevCell = S::select(gOn, cellOf<S>(beat.at(0), grid.at(0)), S::set(std::numeric_limits<float>::quiet_NaN()));
    }
    st.counter += n;
    save(R);
  }
};

// The two calls most frequent by far, without the general setup (its caches must already hold the call's patterns:
// otherwise the general path runs and fills them): every input a pattern, every lane drawn already, no edge, no sync
// grid, density continuous, steps off, and either
//   HOLD   no lane steps in the call (a slow rate: every lane's phase stays below the wrap) and none glides: the held
//          values, the phase advanced;
//   WHITE  every lane steps every frame, white, uniform: the noise atom's loop.
// Each leaves exactly the state the general path leaves. Returns false (nothing done) otherwise.
template <class S>
inline bool quickPath(const KernelArgs& a) noexcept {  // RT-safe
using F = typename S::F;
using I = typename S::I;
constexpr uint32_t W = S::kW;
NoiseStepState& st = *static_cast<NoiseStepState*>(a.state);
float* const out = a.out[0];
const float srF = st.sampleRate > 0.0f ? st.sampleRate : 48000.0f;
  const uint32_t n = a.frames;
  constexpr uint32_t kSplNeeded = (1u << kRate) | (1u << kTrig) | (1u << kGrid) | (1u << kReset) | (1u << kSmooth) |
                                  (1u << kColour) | (1u << kDensity) | (1u << kSteps) | (1u << kSeed);
  if (n == 0 || (a.splMask & kSplNeeded) != kSplNeeded) return false;
  const F r = S::load(a.in[kRate]);
  if (S::any(S::bnot(S::load(st.started))) || !sameBits<S>(r, S::load(st.rateKey))) return false;
  const F tr = S::load(a.in[kTrig]), rs = S::load(a.in[kReset]);
  if (S::any(S::bor(S::andnot(S::load(st.prevTrig), tr), S::andnot(S::load(st.prevReset), rs)))) return false;
  if (S::any(S::gt(S::load(a.in[kGrid]), S::zero()))) return false;
  if (S::any(S::lt(S::load(a.in[kDensity]), S::set(kNoiseDensityContinuous)))) return false;
  if (S::any(S::ge(S::round(S::load(a.in[kSteps])), S::set(1.0f)))) return false;
  // (the seed's and the colour's caches hit: the general path would change no state for them either)
  const F col = S::load(a.in[kColour]);
  if (!sameBits<S>(col, S::load(st.colourKey))) return false;
  const F lanesOn = S::ge(S::load(a.in[kLanes]), S::set(0.5f));
  const F tag = S::bxor(S::load(a.in[kSeed]), S::band(S::bnot(lanesOn), S::asF(S::seti(0x5A5A5A5A))));
  if (!sameBits<S>(tag, S::load(st.keyTag))) return false;
  const F ok = S::gt(r, S::zero());
  const F every = S::band(ok, S::ge(r, S::set(srF)));
  const uint32_t t0 = st.counter;
  if (!S::any(every)) {  // HOLD?
    if (S::any(S::lt(S::load(st.u), S::set(1.0f)))) return false;
    alignas(32) uint32_t ph[8];
    std::memcpy(ph, st.phase, sizeof ph);
    for (uint32_t l = 0; l < W; ++l)
      if (static_cast<uint64_t>(0xFFFFFFFFu - ph[l]) < static_cast<uint64_t>(st.inc[l]) * n) return false;
    for (uint32_t l = 0; l < W; ++l) st.phase[l] = ph[l] + st.inc[l] * n;
    const F to = S::load(st.to);
    if constexpr (W == 8) {
      for (uint32_t f = 0; f < n; ++f) S::store(out + static_cast<size_t>(f) * 8, to);
    } else {
      const __m256 v = _mm256_broadcastss_ps(to);
      uint32_t f = 0;
      for (; f + 8 <= n; f += 8) _mm256_storeu_ps(out + f, v);
      for (; f < n; ++f) S::store(out + f, to);
    }
    S::store(st.out, to);
  } else {  // WHITE?
    if (S::any(S::bnot(every))) return false;
    if (S::any(S::ne(col, S::zero()))) return false;
    if (S::any(S::ge(S::load(a.in[kDist]), S::set(0.5f)))) return false;
    const I key = S::loadu32(st.keys), origin = S::loadu32(st.origin);
    if constexpr (W == 1) {
      const uint32_t c0 = st.keys[0] - st.origin[0] + t0;
      uint32_t i = 0;
      auto cv = S8::addi(S8::seti(static_cast<int32_t>(c0)), S8::laneIndex());
      for (; i + 8 <= n; i += 8) {
        S8::store(out + i, uniformOf<S8>(mix<S8>(cv)));
        cv = S8::addi(cv, S8::seti(8));
      }
      for (; i < n; ++i) S1::store(out + i, uniformOf<S1>(mix<S1>(S1::seti(static_cast<int32_t>(c0 + i)))));
    } else {
      auto cv = S::addi(S::subi(key, origin), S::seti(static_cast<int32_t>(t0)));
      for (uint32_t f = 0; f < n; ++f) {
        S::store(out + static_cast<size_t>(f) * 8, uniformOf<S>(mix<S>(cv)));
        cv = S::addi(cv, S::seti(1));
      }
    }
    // As the general path leaves it (everyFrame).
    S::store(st.from, n >= 2 ? S::load(out + static_cast<size_t>(n - 2) * W) : S::load(st.out));
    const F last = S::load(out + static_cast<size_t>(n - 1) * W);
    S::store(st.to, last), S::store(st.out, last);
    const F s = S::load(a.in[kSmooth]);
    const F periodRate = S::max(S::div(S::set(srF), r), S::set(1.0f));
    const bool smoothOff = !S::any(S::gt(s, S::zero()));
    F du = S::set(2.0f);
    if (!smoothOff) {
      const F sc = S::min(S::max(s, S::zero()), S::set(1.0f));
      du = S::select(S::gt(sc, S::zero()), S::div(S::set(1.0f), S::mul(sc, periodRate)), S::set(2.0f));
    }
    S::store(st.du, du), S::store(st.u, S::set(1.0f));
    S::storeu32(st.lastStep, S::seti(static_cast<int32_t>(t0 + n - 1)));
    S::store(st.hasLast, S::ones());
    S::storeu32(st.phase, S::seti(0));
  }
  S::store(st.prevTrig, tr), S::store(st.prevReset, rs);
  S::store(st.prevCell, S::set(std::numeric_limits<float>::quiet_NaN()));
  st.counter = t0 + n;
  return true;
}


// The general path, out of line: its frame (the kernel's state in locals, the loops' temporaries, saved registers)
// would cost every call — the quick ones included — a fixed ~50 ns if it were the entry's.
template <class S>
__declspec(noinline) void generalPath(const KernelArgs& a) noexcept {
  Kernel<S> k(a);
  k.general();
}

template <class S>
void noiseStepK(const KernelArgs& a) noexcept {  // RT-safe
  if (!quickPath<S>(a)) generalPath<S>(a);
}

}  // namespace

KernelFn findNoiseStepKernel(Atom a, uint32_t lanes, uint32_t) noexcept {
  if (a != Atom::NoiseStep) return nullptr;
  if (lanes == 1) return &noiseStepK<S1>;
  if (lanes == kGroupLanes) return &noiseStepK<S8>;
  return nullptr;
}

}  // namespace pq::dsp::detail
