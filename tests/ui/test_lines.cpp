// PQ Meter line primitives: geometry of the ribbons (feather, miter clamp, bevels), Catmull-Rom subdivision
// and its output cap, the spectrum helpers, and the colour helpers.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/lines.h"
#include "pq/ui/raster.h"

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
  // VZ.2: shared vertices, 4 per point (fringe, core, core, fringe: the core 4 − 1 px wide, a 1 px fringe to alpha 0).
  REQUIRE(v.size() == 12);
  CHECK(dl.indices().size() == 2 * 18);
  CHECK(v[0].pos.y == doctest::Approx(12.5f));  // first point, horizontal segment: the outer fringe at +2.5 in y
  CHECK(v[1].pos.y == doctest::Approx(11.5f));  // the core's edge at +1.5
  CHECK(alphaOf(v[0].col) == 0);
  CHECK(v[1].col == cols[0]);
  CHECK(v[8].pos.x == doctest::Approx(7.5f));   // last point, vertical segment (down): normal (-1, 0) · 2.5
  CHECK(v[9].col == cols[2]);
  // The corner's mitre: the bisector of (0, 1) and (-1, 0), stretched to 1 / cos 45° (under the 2x cap).
  CHECK(v[4].pos.x == doctest::Approx(10.0f - 2.5f));
  CHECK(v[4].pos.y == doctest::Approx(10.0f + 2.5f));

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
  // VZ.2: the fill (4 vertices per segment) and the core stroke (4 per point); the glow is off by default.
  CHECK(dl.vertices().size() == 49 * 4 + 50 * 4);
  CHECK(dl.commands().size() == 1);
  st.glow = true;  // one glow pass
  dl.reset(200, 100);
  spectrumCurve(dl, pts.data(), cols.data(), lvl.data(), 50, 100.0f, st);
  CHECK(dl.vertices().size() == 49 * 4 + 2 * 50 * 4);
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

// ---- WF.2: the curve displays' entry points --------------------------------------------------------------------------

TEST_CASE("lines (WF.2): strokePolyline is smoothPolyline on a gentle path and bevels / mitres on the segments' own normals "
          "where it turns hard") {
  DrawList dl;
  dl.reset(200, 100);
  std::vector<Vec2> gentle;
  for (int i = 0; i < 40; ++i) gentle.push_back({static_cast<float>(i) * 4.0f, 50.0f + 20.0f * std::sin(static_cast<float>(i) * 0.2f)});
  strokePolyline(dl, gentle, rgba(255, 255, 255), 1.5f);
  DrawList ref;
  ref.reset(200, 100);
  smoothPolyline(ref, gentle, rgba(255, 255, 255), 1.5f);
  REQUIRE(dl.vertices().size() == ref.vertices().size());
  for (size_t k = 0; k < ref.vertices().size(); ++k) {
    CHECK(dl.vertices()[k].pos.x == ref.vertices()[k].pos.x);
    CHECK(dl.vertices()[k].pos.y == ref.vertices()[k].pos.y);
  }
  // A square wave's edge: a long run, a short vertical step, a long run. smoothPolyline takes each vertex's normal
  // from its neighbours' chord, so the corner's outer vertex sits off the true mitre; strokePolyline's sits on it.
  const Vec2 sq[4] = {{10, 80}, {100, 80}, {100, 20}, {190, 20}};
  dl.reset(200, 100);
  strokePolyline(dl, sq, rgba(255, 255, 255), 4.0f);
  // Two 90 degree turns: mitred (under 120 degrees: no bevel), 3 segments of 8 vertices.
  REQUIRE(dl.vertices().size() == 3 * 8);
  // Segment 0's end and segment 1's start share one offset: the true mitre (2, -2) · (half width 2) from the corner.
  const auto v = dl.vertices();
  CHECK(dist(v[1].pos, v[8].pos) < 1e-4f);
  CHECK(dist(v[2].pos, v[8 + 3].pos) < 1e-4f);
  const float d1 = dist(v[1].pos, {100, 80}), d2 = dist(v[2].pos, {100, 80});
  CHECK(d1 == doctest::Approx(2.0f * std::sqrt(2.0f)).epsilon(1e-4));
  CHECK(d2 == doctest::Approx(2.0f * std::sqrt(2.0f)).epsilon(1e-4));
  // A hairpin (a reversal): bevelled, one bevel of 5 vertices.
  const Vec2 pin[3] = {{10, 50}, {100, 52}, {10, 54}};
  dl.reset(200, 100);
  strokePolyline(dl, pin, rgba(255, 255, 255), 2.0f);
  CHECK(dl.vertices().size() == 2 * 8 + 5);
}

