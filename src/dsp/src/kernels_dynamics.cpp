// Dynamics atoms (DY.1, docs/plans/DY.md): follow (attack / release one-pole envelope) and window (sliding max / min /
// mean); DY.5: truepeak (the BS.1770-4 4× true-peak estimate). Semantics: pq/dsp/atoms.h "Dynamics"; state layouts:
// atom_state.h FollowState / WindowState / TruePeakState.
//
// CPU: follow's recursion is the only per-sample dependency: y feeds two independent fmas and a compare, then a blend
// (≈ 5 cycles of latency per frame; x·(1 − r) and the input clean-up are off the chain). Its coefficients are one
// vector exp per call when attack / release are param rate. window max / min (1 lane) runs 8 frames per step: an
// in-register prefix extremum (3 permute + max), one carry; the segment's suffix pass is the same, backwards, once per
// L frames. 8 lanes: one vector per frame (the voices), the suffix pass one vector op per row. mean: one double add per
// frame (the running sum), and a 4-way double sum over the ring per L frames.
// truepeak: one vector per 8 frames (1 lane) or per frame (8 lanes), all four phases in one pass over the 12 taps:
// the mirrored phase pairs share s_j / d_j (12 add / sub), then 24 fma, 4 abs, 2 adds, 2 max: 38 arithmetic ops for
// 8 samples where four direct 12-tap phases take 48 fma. Taps read the input in place from element 16 (1 lane) / 88
// (8 lanes) on; before that a stack copy of history + head.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "kernels_common.h"

