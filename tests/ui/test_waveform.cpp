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

TEST_CASE("waveform: the level is the coarsest whose bucket fits a pixel; below 256 frames per px, the samples") {
  Synth s(1 << 20);
  CHECK(waveformLevelFor(s.wd, 100.0) == -1);
  CHECK(waveformLevelFor(s.wd, 256.0) == 0);
  CHECK(waveformLevelFor(s.wd, 1000.0) == 0);
  CHECK(waveformLevelFor(s.wd, 1024.0) == 1);
  CHECK(waveformLevelFor(s.wd, 5000.0) == 2);
  CHECK(waveformLevelFor(s.wd, 1e9) == static_cast<int32_t>(s.levels.size()) - 1);
}

TEST_CASE("waveform: each column covers exactly the rows of its min / max; gain scales, reverse mirrors") {
  constexpr uint32_t W = 200, H = 101;
  Synth s(256 * 1000);
  DrawList dl;
  dl.reset(W, H);
  WaveformDraw d;
  d.frame0 = 0;
  d.frame1 = 256.0 * 200;  // one level-0 bucket per column
  d.rect = {0, 0, static_cast<float>(W), static_cast<float>(H)};
  const WaveformStats st = drawWaveform(dl, s.wd, d);
  CHECK(st.level == 0);
  CHECK(st.columns == W);
  CHECK_FALSE(st.approx);
  const auto img = raster(dl, W, H);
  for (uint32_t x : {0u, 1u, 37u, 99u, 150u, 199u}) {
    const double a = static_cast<double>(x % 100) / 100.0 * 32767.0;
    const double v = std::trunc(a) / 32767.0;  // the int16 value back
    const double centre = H * 0.5, half = H * 0.5;
    const auto [top, bottom] = coveredRows(img, W, H, x);
    CAPTURE(x);
    // Rows whose centres lie in [centre − v·half, centre + v·half] (at least one row).
    const int expTop = static_cast<int>(std::ceil(centre - v * half - 0.5));
    const int expBottom = static_cast<int>(std::ceil(centre + v * half - 0.5));
    if (expBottom - expTop >= 1) {
      CHECK(top == expTop);
      CHECK(bottom == expBottom);
    } else {
      CHECK(bottom - top == 1);
    }
  }
  // Gain 0.5 halves the extent; reverse mirrors the image.
  DrawList half;
  half.reset(W, H);
  d.gain = 0.5f;
  (void)drawWaveform(half, s.wd, d);
  const auto ih = raster(half, W, H);
  const auto [t99, b99] = coveredRows(ih, W, H, 99);
  const auto [T99, B99] = coveredRows(img, W, H, 99);
  CHECK(std::abs((b99 - t99) * 2 - (B99 - T99)) <= 2);
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
  CHECK(st.vertices == 2 * 2 * 102);  // two channels, 100 columns + the two run ends
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

TEST_CASE("waveform: below 256 frames per px the samples are drawn (min / max, then the line); without them, approx") {
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
  WaveformData wd = s.wd;
  wd.samples.frame0 = 0;
  wd.samples.frames = static_cast<int64_t>(x.size());
  wd.samples.channel[0] = x.data();
  DrawList b;
  b.reset(200, 64);
  st = drawWaveform(b, wd, d);
  CHECK_FALSE(st.approx);
  CHECK(st.level == -1);
  CHECK(st.columns == 200);
  // The line: column 100 (frames [1050, 1050.5)) sits at the interpolated value there.
  const auto img = raster(b, 200, 64);
  const auto [top, bottom] = coveredRows(img, 200, 64, 100);
  const float v = std::sin(1050.25f * 0.05f) * 0.8f;
  const float y = 32.0f - v * 32.0f;
  CHECK(static_cast<float>(top) <= y + 1.0f);
  CHECK(static_cast<float>(bottom) >= y - 1.0f);
  // A window that does not cover the columns: the level instead.
  wd.samples.frame0 = 5000;
  wd.samples.frames = 100;
  DrawList c;
  c.reset(200, 64);
  st = drawWaveform(c, wd, d);
  CHECK(st.approx);
}

TEST_CASE("waveform: rasterWaveform writes the pixels drawWaveform's strips cover (CPU raster of the geometry)") {
  for (const auto& [frames, fpp, gain] : {std::tuple{uint64_t{1} << 20, 700.0, 1.0f}, std::tuple{uint64_t{1} << 18, 300.0, 1.7f},
                                          std::tuple{uint64_t{1} << 22, 9000.0, 0.4f}}) {
    Synth s(frames, 2);
    constexpr uint32_t W = 300, H = 57;
    WaveformDraw d;
    d.frame0 = 1234.5;
    d.frame1 = d.frame0 + fpp * 280.0;
    d.rect = {7, 3, 287, 54};
    d.gain = gain;
    d.reverse = fpp > 1000.0;
    DrawList dl;
    dl.reset(W, H);
    (void)drawWaveform(dl, s.wd, d);
    const auto geom = raster(dl, W, H);
    std::vector<uint8_t> direct(static_cast<size_t>(W) * H, 0);
    (void)rasterWaveform(s.wd, d, direct.data(), W, H, W);
    int differ = 0, covered = 0;
    for (size_t i = 0; i < direct.size(); ++i) {
      covered += geom[i] > 127 ? 1 : 0;
      differ += (geom[i] > 127) != (direct[i] == 255) ? 1 : 0;
    }
    CAPTURE(fpp);
    CHECK(covered > 1000);
    CHECK(differ <= covered / 200);  // only centres exactly on an edge (the fill rule) may differ
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

TEST_CASE("waveform (perf): drawing a 2560-px stereo clip takes <= 50 us" * doctest::test_suite("perf")) {
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
}
