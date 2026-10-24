// T3.8 benchmarks: the arrangement view over 64 tracks × 200 clips × 50 notes (tests/views/arrangement_fixture.h)
// at 2560×1440 device px (1× and 2× UI scale). CPU only (text through the CPU atlas): full redraw (paint of the whole
// tree into a draw list), a scroll frame and a zoom frame (input → layout → paint of the damage's bounding box), the
// whole song zoomed out, a playhead frame, and the draw list / process memory over 1000 scroll frames.
// Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <memory>
#include <tuple>
#include <vector>

#include <windows.h>
#include <psapi.h>
#include <intrin.h>

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

// Cycles the calling thread has run (blocked or preempted time excluded).
uint64_t threadCycles() {
  ULONG64 c = 0;
  QueryThreadCycleTime(GetCurrentThread(), &c);
  return c;
}

// The TSC rate (the unit of QueryThreadCycleTime), calibrated against steady_clock over 200 ms.
double tscNsPerCycle() {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const uint64_t c0 = __rdtsc();
  while (clock::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const uint64_t c1 = __rdtsc();
  return std::chrono::duration<double, std::nano>(clock::now() - t0).count() / static_cast<double>(c1 - c0);
}

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
  const double nsPerCycle = tscNsPerCycle();
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

    // Zoom frames: one Ctrl+wheel notch out and back in around the middle of the default view.
    const IRect lv = arr->lanesViewport();
    const float cx = static_cast<float>(lv.x) + static_cast<float>(lv.w) * 0.5f;
    int z = 0;
    auto zoomFrame = [&] {
      arr->zoomAt(cx, (z++ % 2) ? 1.25 : 0.8);
      frame();
    };
    std::snprintf(name, sizeof name, "arrangement.zoom frame %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::measure(zoomFrame, 1), "ns/frame");

    // Every frame timed on its own (input → update → partial paint), at 144 Hz of fake time.
    // Per-frame cost as the UI thread's own CPU time (QueryThreadCycleTime / the TSC rate): the machine is shared,
    // and wall time would count the time other processes hold the core.
    auto timed = [&](auto&& input) {
      const uint64_t c0 = threadCycles();
      input();
      frame();
      return static_cast<double>(threadCycles() - c0) * nsPerCycle;
    };
    auto settle = [&] {  // past the zoom settle time, previews complete
      for (int i = 0; i < 400 && (arr->zooming() || tree.needsFrame()); ++i) frame();
    };

    // Zoom sweep as a user does it, from a cold preview cache: 8 Ctrl+wheel notches out (default … 0.17×, up to
    // ~150 bars in view) and 8 back in, one notch per frame, 20 times; the frames that build deferred previews count.
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    settle();
    arr->dropPreviews();
    std::vector<double> sweep;
    uint64_t deferredFrames = 0;
    for (int rep = 0; rep < 20; ++rep)
      for (int j = 0; j < 16; ++j) {
        sweep.push_back(timed([&] { arr->zoomAt(cx, j < 8 ? 0.8 : 1.25); }));
        deferredFrames += st.previewsDeferred > 0 ? 1 : 0;
      }
    std::snprintf(name, sizeof name, "arrangement.zoom sweep frame (1x..0.17x, cold) %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::summarize(sweep), "ns CPU/frame");
    std::printf("  sweep: %zu frames, %llu with previews deferred (dense while zooming, or over the build budget)\n",
                sweep.size(), static_cast<unsigned long long>(deferredFrames));

    // After the gesture: the frames until every preview is drawn (the settle repaint at the far end of the sweep).
    for (int j = 0; j < 8; ++j) arr->zoomAt(cx, 0.8);
    frame();
    std::vector<double> after;
    for (int i = 0; i < 60; ++i) after.push_back(timed([] {}));
    std::snprintf(name, sizeof name, "arrangement.after-zoom frames (0.17x, 60 frames) %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::summarize(after), "ns CPU/frame");

    // Cold start at the default zoom: frames until every visible preview is built.
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    settle();
    arr->dropPreviews();
    std::vector<double> cold;
    for (int i = 0; i < 200 && (cold.empty() || tree.needsFrame()); ++i) cold.push_back(timed([] {}));
    std::snprintf(name, sizeof name, "arrangement.cold-cache frames (default zoom) %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::summarize(cold), "ns CPU/frame");
    std::printf("  cold cache: %zu frames to build every visible preview\n", cold.size());

    // Per zoom level, settled (previews drawn): the full paint and what it emits.
    for (int step = 0; step <= 8; ++step) {
      settle();
      paintFull();
      const auto s2 = pq::bench::measure(paintFull, 1, 5, 1);
      std::printf("  settled zoom %.3fx: %u clips, %u note rects, %u vertices, full paint median %.0f ns\n",
                  std::pow(0.8, step), st.clipsPainted, st.noteRectsEmitted, static_cast<unsigned>(dl.vertices().size()),
                  s2.medianNs);
      arr->zoomAt(cx, 0.8);
    }
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    settle();

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

    // T3.9 drag frames (UI-thread CPU per frame: pointer move → preview damage → partial paint), previews settled:
    // a selected clip's title dragged around (time and across rows), an Alt-drag copy, an edge resize, a range
    // (rubber band) drag over rows; and the release of a move (one edit dispatched, rows rebuilt, full repaint).
    {
      arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
      arr->scrollToY(0);
      settle();
      auto pointer = [&](PointerType t, Vec2 p, Mods m = mod::kNone) {
        PointerEvent e;
        e.type = t;
        e.pos = p;
        e.mods = m;
        e.button = (t == PointerType::Down || t == PointerType::Up) ? PointerButton::Left : PointerButton::None;
        e.clickCount = 1;
        tree.dispatchPointer(e);
      };
      const ArrangementRow row = arr->rows()[2];
      const doc::ClipRef ref{row.id, row.track->clips[1].id};  // one bar at bar 3
      const int32_t rowH = arr->rowLaneRect(2).h;
      struct DragCase {
        const char* what;
        float dx0;     // grab point from the clip's left edge (device px)
        Mods mods;
        bool rows;     // also move across rows
      };
      const IRect box = arr->clipRect(ref);
      const DragCase cases[] = {{"move", static_cast<float>(box.w) * 0.5f, mod::kNone, true},
                                {"copy", static_cast<float>(box.w) * 0.5f, mod::kAlt, true},
                                {"resize", static_cast<float>(box.w) - 2.0f, mod::kNone, false}};
      for (const DragCase& dc : cases) {
        arr->selectClips(std::span<const doc::ClipRef>(&ref, 1), false);
        settle();
        const IRect cb = arr->clipRect(ref);
        const Vec2 grab{static_cast<float>(cb.x) + dc.dx0, static_cast<float>(cb.y) + 7.0f * scale};
        pointer(PointerType::Move, grab, dc.mods);
        pointer(PointerType::Down, grab, dc.mods);
        pointer(PointerType::Move, {grab.x + 12.0f, grab.y}, dc.mods);
        frame();
        std::vector<double> drag;
        int painted = 0;
        for (int i = 0; i < 600; ++i) {
          const float x = grab.x + 12.0f + static_cast<float>((i * 25) % 500);  // > one grid cell per frame
          const float y = grab.y + (dc.rows ? static_cast<float>(((i / 40) % 3) * rowH) : 0.0f);
          drag.push_back(timed([&] { pointer(PointerType::Move, {x, y}, dc.mods); }));
          painted += dl.vertices().empty() ? 0 : 1;
        }
        std::snprintf(name, sizeof name, "arrangement.drag frame (%s) %ux%u@%.0fx", dc.what, w, h, static_cast<double>(scale));
        pq::bench::reportStats(name, pq::bench::summarize(drag), "ns CPU/frame");
        std::printf("  %s: %d of 600 frames painted\n", dc.what, painted);
        const double release = timed([&] { pointer(PointerType::Up, {grab.x + 60.0f, grab.y}, dc.mods); });
        std::printf("  %s release (one edit dispatched, rows rebuilt, full repaint): %.0f ns CPU\n", dc.what, release);
        (void)ctx.undo();
        settle();
      }
      // Range drag from empty space between clips of row 2 over 6 rows.
      arr->clearSelection();
      settle();
      const IRect lane = arr->rowLaneRect(2);
      const Vec2 from{arr->xOfTick(1.5 * static_cast<double>(kTicksPerWhole)), static_cast<float>(lane.bottom() - 4)};
      pointer(PointerType::Move, from);
      pointer(PointerType::Down, from);
      pointer(PointerType::Move, {from.x + 12.0f, from.y});
      frame();
      std::vector<double> band;
      int bandPainted = 0;
      for (int i = 0; i < 600; ++i) {
        const float x = from.x + 12.0f + static_cast<float>((i * 25) % 500);
        const float y = from.y + static_cast<float>(((i / 30) % 6) * rowH);
        band.push_back(timed([&] { pointer(PointerType::Move, {x, y}); }));
        bandPainted += dl.vertices().empty() ? 0 : 1;
      }
      std::printf("  range: %d of 600 frames painted, %zu clips selected at the end\n", bandPainted,
                  arr->selection().clips.size());
      pointer(PointerType::Up, from);
      std::snprintf(name, sizeof name, "arrangement.drag frame (range) %ux%u@%.0fx", w, h, static_cast<double>(scale));
      pq::bench::reportStats(name, pq::bench::summarize(band), "ns CPU/frame");
      arr->clearSelection();
      settle();
    }

    // Memory over scroll frames: after a 1000-frame warm-up (more than one pass over the song, so the bounded preview
    // cache is at its working size), 10,000 frames sampled every 1000 (the criterion asks for 1000 flat).
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    for (int i = 0; i < 1000; ++i) scrollFrame();
    const double mem0 = privateMib();
    uint32_t maxVerts = 0, maxIdx = 0;
    std::printf("  memory over scroll frames (private MiB):");
    for (int i = 1; i <= 10000; ++i) {
      scrollFrame();
      maxVerts = std::max(maxVerts, static_cast<uint32_t>(dl.vertices().size()));
      maxIdx = std::max(maxIdx, static_cast<uint32_t>(dl.indices().size()));
      if (i % 1000 == 0) std::printf(" %d:%.3f", i, privateMib());
    }
    const double mem1 = privateMib();
    std::printf("\n  10000 scroll frames: private bytes %.2f -> %.2f MiB (%+.3f MiB), max %u vertices / %u indices per "
                "frame, %zu previews cached, %llu evicted\n",
                mem0, mem1, mem1 - mem0, maxVerts, maxIdx, arr->previewCacheSize(),
                static_cast<unsigned long long>(st.previewsEvicted));
    tree.setRoot(std::make_unique<Widget>());
  }
  return 0;
}
