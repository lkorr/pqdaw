// Text: shaping (advance widths against the font's own tables), rasterisation into the atlas, run caching,
// and the draw-list output of a run. CPU only (no GPU).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
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
  CHECK_FALSE(fx.atlas->dirty(0).empty());
  fx.atlas->takeDirty(0);  // as the renderer does once per frame
  CHECK(fx.atlas->dirty(0).empty());
  // ASCII text rasterises nothing new; a non-ASCII glyph is rasterised once and dirties just its rect.
  fx.ts->shape(fx.body, "Hello, World");
  CHECK(fx.ts->stats().glyphsRasterised == warm);
  CHECK(fx.atlas->dirty(0).empty());
  const uint64_t v0 = fx.atlas->version();
  const TextRun inf = fx.ts->shape(fx.body, "\xE2\x88\x9E");  // ∞
  CHECK(fx.ts->stats().glyphsRasterised == warm + 1);
  CHECK(fx.atlas->version() > v0);
  REQUIRE(inf.glyphs.size() == 1);
  const Atlas::DirtyRect d = fx.atlas->takeDirty(0);
  CHECK(d.x1 - d.x0 == inf.glyphs[0].w);
  CHECK(d.y1 - d.y0 == inf.glyphs[0].h);
  CHECK(static_cast<float>(d.x0) / kAtlasWidth == doctest::Approx(inf.glyphs[0].uv0.x));
  fx.ts->shape(fx.body, "x\xE2\x88\x9E");
  CHECK(fx.ts->stats().glyphsRasterised == warm + 1);
  CHECK(fx.atlas->dirty(0).empty());
  // Glyph texels are 8-bit coverage. The 'H' crossbar is hinted onto whole pixel rows, so some row of the
  // bitmap is fully covered across the counter.
  const TextRun h = fx.ts->shape(fx.body, "H");
  REQUIRE(h.glyphs.size() == 1);
  const GlyphQuad& q = h.glyphs[0];
  CHECK(q.page == 0);
  const uint32_t ax = static_cast<uint32_t>(q.uv0.x * kAtlasWidth + 0.5f);
  const uint32_t ay = static_cast<uint32_t>(q.uv0.y * kAtlasHeight + 0.5f);
  CHECK(static_cast<float>(ax + q.w) / kAtlasWidth == doctest::Approx(q.uv1.x));
  uint32_t bestRow = 0;
  for (uint32_t y = 0; y < q.h; ++y) {
    uint32_t sum = 0;
    for (uint32_t x = 0; x < q.w; ++x) sum += *fx.atlas->pageImage(0).at(ax + x, ay + y);
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
  CHECK(dl.commands()[0].texture == atlasPageTexture(0));
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
    // Fills between labels stay in the same command (the shared sprites are on every atlas page).
    dl.addRectFilled({0, 0, 4, 4}, rgba(1, 2, 3));
    fx.ts->draw(dl, fx.body, {0, 90}, "Hi there", rgba(255, 255, 255));
  });
  CHECK(allocs == 0);
  CHECK(fx.ts->stats().cacheHits == hits + 5);
  CHECK(dl.commands().size() == 1);
  // A draw restores a non-atlas texture binding.
  dl.setTexture(kFirstUserTexture);
  fx.ts->draw(dl, fx.body, {0, 60}, "x", rgba(255, 255, 255));
  CHECK(dl.texture() == kFirstUserTexture);
  CHECK(dl.commands().back().texture == atlasPageTexture(0));
  // Empty and unknown-font input draw nothing.
  const size_t nv = dl.vertices().size();
  CHECK(fx.ts->draw(dl, fx.body, {0, 0}, "", rgba(255, 255, 255)) == 0);
  CHECK(fx.ts->draw(dl, kNoFont, {0, 0}, "abc", rgba(255, 255, 255)) == 0);
  CHECK(dl.vertices().size() == nv);
}

