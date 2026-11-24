// T5.5: ui::drawWaveform — level choice, the pixels a column covers (CPU raster of the strips), gain and reverse,
// visible-column clipping, progressive (partial) levels, the sample paths, the `waveform-lod` golden and (perf) the
// cost of a 2560-px clip.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>
#include <vector>

#include "golden_check.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/raster.h"
#include "pq/ui/scenes.h"
#include "pq/ui/waveform.h"
#include "thread_cpu.h"

using namespace pq::ui;

namespace {

// A ramp-ish synthetic mono / stereo overview: level 0 bucket k has min −a(k), max +a(k) with a(k) = (k % 100) / 100.
struct Synth {
  uint64_t frames = 0;
  std::vector<std::vector<int16_t>> data[2];
  std::vector<WaveformLevel> levels;
  WaveformData wd;
  explicit Synth(uint64_t n, uint32_t channels = 1) : frames(n) {
    for (uint32_t c = 0; c < channels; ++c) {
      std::vector<int16_t> l0((n + 255) / 256 * 2);
      for (size_t k = 0; k < l0.size() / 2; ++k) {
        const auto a = static_cast<int16_t>(static_cast<double>(k % 100) / 100.0 * 32767.0);
        l0[2 * k] = static_cast<int16_t>(c ? -a / 2 : -a);
        l0[2 * k + 1] = static_cast<int16_t>(c ? a / 2 : a);
      }
      data[c].push_back(l0);
      while (data[c].back().size() > 2) {
        const auto& b = data[c].back();
        const size_t nb = b.size() / 2;
        std::vector<int16_t> up((nb + 3) / 4 * 2);
        for (size_t k = 0; k < (nb + 3) / 4; ++k) {
          int16_t lo = b[8 * k], hi = b[8 * k + 1];
          for (size_t j = 4 * k; j < std::min(nb, 4 * k + 4); ++j) {
            lo = std::min(lo, b[2 * j]);
            hi = std::max(hi, b[2 * j + 1]);
          }
          up[2 * k] = lo;
          up[2 * k + 1] = hi;
        }
        data[c].push_back(std::move(up));
      }
    }
    levels.resize(data[0].size());
    for (size_t L = 0; L < levels.size(); ++L) {
      levels[L].framesPerBucket = uint64_t{256} << (2 * L);
      levels[L].buckets = data[0][L].size() / 2;
      for (uint32_t c = 0; c < channels; ++c) levels[L].minMax[c] = data[c][L].data();
    }
    wd.channels = channels;
    wd.frames = n;
    wd.levels = levels;
  }
};

std::vector<uint8_t> raster(const DrawList& dl, uint32_t w, uint32_t h) {
  std::vector<uint8_t> img(static_cast<size_t>(w) * h, 0);
  rasterizeCoverage(dl, img.data(), w, h, w, {0.0f, 0.0f});
  return img;
}

// Covered rows [top, bottom) of column x.
std::pair<int, int> coveredRows(const std::vector<uint8_t>& img, uint32_t w, uint32_t h, uint32_t x) {
  int top = -1, bottom = -1;
  for (uint32_t y = 0; y < h; ++y)
    if (img[y * w + x] > 127) {
      if (top < 0) top = static_cast<int>(y);
      bottom = static_cast<int>(y) + 1;
    }
  return {top, bottom};
}

}  // namespace

// WF.1: a column reads two buckets at least (was one: the coarsest level whose bucket fits a pixel).
TEST_CASE("waveform: the level is the coarsest whose bucket fits half a pixel; below 512 frames per px, the samples") {
  Synth s(1 << 20);
  CHECK(waveformLevelFor(s.wd, 100.0) == -1);
  CHECK(waveformLevelFor(s.wd, 511.0) == -1);
  CHECK(waveformLevelFor(s.wd, 512.0) == 0);
  CHECK(waveformLevelFor(s.wd, 2000.0) == 0);
  CHECK(waveformLevelFor(s.wd, 2048.0) == 1);
  CHECK(waveformLevelFor(s.wd, 10000.0) == 2);
  CHECK(waveformLevelFor(s.wd, 1e9) == static_cast<int32_t>(s.levels.size()) - 1);
}

