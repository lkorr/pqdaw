// The oversample region's rate converters (TMM.6): oversample_up / _down (polyphase IIR half-band cascades),
// oversample_phase (unwrapped linear interpolation), oversample_hold / _any (bool up / down). Filters, conventions
// and the measured numbers: pq/dsp/oversample.h.
//
// One kernel per (atom, lane count); the factor is read from the Spl input once per call and picks a template
// instantiation, so the per-sample code is fully unrolled for its factor (every stage, every allpass) with the
// allpass memories in registers for the whole block. 8 lanes: one __m256 per voice group frame; 1 lane: the low
// float of an __m128 (scalar FMAs). Up and down process one base frame at a time through every stage: the stages'
// recursions are independent, so the core overlaps them (the cost per frame tends to the issue width, not to the
// recursion latencies).

#include <immintrin.h>

#include <bit>
#include <cstring>
#include <utility>

#include "kernels_common.h"
#include "pq/dsp/oversample.h"

namespace pq::dsp::detail {
namespace {

struct Lane8 {
  using V = __m256;
  static constexpr uint32_t W = 8;
  static V load(const float* p) noexcept { return _mm256_loadu_ps(p); }
  static void store(float* p, V v) noexcept { _mm256_storeu_ps(p, v); }
  static V set(float c) noexcept { return _mm256_set1_ps(c); }
  static V add(V a, V b) noexcept { return _mm256_add_ps(a, b); }
  static V sub(V a, V b) noexcept { return _mm256_sub_ps(a, b); }
  static V mul(V a, V b) noexcept { return _mm256_mul_ps(a, b); }
  static V fma(V a, V b, V c) noexcept { return _mm256_fmadd_ps(a, b, c); }
  static V fnma(V a, V b, V c) noexcept { return _mm256_fnmadd_ps(a, b, c); }  // c − a·b
  static V bor(V a, V b) noexcept { return _mm256_or_ps(a, b); }
  static V roundNearest(V a) noexcept { return _mm256_round_ps(a, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
  static V floor(V a) noexcept { return _mm256_floor_ps(a); }
  static V min(V a, V b) noexcept { return _mm256_min_ps(a, b); }
  static V blend(V a, V b, V mask) noexcept { return _mm256_blendv_ps(a, b, mask); }  // mask ? b : a
};

struct Lane1 {
  using V = __m128;
  static constexpr uint32_t W = 1;
  static V load(const float* p) noexcept { return _mm_load_ss(p); }
  static void store(float* p, V v) noexcept { _mm_store_ss(p, v); }
  static V set(float c) noexcept { return _mm_set_ss(c); }
  static V add(V a, V b) noexcept { return _mm_add_ss(a, b); }
  static V sub(V a, V b) noexcept { return _mm_sub_ss(a, b); }
  static V mul(V a, V b) noexcept { return _mm_mul_ss(a, b); }
  static V fma(V a, V b, V c) noexcept { return _mm_fmadd_ss(a, b, c); }
  static V fnma(V a, V b, V c) noexcept { return _mm_fnmadd_ss(a, b, c); }
  static V bor(V a, V b) noexcept { return _mm_or_ps(a, b); }
  static V roundNearest(V a) noexcept { return _mm_round_ss(a, a, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
  static V floor(V a) noexcept { return _mm_floor_ss(a, a); }
  static V min(V a, V b) noexcept { return _mm_min_ss(a, b); }
  static V blend(V a, V b, V mask) noexcept { return _mm_blendv_ps(a, b, mask); }
};

template <class F, size_t... I>
inline void unrollImpl(F&& f, std::index_sequence<I...>) noexcept {
  (f(std::integral_constant<uint32_t, static_cast<uint32_t>(I)>{}), ...);
}
template <uint32_t N, class F>
inline void unroll(F&& f) noexcept {
  unrollImpl(f, std::make_index_sequence<N>{});
}

constexpr uint32_t factorStages(uint32_t n) noexcept { return os::stagesOf(n); }

// The allpass memories of one call, in registers (loaded at the start of the block, stored at its end).
template <class L, uint32_t S>
struct Chains {
  using V = typename L::V;
  static constexpr uint32_t kSlots = S == 0 ? 1 : os::kStageFirst[S - 1] + os::kStageSize[S - 1];
  V y1[kSlots];
  V xin[2 * (S == 0 ? 1 : S)];

  void load(const OversampleState& st, uint32_t lane0) noexcept {
    for (uint32_t k = 0; k < kSlots; ++k) y1[k] = L::load(st.y1[k] + lane0);
    for (uint32_t k = 0; k < 2 * S; ++k) xin[k] = L::load(st.xin[k] + lane0);
  }
  void store(OversampleState& st, uint32_t lane0) const noexcept {
    for (uint32_t k = 0; k < kSlots; ++k) L::store(st.y1[k] + lane0, y1[k]);
    for (uint32_t k = 0; k < 2 * S; ++k) L::store(st.xin[k] + lane0, xin[k]);
  }

  // Path P (0: A0 = even coefficients, 1: A1 = odd) of stage Sg (0-based) on one low-rate sample.
  template <uint32_t Sg, uint32_t P>
  V path(V x) noexcept {
    constexpr uint32_t first = os::kStageFirst[Sg], size = os::kStageSize[Sg];
    constexpr uint32_t count = (size + 1 - P) / 2;
    V prevIn = xin[2 * Sg + P];
    xin[2 * Sg + P] = x;
    unroll<count>([&](auto k) {
      constexpr uint32_t idx = first + P + 2 * decltype(k)::value;
      const V c = L::set(static_cast<float>(os::kCoefs[idx]));
      const V y = L::fnma(c, y1[idx], L::fma(c, x, prevIn));  // c·x + x[n−1] − c·y[n−1]
      prevIn = y1[idx];
      y1[idx] = y;
      x = y;
    });
    return x;
  }
};

template <class L>
inline float factorOf(const KernelArgs& a) noexcept { return a.in[1][0]; }

// ---- up ------------------------------------------------------------------------------------------------------------
template <class L, uint32_t N>
void upRun(const KernelArgs& a, uint32_t base) noexcept {  // RT-safe
  using V = typename L::V;
  constexpr uint32_t S = factorStages(N);
  constexpr uint32_t W = L::W;
  auto& st = *static_cast<OversampleState*>(a.state);
  Chains<L, S> ch;
  ch.load(st, 0);
  const float* x = a.in[0];
  float* out = a.out[0];
  for (uint32_t k = 0; k < base; ++k) {
    V buf[N];
    buf[0] = L::load(x + static_cast<size_t>(k) * W);
    unroll<S>([&](auto s) {
      constexpr uint32_t sg = decltype(s)::value;
      constexpr uint32_t count = 1u << sg;
      V next[2 * count];
      unroll<count>([&](auto j) {
        constexpr uint32_t jj = decltype(j)::value;
        next[2 * jj] = ch.template path<sg, 0>(buf[jj]);
        next[2 * jj + 1] = ch.template path<sg, 1>(buf[jj]);
      });
      unroll<2 * count>([&](auto j) { buf[decltype(j)::value] = next[decltype(j)::value]; });
    });
    float* o = out + static_cast<size_t>(k) * N * W;
    unroll<N>([&](auto j) { L::store(o + decltype(j)::value * W, buf[decltype(j)::value]); });
  }
  ch.store(st, 0);
}

// ---- down ----------------------------------------------------------------------------------------------------------
template <class L, uint32_t N>
void downRun(const KernelArgs& a, uint32_t base) noexcept {  // RT-safe
  using V = typename L::V;
  constexpr uint32_t S = factorStages(N);
  constexpr uint32_t W = L::W;
  auto& st = *static_cast<OversampleState*>(a.state);
  Chains<L, S> ch;
  ch.load(st, 0);
  const float* x = a.in[0];
  float* out = a.out[0];
  const V half = L::set(0.5f);
  for (uint32_t k = 0; k < base; ++k) {
    V buf[N];
    const float* in = x + static_cast<size_t>(k) * N * W;
    unroll<N>([&](auto j) { buf[decltype(j)::value] = L::load(in + decltype(j)::value * W); });
    // Stages from the highest rate down: stage sg halves 2^(sg+1) samples into 2^sg.
    unroll<S>([&](auto r) {
      constexpr uint32_t sg = S - 1 - decltype(r)::value;
      constexpr uint32_t count = 1u << sg;
      unroll<count>([&](auto j) {
        constexpr uint32_t jj = decltype(j)::value;
        const V e = buf[2 * jj], o = buf[2 * jj + 1];
        const V pa = ch.template path<sg, 0>(o);
        const V pb = ch.template path<sg, 1>(e);
        buf[jj] = L::mul(L::add(pa, pb), half);
      });
    });
    L::store(out + static_cast<size_t>(k) * W, buf[0]);
  }
  ch.store(st, 0);
}

template <class L, bool kUp>
void halfBandKernel(const KernelArgs& a) noexcept {  // RT-safe
  const uint32_t n = os::sanitizeFactor(factorOf<L>(a));
  const uint32_t base = a.frames / n;
  constexpr uint32_t W = L::W;
  if (n == 1) {
    if (a.frames) std::memcpy(a.out[0], a.in[0], sizeof(float) * a.frames * W);
    return;
  }
  switch (n) {
    case 2: kUp ? upRun<L, 2>(a, base) : downRun<L, 2>(a, base); break;
    case 4: kUp ? upRun<L, 4>(a, base) : downRun<L, 4>(a, base); break;
    case 8: kUp ? upRun<L, 8>(a, base) : downRun<L, 8>(a, base); break;
    default: kUp ? upRun<L, 16>(a, base) : downRun<L, 16>(a, base); break;
  }
  if constexpr (kUp) {
    // A frame count that is not a multiple of n (never from the scheduler): the rest is silence.
    const uint32_t done = base * n;
    if (done < a.frames) std::memset(a.out[0] + static_cast<size_t>(done) * W, 0, sizeof(float) * (a.frames - done) * W);
  }
}

// ---- phase ---------------------------------------------------------------------------------------------------------
template <class L, uint32_t N>
void phaseRun(const KernelArgs& a, uint32_t base) noexcept {  // RT-safe
  using V = typename L::V;
  constexpr uint32_t W = L::W;
  auto& st = *static_cast<OversamplePhaseState*>(a.state);
  V prev = L::load(st.prev), primed = L::load(st.primed);
  const float* x = a.in[0];
  float* out = a.out[0];
  const V belowOne = L::set(0.99999994f);
  const V allOnes = L::set(std::bit_cast<float>(0xFFFFFFFFu));
  for (uint32_t k = 0; k < base; ++k) {
    const V v = L::load(x + static_cast<size_t>(k) * W);
    const V p = L::blend(v, prev, primed);
    V d = L::sub(v, p);
    d = L::sub(d, L::roundNearest(d));  // the shortest way round: [−½, ½]
    const V fl = L::floor(v);
    float* o = out + static_cast<size_t>(k) * N * W;
    unroll<N>([&](auto j) {
      constexpr float t = static_cast<float>(N - 1 - decltype(j)::value) / static_cast<float>(N);
      const V u = L::fnma(d, L::set(t), v);  // v − d·t
      const V fr = L::min(L::sub(u, L::floor(u)), belowOne);
      L::store(o + decltype(j)::value * W, L::add(fl, fr));
    });
    prev = v;
    primed = allOnes;
  }
  if (base) {
    L::store(st.prev, prev);
    L::store(st.primed, L::set(std::bit_cast<float>(0xFFFFFFFFu)));
  }
}

template <class L>
void phaseKernel(const KernelArgs& a) noexcept {  // RT-safe
  const uint32_t n = os::sanitizeFactor(factorOf<L>(a));
  const uint32_t base = a.frames / n;
  constexpr uint32_t W = L::W;
  switch (n) {
    case 1: phaseRun<L, 1>(a, base); break;
    case 2: phaseRun<L, 2>(a, base); break;
    case 4: phaseRun<L, 4>(a, base); break;
    case 8: phaseRun<L, 8>(a, base); break;
    default: phaseRun<L, 16>(a, base); break;
  }
  const uint32_t done = base * n;
  if (done < a.frames) std::memset(a.out[0] + static_cast<size_t>(done) * W, 0, sizeof(float) * (a.frames - done) * W);
}

// ---- bool: hold / any ----------------------------------------------------------------------------------------------
template <uint32_t W>
void holdKernel(const KernelArgs& a) noexcept {  // RT-safe
  const uint32_t n = os::sanitizeFactor(a.in[1][0]);
  const uint32_t base = a.frames / n;
  const float* x = a.in[0];
  float* out = a.out[0];
  for (uint32_t k = 0; k < base; ++k)
    for (uint32_t j = 0; j < n; ++j) std::memcpy(out + (static_cast<size_t>(k) * n + j) * W, x + static_cast<size_t>(k) * W, W * sizeof(float));
  const uint32_t done = base * n;
  if (done < a.frames) std::memset(out + static_cast<size_t>(done) * W, 0, sizeof(float) * (a.frames - done) * W);
}

template <uint32_t W>
void anyKernel(const KernelArgs& a) noexcept {  // RT-safe
  const uint32_t n = os::sanitizeFactor(a.in[1][0]);
  const uint32_t base = a.frames / n;
  const uint32_t* x = reinterpret_cast<const uint32_t*>(a.in[0]);
  uint32_t* out = reinterpret_cast<uint32_t*>(a.out[0]);
  for (uint32_t k = 0; k < base; ++k)
    for (uint32_t l = 0; l < W; ++l) {
      uint32_t m = 0;
      for (uint32_t j = 0; j < n; ++j) m |= x[(static_cast<size_t>(k) * n + j) * W + l];
      out[static_cast<size_t>(k) * W + l] = m != 0 ? 0xFFFFFFFFu : 0u;  // canonical masks
    }
}

}  // namespace

KernelFn findOversampleKernel(Atom a, uint32_t lanes) noexcept {
  const bool eight = lanes == 8;
  if (lanes != 1 && lanes != 8) return nullptr;
  switch (a) {
    case Atom::OversampleUp: return eight ? &halfBandKernel<Lane8, true> : &halfBandKernel<Lane1, true>;
    case Atom::OversampleDown: return eight ? &halfBandKernel<Lane8, false> : &halfBandKernel<Lane1, false>;
    case Atom::OversamplePhase: return eight ? &phaseKernel<Lane8> : &phaseKernel<Lane1>;
    case Atom::OversampleHold: return eight ? &holdKernel<8> : &holdKernel<1>;
    case Atom::OversampleAny: return eight ? &anyKernel<8> : &anyKernel<1>;
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail
