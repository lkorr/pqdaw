// T0.7-fix2: geometry of the widget-grade draw-list primitives (CPU only; the pixel checks are in
// test_render.cpp).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "pq/ui/draw_list.h"

using namespace pq::ui;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

void checkIndicesValid(const DrawList& dl) {
  uint32_t next = 0;
  for (const DrawCmd& c : dl.commands()) {
    CHECK(c.idxOffset == next);
    next += c.idxCount;
  }
  CHECK(next == dl.indices().size());
  CHECK(dl.indices().size() % 3 == 0);
  for (uint32_t i : dl.indices()) REQUIRE(i < dl.vertices().size());
}

// Radius scale that gives an n-gon the circle's area (the implementation's tessellation correction).
float areaScale(uint32_t n) {
  const double nn = static_cast<double>(n);
  return static_cast<float>(std::sqrt(2.0 * std::numbers::pi / (nn * std::sin(2.0 * std::numbers::pi / nn))));
}

// Worst radial distance between a circle of radius R and a polygon edge of angular step `da` whose vertices
// sit at k·R: the vertices bulge out by (k − 1)R, the chord's midpoint sits at k·R·cos(da/2).
float chordError(float R, float k, float da) {
  return std::max(std::fabs(k - 1.0f) * R, std::fabs(1.0f - k * std::cos(0.5f * da)) * R);
}

float minX(const DrawList& dl) {
  float m = 1e9f;
  for (const Vertex& v : dl.vertices()) m = std::min(m, v.pos.x);
  return m;
}
float maxX(const DrawList& dl) {
  float m = -1e9f;
  for (const Vertex& v : dl.vertices()) m = std::max(m, v.pos.x);
  return m;
}
float minY(const DrawList& dl) {
  float m = 1e9f;
  for (const Vertex& v : dl.vertices()) m = std::min(m, v.pos.y);
  return m;
}
float maxY(const DrawList& dl) {
  float m = -1e9f;
  for (const Vertex& v : dl.vertices()) m = std::max(m, v.pos.y);
  return m;
}

}  // namespace

TEST_CASE("primitives: snapThickness rounds to whole device pixels, at least 1") {
  CHECK(DrawList::snapThickness(0.2f) == 1.0f);
  CHECK(DrawList::snapThickness(1.0f) == 1.0f);
  CHECK(DrawList::snapThickness(1.49f) == 1.0f);
  CHECK(DrawList::snapThickness(1.5f) == 2.0f);
  CHECK(DrawList::snapThickness(2.6f) == 3.0f);
}

TEST_CASE("primitives: hline/vline are one snapped solid quad") {
  DrawList dl;
  dl.reset(100, 100);
  // Integer and half-pixel y both select row 10 for a 1 px line; ends round to whole pixels.
  for (float y : {10.0f, 10.5f, 10.3f}) {
    dl.reset(100, 100);
    dl.addHLine(4.4f, 20.6f, y, rgba(255, 255, 255));
    REQUIRE(dl.vertices().size() == 4);
    REQUIRE(dl.indices().size() == 6);
    CAPTURE(y);
    CHECK(minY(dl) == 10.0f);
    CHECK(maxY(dl) == 11.0f);
    CHECK(minX(dl) == 4.0f);
    CHECK(maxX(dl) == 21.0f);
  }
  // 2 px centred on the pixel boundary at 10: rows 9 and 10. 2.4 px snaps to 2.
  dl.reset(100, 100);
  dl.addVLine(10.0f, 30.0f, 5.0f, rgba(255, 255, 255), 2.4f);  // swapped ends accepted
  CHECK(minX(dl) == 9.0f);
  CHECK(maxX(dl) == 11.0f);
  CHECK(minY(dl) == 5.0f);
  CHECK(maxY(dl) == 30.0f);
  for (const Vertex& v : dl.vertices()) CHECK(v.col == rgba(255, 255, 255));
  // Zero length after snapping draws nothing.
  dl.reset(100, 100);
  dl.addHLine(3.2f, 3.4f, 5, rgba(255, 255, 255));
  CHECK(dl.empty());
}

