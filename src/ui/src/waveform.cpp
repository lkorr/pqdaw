// Waveform drawing (T5.5; WF.1): see pq/ui/waveform.h.

#include "pq/ui/waveform.h"

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace pq::ui {

namespace {

constexpr float kInt16 = 1.0f / 32767.0f;
constexpr float kNone = 2.0e30f;  // an empty column: lo = kNone, hi = −kNone (lo > hi)

// Per-column scratch. Each channel's lo / hi row is `stride` = columns + 2 long: one empty column either side, so a
// column's neighbours are always readable.
struct Scratch {
  std::vector<float> lo, hi;       // [channel * stride + 1 + col]
  std::vector<uint8_t> has;        // [col] (the level path)
  std::vector<int32_t> k0, nb;     // [col]: the level path's first bucket and bucket count
  std::vector<float> edge;         // [channel * (columns + 1) + edge]: the sample path's values at the column edges
  std::vector<float> yA, yB, yC, yD, al;  // [col]: a lane's profiles
};
Scratch& scratch() {
  static thread_local Scratch s;
  return s;
}

// ---- the band-limited curve: a 16-tap Hann-windowed sinc, 64 phases (linear between two) ----
constexpr int kTaps = 16, kHalfTaps = 8, kPhases = 64;
struct SincTable {
  alignas(32) float h[kPhases + 1][kTaps];
  SincTable() noexcept {
    for (int r = 0; r <= kPhases; ++r) {
      const double f = static_cast<double>(r) / kPhases;
      double sum = 0.0, v[kTaps];
      for (int t = 0; t < kTaps; ++t) {
        const double d = static_cast<double>(t - (kHalfTaps - 1)) - f;
        const double u = d / kHalfTaps;
        const double win = std::fabs(u) < 1.0 ? 0.5 * (1.0 + std::cos(std::numbers::pi * u)) : 0.0;
        const double sn = std::fabs(d) < 1e-12 ? 1.0 : std::sin(std::numbers::pi * d) / (std::numbers::pi * d);
        v[t] = sn * win;
        sum += v[t];
      }
      for (int t = 0; t < kTaps; ++t) h[r][t] = static_cast<float>(v[t] / sum);  // unity at DC; phase 0 is a delta
    }
  }
};
const SincTable& sincTable() noexcept {
  static const SincTable t;
  return t;
}

// The kernel for the fraction f in [0, 1) between two samples.
struct Kernel {
  __m256 a, b;
};
inline Kernel kernelFor(const SincTable& T, double f) noexcept {
  const double ph = f * kPhases;
  auto r = static_cast<int>(ph);
  r = r < 0 ? 0 : (r >= kPhases ? kPhases - 1 : r);
  const __m256 t = _mm256_set1_ps(static_cast<float>(ph - static_cast<double>(r)));
  const __m256 a0 = _mm256_load_ps(T.h[r]), a1 = _mm256_load_ps(T.h[r + 1]);
  const __m256 b0 = _mm256_load_ps(T.h[r] + 8), b1 = _mm256_load_ps(T.h[r + 1] + 8);
  return {_mm256_add_ps(a0, _mm256_mul_ps(_mm256_sub_ps(a1, a0), t)), _mm256_add_ps(b0, _mm256_mul_ps(_mm256_sub_ps(b1, b0), t))};
}
inline float hsum(__m256 v) noexcept {
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
  return _mm_cvtss_f32(s);
}
// Σ k[t] · x[j − 7 + t]; samples outside [0, n) read 0.
inline float applyKernel(const Kernel& k, const float* x, int64_t n, int64_t j) noexcept {
  const int64_t first = j - (kHalfTaps - 1);
  if (first >= 0 && first + kTaps <= n)
    return hsum(_mm256_add_ps(_mm256_mul_ps(k.a, _mm256_loadu_ps(x + first)), _mm256_mul_ps(k.b, _mm256_loadu_ps(x + first + 8))));
  alignas(32) float tmp[kTaps];
  for (int t = 0; t < kTaps; ++t) {
    const int64_t i = first + t;
    tmp[t] = i >= 0 && i < n ? x[i] : 0.0f;
  }
  return hsum(_mm256_add_ps(_mm256_mul_ps(k.a, _mm256_load_ps(tmp)), _mm256_mul_ps(k.b, _mm256_load_ps(tmp + 8))));
}
// j = floor(p), f = p − j for a window-relative position.
inline void splitPos(double p, int64_t& j, double& f) noexcept {
  const double fl = std::floor(p);
  j = static_cast<int64_t>(fl);
  f = p - fl;
}

}  // namespace

float waveformCurveAt(const float* samples, int64_t frame0, int64_t frames, double position) noexcept {
  if (samples == nullptr || frames <= 0) return 0.0f;
  int64_t j = 0;
  double f = 0.0;
  splitPos(position - static_cast<double>(frame0), j, f);
  return applyKernel(kernelFor(sincTable(), f), samples, frames, j);
}

int32_t waveformLevelFor(const WaveformData& data, double framesPerPx) noexcept {
  // WF.1: a column reads at least two buckets (it was one: a column the size of a bucket straddles two, so it showed
  // up to twice its frames and a slow wave drew as a thick stepped band).
  if (data.levels.empty()) return -1;
  if (framesPerPx < 2.0 * static_cast<double>(data.levels[0].framesPerBucket)) return -1;
  int32_t best = 0;
  for (size_t i = 1; i < data.levels.size(); ++i)
    if (2.0 * static_cast<double>(data.levels[i].framesPerBucket) <= framesPerPx) best = static_cast<int32_t>(i);
  return best;
}

namespace {

// The columns of a draw: [c0, c0 + ncol) from rect.x0 are emitted; the scratch holds [e0, e0 + ncolE) (one more
// either side where the rect has one: a column's profile reads its neighbours, so a tile's edge column equals the
// direct draw's). ok = something to draw.
struct Prep {
  WaveformStats st;
  uint32_t channels = 0;
  int32_t c0 = 0, e0 = 0;
  size_t ncol = 0, ncolE = 0, stride = 0;
  double fpp = 1.0;
  bool ok = false;
};

Prep prepare(const WaveformData& data, const WaveformDraw& d, const ClipRect& clip) {
  Prep pr;
  WaveformStats& st = pr.st;
  const uint32_t channels = std::min(data.channels, kMaxWaveformChannels);
  const float width = d.rect.x1 - d.rect.x0;
  if (channels == 0 || data.frames == 0 || !(width > 0.0f) || !(d.frame1 > d.frame0) || d.rect.y1 <= d.rect.y0) return pr;
  const auto ncols = static_cast<int32_t>(std::ceil(width));
  const float lx = std::max(static_cast<float>(clip.x0), d.clipX0), rx = std::min(static_cast<float>(clip.x1), d.clipX1);
  if (!(rx > lx)) return pr;
  const int32_t c0 = std::max(0, static_cast<int32_t>(std::max(-1.0e9f, std::floor(lx - d.rect.x0))));
  const int32_t c1 = std::min(ncols, static_cast<int32_t>(std::min(1.0e9f, std::ceil(rx - d.rect.x0))));
  if (c1 <= c0 || static_cast<float>(clip.y1) <= d.rect.y0 || static_cast<float>(clip.y0) >= d.rect.y1) return pr;
  const int32_t e0 = std::max(0, c0 - 1), e1 = std::min(ncols, c1 + 1);
  const double fpp = (d.frame1 - d.frame0) / static_cast<double>(width);
  pr.fpp = fpp;
  // The frames column c shows: [fa, fb).
  auto colFrames = [&](int32_t c, double& fa, double& fb) {
    if (!d.reverse) {
      fa = d.frame0 + static_cast<double>(c) * fpp;
      fb = fa + fpp;
    } else {
      fb = d.frame1 - static_cast<double>(c) * fpp;
      fa = fb - fpp;
    }
  };
  const double total = static_cast<double>(data.frames);

  // Level of detail.
  int32_t level = waveformLevelFor(data, fpp);
  const WaveformSamples& sm = data.samples;
  const bool curve = fpp < 2.0;
  if (level < 0) {
    double fa = 0, fb = 0, ga = 0, gb = 0;
    colFrames(e0, fa, fb);
    colFrames(e1 - 1, ga, gb);
    const double margin = curve ? static_cast<double>(kWaveformSampleMargin) : 1.0;
    const double lo = std::max(0.0, std::floor(std::min(fa, ga)) - margin), hi = std::min(total, std::ceil(std::max(fb, gb)) + margin);
    const bool covered = sm.frames > 0 && static_cast<double>(sm.frame0) <= lo && static_cast<double>(sm.frame0 + sm.frames) >= hi;
    if (!covered) {
      if (data.levels.empty()) return pr;
      level = 0;
      st.approx = true;
    }
  }
  st.level = level;
  st.curve = level < 0 && curve;

  Scratch& s = scratch();
  const auto ncolE = static_cast<size_t>(e1 - e0);
  const size_t stride = ncolE + 2;
  if (s.lo.size() < stride * channels) {
    s.lo.resize(stride * channels);
    s.hi.resize(stride * channels);
  }
  for (uint32_t ch = 0; ch < channels; ++ch) {  // the empty columns either side
    s.lo[ch * stride] = s.lo[ch * stride + ncolE + 1] = kNone;
    s.hi[ch * stride] = s.hi[ch * stride + ncolE + 1] = -kNone;
  }

  if (level >= 0) {
    if (s.has.size() < ncolE) s.has.resize(ncolE);
    const WaveformLevel& lv = data.levels[static_cast<size_t>(level)];
    // Buckets are 256 · 4^L frames: multiplying by the inverse is exact (a power of two), and cheaper than dividing.
    const double invFpb = 1.0 / static_cast<double>(lv.framesPerBucket);
    const auto valid = static_cast<int64_t>(lv.buckets);
    uint8_t* const has = s.has.data();
    if (s.k0.size() < ncolE) {
      s.k0.resize(ncolE);
      s.nb.resize(ncolE);
    }
    int32_t* const k0s = s.k0.data();
    int32_t* const nbs = s.nb.data();
    // M5 review M5: first the bucket range of every column (no channel work), then per channel a reduction with a
    // fixed trip count (kFixed lanes, the ones past the column's buckets repeat its first): no data-dependent branch
    // per column. At the level chosen a column spans fewer than 10 buckets (framesPerPx < 8 · framesPerBucket); the
    // top level can span more (a whole file in a few px): those take the loop.
    constexpr int32_t kFixed = 9;
    bool wide = false;
    for (int32_t c = e0; c < e1; ++c) {
      const size_t i = static_cast<size_t>(c - e0);
      double fa = 0, fb = 0;
      colFrames(c, fa, fb);
      fa = fa > 0.0 ? fa : 0.0;
      fb = fb < total ? fb : total;
      // fa, fb ≥ 0: truncation is floor (no library rounding calls in this loop).
      const double xa = fa * invFpb, xb = fb * invFpb;
      const auto k0 = static_cast<int64_t>(xa);
      auto k1 = static_cast<int64_t>(xb);
      k1 += static_cast<double>(k1) < xb ? 1 : 0;
      k1 = k1 < valid ? k1 : valid;
      const bool any = fb > fa && k1 > k0;
      has[i] = any ? 1 : 0;
      k0s[i] = any ? static_cast<int32_t>(k0) : 0;  // word 0 exists whenever a column has data (valid > 0)
      nbs[i] = any ? static_cast<int32_t>(k1 - k0) : 1;
      wide = wide || (any && k1 - k0 > kFixed);
    }
    if (valid <= 0 || valid > (int64_t{1} << 30)) wide = true;  // gathers index with int32
    // Eight columns at a time: a column's (min, max) pairs are 32-bit words, gathered kFixed per column (lanes past
    // its buckets re-read its first word); 16-bit min / max on the words keeps min in the low and max in the high
    // half. 2× faster than the per-column loop (measured: 17.7 → 8.6 µs for 2560 stereo columns).
    const size_t n8 = wide ? 0 : ncolE / 8 * 8;
    const __m256 scale = _mm256_set1_ps(kInt16);
    for (uint32_t ch = 0; ch < channels; ++ch) {
      const int16_t* const base = lv.minMax[ch];
      const int* const words = reinterpret_cast<const int*>(base);
      float* const lo_ = s.lo.data() + ch * stride + 1;
      float* const hi_ = s.hi.data() + ch * stride + 1;
      for (size_t i = 0; i < n8; i += 8) {
        const __m256i k0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(k0s + i));
        const __m256i nb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(nbs + i));
        const __m256i w = _mm256_i32gather_epi32(words, k0, 4);
        __m256i mn = w, mx = w;
        for (int32_t k = 1; k < kFixed; ++k) {
          const __m256i kk = _mm256_set1_epi32(k);
          const __m256i at = _mm256_add_epi32(k0, _mm256_and_si256(_mm256_cmpgt_epi32(nb, kk), kk));
          const __m256i g = _mm256_i32gather_epi32(words, at, 4);
          mn = _mm256_min_epi16(mn, g);
          mx = _mm256_max_epi16(mx, g);
        }
        _mm256_storeu_ps(lo_ + i, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_slli_epi32(mn, 16), 16)), scale));
        _mm256_storeu_ps(hi_ + i, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_srai_epi32(mx, 16)), scale));
      }
      for (size_t i = n8; i < ncolE; ++i) {
        if (!has[i]) continue;
        const int16_t* mm = base + 2 * static_cast<int64_t>(k0s[i]);
        int32_t lo = mm[0], hi = mm[1];
        for (int32_t k = 1; k < nbs[i]; ++k) {
          lo = mm[2 * k] < lo ? mm[2 * k] : lo;
          hi = mm[2 * k + 1] > hi ? mm[2 * k + 1] : hi;
        }
        lo_[i] = static_cast<float>(lo) * kInt16;
        hi_[i] = static_cast<float>(hi) * kInt16;
      }
      // WF.1: empty columns, then the join — two neighbours whose ranges do not meet (a wave slower than the columns)
      // meet at the midpoint of the gap, as the signal between them does somewhere.
      for (size_t i = 0; i < ncolE; ++i)
        if (!has[i]) {
          lo_[i] = kNone;
          hi_[i] = -kNone;
        }
      for (size_t i = 0; i + 1 < ncolE; ++i) {
        if (!(has[i] && has[i + 1])) continue;
        if (lo_[i + 1] > hi_[i]) {
          const float m = 0.5f * (lo_[i + 1] + hi_[i]);
          hi_[i] = m;
          lo_[i + 1] = m;
        } else if (hi_[i + 1] < lo_[i]) {
          const float m = 0.5f * (hi_[i + 1] + lo_[i]);
          lo_[i] = m;
          hi_[i + 1] = m;
        }
      }
    }
  } else {
    // The samples. The curve's value at every column edge (the band-limited curve below 2 frames per px, else the
    // line between the two samples there): a column and its neighbour share it, so their ranges touch.
    const auto w0 = static_cast<double>(sm.frame0);
    const int64_t wn = sm.frames, last = sm.frames - 1;
    const size_t nedge = ncolE + 1;
    if (s.edge.size() < nedge * channels) s.edge.resize(nedge * channels);
    const SincTable& T = sincTable();
    for (size_t e = 0; e < nedge; ++e) {
      const double c = static_cast<double>(e0) + static_cast<double>(e);
      const double pos = std::clamp(!d.reverse ? d.frame0 + c * fpp : d.frame1 - c * fpp, 0.0, total) - w0;
      int64_t j = 0;
      double f = 0.0;
      splitPos(pos, j, f);
      if (curve) {
        const Kernel k = kernelFor(T, f);
        for (uint32_t ch = 0; ch < channels; ++ch) s.edge[ch * nedge + e] = applyKernel(k, sm.channel[ch], wn, j);
      } else {
        const int64_t ja = std::clamp<int64_t>(j, 0, last), jb = std::clamp<int64_t>(j + 1, 0, last);
        const auto t = static_cast<float>(f);
        for (uint32_t ch = 0; ch < channels; ++ch) {
          const float* x = sm.channel[ch];
          s.edge[ch * nedge + e] = x[ja] + (x[jb] - x[ja]) * t;
        }
      }
    }
    const bool mid = curve && fpp > 0.5;  // (above 0.5 frames per px a column may hold a turn between its edges)
    for (int32_t c = e0; c < e1; ++c) {
      const size_t i = static_cast<size_t>(c - e0);
      double fa = 0, fb = 0;
      colFrames(c, fa, fb);
      fa = std::max(fa, 0.0);
      fb = std::min(fb, total);
      if (fb <= fa) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
          s.lo[ch * stride + 1 + i] = kNone;
          s.hi[ch * stride + 1 + i] = -kNone;
        }
        continue;
      }
      // Window-relative positions; sample j sits at position j. The samples strictly inside the column: (a, b).
      const double a = fa - w0, b = fb - w0;
      const double fl = std::floor(a);
      const int64_t j0 = std::clamp<int64_t>(static_cast<int64_t>(fl) + 1, 0, wn);
      auto cb = static_cast<int64_t>(std::ceil(b));
      const int64_t j1 = std::clamp<int64_t>(cb, j0, wn);
      Kernel km{};
      int64_t jm = 0;
      if (mid) {
        double fm = 0.0;
        splitPos(0.5 * (a + b), jm, fm);
        km = kernelFor(T, fm);
      }
      for (uint32_t ch = 0; ch < channels; ++ch) {
        const float* x = sm.channel[ch];
        const float ea = s.edge[ch * nedge + i], eb = s.edge[ch * nedge + i + 1];
        float lo = std::min(ea, eb), hi = std::max(ea, eb);
        int64_t k = j0;
        if (j1 - k >= 8) {
          __m256 vmn = _mm256_set1_ps(lo), vmx = _mm256_set1_ps(hi);
          for (; k + 8 <= j1; k += 8) {
            const __m256 v = _mm256_loadu_ps(x + k);
            vmn = _mm256_min_ps(vmn, v);
            vmx = _mm256_max_ps(vmx, v);
          }
          __m128 m4 = _mm_min_ps(_mm256_castps256_ps128(vmn), _mm256_extractf128_ps(vmn, 1));
          __m128 x4 = _mm_max_ps(_mm256_castps256_ps128(vmx), _mm256_extractf128_ps(vmx, 1));
          m4 = _mm_min_ps(m4, _mm_movehl_ps(m4, m4));
          x4 = _mm_max_ps(x4, _mm_movehl_ps(x4, x4));
          m4 = _mm_min_ss(m4, _mm_shuffle_ps(m4, m4, 1));
          x4 = _mm_max_ss(x4, _mm_shuffle_ps(x4, x4, 1));
          lo = _mm_cvtss_f32(m4);
          hi = _mm_cvtss_f32(x4);
        }
        for (; k < j1; ++k) {
          lo = std::min(lo, x[k]);
          hi = std::max(hi, x[k]);
        }
        if (mid) {
          const float v = applyKernel(km, x, wn, jm);
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
        s.lo[ch * stride + 1 + i] = lo;
        s.hi[ch * stride + 1 + i] = hi;
      }
    }
  }
  pr.channels = channels;
  pr.c0 = c0;
  pr.e0 = e0;
  pr.ncol = static_cast<size_t>(c1 - c0);
  pr.ncolE = ncolE;
  pr.stride = stride;
  pr.ok = true;
  return pr;
}

