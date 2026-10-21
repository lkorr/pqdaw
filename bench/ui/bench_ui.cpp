// ui benchmarks: draw-list build cost per primitive, and offscreen frame cost (upload + record + submit +
// GPU wait) for the test pattern and a dense frame. Run from the release build; results → bench/RESULTS.md.

#include <cstdio>

#include "pq/ui/color.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"
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
  return 0;
}
