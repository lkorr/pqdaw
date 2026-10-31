// Waveform drawing (T5.5): see pq/ui/waveform.h.

#include "pq/ui/waveform.h"

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace pq::ui {

namespace {

constexpr float kInt16 = 1.0f / 32767.0f;

// Per-column scratch: min / max per channel (normalised), and whether the column has data.
struct Scratch {
  std::vector<float> lo, hi;  // [channel * columns + col]
  std::vector<uint8_t> has;   // [col]
  std::vector<int32_t> k0, nb;  // [col]: the level path's first bucket and bucket count
};
Scratch& scratch() {
  static thread_local Scratch s;
  return s;
}

}  // namespace

int32_t waveformLevelFor(const WaveformData& data, double framesPerPx) noexcept {
  if (data.levels.empty()) return -1;
  if (framesPerPx < static_cast<double>(data.levels[0].framesPerBucket)) return -1;
  int32_t best = 0;
  for (size_t i = 1; i < data.levels.size(); ++i)
    if (static_cast<double>(data.levels[i].framesPerBucket) <= framesPerPx) best = static_cast<int32_t>(i);
  return best;
}

namespace {

// The columns of a draw: [c0, c0 + ncol) from rect.x0, per channel min / max in the scratch; ok = something to draw.
struct Prep {
  WaveformStats st;
  uint32_t channels = 0;
  int32_t c0 = 0;
  size_t ncol = 0;
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
  const double fpp = (d.frame1 - d.frame0) / static_cast<double>(width);
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

  // Level of detail.
  int32_t level = waveformLevelFor(data, fpp);
  const WaveformSamples& sm = data.samples;
  if (level < 0) {
    double fa = 0, fb = 0, ga = 0, gb = 0;
    colFrames(c0, fa, fb);
    colFrames(c1 - 1, ga, gb);
    const double lo = std::max(0.0, std::min(fa, ga) - 1.0), hi = std::min(static_cast<double>(data.frames), std::max(fb, gb) + 1.0);
    const bool covered = sm.frames > 0 && static_cast<double>(sm.frame0) <= lo && static_cast<double>(sm.frame0 + sm.frames) >= hi;
    if (!covered) {
      if (data.levels.empty()) return pr;
      level = 0;
      st.approx = true;
    }
  }
  st.level = level;

  Scratch& s = scratch();
  const auto ncol = static_cast<size_t>(c1 - c0);
  if (s.lo.size() < ncol * channels) {
    s.lo.resize(ncol * channels);
    s.hi.resize(ncol * channels);
  }
  if (s.has.size() < ncol) s.has.resize(ncol);
  const double total = static_cast<double>(data.frames);

  if (level >= 0) {
    const WaveformLevel& lv = data.levels[static_cast<size_t>(level)];
    // Buckets are 256 · 4^L frames: multiplying by the inverse is exact (a power of two), and cheaper than dividing.
    const double invFpb = 1.0 / static_cast<double>(lv.framesPerBucket);
    const auto valid = static_cast<int64_t>(lv.buckets);
    float* const los = s.lo.data();
    float* const his = s.hi.data();
    uint8_t* const has = s.has.data();
    if (s.k0.size() < ncol) {
      s.k0.resize(ncol);
      s.nb.resize(ncol);
    }
    int32_t* const k0s = s.k0.data();
    int32_t* const nbs = s.nb.data();
    // M5 review M5: first the bucket range of every column (no channel work), then per channel a reduction with a
    // fixed trip count (kFixed lanes, the ones past the column's buckets repeat its first): no data-dependent branch
    // per column. At the level chosen a column spans fewer than 6 buckets (framesPerPx < 4 · framesPerBucket); the
    // top level can span more (a whole file in a few px): those take the loop.
    constexpr int32_t kFixed = 5;
    bool wide = false;
    for (int32_t c = c0; c < c1; ++c) {
      const size_t i = static_cast<size_t>(c - c0);
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
    const size_t n8 = wide ? 0 : ncol / 8 * 8;
    const __m256 scale = _mm256_set1_ps(kInt16);
    for (uint32_t ch = 0; ch < channels; ++ch) {
      const int16_t* const base = lv.minMax[ch];
      const int* const words = reinterpret_cast<const int*>(base);
      float* const lo_ = los + ch * ncol;
      float* const hi_ = his + ch * ncol;
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
      for (size_t i = n8; i < ncol; ++i) {
        const int16_t* mm = base + 2 * static_cast<int64_t>(k0s[i]);
        int32_t lo = mm[0], hi = mm[1];
        for (int32_t k = 1; k < nbs[i]; ++k) {
          lo = mm[2 * k] < lo ? mm[2 * k] : lo;
          hi = mm[2 * k + 1] > hi ? mm[2 * k + 1] : hi;
        }
        lo_[i] = static_cast<float>(lo) * kInt16;
        hi_[i] = static_cast<float>(hi) * kInt16;
      }
    }
  } else {
    // M5 review M5: per column the sample indices once (not per channel), no library rounding calls, and the
    // min / max of a column's samples 8 at a time (2560 stereo px at 40 frames per px: 114 → 64 µs).
    const auto w0 = static_cast<double>(sm.frame0), wn = static_cast<double>(sm.frames);
    const int64_t last = sm.frames - 1;
    for (int32_t c = c0; c < c1; ++c) {
      const size_t i = static_cast<size_t>(c - c0);
      double fa = 0, fb = 0;
      colFrames(c, fa, fb);
      fa = std::max(fa, 0.0);
      fb = std::min(fb, total);
      if (fb <= fa) {
        s.has[i] = 0;
        continue;
      }
      s.has[i] = 1;
      // Window-relative positions; sample j sits at position j.
      const double a = fa - w0, b = fb - w0;
      if (fpp >= 1.0) {
        const int64_t fl = a >= 0.0 ? static_cast<int64_t>(a) : static_cast<int64_t>(std::floor(a));
        const int64_t j0 = std::clamp<int64_t>(fl, 0, last);
        auto cb = static_cast<int64_t>(b);
        cb += static_cast<double>(cb) < b ? 1 : 0;  // ceil (b > a ≥ 0 here, or clamped below)
        const int64_t j1 = std::clamp<int64_t>(cb, j0 + 1, sm.frames);
        for (uint32_t ch = 0; ch < channels; ++ch) {
          const float* x = sm.channel[ch];
          float lo = x[j0], hi = x[j0];
          int64_t k = j0 + 1;
          if (j1 - k >= 8) {
            __m256 vmn = _mm256_set1_ps(lo), vmx = vmn;
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
          s.lo[ch * ncol + i] = lo;
          s.hi[ch * ncol + i] = hi;
        }
      } else {
        // The interpolation points (clamped to the window): a, b and the one sample between them, if any.
        const double pa = std::clamp(a, 0.0, wn - 1.0), pb = std::clamp(b, 0.0, wn - 1.0);
        const auto ja = static_cast<int64_t>(pa), jb = static_cast<int64_t>(pb);
        const auto ta = static_cast<float>(pa - static_cast<double>(ja)), tb = static_cast<float>(pb - static_cast<double>(jb));
        const int64_t ja1 = std::min<int64_t>(ja + 1, last), jb1 = std::min<int64_t>(jb + 1, last);
        auto cj = static_cast<int64_t>(a);
        if (a < 0.0) cj = static_cast<int64_t>(std::ceil(a));
        else cj += static_cast<double>(cj) < a ? 1 : 0;  // ceil(a)
        const bool mid = static_cast<double>(cj) < b;  // fpp < 1: at most one sample lies inside the column
        const double pm = std::clamp(static_cast<double>(cj), 0.0, wn - 1.0);
        const auto jm = static_cast<int64_t>(pm);
        const auto tm = static_cast<float>(pm - static_cast<double>(jm));
        const int64_t jm1 = std::min<int64_t>(jm + 1, last);
        for (uint32_t ch = 0; ch < channels; ++ch) {
          const float* x = sm.channel[ch];
          float lo = x[ja] + (x[ja1] - x[ja]) * ta, hi = lo;
          const float vb = x[jb] + (x[jb1] - x[jb]) * tb;
          lo = std::min(lo, vb);
          hi = std::max(hi, vb);
          if (mid) {
            const float v = x[jm] + (x[jm1] - x[jm]) * tm;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
          }
          s.lo[ch * ncol + i] = lo;
          s.hi[ch * ncol + i] = hi;
        }
      }
    }
  }
  pr.channels = channels;
  pr.c0 = c0;
  pr.ncol = ncol;
  pr.ok = true;
  return pr;
}

// A column's covered span in lane [y0, y1) (at least 1 px tall), as the strip draws it.
inline void columnSpan(float lo, float hi, float centre, float half, float y0, float y1, float& top, float& bottom) noexcept {
  top = std::clamp(centre - hi * half, y0, y1);
  bottom = std::clamp(centre - lo * half, y0, y1);
  if (bottom - top < 1.0f) {
    const float m = std::clamp(0.5f * (top + bottom), y0 + 0.5f, y1 - 0.5f);
    top = m - 0.5f;
    bottom = m + 0.5f;
  }
}

}  // namespace

WaveformStats drawWaveform(DrawList& dl, const WaveformData& data, const WaveformDraw& d) {
  const Prep pr = prepare(data, d, dl.clip());
  WaveformStats st = pr.st;
  if (!pr.ok) return st;
  Scratch& s = scratch();
  const uint32_t channels = pr.channels;
  const int32_t c0 = pr.c0;
  const size_t ncol = pr.ncol;

  // Emit one strip per channel and run of columns with data.
  const Vec2 uv = dl.whiteUv();
  const float laneH = (d.rect.y1 - d.rect.y0) / static_cast<float>(channels);
  for (uint32_t ch = 0; ch < channels; ++ch) {
    const float y0 = d.rect.y0 + laneH * static_cast<float>(ch);
    const float y1 = y0 + laneH;
    const float centre = 0.5f * (y0 + y1);
    const float half = 0.5f * laneH * d.gain;
    const float* lo = s.lo.data() + ch * ncol;
    const float* hi = s.hi.data() + ch * ncol;
    auto span = [&](size_t i, float& top, float& bottom) { columnSpan(lo[i], hi[i], centre, half, y0, y1, top, bottom); };
    size_t i = 0;
    while (i < ncol) {
      if (!s.has[i]) {
        ++i;
        continue;
      }
      size_t e = i;
      while (e < ncol && s.has[e]) ++e;
      const auto m = static_cast<uint32_t>(e - i);
      const uint32_t pairs = m + 2;
      const DrawList::PrimSpan sp = dl.primReserveSpan(6 * (pairs - 1), 2 * pairs);
      Vertex* v = sp.v;
      float top = 0, bottom = 0;
      const float xl = d.rect.x0 + static_cast<float>(c0) + static_cast<float>(i);
      span(i, top, bottom);
      v[0] = {{xl, top}, uv, d.colour};
      v[1] = {{xl, bottom}, uv, d.colour};
      v += 2;
      size_t k = i;
      {  // eight spans per AVX pass, the same arithmetic as columnSpan (M5 review M5)
        const __m256 vc = _mm256_set1_ps(centre), vh = _mm256_set1_ps(half), vy0 = _mm256_set1_ps(y0), vy1 = _mm256_set1_ps(y1);
        const __m256 vy0h = _mm256_set1_ps(y0 + 0.5f), vy1h = _mm256_set1_ps(y1 - 0.5f);
        const __m256 one = _mm256_set1_ps(1.0f), halfPx = _mm256_set1_ps(0.5f);
        alignas(32) float tops[8], bots[8];
        for (; k + 8 <= e; k += 8) {
          __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_sub_ps(vc, _mm256_mul_ps(_mm256_loadu_ps(hi + k), vh)), vy0), vy1);
          __m256 b = _mm256_min_ps(_mm256_max_ps(_mm256_sub_ps(vc, _mm256_mul_ps(_mm256_loadu_ps(lo + k), vh)), vy0), vy1);
          const __m256 thin = _mm256_cmp_ps(_mm256_sub_ps(b, t), one, _CMP_LT_OQ);
          const __m256 mid = _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(_mm256_add_ps(t, b), halfPx), vy0h), vy1h);
          t = _mm256_blendv_ps(t, _mm256_sub_ps(mid, halfPx), thin);
          b = _mm256_blendv_ps(b, _mm256_add_ps(mid, halfPx), thin);
          _mm256_store_ps(tops, t);
          _mm256_store_ps(bots, b);
          for (int j = 0; j < 8; ++j) {
            const float x = d.rect.x0 + static_cast<float>(c0) + static_cast<float>(k + static_cast<size_t>(j)) + 0.5f;
            v[0] = {{x, tops[j]}, uv, d.colour};
            v[1] = {{x, bots[j]}, uv, d.colour};
            v += 2;
          }
        }
        if (k > i) {
          top = tops[7];
          bottom = bots[7];
        }
      }
      for (; k < e; ++k) {
        span(k, top, bottom);
        const float x = d.rect.x0 + static_cast<float>(c0) + static_cast<float>(k) + 0.5f;
        v[0] = {{x, top}, uv, d.colour};
        v[1] = {{x, bottom}, uv, d.colour};
        v += 2;
      }
      const float xr = d.rect.x0 + static_cast<float>(c0) + static_cast<float>(e);
      v[0] = {{xr, top}, uv, d.colour};
      v[1] = {{xr, bottom}, uv, d.colour};
      uint32_t* ix = sp.i;
      // Four quads (24 indices) per three 8-lane stores (M5 review M5: the index writes were a third of the draw).
      uint32_t q = 0;
      {
        const __m256i p0 = _mm256_setr_epi32(0, 2, 3, 0, 3, 1, 2, 4);
        const __m256i p1 = _mm256_setr_epi32(5, 2, 5, 3, 4, 6, 7, 4);
        const __m256i p2 = _mm256_setr_epi32(7, 5, 6, 8, 9, 6, 9, 7);
        __m256i a = _mm256_set1_epi32(static_cast<int>(sp.base));
        const __m256i eight = _mm256_set1_epi32(8);
        for (; q + 4 < pairs; q += 4) {
          _mm256_storeu_si256(reinterpret_cast<__m256i*>(ix), _mm256_add_epi32(a, p0));
          _mm256_storeu_si256(reinterpret_cast<__m256i*>(ix + 8), _mm256_add_epi32(a, p1));
          _mm256_storeu_si256(reinterpret_cast<__m256i*>(ix + 16), _mm256_add_epi32(a, p2));
          a = _mm256_add_epi32(a, eight);
          ix += 24;
        }
      }
      for (; q + 1 < pairs; ++q) {
        const uint32_t a = sp.base + 2 * q;
        ix[0] = a;
        ix[1] = a + 2;
        ix[2] = a + 3;
        ix[3] = a;
        ix[4] = a + 3;
        ix[5] = a + 1;
        ix += 6;
      }
      st.vertices += 2 * pairs;
      if (ch == 0) st.columns += m;
      i = e;
    }
  }
  return st;
}