namespace pq::dsp::detail {
namespace {

// Non-finite → 0 (both atoms' input rule: a follower or a window never holds a non-finite value). AU.dsp (RB-DY M4):
// ±inf reads as 0 like NaN, no longer ±FLT_MAX: a single inf sample in a dB detector held FLT_MAX dB of reduction
// (~100 s of release); now it is ignored. |x| < inf is false for NaN and ±inf: abs, compare, and (3 ops, was 4).
template <class S>
inline typename S::F clean(typename S::F x) noexcept {  // RT-safe
  return S::band(x, S::lt(S::abs(x), S::set(std::numeric_limits<float>::infinity())));
}

// ---- follow ----------------------------------------------------------------------------------------------------------

// The retention per frame of a time constant of t samples: exp(−1 / max(t, 1e−6)); t ≤ 0 or NaN → 0 (instant).
template <class S>
inline typename S::F retention(typename S::F t) noexcept {  // RT-safe
  return S::exactExp(S::div(S::set(-1.0f), S::max(t, S::set(1e-6f))));
}

template <class S>
inline typename S::F followStep(typename S::F y, typename S::F x, typename S::F ra, typename S::F xa, typename S::F rr,
                                typename S::F xr) noexcept {  // RT-safe
  // xa = x·(1 − ra), xr = x·(1 − rr): independent of y, so the chain is fma → blend (the compare runs beside).
  return S::select(S::gt(x, y), S::fma(y, ra, xa), S::fma(y, rr, xr));
}

struct Follow {
  static constexpr uint32_t kSpec = 3;  // x, attack, release
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    using F = typename S::F;
    constexpr uint32_t L = S::kW;
    auto& st = *static_cast<FollowState*>(a.state);
    const float* x = a.in[0];
    const float* at = a.in[1];
    const float* rt = a.in[2];
    float* out = a.out[0];
    const uint32_t frames = a.frames;
    const F one = S::set(1.0f);
    F y = S::load(st.y);
    auto xAt = [&](uint32_t f) noexcept { return clean<S>(S::load(splBit(SM, 0) ? x : x + static_cast<size_t>(f) * L)); };
    S::store(st.y0, y);  // AU.dsp (H4): where this call starts (dsp::convergeStateStill)
    if constexpr (splBit(SM, 1) && splBit(SM, 2)) {
      const F ra = retention<S>(S::load(at)), rr = retention<S>(S::load(rt));
      const F ia = S::sub(one, ra), ir = S::sub(one, rr);
      if constexpr (splBit(SM, 0)) {
        const F xv = xAt(0), xa = S::mul(xv, ia), xr = S::mul(xv, ir);
        for (uint32_t f = 0; f < frames; ++f) {
          y = followStep<S>(y, xv, ra, xa, rr, xr);
          S::store(out + static_cast<size_t>(f) * L, y);
        }
      } else {
        for (uint32_t f = 0; f < frames; ++f) {
          const F xv = xAt(f);
          y = followStep<S>(y, xv, ra, S::mul(xv, ia), rr, S::mul(xv, ir));
          S::store(out + static_cast<size_t>(f) * L, y);
        }
      }
    } else if constexpr (L == 1) {
      // A Blk time: the coefficients 8 frames at a time (S8's exp, bit-identical to S1's per lane: Q94), then the
      // recursion over them. A Spl one is a pattern of 8 equal values.
      constexpr uint32_t kChunk = 64;
      alignas(32) float ra[kChunk], rr[kChunk];
      for (uint32_t f0 = 0; f0 < frames; f0 += kChunk) {
        const uint32_t n = frames - f0 < kChunk ? frames - f0 : kChunk;
        uint32_t k = 0;
        for (; k + 8 <= n; k += 8) {
          using V = simd::S8;
          _mm256_store_ps(ra + k, retention<V>(splBit(SM, 1) ? V::load(at) : V::load(at + f0 + k)));
          _mm256_store_ps(rr + k, retention<V>(splBit(SM, 2) ? V::load(rt) : V::load(rt + f0 + k)));
        }
        for (; k < n; ++k) {
          S::store(ra + k, retention<S>(S::load(splBit(SM, 1) ? at : at + f0 + k)));
          S::store(rr + k, retention<S>(S::load(splBit(SM, 2) ? rt : rt + f0 + k)));
        }
        for (k = 0; k < n; ++k) {
          const F xv = xAt(f0 + k);
          const F a1 = S::load(ra + k), r1 = S::load(rr + k);
          y = followStep<S>(y, xv, a1, S::mul(xv, S::sub(one, a1)), r1, S::mul(xv, S::sub(one, r1)));
          S::store(out + f0 + k, y);
        }
      }
    } else {
      for (uint32_t f = 0; f < frames; ++f) {
        const F ra = retention<S>(S::load(splBit(SM, 1) ? at : at + static_cast<size_t>(f) * L));
        const F rr = retention<S>(S::load(splBit(SM, 2) ? rt : rt + static_cast<size_t>(f) * L));
        const F xv = xAt(f);
        y = followStep<S>(y, xv, ra, S::mul(xv, S::sub(one, ra)), rr, S::mul(xv, S::sub(one, rr)));
        S::store(out + static_cast<size_t>(f) * L, y);
      }
    }
    S::store(st.y, y);
  }
};

// ---- window ----------------------------------------------------------------------------------------------------------

enum WinOp : uint32_t { kMax = 0, kMin = 1, kMean = 2 };

// The extremum with the x86 NaN rule: a NaN FIRST operand returns the second (maxps / minps), so an empty prefix or
// the empty suffix (NaN) is the identity when it comes first.
template <uint32_t Op>
inline __m256 ext8(__m256 a, __m256 b) noexcept {
  if constexpr (Op == kMax) return _mm256_max_ps(a, b);
  else return _mm256_min_ps(a, b);
}
template <uint32_t Op>
inline __m128 ext1(__m128 a, __m128 b) noexcept {
  if constexpr (Op == kMax) return _mm_max_ss(a, b);
  else return _mm_min_ss(a, b);
}
template <uint32_t Op>
inline float extf(float a, float b) noexcept {
  return _mm_cvtss_f32(ext1<Op>(_mm_set_ss(a), _mm_set_ss(b)));
}

// Inclusive prefix (lane i: ext of lanes 0 … i) and suffix (lane i: ext of lanes i … 7) extrema of 8 frames. Each step
// combines a lane with one 1, 2, 4 lanes away; a lane with none takes itself (ext is idempotent).
template <uint32_t Op>
inline __m256 prefix8(__m256 v) noexcept {
  v = ext8<Op>(v, _mm256_permutevar8x32_ps(v, _mm256_setr_epi32(0, 0, 1, 2, 3, 4, 5, 6)));
  v = ext8<Op>(v, _mm256_permutevar8x32_ps(v, _mm256_setr_epi32(0, 1, 0, 1, 2, 3, 4, 5)));
  return ext8<Op>(v, _mm256_permutevar8x32_ps(v, _mm256_setr_epi32(0, 1, 2, 3, 0, 1, 2, 3)));
}
template <uint32_t Op>
inline __m256 suffix8(__m256 v) noexcept {
  v = ext8<Op>(v, _mm256_permutevar8x32_ps(v, _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 7)));
  v = ext8<Op>(v, _mm256_permutevar8x32_ps(v, _mm256_setr_epi32(2, 3, 4, 5, 6, 7, 6, 7)));
  return ext8<Op>(v, _mm256_permutevar8x32_ps(v, _mm256_setr_epi32(4, 5, 6, 7, 4, 5, 6, 7)));
}

inline double sumOf(const WindowState& st, uint32_t lane) noexcept {
  const uint64_t bits = (static_cast<uint64_t>(st.sumHi[lane]) << 32) | st.sumLo[lane];
  return std::bit_cast<double>(bits);
}
inline void setSum(WindowState& st, uint32_t lane, double v) noexcept {
  const auto bits = std::bit_cast<uint64_t>(v);
  st.sumLo[lane] = static_cast<uint32_t>(bits);
  st.sumHi[lane] = static_cast<uint32_t>(bits >> 32);
}