TEST_CASE("lines (WF.2): strokeSeries — denser than the pixels, one antialiased column per device px (the cost follows the "
          "width, nothing overlaps itself); sparser, a spline with a point per px") {
  constexpr uint32_t W = 240, H = 80;
  auto rasterOf = [&](const DrawList& dl) {
    std::vector<uint8_t> img(static_cast<size_t>(W) * H, 0);
    rasterizeCoverage(dl, img.data(), W, H, W, {0.0f, 0.0f});
    return img;
  };
  // 6000 points of a noisy slow wave over 200 px (30 per px).
  std::vector<Vec2> dense;
  uint32_t rng = 7u;
  for (int i = 0; i < 6000; ++i) {
    rng = rng * 1664525u + 1013904223u;
    const float x = 20.0f + 200.0f * static_cast<float>(i) / 5999.0f;
    dense.push_back({x, 40.0f + 20.0f * std::sin(x * 0.05f) + 4.0f * (static_cast<float>(rng >> 8) / 8388608.0f - 1.0f)});
  }
  DrawList dl;
  dl.reset(W, H);
  strokeSeries(dl, dense, rgba(255, 255, 255, 128), 1.0f);
  CHECK(dl.vertices().size() == 4 * (200 + 2));  // four vertices a column and the run's two ends: not 8 per point
  for (const Vertex& v : dl.vertices()) CHECK(alphaOf(v.col) <= 128);
  auto img = rasterOf(dl);
  for (uint32_t x = 21; x < 219; ++x) {
    // The column covers the series' range inside it (about 8 px of noise).
    float lo = 1e9f, hi = -1e9f;
    for (const Vec2& q : dense)
      if (q.x >= static_cast<float>(x) && q.x < static_cast<float>(x + 1)) lo = std::min(lo, q.y), hi = std::max(hi, q.y);
    REQUIRE(hi > lo);
    const auto mid = static_cast<uint32_t>(0.5f * (lo + hi));
    CAPTURE(x);
    CHECK(std::abs(static_cast<int>(img[mid * W + x]) - 128) <= 1);  // the colour's alpha, once: nothing overlaps
    CHECK(img[static_cast<uint32_t>(lo - 3.0f) * W + x] == 0);
    CHECK(img[static_cast<uint32_t>(hi + 4.0f) * W + x] == 0);
  }
  // A dense but smooth series (a shallow ramp, 10 points per px): the two points a line through it needs, one ribbon —
  // at full strength in every column, the weight of the shared line, wherever it lies between two rows.
  std::vector<Vec2> ramp;
  for (int i = 0; i <= 2000; ++i) ramp.push_back({20.0f + static_cast<float>(i) * 0.1f, 30.3f + static_cast<float>(i) * 0.01f});
  dl.reset(W, H);
  strokeSeries(dl, ramp, rgba(255, 255, 255), 1.0f);
  CHECK(dl.vertices().size() == 8);  // one segment
  img = rasterOf(dl);
  for (uint32_t x = 22; x < 218; ++x) {
    int peak = 0;
    double ink = 0.0;
    for (uint32_t y = 0; y < H; ++y) peak = std::max<int>(peak, img[y * W + x]), ink += img[y * W + x] / 255.0;
    CAPTURE(x);
    CHECK(peak >= 250);
    CHECK(ink >= 1.6);  // (the ribbon's 1 + 0.75 px, point-sampled at the row centres: 1.67 on a row, 2.0 between two)
    CHECK(ink <= 2.05);
  }
  // 12 points over 220 px (20 px apart): a spline through them, a point per px at least, passing through every point.
  std::vector<Vec2> sparse;
  for (int i = 0; i < 12; ++i) sparse.push_back({10.0f + 20.0f * static_cast<float>(i), 40.0f + (i % 2 ? 15.0f : -15.0f)});
  dl.reset(W, H);
  strokeSeries(dl, sparse, rgba(255, 255, 255), 1.5f);
  CHECK(dl.vertices().size() >= 8 * 220);
  img = rasterOf(dl);
  for (size_t i = 0; i + 1 < sparse.size(); ++i)  // (the last point is the stroke's butt end: its pixel's centre lies past it)
    CHECK(img[static_cast<uint32_t>(sparse[i].y) * W + static_cast<uint32_t>(sparse[i].x)] >= 200);
  // Nothing to draw: nothing drawn.
  dl.reset(W, H);
  strokeSeries(dl, std::span<const Vec2>(sparse.data(), 1), rgba(255, 255, 255), 1.0f);
  const Vec2 same[2] = {{5, 5}, {5, 9}};
  strokeSeries(dl, same, rgba(255, 255, 255), 1.0f);
  CHECK(dl.vertices().empty());
}

