// param_ramp (TX.smooth): the compiler's param smoothing. Semantics in pq/dsp/atoms.h; where the compiler puts it
// and how its output is read in pq/graph/schedule.h "Param ramps".
//
// Cost: a call where no lane ramps compares its 1 or 8 inputs with the targets and returns (the consumers keep
// reading the param-rate value); a ramping call writes frames · lanes floats (one multiply and one subtract each,
// vectorised over time for 1 lane, over the lanes for 8).
//
// Per lane the state is (target, inc, rem): the value at frame k of a block is target − inc·(rem − k − 1) while
// rem − k − 1 > 0, else target, so it reaches the target exactly and the value where a new ramp starts is the last
// value written (the same two operations). The arithmetic is spelled with intrinsics so that no compiler contracts
// it into an fma differently in the vector and the scalar paths (any split of a block into calls gives the same bits).

#include <bit>
#include <cmath>

#include "kernels_common.h"

namespace pq::dsp::detail {
namespace {

inline float mulSub(float target, float inc, float r) noexcept {  // target − inc·r, no contraction
  return _mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(target), _mm_mul_ss(_mm_set_ss(inc), _mm_set_ss(r))));
}

// Starts / retargets lane l's ramp towards v; true while it ramps.
inline bool prepareLane(ParamRampState& st, uint32_t l, float v, float frames) noexcept {
  if (st.started[l] == 0.0f) {
    st.started[l] = 1.0f;
    st.target[l] = v;
    st.inc[l] = 0.0f;
    st.rem[l] = 0.0f;
    return false;
  }
  if (std::bit_cast<uint32_t>(v) != std::bit_cast<uint32_t>(st.target[l])) {
    const float cur = st.rem[l] > 0.0f ? mulSub(st.target[l], st.inc[l], st.rem[l]) : st.target[l];
    st.target[l] = v;
    const float inc = (v - cur) / frames;
    if (std::isfinite(v) && std::isfinite(cur) && frames >= 1.0f && std::isfinite(inc) && inc != 0.0f) {
      st.inc[l] = inc;
      st.rem[l] = frames;
    } else {  // a jump: non-finite, or no distance (+0 → −0)
      st.inc[l] = 0.0f;
      st.rem[l] = 0.0f;
    }
  }
  return st.rem[l] > 0.0f;
}

template <uint32_t L>
void rampKernel(const KernelArgs& a) noexcept {  // RT-safe
  auto& st = *static_cast<ParamRampState*>(a.state);
  const float* x = a.in[0];
  float frames = a.in[1][0];
  frames = frames >= 1.0f && frames <= 16777216.0f ? std::floor(frames) : 0.0f;
  bool any = false;
  for (uint32_t l = 0; l < L; ++l) any = prepareLane(st, l, x[l], frames) || any;
  st.active = any ? 1u : 0u;
  const uint32_t n = a.frames;
  if (!any || n == 0) return;
  float* out = a.out[0];
  if constexpr (L == 1) {
    const float target = st.target[0], inc = st.inc[0], rem = st.rem[0];
    const __m256 tv = _mm256_set1_ps(target), iv = _mm256_set1_ps(inc), zero = _mm256_setzero_ps();
    const __m256 k1 = _mm256_setr_ps(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f);
    const uint32_t vecEnd = n & ~7u;
    for (uint32_t k = 0; k < vecEnd; k += 8) {
      const __m256 r = _mm256_sub_ps(_mm256_set1_ps(rem), _mm256_add_ps(_mm256_set1_ps(static_cast<float>(k)), k1));
      const __m256 v = _mm256_sub_ps(tv, _mm256_mul_ps(iv, r));
      _mm256_storeu_ps(out + k, _mm256_blendv_ps(tv, v, _mm256_cmp_ps(r, zero, _CMP_GT_OQ)));
    }
    for (uint32_t k = vecEnd; k < n; ++k) {
      const float r = rem - (static_cast<float>(k) + 1.0f);
      out[k] = r > 0.0f ? mulSub(target, inc, r) : target;
    }
    const float left = rem - static_cast<float>(n);
    st.rem[0] = left > 0.0f ? left : 0.0f;
  } else {
    const __m256 tv = _mm256_load_ps(st.target), iv = _mm256_load_ps(st.inc), rv = _mm256_load_ps(st.rem);
    const __m256 zero = _mm256_setzero_ps();
    for (uint32_t k = 0; k < n; ++k) {
      const __m256 r = _mm256_sub_ps(rv, _mm256_set1_ps(static_cast<float>(k) + 1.0f));
      const __m256 v = _mm256_sub_ps(tv, _mm256_mul_ps(iv, r));
      _mm256_storeu_ps(out + 8 * k, _mm256_blendv_ps(tv, v, _mm256_cmp_ps(r, zero, _CMP_GT_OQ)));
    }
    _mm256_store_ps(st.rem, _mm256_max_ps(_mm256_sub_ps(rv, _mm256_set1_ps(static_cast<float>(n))), zero));
  }
}

}  // namespace

KernelFn findRampKernel(Atom a, uint32_t lanes) noexcept {
  if (a != Atom::ParamRamp) return nullptr;
  if (lanes == 1) return &rampKernel<1>;
  if (lanes == kGroupLanes) return &rampKernel<8>;
  return nullptr;
}

}  // namespace pq::dsp::detail

namespace pq::dsp {

void paramRampHold(const void* state, float* out, uint32_t frames, uint32_t lanes) noexcept {  // RT-safe
  const auto& st = *static_cast<const ParamRampState*>(state);
  if (lanes == 1) {
    const __m256 tv = _mm256_set1_ps(st.target[0]);
    const uint32_t vecEnd = frames & ~7u;
    for (uint32_t k = 0; k < vecEnd; k += 8) _mm256_storeu_ps(out + k, tv);
    for (uint32_t k = vecEnd; k < frames; ++k) out[k] = st.target[0];
    return;
  }
  const __m256 tv = _mm256_load_ps(st.target);
  for (uint32_t k = 0; k < frames; ++k) _mm256_storeu_ps(out + 8 * k, tv);
}

}  // namespace pq::dsp
