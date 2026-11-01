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

// Everything per sample is unrolled at compile time through __forceinline templates over integer sequences (a lambda
// in a fold expression is not reliably inlined by MSVC: called per allpass, the memories went through the stack and
// the cascade ran 2-3x slower). The memories are locals of the run function, so they live in registers.
#define PQ_OS_INLINE __forceinline

template <uint32_t... I>
using Seq = std::integer_sequence<uint32_t, I...>;

constexpr uint32_t factorStages(uint32_t n) noexcept { return os::stagesOf(n); }

// The allpass memories of one call (loaded at the start of the block, stored at its end).
template <class L, uint32_t S>
struct Chains {
  using V = typename L::V;
  static constexpr uint32_t kSlots = S == 0 ? 1 : os::kStageFirst[S - 1] + os::kStageSize[S - 1];
  V y1[kSlots];
  V xin[2 * (S == 0 ? 1 : S)];
};

template <class L, uint32_t S>
PQ_OS_INLINE void loadChains(Chains<L, S>& c, const OversampleState& st) noexcept {
  for (uint32_t k = 0; k < Chains<L, S>::kSlots; ++k) c.y1[k] = L::load(st.y1[k]);
  for (uint32_t k = 0; k < 2 * S; ++k) c.xin[k] = L::load(st.xin[k]);
}
template <class L, uint32_t S>
PQ_OS_INLINE void storeChains(const Chains<L, S>& c, OversampleState& st) noexcept {
  for (uint32_t k = 0; k < Chains<L, S>::kSlots; ++k) L::store(st.y1[k], c.y1[k]);
  for (uint32_t k = 0; k < 2 * S; ++k) L::store(st.xin[k], c.xin[k]);
}

// One allpass (c + z⁻¹)/(1 + c·z⁻¹): c·x + x[n−1] − c·y[n−1]; prevIn carries x[n−1] down the chain (an allpass's
// previous input is the previous allpass's previous output).
template <class L, uint32_t I, uint32_t S>
PQ_OS_INLINE typename L::V allpass(Chains<L, S>& c, typename L::V x, typename L::V& prevIn) noexcept {
  const typename L::V cf = L::set(static_cast<float>(os::kCoefs[I]));
  const typename L::V y = L::fnma(cf, c.y1[I], L::fma(cf, x, prevIn));
  prevIn = c.y1[I];
  c.y1[I] = y;
  return y;
}

// Path P (0: A0 = even coefficients, 1: A1 = odd) of stage Sg (0-based) on one low-rate sample.
template <class L, uint32_t Sg, uint32_t P, uint32_t S, uint32_t... K>
PQ_OS_INLINE typename L::V pathK(Chains<L, S>& c, typename L::V x, Seq<K...>) noexcept {
  typename L::V prevIn = c.xin[2 * Sg + P];
  c.xin[2 * Sg + P] = x;
  ((x = allpass<L, os::kStageFirst[Sg] + P + 2 * K>(c, x, prevIn)), ...);
  return x;
}
template <class L, uint32_t Sg, uint32_t P, uint32_t S>
PQ_OS_INLINE typename L::V path(Chains<L, S>& c, typename L::V x) noexcept {
  return pathK<L, Sg, P>(c, x, std::make_integer_sequence<uint32_t, (os::kStageSize[Sg] + 1 - P) / 2>{});
}

// ---- up ------------------------------------------------------------------------------------------------------------
// Stage Sg turns buf[0 … 2^Sg) into buf[0 … 2^(Sg+1)), in time order.
template <class L, uint32_t Sg, uint32_t S, uint32_t... J>
PQ_OS_INLINE void upStage(Chains<L, S>& c, typename L::V* buf, Seq<J...>) noexcept {
  typename L::V next[2 * sizeof...(J)];
  ((next[2 * J] = path<L, Sg, 0>(c, buf[J]), next[2 * J + 1] = path<L, Sg, 1>(c, buf[J])), ...);
  ((buf[2 * J] = next[2 * J], buf[2 * J + 1] = next[2 * J + 1]), ...);
}
template <class L, uint32_t S, uint32_t... Sg>
PQ_OS_INLINE void upAll(Chains<L, S>& c, typename L::V* buf, Seq<Sg...>) noexcept {
  (upStage<L, Sg>(c, buf, std::make_integer_sequence<uint32_t, (1u << Sg)>{}), ...);
}
template <class L, uint32_t... J>
PQ_OS_INLINE void storeAll(float* o, const typename L::V* buf, Seq<J...>) noexcept {
  (L::store(o + J * L::W, buf[J]), ...);
}

