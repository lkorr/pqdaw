// Smoothed gains (T3.17-fix2, M3 review H1): gain (x, g, time) and gain_add (acc, x, g, time). Semantics in
// pq/dsp/atoms.h. The mixer's hot path: a gain that is not moving costs one multiply per sample (nothing at all for
// gain_add at 0: the accumulator passes through, in place when the scheduler gave the output the accumulator's
// buffer); a moving one costs one more multiply-add (8 lanes: per lane and frame; 1 lane: a scalar recurrence per
// frame, only while it moves: ~20 time constants after a change).
//
// The 1-lane param path (T4.x-fix-dsp) is defined frame by frame, so that the result does not depend on how a block
// is split into calls and the JIT can generate the same arithmetic inline (in a per-sample loop or a chain, per
// frame; in a vectorised run, its settled form): at the call's start gainPrologue (prepare, retarget); then per frame
//   y = target + d;  out = (d == 0 && target == 0) ? (gain_add ? acc : 0) : fma(x, y, acc or 0);
//   d = d·r;  d = |d| < kGainSettled ? 0 : d.
// While d is 0 for the whole call this is the settled vector path (one multiply-add per sample, or a copy / fill).
//
// State: the target the gain is heading for and the residual d (gain = target + d). Keeping d itself, rather than
// the gain, lets it decay to the snap threshold exactly: a gain stored as a float stalls about an ulp of the target
// away, where target + d·r rounds back to the same float (a plain one-pole's float fixed point).
//
// `start` (T4.x-fix-dsp, the last input, a const): 0 = a new instance (or a voice lane after its reset) fades in
// from 0, as the mixer's gain stages do; 1 = its first block starts at the target (devices: a knob's value is
// there from the first sample). Either way one compare per block and lane.

#include <cmath>
#include <cstring>

#include "kernels_common.h"

namespace pq::dsp::detail {
namespace {

// Below this distance from the target (absolute: ≈ −150 dB) a moving gain snaps onto it and is settled.
constexpr float kSettled = kGainSettled;

// r = e^(−1/τ) for τ samples (0: no smoothing), and its powers for the 1-lane closed form, cached per τ.
inline void prepare(GainState& st, uint32_t lane, float tau) noexcept {
  if (st.tau[lane] == tau) return;
  st.tau[lane] = tau;
  const double r = tau > 0.0f && std::isfinite(tau) ? std::exp(-1.0 / static_cast<double>(tau)) : 0.0;
  st.r[lane] = static_cast<float>(r);
  if (lane == 0) {
    double p = 1.0;
    for (uint32_t k = 0; k < 8; ++k, p *= r) st.rpow[k] = static_cast<float>(p);
    st.r8 = static_cast<float>(p);
  }
}

inline float finiteOr0(float v) noexcept { return std::isfinite(v) ? v : 0.0f; }

// The residual towards a (new) target g: the gain stays where it is, the residual moves. A lane's first block with
// `start` set begins at g.
inline float retarget(GainState& st, uint32_t lane, float g, bool startAtTarget) noexcept {
  if (st.started[lane] == 0.0f) {
    st.started[lane] = 1.0f;
    if (startAtTarget) {
      st.target[lane] = g;
      st.d[lane] = 0.0f;
      return 0.0f;
    }
  }
  if (st.target[lane] != g) {
    st.d[lane] = (st.target[lane] + st.d[lane]) - g;
    st.target[lane] = g;
  }
  return st.d[lane];
}

// Spl operands are 8-float patterns: lane 0 is the value for L = 1.
template <bool Spl>
inline __m256 load8(const float* p, uint32_t i) noexcept {
  if constexpr (Spl) return _mm256_set1_ps(p[0]);
  else return _mm256_loadu_ps(p + i);
}
template <bool Spl>
inline float load1(const float* p, uint32_t i) noexcept {
  if constexpr (Spl) return p[0];
  else return p[i];
}

// kAdd: gain_add (input 0 is the accumulator). SM bits: [acc,] x, g (time and start must be Spl: splRequired).
template <bool kAdd>
struct Gain {
  static constexpr uint32_t kA = kAdd ? 1u : 0u;  // index of x
  static constexpr uint32_t kSpec = kA + 2;
  static constexpr uint32_t kStart = kA + 3;  // index of start