TEST_CASE("primitives: rect stroke is four non-overlapping trapezoids inside the snapped rect") {
  DrawList dl;
  dl.reset(100, 100);
  dl.addRectStroke({10.2f, 20.4f, 50.6f, 40.0f}, rgba(1, 2, 3), 2.0f);
  REQUIRE(dl.vertices().size() == 8);
  REQUIRE(dl.indices().size() == 24);
  checkIndicesValid(dl);
  CHECK(minX(dl) == 10.0f);
  CHECK(maxX(dl) == 51.0f);
  CHECK(minY(dl) == 20.0f);
  CHECK(maxY(dl) == 40.0f);
  // Inner corners at exactly t inside.
  const auto v = dl.vertices();
  CHECK(v[4].pos.x == 12.0f);
  CHECK(v[4].pos.y == 22.0f);
  CHECK(v[6].pos.x == 49.0f);
  CHECK(v[6].pos.y == 38.0f);
  // Total triangle area equals the frame area exactly (no overlap, no gap).
  double area = 0.0;
  for (uint32_t i = 0; i < dl.indices().size(); i += 3) {
    const Vec2 a = v[dl.indices()[i]].pos, b = v[dl.indices()[i + 1]].pos, c = v[dl.indices()[i + 2]].pos;
    area += 0.5 * std::fabs(static_cast<double>((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)));
  }
  CHECK(area == doctest::Approx(41.0 * 20.0 - 37.0 * 16.0));
  // A stroke that fills the rect becomes the rect.
  dl.reset(100, 100);
  dl.addRectStroke({0, 0, 4, 10}, rgba(1, 2, 3), 2.0f);
  CHECK(dl.vertices().size() == 4);
}

TEST_CASE("primitives: rounded rect stroke has crisp straight sides and AA corners") {
  DrawList dl;
  dl.reset(200, 200);
  const float radius = 6.0f;
  dl.addRoundedRectStroke({10.3f, 20.0f, 90.0f, 60.4f}, radius, rgba(200, 100, 50), 1.0f);
  const uint32_t q = DrawList::cornerSegments(radius);
  const uint32_t pts = 4 * (q + 1);
  REQUIRE(dl.vertices().size() == 4 * pts);
  REQUIRE(dl.indices().size() == 18 * pts);
  checkIndicesValid(dl);
  // Snapped rect is {10, 20, 90, 60}. Every cross-section: feather ±0.5 around the outer edge and the inner
  // edge (1 px inside). On the top side the four vertices sit at y = 19.5, 20.5, 20.5, 21.5.
  bool sawTop = false;
  for (uint32_t p = 0; p < pts; ++p) {
    const Vertex* s = &dl.vertices()[4 * p];
    CHECK(alphaOf(s[0].col) == 0);
    CHECK(s[1].col == rgba(200, 100, 50));
    CHECK(s[2].col == rgba(200, 100, 50));
    CHECK(alphaOf(s[3].col) == 0);
    if (s[1].pos.y == 20.5f && s[1].pos.x > 10 + radius - 0.01f && s[1].pos.x < 90 - radius + 0.01f) {
      sawTop = true;
      CHECK(s[0].pos.y == 19.5f);
      CHECK(s[2].pos.y == 20.5f);
      CHECK(s[3].pos.y == 21.5f);
    }
    for (int e = 0; e < 4; ++e) {
      CHECK(s[e].pos.x >= 9.49f);
      CHECK(s[e].pos.x <= 90.51f);
      CHECK(s[e].pos.y >= 19.49f);
      CHECK(s[e].pos.y <= 60.51f);
    }
  }
  CHECK(sawTop);
  // Radius below half a pixel is the crisp rect stroke; a stroke that fills the rect is a filled rounded rect.
  dl.reset(200, 200);
  dl.addRoundedRectStroke({10, 10, 50, 50}, 0.3f, rgba(1, 1, 1), 1.0f);
  CHECK(dl.vertices().size() == 8);
  dl.reset(200, 200);
  dl.addRoundedRectStroke({10, 10, 14, 50}, 2.0f, rgba(1, 1, 1), 2.0f);
  CHECK(dl.vertices().size() == 2 * 4 * (DrawList::cornerSegments(2.0f) + 1) - 4);  // pill: duplicates dropped
}