// A lane: where its columns sit vertically.
struct Lane {
  float y0, y1, centre, half, lw;
};
Lane laneOf(const Rect& r, uint32_t lane, uint32_t lanes, float gain, float lineWidth) noexcept {
  const float laneH = (r.y1 - r.y0) / static_cast<float>(lanes);
  Lane L;
  L.y0 = r.y0 + laneH * static_cast<float>(lane);
  L.y1 = L.y0 + laneH;
  L.centre = std::floor(0.5f * (L.y0 + L.y1)) + 0.5f;  // the zero line on a pixel centre: silence is one crisp row
  L.half = 0.5f * laneH * gain;
  L.lw = std::max(0.05f, lineWidth);
  return L;
}

// The coverage profiles of n columns (lo / hi point at the first one; their neighbours are readable either side): a
// trapezoid down each column — alpha 0 at yA, `al` from yB to yC, 0 at yD (see the header). Empty columns: al = −1.
void profiles(const float* lo, const float* hi, size_t n, const Lane& L, float* yA, float* yB, float* yC, float* yD, float* al) noexcept {
  const float laneH = L.y1 - L.y0;
  size_t i = 0;
  {  // eight columns per pass: the arithmetic of the scalar loop below, branch-free
    const __m256 vc = _mm256_set1_ps(L.centre), vh = _mm256_set1_ps(L.half), vy0 = _mm256_set1_ps(L.y0), vy1 = _mm256_set1_ps(L.y1);
    const __m256 vlw = _mm256_set1_ps(L.lw), vlane = _mm256_set1_ps(laneH), one = _mm256_set1_ps(1.0f), hf = _mm256_set1_ps(0.5f);
    const __m256 zero = _mm256_setzero_ps(), e7 = _mm256_set1_ps(1e-7f), e6 = _mm256_set1_ps(1e-6f), neg = _mm256_set1_ps(-1.0f);
    for (; i + 8 <= n; i += 8) {
      const __m256 l = _mm256_loadu_ps(lo + i), h = _mm256_loadu_ps(hi + i);
      const __m256 pl = _mm256_loadu_ps(lo + i - 1), ph = _mm256_loadu_ps(hi + i - 1);
      const __m256 nl = _mm256_loadu_ps(lo + i + 1), nh = _mm256_loadu_ps(hi + i + 1);
      const __m256 r = _mm256_max_ps(_mm256_sub_ps(h, l), e7);
      const __m256 pe = _mm256_cmp_ps(ph, pl, _CMP_LT_OQ), ne = _mm256_cmp_ps(nh, nl, _CMP_LT_OQ);
      const __m256 rise = _mm256_add_ps(_mm256_blendv_ps(_mm256_max_ps(zero, _mm256_sub_ps(ph, l)), r, pe),
                                        _mm256_blendv_ps(_mm256_max_ps(zero, _mm256_sub_ps(h, nl)), r, ne));
      const __m256 fall = _mm256_add_ps(_mm256_blendv_ps(_mm256_max_ps(zero, _mm256_sub_ps(h, pl)), r, pe),
                                        _mm256_blendv_ps(_mm256_max_ps(zero, _mm256_sub_ps(nh, l)), r, ne));
      const __m256 w = _mm256_min_ps(_mm256_max_ps(_mm256_sub_ps(one, _mm256_div_ps(_mm256_min_ps(rise, fall), r)), zero), one);
      const __m256 ya = _mm256_min_ps(_mm256_max_ps(_mm256_sub_ps(vc, _mm256_mul_ps(h, vh)), vy0), vy1);
      const __m256 yb = _mm256_min_ps(_mm256_max_ps(_mm256_sub_ps(vc, _mm256_mul_ps(l, vh)), vy0), vy1);
      const __m256 t0 = _mm256_min_ps(ya, yb), b0 = _mm256_max_ps(ya, yb);
      const __m256 R = _mm256_sub_ps(b0, t0);
      const __m256 rho = _mm256_mul_ps(R, w);
      const __m256 P = _mm256_mul_ps(vlw, _mm256_sqrt_ps(_mm256_add_ps(one, _mm256_mul_ps(rho, rho))));
      const __m256 ext = _mm256_max_ps(zero, _mm256_mul_ps(_mm256_sub_ps(P, _mm256_mul_ps(R, _mm256_sub_ps(one, w))), hf));
      const __m256 S = _mm256_min_ps(_mm256_add_ps(R, _mm256_add_ps(ext, ext)), vlane);
      const __m256 hS = _mm256_mul_ps(S, hf);
      const __m256 m = _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(_mm256_add_ps(t0, b0), hf), _mm256_add_ps(vy0, hS)), _mm256_sub_ps(vy1, hS));
      const __m256 Sx = _mm256_max_ps(S, one);
      __m256 a = _mm256_mul_ps(_mm256_min_ps(one, _mm256_div_ps(P, _mm256_max_ps(rho, e6))), _mm256_div_ps(S, Sx));
      const __m256 hSx = _mm256_mul_ps(Sx, hf);
      const __m256 sT = _mm256_sub_ps(m, hSx), sB = _mm256_add_ps(m, hSx);
      const __m256 g = _mm256_min_ps(rho, hSx);
      const __m256 A = _mm256_sub_ps(sT, hf), D = _mm256_add_ps(sB, hf);
      __m256 B = _mm256_add_ps(_mm256_add_ps(sT, g), hf), C = _mm256_sub_ps(_mm256_sub_ps(sB, g), hf);
      const __m256 cross = _mm256_cmp_ps(B, C, _CMP_GT_OQ);
      a = _mm256_blendv_ps(a, _mm256_mul_ps(a, _mm256_div_ps(_mm256_sub_ps(m, A), _mm256_sub_ps(B, A))), cross);
      B = _mm256_blendv_ps(B, m, cross);
      C = _mm256_blendv_ps(C, m, cross);
      _mm256_storeu_ps(yA + i, A);
      _mm256_storeu_ps(yB + i, B);
      _mm256_storeu_ps(yC + i, C);
      _mm256_storeu_ps(yD + i, D);
      _mm256_storeu_ps(al + i, _mm256_blendv_ps(neg, a, _mm256_cmp_ps(h, l, _CMP_GE_OQ)));
    }
  }
  for (; i < n; ++i) {
    const float l = lo[i], h = hi[i];
    // Whether one stroke passes through the column (w = 1) or it is part of a dense body (w = 0): a stroke's column
    // has one neighbour ending where it starts and the other starting where it ends (rising or falling); what the
    // neighbours reach into its range beyond that takes w down. A missing neighbour counts as reaching all of it (a
    // lone column, a spike among quiet columns and a turn are solid).
    const float r = std::max(h - l, 1e-7f);
    const float pl = lo[i - 1], ph = hi[i - 1], nl = lo[i + 1], nh = hi[i + 1];
    const bool pe = ph < pl, ne = nh < nl;
    const float rise = (pe ? r : std::max(0.0f, ph - l)) + (ne ? r : std::max(0.0f, h - nl));
    const float fall = (pe ? r : std::max(0.0f, h - pl)) + (ne ? r : std::max(0.0f, nh - l));
    const float w = std::clamp(1.0f - std::min(rise, fall) / r, 0.0f, 1.0f);
    const float ya = std::min(std::max(L.centre - h * L.half, L.y0), L.y1), yb = std::min(std::max(L.centre - l * L.half, L.y0), L.y1);
    const float t0 = std::min(ya, yb), b0 = std::max(ya, yb);
    const float R = b0 - t0;
    const float rho = R * w;                              // the stroke's rise across the column, px
    const float P = L.lw * std::sqrt(1.0f + rho * rho);   // its thickness measured down the column
    const float ext = std::max(0.0f, (P - R * (1.0f - w)) * 0.5f);
    const float S = std::min(R + 2.0f * ext, laneH);      // the support, kept inside the lane
    const float m = std::min(std::max(0.5f * (t0 + b0), L.y0 + 0.5f * S), L.y1 - 0.5f * S);
    const float Sx = std::max(S, 1.0f);                   // thinner than a pixel: a pixel tall, fainter
    float a = std::min(1.0f, P / std::max(rho, 1e-6f)) * (S / Sx);
    const float sT = m - 0.5f * Sx, sB = m + 0.5f * Sx;
    const float g = std::min(rho, 0.5f * Sx);
    const float A = sT - 0.5f, D = sB + 0.5f;
    float B = sT + g + 0.5f, C = sB - g - 0.5f;
    if (B > C) {  // the ramps meet before the plateau: a triangle
      a *= (m - A) / (B - A);
      B = C = m;
    }
    yA[i] = A;
    yB[i] = B;
    yC[i] = C;
    yD[i] = D;
    al[i] = h >= l ? a : -1.0f;
  }
}

