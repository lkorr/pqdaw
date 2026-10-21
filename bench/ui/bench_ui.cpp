// ui benchmarks: draw-list build cost per primitive, and offscreen frame cost (upload + record + submit +
// GPU wait) for the test pattern and a dense frame; streaming texture updates, texture create/destroy and
// glyph uploads (T0.8-fix1 / T0.7-fix4). Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "pq/ui/color.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"
#include "pq/ui/text.h"
#include "pq_bench.h"

using namespace pq::ui;

namespace {

constexpr int kN = 10000;

void rects(DrawList& dl) {
  dl.reset(1920, 1080);
  for (int i = 0; i < kN; ++i) {
    const float x = static_cast<float>(i % 100) * 19.0f, y = static_cast<float>(i / 100) * 10.0f;
    dl.addRectFilled({x, y, x + 17, y + 8}, 0xFF804020u + static_cast<uint32_t>(i));
  }
}

void roundedRects(DrawList& dl) {
  dl.reset(1920, 1080);
  for (int i = 0; i < kN; ++i) {
    const float x = static_cast<float>(i % 100) * 19.0f + 0.25f, y = static_cast<float>(i / 100) * 10.0f + 0.5f;
    dl.addRoundedRectFilled({x, y, x + 17, y + 8}, 3.0f, 0xFF804020u + static_cast<uint32_t>(i));
  }
}

void blobs(DrawList& dl) {
  dl.reset(1920, 1080);
  dl.setBlend(BlendMode::Additive);
  for (int i = 0; i < kN; ++i) {
    const float x = static_cast<float>(i % 100) * 19.0f, y = static_cast<float>(i / 100) * 10.0f;
    dl.addBlob({x, y}, 12.0f, 0x80FFC060u);
  }
}

}  // namespace