TEST_CASE("primitives: triangles and convex polygons feather outwards for either winding") {
  for (int winding = 0; winding < 2; ++winding) {
    DrawList dl;
    dl.reset(100, 100);
    Vec2 a{10, 10}, b{40, 10}, c{25, 40};
    if (winding) std::swap(b, c);
    dl.addTriangleFilled(a, b, c, rgba(255, 255, 255));
    REQUIRE(dl.vertices().size() == 6);
    REQUIRE(dl.indices().size() == 3 + 18);
    checkIndicesValid(dl);
    const Vec2 g{25, 20};  // centroid
    for (uint32_t i = 0; i < 3; ++i) {
      const Vertex& in = dl.vertices()[2 * i];
      const Vertex& out = dl.vertices()[2 * i + 1];
      CAPTURE(winding);
      CHECK(alphaOf(in.col) == 255);
      CHECK(alphaOf(out.col) == 0);
      const float din = std::hypot(in.pos.x - g.x, in.pos.y - g.y), dout = std::hypot(out.pos.x - g.x, out.pos.y - g.y);
      CHECK(dout > din);
    }
  }
  DrawList dl;
  dl.reset(100, 100);
  std::vector<Vec2> hexagon;
  for (int i = 0; i < 6; ++i) hexagon.push_back({50 + 10 * std::cos(-kPi / 3 * static_cast<float>(i)), 50 + 10 * std::sin(-kPi / 3 * static_cast<float>(i))});
  dl.addConvexPolyFilled(hexagon, rgba(9, 9, 9));  // counter-clockwise on screen
  REQUIRE(dl.vertices().size() == 12);
  for (uint32_t i = 0; i < 6; ++i) {
    CHECK(std::hypot(dl.vertices()[2 * i].pos.x - 50, dl.vertices()[2 * i].pos.y - 50) < 10.0f);
    CHECK(std::hypot(dl.vertices()[2 * i + 1].pos.x - 50, dl.vertices()[2 * i + 1].pos.y - 50) > 10.0f);
  }
  // Degenerate input draws nothing.
  dl.reset(100, 100);
  dl.addTriangleFilled({0, 0}, {5, 5}, {10, 10}, rgba(1, 1, 1));
  const Vec2 two[2] = {{0, 0}, {1, 1}};
  dl.addConvexPolyFilled(two, rgba(1, 1, 1));
  CHECK(dl.empty());
}

TEST_CASE("primitives: filled circle ring radii and chord error") {
  DrawList dl;
  for (float r : {1.5f, 4.0f, 11.0f, 40.0f, 300.0f}) {
    dl.reset(100, 100);
    const Vec2 c{50.25f, 40.5f};
    dl.addCircleFilled(c, r, rgba(255, 0, 0));
    const uint32_t n = DrawList::circleSegments(r);
    REQUIRE(dl.vertices().size() == 2 * n + 1);
    REQUIRE(dl.indices().size() == 9 * n);
    checkIndicesValid(dl);
    CAPTURE(r);
    const float ks = areaScale(n);
    for (uint32_t k = 0; k < n; ++k) {
      const Vertex& core = dl.vertices()[1 + 2 * k];
      const Vertex& fea = dl.vertices()[2 + 2 * k];
      CHECK(std::hypot(core.pos.x - c.x, core.pos.y - c.y) == doctest::Approx((r - 0.5f) * ks).epsilon(1e-4));
      CHECK(std::hypot(fea.pos.x - c.x, fea.pos.y - c.y) == doctest::Approx((r + 0.5f) * ks).epsilon(1e-4));
      // Radial error of the edge polygon against the true circle, between consecutive points.
      const Vertex& next = dl.vertices()[1 + 2 * ((k + 1) % n)];
      const float a0 = std::atan2(core.pos.y - c.y, core.pos.x - c.x), a1 = std::atan2(next.pos.y - c.y, next.pos.x - c.x);
      float da = std::fabs(a1 - a0);
      if (da > kPi) da = 2 * kPi - da;
      CHECK(chordError(r, ks, da) <= 0.2001f);
    }
  }
}

TEST_CASE("primitives: circle tessellation keeps the circle's area") {
  // Area of the n-gon through the scaled vertices equals π r² (to float precision), for every segment count.
  for (float r : {0.3f, 1.0f, 2.5f, 7.0f, 30.0f, 500.0f}) {
    const uint32_t n = DrawList::circleSegments(r);
    const double k = areaScale(n);
    const double polyArea = 0.5 * n * (k * r) * (k * r) * std::sin(2.0 * std::numbers::pi / n);
    CHECK(polyArea == doctest::Approx(std::numbers::pi * r * r).epsilon(1e-5));
  }
}