TEST_CASE("text: drawRunClipped cuts glyph ink to the clip texel-exactly and equals drawRun when the ink fits (M3 M13)") {
  Fixture fx;
  const TextRun run = fx.ts->shape(fx.body, "Quickly jog");
  REQUIRE(!run.glyphs.empty());
  const Vec2 origin{20.0f, 30.0f};
  // Ink bounds of the run at the origin.
  int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
  for (const GlyphQuad& g : run.glyphs) {
    x0 = std::min(x0, 20 + g.x0);
    y0 = std::min(y0, 30 + g.y0);
    x1 = std::max(x1, 20 + g.x0 + static_cast<int32_t>(g.w));
    y1 = std::max(y1, 30 + g.y0 + static_cast<int32_t>(g.h));
  }
  DrawList a, b;
  a.reset(400, 100);
  b.reset(400, 100);
  TextSystem::drawRun(a, run, origin, rgba(255, 255, 255));
  TextSystem::drawRunClipped(b, run, origin, rgba(255, 255, 255), {x0, y0, x1, y1});
  REQUIRE(a.vertices().size() == b.vertices().size());
  for (size_t i = 0; i < a.vertices().size(); ++i) {
    CHECK(a.vertices()[i].pos.x == b.vertices()[i].pos.x);
    CHECK(a.vertices()[i].pos.y == b.vertices()[i].pos.y);
    CHECK(a.vertices()[i].uv.x == b.vertices()[i].uv.x);
    CHECK(a.vertices()[i].uv.y == b.vertices()[i].uv.y);
  }
  // A clip cutting the descenders ('Q', 'y', 'j', 'g') and the last glyph: every vertex inside it, uv moved by whole
  // texels in proportion (a 1:1 mapping kept), and a glyph wholly outside dropped.
  const ClipRect clip{x0 + 3, y0, x1 - 4, 30};
  DrawList c;
  c.reset(400, 100);
  TextSystem::drawRunClipped(c, run, origin, rgba(255, 255, 255), clip);
  REQUIRE(!c.vertices().empty());
  CHECK(c.vertices().size() <= a.vertices().size());
  const float texel = 1.0f / static_cast<float>(kAtlasWidth);  // square pages
  for (size_t q = 0; q + 3 < c.vertices().size(); q += 4) {
    const Vertex* v = &c.vertices()[q];
    CHECK(v[0].pos.x >= static_cast<float>(clip.x0));
    CHECK(v[0].pos.y >= static_cast<float>(clip.y0));
    CHECK(v[2].pos.x <= static_cast<float>(clip.x1));
    CHECK(v[2].pos.y <= static_cast<float>(clip.y1));
    CHECK(std::abs((v[2].uv.x - v[0].uv.x) - (v[2].pos.x - v[0].pos.x) * texel) < 1e-6f);
    CHECK(std::abs((v[2].uv.y - v[0].uv.y) - (v[2].pos.y - v[0].pos.y) * texel) < 1e-6f);
  }
}

TEST_CASE("text: 20k distinct strings shaped in one frame keep every earlier run valid (M8)" * doctest::test_suite("slow")) {
  Fixture fx;
  constexpr uint32_t kCount = 20000;  // > kMaxCachedRuns: the old cache flushed inside shape() here
  static_assert(kCount > TextSystem::kMaxCachedRuns);
  fx.ts->beginFrame();
  struct Kept {
    TextRun run;
    std::vector<GlyphQuad> glyphs;  // copies taken when the run was returned
    std::vector<CaretStop> carets;
  };
  std::vector<Kept> kept;
  kept.reserve(kCount);
  for (uint32_t i = 0; i < kCount; ++i) {
    const std::string s = "v" + std::to_string(i * 7919u) + (i % 3 == 0 ? " dB" : " Hz");
    const TextRun r = fx.ts->shape(fx.body, s);
    kept.push_back({r, {r.glyphs.begin(), r.glyphs.end()}, {r.carets.begin(), r.carets.end()}});
  }
  CHECK(fx.ts->stats().cacheFlushes == 0);
  CHECK(fx.ts->stats().cachedRuns >= kCount);
  // Every span still points at the same, unchanged data.
  uint32_t bad = 0;
  for (const Kept& k : kept) {
    const bool same = k.run.glyphs.size() == k.glyphs.size() && k.run.carets.size() == k.carets.size() &&
                      std::equal(k.glyphs.begin(), k.glyphs.end(), k.run.glyphs.begin(),
                                 [](const GlyphQuad& a, const GlyphQuad& b) {
                                   return a.x0 == b.x0 && a.y0 == b.y0 && a.w == b.w && a.h == b.h && a.page == b.page &&
                                          a.uv0.x == b.uv0.x && a.uv0.y == b.uv0.y;
                                 }) &&
                      std::equal(k.carets.begin(), k.carets.end(), k.run.carets.begin(),
                                 [](const CaretStop& a, const CaretStop& b) { return a.byte == b.byte && a.x == b.x; });
    bad += same ? 0u : 1u;
  }
  CHECK(bad == 0);
  // The frame boundary trims the cache (wholesale), and shaping keeps working afterwards.
  fx.ts->beginFrame();
  CHECK(fx.ts->stats().cacheFlushes == 1);
  CHECK(fx.ts->stats().cachedRuns == 0);
  CHECK(fx.ts->measure(fx.body, "12345") > 0);
  fx.ts->beginFrame();
  CHECK(fx.ts->stats().cacheFlushes == 1);  // under the cap: kept
  fx.ts->clearRunCache();
  CHECK(fx.ts->stats().cachedRuns == 0);
}

