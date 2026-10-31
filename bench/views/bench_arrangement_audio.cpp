// T5.6 benchmarks: the arrangement over 64 audio tracks × 200 clips (tests/views/audio_fixture.h: warp-Off one-bar
// clips, every track's sample a 20 s stereo overview injected, so nothing is decoded) at 2560×1440 device px (1× and
// 2× UI scale), CPU only with a TileAtlas as with a GPU (the waveform tiles; the renderer's per-frame staging copy is
// counted). Full paint (warm tiles), scroll and zoom frames (input → update → partial paint, per-frame UI-thread CPU),
// the playhead frame, the settle after a zoom (tiles rebuilt), memory over 10,000 scroll frames, idle frames.
// Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
#include "pq/ui/tile_atlas.h"
#include "pq/ui/widget.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq_bench.h"
#include "../../tests/views/audio_fixture.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

uint64_t threadCycles() {
  ULONG64 c = 0;
  QueryThreadCycleTime(GetCurrentThread(), &c);
  return c;
}

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
  constexpr int kTracks = 64, kClips = 200;
  const std::filesystem::path wav = std::filesystem::temp_directory_path() / "pq_bench_arrangement_audio" / "tiny.wav";
  views::test::writeTinyWav(wav);
  const media::OverviewPtr ov = views::test::syntheticOverview(20ull * 48000, 2);
  for (const auto [lw, lh, scale] : {std::tuple{2560u, 1440u, 1.0f}, std::tuple{1280u, 720u, 2.0f}}) {
    auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(scale));
    if (!tex) {
      std::printf("scene text: %s\n", tex.error().message.c_str());
      return 1;
    }
    auto tiles = std::make_shared<TileAtlas>();
    PaintResources resTiles = paintResources(*tex);
    resTiles.tiles = tiles.get();
    const PaintResources& res = resTiles;
    std::vector<uint8_t> staging;
    auto stageTiles = [&] {
      const auto b = tiles->uploadBytes();
      if (staging.size() < b.size()) staging.resize(b.size());
      if (!b.empty()) std::memcpy(staging.data(), b.data(), b.size());
      tiles->clearUploads();
      tiles->beginFrame();
    };
    const UiScale s(scale);
    const auto w = static_cast<uint32_t>(s.px(static_cast<float>(lw))), h = static_cast<uint32_t>(s.px(static_cast<float>(lh)));
    FakeTransport port;
    ViewContext ctx(port);
    views::test::buildLargeAudioArrangement(ctx, kTracks, kClips, wav, ov);
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
      stageTiles();
    };
    auto frame = [&] {
      now += 1.0 / 144.0;
      tree.update(now);
      const IRect box = damageBox(tree);
      tex->text->beginFrame();
      dl.reset(w, h);
      tree.paint(dl, res, now, box);
      stageTiles();
    };
    auto timed = [&](auto&& input) {
      const uint64_t c0 = threadCycles();
      input();
      frame();
      return static_cast<double>(threadCycles() - c0) * nsPerCycle;
    };
    auto settle = [&] {
      for (int i = 0; i < 2000 && (arr->zooming() || tree.needsFrame()); ++i) frame();
    };
    tree.update(now);
    paintFull();
    settle();
    paintFull();
    const auto& st = arr->stats();
    std::printf("audio arrangement %ux%u@%.0fx: %zu rows, %u rows / %u clips painted, %u from tiles, %u direct (%u columns), "
                "%u vertices, %u commands, %llu tiles built\n",
                w, h, static_cast<double>(scale), arr->rows().size(), st.rowsPainted, st.clipsPainted, st.tilesDrawn, st.wavesDirect,
                st.waveColumns, static_cast<unsigned>(dl.vertices().size()), static_cast<unsigned>(dl.commands().size()),
                static_cast<unsigned long long>(st.tilesBuilt));
    char name[160];
    std::snprintf(name, sizeof name, "arrangement.audio paint(full) %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::measure(paintFull, 1), "ns/frame");

    // Scroll frames: 40 device px right per frame (tiles built in the frames' onFrame as clips come into view).
    const double songPx = static_cast<double>(arr->songEnd()) * arr->devicePxPerTick();
    double scrolled = 0.0;
    auto scrollInput = [&] {
      if (scrolled > songPx * 0.8) {
        arr->setTimeView(0.0, arr->pxPerTick());
        scrolled = 0.0;
      }
      arr->scrollByPx(40.0);
      scrolled += 40.0;
    };
    std::vector<double> scroll;
    for (int i = 0; i < 600; ++i) scroll.push_back(timed(scrollInput));
    std::snprintf(name, sizeof name, "arrangement.audio scroll frame %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::summarize(scroll), "ns CPU/frame");
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    settle();

    // Zoom frames: one Ctrl+wheel notch out and back in, alternating (stale tiles stretched until the zoom settles).
    const IRect lv = arr->lanesViewport();
    const float cx = static_cast<float>(lv.x) + static_cast<float>(lv.w) * 0.5f;
    std::vector<double> zoom;
    for (int i = 0; i < 400; ++i) zoom.push_back(timed([&] { arr->zoomAt(cx, (i % 2) ? 1.25 : 0.8); }));
    std::snprintf(name, sizeof name, "arrangement.audio zoom frame %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::summarize(zoom), "ns CPU/frame");
    // The settle: frames after a zoom until idle (tile builds, then one repaint).
    for (int j = 0; j < 3; ++j) arr->zoomAt(cx, 0.8);
    frame();
    std::vector<double> after;
    for (int i = 0; i < 2000 && (arr->zooming() || tree.needsFrame()); ++i) after.push_back(timed([] {}));
    std::snprintf(name, sizeof name, "arrangement.audio settle frames after zoom-out %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::summarize(after), "ns CPU/frame");
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    settle();

    // Playhead frames.
    ctx.togglePlay();
    Ticks tick = 0;
    std::vector<double> play;
    for (int i = 0; i < 600; ++i) {
      tick += static_cast<Ticks>(1.0 / arr->devicePxPerTick()) + 1;
      if (tick > arr->visibleTicks().end - kTicksPerWhole) tick = arr->visibleTicks().start;
      port.setPlayheadTick(tick);
      const uint64_t c0 = threadCycles();
      now += 0.1;
      tree.update(now);
      const IRect box = damageBox(tree);
      tex->text->beginFrame();
      dl.reset(w, h);
      tree.paint(dl, res, now, box);
      play.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
    }
    std::snprintf(name, sizeof name, "arrangement.audio playhead frame %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::reportStats(name, pq::bench::summarize(play), "ns CPU/frame");
    ctx.togglePlay();
    tree.update(now += 0.2);
    settle();

    // Idle: no frames requested once settled.
    int idleFrames = 0;
    for (int i = 0; i < 100; ++i) {
      now += 1.0 / 144.0;
      tree.update(now);
      idleFrames += tree.needsFrame() || tree.needsPaint() ? 1 : 0;
    }
    std::printf("  idle: %d of 100 updates wanted a frame\n", idleFrames);

    // Memory over 10,000 scroll frames after a 1000-frame warm-up.
    arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
    scrolled = 0.0;
    for (int i = 0; i < 1000; ++i) {
      scrollInput();
      frame();
    }
    const double mem0 = privateMib();
    uint32_t maxVerts = 0;
    std::printf("  memory over scroll frames (private MiB):");
    for (int i = 1; i <= 10000; ++i) {
      scrollInput();
      frame();
      maxVerts = std::max(maxVerts, static_cast<uint32_t>(dl.vertices().size()));
      if (i % 2000 == 0) std::printf(" %d:%.3f", i, privateMib());
    }
    const double mem1 = privateMib();
    std::printf("\n  10000 scroll frames: private bytes %.2f -> %.2f MiB (%+.3f MiB), max %u vertices per frame, tile pages %u, "
                "%llu evictions\n",
                mem0, mem1, mem1 - mem0, maxVerts, tiles->pageCount(), static_cast<unsigned long long>(tiles->stats().evictions));
    tree.setRoot(std::make_unique<Widget>());
  }
  return 0;
}