void growProfiles(Scratch& s, size_t n) {
  if (s.yA.size() >= n) return;
  s.yA.resize(n);
  s.yB.resize(n);
  s.yC.resize(n);
  s.yD.resize(n);
  s.al.resize(n);
}

// The index pattern of four column steps (three quads each: ramp, body, ramp), relative to the first vertex.
struct Pattern {
  alignas(32) int i[72];
  Pattern() noexcept {
    int* p = i;
    for (int q = 0; q < 4; ++q)
      for (int b = 0; b < 3; ++b) {
        const int a = 4 * q + b;
        *p++ = a;
        *p++ = a + 4;
        *p++ = a + 5;
        *p++ = a;
        *p++ = a + 5;
        *p++ = a + 1;
      }
  }
};

// The strips of n columns from device x `x0` (their profiles in the scratch): one per run of columns with data.
// Returns the columns drawn; adds the vertices to `vertices`.
uint32_t emitStrips(DrawList& dl, const Scratch& s, size_t n, float x0, Rgba colour, uint32_t& vertices) {
  const Vec2 uv = dl.whiteUv();
  const Rgba clear = withAlpha(colour, 0);
  const auto baseA = static_cast<float>(alphaOf(colour));
  const float *yA = s.yA.data(), *yB = s.yB.data(), *yC = s.yC.data(), *yD = s.yD.data(), *al = s.al.data();
  uint32_t columns = 0;
  size_t i = 0;
  while (i < n) {
    if (al[i] < 0.0f) {
      ++i;
      continue;
    }
    size_t e = i;
    while (e < n && al[e] >= 0.0f) ++e;
    const auto m = static_cast<uint32_t>(e - i);
    const uint32_t sets = m + 2;
    const DrawList::PrimSpan sp = dl.primReserveSpan(18 * (sets - 1), 4 * sets);
    Vertex* v = sp.v;
    auto put = [&](float x, size_t k) {
      const Rgba c = withAlpha(colour, static_cast<uint32_t>(baseA * al[k] + 0.5f));
      v[0] = {{x, yA[k]}, uv, clear};
      v[1] = {{x, yB[k]}, uv, c};
      v[2] = {{x, yC[k]}, uv, c};
      v[3] = {{x, yD[k]}, uv, clear};
      v += 4;
    };
    put(x0 + static_cast<float>(i), i);
    for (size_t k = i; k < e; ++k) put(x0 + static_cast<float>(k) + 0.5f, k);
    put(x0 + static_cast<float>(e), e - 1);
    uint32_t* ix = sp.i;
    uint32_t q = 0;
    {  // four column steps (72 indices) per nine 8-lane stores
      static const Pattern pat;
      __m256i base = _mm256_set1_epi32(static_cast<int>(sp.base));
      const __m256i step = _mm256_set1_epi32(16);
      for (; q + 4 < sets; q += 4) {
        for (int k = 0; k < 9; ++k)
          _mm256_storeu_si256(reinterpret_cast<__m256i*>(ix + 8 * k),
                              _mm256_add_epi32(base, _mm256_load_si256(reinterpret_cast<const __m256i*>(pat.i + 8 * k))));
        base = _mm256_add_epi32(base, step);
        ix += 72;
      }
    }
    for (; q + 1 < sets; ++q) {
      const uint32_t a = sp.base + 4 * q;
      for (uint32_t b = 0; b < 3; ++b) {  // the three quads between two columns: ramp, body, ramp
        ix[0] = a + b;
        ix[1] = a + b + 4;
        ix[2] = a + b + 5;
        ix[3] = a + b;
        ix[4] = a + b + 5;
        ix[5] = a + b + 1;
        ix += 6;
      }
    }
    vertices += 4 * sets;
    columns += m;
    i = e;
  }
  return columns;
}

