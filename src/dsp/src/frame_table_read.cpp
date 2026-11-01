// Frame-table readers (TMM.1): AVX2, 8 reads per step (8 voices of a group, or 8 consecutive samples of a mono
// signal). See frame_table.h for what is read; this file is how.
//
// Per step: the level per lane (one 8-lane gather of the packed level info by the increment's bucket), the index
// and fraction in the level (exact product), then the 6 samples i − 2 … i + 3 of every lane as 6 row vectors: per
// lane two 4-float loads (i − 2 and i + 2), paired lane l with lane l + 4 in one register (a 128-bit insert from
// memory), and a 4×4 transpose inside each 128-bit half (12 shuffles; loads + shuffles instead of 6 two-lane
// 64-bit gathers). With a blend, frame k + 1 is read the same way and the rows are blended lane-wise. Then the
// 6-point polynomial on all 8 lanes at once.
//
// The frame pair and weight depend only on the position: with a Spl position they are computed once per block, and
// a block whose lanes all rest on a frame (w = 0, always when F = 1) takes the one-frame path throughout.

#include <immintrin.h>

#include <cstdint>
#include <cstring>

#include "frame_table_ops.h"
#include "pq/dsp/frame_table.h"

namespace pq::dsp {

namespace {

using simd::S8;
using detail::ft::kInterp;

// Frame k's row offset (k·frameStride), the extra offset of frame k + 1 (frameStride where w ≠ 0, else 0: a lane
// resting on the last frame has no k + 1), the weight, and whether any lane blends.
struct FramePick {
  __m256i off;
  __m256i step;
  __m256 w;
  bool blend;
};

__forceinline FramePick pickFrames(const FrameTableView& t, __m256 u) noexcept {
  if (t.frames <= 1) return {_mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_ps(), false};
  __m256i k;
  __m256 w;
  detail::ft::framePosition<S8>(t, u, k, w);
  const __m256i stride = _mm256_set1_epi32(static_cast<int32_t>(t.frameStride));
  const __m256 moving = _mm256_cmp_ps(w, _mm256_setzero_ps(), _CMP_NEQ_UQ);
  return {_mm256_mullo_epi32(k, stride), _mm256_and_si256(_mm256_castps_si256(moving), stride), w,
          _mm256_movemask_ps(moving) != 0};
}

// Rows y_{−2} … y_3 (r[0..5]) of the 8 lanes whose sample i − 2 sits at d + ix[l].
__forceinline void rows6(const float* d, __m256i idx, __m256 (&r)[6]) noexcept {
  // The 8 indices through memory (one store, 8 scalar loads on the load ports) rather than 8 extracts, which would
  // compete with the transpose for port 5.
  int32_t ix[8];
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(ix), idx);
  __m256 a[4], b[4];
  for (int m = 0; m < 4; ++m) {
    const float* lo = d + ix[m];
    const float* hi = d + ix[m + 4];
    a[m] = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_loadu_ps(lo)), _mm_loadu_ps(hi), 1);
    b[m] = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_loadu_ps(lo + 4)), _mm_loadu_ps(hi + 4), 1);
  }
  // a[m] = [lane m: y−2 y−1 y0 y1 | lane m+4: …]; transpose 4×4 inside each half.
  const __m256 t0 = _mm256_unpacklo_ps(a[0], a[1]), t1 = _mm256_unpacklo_ps(a[2], a[3]);
  const __m256 t2 = _mm256_unpackhi_ps(a[0], a[1]), t3 = _mm256_unpackhi_ps(a[2], a[3]);
  r[0] = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(1, 0, 1, 0));
  r[1] = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(3, 2, 3, 2));
  r[2] = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(1, 0, 1, 0));
  r[3] = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(3, 2, 3, 2));
  const __m256 u0 = _mm256_unpacklo_ps(b[0], b[1]), u1 = _mm256_unpacklo_ps(b[2], b[3]);
  r[4] = _mm256_shuffle_ps(u0, u1, _MM_SHUFFLE(1, 0, 1, 0));
  r[5] = _mm256_shuffle_ps(u0, u1, _MM_SHUFFLE(3, 2, 3, 2));
}

