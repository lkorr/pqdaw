#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"

using namespace pq::ui;

TEST_CASE("draw list: vertex layout is ImDrawVert-compatible") {
  CHECK(sizeof(Vertex) == 20);
  CHECK(offsetof(Vertex, pos) == 0);
  CHECK(offsetof(Vertex, uv) == 8);
  CHECK(offsetof(Vertex, col) == 16);
  // IM_COL32 byte order: R in the low byte, so bytes in memory are R, G, B, A (VK_FORMAT_R8G8B8A8_UNORM).
  const Rgba c = rgba(1, 2, 3, 4);
  const auto* b = reinterpret_cast<const uint8_t*>(&c);
  CHECK(b[0] == 1);
  CHECK(b[1] == 2);
  CHECK(b[2] == 3);
  CHECK(b[3] == 4);
  CHECK(hex(0x78BEFF) == rgba(120, 190, 255, 255));
}

TEST_CASE("draw list: filled rect is one quad sampling the white texel") {
  DrawList dl;
  dl.reset(100, 50);
  dl.addRectFilled({10, 20, 30, 40}, rgba(255, 0, 0));
  REQUIRE(dl.vertices().size() == 4);
  REQUIRE(dl.indices().size() == 6);
  REQUIRE(dl.commands().size() == 1);
  const DrawCmd& c = dl.commands()[0];
  CHECK(c.idxOffset == 0);
  CHECK(c.idxCount == 6);
  CHECK(c.texture == kAtlasTexture);
  CHECK(c.blend == BlendMode::Alpha);
  CHECK(c.clip == ClipRect{0, 0, 100, 50});
  const Vec2 white = baseAtlasUv().white;
  for (const Vertex& v : dl.vertices()) {
    CHECK(v.uv.x == white.x);
    CHECK(v.uv.y == white.y);
    CHECK(v.col == rgba(255, 0, 0));
  }
  CHECK(dl.vertices()[0].pos.x == 10);
  CHECK(dl.vertices()[2].pos.y == 40);

  dl.addRectFilled({5, 5, 5, 9}, rgba(0, 0, 0));  // empty: ignored
  CHECK(dl.vertices().size() == 4);
}

TEST_CASE("draw list: commands split only on state changes") {
  DrawList dl;
  dl.reset(64, 64);
  dl.addRectFilled({0, 0, 1, 1}, 0xFFFFFFFFu);
  dl.addRectFilled({1, 1, 2, 2}, 0xFFFFFFFFu);
  CHECK(dl.commands().size() == 1);

  dl.setBlend(BlendMode::Additive);
  dl.addBlob({10, 10}, 5, 0xFFFFFFFFu);
  dl.addBlob({20, 10}, 5, 0xFFFFFFFFu);
  REQUIRE(dl.commands().size() == 2);
  CHECK(dl.commands()[1].blend == BlendMode::Additive);
  CHECK(dl.commands()[1].idxOffset == 12);
  CHECK(dl.commands()[1].idxCount == 12);

  // A state flip with nothing drawn in between does not leave an empty command behind.
  dl.setBlend(BlendMode::Alpha);
  dl.setTexture(7);
  dl.setTexture(kAtlasTexture);
  dl.setBlend(BlendMode::Additive);
  dl.addBlob({30, 10}, 5, 0xFFFFFFFFu);
  CHECK(dl.commands().size() == 2);

  dl.setBlend(BlendMode::Alpha);
  dl.pushClip({8, 8, 16.5f, 16.5f});
  dl.addRectFilled({0, 0, 64, 64}, 0xFFFFFFFFu);
  dl.popClip();
  REQUIRE(dl.commands().size() == 3);
  CHECK(dl.commands()[2].clip == ClipRect{8, 8, 17, 17});

  // Every index lands inside its command's range and the ranges tile the index buffer.
  uint32_t next = 0;
  for (const DrawCmd& c : dl.commands()) {
    CHECK(c.idxOffset == next);
    next += c.idxCount;
  }
  CHECK(next == dl.indices().size());
  for (uint32_t i : dl.indices()) CHECK(i < dl.vertices().size());
}

TEST_CASE("draw list: images switch texture for the quad and restore it") {
  DrawList dl;
  dl.reset(64, 64);
  dl.addRectFilled({0, 0, 4, 4}, 0xFFFFFFFFu);
  dl.addImage(3, {0, 0, 8, 8}, {0, 0}, {1, 1});
  dl.addImage(3, {8, 0, 16, 8}, {0, 0}, {1, 1});
  dl.addRectFilled({0, 0, 4, 4}, 0xFFFFFFFFu);
  REQUIRE(dl.commands().size() == 3);
  CHECK(dl.commands()[1].texture == 3);
  CHECK(dl.commands()[1].idxCount == 12);  // consecutive images on one texture merge
  CHECK(dl.commands()[2].texture == kAtlasTexture);
  CHECK(dl.texture() == kAtlasTexture);
  const Vertex& v = dl.vertices()[5];  // second vertex of the first image: (x1, y0) with uv (1, 0)
  CHECK(v.pos.x == 8);
  CHECK(v.uv.x == 1);
  CHECK(v.uv.y == 0);
}