// The sample dots of a curve draw: fn(x, y, channel-lane) per visible sample; returns their alpha (0: none).
struct Dots {
  float alpha = 0.0f, radius = 0.0f;
  int64_t j0 = 0, j1 = 0;  // samples [j0, j1), window-relative
};
Dots dotsOf(const WaveformData& data, const WaveformDraw& d, const Prep& pr) noexcept {
  Dots o;
  if (!d.dots || !pr.st.curve || !(pr.fpp > 0.0)) return o;
  const float ppx = static_cast<float>(1.0 / pr.fpp);
  o.alpha = std::clamp((ppx - kWaveformDotsFromPx) / (kWaveformDotsFullPx - kWaveformDotsFromPx), 0.0f, 1.0f);
  if (o.alpha <= 0.0f) return o;
  o.radius = 1.0f + std::max(0.05f, d.lineWidth);
  // The frames under the emitted columns, a radius wider.
  const double pad = static_cast<double>(o.radius + 1.0f);
  const double xa = static_cast<double>(pr.c0) - pad, xb = static_cast<double>(pr.c0) + static_cast<double>(pr.ncol) + pad;
  const double fa = !d.reverse ? d.frame0 + xa * pr.fpp : d.frame1 - xb * pr.fpp;
  const double fb = !d.reverse ? d.frame0 + xb * pr.fpp : d.frame1 - xa * pr.fpp;
  const WaveformSamples& sm = data.samples;
  const auto total = static_cast<int64_t>(data.frames);
  const int64_t lo = std::max<int64_t>({static_cast<int64_t>(std::ceil(std::max(fa, d.frame0))), 0, sm.frame0});
  const int64_t hi = std::min<int64_t>({static_cast<int64_t>(std::floor(std::min(fb, d.frame1))) + 1, total, sm.frame0 + sm.frames});
  o.j0 = lo - sm.frame0;
  o.j1 = std::max(o.j0, hi - sm.frame0);
  return o;
}
inline float dotX(const WaveformDraw& d, const Prep& pr, double frame) noexcept {
  return d.rect.x0 + static_cast<float>((!d.reverse ? frame - d.frame0 : d.frame1 - frame) / pr.fpp);
}

}  // namespace