// The ring's sum in a fixed order for both lane counts: partial sums of the indices ≡ k (mod 4), then (p0 + p1) +
// (p2 + p3). 1 lane: v[i]; 8 lanes: v[i·8 + lane] for all 8 lanes at once (two __m256d per partial).
inline double ringSum1(const float* v, uint32_t len) noexcept {
  __m256d acc = _mm256_setzero_pd();
  uint32_t i = 0;
  for (; i + 4 <= len; i += 4) acc = _mm256_add_pd(acc, _mm256_cvtps_pd(_mm_loadu_ps(v + i)));
  alignas(32) double p[4];
  _mm256_store_pd(p, acc);
  for (; i < len; ++i) p[i & 3] += static_cast<double>(v[i]);
  return (p[0] + p[1]) + (p[2] + p[3]);
}
inline void ringSum8(const float* v, uint32_t len, __m256d& lo, __m256d& hi) noexcept {
  __m256d a[4][2];
  for (auto& k : a) k[0] = k[1] = _mm256_setzero_pd();
  for (uint32_t i = 0; i < len; ++i) {
    const __m256 r = _mm256_load_ps(v + static_cast<size_t>(i) * 8);
    a[i & 3][0] = _mm256_add_pd(a[i & 3][0], _mm256_cvtps_pd(_mm256_castps256_ps128(r)));
    a[i & 3][1] = _mm256_add_pd(a[i & 3][1], _mm256_cvtps_pd(_mm256_extractf128_ps(r, 1)));
  }
  lo = _mm256_add_pd(_mm256_add_pd(a[0][0], a[1][0]), _mm256_add_pd(a[2][0], a[3][0]));
  hi = _mm256_add_pd(_mm256_add_pd(a[0][1], a[1][1]), _mm256_add_pd(a[2][1], a[3][1]));
}

// ---- 1 lane -----------------------------------------------------------------------------------------------------------

template <uint32_t Op, bool kSplX>
void extremum1(WindowState& st, const float* x, float* out, uint32_t frames) noexcept {  // RT-safe
  const uint32_t len = st.length[0];
  float* v = st.values();
  uint32_t pos = st.pos[0] < len ? st.pos[0] : 0u;
  __m128 p1 = _mm_set_ss(st.prefix[0]);
  const __m128 xs = kSplX ? clean<simd::S1>(_mm_load_ss(x)) : _mm_setzero_ps();
  const __m256 x8 = _mm256_set1_ps(_mm_cvtss_f32(xs));
  uint32_t f = 0;
  while (f < frames) {
    const uint32_t run = frames - f < len - pos ? frames - f : len - pos;
    uint32_t k = 0;
    if (run >= 8) {
      __m256 carry = _mm256_set1_ps(_mm_cvtss_f32(p1));
      for (; k + 8 <= run; k += 8) {
        const __m256 xv = kSplX ? x8 : clean<simd::S8>(_mm256_loadu_ps(x + f + k));
        const __m256 pre = ext8<Op>(carry, prefix8<Op>(xv));  // carry NaN (empty): the group's own prefix
        const __m256 suf = _mm256_loadu_ps(v + pos + k + 1);   // read before this group's inputs overwrite v
        _mm256_storeu_ps(v + pos + k, xv);
        _mm256_storeu_ps(out + f + k, ext8<Op>(suf, pre));     // v[L] (NaN): the prefix alone
        carry = _mm256_permutevar8x32_ps(pre, _mm256_set1_epi32(7));
      }
      p1 = _mm256_castps256_ps128(carry);
    }
    for (; k < run; ++k) {
      const __m128 xv = kSplX ? xs : clean<simd::S1>(_mm_load_ss(x + f + k));
      p1 = ext1<Op>(p1, xv);
      const __m128 y = ext1<Op>(_mm_load_ss(v + pos + k + 1), p1);
      _mm_store_ss(v + pos + k, xv);
      _mm_store_ss(out + f + k, y);
    }
    pos += run;
    f += run;
    if (pos == len) {  // the segment is complete: its suffix extrema become the next one's look-back
      uint32_t j = len - 1;  // v[len − 1] is its own suffix
      __m256 c = _mm256_set1_ps(v[j]);
      while (j >= 8) {
        const __m256 s = ext8<Op>(suffix8<Op>(_mm256_loadu_ps(v + j - 8)), c);
        _mm256_storeu_ps(v + j - 8, s);
        c = _mm256_permutevar8x32_ps(s, _mm256_setzero_si256());
        j -= 8;
      }
      float cs = _mm256_cvtss_f32(c);
      while (j > 0) {
        --j;
        cs = extf<Op>(v[j], cs);
        v[j] = cs;
      }
      pos = 0;
      p1 = _mm_set_ss(std::numeric_limits<float>::quiet_NaN());
    }
  }
  st.pos[0] = pos;
  _mm_store_ss(st.prefix, p1);
}