TEST_CASE("text: caret stops per cluster, caretX and hitTest") {
  Fixture fx;
  // "aé z": clusters at bytes 0, 1 (é is 2 bytes), 3 (space), 4; end stop at 5.
  const TextRun r = fx.ts->shape(fx.body, "a\xC3\xA9 z");
  REQUIRE(r.carets.size() == 5);
  const uint32_t bytes[] = {0, 1, 3, 4, 5};
  for (size_t i = 0; i < 5; ++i) CHECK(r.carets[i].byte == bytes[i]);
  CHECK(r.carets[0].x == 0);
  CHECK(r.carets.back().x == r.advance);
  for (size_t i = 1; i < r.carets.size(); ++i) CHECK(r.carets[i].x > r.carets[i - 1].x);
  CHECK(r.visibleBytes == 5);
  CHECK_FALSE(r.truncated);
  // Stops are the pen positions HarfBuzz reports, rounded like the glyph origins.
  const auto g = fx.ts->shapeGlyphs(fx.body, "a\xC3\xA9 z");
  int32_t pen = 0;
  for (size_t i = 0; i < g.size(); ++i) {
    CHECK(r.carets[i].x == (pen + 32) >> 6);
    pen += g[i].xAdvance;
  }
  // caretX: the stop at or before the byte (inside é → its start); past the end → the advance.
  CHECK(r.caretX(0) == 0);
  CHECK(r.caretX(2) == r.carets[1].x);
  CHECK(r.caretX(4) == r.carets[3].x);
  CHECK(r.caretX(99) == r.advance);
  // hitTest: nearest stop; ties go left.
  CHECK(r.hitTest(-5) == 0);
  CHECK(r.hitTest(r.carets[2].x) == 3);
  CHECK(r.hitTest(r.carets[2].x + 1) == 3);
  CHECK(r.hitTest(r.advance + 50) == 5);
  const int32_t mid = (r.carets[3].x + r.carets[4].x) / 2;
  const uint32_t at = r.hitTest(mid);
  CHECK((at == 4 || at == 5));
  CHECK(TextRun{}.caretX(3) == 0);
  CHECK(TextRun{}.hitTest(3) == 0);
}