WaveformStats drawWaveform(DrawList& dl, const WaveformData& data, const WaveformDraw& d) {
  const Prep pr = prepare(data, d, dl.clip());
  WaveformStats st = pr.st;
  if (!pr.ok) return st;
  Scratch& s = scratch();
  growProfiles(s, pr.ncol);
  const uint32_t lanes = d.merge ? 1 : pr.channels;
  const size_t off = static_cast<size_t>(pr.c0 - pr.e0) + 1;
  const float x0 = d.rect.x0 + static_cast<float>(pr.c0);
  const Dots dots = dotsOf(data, d, pr);
  for (uint32_t ch = 0; ch < pr.channels; ++ch) {
    const Lane L = laneOf(d.rect, d.merge ? 0 : ch, lanes, d.gain, d.lineWidth);
    profiles(s.lo.data() + ch * pr.stride + off, s.hi.data() + ch * pr.stride + off, pr.ncol, L, s.yA.data(), s.yB.data(), s.yC.data(),
             s.yD.data(), s.al.data());
    const uint32_t cols = emitStrips(dl, s, pr.ncol, x0, d.colour, st.vertices);
    if (ch == 0) st.columns = cols;
    if (dots.alpha > 0.0f) {
      const Rgba col = mulAlpha(d.colour, dots.alpha);
      const float* x = data.samples.channel[ch];
      for (int64_t j = dots.j0; j < dots.j1; ++j) {
        const float y = std::clamp(L.centre - x[j] * L.half, L.y0, L.y1);
        dl.addCircleFilled({dotX(d, pr, static_cast<double>(data.samples.frame0 + j)), y}, dots.radius, col);
      }
      if (ch == 0) st.dots = static_cast<uint32_t>(dots.j1 - dots.j0);
    }
  }
  return st;
}