__forceinline __m256 coef(int n, __m256 a0, __m256 a1, __m256 a2) noexcept {
  return _mm256_fmadd_ps(_mm256_set1_ps(kInterp[2][n]), a2,
                         _mm256_fmadd_ps(_mm256_set1_ps(kInterp[1][n]), a1, _mm256_mul_ps(_mm256_set1_ps(kInterp[0][n]), a0)));
}

// A step's addresses: per lane the index of sample i − 2 in frame k, the fraction, and the frame choice.
struct Addr {
  __m256i idx;
  __m256 x;
  __m256i step;  // per-sample position only (kMode 2): fp.step, fp.w, fp.blend of this step
  __m256 w;
  bool blend;
};

// Stage 1 (the long dependency chain: increment → level lookup → index). RT-safe.
__forceinline Addr address8(const FrameTableView& t, __m256 p, __m256 inc, const FramePick& fp) noexcept {
  const __m256i info = _mm256_i32gather_epi32(reinterpret_cast<const int*>(t.bucketInfo),
                                              detail::ft::levelBucket<S8>(inc), 4);
  const __m256i off = _mm256_and_si256(info, _mm256_set1_epi32((1 << 17) - 1));
  const __m256 len = _mm256_cvtepi32_ps(_mm256_srli_epi32(info, 17));
  // frac(p); ±inf / NaN give NaN, which max turns into 0 (it returns its second operand for NaN).
  const __m256 f = _mm256_max_ps(_mm256_sub_ps(p, _mm256_floor_ps(p)), _mm256_setzero_ps());
  const __m256 pos = _mm256_mul_ps(f, len);
  const __m256 err = _mm256_fmsub_ps(f, len, pos);  // f·len − pos, exact
  const __m256i fi = _mm256_cvttps_epi32(pos);      // pos ≥ 0: truncation is the floor
  const __m256 x = _mm256_add_ps(_mm256_sub_ps(pos, _mm256_cvtepi32_ps(fi)), err);
  const __m256i idx = _mm256_add_epi32(_mm256_add_epi32(off, fp.off), _mm256_sub_epi32(fi, _mm256_set1_epi32(2)));
  return {idx, x, fp.step, fp.w, fp.blend};
}

// Stage 2: the samples (blended when kBlend) and the polynomial. RT-safe.
template <bool kBlend>
__forceinline __m256 finish8(const FrameTableView& t, const Addr& a, __m256i step, __m256 w) noexcept {
  __m256 r[6];
  rows6(t.data, a.idx, r);
  if constexpr (kBlend) {
    __m256 s[6];
    rows6(t.data, _mm256_add_epi32(a.idx, step), s);
    for (int q = 0; q < 6; ++q) r[q] = _mm256_fmadd_ps(w, _mm256_sub_ps(s[q], r[q]), r[q]);
  }
  // r[q] = y_{q−2}
  const __m256 e0 = _mm256_add_ps(r[2], r[3]), o0 = _mm256_sub_ps(r[2], r[3]);
  const __m256 e1 = _mm256_add_ps(r[1], r[4]), o1 = _mm256_sub_ps(r[1], r[4]);
  const __m256 e2 = _mm256_add_ps(r[0], r[5]), o2 = _mm256_sub_ps(r[0], r[5]);
  const __m256 z = _mm256_sub_ps(a.x, _mm256_set1_ps(0.5f));
  __m256 acc = coef(5, o0, o1, o2);
  acc = _mm256_fmadd_ps(acc, z, coef(4, e0, e1, e2));
  acc = _mm256_fmadd_ps(acc, z, coef(3, o0, o1, o2));
  acc = _mm256_fmadd_ps(acc, z, coef(2, e0, e1, e2));
  acc = _mm256_fmadd_ps(acc, z, coef(1, o0, o1, o2));
  return _mm256_fmadd_ps(acc, z, coef(0, e0, e1, e2));
}