// AU.dsp (RB-DY L10): the mean of a window that holds no negative input is ≥ 0, but the running sum's rounding residue
// after a large drop (x² falling 160 dB: 320 dB of range in the double sum) could read slightly negative, so a
// sqrt(window(x², mean)) gave NaN for up to L frames. So out = max(out, 0) on the frames whose window holds no negative
// (cleaned) input: negLeft counts the frames until the newest one leaves (n ← x < 0 ? L : max(n − 1, 0); clamp where
// n = 0). The inputs are read back from the ring the frame wrote (the output may take x's buffer in place).
// Per chunk of ≤ 64 frames: the differences x − old in double 4 frames at a time (and the ring written), the running
// sum alone on the dependency chain (one add per frame), then the outputs 4 at a time. Every value is the per-frame
// definition's (the 8-lane kernel's per lane), only the work is regrouped.
template <bool kSplX>
void mean1(WindowState& st, const float* x, float* out, uint32_t frames) noexcept {  // RT-safe
  constexpr uint32_t kChunk = 64;
  alignas(32) double d[kChunk];
  const uint32_t len = st.length[0];
  float* v = st.values();
  uint32_t pos = st.pos[0] < len ? st.pos[0] : 0u;
  double sum = sumOf(st, 0);
  uint32_t left = st.negLeft[0];
  const double inv = 1.0 / static_cast<double>(len);
  const __m256d inv4 = _mm256_set1_pd(inv);
  const __m128 xs = kSplX ? clean<simd::S1>(_mm_set1_ps(*x)) : _mm_setzero_ps();
  uint32_t f = 0;
  while (f < frames) {
    const uint32_t run = frames - f < len - pos ? frames - f : len - pos;
    for (uint32_t c = 0; c < run; c += kChunk) {
      const uint32_t n = run - c < kChunk ? run - c : kChunk;
      float* vp = v + pos + c;
      const float* xp = x + f + c;
      float* op = out + f + c;
      uint32_t k = 0;
      for (; k + 4 <= n; k += 4) {
        const __m128 xv = kSplX ? xs : clean<simd::S1>(_mm_loadu_ps(xp + k));
        const __m128 old = _mm_loadu_ps(vp + k);
        _mm_storeu_ps(vp + k, xv);
        _mm256_store_pd(d + k, _mm256_sub_pd(_mm256_cvtps_pd(xv), _mm256_cvtps_pd(old)));
      }
      for (; k < n; ++k) {
        const float xv = _mm_cvtss_f32(kSplX ? xs : clean<simd::S1>(_mm_load_ss(xp + k)));
        d[k] = static_cast<double>(xv) - static_cast<double>(vp[k]);
        vp[k] = xv;
      }
      for (k = 0; k < n; ++k) d[k] = sum += d[k];
      // L10: a chunk without negative inputs (the usual: a square, a level) clamps from where the last one left, one
      // compare per 4 inputs and a max per 4 outputs; otherwise the count per frame.
      __m128 negs = _mm_setzero_ps();
      for (k = 0; k + 4 <= n; k += 4) negs = _mm_or_ps(negs, _mm_cmplt_ps(_mm_loadu_ps(vp + k), _mm_setzero_ps()));
      bool anyNeg = _mm_movemask_ps(negs) != 0;
      for (; k < n; ++k) anyNeg = anyNeg || vp[k] < 0.0f;
      if (!anyNeg) {
        const uint32_t from = left == 0 ? 0u : left - 1 < n ? left - 1 : n;
        left = left > n ? left - n : 0u;
        for (k = 0; k < from; ++k) op[k] = static_cast<float>(d[k] * inv);
        for (; k + 4 <= n; k += 4)
          _mm_storeu_ps(op + k, _mm_max_ps(_mm256_cvtpd_ps(_mm256_mul_pd(_mm256_load_pd(d + k), inv4)), _mm_setzero_ps()));
        for (; k < n; ++k) op[k] = std::max(static_cast<float>(d[k] * inv), 0.0f);
      } else {
        for (k = 0; k < n; ++k) {
          left = vp[k] < 0.0f ? len : left > 0 ? left - 1 : 0u;
          const float y = static_cast<float>(d[k] * inv);
          op[k] = left == 0 ? std::max(y, 0.0f) : y;
        }
      }
    }
    pos += run;
    f += run;
    if (pos == len) {
      pos = 0;
      sum = ringSum1(v, len);  // the drift-free restart: exactly the ring's sum
    }
  }
  st.pos[0] = pos;
  st.negLeft[0] = left;
  setSum(st, 0, sum);
}

// ---- 8 lanes ----------------------------------------------------------------------------------------------------------

// Lane `l` written at phase p re-expressed at phase q (the group's), with the same future outputs (atom_state.h
// WindowState). max / min: G(m) = the extremum of the lane's last m inputs, m = 1 … L, from the old layout (G(m) =
// ext of v[p − m … p − 1] for m ≤ p, else ext(G(p), v[L − m + p]), the suffix that reaches m back); the new layout
// holds v[k] = G(q − k) for k < q (stand-ins for the raw inputs with the same suffix extrema) and v[j] = G(q + L − j)
// for j ≥ q (it includes the q newest inputs too, which every later window that reads it also covers), prefix G(q).
// mean: the ring rotated by q − p (the sum is unchanged). O(L), only after a note-on or a voice move.
template <uint32_t Op>
void realignLane(WindowState& st, uint32_t l, uint32_t p, uint32_t q) noexcept {  // RT-safe
  const uint32_t len = st.length[0];
  float* v = st.values();
  float* g = v + static_cast<size_t>(len + 1) * 8;  // scratch: ⌈L / 8⌉ rows, used flat
  auto at = [&](uint32_t i) -> float& { return v[static_cast<size_t>(i) * 8 + l]; };
  if constexpr (Op == kMean) {
    for (uint32_t k = 0; k < len; ++k) g[k] = at(k);
    const uint32_t shift = (q + len - p) % len;
    for (uint32_t k = 0; k < len; ++k) at((k + shift) % len) = g[k];
  } else {
    float run = std::numeric_limits<float>::quiet_NaN();
    for (uint32_t m = 1; m <= p; ++m) g[m - 1] = run = extf<Op>(run, at(p - m));
    const float gp = run;  // NaN when p = 0: then G(m) = v[L − m]
    for (uint32_t m = p + 1; m <= len; ++m) g[m - 1] = extf<Op>(gp, at(len - m + p));
    for (uint32_t k = 0; k < q; ++k) at(k) = g[q - k - 1];
    for (uint32_t j = q; j < len; ++j) at(j) = g[q + len - j - 1];
    st.prefix[l] = q > 0 ? g[q - 1] : std::numeric_limits<float>::quiet_NaN();
  }
  st.pos[l] = q;
}

