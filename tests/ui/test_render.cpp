// GPU tests: the offscreen path renders through the real Vulkan pipelines (headless, no window).

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"
#include "pq/ui/text.h"

using namespace pq::ui;

namespace {

// One device + renderer for the probe tests (device creation with validation costs ~100 ms).
struct Gpu {
  std::unique_ptr<GpuContext> ctx;
  std::unique_ptr<Renderer> renderer;
  SceneTextures textures;
  TextureId checker2 = kAtlasTexture;  // 2×2: red, green / blue, white (nearest)
};

Gpu& gpu() {
  static Gpu g = [] {
    Gpu out;
    GpuOptions o;
    o.validation = true;  // used when the Khronos validation layer is installed
    auto ctx = GpuContext::create(o);
    REQUIRE_MESSAGE(ctx.ok(), (ctx.ok() ? "" : ctx.error().message));
    out.ctx = std::move(*ctx);
    auto r = Renderer::create(*out.ctx, OffscreenTarget::kFormat, 1);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    out.renderer = std::move(*r);
    auto t = createSceneTextures(*out.renderer);
    REQUIRE(t.ok());
    out.textures = *t;
    Image c;
    c.width = c.height = 2;
    c.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    auto id = out.renderer->createTexture(c, TextureFilter::Nearest);
    REQUIRE(id.ok());
    out.checker2 = *id;
    MESSAGE("GPU: " << out.ctx->deviceName() << std::string(out.ctx->validationEnabled() ? " (validation on)" : " (no validation layer)"));
    return out;
  }();
  return g;
}

Image draw(const DrawList& dl, Rgba clear) {
  auto target = OffscreenTarget::create(*gpu().ctx, dl.targetWidth(), dl.targetHeight());
  REQUIRE(target.ok());
  REQUIRE((*target)->draw(*gpu().renderer, dl, clear).ok());
  auto img = (*target)->read();
  REQUIRE(img.ok());
  return std::move(*img);
}

bool near(int a, int b, int tol) { return a - b <= tol && b - a <= tol; }

}  // namespace

TEST_CASE("render: solid rect fills exactly its pixels") {
  DrawList dl;
  dl.reset(32, 16);
  dl.addRectFilled({4, 2, 12, 10}, rgba(200, 100, 50));
  const Image img = draw(dl, rgba(0, 0, 0));
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 32; ++x) {
      const bool inside = x >= 4 && x < 12 && y >= 2 && y < 10;
      const uint8_t* p = img.pixel(x, y);
      if (inside) {
        CHECK(p[0] == 200);
        CHECK(p[1] == 100);
        CHECK(p[2] == 50);
      } else {
        CHECK(p[0] == 0);
      }
      CHECK(p[3] == 255);
    }
}

TEST_CASE("render: alpha and additive blending") {
  DrawList dl;
  dl.reset(8, 8);
  dl.addRectFilled({0, 0, 4, 8}, rgba(255, 0, 0, 128));  // alpha over (50, 0, 0)
  dl.setBlend(BlendMode::Additive);
  dl.addRectFilled({4, 0, 8, 4}, rgba(100, 0, 0));       // additive: 50 + 100
  dl.addRectFilled({4, 4, 8, 8}, rgba(200, 0, 0));
  dl.addRectFilled({4, 4, 8, 8}, rgba(200, 0, 0));       // saturates at 255
  const Image img = draw(dl, rgba(50, 0, 0));
  // 255·(128/255) + 50·(1 − 128/255) = 152.9
  CHECK(near(img.pixel(1, 1)[0], 153, 1));
  CHECK(img.pixel(1, 1)[3] == 255);  // opaque target stays opaque
  CHECK(near(img.pixel(5, 1)[0], 150, 1));
  CHECK(img.pixel(5, 6)[0] == 255);
  CHECK(img.pixel(5, 6)[3] == 255);
}

TEST_CASE("render: textured quads sample the bound texture (nearest)") {
  DrawList dl;
  dl.reset(8, 8);
  dl.addImage(gpu().checker2, {0, 0, 8, 8}, {0, 0}, {1, 1});
  const Image img = draw(dl, rgba(0, 0, 0));
  auto is = [&](uint32_t x, uint32_t y, int r, int g, int b) {
    const uint8_t* p = img.pixel(x, y);
    return p[0] == r && p[1] == g && p[2] == b;
  };
  CHECK(is(1, 1, 255, 0, 0));
  CHECK(is(6, 1, 0, 255, 0));
  CHECK(is(1, 6, 0, 0, 255));
  CHECK(is(6, 6, 255, 255, 255));
}