// kMode: 0 = one frame (Spl position, no lane blends), 1 = blend (Spl), 2 = per-sample position (decided per step).
template <int kMode>
__forceinline __m256 finish(const FrameTableView& t, const Addr& a, const FramePick& spl) noexcept {
  if constexpr (kMode == 2) return a.blend ? finish8<true>(t, a, a.step, a.w) : finish8<false>(t, a, a.step, a.w);
  else return finish8<kMode == 1>(t, a, spl.step, spl.w);
}

// The loops are software-pipelined: step n + 1's addresses are computed before step n's samples are read, so the
// long chain of the next step overlaps the loads and polynomial of this one. (Helpers, not lambdas: MSVC does not
// inline the lambdas here.)

// Mono: the 8 deltas of a step from its phases and their predecessors; the 8 previous deltas by rotating them one
// lane up (lane 0 takes the step before's last delta), as the wavetable atom's mono path.
template <int kMode>
__forceinline Addr addressL1(const FrameTableView& t, __m256 p, __m256 prev, const float* u, __m256& lastDelta,
                             const FramePick& spl) noexcept {
  const __m256i rot = _mm256_setr_epi32(7, 0, 1, 2, 3, 4, 5, 6);
  const __m256 dl = detail::ft::phaseDelta<S8>(p, prev);
  const __m256 dPrev = _mm256_blend_ps(_mm256_permutevar8x32_ps(dl, rot), _mm256_permutevar8x32_ps(lastDelta, rot), 1);
  lastDelta = dl;
  return address8(t, p, _mm256_min_ps(dl, dPrev), kMode == 2 ? pickFrames(t, _mm256_loadu_ps(u)) : spl);
}

// Full step k of a mono block (8 samples from k·8; k·8 < vecEnd).
template <int kMode>
__forceinline Addr stepL1(const FrameTableView& t, const float* phase, const float* position, uint32_t k,
                          float firstPrev, __m256& lastDelta, const FramePick& spl) noexcept {
  const uint32_t n = k * 8;
  const __m256 p = _mm256_loadu_ps(phase + n);
  // The predecessors: a shifted load, or for the first step the previous block's last phase in lane 0.
  const __m256 prev = n > 0 ? _mm256_loadu_ps(phase + n - 1)
                            : _mm256_blend_ps(_mm256_permutevar8x32_ps(p, _mm256_setr_epi32(0, 0, 1, 2, 3, 4, 5, 6)),
                                              _mm256_set1_ps(firstPrev), 1);
  return addressL1<kMode>(t, p, prev, position + n, lastDelta, spl);
}

template <int kMode>
void runL1(const FrameTableView& t, FrameReadState& st, const float* phase, const float* position, uint32_t frames,
           float* out, const FramePick& spl) noexcept {  // RT-safe
  __m256 lastDelta = _mm256_set1_ps(st.prevDelta[0]);  // broadcast; lane 7 is "the last delta"
  const uint32_t vecEnd = frames & ~7u;
  const uint32_t rem = frames - vecEnd;
  const uint32_t full = vecEnd / 8;
  // Padded last step (lanes past the end repeat the last sample; they are read and discarded).
  float tp[8], tprev[8], tu[8];
  if (rem > 0)
    for (uint32_t l = 0; l < 8; ++l) {
      const uint32_t s = vecEnd + (l < rem ? l : rem - 1);
      tp[l] = phase[s];
      tu[l] = kMode == 2 ? position[s] : position[0];
      tprev[l] = l == 0 ? (vecEnd > 0 ? phase[vecEnd - 1] : st.prevPhase[0]) : tp[l - 1];
    }
  __m256 stepDelta = lastDelta;
  if (full > 0) {
    Addr a = stepL1<kMode>(t, phase, position, 0, st.prevPhase[0], lastDelta, spl);
    for (uint32_t k = 0; k + 1 < full; ++k) {
      const Addr next = stepL1<kMode>(t, phase, position, k + 1, 0.0f, lastDelta, spl);
      _mm256_storeu_ps(out + k * 8, finish<kMode>(t, a, spl));
      a = next;
    }
    _mm256_storeu_ps(out + vecEnd - 8, finish<kMode>(t, a, spl));
    stepDelta = lastDelta;
  }
  if (rem > 0) {
    const Addr a = addressL1<kMode>(t, _mm256_loadu_ps(tp), _mm256_loadu_ps(tprev), tu, lastDelta, spl);
    float res[8];
    _mm256_storeu_ps(res, finish<kMode>(t, a, spl));
    std::memcpy(out + vecEnd, res, rem * sizeof(float));
    stepDelta = lastDelta;
  }
  float ds[8];
  _mm256_storeu_ps(ds, stepDelta);
  st.prevDelta[0] = ds[rem > 0 ? rem - 1 : 7];
  st.prevPhase[0] = phase[frames - 1];
}