TEST_CASE("text: ellipsis truncation fits the width at a cluster boundary") {
  Fixture fx;
  const std::string s = "Master bus compressor";
  const int32_t full = fx.ts->measure(fx.body, s);
  const int32_t ell = fx.ts->measure(fx.body, "\xE2\x80\xA6");
  REQUIRE(ell > 0);
  // Fits: the plain run, not truncated.
  TextRun r = fx.ts->shapeTruncated(fx.body, s, full);
  CHECK_FALSE(r.truncated);
  CHECK(r.advance == full);
  CHECK(r.visibleBytes == s.size());
  // Every width below the full one: fits, ends in "…", and keeps the longest prefix that does.
  for (int32_t w = full - 1; w >= 0; --w) {
    CAPTURE(w);
    r = fx.ts->shapeTruncated(fx.body, s, w);
    CHECK(r.truncated);
    CHECK(r.advance <= w);
    if (w < ell) {
      CHECK(r.advance == 0);
      CHECK(r.glyphs.empty());
      continue;
    }
    REQUIRE(r.carets.size() >= 2);
    CHECK(r.carets.back().byte == r.visibleBytes + 3);
    CHECK(r.carets[r.carets.size() - 2].byte == r.visibleBytes);  // the ellipsis's own stop
    CHECK(r.visibleBytes < s.size());
    if (r.visibleBytes > 0) CHECK(s[r.visibleBytes - 1] != ' ');  // no space before the ellipsis
    // Same as shaping prefix + "…" directly.
    const std::string expect = s.substr(0, r.visibleBytes) + "\xE2\x80\xA6";
    CHECK(r.advance == fx.ts->measure(fx.body, expect));
    // One more (non-space) cluster would not have fit.
    uint32_t next = r.visibleBytes;
    while (next < s.size() && s[next] == ' ') ++next;
    if (next < s.size()) CHECK(fx.ts->measure(fx.body, s.substr(0, next + 1) + "\xE2\x80\xA6") > w);
  }
  // Cached per (string, width): a repeat is a hit and returns the same storage.
  const TextRun a = fx.ts->shapeTruncated(fx.body, s, 60);
  const uint64_t hits = fx.ts->stats().cacheHits;
  const TextRun b = fx.ts->shapeTruncated(fx.body, s, 60);
  CHECK(fx.ts->stats().cacheHits > hits);
  CHECK(a.glyphs.data() == b.glyphs.data());
  // drawTruncated draws exactly the truncated run.
  DrawList dl;
  dl.reset(200, 40);
  CHECK(fx.ts->drawTruncated(dl, fx.body, {0, 20}, s, 60, rgba(255, 255, 255)) == a.advance);
  CHECK(dl.vertices().size() == a.glyphs.size() * 4);
  // Empty input.
  CHECK(fx.ts->shapeTruncated(fx.body, "", 10).advance == 0);
  CHECK_FALSE(fx.ts->shapeTruncated(fx.body, "", 10).truncated);
}