TEST_CASE("primitives: arc tessellation chord error <= 0.2 px, exact ends, round caps") {
  // Round caps leave the end cross-sections unshifted, so every section's outer core vertex lies on
  // radius r + t/2 − 0.5 at its own angle. Sections come first, in angle order.
  DrawList dl;
  int arcs = 0;
  for (float r : {3.0f, 8.0f, 17.0f, 45.0f, 160.0f}) {
    for (float th : {1.0f, 3.0f, 6.0f}) {
      for (float a0 : {-2.0f, 0.0f, 0.3f, 2.356f}) {
        for (float sweep : {0.05f, 0.7f, 1.5f * kPi, 2.0f * kPi - 0.01f}) {
          dl.reset(400, 400);
          const Vec2 c{200.3f, 199.6f};
          const float a1 = a0 + sweep;
          dl.addArcStroke(c, r, a1, a0, rgba(10, 200, 30), th, LineCap::Round);  // reversed order is accepted
          checkIndicesValid(dl);
          const float rOuter = r + 0.5f * th;
          const float ks = areaScale(DrawList::circleSegments(rOuter));
          const float rCore = (rOuter - 0.5f) * ks;
          // Count sections: consecutive groups of 4 whose [0] and [3] are transparent and [1] lies on rCore.
          std::vector<float> angles;
          for (uint32_t i = 0; i + 3 < dl.vertices().size(); i += 4) {
            const Vertex* s = &dl.vertices()[i];
            if (alphaOf(s[0].col) != 0 || alphaOf(s[1].col) == 0 || alphaOf(s[3].col) != 0) break;
            if (std::fabs(std::hypot(s[1].pos.x - c.x, s[1].pos.y - c.y) - rCore) > 1e-3f * rOuter) break;
            angles.push_back(std::atan2(s[1].pos.y - c.y, s[1].pos.x - c.x));
          }
          CAPTURE(r);
          CAPTURE(th);
          CAPTURE(a0);
          CAPTURE(sweep);
          REQUIRE(angles.size() >= 2);
          // Unwrap and check: starts at a0, ends at a1, monotone, each step's sag at the outer edge ≤ 0.2 px.
          float prev = angles[0], acc = 0.0f;
          const float start = std::remainder(angles[0] - a0, 2 * kPi);
          CHECK(std::fabs(start) < 1e-3f);
          for (size_t i = 1; i < angles.size(); ++i) {
            const float step = std::remainder(angles[i] - prev, 2 * kPi);
            CHECK(step > 0.0f);
            CHECK(chordError(rOuter, ks, step) <= 0.2001f);
            acc += step;
            prev = angles[i];
          }
          CHECK(acc == doctest::Approx(sweep).epsilon(1e-3));
          ++arcs;
        }
      }
    }
  }
  CHECK(arcs == 5 * 3 * 4 * 4);
}

TEST_CASE("primitives: butt caps feather the end over 1 px; sub-pixel rings lower alpha instead") {
  DrawList dl;
  dl.reset(200, 200);
  const Vec2 c{100, 100};
  dl.addArcStroke(c, 20, 0.0f, kPi * 0.5f, rgba(255, 255, 255), 4.0f, LineCap::Butt);
  checkIndicesValid(dl);
  // First section is pulled 0.5 px into the arc (towards +y at angle 0); the last 4 vertices of the first
  // cap sit 0.5 px beyond the end, transparent.
  const Vertex* first = &dl.vertices()[0];
  CHECK(first[1].pos.y == doctest::Approx(100.5f));
  CHECK(first[1].pos.x == doctest::Approx(100.0f + 21.5f * areaScale(DrawList::circleSegments(22.0f))));
  const auto v = dl.vertices();
  const Vertex* cap0 = &v[v.size() - 8];
  for (int e = 0; e < 4; ++e) {
    CHECK(alphaOf(cap0[e].col) == 0);
    CHECK(cap0[e].pos.y == doctest::Approx(99.5f));
  }

  // 0.5 px ring: core collapses onto the radius with alpha 2t/(1+t) = 2/3.
  dl.reset(200, 200);
  dl.addCircleStroke(c, 10, rgba(255, 255, 255, 255), 0.5f);
  checkIndicesValid(dl);
  const Vertex* s = &dl.vertices()[0];
  CHECK(alphaOf(s[1].col) == 170);
  const float ks = areaScale(DrawList::circleSegments(10.25f));
  CHECK(std::hypot(s[1].pos.x - c.x, s[1].pos.y - c.y) == doctest::Approx(10.0f * ks));
  CHECK(std::hypot(s[0].pos.x - c.x, s[0].pos.y - c.y) == doctest::Approx(10.75f * ks));
  CHECK(std::hypot(s[3].pos.x - c.x, s[3].pos.y - c.y) == doctest::Approx(9.25f * ks));
  CHECK(dl.vertices().size() == 4 * DrawList::circleSegments(10.25f));

  // Sweeps of 2π or more draw a closed ring (no caps); zero sweep and NaN draw nothing.
  dl.reset(200, 200);
  dl.addArcStroke(c, 10, 1.0f, 1.0f + 7.0f, rgba(255, 255, 255), 2.0f, LineCap::Round);
  CHECK(dl.vertices().size() == 4 * DrawList::circleSegments(11.0f));
  dl.reset(200, 200);
  dl.addArcStroke(c, 10, 1.0f, 1.0f, rgba(255, 255, 255));
  dl.addArcStroke(c, 10, 0.0f, std::numeric_limits<float>::quiet_NaN(), rgba(255, 255, 255));
  CHECK(dl.empty());
}

