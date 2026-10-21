// ui benchmarks: draw-list build cost per primitive, and offscreen frame cost (upload + record + submit +
// GPU wait) for the test pattern and a dense frame. Run from the release build; results → bench/RESULTS.md.

#include <cstdio>

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
