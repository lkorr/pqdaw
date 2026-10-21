// T0.7-fix3 HiDPI content scale: UiScale conversions, font roles resolved at round(role · scale), every glyph
// quad on integer device pixels with a 1:1 texel mapping (no resampling blur) at 1.0 / 1.25 / 1.5 / 1.75 / 2.0,
// the atlas holding all scales' fonts, and a runtime scale change through TestDriver (relayout + new font
// sizes + redraw without rebuilding anything). The scaled goldens are in test_render.cpp, the window-level
// scripted scale change in `pqdaw --window-selftest`.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/image.h"
#include "pq/ui/scenes.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/text.h"
#include "pq/ui/ui_scale.h"
#include "pq/ui/widget.h"

using namespace pq::ui;

namespace {

constexpr float kScales[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};

bool integral(float v) { return v == std::floor(v); }

// A vertex samples a glyph bitmap when its uv lies outside the built-in sprites (white block + blob occupy
// texels [0, 128) × [0, 64) of the atlas page; glyphs are packed everywhere else).
bool isGlyphUv(Vec2 uv) {
  return uv.x * static_cast<float>(kAtlasWidth) >= static_cast<float>(kAtlasBaseWidth) ||
         uv.y * static_cast<float>(kAtlasHeight) >= static_cast<float>(kAtlasBlobSize);
}

struct GlyphCheck {
  uint32_t quads = 0;
  uint32_t offPixel = 0;     // a corner not on an integer device pixel
  uint32_t offTexel = 0;     // a uv not on an integer texel
  uint32_t resampled = 0;    // quad size in px != bitmap size in texels
  uint32_t notAxisQuad = 0;  // the 4 vertices are not an axis-aligned rectangle
  float maxHeight = 0;       // tallest glyph quad (px)
};

// Every glyph quad in `dl` (TextSystem::drawRun writes each as 4 consecutive vertices: tl, tr, br, bl).
GlyphCheck checkGlyphs(const DrawList& dl) {
  GlyphCheck c;
  const auto v = dl.vertices();
  const float aw = static_cast<float>(kAtlasWidth), ah = static_cast<float>(kAtlasHeight);
  for (size_t i = 0; i + 3 < v.size();) {
    if (!isGlyphUv(v[i].uv)) {
      ++i;
      continue;
    }
    const Vertex &tl = v[i], &tr = v[i + 1], &br = v[i + 2], &bl = v[i + 3];
    ++c.quads;
    for (const Vertex* p : {&tl, &tr, &br, &bl}) {
      if (!integral(p->pos.x) || !integral(p->pos.y)) ++c.offPixel;
      if (!integral(p->uv.x * aw) || !integral(p->uv.y * ah)) ++c.offTexel;
    }
    if (!(tr.pos.y == tl.pos.y && br.pos.x == tr.pos.x && bl.pos.x == tl.pos.x && bl.pos.y == br.pos.y)) ++c.notAxisQuad;
    const float w = br.pos.x - tl.pos.x, h = br.pos.y - tl.pos.y;
    if (w != (br.uv.x - tl.uv.x) * aw || h != (br.uv.y - tl.uv.y) * ah) ++c.resampled;
    c.maxHeight = std::max(c.maxHeight, h);
    i += 4;
  }
  return c;
}

}  // namespace

