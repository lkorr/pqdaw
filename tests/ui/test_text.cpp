// Text: shaping (advance widths against the font's own tables), rasterisation into the atlas, run caching,
// and the draw-list output of a run. CPU only (no GPU).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/text.h"

using namespace pq::ui;

namespace {

struct Fixture {
  std::shared_ptr<Atlas> atlas = std::make_shared<Atlas>();
  std::unique_ptr<TextSystem> ts;
  FaceId regular = 0, semi = 0;
  FontId body = kNoFont;  // Regular 13

  Fixture() {
    auto t = TextSystem::create(atlas);
    REQUIRE(t.ok());
    ts = std::move(*t);
    const std::string dir = std::string(PQ_ASSETS_DIR) + "/fonts/";
    auto r = ts->loadFace(dir + "Inter-Regular.ttf");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    regular = *r;
    auto s = ts->loadFace(dir + "Inter-SemiBold.ttf");
    REQUIRE(s.ok());
    semi = *s;
    auto f = ts->font(regular, text_size::kBody);
    REQUIRE(f.ok());
    body = *f;
  }
};

}  // namespace

TEST_CASE("text: shaped advance widths of a known string match the font's hmtx table") {
  Fixture fx;
  // Glyph ids and unkerned advances from tools/fixtures/inter_advances.py, which reads cmap/hmtx/head directly
  // from Inter-Regular.ttf (Inter 4.1, upem 2048): round(units · 13 · 64 / 2048) in 26.6 fixed point.
  //   hmtx: 618, 467, 729, 509, 492, 313, 510, 485, 308, 499, 492, 439, 272, 202, 468
  // Inter's GPOS kerns two pairs of this string, which HarfBuzz applies to the first glyph's advance:
  // "rg" (index 5) by −14 and "fo" (index 8) by −24 (26.6; regression values for HarfBuzz 14.5.1 + Inter 4.1,
  // the script does not parse GPOS). Every other advance must equal hmtx exactly.
  const std::vector<int32_t> glyphIds = {161, 507, 766, 578, 915, 852, 658, 614, 647, 790, 773, 871, 889, 689, 980};
  const std::vector<int32_t> expected = {618, 467, 729, 509, 492, 313 - 14, 510, 485, 308 - 24, 499, 492, 439, 272, 202, 468};
  const auto g = fx.ts->shapeGlyphs(fx.body, "Hamburgefonstiv");
  REQUIRE(g.size() == expected.size());
  for (size_t i = 0; i < g.size(); ++i) {
    CAPTURE(i);
    CHECK(static_cast<int32_t>(g[i].glyphId) == glyphIds[i]);
    CHECK(g[i].cluster == i);
    CHECK(g[i].xOffset == 0);
    CHECK(g[i].yOffset == 0);
    CHECK(g[i].xAdvance == expected[i]);
  }
  // The run's pixel advance is the rounded sum: (6803 − 38) / 64 = 105.7 → 106 px.
  CHECK(std::accumulate(expected.begin(), expected.end(), 0) == 6765);
  CHECK(fx.ts->measure(fx.body, "Hamburgefonstiv") == 106);
}

TEST_CASE("text: kerning, ligature-free clusters and size scaling") {
  Fixture fx;
  // "AV" kerns: the pair is narrower than A + V (each 574 units·13/32 = 574 in 26.6 unkerned).
  const auto av = fx.ts->shapeGlyphs(fx.body, "AV");
  REQUIRE(av.size() == 2);
  CHECK(av[0].xAdvance + av[1].xAdvance < 574 + 574);
  CHECK(fx.ts->measure(fx.body, "AV") < fx.ts->measure(fx.body, "A") + fx.ts->measure(fx.body, "V"));
  // Advances scale linearly with the pixel size (unhinted metrics): 26 px is twice 13 px to within rounding.
  auto big = fx.ts->font(fx.regular, 26);
  REQUIRE(big.ok());
  const auto g13 = fx.ts->shapeGlyphs(fx.body, "Hamburgefonstiv");
  const auto g26 = fx.ts->shapeGlyphs(*big, "Hamburgefonstiv");
  REQUIRE(g13.size() == g26.size());
  for (size_t i = 0; i < g13.size(); ++i) CHECK(std::abs(g26[i].xAdvance - 2 * g13[i].xAdvance) <= 1);
  // UTF-8: one cluster per code point, byte offsets as clusters ("é" is two bytes).
  const auto u = fx.ts->shapeGlyphs(fx.body, "a\xC3\xA9z");
  REQUIRE(u.size() == 3);
  CHECK(u[0].cluster == 0);
  CHECK(u[1].cluster == 1);
  CHECK(u[2].cluster == 3);
}