TEST_CASE("render: clip rect scissors drawing") {
  DrawList dl;
  dl.reset(16, 16);
  dl.pushClip({4, 4, 8, 8});
  dl.addRectFilled({0, 0, 16, 16}, rgba(255, 255, 255));
  dl.popClip();
  const Image img = draw(dl, rgba(0, 0, 0));
  CHECK(img.pixel(5, 5)[0] == 255);
  CHECK(img.pixel(3, 5)[0] == 0);
  CHECK(img.pixel(8, 5)[0] == 0);
  CHECK(img.pixel(5, 8)[0] == 0);
}

TEST_CASE("render: AA edge on a half-pixel boundary is half coverage") {
  DrawList dl;
  dl.reset(16, 8);
  dl.addRoundedRectFilled({2.5f, -4.0f, 10.5f, 12.0f}, 0.0f, rgba(255, 255, 255));
  const Image img = draw(dl, rgba(0, 0, 0));
  CHECK(img.pixel(1, 4)[0] == 0);
  CHECK(near(img.pixel(2, 4)[0], 128, 2));   // pixel centre 2.5 sits on the edge
  CHECK(img.pixel(3, 4)[0] == 255);
  CHECK(img.pixel(9, 4)[0] == 255);
  CHECK(near(img.pixel(10, 4)[0], 128, 2));
  CHECK(img.pixel(11, 4)[0] == 0);
}

TEST_CASE("render: glow blob peaks at its centre and falls to the background") {
  DrawList dl;
  dl.reset(64, 64);
  dl.setBlend(BlendMode::Additive);
  dl.addBlob({32, 32}, 24, rgba(255, 255, 255));
  const Image img = draw(dl, rgba(0, 0, 0));
  CHECK(img.pixel(32, 32)[0] > 230);
  CHECK(img.pixel(32 + 12, 32)[0] < img.pixel(32 + 4, 32)[0]);
  CHECK(img.pixel(32 + 23, 32)[0] <= 3);
  CHECK(img.pixel(2, 2)[0] == 0);
}

namespace {

// Renders `scene` at the default logical size and UI scale `scale` and compares it with
// tests/ui/golden/<scene>.png (scale 1) or <scene>@<scaleName>x.png (T0.7-fix3 HiDPI goldens).
void checkGolden(const std::string& scene, float scale = 1.0f, const char* scaleName = nullptr) {
  const std::string file = scaleName ? scene + "@" + scaleName + "x" : scene;
  const std::string golden = std::string(PQ_TEST_DATA_DIR) + "/ui/golden/" + file + ".png";
  GpuOptions o;
  o.validation = true;
  auto img = renderSceneImage(scene, kDefaultSceneWidth, kDefaultSceneHeight, o, UiScale(scale));
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  auto ref = readPng(golden);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));

  // ≤ 2/255 per channel everywhere except ≤ 0.1 % of pixels, which may differ more: rasterisation rules
  // and texture filtering precision legitimately vary a little between GPU vendors and drivers.
  const ImageDiff d = compareImages(*img, *ref, 2);
  const uint64_t allowedOutliers = d.pixelCount / 1000;
  if (!d.sameSize || d.pixelsOverTolerance > allowedOutliers) {
    const std::string out = file + ".actual.png";  // in the test's working directory (build/<preset>/tests)
    (void)writePng(out, *img);
    MESSAGE("wrote " << out << " for inspection");
  }
  REQUIRE(d.sameSize);
  MESSAGE(file << ": max channel diff " << d.maxChannelDiff << ", " << d.pixelsOverTolerance << " px over tolerance");
  CHECK(d.pixelsOverTolerance <= allowedOutliers);
}

}  // namespace

TEST_CASE("render: test-pattern matches the golden image") { checkGolden("test-pattern"); }

// T0.8: Inter through FreeType + HarfBuzz, glyphs uploaded into the atlas page by Renderer::upload.
TEST_CASE("render: text matches the golden image") { checkGolden("text"); }

// T0.7-fix3: the same scenes at Windows' common display scales. Device size = 800×480 logical × scale; text
// at round(role · scale) px hinted fonts, hairlines and AA feathers 1 device px.
TEST_CASE("render: text matches the golden image at UI scale 1.25") { checkGolden("text", 1.25f, "1.25"); }
TEST_CASE("render: text matches the golden image at UI scale 1.5") { checkGolden("text", 1.5f, "1.5"); }
TEST_CASE("render: text matches the golden image at UI scale 2.0") { checkGolden("text", 2.0f, "2.0"); }
TEST_CASE("render: test-pattern matches the golden image at UI scale 1.25") { checkGolden("test-pattern", 1.25f, "1.25"); }
TEST_CASE("render: test-pattern matches the golden image at UI scale 1.5") { checkGolden("test-pattern", 1.5f, "1.5"); }
TEST_CASE("render: test-pattern matches the golden image at UI scale 2.0") { checkGolden("test-pattern", 2.0f, "2.0"); }