TEST_CASE("hidpi: UiScale snaps logical px to device px") {
  const UiScale s1;
  CHECK(s1.factor == 1.0f);
  CHECK(s1.px(13.0f) == 13);
  const UiScale s(1.25f);
  CHECK(s.px(13.0f) == 16);  // 16.25
  CHECK(s.px(14.0f) == 18);  // 17.5 rounds away from zero
  CHECK(s.px(-2.0f) == -3);  // -2.5 likewise (offsets left/up)
  // Each edge snaps on its own: adjacent logical rects stay adjacent in device px (no gaps, no overlap).
  const Rect a = s.rect(0, 0, 13, 10), b = s.rect(13, 0, 27, 10);
  CHECK(a.x1 == b.x0);
  CHECK(integral(a.x1));
  CHECK(UiScale(1.5f).exact(3.0f) == 4.5f);
  CHECK(UiScale(1.5f).toLogical(300.0f) == 200.0f);
  // Unknown / absurd display scales.
  CHECK(UiScale(0.0f).factor == 1.0f);
  CHECK(UiScale(-1.0f).factor == 1.0f);
  CHECK(UiScale(std::nanf("")).factor == 1.0f);
  CHECK(UiScale(INFINITY).factor == 1.0f);
  CHECK(UiScale(0.1f).factor == UiScale::kMin);
  CHECK(UiScale(9.0f).factor == UiScale::kMax);
  // Font roles: round(role · scale), integer px.
  CHECK(UiScale(1.25f).fontPx(12) == 15);
  CHECK(UiScale(1.25f).fontPx(13) == 16);
  CHECK(UiScale(1.25f).fontPx(14) == 18);
  CHECK(UiScale(1.5f).fontPx(13) == 20);  // 19.5
  CHECK(UiScale(2.0f).fontPx(24) == 48);
}

TEST_CASE("hidpi: scene font roles resolve to round(role x scale) px fonts") {
  auto t = createSceneText(std::make_shared<Atlas>());
  REQUIRE_MESSAGE(t.ok(), (t.ok() ? "" : t.error().message));
  const TextSystem& ts = *t->text;
  for (float f : kScales) {
    CAPTURE(f);
    const UiScale s(f);
    REQUIRE(setSceneScale(*t, s).ok());
    CHECK(t->scale == s);
    const SceneFonts& fn = t->fonts;
    CHECK(ts.metrics(fn.section).pixelSize == static_cast<uint32_t>(std::lround(12 * f)));
    CHECK(ts.metrics(fn.body).pixelSize == static_cast<uint32_t>(std::lround(13 * f)));
    CHECK(ts.metrics(fn.title).pixelSize == static_cast<uint32_t>(std::lround(14 * f)));
    CHECK(ts.metrics(fn.bodySemi).pixelSize == s.fontPx(13));
    CHECK(ts.metrics(fn.titleSemi).pixelSize == s.fontPx(14));
    CHECK(ts.metrics(fn.meter).pixelSize == s.fontPx(24));
    CHECK(ts.metrics(fn.bodyLight).pixelSize == s.fontPx(13));
    CHECK(fn.body != fn.bodyLight);  // same size, different hinting: distinct fonts
  }
  // Returning to a scale seen before is a lookup: no new fonts, no rasterisation.
  const uint64_t rasterised = ts.stats().glyphsRasterised;
  const SceneFonts at15 = [&] {
    REQUIRE(setSceneScale(*t, UiScale(1.5f)).ok());
    return t->fonts;
  }();
  REQUIRE(setSceneScale(*t, UiScale(1.0f)).ok());
  REQUIRE(setSceneScale(*t, UiScale(1.5f)).ok());
  CHECK(t->fonts.body == at15.body);
  CHECK(ts.stats().glyphsRasterised == rasterised);
  // The one atlas page holds every scale's fonts (ASCII warmed) without dropping a glyph.
  CHECK(ts.stats().atlasFull == 0);
  MESSAGE("atlas rows used by the scene fonts at 5 scales: " << ts.atlas()->usedHeight() << " / " << kAtlasHeight);
}