// Puts every lane at its group's phase (the position most lanes hold; the first such lane on a tie).
template <uint32_t Op>
void align8(WindowState& st) noexcept {  // RT-safe
  const uint32_t len = st.length[0];
  const __m256i p = _mm256_load_si256(reinterpret_cast<const __m256i*>(st.pos));
  if (_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(p, _mm256_set1_epi32(static_cast<int>(st.pos[0]))))) ==
          0xFF &&
      st.pos[0] < len)
    return;
  uint32_t best = 0, bestCount = 0;
  for (uint32_t l = 0; l < 8; ++l) {
    if (st.pos[l] >= len) continue;
    uint32_t c = 0;
    for (uint32_t k = 0; k < 8; ++k) c += st.pos[k] == st.pos[l] ? 1u : 0u;
    if (c > bestCount) {
      bestCount = c;
      best = l;
    }
  }
  const uint32_t q = bestCount > 0 ? st.pos[best] : 0u;
  for (uint32_t l = 0; l < 8; ++l)
    if (st.pos[l] != q) realignLane<Op>(st, l, st.pos[l] < len ? st.pos[l] : 0u, q);
}

template <uint32_t Op, bool kSplX>
void extremum8(WindowState& st, const float* x, float* out, uint32_t frames) noexcept {  // RT-safe
  align8<Op>(st);
  const uint32_t len = st.length[0];
  float* v = st.values();
  uint32_t pos = st.pos[0];
  __m256 pre = _mm256_load_ps(st.prefix);
  const __m256 xs = kSplX ? clean<simd::S8>(_mm256_loadu_ps(x)) : _mm256_setzero_ps();
  uint32_t f = 0;
  while (f < frames) {
    const uint32_t run = frames - f < len - pos ? frames - f : len - pos;
    for (uint32_t k = 0; k < run; ++k) {
      const __m256 xv = kSplX ? xs : clean<simd::S8>(_mm256_loadu_ps(x + static_cast<size_t>(f + k) * 8));
      pre = ext8<Op>(pre, xv);
      const __m256 y = ext8<Op>(_mm256_load_ps(v + static_cast<size_t>(pos + k + 1) * 8), pre);
      _mm256_store_ps(v + static_cast<size_t>(pos + k) * 8, xv);
      _mm256_storeu_ps(out + static_cast<size_t>(f + k) * 8, y);
    }
    pos += run;
    f += run;
    if (pos == len) {
      __m256 c = _mm256_load_ps(v + static_cast<size_t>(len - 1) * 8);
      for (uint32_t j = len - 1; j-- > 0;) {
        c = ext8<Op>(_mm256_load_ps(v + static_cast<size_t>(j) * 8), c);
        _mm256_store_ps(v + static_cast<size_t>(j) * 8, c);
      }
      pos = 0;
      pre = _mm256_set1_ps(std::numeric_limits<float>::quiet_NaN());
    }
  }
  _mm256_store_si256(reinterpret_cast<__m256i*>(st.pos), _mm256_set1_epi32(static_cast<int>(pos)));
  _mm256_store_ps(st.prefix, pre);
}

