// PQ Meter line primitives: geometry of the ribbons (feather, miter clamp, bevels), Catmull-Rom subdivision
// and its output cap, the spectrum helpers, and the colour helpers.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/lines.h"

using namespace pq::ui;

namespace {

float dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace

TEST_CASE("lines: smoothPolyline on a straight line is a core plus a 0.75 px feather each side") {
  DrawList dl;
  dl.reset(100, 100);
  const Vec2 pts[3] = {{10, 50}, {30, 50}, {60, 50}};
  smoothPolyline(dl, pts, 3, rgba(10, 20, 30, 200), 2.0f);
  REQUIRE(dl.vertices().size() == 2 * 8);
  REQUIRE(dl.indices().size() == 2 * 18);
  const auto v = dl.vertices();
  // Segment 0: core ring 0..3 at y = 50 ± 1 (half thickness), outer ring 4..7 at ± 1.75, alpha 0.
  CHECK(v[0].pos.x == 10.0f);
  CHECK(v[0].pos.y == doctest::Approx(51.0f));
  CHECK(v[3].pos.y == doctest::Approx(49.0f));
  CHECK(v[4].pos.y == doctest::Approx(51.75f));
  CHECK(v[7].pos.y == doctest::Approx(48.25f));
  CHECK(v[0].col == rgba(10, 20, 30, 200));
  CHECK(v[4].col == rgba(10, 20, 30, 0));
  for (const Vertex& x : v) CHECK(x.uv.x == dl.whiteUv().x);
  // Indices stay within the segment's 8 vertices.
  for (uint32_t i = 0; i < 18; ++i) CHECK(dl.indices()[i] < 8);
  for (uint32_t i = 18; i < 36; ++i) CHECK((dl.indices()[i] >= 8 && dl.indices()[i] < 16));
  // Degenerate input draws nothing.
  const size_t n = dl.vertices().size();
  smoothPolyline(dl, pts, 1, rgba(255, 255, 255), 2.0f);
  smoothPolyline(dl, pts, 3, rgba(255, 255, 255), 0.0f);
  CHECK(dl.vertices().size() == n);
}

TEST_CASE("lines: smoothPolyline miters keep the width around a bend and clamp at 2.5x") {
  DrawList dl;
  dl.reset(200, 200);
  // 90° bend: the shared-vertex miter for the averaged normal is 1/cos(45°) = 1.414 (under the clamp).
  const Vec2 bend[3] = {{10, 100}, {60, 100}, {60, 50}};
  smoothPolyline(dl, bend, 3, rgba(255, 255, 255), 4.0f, 1.0f);
  const auto v = dl.vertices();
  // Segment 0 end b = (60,100): core offset = hw · ext along the averaged normal.
  CHECK(dist(v[1].pos, {60, 100}) == doctest::Approx(2.0f * std::sqrt(2.0f)).epsilon(1e-4));
  // A hairpin: the averaged normal is nearly along the segment, so the exact miter (1/0.0995 ≈ 10×) would
  // shoot off; it is clamped to 2.5 · hw.
  dl.reset(200, 200);
  const Vec2 hairpin[3] = {{10, 100}, {100, 100}, {14, 140}};
  smoothPolyline(dl, hairpin, 3, rgba(255, 255, 255), 4.0f, 1.0f);
  const auto w = dl.vertices();
  CHECK(dist(w[1].pos, {100, 100}) == doctest::Approx(2.5f * 2.0f).epsilon(1e-4));
  CHECK(dist(w[5].pos, {100, 100}) == doctest::Approx(2.5f * 3.0f).epsilon(1e-4));
}

TEST_CASE("lines: smoothPolylineCols bevels turns sharper than 120 degrees only") {
  DrawList dl;
  dl.reset(400, 200);
  // Zig-zag with ±60 px spikes 10 px apart: every interior turn is far sharper than 120°.
  std::vector<Vec2> zz;
  std::vector<Rgba> cols;
  for (int i = 0; i < 9; ++i) {
    zz.push_back({10.0f + 10.0f * static_cast<float>(i), i % 2 ? 40.0f : 100.0f});
    cols.push_back(rgba(static_cast<uint32_t>(i * 20), 100, 200, 255));
  }
  smoothPolylineCols(dl, zz.data(), cols.data(), 9, 2.0f);
  CHECK(dl.vertices().size() == 8 * 8 + 7 * 5);
  CHECK(dl.indices().size() == 8 * 18 + 7 * 9);
  // Per-vertex colour carried on the core; the feather ring is transparent.
  CHECK(dl.vertices()[8].col == cols[1]);
  CHECK(alphaOf(dl.vertices()[12].col) == 0);
  // A gentle polyline (turns of ~10°) gets miters, no bevels.
  dl.reset(400, 200);
  std::vector<Vec2> arc;
  for (int i = 0; i < 9; ++i) {
    const float a = 0.17f * static_cast<float>(i);
    arc.push_back({200.0f + 80.0f * std::sin(a), 150.0f - 80.0f * std::cos(a)});
  }
  smoothPolylineCols(dl, arc.data(), cols.data(), 9, 2.0f);
  CHECK(dl.vertices().size() == 8 * 8);
  // Interior shared vertices get ONE offset (no bead where two segments meet): segment 0's end equals
  // segment 1's start.
  CHECK(dl.vertices()[1].pos.x == doctest::Approx(dl.vertices()[8].pos.x));
  CHECK(dl.vertices()[1].pos.y == doctest::Approx(dl.vertices()[8].pos.y));
}

TEST_CASE("lines: splinePath interpolates the input and subdivides long segments") {
  const std::vector<Vec2> in = {{0, 0}, {20, 10}, {21, 10}, {50, -5}};
  std::vector<Vec2> out;
  splinePath(in, out, 1.5f);
  CHECK(out.front().x == 0.0f);
  CHECK(out.back().x == doctest::Approx(50.0f));
  CHECK(out.back().y == doctest::Approx(-5.0f));
  // Every input point is on the output (Catmull-Rom passes through its control points).
  for (const Vec2& p : in) {
    const bool found = std::any_of(out.begin(), out.end(), [&](Vec2 q) { return dist(p, q) < 1e-3f; });
    CHECK(found);
  }
  // Chords stay near the 1.5 px target (≤ 24 steps per segment; a curve's chord ≤ its arc).
  for (size_t i = 1; i < out.size(); ++i) CHECK(dist(out[i - 1], out[i]) <= 2.0f);
  // The 1-px segment (20,10)→(21,10) is copied, not split: exactly one point follows (20,10).
  const auto it = std::find_if(out.begin(), out.end(), [](Vec2 q) { return q.x == 20.0f && q.y == 10.0f; });
  REQUIRE(it != out.end());
  CHECK((it + 1)->x == 21.0f);
  // A segment longer than 24 · target is split into at most 24 steps.
  splinePath(std::vector<Vec2>{{0, 0}, {100, 0}}, out, 1.5f);
  CHECK(out.size() == 1 + kSplineMaxStepsPerSegment);
  // Degenerate inputs pass through.
  splinePath(std::vector<Vec2>{{1, 2}}, out);
  CHECK(out.size() == 1);
}

TEST_CASE("lines: splinePath output is capped near 6000 points on a dense trace") {
  // 4000 points zig-zagging ±100 px: 400 000 px of arc. Uncapped at 1.5 px that would be ~267k points.
  std::vector<Vec2> in;
  for (int i = 0; i < 4000; ++i) in.push_back({static_cast<float>(i) * 0.5f, i % 2 ? 0.0f : 100.0f});
  std::vector<Vec2> out;
  splinePath(in, out, 1.5f);
  CHECK(out.size() <= static_cast<size_t>(kSplineMaxOutPoints) + in.size());
  CHECK(out.size() >= in.size());
}

TEST_CASE("lines: smoothBand, gradientPolyline, gradientBand geometry") {
  DrawList dl;
  dl.reset(100, 100);
  const Vec2 up[2] = {{10, 40}, {20, 30}};
  const Vec2 dn[2] = {{10, 60}, {20, 70}};
  smoothBand(dl, up, dn, 2, rgba(1, 2, 3, 255));
  auto v = dl.vertices();
  REQUIRE(v.size() == 8);
  CHECK(v[4].pos.y == doctest::Approx(40.0f - kSmoothFeather));
  CHECK(v[6].pos.y == doctest::Approx(70.0f + kSmoothFeather));
  CHECK(alphaOf(v[4].col) == 0);

  dl.reset(100, 100);
  const Vec2 pts[3] = {{0, 10}, {10, 10}, {10, 30}};
  const Rgba cols[3] = {rgba(255, 0, 0), rgba(0, 255, 0), rgba(0, 0, 255)};
  gradientPolyline(dl, pts, cols, 3, 4.0f);
  v = dl.vertices();
  REQUIRE(v.size() == 8);
  CHECK(v[0].pos.y == doctest::Approx(12.0f));  // horizontal segment: ±2 in y
  CHECK(v[1].col == cols[1]);
  CHECK(v[4].pos.x == doctest::Approx(8.0f));   // vertical segment (down): normal (-1, 0) · 2
  CHECK(v[6].col == cols[2]);

  dl.reset(100, 100);
  const Rgba top[2] = {rgba(10, 10, 10, 200), rgba(20, 20, 20, 100)};
  gradientBand(dl, pts, top, 90.0f, 0.5f, 2);
  v = dl.vertices();
  REQUIRE(v.size() == 4);
  CHECK(v[2].pos.y == 90.0f);
  CHECK(alphaOf(v[2].col) == 50);   // bottom alpha = top alpha × bottomScale
  CHECK(alphaOf(v[3].col) == 100);
}

TEST_CASE("lines: splineSmooth passes through the window maxima at the knots") {
  std::vector<float> src(64), dst(64);
  // Flat shelves with steps: 16 at a time.
  for (int i = 0; i < 64; ++i) src[i] = static_cast<float>((i / 16) * 3);
  splineSmooth(src.data(), dst.data(), 64, 8);
  // At knot columns (multiples of 8), the output is the max of the ±4 window.
  for (int k = 0; k * 8 < 64; ++k) {
    const int c = k * 8;
    float m = src[std::max(0, c - 4)];
    for (int i = std::max(0, c - 4); i <= std::min(63, c + 4); ++i) m = std::max(m, src[i]);
    CHECK(dst[c] == doctest::Approx(m));
  }
  // Constant in → constant out; short input copies through.
  std::fill(src.begin(), src.end(), -12.0f);
  splineSmooth(src.data(), dst.data(), 64, 8);
  for (float x : dst) CHECK(x == doctest::Approx(-12.0f));
  src[0] = 5.0f;
  splineSmooth(src.data(), dst.data(), 10, 8);
  CHECK(dst[0] == 5.0f);
}

TEST_CASE("lines: glow passes use the requested blend mode and restore the previous one") {
  DrawList dl;
  dl.reset(200, 100);
  std::vector<Vec2> pts;
  for (int i = 0; i < 50; ++i) pts.push_back({static_cast<float>(i) * 4.0f, 50.0f + 20.0f * std::sin(static_cast<float>(i) * 0.3f)});
  scopeTrace(dl, pts, rgba(120, 220, 160), 1.4f, 0.35f, BlendMode::Additive);
  REQUIRE(dl.commands().size() == 2);
  CHECK(dl.commands()[0].blend == BlendMode::Additive);  // the two glow passes
  CHECK(dl.commands()[1].blend == BlendMode::Alpha);     // the core
  CHECK(dl.blend() == BlendMode::Alpha);
  CHECK(dl.vertices().size() == 3 * 49 * 8);  // 2 glow passes + core

  dl.reset(200, 100);
  std::vector<Rgba> cols(pts.size(), rgba(110, 217, 255));
  std::vector<float> lvl(pts.size(), 0.8f);
  SpectrumCurveStyle st;
  spectrumCurve(dl, pts.data(), cols.data(), lvl.data(), 50, 100.0f, st);
  // fill + 3 bloom + body + core, all quad strips of 4 vertices per segment.
  CHECK(dl.vertices().size() == 6 * 49 * 4);
  CHECK(dl.commands().size() == 1);
  st.glowBlend = BlendMode::Additive;
  dl.reset(200, 100);
  spectrumCurve(dl, pts.data(), cols.data(), lvl.data(), 50, 100.0f, st);
  CHECK(dl.commands().size() == 3);
  CHECK(dl.commands()[1].blend == BlendMode::Additive);
}

TEST_CASE("lines: warm ribbon generation does not allocate") {
  DrawList dl;
  std::vector<Vec2> in, out;
  for (int i = 0; i < 512; ++i) in.push_back({static_cast<float>(i), 50.0f + 30.0f * std::sin(static_cast<float>(i) * 0.05f)});
  auto frame = [&] {
    dl.reset(600, 100);
    splinePath(in, out, 1.5f);
    smoothPolyline(dl, out.data(), static_cast<uint32_t>(out.size()), rgba(255, 255, 255), 1.4f);
    scopeTrace(dl, out, rgba(255, 255, 255), 1.4f, 0.35f);
  };
  frame();
  CHECK(pq::alloc_trap::countRtViolations(frame) == 0);
}

TEST_CASE("color: PQ Meter helpers round as the originals do") {
  CHECK(brighten(rgba(100, 200, 255, 77), 0.5f) == rgba(177, 227, 255, 77));
  CHECK(scaleRgb(rgba(100, 200, 50, 9), 0.5f) == rgba(50, 100, 25, 9));
  CHECK(scaleRgb(rgba(200, 200, 200), 2.0f) == rgba(255, 255, 255));
  CHECK(setAlpha(rgba(1, 2, 3, 40), 0.5f) == rgba(1, 2, 3, 128));
  CHECK(mulAlpha(rgba(1, 2, 3, 200), 0.5f) == rgba(1, 2, 3, 100));
  CHECK(smoothAlphaMul(rgba(1, 2, 3, 200), 0.5f) == rgba(1, 2, 3, 100));
  CHECK(smoothAlphaMul(rgba(1, 2, 3, 200), 3.0f) == rgba(1, 2, 3, 255));
  CHECK(smoothAlphaMul(rgba(1, 2, 3, 200), 0.0f) == rgba(1, 2, 3, 0));
  const Vec2 a{0, 0}, b{1, 1}, c{2, 0}, d{3, 1};
  const Vec2 p0 = catmullRom(a, b, c, d, 0.0f), p1 = catmullRom(a, b, c, d, 1.0f);
  CHECK(p0.x == doctest::Approx(1.0f));
  CHECK(p1.x == doctest::Approx(2.0f));
  CHECK(p1.y == doctest::Approx(0.0f));
}