TEST_CASE("draw list: clip stack intersects and stays balanced past its depth limit") {
  DrawList dl;
  dl.reset(100, 100);
  dl.pushClip({10, 10, 90, 90});
  dl.pushClip({50, -20, 200, 60});
  CHECK(dl.clip() == ClipRect{50, 10, 90, 60});
  dl.pushClip({0, 70, 10, 80});  // disjoint: empty clip, never negative size
  CHECK(dl.clip().x1 >= dl.clip().x0);
  CHECK(dl.clip().y1 >= dl.clip().y0);
  dl.popClip();
  dl.popClip();
  CHECK(dl.clip() == ClipRect{10, 10, 90, 90});
  dl.popClip();
  CHECK(dl.clip() == ClipRect{0, 0, 100, 100});
}

TEST_CASE("draw list: low-level prim writer (PQ Meter port target)") {
  DrawList dl;
  dl.reset(32, 32);
  dl.addRectFilled({0, 0, 1, 1}, 0xFFFFFFFFu);
  dl.primReserve(3, 3);
  const uint32_t base = dl.vtxCurrentIdx();
  CHECK(base == 4);
  dl.primWriteVtx({0, 0}, dl.whiteUv(), 1);
  dl.primWriteVtx({1, 0}, dl.whiteUv(), 2);
  dl.primWriteVtx({0, 1}, dl.whiteUv(), 3);
  dl.primWriteIdx(base);
  dl.primWriteIdx(base + 1);
  dl.primWriteIdx(base + 2);
  CHECK(dl.vtxCurrentIdx() == 7);
  CHECK(dl.vertices().size() == 7);
  CHECK(dl.indices().size() == 9);
  CHECK(dl.commands().size() == 1);
  CHECK(dl.indices()[8] == 6);
  CHECK(dl.vertices()[6].col == 3u);
}

TEST_CASE("draw list: rounded rect geometry") {
  DrawList dl;
  dl.reset(400, 400);
  const Rect r{10.25f, 20.5f, 110.25f, 70.5f};
  const float radius = 12.0f;
  dl.addRoundedRectFilled(r, radius, rgba(10, 20, 30, 200));
  const uint32_t seg = DrawList::cornerSegments(radius);
  const uint32_t n = 4 * (seg + 1);  // outline points
  REQUIRE(dl.vertices().size() == 2 * n);
  CHECK(dl.indices().size() == 3 * (n - 2) + 6 * n);
  for (uint32_t i = 0; i < n; ++i) {
    const Vertex& in = dl.vertices()[2 * i];
    const Vertex& out = dl.vertices()[2 * i + 1];
    CHECK(in.col == rgba(10, 20, 30, 200));
    CHECK(alphaOf(out.col) == 0);
    CHECK((out.col & 0xFFFFFFu) == (in.col & 0xFFFFFFu));
    // Inner ring 0.5 px inside the shape, outer ring 0.5 px outside (≤ 0.5·√2 at the corners).
    CHECK(in.pos.x >= r.x0 + 0.49f);
    CHECK(in.pos.x <= r.x1 - 0.49f);
    CHECK(in.pos.y >= r.y0 + 0.49f);
    CHECK(in.pos.y <= r.y1 - 0.49f);
    CHECK(out.pos.x >= r.x0 - 0.71f);
    CHECK(out.pos.x <= r.x1 + 0.71f);
    CHECK(out.pos.y >= r.y0 - 0.71f);
    CHECK(out.pos.y <= r.y1 + 0.71f);
  }
  // The straight top edge sits exactly 0.5 px inside / outside.
  bool sawTop = false;
  for (uint32_t i = 0; i < n; ++i) {
    const Vertex& in = dl.vertices()[2 * i];
    if (std::fabs(in.pos.y - (r.y0 + 0.5f)) < 1e-4f && in.pos.x > r.x0 + radius - 0.01f && in.pos.x < r.x1 - radius + 0.01f) {
      sawTop = true;
      CHECK(dl.vertices()[2 * i + 1].pos.y == doctest::Approx(r.y0 - 0.5f));
    }
  }
  CHECK(sawTop);
}