TEST_CASE("render: scaled screenshots have the scaled device size") {
  auto img = renderSceneImage("text", 333, 217, {}, UiScale(1.5f));
  REQUIRE(img.ok());
  CHECK(img->width == 500);  // 499.5 rounds up
  CHECK(img->height == 326);  // 325.5
}

// T0.9: PQ Meter's line primitives (ribbons, glow passes incl. additive, spectrum layering, colormaps).
TEST_CASE("render: lines matches the golden image") { checkGolden("lines"); }

// T3.1: widget tree (rows/columns, splitters, scroll view over virtualised content) painted by UiTree.
TEST_CASE("render: widgets-core matches the golden image") { checkGolden("widgets-core"); }

// T0.7-fix2: widget-grade primitives (crisp strokes, separators, arcs, circles, triangles, bilinear gradient).
TEST_CASE("render: primitives matches the golden image") { checkGolden("primitives"); }

TEST_CASE("render: a 1 px crisp stroke at integer coordinates is exactly one pixel column at full alpha") {
  // White on black; columns either side must be untouched, the stroke column fully white along its length.
  const Rgba white = rgba(255, 255, 255);
  auto checkColumn = [](const Image& img, uint32_t x, uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
      CAPTURE(x);
      CAPTURE(y);
      CHECK(img.pixel(x, y)[0] == 255);
      CHECK(img.pixel(x - 1, y)[0] == 0);
      CHECK(img.pixel(x + 1, y)[0] == 0);
    }
  };
  {
    DrawList dl;
    dl.reset(64, 64);
    dl.addVLine(10.0f, 4.0f, 60.0f, white);  // integer x: column 10
    dl.addVLine(20.5f, 4.0f, 60.0f, white);  // pixel-centre x: column 20 as well
    const Image img = draw(dl, rgba(0, 0, 0));
    checkColumn(img, 10, 4, 60);
    checkColumn(img, 20, 4, 60);
    CHECK(img.pixel(10, 3)[0] == 0);
    CHECK(img.pixel(10, 60)[0] == 0);
  }
  {
    // Rect border: the left side is column 8 only; the interior next to it stays background.
    DrawList dl;
    dl.reset(64, 64);
    dl.addRectStroke({8, 8, 40, 40}, white);
    const Image img = draw(dl, rgba(0, 0, 0));
    checkColumn(img, 8, 9, 39);  // rows 8 and 39 belong to the horizontal sides
    checkColumn(img, 39, 9, 39);
    for (uint32_t x = 8; x < 40; ++x) {
      CHECK(img.pixel(x, 7)[0] == 0);
      CHECK(img.pixel(x, 8)[0] == 255);
      CHECK(img.pixel(x, 39)[0] == 255);
      CHECK(img.pixel(x, 40)[0] == 0);
    }
    for (uint32_t x = 9; x < 39; ++x) {
      CHECK(img.pixel(x, 9)[0] == 0);
      CHECK(img.pixel(x, 38)[0] == 0);
    }
  }
  {
    // Rounded border: along its straight sides it is just as crisp (the AA ramp only shows at the corners).
    DrawList dl;
    dl.reset(64, 64);
    dl.addRoundedRectStroke({8, 8, 48, 48}, 6.0f, white);
    const Image img = draw(dl, rgba(0, 0, 0));
    checkColumn(img, 8, 8 + 6, 48 - 6);
    checkColumn(img, 47, 8 + 6, 48 - 6);
    for (uint32_t x = 8 + 6; x < 48 - 6; ++x) {
      CHECK(img.pixel(x, 8)[0] == 255);
      CHECK(img.pixel(x, 7)[0] == 0);
      CHECK(img.pixel(x, 9)[0] == 0);
    }
    // ... and the corner is antialiased: some partially covered pixels around the arc.
    uint32_t partial = 0;
    for (uint32_t y = 8; y < 14; ++y)
      for (uint32_t x = 8; x < 14; ++x) partial += (img.pixel(x, y)[0] > 0 && img.pixel(x, y)[0] < 255) ? 1u : 0u;
    CHECK(partial >= 4);
  }
}

