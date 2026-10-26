#pragma once

// Hand-written C++ reference implementations of the stdlib subcircuits (T1b.3; tests and benchmarks only, never
// shipped). Each one is what a DSP programmer would write for the same algorithm as the .pqp file, with the same
// float32 arithmetic in the same order, so the tests can hold the compiled subcircuits to 1e-5 and the benchmark
// (bench/jit/bench_stdlib.cpp) can compare their cost with code that keeps everything in registers.
//
// Every filter is a template on the lane type V: `float` (one channel; the tests run one instance per voice) or
// `F8` (eight voices in one __m256, the layout of a voice group: frame-major, 8 lanes per frame). Masks are `bool`
// for float and `M8` (an __m256 of all-ones / zero lanes) for F8. Transcendentals: the SVML functions MSVC provides
// (_mm256_tan_ps …) for both, as the engine computes them.
//
// Fused multiply-adds: the optimiser turns add(mul(a, b), c) into one muladd (an FMA, one rounding) when the
// product has no other consumer and the same rate (pq/graph/optimise.h pass 3). The references write fma(a, b, c)
// at exactly those places, so the arithmetic is the same; a hand-written filter would use FMA there too.
//
// Not RT-checked (tests); no allocation after construction except DelayLine's constructor.

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace stdref {

constexpr float kPi = 3.141592653589793f;
constexpr float kTwoPi = 6.283185307179586f;

// ---- lane types ------------------------------------------------------------------------------------------------
struct M8 {
  __m256 m;
};
struct F8 {
  __m256 v;
  F8() : v(_mm256_setzero_ps()) {}
  F8(float x) : v(_mm256_set1_ps(x)) {}  // NOLINT: broadcast, like a scalar constant
  explicit F8(__m256 x) : v(x) {}
};
inline F8 operator+(F8 a, F8 b) { return F8(_mm256_add_ps(a.v, b.v)); }
inline F8 operator-(F8 a, F8 b) { return F8(_mm256_sub_ps(a.v, b.v)); }
inline F8 operator*(F8 a, F8 b) { return F8(_mm256_mul_ps(a.v, b.v)); }
inline F8 operator/(F8 a, F8 b) { return F8(_mm256_div_ps(a.v, b.v)); }
inline F8 operator-(F8 a) { return F8(_mm256_xor_ps(a.v, _mm256_set1_ps(-0.0f))); }
inline M8 operator<(F8 a, F8 b) { return {_mm256_cmp_ps(a.v, b.v, _CMP_LT_OQ)}; }
inline M8 operator>(F8 a, F8 b) { return {_mm256_cmp_ps(a.v, b.v, _CMP_GT_OQ)}; }
inline M8 operator>=(F8 a, F8 b) { return {_mm256_cmp_ps(a.v, b.v, _CMP_GE_OQ)}; }
inline M8 operator&&(M8 a, M8 b) { return {_mm256_and_ps(a.m, b.m)}; }
inline M8 operator||(M8 a, M8 b) { return {_mm256_or_ps(a.m, b.m)}; }
inline M8 operator!(M8 a) { return {_mm256_xor_ps(a.m, _mm256_castsi256_ps(_mm256_set1_epi32(-1)))}; }

inline float vmin(float a, float b) { return std::min(a, b); }
inline float vmax(float a, float b) { return std::max(a, b); }
inline float vabs(float a) { return std::fabs(a); }
inline float vsqrt(float a) { return std::sqrt(a); }
// The exact transcendentals are the SVML functions on one lane, as in the engine (QUESTIONS Q94: one implementation
// at every lane count). std::tan differs from them by up to 1 ulp, which a high-Q filter amplifies past 1e-5.
inline float vtan(float a) { return _mm256_cvtss_f32(_mm256_tan_ps(_mm256_set1_ps(a))); }
inline float vexp(float a) { return _mm256_cvtss_f32(_mm256_exp_ps(_mm256_set1_ps(a))); }
inline float vsin(float a) { return _mm256_cvtss_f32(_mm256_sin_ps(_mm256_set1_ps(a))); }
inline float vfloor(float a) { return std::floor(a); }
inline float sel(bool m, float a, float b) { return m ? a : b; }
inline float fma_(float a, float b, float c) {
  return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
}
inline double fma_(double a, double b, double c) { return std::fma(a, b, c); }
inline double vmin(double a, double b) { return std::min(a, b); }
inline double vmax(double a, double b) { return std::max(a, b); }
inline double vsqrt(double a) { return std::sqrt(a); }
inline double vtan(double a) { return std::tan(a); }
inline F8 vmin(F8 a, F8 b) { return F8(_mm256_min_ps(a.v, b.v)); }
inline F8 vmax(F8 a, F8 b) { return F8(_mm256_max_ps(a.v, b.v)); }
inline F8 vabs(F8 a) { return F8(_mm256_andnot_ps(_mm256_set1_ps(-0.0f), a.v)); }
inline F8 vsqrt(F8 a) { return F8(_mm256_sqrt_ps(a.v)); }
inline F8 vtan(F8 a) { return F8(_mm256_tan_ps(a.v)); }
inline F8 vexp(F8 a) { return F8(_mm256_exp_ps(a.v)); }
inline F8 vsin(F8 a) { return F8(_mm256_sin_ps(a.v)); }
inline F8 vfloor(F8 a) { return F8(_mm256_floor_ps(a.v)); }
inline F8 sel(M8 m, F8 a, F8 b) { return F8(_mm256_blendv_ps(b.v, a.v, m.m)); }
inline F8 fma_(F8 a, F8 b, F8 c) { return F8(_mm256_fmadd_ps(a.v, b.v, c.v)); }
template <class V>
V vclip(V x, V lo, V hi) {
  return vmin(vmax(x, lo), hi);
}

template <class V> struct MaskOf;
template <> struct MaskOf<float> { using type = bool; };
template <> struct MaskOf<F8> { using type = M8; };
template <class V> using Mask = typename MaskOf<V>::type;
inline bool falseMask(bool) { return false; }
inline M8 falseMask(M8) { return {_mm256_setzero_ps()}; }

// Frame t of a block buffer: one float (mono) or eight lanes (a voice group).
inline float load(const float* p, uint32_t t, float) { return p[t]; }
inline F8 load(const float* p, uint32_t t, F8) { return F8(_mm256_loadu_ps(p + static_cast<size_t>(t) * 8)); }
inline void store(float* p, uint32_t t, float v) { p[t] = v; }
inline void store(float* p, uint32_t t, F8 v) { _mm256_storeu_ps(p + static_cast<size_t>(t) * 8, v.v); }
inline bool loadMask(const float* p, uint32_t t, bool) { return p[t] != 0.0f; }
inline M8 loadMask(const float* p, uint32_t t, M8) {
  return {_mm256_cmp_ps(_mm256_loadu_ps(p + static_cast<size_t>(t) * 8), _mm256_setzero_ps(), _CMP_NEQ_UQ)};
}

// Prewarped integrator gain of the TPT designs: tan(π·clip(fc, 0, 0.49·sr)/sr).
template <class V>
V prewarp(V cutoff, float sr) {
  return vtan(kPi * vclip(cutoff, V(0.0f), V(0.49f * sr)) / V(sr));
}

// ---- onepole (stdlib/filters/onepole.pqp) -------------------------------------------------------------------------
template <class V>
struct OnePole {
  V s{};
  static V coef(V cutoff, float sr) {
    const V g = prewarp(cutoff, sr);
    return g / (V(1.0f) + g);
  }
  // Returns lp; hp = x − lp, allpass = lp − hp.
  V tick(V x, V G) {
    const V v = (x - s) * G;
    const V low = s + v;
    s = low + v;
    return low;
  }
};

// ---- svf (stdlib/filters/svf.pqp) -----------------------------------------------------------------------------------
template <class V>
struct SvfCore {
  V ic1{}, ic2{};
  struct Coef {
    V a1, a2, a3;
  };
  static Coef coef(V g, V k) {
    Coef c;
    c.a1 = V(1.0f) / fma_(g, g + k, V(1.0f));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
  }
  void tick(V x, const Coef& c, V& band, V& low) {
    const V v3 = x - ic2;
    const V v1 = fma_(c.a1, ic1, c.a2 * v3);
    const V v2 = fma_(c.a3, v3, fma_(c.a2, ic1, ic2));
    ic1 = V(2.0f) * v1 - ic1;
    ic2 = V(2.0f) * v2 - ic2;
    band = v1;
    low = v2;
  }
};

// Every response of `svf`, from one core each for the plain outputs, the bell and the two shelves.
template <class V>
struct Svf {
  SvfCore<V> f, b, ls, hs;
  struct Coef {
    V k, A, gain;
    typename SvfCore<V>::Coef cf, cb, cls, chs;
  };
  static Coef coef(V cutoff, V q, V gain, float sr) {
    Coef c;
    const V g = prewarp(cutoff, sr);
    c.k = V(1.0f) / vmax(q, V(0.01f));
    c.A = vsqrt(gain);
    c.gain = gain;
    c.cf = SvfCore<V>::coef(g, c.k);
    c.cb = SvfCore<V>::coef(g, c.k / c.A);
    c.cls = SvfCore<V>::coef(g / vsqrt(c.A), c.k);
    c.chs = SvfCore<V>::coef(g * vsqrt(c.A), c.k);
    return c;
  }
  struct Out {
    V lp, bp, hp, notch, peak, allpass, bell, lowshelf, highshelf;
  };
  Out tick(V x, const Coef& c) {
    Out o;
    V band, low;
    f.tick(x, c.cf, band, low);
    o.lp = low;
    o.bp = c.k * band;
    o.notch = x - o.bp;
    o.hp = o.notch - low;
    o.peak = o.notch - V(2.0f) * low;
    o.allpass = x - V(2.0f) * c.k * band;
    b.tick(x, c.cb, band, low);
    o.bell = fma_(c.k / c.A * (c.gain - V(1.0f)), band, x);
    ls.tick(x, c.cls, band, low);
    o.lowshelf = fma_(c.gain - V(1.0f), low, fma_(c.k * (c.A - V(1.0f)), band, x));
    hs.tick(x, c.chs, band, low);
    o.highshelf = fma_(V(1.0f) - c.gain, low, fma_(c.gain, x, c.k * (V(1.0f) - c.A) * c.A * band));
    return o;
  }
};

// ---- biquad (stdlib/filters/biquad.pqp) -----------------------------------------------------------------------------
template <class V>
struct BiquadCoef {
  V b0{1.0f}, b1{}, b2{}, a1{}, a2{};
  // b2 is b0 (the low-pass, high-pass and notch designs pass one value to both): the compiler then computes b0·x
  // once (CSE), so y = b0·x + s1 is a plain add instead of an FMA. A hand-written symmetric biquad does the same.
  bool b2IsB0 = false;
};

template <class V>
struct Biquad {
  V s1{}, s2{};
  V tick(V x, const BiquadCoef<V>& c) {
    if (c.b2IsB0) {
      const V bx = c.b0 * x;
      const V y = bx + s1;
      s1 = fma_(c.b1, x, s2) - c.a1 * y;
      s2 = bx - c.a2 * y;
      return y;
    }
    const V y = fma_(c.b0, x, s1);
    s1 = fma_(c.b1, x, s2) - c.a1 * y;
    s2 = c.b2 * x - c.a2 * y;
    return y;
  }
};

enum class BiquadType { Lowpass, Highpass, Bandpass, Notch, Allpass, Peak, Lowshelf, Highshelf };

// The designers' coefficients, in the .pqp files' operation order.
template <class V>
BiquadCoef<V> design(BiquadType type, V cutoff, V q, V gain, float sr) {
  const V one(1.0f), two(2.0f);
  const V K = prewarp(cutoff, sr);
  const V KK = K * K;
  BiquadCoef<V> c;
  switch (type) {
    case BiquadType::Lowpass:
    case BiquadType::Highpass:
    case BiquadType::Bandpass:
    case BiquadType::Notch:
    case BiquadType::Allpass: {
      const V n = one / (one + K / q + KK);
      c.a1 = two * (KK - one) * n;
      c.a2 = (one - K / q + KK) * n;
      c.b2IsB0 = type == BiquadType::Lowpass || type == BiquadType::Highpass || type == BiquadType::Notch;
      if (type == BiquadType::Lowpass) {
        c.b0 = KK * n;
        c.b1 = two * c.b0;
        c.b2 = c.b0;
      } else if (type == BiquadType::Highpass) {
        c.b0 = n;
        c.b1 = V(-2.0f) * n;
        c.b2 = n;
      } else if (type == BiquadType::Bandpass) {
        c.b0 = K / q * n;
        c.b1 = V(0.0f);
        c.b2 = -c.b0;
      } else if (type == BiquadType::Notch) {
        c.b0 = (one + KK) * n;
        c.b1 = c.a1;
        c.b2 = c.b0;
      } else {
        c.b0 = c.a2;
        c.b1 = c.a1;
        c.b2 = one;
      }
      break;
    }
    case BiquadType::Peak: {
      const V A = vsqrt(gain);
      const V n = one / (one + K / (q * A) + KK);
      c.a1 = two * (KK - one) * n;
      c.b0 = (one + K * A / q + KK) * n;
      c.b1 = c.a1;
      c.b2 = (one - K * A / q + KK) * n;
      c.a2 = (one - K / (q * A) + KK) * n;
      break;
    }
    case BiquadType::Lowshelf: {
      const V A = vsqrt(gain);
      const V sA = vsqrt(A) * K / q;
      const V AKK = A * K * K;
      const V n = one / (A + KK + sA);
      c.b0 = A * (one + AKK + sA) * n;
      c.b1 = two * A * (AKK - one) * n;
      c.b2 = A * (one + AKK - sA) * n;
      c.a1 = two * (KK - A) * n;
      c.a2 = (A + KK - sA) * n;
      break;
    }
    case BiquadType::Highshelf: {
      const V A = vsqrt(gain);
      const V sA = vsqrt(A) * K / q;
      const V AKK = A * K * K;
      const V n = one / (one + AKK + sA);
      c.b0 = A * (A + KK + sA) * n;
      c.b1 = two * A * (KK - A) * n;
      c.b2 = A * (A + KK - sA) * n;
      c.a1 = two * (AKK - one) * n;
      c.a2 = (one + AKK - sA) * n;
      break;
    }
  }
  return c;
}

// ---- delay line (the `delay` atom's linear read, pq/dsp kernels_memory.cpp) ----------------------------------------
// Ring of `lanes` interleaved lanes; read(d) is the linearly interpolated value d samples old (d clamped to
// [1, maxDelay]); write(x) stores the current sample and advances.
class DelayLine {
public:
  DelayLine(uint32_t maxDelayFrames, uint32_t lanes) : lanes_(lanes), maxDelay_(static_cast<float>(maxDelayFrames)) {
    uint32_t cap = 1;
    while (cap < maxDelayFrames + 10) cap <<= 1;
    mask_ = cap - 1;
    ring_.assign(static_cast<size_t>(cap) * lanes, 0.0f);
  }
  float read(float dIn, uint32_t lane = 0) const {
    const float d = std::min(std::max(dIn, 1.0f), maxDelay_);
    const float fl = std::floor(d);
    const float fr = d - fl;
    const uint32_t n = static_cast<uint32_t>(fl);
    const float x0 = ring_[(((pos_ - n) & mask_) * lanes_) + lane];
    const float x1 = ring_[(((pos_ - n - 1) & mask_) * lanes_) + lane];
    return std::fma(fr, x1 - x0, x0);
  }
  F8 read(F8 dIn) const {
    const __m256 d = _mm256_min_ps(_mm256_max_ps(dIn.v, _mm256_set1_ps(1.0f)), _mm256_set1_ps(maxDelay_));
    const __m256 fl = _mm256_floor_ps(d);
    const __m256 fr = _mm256_sub_ps(d, fl);
    const __m256i n = _mm256_cvttps_epi32(fl);
    const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256i m = _mm256_set1_epi32(static_cast<int>(mask_));
    const __m256i p0 = _mm256_sub_epi32(_mm256_set1_epi32(static_cast<int>(pos_)), n);
    const __m256i i0 = _mm256_add_epi32(_mm256_slli_epi32(_mm256_and_si256(p0, m), 3), lane);
    const __m256i i1 = _mm256_add_epi32(
        _mm256_slli_epi32(_mm256_and_si256(_mm256_sub_epi32(p0, _mm256_set1_epi32(1)), m), 3), lane);
    const __m256 x0 = _mm256_i32gather_ps(ring_.data(), i0, 4);
    const __m256 x1 = _mm256_i32gather_ps(ring_.data(), i1, 4);
    return F8(_mm256_fmadd_ps(fr, _mm256_sub_ps(x1, x0), x0));
  }
  void write(float x, uint32_t lane = 0) { ring_[((pos_ & mask_) * lanes_) + lane] = x; }
  void write(F8 x) { _mm256_storeu_ps(ring_.data() + static_cast<size_t>(pos_ & mask_) * 8, x.v); }
  void advance() { ++pos_; }

private:
  std::vector<float> ring_;
  uint32_t lanes_, mask_ = 0, pos_ = 0;
  float maxDelay_;
};
inline float readDelay(const DelayLine& d, float t) { return d.read(t); }
inline F8 readDelay(const DelayLine& d, F8 t) { return d.read(t); }
inline void writeDelay(DelayLine& d, float x) { d.write(x); }
inline void writeDelay(DelayLine& d, F8 x) { d.write(x); }

// ceil(max · sr): the frames a delay line of `maxSeconds` holds (the schedule's sizing).
inline uint32_t delayFrames(float maxSeconds, float sr) {
  return static_cast<uint32_t>(std::ceil(static_cast<double>(maxSeconds) * sr));
}

// ---- allpass (stdlib/filters/allpass.pqp) ----------------------------------------------------------------------------
// First-order: OnePole's allpass output. Schroeder (delay) allpass:
template <class V>
struct AllpassDelay {
  DelayLine line;
  AllpassDelay(float maxSeconds, float sr, uint32_t lanes) : line(delayFrames(maxSeconds, sr), lanes) {}
  static V delaySamples(V time, float sr) { return vmax(time, V(0.0001f)) * V(sr); }
  V tick(V x, V d, V g) {
    const V r = readDelay(line, d);
    const V w = fma_(g, r, x);
    writeDelay(line, w);
    line.advance();
    return r - g * w;
  }
};

// ---- dcblock (stdlib/filters/dcblock.pqp) ----------------------------------------------------------------------------
template <class V>
struct DcBlock {
  V x1{}, y1{};
  static V coef(V cutoff, float sr) { return vexp(V(-kTwoPi) * cutoff / V(sr)); }
  V tick(V x, V R) {
    const V y = fma_(R, y1, x - x1);
    x1 = x;
    y1 = y;
    return y;
  }
};

// ---- comb (stdlib/filters/comb.pqp) --------------------------------------------------------------------------------
template <class V>
struct Comb {
  DelayLine line;
  Comb(float maxSeconds, float sr, uint32_t lanes) : line(delayFrames(maxSeconds, sr), lanes) {}
  static V delaySamples(V time, float sr) { return vmax(time, V(0.0001f)) * V(sr); }
  V tick(V x, V d, V fb) {
    const V y = fma_(fb, readDelay(line, d), x);
    writeDelay(line, y);
    line.advance();
    return y;
  }
};

// Damped comb: the linear-phase FIR a·y + (1 − 2a)·y[n−1] + a·y[n−2] (a = damp/4, one sample of delay) in front of a
// delay line one sample shorter.
template <class V>
struct CombLp {
  DelayLine line;
  V y1{}, y2{};  // y[n − 1], y[n − 2]
  CombLp(float maxSeconds, float sr, uint32_t lanes) : line(delayFrames(maxSeconds, sr), lanes) {}
  static V delaySamples(V time, float sr) { return vmax(time - V(1.0f / sr), V(0.0001f)) * V(sr); }
  V tick(V x, V d, V fb, V damp) {
    const V a = damp * V(0.25f);
    const V y = fma_(fb, readDelay(line, d), x);
    const V lp = fma_(a, y + y2, (V(1.0f) - V(2.0f) * a) * y1);
    writeDelay(line, lp);
    line.advance();
    y2 = y1;
    y1 = y;
    return y;
  }
};

// ---- adsr (stdlib/envelopes/adsr.pqp) ------------------------------------------------------------------------------
template <class V>
struct Adsr {
  using M = Mask<V>;
  V level{};
  M attacking = falseMask(M{}), prevGate = falseMask(M{});
  struct Coef {
    V rise, dcoef, rcoef, s;
  };
  static Coef coef(V a, V d, V s, V r, float sr) {
    constexpr float kLn1e4 = 9.210340371976184f;
    Coef c;
    c.rise = V(1.0f) / vmax(a * V(sr), V(1.0f));
    c.dcoef = vexp(V(-kLn1e4) / vmax(d * V(sr), V(1.0f)));
    c.rcoef = vexp(V(-kLn1e4) / vmax(r * V(sr), V(1.0f)));
    c.s = s;
    return c;
  }
  // Returns the level; `done` = released and below −80 dB.
  V tick(M gate, const Coef& c, M& done) {
    const M rise = gate && !prevGate;
    prevGate = gate;
    const M reset = level >= V(1.0f) || !gate;
    attacking = (attacking || rise) && !reset;  // srff: reset wins
    const V next = sel(attacking, vmin(level + c.rise, V(1.0f)),
                       sel(gate, fma_(level - c.s, c.dcoef, c.s), level * c.rcoef));
    level = next;
    done = !gate && next < V(0.0001f);
    return next;
  }
};

// ---- envfollow (stdlib/dynamics/envfollow.pqp) ------------------------------------------------------------------------
template <class V>
struct EnvFollow {
  V env{};
  static V coef(V time, float sr) { return vexp(V(-1.0f) / vmax(time * V(sr), V(0.000001f))); }
  V tick(V x, V ca, V cr, bool rms) {
    const V det = rms ? x * x : vabs(x);
    const V prev = env;
    env = fma_(prev - det, sel(det > prev, ca, cr), det);
    return rms ? vsqrt(env) : env;
  }
};

// ---- slew, smooth (stdlib/control/) --------------------------------------------------------------------------------
template <class V>
struct Slew {
  V y{};
  static V up(V rise, float sr) { return V(1.0f) / vmax(rise * V(sr), V(0.000001f)); }
  static V down(V fall, float sr) { return V(-1.0f) / vmax(fall * V(sr), V(0.000001f)); }
  V tick(V x, V up, V down) {
    y = y + vclip(x - y, down, up);
    return y;
  }
};

template <class V>
struct Smooth {
  V y{};
  static V coef(V time, float sr) { return V(1.0f) - vexp(V(-1.0f) / vmax(time * V(sr), V(0.000001f))); }
  V tick(V x, V c) {
    y = fma_(x - y, c, y);
    return y;
  }
};

// ---- lfo (stdlib/control/lfo.pqp) ----------------------------------------------------------------------------------
// The `phasor` atom: uint32 phase, increment round(hz · 2^32/sr) (hz clamped to ±sr/2, NaN → 0), output the top
// 24 bits as [0, 1) before the increment; reset sets the phase to 0 for that sample.
struct Phasor {
  uint32_t phase = 0;
  float tick(float hz, bool reset, float sr) {
    if (reset) phase = 0;
    const float out = static_cast<float>(phase >> 8) * 5.9604644775390625e-08f;
    const float half = sr * 0.5f;
    const float h = hz != hz ? 0.0f : std::min(std::max(hz, -half), half);
    const double inc = std::nearbyint(static_cast<double>(h) * (4294967296.0 / sr));
    phase += static_cast<uint32_t>(static_cast<int64_t>(inc));
    return out;
  }
};

struct Lfo {
  Phasor ph;
  struct Out {
    float sine, tri, saw, square, cycle;
  };
  static float frac(float x) { return x - std::floor(x); }
  // `beat`: the transport position (transport.beat) at this sample.
  Out tick(float rate, bool reset, bool sync, float beats, float phase, float beat, float sr) {
    const float free = ph.tick(rate, reset, sr);
    const float song = frac(beat / beats);
    const float p = frac((sync ? song : free) + phase);
    Out o;
    o.cycle = p;
    o.sine = vsin(kTwoPi * (p - std::nearbyint(p)));  // the `sine` atom's argument reduction (it is a fast sine)
    o.tri = 4.0f * std::fabs(frac(p + 0.75f) - 0.5f) - 1.0f;
    o.saw = 2.0f * frac(p + 0.5f) - 1.0f;
    o.square = p < 0.5f ? 1.0f : -1.0f;
    return o;
  }
};

}  // namespace stdref