uint32_t drawWaveformColumns(DrawList& dl, const WaveformColumns& c) {
  if (c.count == 0 || c.lo == nullptr || c.hi == nullptr || !(c.y1 > c.y0)) return 0;
  const ClipRect clip = dl.clip();
  const float lx = std::max(static_cast<float>(clip.x0), c.clipX0), rx = std::min(static_cast<float>(clip.x1), c.clipX1);
  if (!(rx > lx)) return 0;
  const int32_t c0 = std::max(0, static_cast<int32_t>(std::max(-1.0e9f, std::floor(lx - c.x0))));
  const int32_t c1 = std::min(static_cast<int32_t>(c.count), static_cast<int32_t>(std::min(1.0e9f, std::ceil(rx - c.x0))));
  if (c1 <= c0) return 0;
  Scratch& s = scratch();
  const auto n = static_cast<size_t>(c1 - c0);
  growProfiles(s, n);
  if (s.lo.size() < n + 2) {
    s.lo.resize(n + 2);
    s.hi.resize(n + 2);
  }
  // The columns and one neighbour either side (empty at the ends).
  s.lo[0] = c0 > 0 ? c.lo[c0 - 1] : kNone;
  s.hi[0] = c0 > 0 ? c.hi[c0 - 1] : -kNone;
  std::memcpy(s.lo.data() + 1, c.lo + c0, n * sizeof(float));
  std::memcpy(s.hi.data() + 1, c.hi + c0, n * sizeof(float));
  s.lo[n + 1] = c1 < static_cast<int32_t>(c.count) ? c.lo[c1] : kNone;
  s.hi[n + 1] = c1 < static_cast<int32_t>(c.count) ? c.hi[c1] : -kNone;
  Lane L;
  L.y0 = c.y0;
  L.y1 = c.y1;
  L.centre = c.centre;
  L.half = c.half;
  L.lw = std::max(0.05f, c.lineWidth);
  profiles(s.lo.data() + 1, s.hi.data() + 1, n, L, s.yA.data(), s.yB.data(), s.yC.data(), s.yD.data(), s.al.data());
  uint32_t vertices = 0;
  return emitStrips(dl, s, n, c.x0 + static_cast<float>(c0), c.colour, vertices);
}

