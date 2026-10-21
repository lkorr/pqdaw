// T0.7-fix1 partial redraw: frame cost of a 2 px playhead animating over the `lines` scene, full redraw (rebuild
// the whole scene, upload it, clear and draw everything; the pre-T0.7-fix1 path) vs partial (retained static layer,
// rebuild + upload only the playhead, redraw its old and new columns), offscreen with 2 frames in flight like
// the window. CPU = UI-thread time per frame (build + upload + record + submit; the GPU keeps up, so the slot
// waits are short); GPU = timestamps around the drawing and fragment-shader invocations (GpuProfiler). Also the
// widget tree's paint: 10,000 cells, full vs one damaged cell. Run from the release build; results →
// bench/RESULTS.md.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "pq/ui/damage.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"
#include "pq/ui/widget.h"
#include "pq_bench.h"

using namespace pq::ui;

namespace {

class Cell : public Widget {
public:
  explicit Cell(std::string name) : Widget(std::move(name)) {}
  void paint(PaintContext& ctx) override { ctx.dl.addRectFilled(rect().toRect(), 0xFF303040u); }
};

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0.0 : v[v.size() / 2];
}

int playhead(GpuContext& gpu, uint32_t W, uint32_t H) {
  auto rr = Renderer::create(gpu, OffscreenTarget::kFormat, 2);
  if (!rr) return 1;
  Renderer& renderer = **rr;
  auto tex = createSceneTextures(renderer);
  if (!tex) return 1;
  auto tt = OffscreenTarget::create(gpu, W, H, 2);
  if (!tt) return 1;
  OffscreenTarget& target = **tt;

  DrawList staticLayer, overlay, full;
  staticLayer.reset(W, H);
  buildSceneStatic(kPlayheadScene, staticLayer, *tex);
  std::printf("%ux%u: static layer %zu vertices, %zu indices, %zu commands\n", W, H, staticLayer.vertices().size(),
              staticLayer.indices().size(), staticLayer.commands().size());
  const DrawLayer layers[2] = {{&staticLayer, 0, 1}, {&overlay, kTransientLayer, 0}};
  double t = 0.0;
  overlay.reset(W, H);
  buildSceneOverlay(kPlayheadScene, overlay, *tex, t);
  FrameDesc f;
  f.layers = layers;
  f.clear = kSceneClearColor;
  if (!target.submit(renderer, f).ok()) return 1;
  ClipRect last = playheadRect(W, H, t);

  auto partialFrame = [&] {
    t += 1.0 / 144.0;
    const ClipRect now = playheadRect(W, H, t);
    Damage d;
    d.addPixels(last);
    d.addPixels(now);
    last = now;
    overlay.reset(W, H);
    buildSceneOverlay(kPlayheadScene, overlay, *tex, t);
    FrameDesc pf;
    pf.layers = layers;
    pf.full = false;
    pf.damage = d.rects();
    pf.clear = kSceneClearColor;
    (void)target.submit(renderer, pf);
  };
  auto fullFrame = [&] {
    t += 1.0 / 144.0;
    full.reset(W, H);
    buildScene(kPlayheadScene, full, *tex, t);
    (void)target.submit(renderer, full, kSceneClearColor);
  };
  char name[96];
  const auto sp = pq::bench::measure(partialFrame, 1, 15, 3, 2e7);
  std::snprintf(name, sizeof name, "partial/playhead frame CPU %ux%u", W, H);
  pq::bench::report(name, sp, "ns/frame");
  const auto sf = pq::bench::measure(fullFrame, 1, 15, 3, 2e7);
  std::snprintf(name, sizeof name, "full/playhead frame CPU %ux%u", W, H);
  pq::bench::report(name, sf, "ns/frame");
  std::printf("  partial / full CPU: %.1f %% (median)\n", 100.0 * sp.medianNs / sf.medianNs);
  {
    // CPU of the full path without the scene synthesis: the same full frame from an already built list.
    full.reset(W, H);
    buildScene(kPlayheadScene, full, *tex, t);
    const auto s = pq::bench::measure([&] { (void)target.submit(renderer, full, kSceneClearColor); }, 1, 15, 3, 2e7);
    std::snprintf(name, sizeof name, "full/playhead frame CPU, list prebuilt %ux%u", W, H);
    pq::bench::report(name, s, "ns/frame");
  }

  // GPU: one frame at a time (the profiler reads after each), back to back.
  if (target.enableProfiling().ok()) {
    std::vector<double> pMs, fMs, pFrag, fFrag;
    for (int i = 0; i < 200; ++i) {
      partialFrame();
      auto st = target.lastFrameStats();
      if (st && st->hasTimes) pMs.push_back(st->drawMs);
      if (st && st->hasFragments) pFrag.push_back(static_cast<double>(st->fragments));
      fullFrame();
      st = target.lastFrameStats();
      if (st && st->hasTimes) fMs.push_back(st->drawMs);
      if (st && st->hasFragments) fFrag.push_back(static_cast<double>(st->fragments));
    }
    const double px = static_cast<double>(W) * H;
    std::printf("BENCH gpu/playhead draw %ux%u: partial median %.4f ms, full median %.4f ms\n", W, H, median(pMs), median(fMs));
    std::printf("BENCH gpu/playhead fragments %ux%u: partial median %.0f (%.3f %% of the pixels), full median %.0f (%.1f %%)\n", W, H,
                median(pFrag), 100.0 * median(pFrag) / px, median(fFrag), 100.0 * median(fFrag) / px);
  }
  (void)target.finish();
  return 0;
}