TEST_CASE("render: the 4-corner gradient is bilinear (centre pixel within 2/255)") {
  // Corners chosen for a large twist in every channel (red/green/blue/alpha-free): the old two-triangle
  // quad was off by up to twist/4 ≈ 60/255 at the centre.
  const Rgba tl = rgba(240, 20, 30), tr = rgba(20, 230, 40), br = rgba(250, 10, 220), bl = rgba(10, 240, 200);
  DrawList dl;
  dl.reset(64, 64);
  dl.addRectFilledMultiColor({0, 0, 64, 64}, tl, tr, br, bl);
  const Image img = draw(dl, rgba(0, 0, 0));
  auto bilinear = [&](float u, float v, uint32_t ch) {
    auto c = [ch](Rgba x) { return static_cast<float>((x >> (8 * ch)) & 0xFFu); };
    const float top = c(tl) + (c(tr) - c(tl)) * u, bot = c(bl) + (c(br) - c(bl)) * u;
    return top + (bot - top) * v;
  };
  // Centre pixel (32, 32): its sample point is (32.5, 32.5).
  for (uint32_t ch = 0; ch < 3; ++ch) {
    CAPTURE(ch);
    CHECK(std::fabs(static_cast<float>(img.pixel(32, 32)[ch]) - bilinear(32.5f / 64, 32.5f / 64, ch)) <= 2.0f);
  }
  // Everywhere else as well (the fan grid's bound is 1/255 + vertex rounding).
  float worst = 0.0f;
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < 64; ++x)
      for (uint32_t ch = 0; ch < 3; ++ch) {
        const float want = bilinear((static_cast<float>(x) + 0.5f) / 64, (static_cast<float>(y) + 0.5f) / 64, ch);
        worst = std::max(worst, std::fabs(static_cast<float>(img.pixel(x, y)[ch]) - want));
      }
  MESSAGE("4-corner gradient: worst channel error " << worst);
  CHECK(worst <= 2.0f);
}

TEST_CASE("render: circles, arcs and triangles cover their area") {
  DrawList dl;
  dl.reset(64, 64);
  dl.addCircleFilled({16, 16}, 10, rgba(255, 255, 255));
  dl.addArcStroke({48, 16}, 10, 0.0f, std::numbers::pi_v<float>, rgba(255, 255, 255), 4.0f);  // lower half
  dl.addTriangleFilled({8, 40}, {24, 40}, {16, 56}, rgba(255, 255, 255));
  const Image img = draw(dl, rgba(0, 0, 0));
  // Total coverage ≈ area (box-filter AA keeps it within a few % of the analytic value).
  auto coverage = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    double s = 0;
    for (uint32_t y = y0; y < y1; ++y)
      for (uint32_t x = x0; x < x1; ++x) s += img.pixel(x, y)[0] / 255.0;
    return s;
  };
  const double pi = std::numbers::pi;
  CHECK(coverage(0, 0, 32, 32) == doctest::Approx(pi * 100.0).epsilon(0.02));
  CHECK(coverage(32, 0, 64, 32) == doctest::Approx(0.5 * pi * (144.0 - 64.0)).epsilon(0.03));
  CHECK(coverage(0, 32, 32, 64) == doctest::Approx(0.5 * 16.0 * 16.0).epsilon(0.03));
  CHECK(img.pixel(16, 16)[0] == 255);
  CHECK(img.pixel(48, 26)[0] == 255);  // bottom of the arc, on its mid-line
  CHECK(img.pixel(48, 6)[0] == 0);     // top half not drawn
}

TEST_CASE("render: glyphs added after the first frame reach the GPU") {
  // The atlas page starts with ASCII only; a new glyph must be uploaded by the next draw.
  auto text = TextSystem::create(gpu().renderer->atlas());
  REQUIRE(text.ok());
  auto face = (*text)->loadFace(assetsDir() + "/fonts/Inter-Regular.ttf");
  REQUIRE(face.ok());
  auto font = (*text)->font(*face, 40);
  REQUIRE(font.ok());
  for (int frame = 0; frame < 2; ++frame) {
    DrawList dl;
    dl.reset(64, 64);
    // Frame 0: ASCII 'I' (rasterised when the font was created). Frame 1: '∞', rasterised now.
    (*text)->draw(dl, *font, {8, 48}, frame == 0 ? "I" : "\xE2\x88\x9E", rgba(255, 255, 255));
    const Image img = draw(dl, rgba(0, 0, 0));
    uint32_t lit = 0;
    for (uint32_t y = 0; y < 64; ++y)
      for (uint32_t x = 0; x < 64; ++x) lit += img.pixel(x, y)[0] > 128 ? 1u : 0u;
    CAPTURE(frame);
    CHECK(lit > 40);
  }
}