WaveformStats rasterWaveform(const WaveformData& data, const WaveformDraw& d, uint8_t* dst, uint32_t width, uint32_t height,
                             uint32_t rowPitch) {
  const Prep pr = prepare(data, d, ClipRect{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)});
  WaveformStats st = pr.st;
  if (!pr.ok) return st;
  Scratch& s = scratch();
  growProfiles(s, pr.ncol);
  const uint32_t lanes = d.merge ? 1 : pr.channels;
  const size_t off = static_cast<size_t>(pr.c0 - pr.e0) + 1;
  const Dots dots = dotsOf(data, d, pr);
  const auto W = static_cast<int32_t>(width), H = static_cast<int32_t>(height);
  for (uint32_t ch = 0; ch < pr.channels; ++ch) {
    const Lane L = laneOf(d.rect, d.merge ? 0 : ch, lanes, d.gain, d.lineWidth);
    profiles(s.lo.data() + ch * pr.stride + off, s.hi.data() + ch * pr.stride + off, pr.ncol, L, s.yA.data(), s.yB.data(), s.yC.data(),
             s.yD.data(), s.al.data());
    for (size_t i = 0; i < pr.ncol; ++i) {
      const float a = s.al[i];
      if (a < 0.0f) continue;
      const int32_t x = static_cast<int32_t>(d.rect.x0) + pr.c0 + static_cast<int32_t>(i);
      if (x < 0 || x >= W) continue;
      if (ch == 0) ++st.columns;
      const float A = s.yA[i], B = s.yB[i], C = s.yC[i], D = s.yD[i];
      // Rows whose centre lies in (A, D): the profile at the centre, as the GPU interpolates the strip.
      const int32_t r0 = std::max(0, static_cast<int32_t>(std::floor(A + 0.5f)));
      const int32_t r1 = std::min(H, static_cast<int32_t>(std::ceil(D - 0.5f)));
      const float up = B > A ? a / (B - A) : 0.0f, down = D > C ? a / (D - C) : 0.0f;
      uint8_t* p = dst + static_cast<size_t>(r0) * rowPitch + static_cast<size_t>(x);
      for (int32_t y = r0; y < r1; ++y, p += rowPitch) {
        const float yc = static_cast<float>(y) + 0.5f;
        const float v = yc < B ? (yc - A) * up : yc <= C ? a : (D - yc) * down;
        const auto q = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        if (q > *p) *p = q;
      }
    }
    if (dots.alpha > 0.0f) {
      const float* xs = data.samples.channel[ch];
      const float r = dots.radius;
      for (int64_t j = dots.j0; j < dots.j1; ++j) {
        const float cx = dotX(d, pr, static_cast<double>(data.samples.frame0 + j));
        const float cy = std::clamp(L.centre - xs[j] * L.half, L.y0, L.y1);
        const int32_t xa = std::max(0, static_cast<int32_t>(std::floor(cx - r - 1.0f))), xb = std::min(W, static_cast<int32_t>(std::ceil(cx + r + 1.0f)));
        const int32_t ya = std::max(0, static_cast<int32_t>(std::floor(cy - r - 1.0f))), yb = std::min(H, static_cast<int32_t>(std::ceil(cy + r + 1.0f)));
        for (int32_t y = ya; y < yb; ++y)
          for (int32_t x = xa; x < xb; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
            const float v = std::clamp(r + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f) * dots.alpha;
            const auto q = static_cast<uint8_t>(v * 255.0f + 0.5f);
            uint8_t& p = dst[static_cast<size_t>(y) * rowPitch + static_cast<size_t>(x)];
            if (q > p) p = q;
          }
      }
      if (ch == 0) st.dots = static_cast<uint32_t>(dots.j1 - dots.j0);
    }
  }
  return st;
}

}  // namespace pq::ui