TEST_CASE("text: atlas eviction is LRU by page and never touches a page the current frame uses") {
  // Two pages at a large size: a few hundred glyphs overflow them, forcing eviction across frames.
  auto atlas = std::make_shared<Atlas>(2);
  auto t = TextSystem::create(atlas);
  REQUIRE(t.ok());
  TextSystem& ts = **t;
  auto face = ts.loadFace(std::string(PQ_ASSETS_DIR) + "/fonts/Inter-Regular.ttf");
  REQUIRE(face.ok());
  auto font = ts.font(*face, 128);
  REQUIRE(font.ok());
  // Strings of 16 distinct non-ASCII letters each (Latin-1 Supplement, Latin Extended-A/B, Greek, Cyrillic).
  auto block = [](uint32_t first) {
    std::string s;
    for (uint32_t c = first; c < first + 16; ++c) {
      s += static_cast<char>(0xC0 | (c >> 6));
      s += static_cast<char>(0x80 | (c & 0x3F));
    }
    return s;
  };
  const uint32_t starts[] = {0xC0, 0xD8, 0xE8, 0x100, 0x110, 0x120, 0x130, 0x140, 0x150, 0x160, 0x170,
                             0x180, 0x190, 0x1A0, 0x1B0, 0x1C4, 0x1D4, 0x1E4, 0x1F4, 0x204, 0x214, 0x224,
                             0x391, 0x3A3, 0x3B1, 0x3C1, 0x410, 0x420, 0x430, 0x440, 0x450, 0x460};
  // Frame 1: "ABC" (ASCII, page 0). Keep its texels to compare after it has been evicted and rebuilt.
  ts.beginFrame();
  auto texelsOf = [&](const TextRun& r) {
    std::vector<uint8_t> out;
    for (const GlyphQuad& q : r.glyphs) {
      const uint32_t ax = static_cast<uint32_t>(q.uv0.x * kAtlasWidth + 0.5f), ay = static_cast<uint32_t>(q.uv0.y * kAtlasHeight + 0.5f);
      for (uint32_t y = 0; y < q.h; ++y)
        for (uint32_t x = 0; x < q.w; ++x) out.push_back(*atlas->pageImage(q.page).at(ax + x, ay + y));
    }
    return out;
  };
  const std::vector<uint8_t> abc = texelsOf(ts.shape(*font, "ABC"));
  // Each later frame shapes one block. Within a frame, every page referenced by a run returned in that frame
  // must keep its generation (no eviction under a live run).
  uint32_t frames = 0;
  for (uint32_t start : starts) {
    ts.beginFrame();
    ++frames;
    const TextRun r = ts.shape(*font, block(start));
    std::vector<std::pair<uint32_t, uint32_t>> used;
    for (const GlyphQuad& q : r.glyphs) used.push_back({q.page, atlas->generation(q.page)});
    const TextRun again = ts.shape(*font, block(start));  // same frame: a hit, same storage
    CHECK(again.glyphs.data() == r.glyphs.data());
    for (auto [page, gen] : used) CHECK(atlas->generation(page) == gen);
    if (atlas->evictions() >= 2) break;
  }
  MESSAGE("evictions after " << frames << " frames: " << atlas->evictions() << ", glyphs dropped: " << ts.stats().atlasFull);
  REQUIRE(atlas->evictions() >= 1);
  CHECK(atlas->pageCount() == 2);
  CHECK(atlas->memoryBytes() == 2 * kAtlasPageBytes);
  CHECK(ts.stats().atlasFull == 0);  // only one block per frame: there was always an unused page to evict
  // "ABC" was built in frame 1; its page has been evicted since, so the next shape rebuilds it (re-rasterised,
  // texel-identical).
  ts.beginFrame();
  const uint64_t rebuilt = ts.stats().runsRebuilt;
  const TextRun abc2 = ts.shape(*font, "ABC");
  CHECK(ts.stats().runsRebuilt == rebuilt + 1);
  CHECK(texelsOf(abc2) == abc);
  for (const GlyphQuad& q : abc2.glyphs) CHECK(q.page < atlas->pageCount());
}

TEST_CASE("text: a full atlas drops glyphs (counted, not cached) and the run recovers on a later frame") {
  auto atlas = std::make_shared<Atlas>(1);
  auto t = TextSystem::create(atlas);
  REQUIRE(t.ok());
  TextSystem& ts = **t;
  auto face = ts.loadFace(std::string(PQ_ASSETS_DIR) + "/fonts/Inter-Regular.ttf");
  REQUIRE(face.ok());
  auto font = ts.font(*face, 120);
  REQUIRE(font.ok());
  // One frame asks for far more ink than one page holds: everything is in use by this frame, nothing can go.
  ts.beginFrame();
  std::string many;
  for (uint32_t c = 0x100; c < 0x180; ++c) {
    many += static_cast<char>(0xC0 | (c >> 6));
    many += static_cast<char>(0x80 | (c & 0x3F));
  }
  const TextRun r = ts.shape(*font, many);
  CHECK(ts.stats().atlasFull > 0);
  CHECK(r.glyphs.size() < 128);
  CHECK(atlas->evictions() == 0);
  // Next frame: the incomplete run is rebuilt (the page is now evictable, so its glyphs fit again or are
  // dropped again; either way it is rebuilt rather than served stale).
  ts.beginFrame();
  const uint64_t rebuilt = ts.stats().runsRebuilt;
  ts.shape(*font, many);
  CHECK(ts.stats().runsRebuilt == rebuilt + 1);
}