TEST_CASE("render: offscreen output is deterministic and any size works") {
  auto a = renderSceneImage("test-pattern", 333, 217);
  auto b = renderSceneImage("test-pattern", 333, 217);
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  CHECK(a->width == 333);
  CHECK(a->height == 217);
  CHECK(compareImages(*a, *b, 0).maxChannelDiff == 0);
  CHECK_FALSE(renderSceneImage("no-such-scene", 64, 64).ok());
  CHECK_FALSE(renderSceneImage("test-pattern", 0, 64).ok());
}

TEST_CASE("render: glyphs on every atlas page sample their R8 coverage exactly (swizzle 1,1,1,R)") {
  // A large font overflows page 0 while warming ASCII, so some glyphs land on page 1.
  auto r = Renderer::create(*gpu().ctx, OffscreenTarget::kFormat, 1);
  REQUIRE(r.ok());
  Renderer& rr = **r;
  auto text = TextSystem::create(rr.atlas());
  REQUIRE(text.ok());
  auto face = (*text)->loadFace(assetsDir() + "/fonts/Inter-Regular.ttf");
  REQUIRE(face.ok());
  auto font = (*text)->font(*face, 180);
  REQUIRE(font.ok());
  REQUIRE(rr.atlas()->pageCount() >= 2);
  // One glyph from page 0 and one from the last page, each drawn white on black: pixel = coverage.
  const uint32_t last = rr.atlas()->pageCount() - 1;
  std::string onFirst, onLast;
  for (char c = '!'; c <= '~'; ++c) {
    const TextRun g = (*text)->shape(*font, std::string(1, c));
    if (g.glyphs.size() != 1) continue;
    if (g.glyphs[0].page == 0 && onFirst.empty()) onFirst = std::string(1, c);
    if (g.glyphs[0].page == last && onLast.empty()) onLast = std::string(1, c);
  }
  REQUIRE_FALSE(onFirst.empty());
  REQUIRE_FALSE(onLast.empty());
  for (const std::string& s : {onFirst, onLast}) {
    CAPTURE(s);
    const TextRun run = (*text)->shape(*font, s);
    const GlyphQuad& q = run.glyphs[0];
    DrawList dl;
    dl.reset(256, 256);
    dl.addRectFilled({0, 0, 2, 2}, rgba(0, 0, 0));  // a fill first: the glyph joins its command
    const Vec2 origin{static_cast<float>(16 - q.x0), static_cast<float>(16 - q.y0)};
    TextSystem::drawRun(dl, run, origin, rgba(255, 255, 255));
    CHECK(dl.commands().size() == 1);
    CHECK(dl.commands()[0].texture == atlasPageTexture(q.page));
    auto target = OffscreenTarget::create(*gpu().ctx, 256, 256);
    REQUIRE(target.ok());
    REQUIRE((*target)->draw(rr, dl, rgba(0, 0, 0)).ok());
    auto img = (*target)->read();
    REQUIRE(img.ok());
    const uint32_t ax = static_cast<uint32_t>(q.uv0.x * kAtlasWidth + 0.5f), ay = static_cast<uint32_t>(q.uv0.y * kAtlasHeight + 0.5f);
    int worst = 0;
    uint32_t lit = 0;
    for (uint32_t y = 0; y < q.h; ++y)
      for (uint32_t x = 0; x < q.w; ++x) {
        const int cov = *rr.atlas()->pageImage(q.page).at(ax + x, ay + y);
        const int px = img->pixel(16 + x, 16 + y)[0];
        worst = std::max(worst, std::abs(cov - px));
        lit += cov > 128 ? 1u : 0u;
      }
    CHECK(lit > 100);
    CHECK(worst <= 1);
  }
}