TEST_CASE("primitives: 4-corner gradient is a single quad when linear, a centre-fan grid otherwise") {
  DrawList dl;
  dl.reset(200, 200);
  const Rgba a = rgba(10, 20, 30, 255), b = rgba(200, 100, 0, 128);
  dl.addRectFilledMultiColor({0, 0, 100, 50}, a, b, b, a);  // horizontal: twist 0
  CHECK(dl.vertices().size() == 4);
  dl.reset(200, 200);
  dl.addRectFilledMultiColor({0, 0, 100, 50}, a, a, b, b);  // vertical: twist 0
  CHECK(dl.vertices().size() == 4);

  // Opposite corners: twist = 2·190 = 380 in red → N = ceil(sqrt(380/16)) = 5.
  const Rgba tl = rgba(200, 0, 0), tr = rgba(10, 0, 0), br = rgba(200, 0, 0), bl = rgba(10, 0, 0);
  dl.reset(200, 200);
  const Rect r{10, 20, 110, 70};
  dl.addRectFilledMultiColor(r, tl, tr, br, bl);
  const uint32_t n = 5;
  REQUIRE(dl.vertices().size() == (n + 1) * (n + 1) + n * n);
  REQUIRE(dl.indices().size() == 12 * n * n);
  checkIndicesValid(dl);
  // Every vertex carries the exact bilinear colour (± rounding).
  for (const Vertex& v : dl.vertices()) {
    const float u = (v.pos.x - r.x0) / r.width(), w = (v.pos.y - r.y0) / r.height();
    const float top = 200 + (10 - 200) * u, bot = 10 + (200 - 10) * u;
    const float want = top + (bot - top) * w;
    CHECK(std::fabs(static_cast<float>(redOf(v.col)) - want) <= 0.5f + 1e-3f);
  }
  CHECK(minX(dl) == r.x0);
  CHECK(maxX(dl) == r.x1);
  CHECK(minY(dl) == r.y0);
  CHECK(maxY(dl) == r.y1);
}

TEST_CASE("primitives: steady-state frames do not reallocate") {
  DrawList dl;
  auto build = [&] {
    dl.reset(800, 600);
    for (int i = 0; i < 200; ++i) {
      const float x = static_cast<float>(i % 20) * 40.0f, y = static_cast<float>(i / 20) * 40.0f;
      dl.addArcStroke({x + 20, y + 20}, 15, 2.3f, 7.0f, 0xFF00FFFFu, 3, LineCap::Round);
      dl.addCircleFilled({x + 20, y + 20}, 9, 0xFF808080u);
      dl.addRoundedRectStroke({x, y, x + 38, y + 38}, 4, 0xFF000000u);
      dl.addRectFilledMultiColor({x, y, x + 10, y + 10}, 0xFF0000FFu, 0xFF00FF00u, 0xFF0000FFu, 0xFFFF0000u);
    }
  };
  build();
  const Vertex* v = dl.vertices().data();
  const uint32_t* idx = dl.indices().data();
  build();
  CHECK(dl.vertices().data() == v);
  CHECK(dl.indices().data() == idx);
}