int main() {
  DrawList dl;
  SceneTextures tex;  // ids only matter on the GPU; build cost is identical

  auto s = pq::bench::measure([&] { rects(dl); pq::bench::doNotOptimize(dl.indices().size()); }, kN);
  pq::bench::report("drawlist/addRectFilled", s, "ns/rect");
  s = pq::bench::measure([&] { roundedRects(dl); pq::bench::doNotOptimize(dl.indices().size()); }, kN);
  pq::bench::report("drawlist/addRoundedRectFilled r=3", s, "ns/rect");
  {
    // Cache-resident size (a typical widget frame): isolates compute from memory bandwidth.
    DrawList small;
    s = pq::bench::measure(
        [&] {
          small.reset(1920, 1080);
          for (int i = 0; i < 500; ++i) {
            const float x = static_cast<float>(i % 100) * 19.0f + 0.25f, y = static_cast<float>(i / 100) * 10.0f + 0.5f;
            small.addRoundedRectFilled({x, y, x + 17, y + 8}, 3.0f, 0xFF804020u);
          }
          pq::bench::doNotOptimize(small.indices().size());
        },
        500);
    pq::bench::report("drawlist/addRoundedRectFilled r=3 (500, in cache)", s, "ns/rect");
  }
  std::printf("  (rounded r=3: %u vertices, %u indices per rect)\n", static_cast<unsigned>(dl.vertices().size() / kN),
              static_cast<unsigned>(dl.indices().size() / kN));
  s = pq::bench::measure([&] { blobs(dl); pq::bench::doNotOptimize(dl.indices().size()); }, kN);
  pq::bench::report("drawlist/addBlob", s, "ns/blob");
  s = pq::bench::measure(
      [&] {
        dl.reset(kDefaultSceneWidth, kDefaultSceneHeight);
        buildScene("test-pattern", dl, tex, 0.0);
        pq::bench::doNotOptimize(dl.indices().size());
      },
      1);
  pq::bench::report("drawlist/test-pattern build 800x480", s, "ns/frame");
  std::printf("  (test-pattern: %u vertices, %u indices, %u commands)\n", static_cast<uint32_t>(dl.vertices().size()),
              static_cast<uint32_t>(dl.indices().size()), static_cast<uint32_t>(dl.commands().size()));

  // T0.7-fix2 widget primitives: 1000 per frame (a busy widget frame, cache-resident), widget-typical sizes.
  {
    constexpr int kP = 1000;
    DrawList w;
    auto prim = [&](const char* name, auto&& add) {
      auto st = pq::bench::measure(
          [&] {
            w.reset(1920, 1080);
            for (int i = 0; i < kP; ++i) add(static_cast<float>(i % 40) * 46.0f + 20.25f, static_cast<float>(i / 40) * 42.0f + 20.5f, i);
            pq::bench::doNotOptimize(w.indices().size());
          },
          kP);
      char unit[96];
      std::snprintf(unit, sizeof unit, "ns/prim (%u vtx, %u idx)", static_cast<unsigned>(w.vertices().size() / kP),
                    static_cast<unsigned>(w.indices().size() / kP));
      pq::bench::report(name, st, unit);
    };
    const Rgba col = 0xFF2EA8FFu;
    prim("drawlist/addHLine 1px", [&](float x, float y, int) { w.addHLine(x - 18, x + 18, y, col); });
    prim("drawlist/addRectStroke 1px", [&](float x, float y, int) { w.addRectStroke({x - 18, y - 16, x + 18, y + 16}, col); });
    prim("drawlist/addRoundedRectStroke r=4 1px", [&](float x, float y, int) {
      w.addRoundedRectStroke({x - 18, y - 16, x + 18, y + 16}, 4.0f, col);
    });
    prim("drawlist/addCircleFilled r=6", [&](float x, float y, int) { w.addCircleFilled({x, y}, 6.0f, col); });
    prim("drawlist/addCircleStroke r=11 1px", [&](float x, float y, int) { w.addCircleStroke({x, y}, 11.0f, col); });
    prim("drawlist/addArcStroke knob r=17 270deg 3px butt", [&](float x, float y, int) {
      w.addArcStroke({x, y}, 17.0f, 2.356f, 2.356f + 4.712f, col, 3.0f);
    });
    prim("drawlist/addArcStroke mod r=21.5 90deg 2px round", [&](float x, float y, int i) {
      const float a = 2.4f + 0.001f * static_cast<float>(i);  // varying angles: no memo help
      w.addArcStroke({x, y}, 21.5f, a, a + 1.57f, col, 2.0f, LineCap::Round);
    });
    prim("drawlist/addTriangleFilled 8px", [&](float x, float y, int) {
      w.addTriangleFilled({x - 2.5f, y - 4}, {x + 2.5f, y}, {x - 2.5f, y + 4}, col);
    });
    prim("drawlist/addConvexPolyFilled hexagon", [&](float x, float y, int) {
      const Vec2 h[6] = {{x + 10, y}, {x + 5, y + 8.66f}, {x - 5, y + 8.66f}, {x - 10, y}, {x - 5, y - 8.66f}, {x + 5, y - 8.66f}};
      w.addConvexPolyFilled(h, col);
    });
    prim("drawlist/addRectFilledMultiColor linear", [&](float x, float y, int) {
      w.addRectFilledMultiColor({x - 18, y - 16, x + 18, y + 16}, 0xFF0000FFu, 0xFF00FF00u, 0xFF00FF00u, 0xFF0000FFu);
    });
    prim("drawlist/addRectFilledMultiColor bilinear (twist 510: worst case)", [&](float x, float y, int) {
      w.addRectFilledMultiColor({x - 18, y - 16, x + 18, y + 16}, 0xFF0000FFu, 0xFF00FF00u, 0xFF0000FFu, 0xFFFF0000u);
    });
  }
  s = pq::bench::measure(
      [&] {
        dl.reset(kDefaultSceneWidth, kDefaultSceneHeight);
        buildScene("primitives", dl, tex, 0.0);
        pq::bench::doNotOptimize(dl.indices().size());
      },
      1);
  pq::bench::report("drawlist/primitives scene build 800x480 (no text)", s, "ns/frame");
  std::printf("  (primitives: %u vertices, %u indices, %u commands)\n", static_cast<uint32_t>(dl.vertices().size()),
              static_cast<uint32_t>(dl.indices().size()), static_cast<uint32_t>(dl.commands().size()));

  auto gpu = GpuContext::create({});
  if (!gpu) {
    std::printf("GPU benchmarks skipped: %s\n", gpu.error().message.c_str());
    return 0;
  }
  auto renderer = Renderer::create(**gpu, OffscreenTarget::kFormat, 1);
  auto textures = renderer ? createSceneTextures(**renderer) : pq::Result<SceneTextures>(renderer.error());
  if (!renderer || !textures) {
    std::printf("renderer setup failed\n");
    return 1;
  }
  std::printf("GPU: %s\n", (*gpu)->deviceName().c_str());

  {
    auto target = OffscreenTarget::create(**gpu, kDefaultSceneWidth, kDefaultSceneHeight);
    if (!target) return 1;
    dl.reset(kDefaultSceneWidth, kDefaultSceneHeight);
    buildScene("test-pattern", dl, *textures, 0.0);
    s = pq::bench::measure([&] { (void)(*target)->draw(**renderer, dl, kSceneClearColor); }, 1, 15, 3, 2e7);
    pq::bench::report("offscreen/test-pattern 800x480 frame", s, "ns/frame (upload+record+submit+GPU wait)");
    s = pq::bench::measure([&] { (void)(*renderer)->upload(0, dl); }, 1);
    pq::bench::report("renderer/upload test-pattern", s, "ns/frame");
  }
  {
    auto target = OffscreenTarget::create(**gpu, 1920, 1080);
    if (!target) return 1;
    roundedRects(dl);
    s = pq::bench::measure([&] { (void)(*target)->draw(**renderer, dl, kSceneClearColor); }, 1, 15, 3, 2e7);
    pq::bench::report("offscreen/10k rounded rects 1920x1080", s, "ns/frame (upload+record+submit+GPU wait)");
    s = pq::bench::measure([&] { (void)(*renderer)->upload(0, dl); }, 1);
    pq::bench::report("renderer/upload 10k rounded rects", s, "ns/frame");
  }

  // --- T0.7-fix4: streaming updates and texture lifetime ------------------------------------------------
  {
    auto r2 = Renderer::create(**gpu, OffscreenTarget::kFormat, 2);
    auto target = OffscreenTarget::create(**gpu, 1024, 512, 2);
    if (!r2 || !target) return 1;
    Image spec;
    spec.width = 1024;
    spec.height = 512;
    spec.rgba.assign(size_t(1024) * 512 * 4, 0);
    auto specTex = (*r2)->createTexture(spec, TextureFilter::Nearest);
    if (!specTex) return 1;
    std::vector<uint8_t> column(512 * 4, 200);
    uint32_t x = 0;
    // 256 column updates, then a frame boundary (upload) that binds them and recycles the slot's chunks, as
    // a frame loop would; without frames the staging ring would only grow.
    DrawList none;
    none.reset(1024, 512);
    s = pq::bench::measure(
        [&] {
          for (int i = 0; i < 256; ++i) (void)(*r2)->updateTexture(*specTex, {x++ & 1023u, 0, 1, 512}, column);
          (void)(*r2)->upload(0, none);
        },
        256);
    pq::bench::report("renderer/updateTexture 1x512 column (staging)", s, "ns/update");
    dl.reset(1024, 512);
    dl.addImage(*specTex, {0, 0, 1024, 512}, {0, 0}, {1, 1});
    s = pq::bench::measure(
        [&] {
          (void)(*r2)->updateTexture(*specTex, {x++ & 1023u, 0, 1, 512}, column);
          (void)(*target)->submit(**r2, dl, kSceneClearColor);
        },
        1, 15, 3, 2e7);
    pq::bench::report("offscreen/spectrogram column frame, 2 in flight", s, "ns/frame (update+upload+record+submit)");
    std::printf("  (unpaced: %u of the frames above waited for their slot; at 144 Hz the test sees 0)\n", (*target)->slotStalls());
    (void)(*target)->finish();

    DrawList empty;
    empty.reset(1024, 512);
    Image small;
    small.width = small.height = 64;
    small.rgba.assign(size_t(64) * 64 * 4, 99);
    std::vector<TextureId> ids;
    s = pq::bench::measure(
        [&] {
          for (int i = 0; i < 64; ++i) ids.push_back(*(*r2)->createTexture(small, TextureFilter::Linear));
          (void)(*target)->submit(**r2, empty, kSceneClearColor);
          for (TextureId id : ids) (void)(*r2)->destroyTexture(id);
          ids.clear();
          (void)(*target)->submit(**r2, empty, kSceneClearColor);
          (void)(*target)->submit(**r2, empty, kSceneClearColor);
        },
        64);
    pq::bench::report("renderer/create+destroy 64x64 texture", s, "ns/texture (incl. 3 frames per 64)");
    (void)(*target)->finish();
  }

  // --- T0.8-fix1: a frame that introduces 200 new glyphs ------------------------------------------------
  {
    using Clock = std::chrono::steady_clock;
    auto target = OffscreenTarget::create(**gpu, kDefaultSceneWidth, kDefaultSceneHeight);
    if (!target) return 1;
    TextSystem& ts = *textures->text;
    // 200 Latin-1 / Latin Extended code points Inter has.
    std::vector<std::string> lines(4);
    {
      auto probe = ts.font(textures->regular, 13);
      uint32_t found = 0;
      for (uint32_t c = 0xC0; c < 0x250 && found < 200; ++c) {
        std::string u;
        u += static_cast<char>(0xC0 | (c >> 6));
        u += static_cast<char>(0x80 | (c & 0x3F));
        const auto g = ts.shapeGlyphs(*probe, u);
        if (g.size() != 1 || g[0].glyphId == 0) continue;
        lines[found / 50] += u;
        ++found;
      }
    }
    std::vector<double> rasterMs, frameMs, warmMs;
    std::vector<uint64_t> staged, ink;
    for (uint32_t size = 15; size < 35; ++size) {
      auto font = ts.font(textures->regular, size, {TextHinting::Light, false});  // fresh glyphs every round
      if (!font) return 1;
      ts.beginFrame();
      dl.reset(kDefaultSceneWidth, kDefaultSceneHeight);
      buildScene("text", dl, *textures, 0.0);
      (void)(*target)->draw(**renderer, dl, kSceneClearColor);  // uploads the new font's ASCII (and any eviction)
      ts.beginFrame();
      uint64_t inkTexels = 0;
      const auto t0 = Clock::now();
      for (size_t i = 0; i < lines.size(); ++i) ts.draw(dl, *font, {8, 40.0f + 30.0f * static_cast<float>(i)}, lines[i], 0xFFFFFFFFu);
      const auto t1 = Clock::now();
      for (const std::string& l : lines)
        for (const GlyphQuad& q : ts.shape(*font, l).glyphs) inkTexels += uint64_t(q.w) * q.h;
      const uint64_t before = (*renderer)->stats().atlasBytesStaged;
      (void)(*target)->draw(**renderer, dl, kSceneClearColor);
      const auto t2 = Clock::now();
      staged.push_back((*renderer)->stats().atlasBytesStaged - before);
      ink.push_back(inkTexels);
      (void)(*target)->draw(**renderer, dl, kSceneClearColor);  // same frame again, glyphs already resident
      const auto t3 = Clock::now();
      rasterMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
      frameMs.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
      warmMs.push_back(std::chrono::duration<double, std::milli>(t3 - t2).count());
    }
    auto med = [](std::vector<double> v) {
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    std::sort(staged.begin(), staged.end());
    std::sort(ink.begin(), ink.end());
    std::printf("BENCH %-40s median %10.4f  ms rasterise (FreeType+HarfBuzz) | frame with uploads %.4f ms | same frame warm %.4f ms | %llu atlas bytes staged for %llu glyph texels\n",
                "text/200 new glyphs in one frame", med(rasterMs), med(frameMs), med(warmMs),
                static_cast<unsigned long long>(staged[staged.size() / 2]), static_cast<unsigned long long>(ink[ink.size() / 2]));
  }
  return 0;
}