TEST_CASE("hidpi: every glyph quad lands on integer device pixels at every scale") {
  auto t = createSceneText(std::make_shared<Atlas>());
  REQUIRE(t.ok());
  DrawList dl;
  for (float f : kScales) {
    REQUIRE(setSceneScale(*t, UiScale(f)).ok());
    for (const char* scene : {"text", "widgets-core"}) {
      CAPTURE(f);
      CAPTURE(scene);
      const UiScale s(f);
      dl.reset(static_cast<uint32_t>(s.px(800)), static_cast<uint32_t>(s.px(480)));
      REQUIRE(buildScene(scene, dl, *t, 0.0));
      const GlyphCheck c = checkGlyphs(dl);
      CHECK(c.quads > (std::string(scene) == "text" ? 800u : 60u));
      CHECK(c.offPixel == 0);
      CHECK(c.offTexel == 0);
      CHECK(c.resampled == 0);
      CHECK(c.notAxisQuad == 0);
    }
  }
  // Fractional baselines (a caller that forgot to snap) are still snapped by the text system.
  REQUIRE(setSceneScale(*t, UiScale(1.25f)).ok());
  dl.reset(400, 100);
  t->text->draw(dl, t->fonts.body, {10.3f, 40.6f}, "Snap me: 0.37 px off", rgba(255, 255, 255));
  t->text->draw(dl, t->fonts.meter, {12.5f * 1.25f, 80.49f}, "-12.5 dB", rgba(255, 255, 255));
  const GlyphCheck c = checkGlyphs(dl);
  CHECK(c.quads > 15);
  CHECK(c.offPixel == 0);
  CHECK(c.resampled == 0);
}

TEST_CASE("hidpi: glyphs grow with the scale (fonts are rasterised at the device size)") {
  auto t = createSceneText(std::make_shared<Atlas>());
  REQUIRE(t.ok());
  DrawList dl;
  float prev = 0;
  for (float f : kScales) {
    REQUIRE(setSceneScale(*t, UiScale(f)).ok());
    dl.reset(1600, 200);
    t->text->draw(dl, t->fonts.body, {10, 100}, "Hxg|", rgba(255, 255, 255));
    const float h = checkGlyphs(dl).maxHeight;
    CAPTURE(f);
    CHECK(h > prev);
    // '|' spans roughly ascender to descender: ~1.2 em; within a few px of 1.2 · round(13 · scale).
    CHECK(std::fabs(h - 1.2f * static_cast<float>(UiScale(f).fontPx(13))) < 4.0f);
    prev = h;
  }
}

TEST_CASE("hidpi: a runtime scale change relayouts, re-resolves fonts and redraws (TestDriver)") {
  TestDriver d(800, 480, 1.0f);
  d.tree().setRoot(makeWidgetsCoreDemo());
  REQUIRE(d.enableGpu().ok());
  d.step();
  const uint64_t frames = d.framesDrawn();
  const PaintResources& res = d.resources();
  const FontId before = res.fonts->section;
  CHECK(res.text->metrics(before).pixelSize == 12);
  CHECK(d.rect("header").h == 28);
  const float glyphH1 = checkGlyphs(d.drawList()).maxHeight;

  // 150 %: the window's device size grows with the scale (same logical size).
  d.resize(1200, 720, 1.5f);
  CHECK(d.tree().scale() == 1.5f);
  CHECK(res.text->metrics(res.fonts->section).pixelSize == 18);
  CHECK(res.fonts->section != before);
  CHECK(d.rect("header").h == 42);
  CHECK(d.rect("body/browser/item[0]").h == 33);
  CHECK(d.step());  // damaged → a frame is drawn without rebuilding the tree or the GPU resources
  CHECK(d.framesDrawn() == frames + 1);
  const GlyphCheck c = checkGlyphs(d.drawList());
  CHECK(c.quads > 60);
  CHECK(c.offPixel == 0);
  CHECK(c.resampled == 0);
  CHECK(c.maxHeight > glyphH1 * 1.3f);

  // And it renders through the GPU at the new device size.
  auto img = d.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  CHECK(img->width == 1200);
  CHECK(img->height == 720);

  // Back to 1.0: the old fonts are found again.
  d.resize(800, 480, 1.0f);
  CHECK(res.fonts->section == before);
  CHECK(d.rect("header").h == 28);
  CHECK(d.step());
}