TEST_CASE("render: the scene fonts at 5 scales fit one R8 page: atlas memory 1/4 of the old RGBA page") {
  auto r = Renderer::create(*gpu().ctx, OffscreenTarget::kFormat, 1);
  REQUIRE(r.ok());
  Renderer& rr = **r;
  auto t = createSceneTextures(rr);
  REQUIRE(t.ok());
  for (float f : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) {
    REQUIRE(setSceneScale(*t, UiScale(f)).ok());
    t->text->beginFrame();
    const UiScale s(f);
    DrawList dl;
    const uint32_t w = static_cast<uint32_t>(s.px(800)), h = static_cast<uint32_t>(s.px(480));
    dl.reset(w, h);
    REQUIRE(buildScene("text", dl, *t, 0.0));
    auto target = OffscreenTarget::create(*gpu().ctx, w, h);
    REQUIRE(target.ok());
    REQUIRE((*target)->draw(rr, dl, kSceneClearColor).ok());
  }
  CHECK(t->text->stats().atlasFull == 0);
  CHECK(rr.atlas()->pageCount() == 1);
  constexpr VkDeviceSize kOldRgbaPage = VkDeviceSize(kAtlasWidth) * kAtlasHeight * 4;  // T0.8: one RGBA8 page
  const VkDeviceSize gpuBytes = rr.atlasGpuBytes();
  MESSAGE("glyph atlas GPU memory for 7 roles x 5 scales: " << gpuBytes << " bytes (old RGBA page: " << kOldRgbaPage << ")");
  CHECK(gpuBytes > 0);
  CHECK(gpuBytes * 4 <= kOldRgbaPage);
  CHECK(rr.atlas()->memoryBytes() * 4 <= kOldRgbaPage);
  // A new glyph uploads only its own rect (one byte per texel), not whole rows.
  const uint64_t before = rr.stats().atlasBytesStaged;
  const TextRun inf = t->text->shape(t->fonts.body, "\xE2\x88\x9E");  // ∞
  REQUIRE(inf.glyphs.size() == 1);
  DrawList dl;
  dl.reset(64, 64);
  TextSystem::drawRun(dl, inf, {10, 40}, rgba(255, 255, 255));
  auto target = OffscreenTarget::create(*gpu().ctx, 64, 64);
  REQUIRE(target.ok());
  REQUIRE((*target)->draw(rr, dl, rgba(0, 0, 0)).ok());
  CHECK(rr.stats().atlasBytesStaged - before == uint64_t(inf.glyphs[0].w) * inf.glyphs[0].h);
}

TEST_CASE("render: updateTexture lands in the next frame; overlapping updates apply in order") {
  auto r = Renderer::create(*gpu().ctx, OffscreenTarget::kFormat, 1);
  REQUIRE(r.ok());
  Renderer& rr = **r;
  auto solid = [](uint32_t w, uint32_t h, Rgba c) {
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
      img.rgba[i] = static_cast<uint8_t>(redOf(c));
      img.rgba[i + 1] = static_cast<uint8_t>(greenOf(c));
      img.rgba[i + 2] = static_cast<uint8_t>(blueOf(c));
      img.rgba[i + 3] = static_cast<uint8_t>(alphaOf(c));
    }
    return img;
  };
  auto id = rr.createTexture(solid(4, 4, rgba(255, 0, 0)), TextureFilter::Nearest);
  REQUIRE(id.ok());
  CHECK(*id >= kFirstUserTexture);
  DrawList dl;
  dl.reset(8, 8);
  dl.addImage(*id, {0, 0, 8, 8}, {0, 0}, {1, 1});  // texel (i, j) → pixels [2i, 2i+2) × [2j, 2j+2)
  auto target = OffscreenTarget::create(*gpu().ctx, 8, 8);
  REQUIRE(target.ok());
  auto frame = [&] {
    REQUIRE((*target)->draw(rr, dl, rgba(0, 0, 0)).ok());
    auto img = (*target)->read();
    REQUIRE(img.ok());
    return std::move(*img);
  };
  auto is = [](const Image& img, uint32_t x, uint32_t y, Rgba c) {
    const uint8_t* p = img.pixel(x, y);
    return p[0] == redOf(c) && p[1] == greenOf(c) && p[2] == blueOf(c);
  };
  Image img = frame();
  CHECK(is(img, 1, 1, rgba(255, 0, 0)));
  // 2×2 green at texel (2, 2).
  const Image green = solid(2, 2, rgba(0, 255, 0));
  REQUIRE(rr.updateTexture(*id, {2, 2, 2, 2}, green.rgba).ok());
  img = frame();
  CHECK(is(img, 1, 1, rgba(255, 0, 0)));
  CHECK(is(img, 5, 5, rgba(0, 255, 0)));
  CHECK(is(img, 7, 7, rgba(0, 255, 0)));
  // Same frame: whole texture blue, then texel (1,1) white (overlap → ordered by a barrier), then a 1×1
  // update read from a padded row (rowPitch).
  REQUIRE(rr.updateTexture(*id, {0, 0, 4, 4}, solid(4, 4, rgba(0, 0, 255)).rgba).ok());
  REQUIRE(rr.updateTexture(*id, {1, 1, 1, 1}, solid(1, 1, rgba(255, 255, 255)).rgba).ok());
  const std::vector<uint8_t> padded = {9, 9, 9, 9, 0, 255, 255, 255, 9, 9, 9, 9, 0, 0, 0, 0};
  REQUIRE(rr.updateTexture(*id, {3, 0, 1, 1}, std::span<const uint8_t>(padded).subspan(4), 12).ok());
  img = frame();
  CHECK(is(img, 0, 0, rgba(0, 0, 255)));
  CHECK(is(img, 2, 2, rgba(255, 255, 255)));
  CHECK(is(img, 3, 3, rgba(255, 255, 255)));
  CHECK(is(img, 5, 5, rgba(0, 0, 255)));
  CHECK(is(img, 6, 0, rgba(0, 255, 255)));
  // Errors: not a user texture, outside the texture, too little data.
  CHECK_FALSE(rr.updateTexture(kAtlasTexture, {0, 0, 1, 1}, green.rgba).ok());
  CHECK_FALSE(rr.updateTexture(atlasPageTexture(0), {0, 0, 1, 1}, green.rgba).ok());
  CHECK_FALSE(rr.updateTexture(*id, {3, 3, 2, 2}, green.rgba).ok());
  CHECK_FALSE(rr.updateTexture(*id, {0, 0, 4, 4}, green.rgba).ok());
  // Destroy is deferred to the end of the frames that may use it; the id is then reused.
  CHECK(rr.liveTextures() == 1);
  REQUIRE(rr.destroyTexture(*id).ok());
  CHECK_FALSE(rr.destroyTexture(*id).ok());
  CHECK_FALSE(rr.updateTexture(*id, {0, 0, 1, 1}, green.rgba).ok());
  CHECK(rr.liveTextures() == 0);
  CHECK(rr.retiringTextures() == 1);
  DrawList empty;
  empty.reset(8, 8);
  REQUIRE((*target)->draw(rr, empty, rgba(0, 0, 0)).ok());
  CHECK(rr.retiringTextures() == 0);
  auto again = rr.createTexture(solid(2, 2, rgba(1, 2, 3)), TextureFilter::Linear);
  REQUIRE(again.ok());
  CHECK(*again == *id);
}

