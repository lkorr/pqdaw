// T3.15 benchmarks: automation lanes at 2560×1440 device px, CPU only (text through the CPU atlas). 16 tracks, each
// with a volume lane of N breakpoints over 64 bars (every fourth segment curved), all lanes shown and automation
// mode on (the envelopes also drawn over the clips): the lanes' paint at the default zoom and zoomed out to the whole
// 64 bars (thousands of points per lane in view: the column collapse bounds the ribbon), and a breakpoint drag frame
// (pointer move → preview → damage of the neighbours' columns → partial paint).
// T3.x-fix-paint: paints go through a CPU TileAtlas (envelope tiles, as with a GPU; the renderer's per-frame staging
// copy is done here and counted), frames paint once per damage rect as the window loop does, and a "playing" row
// measures playback: the playhead moving every frame, the control bar's position readout, every header meter changing
// at ~30 Hz, the 16 lanes shown and automation mode on.
// Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/tile_atlas.h"
#include "pq/ui/widget.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq_bench.h"
#include "../../tests/views/automation_fixture.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

int run(int points) {
  constexpr int kTracks = 16;
  const uint32_t w = 2560, h = 1440;
  auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(1.0f));
  if (!tex) return 1;
  auto tiles = std::make_shared<TileAtlas>();
  PaintResources res = paintResources(*tex);
  res.tiles = tiles.get();
  std::vector<uint8_t> staging;
  auto stageTiles = [&] {  // the renderer's per-frame share: copy the new tiles to staging, then the frame boundary
    const auto b = tiles->uploadBytes();
    if (staging.size() < b.size()) staging.resize(b.size());
    if (!b.empty()) std::memcpy(staging.data(), b.data(), b.size());
    tiles->clearUploads();
    tiles->beginFrame();
  };
  FakeTransport port;
  ViewContext ctx(port);
  views::test::buildAutomationBench(ctx.store(), kTracks, points);
  UiTree tree;
  tree.setViewport(w, h, UiScale(1.0f));
  tree.setResources(&res);
  auto shell = Shell::create(tree, ctx);
  if (!shell) return 1;
  auto* arr = dynamic_cast<ArrangementView*>(tree.find("main/body/top/track-area/arrangement"));
  if (!arr) return 1;
  ctx.setDetailVisible(false);
  ArrangementAutomation& au = arr->automation();
  DrawList dl;
  double now = 1.0;
  tree.layoutNow();
  tree.update(now);
  auto paintRegion = [&](const IRect& region) {
    tex->text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, res, now, region);
    stageTiles();
  };
  // A frame as the window loop paints it: once per damage rect.
  auto paintDamage = [&] {
    tex->text->beginFrame();
    dl.reset(w, h);
    if (tree.damage().full()) {
      tree.paint(dl, res, now);
    } else {
      IRect rs[Damage::kMaxRects];
      int64_t area = 0;
      const uint32_t n = tree.damageRects(rs, area);
      tree.paint(dl, res, now, std::span<const IRect>(rs, n));
    }
    stageTiles();
  };
  auto frame = [&] {
    now += 1.0 / 144.0;
    tree.update(now);
    paintDamage();
  };
  // Frames until nothing more is requested (envelope tiles built in onFrame, their chunks repainted from them).
  auto settle = [&] {
    for (int i = 0; i < 600 && (i == 0 || tree.needsFrame() || arr->zooming()); ++i) frame();
  };
  char name[200];
  // 0. Baseline: the same lanes area without automation shown (16 track rows of clips, grid, empty space).
  paintRegion({0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)});
  std::snprintf(name, sizeof name, "automation.baseline lanes-area paint without automation (%d points) 2560x1440", points);
  pq::bench::reportStats(name, pq::bench::measure([&] { paintRegion(arr->lanesViewport()); }, 1), "ns/frame");
  for (const doc::TrackPtr& t : ctx.doc()->tracks) au.showLanes(t->id, true);
  au.setMode(true);
  tree.layoutNow();
  tree.update(now += 0.2);
  const IRect lv = arr->lanesViewport();
  const IRect all{0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)};
  paintRegion(all);
  settle();
  auto report = [&] {
    std::printf("  bands %u, polyline vertices %u, handles %u, envelope tiles %u (missing %u, %zu cached), draw list %u vertices, %u commands\n",
                au.stats().bandsPainted, au.stats().vertices, au.stats().handles, au.stats().envTilesDrawn, au.stats().envTilesMissing,
                au.envelopeTileCount(), static_cast<unsigned>(dl.vertices().size()), static_cast<unsigned>(dl.commands().size()));
  };

  // 1. The lanes (and the envelopes over the rows) at the default zoom (≈ 26 bars in view).
  std::snprintf(name, sizeof name, "automation.lanes paint %d lanes x %d points, default zoom 2560x1440", kTracks, points);
  pq::bench::reportStats(name, pq::bench::measure([&] { paintRegion(lv); }, 1), "ns/frame");
  report();

  // 1b. Playing: the playhead advances every frame (1/144 s at 120 BPM), the control bar's readout ticks and every
  // header meter changes at ~30 Hz.
  {
    FixedMeterSource src;
    MeterLevels lvl;
    MixerView* mixer = findMixer(**shell);
    if (!mixer) return 1;
    mixer->setMeterSource(&src);
    src.setLive(true);
    auto setLevels = [&](int i) {
      lvl.rmsDb[0] = lvl.rmsDb[1] = -12.0f - static_cast<float>(i % 17);
      lvl.peakDb[0] = lvl.peakDb[1] = -6.0f - static_cast<float>(i % 11);
      doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) { src.set(t.id, lvl); });
    };
    setLevels(0);
    port.setPlayheadTick(0);
    ctx.togglePlay();
    settle();
    int i = 0;
    double tick = 0.0;
    auto playFrame = [&] {
      ++i;
      tick += static_cast<double>(kTicksPerWhole) / 2.0 / 144.0;  // 120 BPM in 4/4: half a bar per second
      port.setPlayheadTick(static_cast<Ticks>(tick));
      if (i % 5 == 0) setLevels(i / 5);  // ~30 Hz at 144 Hz frames
      frame();
    };
    for (int k = 0; k < 200; ++k) playFrame();
    std::snprintf(name, sizeof name, "automation.playing frame (%d lanes x %d points, automation mode, meters live; per-rect paint, tiles) 2560x1440",
                  kTracks, points);
    pq::bench::reportStats(name, pq::bench::measure(playFrame, 1), "ns/frame");
    playFrame();
    report();
    ctx.togglePlay();
    mixer->setMeterSource(nullptr);
    settle();
  }

  // 2. Zoomed out to the whole 64 bars: every point in view.
  arr->setTimeView(0.0, static_cast<double>(lv.w) / (65.0 * static_cast<double>(kTicksPerWhole)));
  tree.update(now += 0.2);
  paintRegion(all);
  settle();
  std::snprintf(name, sizeof name, "automation.lanes paint %d lanes x %d points, all in view 2560x1440", kTracks, points);
  pq::bench::reportStats(name, pq::bench::measure([&] { paintRegion(lv); }, 1), "ns/frame");
  report();

  // 3. A breakpoint drag in the first lane at the default zoom: one pointer move per frame.
  arr->setTimeView(0.0, 96.0 / static_cast<double>(kTicksPerWhole));
  tree.update(now += 0.2);
  paintRegion(all);
  settle();
  const doc::Track& first = *ctx.doc()->tracks.front();
  const auto band = au.band(first.id, doc::ParamRef::track(doc::ParamKind::TrackVolume, first.id), false);
  if (!band || !band->lane) return 1;
  const doc::Breakpoint* target = nullptr;
  for (const doc::Breakpoint& b : band->lane->points)
    if (arr->xOfTick(static_cast<double>(b.tick)) > static_cast<float>(lv.x + lv.w / 3)) {
      target = &b;
      break;
    }
  if (!target) return 1;
  Vec2 p = *au.pointPos(*band, target->id);
  PointerEvent e;
  e.type = PointerType::Move;
  e.pos = p;
  tree.dispatchPointer(e);
  e.type = PointerType::Down;
  e.button = PointerButton::Left;
  e.clickCount = 1;
  tree.dispatchPointer(e);
  frame();
  float dir = 1.0f;
  int moves = 0;
  auto dragFrame = [&] {
    if (++moves % 8 == 0) dir = -dir;
    p.y += dir;
    PointerEvent m;
    m.type = PointerType::Move;
    m.pos = p;
    tree.dispatchPointer(m);
    frame();
  };
  std::snprintf(name, sizeof name, "automation.drag frame (one breakpoint, %d-point lane) 2560x1440", points);
  pq::bench::reportStats(name, pq::bench::measure(dragFrame, 1), "ns/frame");
  if (!au.dragging()) return 1;
  e.type = PointerType::Up;
  e.pos = p;
  tree.dispatchPointer(e);
  // The release: one moveBreakpoints dispatch, then the repaint of the bands showing the lane.
  return 0;
}

}  // namespace

int main() {
  for (int points : {1000, 20000})
    if (const int r = run(points); r != 0) return r;
  return 0;
}