WaveformStats rasterWaveform(const WaveformData& data, const WaveformDraw& d, uint8_t* dst, uint32_t width, uint32_t height,
                             uint32_t rowPitch) {
  const Prep pr = prepare(data, d, ClipRect{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)});
  WaveformStats st = pr.st;
  if (!pr.ok) return st;
  Scratch& s = scratch();
  const float laneH = (d.rect.y1 - d.rect.y0) / static_cast<float>(pr.channels);
  for (uint32_t ch = 0; ch < pr.channels; ++ch) {
    const float y0 = d.rect.y0 + laneH * static_cast<float>(ch);
    const float y1 = y0 + laneH;
    const float centre = 0.5f * (y0 + y1);
    const float half = 0.5f * laneH * d.gain;
    const float* lo = s.lo.data() + ch * pr.ncol;
    const float* hi = s.hi.data() + ch * pr.ncol;
    for (size_t i = 0; i < pr.ncol; ++i) {
      if (!s.has[i]) continue;
      const int32_t x = static_cast<int32_t>(d.rect.x0) + pr.c0 + static_cast<int32_t>(i);
      if (x < 0 || x >= static_cast<int32_t>(width)) continue;
      float top = 0, bottom = 0;
      columnSpan(lo[i], hi[i], centre, half, y0, y1, top, bottom);
      // Rows whose centre lies in [top, bottom): what the GPU covers sampling at pixel centres.
      const int32_t r0 = std::max(0, static_cast<int32_t>(std::ceil(top - 0.5f)));
      const int32_t r1 = std::min(static_cast<int32_t>(height), static_cast<int32_t>(std::ceil(bottom - 0.5f)));
      for (int32_t y = r0; y < r1; ++y) dst[static_cast<size_t>(y) * rowPitch + static_cast<size_t>(x)] = 255;
      if (ch == 0) ++st.columns;
    }
  }
  return st;
}

}  // namespace pq::ui