  // Lane l's `start` (a Spl pattern; absent when a caller passes only the T3.17-fix2 inputs: 0).
  static bool startAt(const KernelArgs& a, uint32_t lane) noexcept {
    return a.numIn > kStart && a.in[kStart][lane] >= 0.5f;
  }

  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    if (a.frames == 0) return;
    constexpr bool accSpl = kAdd && splBit(SM, 0);
    constexpr bool xSpl = splBit(SM, kA);
    constexpr bool gSpl = splBit(SM, kA + 1);
    if constexpr (S::kW == 1) {
      if constexpr (gSpl) param1<accSpl, xSpl>(a);
      else audio1<accSpl, xSpl>(a);
    } else {
      if constexpr (gSpl) param8<accSpl, xSpl>(a);
      else audio8<accSpl, xSpl>(a);
    }
  }

  // ---- 1 lane (global) ----
  // An audio-rate gain is used as it is (smoothing is for stepwise param changes).
  template <bool accSpl, bool xSpl>
  static void audio1(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<GainState*>(a.state);
    const uint32_t n = a.frames, vecEnd = n & ~7u;
    const float* acc = kAdd ? a.in[0] : nullptr;
    const float* x = a.in[kA];
    const float* g = a.in[kA + 1];
    float* out = a.out[0];
    for (uint32_t i = 0; i < vecEnd; i += 8) {
      const __m256 s = kAdd ? load8<accSpl>(acc, i) : _mm256_setzero_ps();
      _mm256_storeu_ps(out + i, _mm256_fmadd_ps(load8<xSpl>(x, i), _mm256_loadu_ps(g + i), s));
    }
    for (uint32_t i = vecEnd; i < n; ++i) out[i] = std::fma(load1<xSpl>(x, i), g[i], kAdd ? load1<accSpl>(acc, i) : 0.0f);
    st.target[0] = finiteOr0(g[n - 1]);
    st.d[0] = 0.0f;
    st.started[0] = 1.0f;
  }

  // A param gain (the frame-by-frame definition in the header comment).
  template <bool accSpl, bool xSpl>
  static void param1(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<GainState*>(a.state);
    const uint32_t n = a.frames, vecEnd = n & ~7u;
    const float* acc = kAdd ? a.in[0] : nullptr;
    const float* x = a.in[kA];
    float* out = a.out[0];
    auto accAt8 = [&](uint32_t i) noexcept { return kAdd ? load8<accSpl>(acc, i) : _mm256_setzero_ps(); };
    auto accAt1 = [&](uint32_t i) noexcept { return kAdd ? load1<accSpl>(acc, i) : 0.0f; };
    const float gv = finiteOr0(a.in[kA + 1][0]);
    float d = prologue1(st, gv, a.in[kA + 2][0], startAt(a, 0));
    if (d == 0.0f) {  // settled
      if (gv == 0.0f) {
        if constexpr (kAdd) {
          if constexpr (accSpl) {
            const __m256 v = _mm256_set1_ps(acc[0]);
            for (uint32_t i = 0; i < vecEnd; i += 8) _mm256_storeu_ps(out + i, v);
            for (uint32_t i = vecEnd; i < n; ++i) out[i] = acc[0];
          } else if (out != acc) {
            std::memcpy(out, acc, sizeof(float) * n);
          }  // else in place: nothing to do
        } else {
          std::memset(out, 0, sizeof(float) * n);
        }
        return;
      }
      const __m256 gv8 = _mm256_set1_ps(gv);
      for (uint32_t i = 0; i < vecEnd; i += 8)
        _mm256_storeu_ps(out + i, _mm256_fmadd_ps(load8<xSpl>(x, i), gv8, accAt8(i)));
      for (uint32_t i = vecEnd; i < n; ++i) out[i] = std::fma(load1<xSpl>(x, i), gv, accAt1(i));
      return;
    }
    // Moving: frame by frame (gain_k = gv + d_k, d_{k+1} = d_k·r snapped; the first frame uses the previous
    // block's end value, as a one-pole y ← y + (g − y)(1 − r) read before its update).
    const float r = st.r[0];
    for (uint32_t i = 0; i < n; ++i) {
      const float s = accAt1(i);
      const float y = std::fma(load1<xSpl>(x, i), gv + d, s);
      out[i] = d == 0.0f && gv == 0.0f ? (kAdd ? s : 0.0f) : y;
      d *= r;
      d = std::fabs(d) < kSettled ? 0.0f : d;
    }
    st.d[0] = d;
  }