TEST_CASE("render: creating and destroying 10k textures leaks nothing") {
  auto r = Renderer::create(*gpu().ctx, OffscreenTarget::kFormat, 2);
  REQUIRE(r.ok());
  Renderer& rr = **r;
  auto target = OffscreenTarget::create(*gpu().ctx, 64, 64, 2);
  REQUIRE(target.ok());
  // 80 per round: with two frames in flight a round's textures retire two frames after it, so at most three
  // rounds' ids (240) are taken at once (the table holds Renderer::kMaxUserTextures).
  constexpr uint32_t kPerRound = 80, kRounds = 125;
  static_assert(3 * kPerRound <= Renderer::kMaxUserTextures);
  uint32_t created = 0, maxId = 0;
  auto round = [&](uint32_t seed) {
    std::vector<TextureId> ids;
    DrawList dl;
    dl.reset(64, 64);
    for (uint32_t i = 0; i < kPerRound; ++i) {
      Image img;
      img.width = 1 + (seed + i) % 9;
      img.height = 1 + (seed * 3 + i) % 7;
      img.rgba.assign(size_t(img.width) * img.height * 4, static_cast<uint8_t>(seed + i));
      auto id = rr.createTexture(img, (i & 1) ? TextureFilter::Linear : TextureFilter::Nearest);
      REQUIRE(id.ok());
      maxId = std::max(maxId, *id);
      ids.push_back(*id);
      dl.addImage(*id, {static_cast<float>(i % 8) * 8, static_cast<float>(i / 8 % 8) * 8, static_cast<float>(i % 8) * 8 + 8,
                        static_cast<float>(i / 8 % 8) * 8 + 8},
                  {0, 0}, {1, 1});
      ++created;
    }
    REQUIRE((*target)->submit(rr, dl, rgba(0, 0, 0)).ok());
    for (TextureId id : ids) REQUIRE(rr.destroyTexture(id).ok());
  };
  DrawList empty;
  empty.reset(64, 64);
  auto drain = [&] {
    for (uint32_t i = 0; i < 2; ++i) REQUIRE((*target)->submit(rr, empty, rgba(0, 0, 0)).ok());
    REQUIRE((*target)->finish().ok());
  };
  // Baseline after a few rounds, so the staging ring and draw buffers have reached their working size.
  for (uint32_t k = 0; k < 3; ++k) round(k);
  drain();
  const GpuContext::MemoryStats base = gpu().ctx->memoryStats();
  REQUIRE(rr.liveTextures() == 0);
  for (uint32_t k = 3; k < 3 + kRounds; ++k) round(k);
  drain();
  const GpuContext::MemoryStats after = gpu().ctx->memoryStats();
  MESSAGE(created << " textures created and destroyed; highest id " << maxId << "; VMA " << base.allocations << " allocations / "
                  << base.bytes << " bytes before, " << after.allocations << " / " << after.bytes << " after");
  CHECK(created - 3 * kPerRound == 10000);
  CHECK(rr.liveTextures() == 0);
  CHECK(rr.retiringTextures() == 0);
  CHECK(maxId < Renderer::kMaxTextures);
  CHECK(after.allocations == base.allocations);
  CHECK(after.bytes == base.bytes);
}