TEST_CASE("atlas: shelf packing keeps rects disjoint, inside the page and clear of the built-ins") {
  Atlas a(1);
  struct R {
    uint32_t x, y, w, h;
  };
  std::vector<R> got;
  for (uint32_t i = 0; i < 400; ++i) {
    const uint32_t w = 3 + (i * 7) % 17, h = 4 + (i * 5) % 19;
    Atlas::Slot s;
    REQUIRE(a.allocate(w, h, s));
    CHECK(s.page == 0);
    got.push_back({s.x, s.y, w, h});
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
  // Exhaustion within one frame (every page in use) is reported, not overrun.
  Atlas::Slot s;
  bool full = false;
  for (int i = 0; i < 100000 && !full; ++i) full = !a.allocate(60, 60, s);
  CHECK(full);
  CHECK(a.evictions() == 0);
  CHECK_FALSE(a.allocate(kAtlasWidth, 10, s));
  // After a frame boundary the page is evictable: it is cleared back to the base layout and reused.
  a.beginFrame();
  const uint32_t gen = a.generation(0);
  REQUIRE(a.allocate(60, 60, s));
  CHECK(a.generation(0) == gen + 1);
  CHECK(a.evictions() == 1);
  CHECK(a.evictedAt(0) == 1);
  CHECK(s.y == 1);  // first shelf again
  CHECK(*a.pageImage(0).at(1, 1) == 255);  // the white block survives
  CHECK(*a.pageImage(0).at(500, 500) == 0);
}

TEST_CASE("atlas: pages open on demand, up to the limit, and change tracking is one rect per page") {
  Atlas a(3);
  CHECK(a.pageCount() == 1);
  CHECK(a.memoryBytes() == kAtlasPageBytes);
  // A new page starts fully dirty (its first upload is the whole page), as does page 0.
  CHECK(a.takeDirty(0).x1 == kAtlasWidth);
  CHECK(a.dirty(0).empty());
  Atlas::Slot s;
  uint32_t maxPage = 0, placed = 0;
  while (placed < 2000 && a.allocate(100, 100, s)) {
    maxPage = std::max(maxPage, s.page);
    ++placed;
  }
  CHECK(placed < 2000);  // 3 pages, all used this frame: the next one fails
  CHECK(placed >= 3 * 70);  // 7 shelves (101 + 1/4 slack rows) × 10 cells (101 texels incl. the gutter) per page
  CHECK(a.pageCount() == 3);
  CHECK(maxPage == 2);
  CHECK(a.evictions() == 0);
  // Dirty rects are the union of what was written since the last take.
  Atlas b(1);
  b.takeDirty(0);
  const uint64_t v = b.version();
  b.markDirty(0, 200, 10, 210, 20);
  b.markDirty(0, 211, 10, 215, 30);  // next glyph on the shelf: merges
  b.markDirty(0, 150, 100, 160, 110);
  CHECK(b.version() > v);
  REQUIRE(b.dirtyRects(0).size() == 2);
  CHECK(b.dirtyRects(0)[0].x0 == 200);
  CHECK(b.dirtyRects(0)[0].x1 == 215);
  CHECK(b.dirtyRects(0)[0].y1 == 30);
  CHECK(b.dirtyRects(0)[1].x0 == 150);
  const Atlas::DirtyRect d = b.takeDirty(0);
  CHECK(d.x0 == 150);
  CHECK(d.y0 == 10);
  CHECK(d.x1 == 215);
  CHECK(d.y1 == 110);
  CHECK(b.takeDirty(0).empty());
  CHECK(b.dirtyRects(0).empty());
  // Past kMaxDirtyRects the page falls back to the bounding rect.
  for (uint32_t i = 0; i <= Atlas::kMaxDirtyRects; ++i) b.markDirty(0, 10, 100 + 3 * i, 12, 101 + 3 * i);
  REQUIRE(b.dirtyRects(0).size() == 1);
  CHECK(b.dirtyRects(0)[0].y0 == 100);
  CHECK(b.dirtyRects(0)[0].y1 == 101 + 3 * Atlas::kMaxDirtyRects);
  b.takeDirty(0);
  b.markDirty(0, 5, 5, 5, 9);  // empty rects are ignored
  CHECK(b.dirty(0).empty());
}

TEST_CASE("text: assetsDir finds the bundled fonts") {
  const std::string dir = assetsDir();
  auto t = TextSystem::create(std::make_shared<Atlas>());
  REQUIRE(t.ok());
  CHECK((*t)->loadFace(dir + "/fonts/Inter-Regular.ttf").ok());
}
