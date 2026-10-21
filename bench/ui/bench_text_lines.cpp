// T0.8 / T0.9 benchmarks: text shaping and drawing cost, PQ Meter line-primitive generation cost, and the
// text / lines scenes. Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/lines.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"
#include "pq/ui/text.h"
#include "pq_bench.h"

using namespace pq::ui;

namespace {

std::vector<Vec2> sineTrace(uint32_t n, float width) {
  std::vector<Vec2> p(n);
  for (uint32_t i = 0; i < n; ++i) {
    const float x = static_cast<float>(i) * width / static_cast<float>(n - 1);
    const float ph = x * 0.05f;
    p[i] = {x, 200.0f + 120.0f * std::sin(ph) + 15.0f * std::sin(7.3f * ph)};
  }
  return p;
}

void benchText() {
  auto text = TextSystem::create(std::make_shared<Atlas>());
  if (!text) return;
  TextSystem& ts = **text;
  const std::string dir = assetsDir() + "/fonts/";
  auto face = ts.loadFace(dir + "Inter-Regular.ttf");
  if (!face) {
    std::printf("text benchmarks skipped: %s\n", face.error().message.c_str());
    return;
  }
  // Font creation = FT face + size + hb font + rasterising printable ASCII (94 inked glyphs, hinted). Timed
  // once per fresh text system over sizes 10..21 (UI sizes), median of 7.
  {
    std::vector<double> perGlyph;
    for (int rep = 0; rep < 7; ++rep) {
      auto t2 = TextSystem::create(std::make_shared<Atlas>());
      auto f2 = (*t2)->loadFace(dir + "Inter-Regular.ttf");
      const auto t0 = std::chrono::steady_clock::now();
      for (uint32_t px = 10; px < 22; ++px) pq::bench::doNotOptimize((*t2)->font(*f2, px).ok());
      const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
      perGlyph.push_back(ns / static_cast<double>((*t2)->stats().glyphsRasterised));
      if ((*t2)->stats().atlasFull != 0) std::printf("  (atlas full during font bench!)\n");
    }
    std::sort(perGlyph.begin(), perGlyph.end());
    pq::bench::report("text/font create + ASCII raster, 10..21 px", {perGlyph[3], perGlyph[0]}, "ns/glyph");
  }

  auto body = ts.font(*face, text_size::kBody);
  if (!body) return;
  // 64 distinct 12-character labels, like a device's parameter names.
  std::vector<std::string> labels;
  for (int i = 0; i < 64; ++i) labels.push_back("Param " + std::to_string(1000 + i * 37) + " Hz");
  // Uncached: HarfBuzz shaping + run build (glyphs already in the atlas).
  auto s = pq::bench::measure(
      [&] {
        ts.clearRunCache();
        for (const std::string& l : labels) pq::bench::doNotOptimize(ts.shape(*body, l).advance);
      },
      64);
  pq::bench::report("text/shape uncached, 12-char label", s, "ns/label");
  s = pq::bench::measure([&] { pq::bench::doNotOptimize(ts.shapeGlyphs(*body, labels[3]).size()); }, 1);
  pq::bench::report("text/HarfBuzz shapeGlyphs only, 12-char", s, "ns/label");
  // Cached: lookup + draw-list writes.
  for (const std::string& l : labels) ts.shape(*body, l);
  s = pq::bench::measure([&] {
    for (const std::string& l : labels) pq::bench::doNotOptimize(ts.measure(*body, l));
  }, 64);
  pq::bench::report("text/measure cached, 12-char label", s, "ns/label");
  DrawList dl;
  s = pq::bench::measure(
      [&] {
        dl.reset(1920, 1080);
        for (int rep = 0; rep < 16; ++rep)
          for (size_t i = 0; i < labels.size(); ++i)
            ts.draw(dl, *body, {static_cast<float>(rep * 110), static_cast<float>(i * 16 + 13)}, labels[i], 0xFFE0E0E0u);
        pq::bench::doNotOptimize(dl.indices().size());
      },
      16 * 64);
  pq::bench::report("text/draw cached, 12-char label (1024/frame)", s, "ns/label");
  std::printf("  (%u glyph quads per frame -> %.1f ns/glyph)\n", static_cast<unsigned>(dl.vertices().size() / 4),
              s.medianNs * 16 * 64 / static_cast<double>(dl.vertices().size() / 4));
}

void benchLines() {
  DrawList dl;
  const std::vector<Vec2> pts = sineTrace(2048, 1800.0f);
  std::vector<Vec2> sp;
  splinePath(pts, sp, 1.5f);
  std::printf("  (2048-point trace -> %zu spline points)\n", sp.size());
  auto s = pq::bench::measure([&] { splinePath(pts, sp, 1.5f); pq::bench::doNotOptimize(sp.size()); }, 2048);
  pq::bench::report("lines/splinePath 2048 pts", s, "ns/input pt");
  s = pq::bench::measure(
      [&] {
        dl.reset(1920, 1080);
        smoothPolyline(dl, pts.data(), 2048, 0xFFA0DC78u, 1.4f);
        pq::bench::doNotOptimize(dl.indices().size());
      },
      2048);
  pq::bench::report("lines/smoothPolyline 2048 pts", s, "ns/pt");
  s = pq::bench::measure(
      [&] {
        dl.reset(1920, 1080);
        splinePath(pts, sp, 1.5f);
        smoothPolyline(dl, sp.data(), static_cast<uint32_t>(sp.size()), 0xFFA0DC78u, 1.4f);
        pq::bench::doNotOptimize(dl.indices().size());
      },
      2048);
  pq::bench::report("lines/Catmull-Rom ribbon 2048 pts (spline+ribbon)", s, "ns/input pt");
  s = pq::bench::measure(
      [&] {
        dl.reset(1920, 1080);
        splinePath(pts, sp, 1.5f);
        scopeTrace(dl, sp, 0xFFA0DC78u, 1.4f, 0.35f);
        pq::bench::doNotOptimize(dl.indices().size());
      },
      2048);
  pq::bench::report("lines/scopeTrace 2048 pts (spline + 2 glow + core)", s, "ns/input pt");
  std::vector<Rgba> cols(2048, 0xFFFFC080u);
  s = pq::bench::measure(
      [&] {
        dl.reset(1920, 1080);
        smoothPolylineCols(dl, pts.data(), cols.data(), 2048, 1.4f);
        pq::bench::doNotOptimize(dl.indices().size());
      },
      2048);
  pq::bench::report("lines/smoothPolylineCols 2048 pts", s, "ns/pt");
  // Spectrum: 1024 columns, the full PQ Meter layering (fill + 3 bloom + body + core).
  const std::vector<Vec2> spec = sineTrace(1024, 1800.0f);
  std::vector<float> lvl(1024, 0.6f);
  s = pq::bench::measure(
      [&] {
        dl.reset(1920, 1080);
        spectrumCurve(dl, spec.data(), cols.data(), lvl.data(), 1024, 1000.0f);
        pq::bench::doNotOptimize(dl.indices().size());
      },
      1024);
  pq::bench::report("lines/spectrumCurve 1024 pts (6 strips)", s, "ns/pt");
  std::vector<float> src(1024), dst(1024);
  for (int i = 0; i < 1024; ++i) src[static_cast<size_t>(i)] = static_cast<float>(i / 16);
  s = pq::bench::measure([&] { splineSmooth(src.data(), dst.data(), 1024, 8); pq::bench::doNotOptimize(dst[5]); }, 1024);
  pq::bench::report("lines/splineSmooth 1024 knotStep 8", s, "ns/pt");
}

void benchScenes() {
  auto gpu = GpuContext::create({});
  if (!gpu) {
    std::printf("scene benchmarks skipped: %s\n", gpu.error().message.c_str());
    return;
  }
  auto renderer = Renderer::create(**gpu, OffscreenTarget::kFormat, 1);
  if (!renderer) return;
  auto tex = createSceneTextures(**renderer);
  if (!tex) {
    std::printf("scene textures failed: %s\n", tex.error().message.c_str());
    return;
  }
  auto target = OffscreenTarget::create(**gpu, kDefaultSceneWidth, kDefaultSceneHeight);
  if (!target) return;
  DrawList dl;
  for (const char* scene : {"text", "lines"}) {
    auto s = pq::bench::measure(
        [&] {
          dl.reset(kDefaultSceneWidth, kDefaultSceneHeight);
          buildScene(scene, dl, *tex, 0.0);
          pq::bench::doNotOptimize(dl.indices().size());
        },
        1);
    const std::string name = std::string("scene/") + scene + " build 800x480";
    pq::bench::report(name.c_str(), s, "ns/frame");
    std::printf("  (%s: %u vertices, %u indices, %u commands)\n", scene, static_cast<uint32_t>(dl.vertices().size()),
                static_cast<uint32_t>(dl.indices().size()), static_cast<uint32_t>(dl.commands().size()));
    s = pq::bench::measure([&] { (void)(*target)->draw(**renderer, dl, kSceneClearColor); }, 1, 15, 3, 2e7);
    const std::string fname = std::string("offscreen/") + scene + " 800x480 frame";
    pq::bench::report(fname.c_str(), s, "ns/frame (upload+record+submit+GPU wait)");
  }
}

}  // namespace

int main() {
  benchText();
  benchLines();
  benchScenes();
  return 0;
}