  // The start of a 1-lane param call: prepare, retarget; returns the residual (0: settled).
  static float prologue1(GainState& st, float gv, float tau, bool startAtTarget) noexcept {  // RT-safe
    prepare(st, 0, tau);
    return retarget(st, 0, gv, startAtTarget);
  }

  // ---- 8 lanes (one voice group): per-lane one-pole per sample ----
  template <bool accSpl, bool xSpl>
  static __m256 accAt(const float* acc, uint32_t f) noexcept {
    if constexpr (!kAdd) return _mm256_setzero_ps();
    else if constexpr (accSpl) return _mm256_loadu_ps(acc);
    else return _mm256_loadu_ps(acc + 8 * f);
  }
  template <bool xSpl>
  static __m256 xAt(const float* x, uint32_t f) noexcept {
    if constexpr (xSpl) return _mm256_loadu_ps(x);
    else return _mm256_loadu_ps(x + 8 * f);
  }

  template <bool accSpl, bool xSpl>
  static void audio8(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<GainState*>(a.state);
    const uint32_t n = a.frames;
    const float* acc = kAdd ? a.in[0] : nullptr;
    const float* x = a.in[kA];
    const float* g = a.in[kA + 1];
    float* out = a.out[0];
    for (uint32_t f = 0; f < n; ++f)
      _mm256_storeu_ps(out + 8 * f,
                       _mm256_fmadd_ps(xAt<xSpl>(x, f), _mm256_loadu_ps(g + 8 * f), accAt<accSpl, xSpl>(acc, f)));
    for (uint32_t l = 0; l < 8; ++l) {
      st.target[l] = finiteOr0(g[8 * (n - 1) + l]);
      st.d[l] = 0.0f;
      st.started[l] = 1.0f;
    }
  }

  template <bool accSpl, bool xSpl>
  static void param8(const KernelArgs& a) noexcept {  // RT-safe
    auto& st = *static_cast<GainState*>(a.state);
    const uint32_t n = a.frames;
    const float* acc = kAdd ? a.in[0] : nullptr;
    const float* x = a.in[kA];
    const float* g = a.in[kA + 1];
    const float* t = a.in[kA + 2];
    float* out = a.out[0];
    bool moving = false;
    for (uint32_t l = 0; l < 8; ++l) {
      prepare(st, l, t[l]);
      moving = retarget(st, l, finiteOr0(g[l]), startAt(a, l)) != 0.0f || moving;
    }
    const __m256 gv = _mm256_load_ps(st.target);
    if (!moving) {
      for (uint32_t f = 0; f < n; ++f)
        _mm256_storeu_ps(out + 8 * f, _mm256_fmadd_ps(xAt<xSpl>(x, f), gv, accAt<accSpl, xSpl>(acc, f)));
      return;
    }
    __m256 d = _mm256_load_ps(st.d);
    const __m256 r = _mm256_load_ps(st.r);
    for (uint32_t f = 0; f < n; ++f) {
      _mm256_storeu_ps(out + 8 * f, _mm256_fmadd_ps(xAt<xSpl>(x, f), _mm256_add_ps(gv, d), accAt<accSpl, xSpl>(acc, f)));
      d = _mm256_mul_ps(d, r);
    }
    const __m256 far = _mm256_cmp_ps(_mm256_andnot_ps(_mm256_set1_ps(-0.0f), d), _mm256_set1_ps(kSettled), _CMP_GE_OQ);
    _mm256_store_ps(st.d, _mm256_and_ps(d, far));  // 0 where settled
  }
};

}  // namespace

KernelFn findGainKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept {
  switch (a) {
    case Atom::Gain: return pick<Gain<false>>(lanes, specMask);
    case Atom::GainAdd: return pick<Gain<true>>(lanes, specMask);
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail

namespace pq::dsp {

void gainPrologue(void* state, float g, float tau, float start) noexcept {  // RT-safe
  (void)detail::Gain<false>::prologue1(*static_cast<GainState*>(state), std::isfinite(g) ? g : 0.0f, tau, start >= 0.5f);
}

}  // namespace pq::dsp