template <bool kSplX>
void mean8(WindowState& st, const float* x, float* out, uint32_t frames) noexcept {  // RT-safe
  align8<kMean>(st);
  const uint32_t len = st.length[0];
  float* v = st.values();
  uint32_t pos = st.pos[0];
  alignas(32) double s[8];
  for (uint32_t l = 0; l < 8; ++l) s[l] = sumOf(st, l);
  __m256d lo = _mm256_load_pd(s), hi = _mm256_load_pd(s + 4);
  const __m256d inv = _mm256_set1_pd(1.0 / static_cast<double>(len));
  const __m256 xs = kSplX ? clean<simd::S8>(_mm256_loadu_ps(x)) : _mm256_setzero_ps();
  // L10: the count per lane (5 vector ops per frame).
  __m256i left = _mm256_load_si256(reinterpret_cast<const __m256i*>(st.negLeft));
  const __m256i lenV = _mm256_set1_epi32(static_cast<int>(len)), one = _mm256_set1_epi32(1), zi = _mm256_setzero_si256();
  const __m256 zf = _mm256_setzero_ps();
  uint32_t f = 0;
  while (f < frames) {
    const uint32_t run = frames - f < len - pos ? frames - f : len - pos;
    for (uint32_t k = 0; k < run; ++k) {
      const __m256 xv = kSplX ? xs : clean<simd::S8>(_mm256_loadu_ps(x + static_cast<size_t>(f + k) * 8));
      float* slot = v + static_cast<size_t>(pos + k) * 8;
      const __m256 old = _mm256_load_ps(slot);
      _mm256_store_ps(slot, xv);
      lo = _mm256_add_pd(lo, _mm256_sub_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(xv)),
                                           _mm256_cvtps_pd(_mm256_castps256_ps128(old))));
      hi = _mm256_add_pd(hi, _mm256_sub_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(xv, 1)),
                                           _mm256_cvtps_pd(_mm256_extractf128_ps(old, 1))));
      const __m256 y = _mm256_set_m128(_mm256_cvtpd_ps(_mm256_mul_pd(hi, inv)), _mm256_cvtpd_ps(_mm256_mul_pd(lo, inv)));
      left = _mm256_max_epi32(_mm256_sub_epi32(left, one), zi);
      left = _mm256_castps_si256(
          _mm256_blendv_ps(_mm256_castsi256_ps(left), _mm256_castsi256_ps(lenV), _mm256_cmp_ps(xv, zf, _CMP_LT_OQ)));
      const __m256 clamped = _mm256_blendv_ps(y, _mm256_max_ps(y, zf), _mm256_castsi256_ps(_mm256_cmpeq_epi32(left, zi)));
      _mm256_storeu_ps(out + static_cast<size_t>(f + k) * 8, clamped);
    }
    pos += run;
    f += run;
    if (pos == len) {
      pos = 0;
      ringSum8(v, len, lo, hi);
    }
  }
  _mm256_store_pd(s, lo);
  _mm256_store_pd(s + 4, hi);
  for (uint32_t l = 0; l < 8; ++l) setSum(st, l, s[l]);
  _mm256_store_si256(reinterpret_cast<__m256i*>(st.pos), _mm256_set1_epi32(static_cast<int>(pos)));
  _mm256_store_si256(reinterpret_cast<__m256i*>(st.negLeft), left);
}

struct Window {
  static constexpr uint32_t kSpec = 1;  // x
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    if (a.frames == 0) return;
    auto& st = *static_cast<WindowState*>(a.state);
    constexpr bool spl = splBit(SM, 0);
    const float* x = a.in[0];
    float* out = a.out[0];
    const uint32_t op = st.op[0];
    if constexpr (S::kW == 1) {
      if (op == kMean) mean1<spl>(st, x, out, a.frames);
      else if (op == kMin) extremum1<kMin, spl>(st, x, out, a.frames);
      else extremum1<kMax, spl>(st, x, out, a.frames);
    } else {
      if (op == kMean) mean8<spl>(st, x, out, a.frames);
      else if (op == kMin) extremum8<kMin, spl>(st, x, out, a.frames);
      else extremum8<kMax, spl>(st, x, out, a.frames);
    }
  }
};

// ---- truepeak (DY.5) -------------------------------------------------------------------------------------------------

// ITU-R BS.1770-4 Annex 2: the 48-tap 4× interpolating filter as its four 12-tap phases (tap j of phase p is h[4j + p]).
// Group delay 23.5 at 4×: phase p's output at frame n sits at n − 5.875 + p / 4.
constexpr float kTpPhase[4][12] = {
    {0.0017089843750f, 0.0109863281250f, -0.0196533203125f, 0.0332031250000f, -0.0594482421875f, 0.1373291015625f,
     0.9721679687500f, -0.1022949218750f, 0.0476074218750f, -0.0266113281250f, 0.0148925781250f, -0.0083007812500f},
    {-0.0291748046875f, 0.0292968750000f, -0.0517578125000f, 0.0891113281250f, -0.1665039062500f, 0.4650878906250f,
     0.7797851562500f, -0.2003173828125f, 0.1015625000000f, -0.0582275390625f, 0.0330810546875f, -0.0189208984375f},
    {-0.0189208984375f, 0.0330810546875f, -0.0582275390625f, 0.1015625000000f, -0.2003173828125f, 0.7797851562500f,
     0.4650878906250f, -0.1665039062500f, 0.0891113281250f, -0.0517578125000f, 0.0292968750000f, -0.0291748046875f},
    {-0.0083007812500f, 0.0148925781250f, -0.0266113281250f, 0.0476074218750f, -0.1022949218750f, 0.9721679687500f,
     0.1373291015625f, -0.0594482421875f, 0.0332031250000f, -0.0196533203125f, 0.0109863281250f, 0.0017089843750f},
};

constexpr bool tpMirrored() {
  for (int j = 0; j < 12; ++j)
    if (kTpPhase[3][j] != kTpPhase[0][11 - j] || kTpPhase[2][j] != kTpPhase[1][11 - j]) return false;
  return true;
}
static_assert(tpMirrored(), "BS.1770's phases 3 and 2 are phases 0 and 1 reversed (the kernel relies on it)");