TEST_CASE("text: tabular figures give every digit the same advance") {
  Fixture fx;
  FontOptions o;
  o.tabularFigures = true;
  auto tnum = fx.ts->font(fx.semi, text_size::kMeter, o);
  REQUIRE(tnum.ok());
  const auto g = fx.ts->shapeGlyphs(*tnum, "0123456789");
  REQUIRE(g.size() == 10);
  for (const ShapedGlyph& x : g) CHECK(x.xAdvance == g[0].xAdvance);
  // Proportional (default) digits differ: "1" is narrower than "0" in Inter.
  auto prop = fx.ts->font(fx.semi, text_size::kMeter);
  REQUIRE(prop.ok());
  const auto p = fx.ts->shapeGlyphs(*prop, "01");
  CHECK(p[1].xAdvance < p[0].xAdvance);
  CHECK(fx.ts->measure(*tnum, "1111") == fx.ts->measure(*tnum, "8888"));
}

TEST_CASE("text: fonts are deduplicated and metrics are sane") {
  Fixture fx;
  auto again = fx.ts->font(fx.regular, text_size::kBody);
  REQUIRE(again.ok());
  CHECK(*again == fx.body);
  auto light = fx.ts->font(fx.regular, text_size::kBody, {TextHinting::Light, false});
  REQUIRE(light.ok());
  CHECK(*light != fx.body);
  const FontMetrics& m = fx.ts->metrics(fx.body);
  CHECK(m.pixelSize == 13);
  CHECK(m.ascent >= 12);  // Inter's ascender is 0.97 em
  CHECK(m.ascent <= 14);
  CHECK(m.descent >= 3);
  // Ascent and descent are each rounded up, the line height as a whole: they agree to within a pixel.
  CHECK(m.lineHeight >= m.ascent + m.descent - 1);
  CHECK_FALSE(fx.ts->font(fx.regular, 2).ok());
  CHECK_FALSE(fx.ts->font(99, 13).ok());
  CHECK_FALSE(fx.ts->loadFace(std::string(PQ_ASSETS_DIR) + "/fonts/missing.ttf").ok());
  std::vector<uint8_t> junk(64, 0x5A);
  CHECK_FALSE(fx.ts->loadFaceFromMemory(junk, "junk").ok());
}