TEST_CASE("draw list: rounded rect radius clamps to a pill, zero radius is an AA rect") {
  DrawList dl;
  dl.reset(400, 400);
  dl.addRoundedRectFilled({0, 0, 100, 20}, 50.0f, 0xFFFFFFFFu);  // radius clamps to 10
  const uint32_t seg = DrawList::cornerSegments(10.0f);
  // Left/right sides have zero length on a pill: their duplicate endpoints are dropped.
  CHECK(dl.vertices().size() == 2 * (4 * (seg + 1) - 2));
  for (const Vertex& v : dl.vertices()) {
    CHECK(v.pos.y >= -0.71f);
    CHECK(v.pos.y <= 20.71f);
  }
  dl.reset(400, 400);
  dl.addRoundedRectFilled({0.5f, 0.5f, 10.5f, 10.5f}, 0.0f, 0xFFFFFFFFu);
  CHECK(dl.vertices().size() == 8);
  CHECK(dl.indices().size() == 6 + 24);
}

TEST_CASE("draw list: vertical gradient colours follow y") {
  DrawList dl;
  dl.reset(200, 200);
  const Rgba top = rgba(255, 0, 0), bottom = rgba(0, 0, 255, 0);
  dl.addRoundedRectGradientV({0, 0, 100, 100}, 8.0f, top, bottom);
  float minY = 1e9f, maxY = -1e9f;
  Rgba atMin = 0, atMax = 0;
  for (uint32_t i = 0; i < dl.vertices().size(); i += 2) {
    const Vertex& v = dl.vertices()[i];
    if (v.pos.y < minY) {
      minY = v.pos.y;
      atMin = v.col;
    }
    if (v.pos.y > maxY) {
      maxY = v.pos.y;
      atMax = v.col;
    }
  }
  CHECK(atMin == top);
  CHECK(atMax == bottom);
}

TEST_CASE("draw list: corner segment count tracks radius") {
  CHECK(DrawList::cornerSegments(0.1f) == 1);
  uint32_t prev = 1;
  for (float r = 1.0f; r < 2000.0f; r *= 1.5f) {
    const uint32_t s = DrawList::cornerSegments(r);
    CHECK(s >= prev);
    CHECK(s <= 64);
    // Chord sag of one segment stays ≤ 0.2 px (until the 64 cap).
    const float theta = (3.14159265f * 0.5f) / static_cast<float>(s);
    if (s < 64) CHECK(r * (1.0f - std::cos(theta * 0.5f)) <= 0.2001f);
    prev = s;
  }
}

TEST_CASE("draw list: blob uses the atlas sprite rect") {
  DrawList dl;
  dl.reset(100, 100);
  dl.addBlob({50, 50}, 10, rgba(1, 2, 3, 4));
  REQUIRE(dl.vertices().size() == 4);
  const Rect uv = baseAtlasUv().blob;
  CHECK(dl.vertices()[0].pos.x == 40);
  CHECK(dl.vertices()[0].uv.x == uv.x0);
  CHECK(dl.vertices()[2].pos.y == 60);
  CHECK(dl.vertices()[2].uv.y == uv.y1);
}

TEST_CASE("draw list: reset keeps capacity (steady-state frames do not reallocate)") {
  DrawList dl;
  auto build = [&] {
    dl.reset(800, 600);
    for (int i = 0; i < 2000; ++i) {
      const float x = static_cast<float>(i % 40) * 20.0f, y = static_cast<float>(i / 40) * 12.0f;
      dl.addRoundedRectFilled({x, y, x + 18, y + 10}, 3, 0xFF00FF00u);
    }
  };
  build();
  const Vertex* v = dl.vertices().data();
  const uint32_t* idx = dl.indices().data();
  const size_t nv = dl.vertices().size();
  build();
  CHECK(dl.vertices().data() == v);
  CHECK(dl.indices().data() == idx);
  CHECK(dl.vertices().size() == nv);
}

TEST_CASE("atlas: white block and blob sprite") {
  const Image a = buildBaseAtlas();
  REQUIRE(a.width == kAtlasWidth);
  REQUIRE(a.height == kAtlasHeight);
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) CHECK(a.pixel(x, y)[3] == 255);
  const AtlasUv uv = baseAtlasUv();
  CHECK(uv.white.x * kAtlasWidth == doctest::Approx(2.0f));
  // Blob: opaque-ish centre, zero rim, monotone falloff along a radius.
  const uint32_t bx = static_cast<uint32_t>(uv.blob.x0 * kAtlasWidth);
  const uint32_t c = kAtlasBlobSize / 2;
  CHECK(a.pixel(bx + c, c)[3] > 240);
  CHECK(a.pixel(bx, c)[3] <= 1);
  CHECK(a.pixel(bx, 0)[3] == 0);
  for (uint32_t x = c; x + 1 < kAtlasBlobSize; ++x) CHECK(a.pixel(bx + x + 1, c)[3] <= a.pixel(bx + x, c)[3]);
}