template <class L, uint32_t N>
void upRun(const KernelArgs& a, uint32_t base) noexcept {  // RT-safe
  using V = typename L::V;
  constexpr uint32_t S = factorStages(N);
  constexpr uint32_t W = L::W;
  auto& st = *static_cast<OversampleState*>(a.state);
  Chains<L, S> ch;
  loadChains(ch, st);
  const float* x = a.in[0];
  float* out = a.out[0];
  for (uint32_t k = 0; k < base; ++k) {
    V buf[N];
    buf[0] = L::load(x + static_cast<size_t>(k) * W);
    upAll<L>(ch, buf, std::make_integer_sequence<uint32_t, S>{});
    storeAll<L>(out + static_cast<size_t>(k) * N * W, buf, std::make_integer_sequence<uint32_t, N>{});
  }
  storeChains(ch, st);
}

// ---- down ----------------------------------------------------------------------------------------------------------
// Stage Sg halves buf[0 … 2^(Sg+1)) into buf[0 … 2^Sg): (e, o) → ½·(A0(o) + A1(e)).
template <class L, uint32_t Sg, uint32_t S, uint32_t... J>
PQ_OS_INLINE void downStage(Chains<L, S>& c, typename L::V* buf, typename L::V half, Seq<J...>) noexcept {
  ((buf[J] = L::mul(L::add(path<L, Sg, 0>(c, buf[2 * J + 1]), path<L, Sg, 1>(c, buf[2 * J])), half)), ...);
}
template <class L, uint32_t S, uint32_t... R>
PQ_OS_INLINE void downAll(Chains<L, S>& c, typename L::V* buf, typename L::V half, Seq<R...>) noexcept {
  // From the highest rate down (R = 0 is stage S − 1).
  (downStage<L, S - 1 - R>(c, buf, half, std::make_integer_sequence<uint32_t, (1u << (S - 1 - R))>{}), ...);
}
template <class L, uint32_t... J>
PQ_OS_INLINE void loadAll(typename L::V* buf, const float* in, Seq<J...>) noexcept {
  ((buf[J] = L::load(in + J * L::W)), ...);
}

template <class L, uint32_t N>
void downRun(const KernelArgs& a, uint32_t base) noexcept {  // RT-safe
  using V = typename L::V;
  constexpr uint32_t S = factorStages(N);
  constexpr uint32_t W = L::W;
  auto& st = *static_cast<OversampleState*>(a.state);
  Chains<L, S> ch;
  loadChains(ch, st);
  const float* x = a.in[0];
  float* out = a.out[0];
  const V half = L::set(0.5f);
  for (uint32_t k = 0; k < base; ++k) {
    V buf[N];
    loadAll<L>(buf, x + static_cast<size_t>(k) * N * W, std::make_integer_sequence<uint32_t, N>{});
    downAll<L>(ch, buf, half, std::make_integer_sequence<uint32_t, S>{});
    L::store(out + static_cast<size_t>(k) * W, buf[0]);
  }
  storeChains(ch, st);
}

template <class L, bool kUp>
void halfBandKernel(const KernelArgs& a) noexcept {  // RT-safe
  const uint32_t n = os::sanitizeFactor(a.in[1][0]);
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
// Sub-sample j of a frame: floor(x) + frac(x − d·(N − 1 − j)/N).
template <class L, uint32_t N, uint32_t... J>
PQ_OS_INLINE void phaseSubs(float* o, typename L::V v, typename L::V d, typename L::V fl, typename L::V belowOne,
                            Seq<J...>) noexcept {
  ((L::store(o + J * L::W,
             L::add(fl, L::min(L::sub(L::fnma(d, L::set(static_cast<float>(N - 1 - J) / static_cast<float>(N)), v),
                                      L::floor(L::fnma(d, L::set(static_cast<float>(N - 1 - J) / static_cast<float>(N)), v))),
                               belowOne)))),
   ...);
}
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
    phaseSubs<L, N>(o, v, d, fl, belowOne, std::make_integer_sequence<uint32_t, N>{});
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
  const uint32_t n = os::sanitizeFactor(a.in[1][0]);
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
