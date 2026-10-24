// T3.15 benchmarks: automation lanes at 2560×1440 device px, CPU only (text through the CPU atlas). 16 tracks, each
// with a volume lane of N breakpoints over 64 bars (every fourth segment curved), all lanes shown and automation
// mode on (the envelopes also drawn over the clips): the lanes' paint at the default zoom and zoomed out to the whole
// 64 bars (thousands of points per lane in view: the column collapse bounds the ribbon), and a breakpoint drag frame
// (pointer move → preview → damage of the neighbours' columns → partial paint).
// Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <climits>
#include <cstdio>
#include <memory>

#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq_bench.h"
#include "../../tests/views/automation_fixture.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

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

int run(int points) {
  constexpr int kTracks = 16;
  const uint32_t w = 2560, h = 1440;
  auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(1.0f));
  if (!tex) return 1;
  const PaintResources res = paintResources(*tex);
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
  };
  char name[160];
  // 0. Baseline: the same lanes area without automation shown (16 track rows of clips, grid, empty space).
  paintRegion({0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)});
  std::snprintf(name, sizeof name, "automation.baseline lanes-area paint without automation (%d points) 2560x1440", points);
  pq::bench::reportStats(name, pq::bench::measure([&] { paintRegion(arr->lanesViewport()); }, 1), "ns/frame");
  for (const doc::TrackPtr& t : ctx.doc()->tracks) au.showLanes(t->id, true);
  au.setMode(true);
  tree.layoutNow();
  tree.update(now += 0.2);
  auto frame = [&] {
    now += 1.0 / 144.0;
    tree.update(now);
    paintRegion(damageBox(tree));
  };
  const IRect lv = arr->lanesViewport();
  const IRect all{0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)};
  paintRegion(all);

  // 1. The lanes (and the envelopes over the rows) at the default zoom (≈ 26 bars in view).
  std::snprintf(name, sizeof name, "automation.lanes paint %d lanes x %d points, default zoom 2560x1440", kTracks, points);
  pq::bench::reportStats(name, pq::bench::measure([&] { paintRegion(lv); }, 1), "ns/frame");
  std::printf("  bands %u, polyline vertices %u, handles %u, draw list %u vertices\n", au.stats().bandsPainted, au.stats().vertices,
              au.stats().handles, static_cast<unsigned>(dl.vertices().size()));

  // 2. Zoomed out to the whole 64 bars: every point in view.
  arr->setTimeView(0.0, static_cast<double>(lv.w) / (65.0 * static_cast<double>(kTicksPerWhole)));
  tree.update(now += 0.2);
  paintRegion(all);
  std::snprintf(name, sizeof name, "automation.lanes paint %d lanes x %d points, all in view 2560x1440", kTracks, points);
  pq::bench::reportStats(name, pq::bench::measure([&] { paintRegion(lv); }, 1), "ns/frame");
  std::printf("  bands %u, polyline vertices %u, handles %u, draw list %u vertices\n", au.stats().bandsPainted, au.stats().vertices,
              au.stats().handles, static_cast<unsigned>(dl.vertices().size()));

  // 3. A breakpoint drag in the first lane at the default zoom: one pointer move per frame.
  arr->setTimeView(0.0, 96.0 / static_cast<double>(kTicksPerWhole));
  tree.update(now += 0.2);
  paintRegion(all);
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