// WF.1: the contract moved from "the rows whose centre lies in [min, max]" (hard, stair-stepped edges) to a coverage
// profile per column: a dense column is solid from its min to its max with box-filter coverage in its edge rows.
TEST_CASE("waveform: a dense column covers its min / max with fractional coverage in the edge rows; gain scales, reverse mirrors") {
  constexpr uint32_t W = 200, H = 101;
  Synth s(256 * 1000);
  DrawList dl;
  dl.reset(W, H);
  WaveformDraw d;
  d.frame0 = 0;
  d.frame1 = 512.0 * 200;  // two level-0 buckets per column
  d.rect = {0, 0, static_cast<float>(W), static_cast<float>(H)};
  const WaveformStats st = drawWaveform(dl, s.wd, d);
  CHECK(st.level == 0);
  CHECK(st.columns == W);
  CHECK_FALSE(st.approx);
  CHECK_FALSE(st.curve);
  const auto img = raster(dl, W, H);
  for (uint32_t x : {0u, 1u, 37u, 98u, 150u, 199u}) {
    const double a = static_cast<double>((2 * x + 1) % 100) / 100.0 * 32767.0;  // the larger of buckets 2x, 2x + 1
    const double v = std::trunc(a) / 32767.0;  // the int16 value back
    const double centre = 50.5, half = H * 0.5;  // the zero line on the centre of row 50
    double top = centre - v * half, bottom = centre + v * half;
    if (bottom - top < 1.0) top = centre - 0.5, bottom = centre + 0.5;  // at least lineWidth tall
    CAPTURE(x);
    for (uint32_t y = 0; y < H; ++y) {
      // The part of row [y, y + 1) inside [top, bottom].
      const double cover = std::clamp(std::min<double>(y + 1, bottom) - std::max<double>(y, top), 0.0, 1.0);
      CAPTURE(y);
      CHECK(std::abs(static_cast<double>(img[y * W + x]) / 255.0 - cover) <= 0.02);
    }
  }
  // Gain 0.5 halves the extent; reverse mirrors the image.
  DrawList half;
  half.reset(W, H);
  d.gain = 0.5f;
  (void)drawWaveform(half, s.wd, d);
  const auto ih = raster(half, W, H);
  auto weight = [&](const std::vector<uint8_t>& im, uint32_t x) {
    double sum = 0.0;
    for (uint32_t y = 0; y < H; ++y) sum += im[y * W + x] / 255.0;
    return sum;
  };
  CHECK(weight(ih, 99) * 2.0 == doctest::Approx(weight(img, 99)).epsilon(0.02));
  DrawList rev;
  rev.reset(W, H);
  d.gain = 1.0f;
  d.reverse = true;
  (void)drawWaveform(rev, s.wd, d);
  const auto ir = raster(rev, W, H);
  for (uint32_t x = 0; x < W; ++x)
    for (uint32_t y = 0; y < H; y += 5) REQUIRE(ir[y * W + x] == img[y * W + (W - 1 - x)]);
}

TEST_CASE("waveform: only the columns inside the clip rect are generated; partial levels stop at the built part") {
  Synth s(1 << 20, 2);
  DrawList dl;
  dl.reset(2560, 100);
  WaveformDraw d;
  d.frame0 = 0;
  d.frame1 = static_cast<double>(1 << 20);
  d.rect = {0, 0, 2560, 100};
  dl.pushClip({1000, 0, 1100, 100});
  WaveformStats st = drawWaveform(dl, s.wd, d);
  dl.popClip();
  CHECK(st.columns == 100);
  CHECK(st.vertices == 2 * 4 * 102);  // two channels, 100 columns + the two run ends, four vertices down each (WF.1)
  // Half the file built: level 0 holds half its buckets, every coarser level its complete ones.
  std::vector<WaveformLevel> part = s.levels;
  for (WaveformLevel& lv : part) lv.buckets = (uint64_t{1} << 19) / lv.framesPerBucket;
  WaveformData pd = s.wd;
  pd.levels = part;
  DrawList dp;
  dp.reset(2560, 100);
  st = drawWaveform(dp, pd, d);
  CHECK(st.columns == 1280);
  // Outside the file: nothing.
  d.frame0 = static_cast<double>(1 << 21);
  d.frame1 = d.frame0 + 1000.0;
  DrawList de;
  de.reset(2560, 100);
  st = drawWaveform(de, s.wd, d);
  CHECK(st.columns == 0);
  CHECK(de.empty());
}