// The mirrored pairs in sum / difference form, each row broadcast to 8 lanes: rows 0–5 c0_j = (h0[j] + h0[11 − j]) / 2,
// 6–11 e0_j = (h0[j] − h0[11 − j]) / 2, 12–23 the same of phase 1. Exact: every tap is a multiple of 2^−13.
// With s_j = x[n − j] + x[n − 11 + j], d_j = x[n − j] − x[n − 11 + j]: y0 = P0 + Q0, y3 = P0 − Q0 (P0 = Σ c0_j s_j,
// Q0 = Σ e0_j d_j), y1 = P1 + Q1, y2 = P1 − Q1.
struct TpRows {
  alignas(32) float v[24][8];
};
constexpr TpRows makeTpRows() {
  TpRows r{};
  for (int ph = 0; ph < 2; ++ph)
    for (int j = 0; j < 6; ++j)
      for (int l = 0; l < 8; ++l) {
        r.v[ph * 12 + j][l] = (kTpPhase[ph][j] + kTpPhase[ph][11 - j]) * 0.5f;
        r.v[ph * 12 + 6 + j][l] = (kTpPhase[ph][j] - kTpPhase[ph][11 - j]) * 0.5f;
      }
  return r;
}
alignas(32) constexpr TpRows kTpRows = makeTpRows();

// One vector of estimates: 8 frames of one lane (W = 1) or one frame of 8 lanes (W = 8). p is the newest input element
// (x[n]); reads p − j·W for j = 0 … 11. max(|P + Q|, |P − Q|) = |P| + |Q|, so each pair is two abs and an add. The
// operation order is the same for both widths (a lane's bits do not depend on W).
template <uint32_t W>
inline __m256 truePeakVec(const float* p) noexcept {  // RT-safe
  const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
  auto row = [](int r) noexcept { return _mm256_load_ps(kTpRows.v[r]); };
  __m256 a = _mm256_loadu_ps(p), b = _mm256_loadu_ps(p - 11 * W);
  __m256 s = _mm256_add_ps(a, b), d = _mm256_sub_ps(a, b);
  __m256 p0 = _mm256_mul_ps(s, row(0)), q0 = _mm256_mul_ps(d, row(6));
  __m256 p1 = _mm256_mul_ps(s, row(12)), q1 = _mm256_mul_ps(d, row(18));
  for (int j = 1; j < 6; ++j) {
    a = _mm256_loadu_ps(p - j * static_cast<int>(W));
    b = _mm256_loadu_ps(p - (11 - j) * static_cast<int>(W));
    s = _mm256_add_ps(a, b);
    d = _mm256_sub_ps(a, b);
    p0 = _mm256_fmadd_ps(s, row(j), p0);
    q0 = _mm256_fmadd_ps(d, row(6 + j), q0);
    p1 = _mm256_fmadd_ps(s, row(12 + j), p1);
    q1 = _mm256_fmadd_ps(d, row(18 + j), q1);
  }
  // b is now x[n − 6], the sample itself. maxps returns its second operand when either is NaN: a NaN interpolant is
  // skipped, a NaN sample reads NaN.
  const __m256 m0 = _mm256_add_ps(_mm256_and_ps(p0, absMask), _mm256_and_ps(q0, absMask));
  const __m256 m1 = _mm256_add_ps(_mm256_and_ps(p1, absMask), _mm256_and_ps(q1, absMask));
  return _mm256_max_ps(m0, _mm256_max_ps(m1, _mm256_and_ps(b, absMask)));
}

inline void storePart(float* out, __m256 y, uint32_t n) noexcept {  // RT-safe; n ≤ 8
  alignas(32) float t[8];
  _mm256_store_ps(t, y);
  std::memcpy(out, t, n * sizeof(float));
}

struct TruePeak {
  static constexpr uint32_t kSpec = 0;  // x is always a block
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    constexpr uint32_t W = S::kW;
    constexpr uint32_t H = 11 * W;               // history elements
    constexpr uint32_t kHead = (H + 7) / 8 * 8;  // the leading elements whose vectors read history: 16 / 88
    const uint32_t total = a.frames * W;
    if (total == 0) return;
    auto& st = *static_cast<TruePeakState*>(a.state);
    const float* x = a.in[0];
    float* out = a.out[0];
    // The history and the call's first kHead elements, contiguous (zeros past the call's end). Fixed-size copies
    // unless the call is shorter than kHead.
    alignas(32) float buf[H + kHead];
    std::memcpy(buf, st.h, H * sizeof(float));
    uint32_t nh = kHead;
    if (total >= kHead) {
      std::memcpy(buf + H, x, kHead * sizeof(float));
    } else {
      nh = total;
      std::memcpy(buf + H, x, nh * sizeof(float));
      std::memset(buf + H + nh, 0, (kHead - nh) * sizeof(float));
    }
    // From kHead on every tap is inside the call's input. These run first, so the copies above have left the store
    // buffer before the head's loads read them (a load spanning two pending stores cannot be forwarded: a stall). One
    // vector per step: two at once (sharing the coefficient rows) measured slower — MSVC re-folds the rows into every
    // fma and spills the eight accumulators.
    uint32_t e = kHead;
    for (; e + 8 <= total; e += 8) _mm256_storeu_ps(out + e, truePeakVec<W>(x + e));
    for (uint32_t h = 0; h < nh; h += 8) {
      const __m256 y = truePeakVec<W>(buf + H + h);
      if (h + 8 <= total) _mm256_storeu_ps(out + h, y);
      else storePart(out + h, y, total - h);
    }
    if (e < total) {  // 1 lane: the last frames of a call that is not a multiple of 8 (e ≥ kHead > H)
      alignas(32) float t[H + 8] = {};
      std::memcpy(t, x + e - H, (total - e + H) * sizeof(float));
      storePart(out + e, truePeakVec<W>(t + H), total - e);
    }
    if (total >= H) {
      std::memcpy(st.h, x + total - H, H * sizeof(float));
    } else {
      std::memmove(st.h, st.h + total, (H - total) * sizeof(float));
      std::memcpy(st.h + H - total, x, total * sizeof(float));
    }
  }
};

}  // namespace