TEST_CASE("render: a 1024x512 spectrogram scrolls one column per frame at 144 Hz without the CPU waiting") {
  using Clock = std::chrono::steady_clock;
  constexpr uint32_t kW = 1024, kH = 512, kFrames = 288;  // 2 s at 144 Hz
  auto texel = [](uint32_t x, uint32_t y) {
    return std::array<uint8_t, 4>{static_cast<uint8_t>(x * 7), static_cast<uint8_t>(y), static_cast<uint8_t>(x + y), 255};
  };
  uint32_t stalls = 0;
  double medianUs = 0, maxUs = 0;
  bool contentOk = false;
  for (int attempt = 0; attempt < 2; ++attempt) {  // one retry: the machine is shared with other builds
    auto r = Renderer::create(*gpu().ctx, OffscreenTarget::kFormat, 2);
    REQUIRE(r.ok());
    Renderer& rr = **r;
    auto target = OffscreenTarget::create(*gpu().ctx, kW, kH, 2);
    REQUIRE(target.ok());
    Image black;
    black.width = kW;
    black.height = kH;
    black.rgba.assign(size_t(kW) * kH * 4, 0);
    for (size_t i = 3; i < black.rgba.size(); i += 4) black.rgba[i] = 255;
    auto tex = rr.createTexture(black, TextureFilter::Nearest);
    REQUIRE(tex.ok());
    DrawList dl;
    dl.reset(kW, kH);
    dl.addImage(*tex, {0, 0, static_cast<float>(kW), static_cast<float>(kH)}, {0, 0}, {1, 1});
    std::vector<uint8_t> column(kH * 4);
    std::vector<double> cpuUs;
    const auto period = std::chrono::nanoseconds(1'000'000'000 / 144);
    auto next = Clock::now();
    for (uint32_t f = 0; f < kFrames; ++f) {
      for (uint32_t y = 0; y < kH; ++y) std::memcpy(&column[y * 4], texel(f, y).data(), 4);
      const auto t0 = Clock::now();
      REQUIRE(rr.updateTexture(*tex, {f, 0, 1, kH}, column).ok());
      REQUIRE((*target)->submit(rr, dl, rgba(0, 0, 0)).ok());
      cpuUs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
      next += period;
      while (Clock::now() < next) std::this_thread::yield();
    }
    stalls = (*target)->slotStalls();
    std::sort(cpuUs.begin(), cpuUs.end());
    medianUs = cpuUs[cpuUs.size() / 2];
    maxUs = cpuUs.back();
    auto img = (*target)->read();
    REQUIRE(img.ok());
    contentOk = true;
    for (uint32_t x = 0; x < kW && contentOk; x += (x < kFrames ? 1 : 37))
      for (uint32_t y = 0; y < kH; y += 13) {
        const uint8_t* p = img->pixel(x, y);
        const auto want = x < kFrames ? texel(x, y) : std::array<uint8_t, 4>{0, 0, 0, 255};
        if (p[0] != want[0] || p[1] != want[1] || p[2] != want[2]) {
          contentOk = false;
          break;
        }
      }
    if (stalls == 0) break;
  }
  MESSAGE("spectrogram column update + frame submit: median " << medianUs << " us, max " << maxUs << " us per frame; "
                                                              << stalls << " frames waited for the GPU");
  CHECK(contentOk);
  CHECK(stalls == 0);
  CHECK(medianUs < 1000.0);  // a small fraction of the 6.9 ms frame
}

TEST_CASE("render: no Vulkan validation errors") {
  // Runs last in this file (doctest runs in file order): covers every draw above.
  CHECK(gpu().ctx->validationErrorCount() == 0);
}
