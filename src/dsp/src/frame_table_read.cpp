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
//
// TMM.12-fix1: a sine table (FrameTableView::sine) whose lanes all rest on a frame for the block takes the sine path
// instead: per lane the frame's {dc, amplitude, θ} once per block, per step the `sine` atom's evaluation (no level,
// no rows). The reader state is written as the rows' path writes it.

#include <immintrin.h>

#include <cstdint>
#include <cstring>

#include "frame_table_ops.h"
#include "frame_table_simd.h"
#include "pq/core/fast_math.h"
#include "pq/dsp/frame_table.h"

namespace pq::dsp {

namespace {

using simd::S8;

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

using detail::ft::rows6;

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
  __m256i idx;
  __m256 x;
  detail::ft::splitPhase<S8>(info, p, fp.off, idx, x);
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
  return detail::ft::poly6(r, a.x);
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

// ---- TMM.12-fix1: the sine path --------------------------------------------------------------------------------

constexpr float kTwoPi = 6.28318530717958648f;  // as the sine atom (kernels_math.cpp)

struct SineCoef {
  __m256 dc, amp, th;
};

// The lanes' frame coefficients for a Spl position u (per lane); false when a lane blends two frames.
__forceinline bool sinePick(const FrameTableView& t, __m256 u, SineCoef& c) noexcept {
  if (t.frames <= 1) {
    c = {_mm256_set1_ps(t.sine[0]), _mm256_set1_ps(t.sine[1]), _mm256_set1_ps(t.sine[2])};
    return true;
  }
  __m256i k;
  __m256 w;
  detail::ft::framePosition<S8>(t, u, k, w);
  if (_mm256_movemask_ps(_mm256_cmp_ps(w, _mm256_setzero_ps(), _CMP_NEQ_UQ)) != 0) return false;
  const __m256i i = _mm256_slli_epi32(k, 2);
  c = {_mm256_i32gather_ps(t.sine, i, 4), _mm256_i32gather_ps(t.sine + 1, i, 4), _mm256_i32gather_ps(t.sine + 2, i, 4)};
  return true;
}

// dc + a·sin(2π(r + θ)), r = p − round(p) ∈ [−½, ½] (exact; ±inf / NaN → 0, phase 0 as splitPhase reads them). In a
// feedback loop this is on the loop's latency path: no separate frac. RT-safe.
__forceinline __m256 sineStep(__m256 p, const SineCoef& c) noexcept {
  __m256 r = _mm256_sub_ps(p, _mm256_round_ps(p, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
  r = _mm256_and_ps(r, _mm256_cmp_ps(r, r, _CMP_ORD_Q));
  return _mm256_fmadd_ps(c.amp, fastSin(_mm256_mul_ps(_mm256_add_ps(r, c.th), _mm256_set1_ps(kTwoPi))), c.dc);
}

void sineL1(FrameReadState& st, const float* phase, uint32_t frames, float* out,
            const SineCoef& c) noexcept {  // RT-safe
  const uint32_t vecEnd = frames & ~7u;
  for (uint32_t n = 0; n < vecEnd; n += 8) _mm256_storeu_ps(out + n, sineStep(_mm256_loadu_ps(phase + n), c));
  if (vecEnd < frames) {
    float tp[8] = {}, res[8];
    std::memcpy(tp, phase + vecEnd, (frames - vecEnd) * sizeof(float));
    _mm256_storeu_ps(res, sineStep(_mm256_loadu_ps(tp), c));
    std::memcpy(out + vecEnd, res, (frames - vecEnd) * sizeof(float));
  }
  // The state as runL1 leaves it: the last phase and its |Δphase|.
  const float before = frames > 1 ? phase[frames - 2] : st.prevPhase[0];
  st.prevDelta[0] =
      _mm256_cvtss_f32(detail::ft::phaseDelta<S8>(_mm256_set1_ps(phase[frames - 1]), _mm256_set1_ps(before)));
  st.prevPhase[0] = phase[frames - 1];
}

void sineL8(FrameReadState& st, const float* phase, uint32_t frames, float* out,
            const SineCoef& c) noexcept {  // RT-safe
  for (uint32_t f = 0; f < frames; ++f) {
    const std::size_t o = static_cast<std::size_t>(f) * 8;
    _mm256_storeu_ps(out + o, sineStep(_mm256_loadu_ps(phase + o), c));
  }
  // The state as runL8 leaves it.
  const __m256 last = _mm256_loadu_ps(phase + static_cast<std::size_t>(frames - 1) * 8);
  const __m256 before = frames > 1 ? _mm256_loadu_ps(phase + static_cast<std::size_t>(frames - 2) * 8)
                                   : _mm256_load_ps(st.prevPhase);
  _mm256_store_ps(st.prevDelta, detail::ft::phaseDelta<S8>(last, before));
  _mm256_store_ps(st.prevPhase, last);
}

}  // namespace

void readFrameTableL1(const FrameTableView& t, FrameReadState& st, const float* phase, const float* position,
                      bool positionPerSample, uint32_t frames, float* out) noexcept {  // RT-safe
  if (frames == 0) return;
  if (t.sine != nullptr && (t.frames <= 1 || !positionPerSample)) {
    SineCoef c;
    if (sinePick(t, _mm256_set1_ps(position[0]), c)) return sineL1(st, phase, frames, out, c);
  }
  const FramePick spl = pickFrames(t, _mm256_set1_ps(position[0]));
  if (positionPerSample) runL1<2>(t, st, phase, position, frames, out, spl);
  else if (spl.blend) runL1<1>(t, st, phase, position, frames, out, spl);
  else runL1<0>(t, st, phase, position, frames, out, spl);
}

void readFrameTableL8(const FrameTableView& t, FrameReadState& st, const float* phase, const float* position,
                      bool positionPerSample, uint32_t frames, float* out) noexcept {  // RT-safe
  if (frames == 0) return;
  if (t.sine != nullptr && (t.frames <= 1 || !positionPerSample)) {
    SineCoef c;
    if (sinePick(t, _mm256_loadu_ps(position), c)) return sineL8(st, phase, frames, out, c);
  }
  const FramePick spl = pickFrames(t, _mm256_loadu_ps(position));
  if (positionPerSample) runL8<2>(t, st, phase, position, frames, out, spl);
  else if (spl.blend) runL8<1>(t, st, phase, position, frames, out, spl);
  else runL8<0>(t, st, phase, position, frames, out, spl);
}

bool frameTableSineCoefs(const FrameTableView& t, const float* position, uint32_t lanes, float* dc, float* amp,
                         float* th) noexcept {  // RT-safe
  if (t.sine == nullptr || (t.frames > 1 && position == nullptr)) return false;
  const __m256 u = position == nullptr ? _mm256_setzero_ps()
                   : lanes == 8        ? _mm256_loadu_ps(position)
                                       : _mm256_set1_ps(position[0]);
  SineCoef c;
  if (!sinePick(t, u, c)) return false;
  _mm256_storeu_ps(dc, c.dc);
  _mm256_storeu_ps(amp, c.amp);
  _mm256_storeu_ps(th, c.th);
  return true;
}

}  // namespace pq::dsp
