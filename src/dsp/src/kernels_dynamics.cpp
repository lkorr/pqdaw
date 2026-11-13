// Dynamics atoms (DY.1, docs/plans/DY.md): follow (attack / release one-pole envelope) and window (sliding max / min /
// mean). Semantics: pq/dsp/atoms.h "Dynamics"; state layouts: atom_state.h FollowState / WindowState.
//
// CPU: follow's recursion is the only per-sample dependency: y feeds two independent fmas and a compare, then a blend
// (≈ 5 cycles of latency per frame; x·(1 − r) and the input clean-up are off the chain). Its coefficients are one
// vector exp per call when attack / release are param rate. window max / min (1 lane) runs 8 frames per step: an
// in-register prefix extremum (3 permute + max), one carry; the segment's suffix pass is the same, backwards, once per
// L frames. 8 lanes: one vector per frame (the voices), the suffix pass one vector op per row. mean: one double add per
// frame (the running sum), and a 4-way double sum over the ring per L frames.

#include <cmath>
#include <cstring>
#include <limits>

#include "kernels_common.h"

namespace pq::dsp::detail {
namespace {

constexpr float kFltMax = std::numeric_limits<float>::max();

// NaN → 0, ±inf → ±FLT_MAX (both atoms' input rule: a follower or a window never holds a non-finite value).
template <class S>
inline typename S::F clean(typename S::F x) noexcept {  // RT-safe
  const auto ordered = S::eq(x, x);
  return S::min(S::max(S::band(x, ordered), S::set(-kFltMax)), S::set(kFltMax));
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

template <bool kSplX>
void mean1(WindowState& st, const float* x, float* out, uint32_t frames) noexcept {  // RT-safe
  const uint32_t len = st.length[0];
  float* v = st.values();
  uint32_t pos = st.pos[0] < len ? st.pos[0] : 0u;
  double sum = sumOf(st, 0);
  const double inv = 1.0 / static_cast<double>(len);
  const float xs = kSplX ? _mm_cvtss_f32(clean<simd::S1>(_mm_load_ss(x))) : 0.0f;
  uint32_t f = 0;
  while (f < frames) {
    const uint32_t run = frames - f < len - pos ? frames - f : len - pos;
    for (uint32_t k = 0; k < run; ++k) {
      const float xv = kSplX ? xs : _mm_cvtss_f32(clean<simd::S1>(_mm_load_ss(x + f + k)));
      sum += static_cast<double>(xv) - static_cast<double>(v[pos + k]);
      v[pos + k] = xv;
      out[f + k] = static_cast<float>(sum * inv);
    }
    pos += run;
    f += run;
    if (pos == len) {
      pos = 0;
      sum = ringSum1(v, len);  // the drift-free restart: exactly the ring's sum
    }
  }
  st.pos[0] = pos;
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
      _mm256_storeu_ps(out + static_cast<size_t>(f + k) * 8, y);
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

}  // namespace

KernelFn findDynamicsKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept {
  switch (a) {
    case Atom::Follow: return pick<Follow>(lanes, specMask);
    case Atom::Window: return pick<Window>(lanes, specMask);
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

// Every value the window still holds (the ring / segment values, the prefix where not empty, the running sum), every
// lane: ≤ eps. The empty-suffix sentinel (values[L]) and an empty prefix (NaN) are not history.
bool windowStateSmall(const void* state, float eps) noexcept {  // RT-safe
  const auto& st = *static_cast<const WindowState*>(state);
  const uint32_t len = st.length[0];
  const size_t n = static_cast<size_t>(len) * (st.lanes[0] == 8 ? 8u : 1u);
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

}  // namespace pq::dsp::detail
