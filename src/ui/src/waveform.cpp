// Waveform drawing (T5.5): see pq/ui/waveform.h.

#include "pq/ui/waveform.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pq::ui {

namespace {

constexpr float kInt16 = 1.0f / 32767.0f;

// Per-column scratch: min / max per channel (normalised), and whether the column has data.
struct Scratch {
  std::vector<float> lo, hi;  // [channel * columns + col]
  std::vector<uint8_t> has;   // [col]
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

WaveformStats drawWaveform(DrawList& dl, const WaveformData& data, const WaveformDraw& d) {
  WaveformStats st;
  const uint32_t channels = std::min(data.channels, kMaxWaveformChannels);
  const float width = d.rect.x1 - d.rect.x0;
  if (channels == 0 || data.frames == 0 || !(width > 0.0f) || !(d.frame1 > d.frame0) || d.rect.y1 <= d.rect.y0) return st;
  const ClipRect clip = dl.clip();
  const auto ncols = static_cast<int32_t>(std::ceil(width));
  const int32_t c0 = std::max(0, static_cast<int32_t>(std::floor(static_cast<float>(clip.x0) - d.rect.x0)));
  const int32_t c1 = std::min(ncols, static_cast<int32_t>(std::ceil(static_cast<float>(clip.x1) - d.rect.x0)));
  if (c1 <= c0 || static_cast<float>(clip.y1) <= d.rect.y0 || static_cast<float>(clip.y0) >= d.rect.y1) return st;
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
      if (data.levels.empty()) return st;
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
    const double fpb = static_cast<double>(lv.framesPerBucket);
    const auto valid = static_cast<int64_t>(lv.buckets);
    for (int32_t c = c0; c < c1; ++c) {
      const size_t i = static_cast<size_t>(c - c0);
      double fa = 0, fb = 0;
      colFrames(c, fa, fb);
      fa = std::max(fa, 0.0);
      fb = std::min(fb, total);
      const auto k0 = static_cast<int64_t>(std::floor(fa / fpb));
      const int64_t k1 = std::min(valid, static_cast<int64_t>(std::ceil(fb / fpb)));
      if (fb <= fa || k1 <= k0) {
        s.has[i] = 0;
        continue;
      }
      s.has[i] = 1;
      for (uint32_t ch = 0; ch < channels; ++ch) {
        const int16_t* mm = lv.minMax[ch];
        int16_t lo = mm[2 * k0], hi = mm[2 * k0 + 1];
        for (int64_t k = k0 + 1; k < k1; ++k) {
          lo = std::min(lo, mm[2 * k]);
          hi = std::max(hi, mm[2 * k + 1]);
        }
        s.lo[ch * ncol + i] = static_cast<float>(lo) * kInt16;
        s.hi[ch * ncol + i] = static_cast<float>(hi) * kInt16;
      }
    }
  } else {
    const auto w0 = static_cast<double>(sm.frame0), wn = static_cast<double>(sm.frames);
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
      for (uint32_t ch = 0; ch < channels; ++ch) {
        const float* x = sm.channel[ch];
        auto at = [&](double p) {  // linear interpolation between samples, clamped to the window
          p = std::clamp(p, 0.0, wn - 1.0);
          const auto j = static_cast<int64_t>(std::floor(p));
          const auto t = static_cast<float>(p - static_cast<double>(j));
          const float v0 = x[j];
          const float v1 = x[std::min<int64_t>(j + 1, sm.frames - 1)];
          return v0 + (v1 - v0) * t;
        };
        float lo, hi;
        if (fpp >= 1.0) {
          const auto j0 = std::clamp<int64_t>(static_cast<int64_t>(std::floor(a)), 0, sm.frames - 1);
          const auto j1 = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(b)), j0 + 1, sm.frames);
          lo = hi = x[j0];
          for (int64_t j = j0 + 1; j < j1; ++j) {
            lo = std::min(lo, x[j]);
            hi = std::max(hi, x[j]);
          }
        } else {
          lo = hi = at(a);
          const float vb = at(b);
          lo = std::min(lo, vb);
          hi = std::max(hi, vb);
          for (auto j = static_cast<int64_t>(std::ceil(a)); static_cast<double>(j) < b; ++j) {
            const float v = at(static_cast<double>(j));
            lo = std::min(lo, v);
            hi = std::max(hi, v);
          }
        }
        s.lo[ch * ncol + i] = lo;
        s.hi[ch * ncol + i] = hi;
      }
    }
  }

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
    auto span = [&](size_t i, float& top, float& bottom) {
      top = std::clamp(centre - hi[i] * half, y0, y1);
      bottom = std::clamp(centre - lo[i] * half, y0, y1);
      if (bottom - top < 1.0f) {
        const float m = std::clamp(0.5f * (top + bottom), y0 + 0.5f, y1 - 0.5f);
        top = m - 0.5f;
        bottom = m + 0.5f;
      }
    };
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
      for (size_t k = i; k < e; ++k) {
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
      for (uint32_t q = 0; q + 1 < pairs; ++q) {
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

}  // namespace pq::ui