TEST_CASE("lines (WF.2): curveSamples is a sample per device px within its bounds; curveParams adds the knots") {
  CHECK(curveSamples(0.0f) == 16);
  CHECK(curveSamples(100.0f) == 100);
  CHECK(curveSamples(100.2f) == 101);
  CHECK(curveSamples(1.0e9f) == 4096);
  CHECK(curveSamples(300.0f, 64, 256) == 256);
  std::vector<double> t;
  const double knots[] = {0.5, 0.3, 0.0, 1.0, 0.2500001};
  curveParams(t, 4, knots);
  REQUIRE(t.size() == 6);
  CHECK(t[0] == 0.0);
  CHECK(t[1] == doctest::Approx(0.25));
  CHECK(t[2] == 0.3);
  CHECK(t[3] == 0.5);
  CHECK(t[4] == 0.75);
  CHECK(t[5] == 1.0);
  curveParams(t, 3, {});
  CHECK(t.size() == 4);
}

TEST_CASE("face painter (WF.2): a dash is one ribbon through the polyline's vertices inside it; series and polylineCols map "
          "world units") {
  DrawList dl;
  dl.reset(400, 400);
  FacePainter p(dl, FaceTransform{{10.0f, 20.0f}, 2.0f});
  // A right angle 6 units into a 20 unit dash: both pieces of the dash share the corner's vertices (they were two
  // butt-ended ribbons that overlapped inside the corner and left a notch outside it).
  const Vec2 corner[3] = {{0, 0}, {6, 0}, {6, 10}};
  p.dashed(corner, 2.0f, 20.0f, 5.0f, rgba(255, 255, 255));
  REQUIRE(dl.vertices().size() == 2 * 8);
  const auto v = dl.vertices();
  CHECK(dist(v[1].pos, v[8].pos) < 1e-4f);
  CHECK(dist(v[2].pos, v[8 + 3].pos) < 1e-4f);
  // Dashes of 4 on, 4 off along 40 straight units cut into 40 segments: 5 ribbons of 4 segments.
  std::vector<Vec2> line;
  for (int i = 0; i <= 40; ++i) line.push_back({static_cast<float>(i), 50.0f});
  dl.reset(400, 400);
  p.dashed(line, 1.0f, 4.0f, 4.0f, rgba(255, 255, 255));
  CHECK(dl.vertices().size() == 5 * 4 * 8);
  // Consecutive segments of one dash share their joint.
  CHECK(dist(dl.vertices()[1].pos, dl.vertices()[8].pos) < 1e-4f);
  // series: world -> device (origin + world · scale); a column per device px when dense.
  std::vector<Vec2> s;
  for (int i = 0; i <= 1000; ++i) s.push_back({static_cast<float>(i) * 0.05f, 30.0f + std::sin(static_cast<float>(i) * 0.7f)});
  dl.reset(400, 400);
  p.series(s, 1.0f, rgba(255, 255, 255));
  CHECK(dl.vertices().size() == 4 * (100 + 2));  // 50 units = 100 device px from x = 10 (a wave of 0.9 px per cycle)
  float x0 = 1e9f, x1 = -1e9f;
  for (const Vertex& q : dl.vertices()) x0 = std::min(x0, q.pos.x), x1 = std::max(x1, q.pos.x);
  CHECK(x0 == 10.0f);
  CHECK(x1 == 110.0f);
  // polylineCols: one joined stroke, a colour per point.
  const Vec2 pc[3] = {{0, 0}, {10, 0}, {20, 0}};
  const Rgba cc[3] = {rgba(255, 0, 0), rgba(0, 255, 0), rgba(0, 0, 255, 100)};
  dl.reset(400, 400);
  p.polylineCols(pc, cc, 2.0f);
  REQUIRE(dl.vertices().size() == 2 * 8);
  CHECK(dl.vertices()[0].col == rgba(255, 0, 0));
  CHECK(dl.vertices()[1].col == rgba(0, 255, 0));
  CHECK(dl.vertices()[8 + 1].col == rgba(0, 0, 255, 100));
  CHECK(dl.vertices()[0].pos.x == 10.0f);
  CHECK(dl.vertices()[1].pos.x == 30.0f);
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
