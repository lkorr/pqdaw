// T3.8 benchmarks: the arrangement view over 64 tracks × 200 clips × 50 notes (tests/views/arrangement_fixture.h)
// at 2560×1440 device px (1× and 2× UI scale). CPU only (text through the CPU atlas): full redraw (paint of the whole
// tree into a draw list), a scroll frame and a zoom frame (input → layout → paint of the damage's bounding box), the
// whole song zoomed out, a playhead frame, and the draw list / process memory over 1000 scroll frames.
// Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <memory>
#include <tuple>

#include <windows.h>
#include <psapi.h>

#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq_bench.h"
#include "../../tests/views/arrangement_fixture.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

double privateMib() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
}

IRect damageBox(UiTree& tree) {
  if (tree.damage().full()) return {0, 0, static_cast<int32_t>(tree.width()), static_cast<int32_t>(tree.height())};
  const auto rects = tree.damage().rects();
  if (rects.empty()) return {};
  int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
  for (const ClipRect& r : rects) {
    x0 = std::min(x0, r.x0);
    y0 = std::min(y0, r.y0);
    x1 = std::max(x1, r.x1);
    y1 = std::max(y1, r.y1);
  }
  return {x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

int main() {
  constexpr int kTracks = 64, kClips = 200, kNotes = 50;
  for (const auto [lw, lh, scale] : {std::tuple{2560u, 1440u, 1.0f}, std::tuple{1280u, 720u, 2.0f}}) {
    auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(scale));
    if (!tex) {
      std::printf("scene text: %s\n", tex.error().message.c_str());
      return 1;
    }
    const PaintResources res = paintResources(*tex);
    const UiScale s(scale);
    const auto w = static_cast<uint32_t>(s.px(static_cast<float>(lw))), h = static_cast<uint32_t>(s.px(static_cast<float>(lh)));
    FakeTransport port;
    ViewContext ctx(port);
    views::test::buildLargeArrangement(ctx.store(), kTracks, kClips, kNotes);
    UiTree tree;
    tree.setViewport(w, h, s);
    tree.setResources(&res);
    auto shell = Shell::create(tree, ctx);
    if (!shell) {
      std::printf("shell: %s\n", shell.error().message.c_str());
      return 1;
    }
    auto* arr = dynamic_cast<ArrangementView*>(tree.find("main/body/top/track-area/arrangement"));
    if (!arr) return 1;
    DrawList dl;
    double now = 1.0;
    auto paintFull = [&] {
      tex->text->beginFrame();
      dl.reset(w, h);
      tree.paint(dl, res, now);
    };
    auto frame = [&] {
      now += 1.0 / 144.0;
      tree.update(now);
      const IRect box = damageBox(tree);
      tex->text->beginFrame();
      dl.reset(w, h);
      tree.paint(dl, res, now, box);
    };
    tree.update(now);
    paintFull();
    const auto& st = arr->stats();
    std::printf("arrangement %ux%u@%.0fx: %zu rows, %u rows / %u clips painted, %u note rects, %u vertices, %u commands, "
                "%llu previews built\n",
                w, h, static_cast<double>(scale), arr->rows().size(), st.rowsPainted, st.clipsPainted, st.noteRectsEmitted,
                static_cast<unsigned>(dl.vertices().size()), static_cast<unsigned>(dl.commands().size()),
                static_cast<unsigned long long>(st.previewBuilds));
    char name[128];
    std::snprintf(name, sizeof name, "arrangement.paint(full) %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::measure(paintFull, 1), "ns/frame");

    // Where the full frame goes: the lanes alone, the header column alone.
    {
      const IRect lanes = arr->lanesViewport();
      const IRect sv = arr->trackScroll().rect();
      const IRect headers{lanes.right(), sv.y, sv.right() - lanes.right(), sv.h};
      for (const auto& [what, region] : {std::pair{"lanes", lanes}, std::pair{"headers", headers}}) {
        std::snprintf(name, sizeof name, "arrangement.paint(%s only) %ux%u@%.0fx", what, w, h, static_cast<double>(scale));
        pq::bench::reportStats(name,
                               pq::bench::measure(
                                   [&] {
                                     tex->text->beginFrame();
                                     dl.reset(w, h);
                                     tree.paint(dl, res, now, region);
                                   },
                                   1),
                               "ns/frame");
      }
    }

    // Scroll frames: 40 device px right per frame, wrapping back over the song.
    const double songPx = static_cast<double>(arr->songEnd()) * arr->devicePxPerTick();
    double scrolled = 0.0;
    auto scrollFrame = [&] {
      if (scrolled > songPx * 0.8) {
        arr->setTimeView(0.0, arr->pxPerTick());
        scrolled = 0.0;
      }
      arr->scrollByPx(40.0);
      scrolled += 40.0;
      frame();
    };
    std::snprintf(name, sizeof name, "arrangement.scroll frame %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::measure(scrollFrame, 1), "ns/frame");
    // Vertical scroll frames (the track scroll view).
    int32_t vy = 0;
    auto vscrollFrame = [&] {
      vy = (vy + 37) % 1200;
      arr->scrollToY(vy);
      frame();
    };
    std::snprintf(name, sizeof name, "arrangement.vscroll frame %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::measure(vscrollFrame, 1), "ns/frame");
    arr->scrollToY(0);
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    frame();

    // Zoom frames: one Ctrl+wheel notch out and back in around the middle of the default view (the interaction the
    // criterion is about), then a sweep over 8 notches out (default … 0.17×, i.e. up to ~150 bars in view).
    const IRect lv = arr->lanesViewport();
    const float cx = static_cast<float>(lv.x) + static_cast<float>(lv.w) * 0.5f;
    int z = 0;
    auto zoomFrame = [&] {
      arr->zoomAt(cx, (z++ % 2) ? 1.25 : 0.8);
      frame();
    };
    std::snprintf(name, sizeof name, "arrangement.zoom frame %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::measure(zoomFrame, 1), "ns/frame");
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    z = 0;
    auto sweepFrame = [&] {
      arr->zoomAt(cx, (z++ / 8) % 2 ? 1.25 : 0.8);
      frame();
    };
    std::snprintf(name, sizeof name, "arrangement.zoom sweep frame (1x..0.17x) %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::measure(sweepFrame, 1), "ns/frame");

    // The whole song in view (every visible track's 200 clips, 10,000 notes per row).
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    arr->zoomToSelection();
    tree.update(now);
    paintFull();
    std::printf("  zoomed out: %u clips painted, %u note rects, %u vertices\n", st.clipsPainted, st.noteRectsEmitted,
                static_cast<unsigned>(dl.vertices().size()));
    std::snprintf(name, sizeof name, "arrangement.paint(full, whole song) %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::measure(paintFull, 1), "ns/frame");
    (void)arr->zoomBack();

    // Playing: the playhead moves a pixel per frame; the frame paints its two columns.
    ctx.togglePlay();
    Ticks tick = 0;
    uint64_t verts = 0, frames = 0;
    auto playFrame = [&] {
      tick += static_cast<Ticks>(1.0 / arr->devicePxPerTick()) + 1;
      if (tick > arr->visibleTicks().end - kTicksPerWhole) tick = arr->visibleTicks().start;
      port.setPlayheadTick(tick);
      now += 0.1;  // past the playhead timer (and the control bar's)
      tree.update(now);
      const IRect box = damageBox(tree);
      tex->text->beginFrame();
      dl.reset(w, h);
      tree.paint(dl, res, now, box);
      verts += dl.vertices().size();
      ++frames;
    };
    std::snprintf(name, sizeof name, "arrangement.playhead frame %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::measure(playFrame, 1), "ns/frame");
    std::printf("  playhead frame: %.0f vertices on average (two columns + the position readout)\n",
                frames ? static_cast<double>(verts) / static_cast<double>(frames) : 0.0);
    ctx.togglePlay();
    tree.update(now += 0.2);

    // Memory over 1000 scroll frames (after a warm-up pass over the whole song, so every preview is cached).
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    for (int i = 0; i < 1000; ++i) scrollFrame();
    const double mem0 = privateMib();
    uint32_t maxVerts = 0, maxIdx = 0;
    for (int i = 0; i < 1000; ++i) {
      scrollFrame();
      maxVerts = std::max(maxVerts, static_cast<uint32_t>(dl.vertices().size()));
      maxIdx = std::max(maxIdx, static_cast<uint32_t>(dl.indices().size()));
    }
    const double mem1 = privateMib();
    std::printf("  1000 scroll frames: private bytes %.2f -> %.2f MiB (%+.3f MiB), max %u vertices / %u indices per "
                "frame, %zu previews cached\n",
                mem0, mem1, mem1 - mem0, maxVerts, maxIdx, arr->previewCacheSize());
    tree.setRoot(std::make_unique<Widget>());
  }
  return 0;
}