TEST_CASE("text: glyphs are rasterised into the atlas once, ASCII up front") {
  Fixture fx;
  // Creating the font warmed printable ASCII (95 code points, minus the space which has no ink).
  const uint64_t warm = fx.ts->stats().glyphsRasterised;
  CHECK(warm >= 90);
  CHECK(fx.atlas->version() > 0);
  const auto rows = fx.atlas->dirtyRowsSince(0);
  CHECK(rows.y1 > rows.y0);
  // ASCII text rasterises nothing new; a non-ASCII glyph is rasterised once.
  fx.ts->shape(fx.body, "Hello, World");
  CHECK(fx.ts->stats().glyphsRasterised == warm);
  const uint64_t v0 = fx.atlas->version();
  fx.ts->shape(fx.body, "\xE2\x88\x9E");  // ∞
  CHECK(fx.ts->stats().glyphsRasterised == warm + 1);
  CHECK(fx.atlas->version() > v0);
  const auto r = fx.atlas->dirtyRowsSince(v0);
  CHECK(r.y1 > r.y0);
  fx.ts->shape(fx.body, "x\xE2\x88\x9E");
  CHECK(fx.ts->stats().glyphsRasterised == warm + 1);
  CHECK(fx.atlas->dirtyRowsSince(fx.atlas->version()).y1 == fx.atlas->dirtyRowsSince(fx.atlas->version()).y0);
  // Glyph texels are white with coverage in alpha. The 'H' crossbar is hinted onto whole pixel rows, so some
  // row of the bitmap is fully covered across the counter.
  const TextRun h = fx.ts->shape(fx.body, "H");
  REQUIRE(h.glyphs.size() == 1);
  const GlyphQuad& q = h.glyphs[0];
  const uint32_t ax = static_cast<uint32_t>(q.uv0.x * kAtlasWidth + 0.5f);
  const uint32_t ay = static_cast<uint32_t>(q.uv0.y * kAtlasHeight + 0.5f);
  CHECK(static_cast<float>(ax + q.w) / kAtlasWidth == doctest::Approx(q.uv1.x));
  uint32_t bestRow = 0;
  for (uint32_t y = 0; y < q.h; ++y) {
    uint32_t sum = 0;
    for (uint32_t x = 0; x < q.w; ++x) {
      const uint8_t* p = fx.atlas->image().pixel(ax + x, ay + y);
      CHECK(p[0] == 255);
      sum += p[3];
    }
    bestRow = std::max(bestRow, sum);
  }
  CHECK(bestRow >= 255u * (q.w - 2));
  // Bitmap sits on the baseline: 'H' has no descender, its top is the cap height above it.
  CHECK(q.y0 + static_cast<int32_t>(q.h) == 0);
  CHECK(q.y0 < -7);
}

TEST_CASE("text: runs are cached; a cached draw writes 4 vertices per inked glyph and does not allocate") {
  Fixture fx;
  DrawList dl;
  dl.reset(400, 100);
  const int32_t adv = fx.ts->draw(dl, fx.body, {10.4f, 30.6f}, "Hi there", rgba(200, 210, 220));
  CHECK(adv == fx.ts->measure(fx.body, "Hi there"));
  CHECK(fx.ts->stats().cacheHits >= 1);  // measure() hit the run draw() cached
  // 7 inked glyphs (the space has none).
  CHECK(dl.vertices().size() == 7 * 4);
  CHECK(dl.indices().size() == 7 * 6);
  REQUIRE(dl.commands().size() == 1);
  CHECK(dl.commands()[0].texture == kAtlasTexture);
  for (const Vertex& v : dl.vertices()) {
    // Origin snapped to (10, 31): every corner on a whole pixel, so texels map 1:1 onto pixels.
    CHECK(v.pos.x == std::floor(v.pos.x));
    CHECK(v.pos.y == std::floor(v.pos.y));
    CHECK(v.col == rgba(200, 210, 220));
  }
  // The first glyph ('H') starts at the origin plus its left bearing (1 px at 13 px).
  CHECK(dl.vertices()[0].pos.x >= 10.0f);
  CHECK(dl.vertices()[0].pos.x <= 12.0f);
  // Warm path: same strings, capacity already grown → no heap traffic at all.
  dl.reset(400, 100);
  const uint64_t hits = fx.ts->stats().cacheHits;
  const uint64_t allocs = pq::alloc_trap::countRtViolations([&] {
    for (int i = 0; i < 4; ++i) fx.ts->draw(dl, fx.body, {10.0f, 30.0f + 14.0f * static_cast<float>(i)}, "Hi there", rgba(255, 255, 255));
  });
  CHECK(allocs == 0);
  CHECK(fx.ts->stats().cacheHits == hits + 4);
  // A draw restores a non-atlas texture binding.
  dl.setTexture(7);
  fx.ts->draw(dl, fx.body, {0, 60}, "x", rgba(255, 255, 255));
  CHECK(dl.texture() == 7);
  CHECK(dl.commands().back().texture == kAtlasTexture);
  // Empty and unknown-font input draw nothing.
  const size_t nv = dl.vertices().size();
  CHECK(fx.ts->draw(dl, fx.body, {0, 0}, "", rgba(255, 255, 255)) == 0);
  CHECK(fx.ts->draw(dl, kNoFont, {0, 0}, "abc", rgba(255, 255, 255)) == 0);
  CHECK(dl.vertices().size() == nv);
}

