// GPU tests: the offscreen path renders through the real Vulkan pipelines (headless, no window).

#include <doctest/doctest.h>

#include <cstdio>
#include <memory>
#include <string>

#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"

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

TEST_CASE("render: test-pattern matches the golden image") {
  const std::string golden = std::string(PQ_TEST_DATA_DIR) + "/ui/golden/test-pattern.png";
  GpuOptions o;
  o.validation = true;
  auto img = renderSceneImage("test-pattern", kDefaultSceneWidth, kDefaultSceneHeight, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  auto ref = readPng(golden);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));

  // ≤ 2/255 per channel everywhere except ≤ 0.1 % of pixels, which may differ more: rasterisation rules
  // and texture filtering precision legitimately vary a little between GPU vendors and drivers.
  const ImageDiff d = compareImages(*img, *ref, 2);
  const uint64_t allowedOutliers = d.pixelCount / 1000;
  if (!d.sameSize || d.pixelsOverTolerance > allowedOutliers) {
    const char* out = "test-pattern.actual.png";  // in the test's working directory (build/<preset>/tests)
    (void)writePng(out, *img);
    MESSAGE("wrote " << out << " for inspection");
  }
  REQUIRE(d.sameSize);
  MESSAGE("max channel diff " << d.maxChannelDiff << ", " << d.pixelsOverTolerance << " px over tolerance");
  CHECK(d.pixelsOverTolerance <= allowedOutliers);
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

TEST_CASE("render: no Vulkan validation errors") {
  // Runs last in this file (doctest runs in file order): covers every draw above.
  CHECK(gpu().ctx->validationErrorCount() == 0);
}