// Voices: frame f's increment estimate and addresses.
template <int kMode>
__forceinline Addr addressL8(const FrameTableView& t, const float* phase, const float* position, uint32_t f,
                             __m256& prevPhase, __m256& prevDelta, const FramePick& spl) noexcept {
  const std::size_t o = static_cast<std::size_t>(f) * 8;
  const __m256 p = _mm256_loadu_ps(phase + o);
  const __m256 d = detail::ft::phaseDelta<S8>(p, prevPhase);
  const __m256 inc = _mm256_min_ps(d, prevDelta);
  prevDelta = d;
  prevPhase = p;
  return address8(t, p, inc, kMode == 2 ? pickFrames(t, _mm256_loadu_ps(position + o)) : spl);
}

template <int kMode>
void runL8(const FrameTableView& t, FrameReadState& st, const float* phase, const float* position, uint32_t frames,
           float* out, const FramePick& spl) noexcept {  // RT-safe
  __m256 prevPhase = _mm256_load_ps(st.prevPhase);
  __m256 prevDelta = _mm256_load_ps(st.prevDelta);
  Addr a = addressL8<kMode>(t, phase, position, 0, prevPhase, prevDelta, spl);
  for (uint32_t f = 0; f + 1 < frames; ++f) {
    const Addr next = addressL8<kMode>(t, phase, position, f + 1, prevPhase, prevDelta, spl);
    _mm256_storeu_ps(out + static_cast<std::size_t>(f) * 8, finish<kMode>(t, a, spl));
    a = next;
  }
  _mm256_storeu_ps(out + static_cast<std::size_t>(frames - 1) * 8, finish<kMode>(t, a, spl));
  _mm256_store_ps(st.prevPhase, prevPhase);
  _mm256_store_ps(st.prevDelta, prevDelta);
}

}  // namespace

void readFrameTableL1(const FrameTableView& t, FrameReadState& st, const float* phase, const float* position,
                      bool positionPerSample, uint32_t frames, float* out) noexcept {  // RT-safe
  if (frames == 0) return;
  const FramePick spl = pickFrames(t, _mm256_set1_ps(position[0]));
  if (positionPerSample) runL1<2>(t, st, phase, position, frames, out, spl);
  else if (spl.blend) runL1<1>(t, st, phase, position, frames, out, spl);
  else runL1<0>(t, st, phase, position, frames, out, spl);
}

void readFrameTableL8(const FrameTableView& t, FrameReadState& st, const float* phase, const float* position,
                      bool positionPerSample, uint32_t frames, float* out) noexcept {  // RT-safe
  if (frames == 0) return;
  const FramePick spl = pickFrames(t, _mm256_loadu_ps(position));
  if (positionPerSample) runL8<2>(t, st, phase, position, frames, out, spl);
  else if (spl.blend) runL8<1>(t, st, phase, position, frames, out, spl);
  else runL8<0>(t, st, phase, position, frames, out, spl);
}

}  // namespace pq::dsp