namespace {
// A column's coverage: its sum (rows of ink), its centroid row (pixel centres) and the rows with more than 5 %.
struct Ink {
  double sum = 0.0, centroid = 0.0;
  int top = -1, bottom = -1;  // [top, bottom)
};
Ink inkOf(const std::vector<uint8_t>& img, uint32_t w, uint32_t h, uint32_t x) {
  Ink k;
  for (uint32_t y = 0; y < h; ++y) {
    const double v = img[y * w + x] / 255.0;
    k.sum += v;
    k.centroid += v * (y + 0.5);
    if (v > 0.05) {
      if (k.top < 0) k.top = static_cast<int>(y);
      k.bottom = static_cast<int>(y) + 1;
    }
  }
  if (k.sum > 0.0) k.centroid /= k.sum;
  return k;
}
WaveformData withSamples(const Synth& s, const std::vector<float>& x) {
  WaveformData wd = s.wd;
  wd.samples.frame0 = 0;
  wd.samples.frames = static_cast<int64_t>(x.size());
  wd.samples.channel[0] = x.data();
  return wd;
}
}  // namespace

TEST_CASE("waveform: below 512 frames per px the samples are drawn; without them (or without the curve's margin), approx") {
  Synth s(1 << 16);
  std::vector<float> x(1 << 16);
  for (size_t i = 0; i < x.size(); ++i) x[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.8f;
  WaveformDraw d;
  d.frame0 = 1000;
  d.frame1 = 1000 + 100;  // 0.5 frames per px over 200 px
  d.rect = {0, 0, 200, 64};
  DrawList a;
  a.reset(200, 64);
  WaveformStats st = drawWaveform(a, s.wd, d);
  CHECK(st.approx);
  CHECK(st.level == 0);
  WaveformData wd = withSamples(s, x);
  DrawList b;
  b.reset(200, 64);
  st = drawWaveform(b, wd, d);
  CHECK_FALSE(st.approx);
  CHECK(st.level == -1);
  CHECK(st.curve);
  CHECK(st.columns == 200);
  // The curve: column 100 (frames [1050, 1050.5)) sits at the signal's value there, about a line's worth of ink.
  const auto img = raster(b, 200, 64);
  const Ink k = inkOf(img, 200, 64, 100);
  const float v = std::sin(1050.25f * 0.05f) * 0.8f;
  CHECK(std::abs(k.centroid - (32.5 - static_cast<double>(v) * 32.0)) <= 0.35);
  CHECK(k.sum >= 0.9);
  CHECK(k.sum <= 2.2);
  // A window that does not cover the columns: the level instead.
  wd.samples.frame0 = 5000;
  wd.samples.frames = 100;
  DrawList c;
  c.reset(200, 64);
  st = drawWaveform(c, wd, d);
  CHECK(st.approx);
  // A window that covers the columns but not the curve's reach either side (kWaveformSampleMargin): the level too
  // (a view asks for the margin); with the margin, or at the file's own ends, the samples.
  wd.samples.frame0 = 1000;
  wd.samples.frames = 100;
  wd.samples.channel[0] = x.data() + 1000;
  DrawList e;
  e.reset(200, 64);
  CHECK(drawWaveform(e, wd, d).approx);
  wd.samples.frame0 = 1000 - kWaveformSampleMargin;
  wd.samples.frames = 100 + 2 * kWaveformSampleMargin;
  wd.samples.channel[0] = x.data() + 1000 - kWaveformSampleMargin;
  e.reset(200, 64);
  CHECK_FALSE(drawWaveform(e, wd, d).approx);
  std::vector<float> tiny(40, 0.25f);  // a whole 40-frame file: nothing beyond its ends to ask for
  Synth ts(40);
  WaveformData td = withSamples(ts, tiny);
  d.frame0 = 0;
  d.frame1 = 40;
  e.reset(200, 64);
  st = drawWaveform(e, td, d);
  CHECK_FALSE(st.approx);
  CHECK(st.curve);
}

TEST_CASE("waveform: sample columns (2 to 512 frames per px) are one unbroken antialiased stroke along a slow wave") {
  // A 40 Hz sine at 48 kHz, 8 frames per px: 150 px per cycle, slopes up to ~1.5 rows per column.
  Synth s(1 << 16);
  std::vector<float> x(1 << 16);
  for (size_t i = 0; i < x.size(); ++i) x[i] = std::sin(static_cast<float>(i) * (6.2831853f * 40.0f / 48000.0f)) * 0.9f;
  const WaveformData wd = withSamples(s, x);
  constexpr uint32_t W = 300, H = 80;
  WaveformDraw d;
  d.frame0 = 4000;
  d.frame1 = 4000 + 8.0 * W;
  d.rect = {0, 0, static_cast<float>(W), static_cast<float>(H)};
  DrawList dl;
  dl.reset(W, H);
  const WaveformStats st = drawWaveform(dl, wd, d);
  CHECK(st.level == -1);
  CHECK_FALSE(st.curve);
  CHECK(st.columns == W);
  const auto img = raster(dl, W, H);
  double minInk = 1e9, maxInk = 0.0;
  int partial = 0;
  for (uint32_t c = 0; c < W; ++c) {
    const Ink k = inkOf(img, W, H, c);
    CAPTURE(c);
    // On the curve: the centroid within 0.6 px of the signal at the column's middle.
    const double v = std::sin((4000.0 + 8.0 * (c + 0.5)) * (6.2831853 * 40.0 / 48000.0)) * 0.9;
    CHECK(std::abs(k.centroid - (40.5 - v * 40.0)) <= 0.6);
    minInk = std::min(minInk, k.sum);
    maxInk = std::max(maxInk, k.sum);
    for (uint32_t y = 0; y < H; ++y) partial += img[y * W + c] > 12 && img[y * W + c] < 243 ? 1 : 0;
    // Unbroken: the rows this column inks meet the next column's.
    if (c + 1 < W) {
      const Ink n = inkOf(img, W, H, c + 1);
      CHECK(k.top <= n.bottom);
      CHECK(n.top <= k.bottom);
    }
  }
  // An even weight: every column carries about a line's worth of ink (1 px flat, x sqrt(1 + slope^2) on a slope).
  MESSAGE("ink per column: " << minInk << " to " << maxInk);
  CHECK(minInk >= 0.9);
  CHECK(maxInk <= 2.1);
  CHECK(partial >= static_cast<int>(W));  // antialiased: fractional coverage in every column, not whole rows
}

TEST_CASE("waveform: the curve passes through the sample values and follows a band-limited signal between them") {
  std::vector<float> x(512);
  uint32_t rng = 7u;
  for (float& v : x) {
    rng = rng * 1664525u + 1013904223u;
    v = static_cast<float>(rng >> 8) / 8388608.0f - 1.0f;
  }
  for (int64_t j : {0, 1, 7, 8, 100, 255, 503, 504, 511})
    CHECK(waveformCurveAt(x.data(), 0, 512, static_cast<double>(j)) == x[static_cast<size_t>(j)]);  // exactly
  // A window further into the file reads the same curve (away from the window's ends).
  CHECK(waveformCurveAt(x.data(), 0, 512, 100.4) == waveformCurveAt(x.data() + 60, 60, 452, 100.4));
  // A sine at a quarter of the sample rate: the curve between the samples is the sine (16 taps: within 1.5 %), where
  // the straight line between them is 29 % short at the peak.
  std::vector<float> sn(512);
  for (size_t i = 0; i < sn.size(); ++i) sn[i] = static_cast<float>(std::sin(static_cast<double>(i) * 1.5707963267948966 + 0.7853981633974483));
  double worst = 0.0;
  for (double p = 100.0; p < 400.0; p += 0.137)
    worst = std::max(worst, std::abs(static_cast<double>(waveformCurveAt(sn.data(), 0, 512, p)) - std::sin(p * 1.5707963267948966 + 0.7853981633974483)));
  MESSAGE("curve vs sine at fs/4: worst " << worst);
  CHECK(worst <= 0.015);

  // Drawn at 16 px per sample: at each sample's x the stroke is at the sample's value, with a dot on it.
  Synth s(512);
  const WaveformData wd = withSamples(s, x);
  constexpr uint32_t W = 320, H = 200;
  WaveformDraw d;
  d.frame0 = 100.0;
  d.frame1 = 120.0;  // 20 samples over 320 px
  d.rect = {0, 0, static_cast<float>(W), static_cast<float>(H)};
  d.dots = false;
  DrawList dl;
  dl.reset(W, H);
  WaveformStats st = drawWaveform(dl, wd, d);
  CHECK(st.curve);
  CHECK(st.dots == 0);
  const auto img = raster(dl, W, H);
  for (int j = 101; j < 120; ++j) {
    const auto col = static_cast<uint32_t>((j - 100) * 16);  // the column that starts at sample j
    const double y = 100.5 - static_cast<double>(x[static_cast<size_t>(j)]) * 100.0;
    CAPTURE(j);
    const Ink k = inkOf(img, W, H, col);
    CHECK(static_cast<double>(k.top) <= y + 0.5);  // the stroke's ink reaches the sample's row
    CHECK(static_cast<double>(k.bottom) >= y - 0.5);
  }
  // Dots: none below 6 px per sample, all of the visible samples from there (opaque from 10 px), on the samples.
  d.dots = true;
  dl.reset(W, H);
  st = drawWaveform(dl, wd, d);
  CHECK(st.dots == 21);  // samples 100 ... 120 (the one on the right edge shows its left half)
  const auto dotted = raster(dl, W, H);
  for (int j = 101; j < 120; ++j) {
    const int cx = (j - 100) * 16;
    const double y = 100.5 - static_cast<double>(x[static_cast<size_t>(j)]) * 100.0;
    const int row = std::clamp(static_cast<int>(std::floor(y)), 0, static_cast<int>(H) - 1);
    CAPTURE(j);
    CHECK(dotted[static_cast<size_t>(row) * W + static_cast<size_t>(cx)] >= 250);
    CHECK(dotted[static_cast<size_t>(row) * W + static_cast<size_t>(cx - 1)] >= 250);
  }
  d.frame1 = 100.0 + 320.0 / 5.9;  // 5.9 px per sample
  dl.reset(W, H);
  CHECK(drawWaveform(dl, wd, d).dots == 0);
  d.frame1 = 100.0 + 320.0 / 8.0;  // 8 px per sample: half faded in
  dl.reset(W, H);
  CHECK(drawWaveform(dl, wd, d).dots == 41);
}

TEST_CASE("waveform: nothing pops at the regime boundaries (mip / samples at 512, columns / curve at 2, dots at 6 px)") {
  // The same frames drawn just either side of a boundary: the mean difference per inked pixel and the largest
  // relative difference of a column's ink stay under a stated bound.
  std::vector<float> x(1 << 18);
  uint32_t rng = 99u;
  double ph = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {  // a tone under a little noise, with a beat (a body and strokes)
    rng = rng * 1664525u + 1013904223u;
    ph += 6.2831853 * 220.0 / 48000.0;
    x[i] = static_cast<float>(0.6 * std::sin(ph) * (0.6 + 0.4 * std::sin(static_cast<double>(i) * 0.0004)) +
                              0.05 * (static_cast<double>(rng >> 8) / 8388608.0 - 1.0));
  }
  // An overview of it as media builds one (floor / ceil per 256-frame bucket).
  std::vector<int16_t> l0(x.size() / 256 * 2);
  for (size_t b = 0; b < l0.size() / 2; ++b) {
    float lo = x[b * 256], hi = lo;
    for (size_t i = b * 256; i < b * 256 + 256; ++i) {
      lo = std::min(lo, x[i]);
      hi = std::max(hi, x[i]);
    }
    l0[2 * b] = static_cast<int16_t>(std::floor(lo * 32767.0f));
    l0[2 * b + 1] = static_cast<int16_t>(std::ceil(hi * 32767.0f));
  }
  WaveformLevel lv;
  lv.framesPerBucket = 256;
  lv.buckets = l0.size() / 2;
  lv.minMax[0] = l0.data();
  WaveformData wd;
  wd.channels = 1;
  wd.frames = x.size();
  wd.levels = {&lv, 1};
  wd.samples.frame0 = 0;
  wd.samples.frames = static_cast<int64_t>(x.size());
  wd.samples.channel[0] = x.data();
  constexpr uint32_t W = 400, H = 120;
  struct Diff {
    double mean = 0.0, column = 0.0;
  };
  auto compare = [&](double fppA, double fppB, double frame0, int32_t levelA, int32_t levelB, bool curveA, bool curveB) {
    std::vector<uint8_t> a(static_cast<size_t>(W) * H, 0), b(a.size(), 0);
    WaveformDraw d;
    d.rect = {0, 0, static_cast<float>(W), static_cast<float>(H)};
    d.frame0 = frame0;
    d.frame1 = frame0 + fppA * W;
    const WaveformStats sa = rasterWaveform(wd, d, a.data(), W, H, W);
    d.frame1 = frame0 + fppB * W;
    const WaveformStats sb = rasterWaveform(wd, d, b.data(), W, H, W);
    CHECK(sa.level == levelA);
    CHECK(sb.level == levelB);
    CHECK(sa.curve == curveA);
    CHECK(sb.curve == curveB);
    Diff out;
    double inked = 0.0;
    for (uint32_t c = 0; c < W; ++c) {
      double ia = 0.0, ib = 0.0;
      for (uint32_t y = 0; y < H; ++y) {
        const double va = a[y * W + c] / 255.0, vb = b[y * W + c] / 255.0;
        ia += va;
        ib += vb;
        out.mean += std::abs(va - vb);
        inked += std::max(va, vb);
      }
      out.column = std::max(out.column, std::abs(ia - ib) / std::max(1.0, std::max(ia, ib)));
    }
    out.mean /= std::max(1.0, inked);
    return out;
  };
  {  // mip -> samples (the bucket grid aligned): the int16 floor / ceil against the floats
    const Diff m = compare(512.0, 511.999, 512.0 * 20, 0, -1, false, false);
    MESSAGE("mip / samples: mean " << m.mean << ", column " << m.column);
    CHECK(m.mean <= 0.02);
    CHECK(m.column <= 0.05);
  }
  {  // columns -> curve
    const Diff m = compare(2.0, 1.9999, 30000.0, -1, -1, false, true);
    MESSAGE("columns / curve: mean " << m.mean << ", column " << m.column);
    CHECK(m.mean <= 0.08);
    CHECK(m.column <= 0.15);
  }
  {  // the dots fade in from nothing: just past 6 px per sample they add under 1 % to any pixel
    std::vector<uint8_t> a(static_cast<size_t>(W) * H, 0), b(a.size(), 0);
    WaveformDraw d;
    d.rect = {0, 0, static_cast<float>(W), static_cast<float>(H)};
    d.frame0 = 30000.0;
    d.frame1 = 30000.0 + W / 6.01;
    const WaveformStats on = rasterWaveform(wd, d, a.data(), W, H, W);
    d.dots = false;
    const WaveformStats off = rasterWaveform(wd, d, b.data(), W, H, W);
    CHECK(on.dots > 60);
    CHECK(off.dots == 0);
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    CHECK(worst <= 2);
  }
}

// WF.1: was "the pixels drawWaveform's strips cover" (255 or nothing, up to the fill rule); now the same coverage
// profile either way, to the rounding of an 8-bit value.
TEST_CASE("waveform: rasterWaveform writes the coverage of drawWaveform's strips (CPU raster of the geometry)") {
  for (const auto& [frames, fpp, gain] : {std::tuple{uint64_t{1} << 20, 700.0, 1.0f}, std::tuple{uint64_t{1} << 18, 600.0, 1.7f},
                                          std::tuple{uint64_t{1} << 22, 9000.0, 0.4f}, std::tuple{uint64_t{1} << 16, 40.0, 1.0f},
                                          std::tuple{uint64_t{1} << 16, 0.7, 1.0f}, std::tuple{uint64_t{1} << 16, 0.05, 0.8f}}) {
    Synth s(frames, 2);
    std::vector<float> x[2];
    WaveformData wd = s.wd;
    if (fpp < 512.0) {
      for (uint32_t c = 0; c < 2; ++c) {
        x[c].resize(frames);
        for (size_t i = 0; i < x[c].size(); ++i)
          x[c][i] = std::sin(static_cast<float>(i) * (c ? 0.9f : 0.013f)) * 0.8f + std::sin(static_cast<float>(i) * 2.1f) * 0.1f;
        wd.samples.channel[c] = x[c].data();
      }
      wd.samples.frames = static_cast<int64_t>(frames);
    }
    constexpr uint32_t W = 300, H = 57;
    WaveformDraw d;
    d.frame0 = 1234.5;
    d.frame1 = d.frame0 + fpp * 280.0;
    d.rect = {7, 3, 287, 54};
    d.gain = gain;
    d.reverse = fpp > 1000.0;
    d.dots = false;  // (a dot is a triangle fan on the GPU and a distance in the image: its centre is checked above)
    DrawList dl;
    dl.reset(W, H);
    const WaveformStats sg = drawWaveform(dl, wd, d);
    const auto geom = raster(dl, W, H);
    std::vector<uint8_t> direct(static_cast<size_t>(W) * H, 0);
    const WaveformStats sr = rasterWaveform(wd, d, direct.data(), W, H, W);
    CHECK(sg.columns == sr.columns);
    CHECK(sg.level == sr.level);
    int worst = 0, covered = 0;
    for (size_t i = 0; i < direct.size(); ++i) {
      covered += geom[i] > 127 ? 1 : 0;
      worst = std::max(worst, std::abs(static_cast<int>(geom[i]) - static_cast<int>(direct[i])));
    }
    CAPTURE(fpp);
    CHECK(covered > 200);
    CHECK(worst <= 3);
  }
}

TEST_CASE("render: waveform-lod matches the golden image") {
  GpuOptions o;
  o.validation = true;
  auto img = renderSceneImage("waveform-lod", kDefaultSceneWidth, kDefaultSceneHeight, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const Image ref = test::loadGolden("waveform-lod");
  ImageDiff diff;
  const bool pass = test::matchesGolden("waveform-lod", *img, ref, true, &diff);
  REQUIRE(diff.sameSize);
  CHECK(pass);
}

TEST_CASE("waveform (perf): drawing a 2560-px stereo clip takes <= 50 us (sample level <= 100 us)" * doctest::test_suite("perf")) {
  Synth s(10ull * 60 * 48000, 2);
  DrawList dl;
  WaveformDraw d;
  d.frame0 = 0;
  d.frame1 = static_cast<double>(s.frames);
  d.rect = {0, 0, 2560, 80};
  std::vector<double> us;
  for (int i = 0; i < 200; ++i) {
    dl.reset(2560, 80);
    const double t0 = pq::test::threadCpuUs();
    (void)drawWaveform(dl, s.wd, d);
    us.push_back(pq::test::threadCpuUs() - t0);
  }
  std::sort(us.begin(), us.end());
  MESSAGE("2560-px stereo waveform: median " << us[100] << " us, p99 " << us[198] << " us");
  CHECK(us[100] <= 50.0);
  // M5 review M5: the sample-level path too (deep zoom: 0.5 and 40 frames per px from a sample window). It reads
  // the samples themselves (40 frames per px: 800 KiB of floats), so its bound is twice the overview path's.
  std::vector<float> x[2];
  for (uint32_t c = 0; c < 2; ++c) {
    x[c].resize(1u << 18);
    for (size_t i = 0; i < x[c].size(); ++i) x[c][i] = std::sin(static_cast<float>(i) * 0.01f + static_cast<float>(c)) * 0.7f;
  }
  WaveformData wd = s.wd;
  wd.samples.frame0 = 0;
  wd.samples.frames = static_cast<int64_t>(x[0].size());
  wd.samples.channel[0] = x[0].data();
  wd.samples.channel[1] = x[1].data();
  for (const double fpp : {0.5, 40.0}) {
    d.frame0 = 1000.0;
    d.frame1 = 1000.0 + 2560.0 * fpp;
    std::vector<double> t;
    for (int i = 0; i < 200; ++i) {
      dl.reset(2560, 80);
      const double t0 = pq::test::threadCpuUs();
      const WaveformStats st = drawWaveform(dl, wd, d);
      t.push_back(pq::test::threadCpuUs() - t0);
      REQUIRE(st.level == -1);
    }
    std::sort(t.begin(), t.end());
    MESSAGE("2560-px stereo waveform at " << fpp << " frames/px (samples): median " << t[100] << " us");
    CHECK(t[100] <= 100.0);
  }
}
