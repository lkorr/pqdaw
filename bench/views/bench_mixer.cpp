// T3.10 benchmarks: track headers with their mixer sections over 64 tracks (+ 2 returns, the master) at 2560×1440
// device px, CPU only (text through the CPU atlas): the header column's paint, a volume-drag frame (pointer move →
// one Param dispatch inside a gesture → rows rebound → partial paint of the damage), the meter update (one batched
// MeterSource read for every strip, steady and changing levels) and the mixer view's full paint.
// T3.x-fix-paint: frames paint once per damage rect (the window loop), and the volume-drag frame is also measured at
// 8 and 256 tracks: it must not grow with the track count (M3 review M12a).
// Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <climits>
#include <cstdio>
#include <memory>
#include <span>

#include "pq/document/commands.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq_bench.h"
#include "../../tests/views/arrangement_fixture.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

int run(int trackCount, bool all) {
  const uint32_t w = 2560, h = 1440;
  auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(1.0f));
  if (!tex) return 1;
  const PaintResources res = paintResources(*tex);
  FakeTransport port;
  ViewContext ctx(port);
  views::test::buildLargeArrangement(ctx.store(), trackCount, 20, 10);
  for (int i = 0; i < 2; ++i) {
    auto e = doc::ops::createTrack(*ctx.doc(), ctx.store().ids(), doc::TrackKind::Return);
    if (!e || !ctx.store().dispatch(e->command)) return 1;
  }
  for (const doc::TrackPtr& t : ctx.doc()->tracks) ctx.store().view().tracks[t->id].height = 64;  // sends line shown
  UiTree tree;
  tree.setViewport(w, h, UiScale(1.0f));
  tree.setResources(&res);
  auto shell = Shell::create(tree, ctx);
  if (!shell) return 1;
  auto* arr = dynamic_cast<ArrangementView*>(tree.find("main/body/top/track-area/arrangement"));
  MixerView* mixer = findMixer(**shell);
  if (!arr || !mixer) return 1;
  DrawList dl;
  double now = 1.0;
  tree.update(now);
  auto paintRegion = [&](const IRect& region) {
    tex->text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, res, now, region);
  };
  auto frame = [&] {
    now += 1.0 / 144.0;
    tree.update(now);
    tex->text->beginFrame();
    dl.reset(w, h);
    if (tree.damage().full()) {
      tree.paint(dl, res, now);
      return;
    }
    IRect rs[Damage::kMaxRects];
    int64_t area = 0;
    const uint32_t n = tree.damageRects(rs, area);
    tree.paint(dl, res, now, std::span<const IRect>(rs, n));
  };
  paintRegion({0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)});

  // 1. The header column (titles, chips, mixer sections, pinned returns / master), painted alone.
  TrackHeader* first = arr->header(ctx.doc()->tracks.front()->id);
  const IRect col{first->rect().x, 0, first->rect().w, static_cast<int32_t>(h)};
  pq::bench::reportStats("mixer.headers paint 64 tracks 2560x1440", pq::bench::measure([&] { paintRegion(col); }, 1), "ns/frame");
  std::printf("headers: %u vertices, %u commands\n", static_cast<unsigned>(dl.vertices().size()), static_cast<unsigned>(dl.commands().size()));
  pq::bench::reportStats("mixer.full paint (arrangement + headers) 2560x1440",
                         pq::bench::measure([&] { paintRegion({0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)}); }, 1), "ns/frame");

  // 2. A volume drag on the first track's header box: per frame one pointer move → SetTrackVolume (gesture) → rows
  // rebound, headers / strips updated → partial paint.
  auto& strip = static_cast<MixerStrip&>(first->mixerSection().child(0));
  const IRect vb = strip.volume()->rect();
  Vec2 p{static_cast<float>(vb.x + vb.w / 2), static_cast<float>(vb.y + vb.h / 2)};
  PointerEvent e;
  e.type = PointerType::Move;
  e.pos = p;
  tree.dispatchPointer(e);
  e.type = PointerType::Down;
  e.button = PointerButton::Left;
  e.clickCount = 1;
  tree.dispatchPointer(e);
  frame();
  float dir = -1.0f;
  int moves = 0;
  auto dragFrame = [&] {
    if (++moves % 60 == 0) dir = -dir;
    p.y += dir;
    PointerEvent m;
    m.type = PointerType::Move;
    m.pos = p;
    tree.dispatchPointer(m);
    frame();
  };
  char name[120];
  std::snprintf(name, sizeof name, "mixer.volume drag frame %d tracks 2560x1440 (per-rect paint)", trackCount);
  pq::bench::reportStats(name, pq::bench::measure(dragFrame, 1), "ns/frame");
  std::printf("  drag frame: %u vertices, %llu widgets painted\n", static_cast<unsigned>(dl.vertices().size()),
              static_cast<unsigned long long>(tree.stats().widgetsPainted));
  e.type = PointerType::Up;
  e.pos = p;
  tree.dispatchPointer(e);
  frame();
  if (!all) return 0;

  // 3. Meters: one batched read for every strip and the meter updates (steady levels: no repaint; changing: the
  // meters repaint their rects).
  FixedMeterSource src;
  MeterLevels lv;
  lv.rmsDb[0] = lv.rmsDb[1] = -12.0f;
  lv.peakDb[0] = lv.peakDb[1] = -6.0f;
  doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) { src.set(t.id, lv); });
  mixer->setMeterSource(&src);
  pq::bench::reportStats("mixer.meter poll 67 strips (steady levels)", pq::bench::measure([&] { (void)mixer->pollMeters(); }, 1), "ns/poll");
  int k = 0;
  auto changing = [&] {
    lv.rmsDb[0] = -12.0f - static_cast<float>(k++ % 20);
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) { src.set(t.id, lv); });
    (void)mixer->pollMeters();
  };
  pq::bench::reportStats("mixer.meter poll 67 strips (changing levels, incl. source fill)", pq::bench::measure(changing, 1), "ns/poll");
  auto meterFrame = [&] {
    changing();
    frame();
  };
  pq::bench::reportStats("mixer.meter frame (poll + partial paint) 2560x1440", pq::bench::measure(meterFrame, 1), "ns/frame");
  mixer->setMeterSource(nullptr);

  // 4. The mixer view, full paint.
  mixer->setShown(true);
  tree.update(now += 0.01);
  pq::bench::reportStats("mixer.view paint(full) 67 strips 2560x1440",
                         pq::bench::measure([&] { paintRegion({0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)}); }, 1), "ns/frame");
  return 0;
}

}  // namespace

int main() {
  for (const int n : {8, 256})
    if (const int r = run(n, false); r != 0) return r;
  return run(64, true);
}