void widgets() {
  UiTree tree;
  tree.setViewport(1920, 1080);
  auto root = std::make_unique<Widget>();
  root->setLayout(LayoutKind::Column);
  std::vector<Cell*> cells;
  for (int r = 0; r < 100; ++r) {
    Widget& row = root->add<Widget>("row[" + std::to_string(r) + "]");
    row.setLayout(LayoutKind::Row);
    for (int c = 0; c < 100; ++c) cells.push_back(&row.add<Cell>("c" + std::to_string(c)));
  }
  tree.setRoot(std::move(root));
  tree.layoutNow();
  DrawList dl;
  PaintResources res;
  auto sf = pq::bench::measure(
      [&] {
        dl.reset(1920, 1080);
        tree.paint(dl, res, 0.0);
        pq::bench::doNotOptimize(dl.indices().size());
      },
      1);
  pq::bench::report("partial/widgets 10k paint, full", sf, "ns/frame");
  size_t i = 0;
  auto sp = pq::bench::measure(
      [&] {
        Cell& c = *cells[(i += 7919) % cells.size()];
        c.markDirty();
        const Rect b = tree.damage().bounds();
        dl.reset(1920, 1080);
        tree.paint(dl, res, 0.0,
                   {static_cast<int32_t>(b.x0), static_cast<int32_t>(b.y0), static_cast<int32_t>(b.x1 - b.x0), static_cast<int32_t>(b.y1 - b.y0)});
        pq::bench::doNotOptimize(dl.indices().size());
      },
      1);
  pq::bench::report("partial/widgets 10k paint, one cell damaged", sp, "ns/frame");
  std::printf("  widgets painted: %llu (one-cell frame) of 10,101\n", static_cast<unsigned long long>(tree.stats().widgetsPainted));
}

}  // namespace

int main() {
  auto gpu = GpuContext::create({});
  if (!gpu) {
    std::printf("no GPU: %s\n", gpu.error().message.c_str());
    return 1;
  }
  std::printf("GPU: %s (pipeline statistics %s)\n", (*gpu)->deviceName().c_str(), (*gpu)->pipelineStatistics() ? "yes" : "no");
  if (playhead(**gpu, 800, 480) != 0) return 1;
  if (playhead(**gpu, 1600, 960) != 0) return 1;
  widgets();
  return 0;
}
