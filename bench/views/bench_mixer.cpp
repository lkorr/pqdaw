// T3.10 benchmarks: track headers with their mixer sections over 64 tracks (+ 2 returns, the master) at 2560×1440
// device px, CPU only (text through the CPU atlas): the header column's paint, a volume-drag frame (pointer move →
// one Param dispatch inside a gesture → rows rebound → partial paint of the damage), the meter update (one batched
// MeterSource read for every strip, steady and changing levels) and the mixer view's full paint.
// Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <climits>
#include <cstdio>
#include <memory>

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
  constexpr int kTracks = 64;
  const uint32_t w = 2560, h = 1440;
  auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(1.0f));
  if (!tex) return 1;
  const PaintResources res = paintResources(*tex);
  FakeTransport port;
  ViewContext ctx(port);
  views::test::buildLargeArrangement(ctx.store(), kTracks, 20, 10);
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
    paintRegion(damageBox(tree));
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
  pq::bench::reportStats("mixer.volume drag frame 64 tracks 2560x1440", pq::bench::measure(dragFrame, 1), "ns/frame");
  e.type = PointerType::Up;
  e.pos = p;
  tree.dispatchPointer(e);
  frame();

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