TEST_CASE("text: the run cache flushes wholesale at its cap and keeps working") {
  Fixture fx;
  for (uint32_t i = 0; i < TextSystem::kMaxCachedRuns + 10; ++i) fx.ts->shape(fx.body, std::to_string(i));
  CHECK(fx.ts->stats().cacheFlushes == 1);
  CHECK(fx.ts->stats().cachedRuns <= TextSystem::kMaxCachedRuns);
  CHECK(fx.ts->measure(fx.body, "12345") > 0);
  fx.ts->clearRunCache();
  CHECK(fx.ts->stats().cachedRuns == 0);
}

TEST_CASE("atlas: shelf packing keeps rects disjoint, inside the page and clear of the built-ins") {
  Atlas a;
  struct R {
    uint32_t x, y, w, h;
  };
  std::vector<R> got;
  for (uint32_t i = 0; i < 400; ++i) {
    const uint32_t w = 3 + (i * 7) % 17, h = 4 + (i * 5) % 19;
    uint32_t x = 0, y = 0;
    REQUIRE(a.allocate(w, h, x, y));
    got.push_back({x, y, w, h});
  }
  for (size_t i = 0; i < got.size(); ++i) {
    const R& r = got[i];
    CHECK(r.x + r.w < kAtlasWidth);
    CHECK(r.y + r.h <= kAtlasHeight);
    CHECK(r.y >= 1);
    CHECK_FALSE((r.x < kAtlasBaseWidth + 1 && r.y < kAtlasBlobSize));  // never over the white block / blob
    for (size_t j = i + 1; j < got.size(); ++j) {
      const R& s = got[j];
      // Disjoint including the 1-texel gutter.
      const bool apart = r.x + r.w + 1 <= s.x || s.x + s.w + 1 <= r.x || r.y + r.h + 1 <= s.y || s.y + s.h + 1 <= r.y;
      CHECK(apart);
    }
  }
  // Exhaustion is reported, not overrun.
  uint32_t x = 0, y = 0;
  bool full = false;
  for (int i = 0; i < 100000 && !full; ++i) full = !a.allocate(60, 60, x, y);
  CHECK(full);
  CHECK_FALSE(a.allocate(kAtlasWidth, 10, x, y));
}

TEST_CASE("atlas: change tracking reports the union of rows changed since a version") {
  Atlas a;
  const uint64_t v0 = a.version();
  a.markDirty(10, 20);
  const uint64_t v1 = a.version();
  a.markDirty(100, 110);
  auto r = a.dirtyRowsSince(v0);
  CHECK(r.y0 == 10);
  CHECK(r.y1 == 110);
  r = a.dirtyRowsSince(v1);
  CHECK(r.y0 == 100);
  CHECK(r.y1 == 110);
  r = a.dirtyRowsSince(a.version());
  CHECK(r.y0 == r.y1);
  // Past the change-list limit, old changes fold into a conservative union, never lost.
  for (uint32_t i = 0; i < 200; ++i) a.markDirty(300 + i, 301 + i);
  r = a.dirtyRowsSince(v0);
  CHECK(r.y0 == 10);
  CHECK(r.y1 == 500);
  r = a.dirtyRowsSince(a.version() - 1);
  CHECK(r.y0 == 499);
  CHECK(r.y1 == 500);
}

TEST_CASE("text: assetsDir finds the bundled fonts") {
  const std::string dir = assetsDir();
  auto t = TextSystem::create(std::make_shared<Atlas>());
  REQUIRE(t.ok());
  CHECK((*t)->loadFace(dir + "/fonts/Inter-Regular.ttf").ok());
}