KernelFn findDynamicsKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept {
  switch (a) {
    case Atom::Follow: return pick<Follow>(lanes, specMask);
    case Atom::Window: return pick<Window>(lanes, specMask);
    case Atom::TruePeak: return pick<TruePeak>(lanes, specMask);
    default: return nullptr;
  }
}

bool followStateSmall(const void* state, float eps) noexcept {  // RT-safe
  const auto& st = *static_cast<const FollowState*>(state);
  for (float y : st.y)
    if (!(std::fabs(y) <= eps)) return false;
  return true;
}

bool followStateFlush(void* state) noexcept {  // RT-safe
  auto& st = *static_cast<FollowState*>(state);
  bool any = false;
  for (float& y : st.y) {
    any |= (std::bit_cast<uint32_t>(y) & 0x7FFFFFFFu) != 0;
    y = 0.0f;
  }
  return any;
}

// AU.dsp (H4): the last call left every lane's envelope where it started (a fixed point on a constant input), or at
// most eps and not growing (decayed toward 0: set to 0).
bool followStill(void* state, float eps) noexcept {  // RT-safe
  auto& st = *static_cast<FollowState*>(state);
  bool zero = false;
  for (uint32_t l = 0; l < 8; ++l) {
    if (std::bit_cast<uint32_t>(st.y[l]) == std::bit_cast<uint32_t>(st.y0[l])) continue;
    const float a = std::fabs(st.y[l]), b = std::fabs(st.y0[l]);
    if (!(a <= eps && b <= eps && a <= b)) return false;
    zero = true;
  }
  if (zero)
    for (uint32_t l = 0; l < 8; ++l)
      if (std::bit_cast<uint32_t>(st.y[l]) != std::bit_cast<uint32_t>(st.y0[l])) st.y[l] = st.y0[l] = 0.0f;
  return true;
}

// Every value the window still holds (the ring / segment values, the prefix where not empty, the running sum), every
// lane: ≤ eps. The empty-suffix sentinel (values[L]) and an empty prefix (NaN) are not history. historySmall (AU.dsp,
// RB-DY L11): the values were ≤ eps at an earlier look and only silent input came since (it writes zeros): the O(L)
// scan is skipped, the O(1) words are still checked.
bool windowStateSmall(const void* state, float eps, bool historySmall) noexcept {  // RT-safe
  const auto& st = *static_cast<const WindowState*>(state);
  const uint32_t len = st.length[0];
  const size_t n = historySmall ? 0u : static_cast<size_t>(len) * (st.lanes[0] == 8 ? 8u : 1u);
  const float* v = st.values();
  for (size_t i = 0; i < n; ++i)
    if (!(std::fabs(v[i]) <= eps)) return false;
  for (uint32_t l = 0; l < 8; ++l) {
    if (st.prefix[l] == st.prefix[l] && !(std::fabs(st.prefix[l]) <= eps)) return false;
    if (!(std::fabs(sumOf(st, l)) <= static_cast<double>(eps))) return false;
  }
  return true;
}

// Zero history at the current positions: values 0, sum 0, the prefix 0 where the segment has begun (else empty).
bool windowStateFlush(void* state) noexcept {  // RT-safe
  auto& st = *static_cast<WindowState*>(state);
  const uint32_t len = st.length[0];
  const size_t n = static_cast<size_t>(len) * (st.lanes[0] == 8 ? 8u : 1u);
  float* v = st.values();
  bool any = false;
  for (size_t i = 0; i < n; ++i) {
    any |= (std::bit_cast<uint32_t>(v[i]) & 0x7FFFFFFFu) != 0;
    v[i] = 0.0f;
  }
  for (uint32_t l = 0; l < 8; ++l) {
    const float p = st.pos[l] > 0 ? 0.0f : std::numeric_limits<float>::quiet_NaN();
    any |= st.prefix[l] == st.prefix[l] && (std::bit_cast<uint32_t>(st.prefix[l]) & 0x7FFFFFFFu) != 0;
    st.prefix[l] = p;
    any |= (std::bit_cast<uint64_t>(sumOf(st, l)) & 0x7FFFFFFFFFFFFFFFull) != 0;
    setSum(st, l, 0.0);
  }
  return any;
}

// The history (every lane's 11 words; a 1-lane block's unused words are 0) ≤ eps / set to 0.
bool truePeakStateSmall(const void* state, float eps) noexcept {  // RT-safe
  for (float v : static_cast<const TruePeakState*>(state)->h)
    if (!(std::fabs(v) <= eps)) return false;
  return true;
}

bool truePeakStateFlush(void* state) noexcept {  // RT-safe
  bool any = false;
  for (float& v : static_cast<TruePeakState*>(state)->h) {
    any |= (std::bit_cast<uint32_t>(v) & 0x7FFFFFFFu) != 0;
    v = 0.0f;
  }
  return any;
}

}  // namespace pq::dsp::detail
