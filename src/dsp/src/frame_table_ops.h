#pragma once

// Private to pq_dsp: the frame-table readers' per-lane steps, written once over the lane-width ops (simd.h) so the
// AVX2 readers (S8) and the scalar functions in frame_table.cpp (S1: frameTableLevel, the reference's increment
// estimate) compute bit-identical levels.

#include "pq/dsp/frame_table.h"
#include "simd.h"

namespace pq::dsp::detail::ft {

// The interpolator (tools/fit/frame_interp_coeffs.py): on |t| ∈ [d, d + 1) the impulse response is
// Σ_n kInterp[d][n]·z^n, z = |t| − (d + ½). Read at x ∈ [0, 1) between y0 and y1, z = x − ½:
//   e_d = y_{−d} + y_{d+1}, o_d = y_{−d} − y_{d+1},  c_n = Σ_d kInterp[d][n]·(n even ? e_d : o_d),  out = Σ c_n z^n.
inline constexpr float kInterp[3][6] = {
    {0.5476760268211365f, -0.7570295929908752f, -0.36000126600265503f, 0.6779742240905762f, 0.051712311804294586f,
     -0.08091908693313599f},
    {-0.03819325938820839f, -0.14660607278347015f, 0.4151029586791992f, -0.2548755705356598f, -0.07813157886266708f,
     0.040023323148489f},
    {-0.009482664056122303f, 0.03937431424856186f, -0.055108692497015f, 0.017304278910160065f, 0.026457421481609344f,
     -0.007806187495589256f},
};

// |p − prev| in cycles, wrapped to [0, ½]; NaN → 0 (max returns its second operand for NaN). As the wavetable atom.
template <class S>
__forceinline typename S::F phaseDelta(typename S::F p, typename S::F prev) noexcept {  // RT-safe
  auto d = S::sub(p, prev);
  d = S::abs(S::sub(d, S::round(d)));
  return S::max(d, S::zero());
}

// The increment's bucket in the level lookup (frame_table.h): (float bits >> 16) − base, clamped to the table.
// inc < 2^−24, 0 and NaN (max returns its second operand for NaN) give bucket 0, level 0.
template <class S>
__forceinline typename S::I levelBucket(typename S::F inc) noexcept {  // RT-safe
  const auto x = S::max(inc, S::zero());
  const auto key = S::subi(S::template srai<16>(S::asI(x)), S::seti(kFrameLevelBucketBase));
  return S::mini(S::maxi(key, S::seti(0)), S::seti(static_cast<int32_t>(kFrameLevelBuckets) - 1));
}

// Frame pair and weight for position u (F ≥ 2): u clamped to [0, 1] (NaN → 0), fp = u·(F − 1) split exactly
// (two-product), k = ⌊fp⌋, w = fp − k. A weight within 2^−20 of 0 or 1 snaps to the frame (w = 0, k or k + 1), which
// is below the float position's own resolution near u = 1 (ulp·(F − 1) ≈ 3.8e-6 for F = 64); then k may be F − 1,
// and only w ≠ 0 reads frame k + 1 (so k ≤ F − 2 there).
template <class S>
__forceinline void framePosition(const FrameTableView& t, typename S::F u, typename S::I& k, typename S::F& w) noexcept {
  const auto uu = S::min(S::max(u, S::zero()), S::set(1.0f));
  const auto fm1 = S::set(t.framesMinus1);
  const auto fp = S::mul(uu, fm1);
  const auto err = S::fma(uu, fm1, S::neg(fp));  // uu·fm1 − fp, exact
  auto kf = S::floor(fp);
  auto wf = S::add(S::sub(fp, kf), err);
  constexpr float kSnap = 9.5367431640625e-07f;  // 2^-20
  const auto hi = S::ge(wf, S::set(1.0f - kSnap));
  kf = S::select(hi, S::add(kf, S::set(1.0f)), kf);
  wf = S::band(S::bnot(S::bor(hi, S::le(wf, S::set(kSnap)))), wf);
  k = S::truncToInt(kf);
  w = wf;
}

}  // namespace pq::dsp::detail::ft
