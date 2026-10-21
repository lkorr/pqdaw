// T3.13 MOS wheel region chart: a click inside a band selects that band's MOS (100 random points, checked against
// tuning::buildMosRegions with an independent cents mapping, at 1× and 1.5×); generator drag snaps exactly to an EDO
// marker (7\12 → 700 ¢), Alt bypasses; dragging past a band's end crosses into tuning::findAdjacentRegion's region
// and carries the mode (transferModeRotation); EDO marker click; Esc cancels; wheel zoom keeps the cents under the
// pointer; size scrolling (scrollbar, Shift+wheel, background drag); config changes keep the selection; damage is
// confined to what changed, a generator drag does not relayout, and partial redraws over a scripted session equal a
// full redraw on the GPU; hover info; the `mos-chart-default` / `mos-chart-zoomed` goldens.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "pq/tuning/mos_regions.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/test_driver.h"
#include "pq/views/mos_wheel/mos_chart.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
namespace tu = pq::tuning;
namespace mcm = pq::views::mos_chart_metrics;

namespace {

struct Event {
  std::optional<MosSelection> sel;
  MosGesture gesture;
};

struct Chart {
  TestDriver d;
  MosChart* c = nullptr;
  std::vector<Event> events;
  explicit Chart(float scale = 1.0f, MosChartConfig cfg = defaultConfig(), uint32_t w = 800, uint32_t h = 600)
      : d(static_cast<uint32_t>(std::lround(static_cast<float>(w) * scale)), static_cast<uint32_t>(std::lround(static_cast<float>(h) * scale)),
          scale) {
    c = &d.setRoot<MosChart>("mos-chart", cfg);
    c->setOnChange([this](const std::optional<MosSelection>& s, MosGesture g) { events.push_back({s, g}); });
    d.tree().layoutNow();
  }
  static MosChartConfig defaultConfig() {
    MosChartConfig cfg;
    cfg.maxEdo = 31;
    return cfg;
  }
  Vec2 axisPoint(double cents) const {
    const IRect a = c->axisRect();
    return {c->xOfCents(cents), static_cast<float>(a.y + a.h / 2)};
  }
  const MosChartEdoMarker* marker(int32_t step, int32_t edo) const {
    for (const MosChartEdoMarker& m : c->edoMarkers())
      if (m.step == step && m.edo == edo) return &m;
    return nullptr;
  }
};

// Union area of the current damage rects (they are disjoint).
int64_t damageArea(UiTree& t) {
  int64_t a = 0;
  for (const ClipRect& r : t.damage().rects()) a += static_cast<int64_t>(r.x1 - r.x0) * (r.y1 - r.y0);
  return a;
}

// The region of `size` containing `cents` at least `slack` inside its ends, with no other region of that size within
// `slack` (points closer than that to a boundary are ambiguous at pixel resolution and are skipped; so are bands
// narrower than 2 × slack, which the chart may not draw at all).
const tu::MosRegion* expectedRegion(const tu::MosRegionSet& set, int32_t size, double cents, double slack) {
  const tu::MosRegion* found = nullptr;
  for (const tu::MosRegion& r : set.regions) {
    if (r.perPeriodSize != size) continue;
    if (cents >= r.lowCents - slack && cents <= r.highCents + slack) {
      if (found) return nullptr;
      found = &r;
    }
  }
  if (found && !(cents >= found->lowCents + slack && cents <= found->highCents - slack)) return nullptr;
  return found;
}

}  // namespace

TEST_CASE("mos chart: a click inside a band selects its MOS (100 random points vs buildMosRegions, 1x and 1.5x)") {
  for (const float scale : {1.0f, 1.5f}) {
    for (const auto& [vmin, vmax] : {std::pair{0.0, 1200.0}, std::pair{430.0, 770.0}}) {
      CAPTURE(scale);
      CAPTURE(vmin);
      Chart f(scale);
      f.c->setView(vmin, vmax);
      const tu::MosRegionSet set = tu::buildMosRegions(18, 18, 1200.0, 1);
      std::mt19937 rng(20261021u + static_cast<uint32_t>(scale * 10) + static_cast<uint32_t>(vmin));
      int checked = 0;
      for (int attempt = 0; attempt < 20000 && checked < 100; ++attempt) {
        f.c->clearSelection();
        f.d.tree().layoutNow();
        const IRect plot = f.c->plotRect();
        // The documented mapping, computed here independently of the widget: margins of 24 logical px.
        const double x0 = plot.x + mcm::kMargin * scale, w = plot.w - 2.0 * mcm::kMargin * scale;
        std::uniform_real_distribution<float> ux(static_cast<float>(plot.x) + 1.0f, static_cast<float>(plot.right()) - 1.0f);
        std::uniform_int_distribution<int32_t> us(2, 18);
        const int32_t size = us(rng);
        const IRect row = f.c->rowRect(size);
        if (row.empty()) continue;
        std::uniform_real_distribution<float> uy(static_cast<float>(row.y) + 1.0f, static_cast<float>(row.bottom()) - 1.0f);
        const Vec2 p{std::floor(ux(rng)) + 0.5f, std::floor(uy(rng)) + 0.5f};
        const double cents = vmin + (p.x - x0) / w * (vmax - vmin);
        if (cents < vmin || cents > vmax) continue;  // the margins: bands starting outside the view are not drawn (as in the explorer)
        const double pxCents = 1.5 * (vmax - vmin) / w;
        const tu::MosRegion* want = expectedRegion(set, size, cents, pxCents);
        if (!want) continue;
        CHECK(f.c->sizeAtY(p.y) == size);
        f.events.clear();
        CAPTURE(p.x);
        CAPTURE(p.y);
        CAPTURE(cents);
        CAPTURE(f.c->regionAt(p));
        f.d.click(p);
        REQUIRE(f.c->selection().has_value());
        const MosSelection& s = *f.c->selection();
        CHECK(s.perPeriodKey == want->perPeriodKey);
        CHECK(s.key == want->key);
        CHECK(s.regionLow == want->lowCents);
        CHECK(s.regionHigh == want->highCents);
        CHECK(s.mirror == want->isMirror);
        CHECK(s.generatorCents == doctest::Approx(std::clamp(cents, want->lowCents, want->highCents)).epsilon(1e-9));
        CHECK(s.modeRotation == 0);
        REQUIRE(f.events.size() == 1);
        CHECK(f.events[0].gesture == MosGesture::Commit);
        CHECK(f.events[0].sel == f.c->selection());
        ++checked;
      }
      CHECK(checked == 100);
    }
  }
}

TEST_CASE("mos chart: dragging the marker near 7\\12 snaps exactly to 700 cents; Alt bypasses the snap") {
  Chart f;
  f.c->setView(680.0, 720.0);
  REQUIRE(f.c->select({5, 2}, 690.0));
  f.d.tree().layoutNow();
  const MosChartEdoMarker* m = f.marker(7, 12);
  REQUIRE(m != nullptr);
  CHECK(m->cents == 700.0);
  // Press on the axis at 690, drag to 4 device px right of the 7\12 marker (≈0.2 cents off).
  const Vec2 near{m->x + 4.0f, f.axisPoint(700).y};
  f.d.drag(f.axisPoint(690.0), near, 6);
  REQUIRE(f.c->selection().has_value());
  CHECK(f.c->selection()->generatorCents == 700.0);  // exactly, not approximately
  REQUIRE(f.c->selection()->edo.has_value());
  CHECK(f.c->selection()->edo->step == 7);
  CHECK(f.c->selection()->edo->edo == 12);
  REQUIRE(f.events.size() >= 2);
  CHECK(f.events.back().gesture == MosGesture::Commit);
  CHECK(f.events.back().sel->generatorCents == 700.0);
  for (size_t i = 0; i + 1 < f.events.size(); ++i) CHECK(f.events[i].gesture == MosGesture::Live);

  // Grabbing the marker line itself (below the axis) and dragging with Alt: no snap.
  f.events.clear();
  const IRect row = f.c->rowRect(7);
  const Vec2 onLine{f.c->xOfCents(700.0), static_cast<float>(row.y + row.h / 2)};
  f.d.drag(onLine, {near.x, onLine.y}, 4, mod::kAlt);
  CHECK(f.c->selection()->generatorCents != 700.0);
  CHECK(f.c->selection()->generatorCents == doctest::Approx(f.c->centsAtX(near.x)).epsilon(1e-12));
  CHECK_FALSE(f.c->selection()->edo.has_value());
  REQUIRE_FALSE(f.events.empty());
  CHECK(f.events.back().gesture == MosGesture::Commit);

  // Far from every marker: the generator follows the pointer exactly.
  f.d.drag(f.axisPoint(700.0), f.axisPoint(703.0), 3);
  CHECK(f.c->selection()->generatorCents == doctest::Approx(f.c->centsAtX(f.axisPoint(703.0).x)).epsilon(1e-12));
}

TEST_CASE("mos chart: dragging past the band crosses into findAdjacentRegion's region and carries the mode") {
  Chart f;
  f.c->setView(600.0, 800.0);
  REQUIRE(f.c->select({5, 2}, 710.0, 2));
  f.d.tree().layoutNow();
  const tu::MosRegion sel = f.c->regionSet().regions[static_cast<size_t>(f.c->selectedRegion())];
  const double target = 731.0;
  const Vec2 p = f.axisPoint(target);
  const double cc = f.c->centsAtX(p.x);
  const int32_t adj = tu::findAdjacentRegion(f.c->regionSet().regions, sel, cc, f.c->firstSize(), 18);
  REQUIRE(adj >= 0);
  const tu::MosRegion& a = f.c->regionSet().regions[static_cast<size_t>(adj)];
  REQUIRE_FALSE(a.key == sel.key);
  f.d.down(f.axisPoint(710.0), PointerButton::Left, mod::kAlt);  // Alt: no snapping, so the values are exact
  const double genBefore = f.c->selection()->generatorCents;
  f.d.move(p, mod::kAlt);
  REQUIRE(f.c->selection().has_value());
  CHECK(f.c->selection()->key == a.key);
  CHECK(f.c->selectedRegion() == adj);
  const double newGen = std::clamp(cc, a.lowCents, a.highCents);
  CHECK(f.c->selection()->generatorCents == newGen);
  const tu::RegionHint hint = a.hint();
  CHECK(f.c->selection()->modeRotation == tu::transferModeRotation(genBefore, sel.perPeriodSize, 2, newGen, a.perPeriodSize, 1200.0, &hint));
  f.d.up(p, PointerButton::Left, mod::kAlt);
  CHECK(f.events.back().gesture == MosGesture::Commit);
}

TEST_CASE("mos chart: clicking an EDO marker sets its generator exactly; clicking outside clears") {
  Chart f;
  f.c->setView(660.0, 740.0);
  REQUIRE(f.c->select({5, 2}, 690.0));
  f.d.tree().layoutNow();
  const MosChartEdoMarker* m = f.marker(11, 19);  // (18 is hidden here: too close to 119, lower n wins)
  REQUIRE(m != nullptr);
  CHECK(f.marker(18, 31) == nullptr);
  const IRect a = f.c->axisRect();
  f.d.click({m->x + 3.0f, static_cast<float>(a.bottom()) + 40.0f});  // the marker's label area
  CHECK(f.c->selection()->generatorCents == 11.0 * 1200.0 / 19.0);
  CHECK(f.c->selection()->edo->edo == 19);
  REQUIRE(f.events.size() == 1);
  CHECK(f.events[0].gesture == MosGesture::Commit);
  // Every marker shown is an EDO of at most maxEdo inside the selected band.
  for (const MosChartEdoMarker& mk : f.c->edoMarkers()) {
    CHECK(mk.edo <= 31);
    CHECK(mk.cents >= f.c->selection()->regionLow);
    CHECK(mk.cents <= f.c->selection()->regionHigh);
  }
  // A click in the plot outside the selected range and outside every band clears the selection.
  f.events.clear();
  f.d.click({static_cast<float>(f.c->plotRect().x) + 30.0f, static_cast<float>(a.y) + 2.0f});
  CHECK_FALSE(f.c->selection().has_value());
  REQUIRE(f.events.size() == 1);
  CHECK_FALSE(f.events[0].sel.has_value());
}

TEST_CASE("mos chart: Esc (and a right click) during a drag cancels back to the selection at the press") {
  Chart f;
  f.c->setView(600.0, 800.0);
  REQUIRE(f.c->select({5, 2}, 700.0));
  const MosSelection start = *f.c->selection();
  f.d.down(f.axisPoint(700.0));
  f.d.move(f.axisPoint(760.0));  // crosses into another region
  CHECK_FALSE(f.c->selection()->key == start.key);
  CHECK(f.d.keyDown(key::kEscape) == KeyResult::Widget);
  CHECK(*f.c->selection() == start);
  REQUIRE_FALSE(f.events.empty());
  CHECK(f.events.back().gesture == MosGesture::Cancel);
  CHECK(*f.events.back().sel == start);
  CHECK_FALSE(f.c->dragging());
  f.d.up(f.axisPoint(760.0));
  CHECK(*f.c->selection() == start);

  f.events.clear();
  f.d.down(f.axisPoint(712.0));
  f.d.move(f.axisPoint(705.0));
  f.d.down(f.axisPoint(705.0), PointerButton::Right);
  CHECK(*f.c->selection() == start);
  CHECK(f.events.back().gesture == MosGesture::Cancel);
}

TEST_CASE("mos chart: wheel zoom keeps the cents under the pointer; Ctrl+wheel rows; sizes scroll") {
  Chart f;
  const Vec2 p{300.5f, 300.0f};
  const double before = f.c->centsAtX(p.x);
  f.d.wheel(p, 3.0f);
  CHECK(f.c->viewMax() - f.c->viewMin() == doctest::Approx(1200.0 / std::pow(1.15, 3.0)));
  CHECK(f.c->centsAtX(p.x) == doctest::Approx(before).epsilon(1e-12));
  f.d.wheel(p, -1.0f);
  CHECK(f.c->centsAtX(p.x) == doctest::Approx(before).epsilon(1e-12));

  const float rs = f.c->rowScale();
  f.d.wheel(p, 1.0f, 0.0f, mod::kCtrl);
  CHECK(f.c->rowScale() == doctest::Approx(rs * 1.15f));

  CHECK(f.c->firstSize() == 2);
  f.d.wheel(p, -3.0f, 0.0f, mod::kShift);
  CHECK(f.c->firstSize() == 5);

  // Scrollbar: drag the thumb to the bottom → the last sizes.
  const IRect sb = f.c->scrollbarRect();
  REQUIRE_FALSE(sb.empty());
  f.d.drag({static_cast<float>(sb.x + sb.w / 2), static_cast<float>(sb.y) + 2.0f},
           {static_cast<float>(sb.x + sb.w / 2), static_cast<float>(sb.bottom()) + 50.0f});
  CHECK(f.c->firstSize() == 18);

  // Background drag: pans the view and scrolls the sizes (1 per 20 px, up = smaller).
  f.c->setFirstSize(10);
  f.c->setView(400.0, 800.0);
  f.d.tree().layoutNow();
  const IRect plot = f.c->plotRect();
  const Vec2 from{static_cast<float>(plot.x + plot.w / 2), static_cast<float>(plot.bottom() - 10)};
  f.d.drag(from, {from.x - 94.0f, from.y + 40.0f}, 4);
  CHECK(f.c->viewMin() == doctest::Approx(400.0 + 94.0 * 400.0 / (plot.w - 48.0)));
  CHECK(f.c->firstSize() == 8);
}

TEST_CASE("mos chart: setConfig keeps the selection (recalc), maxEdo filters the markers") {
  Chart f;
  f.c->setView(660.0, 740.0);
  REQUIRE(f.c->select({5, 2}, 696.0, 3));
  f.c->setConfig({1200.0, 1, 24, 12, 0.0});
  REQUIRE(f.c->selection().has_value());
  CHECK(f.c->selection()->key == tu::MosKey{5, 2});
  CHECK(f.c->selection()->generatorCents == 696.0);
  CHECK(f.c->selection()->modeRotation == 3);
  f.d.tree().layoutNow();
  for (const MosChartEdoMarker& m : f.c->edoMarkers()) CHECK(m.edo <= 12);
  CHECK(f.marker(7, 12) != nullptr);
  // A sub-octave period: whole-scale keys count every period, the bands repeat across the equave.
  f.c->setConfig({600.0, 2, 12, 31, 0.0});
  f.c->setView(0.0, 1200.0);
  CHECK(f.c->select({2, 1}, 250.0));
  CHECK(f.c->selection()->key == tu::MosKey{4, 2});
  CHECK(f.c->selection()->periodsPerEquave == 2);
  CHECK(f.c->selection()->params().n == 3);
}

TEST_CASE("mos chart: a generator drag damages only the marker, the pill and the selected band, without relayout") {
  Chart f;
  f.c->setView(600.0, 800.0);
  REQUIRE(f.c->select({5, 2}, 700.0));
  f.d.step();
  f.d.down(f.axisPoint(700.0), PointerButton::Left, mod::kAlt);
  f.d.step();
  const uint64_t layouts = f.c->stats().layouts;
  const IRect band = f.c->rowRect(7);
  const int64_t chartArea = static_cast<int64_t>(f.c->rect().w) * f.c->rect().h;
  for (int i = 1; i <= 10; ++i) {
    f.d.move(f.axisPoint(700.0 + i * 1.3), mod::kAlt);
    REQUIRE(f.c->selection()->key == tu::MosKey{5, 2});
    const int64_t a = damageArea(f.d.tree());
    CHECK(a > 0);
    CHECK(a < chartArea / 4);
    for (const ClipRect& r : f.d.tree().damage().rects()) {
      const bool column = r.x1 - r.x0 <= 40;
      const bool inBandRow = r.y0 >= band.y && r.y1 <= band.bottom();  // the band (+ the marker half width)
      const bool pill = r.y1 <= f.c->axisRect().y && r.x1 - r.x0 < 120;
      CHECK_MESSAGE((column || inBandRow || pill), r.x0 << "," << r.y0 << " to " << r.x1 << "," << r.y1);
    }
    f.d.step();
  }
  CHECK(f.c->stats().layouts == layouts);  // the generator never relayouts inside one region
  // A move that does not change the generator damages nothing.
  f.d.move(f.axisPoint(700.0 + 10 * 1.3), mod::kAlt);
  CHECK(damageArea(f.d.tree()) == 0);
  f.d.up(f.axisPoint(700.0 + 10 * 1.3), PointerButton::Left, mod::kAlt);
  f.d.step();
  // Hover with nothing selected: moving from one band to another damages those bands (and the card, if any).
  f.c->clearSelection();
  f.d.step();
  const IRect r8 = f.c->rowRect(8);
  f.d.move({f.c->xOfCents(650.0), static_cast<float>(r8.y + 3)});
  f.d.step();
  f.d.move({f.c->xOfCents(760.0), static_cast<float>(r8.y + 3)});
  CHECK(damageArea(f.d.tree()) < chartArea / 6);
  for (const ClipRect& r : f.d.tree().damage().rects()) CHECK((r.y0 >= r8.y && r.y1 <= r8.bottom()));
}

TEST_CASE("mos chart: hover card text (name, range, parent, hardness, UDP) and the info view") {
  Chart f;
  REQUIRE(f.d.enableGpu().ok());
  f.c->setView(600.0, 800.0);
  f.d.step();
  const IRect r7 = f.c->rowRect(7);
  f.d.move({f.c->xOfCents(700.0), static_cast<float>(r7.y + r7.h / 2)});
  const auto lines = f.c->infoLines();
  REQUIRE(lines.size() >= 5);
  CHECK(lines[0] == "5L 2s \xE2\x80\x94 Diatonic");
  CHECK(lines[1].find("685.71") == 0);
  CHECK(lines[1].find("7 notes") != std::string::npos);
  auto has = [&](std::string_view s) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(s) != std::string::npos; });
  };
  CHECK(has("Parent: 2L 3s"));
  CHECK(has("L:s = 2.00"));
  const tu::RegionHint hint{7, 4.0 / 7.0 * 1200.0, 3.0 / 5.0 * 1200.0};
  const auto udp = tu::computeUdp(700.0, 7, 0, 1200.0, &hint);
  REQUIRE(udp.has_value());
  CHECK(has("UDP " + *udp));
  CHECK(has("Click to select"));
  CHECK_FALSE(f.c->infoCardRect().empty());
  const IRect card = f.c->infoCardRect();
  CHECK(card.x >= f.c->rect().x);
  CHECK(card.right() <= f.c->rect().right());
  CHECK(card.bottom() <= f.c->rect().bottom());
  CHECK(f.c->info().title == lines[0]);
  // Leaving the bands hides it.
  f.d.move({f.c->xOfCents(700.0), static_cast<float>(f.c->axisRect().y)});
  CHECK(f.c->infoLines().empty());
  CHECK(f.c->infoCardRect().empty());
}

namespace {

struct GpuRig {
  std::unique_ptr<GpuContext> ctx;
  std::unique_ptr<Renderer> renderer;
  SceneTextures tex;
  PaintResources res;
};

GpuRig makeRig(float scale) {
  GpuRig g;
  GpuOptions o;
  o.validation = true;
  auto ctx = GpuContext::create(o);
  REQUIRE_MESSAGE(ctx.ok(), (ctx.ok() ? "" : ctx.error().message));
  g.ctx = std::move(*ctx);
  auto r = Renderer::create(*g.ctx, OffscreenTarget::kFormat, 1);
  REQUIRE(r.ok());
  g.renderer = std::move(*r);
  auto t = createSceneTextures(*g.renderer, UiScale(scale));
  REQUIRE(t.ok());
  g.tex = *t;
  g.res = paintResources(g.tex);
  return g;
}

}  // namespace

TEST_CASE("mos chart: partial redraws over a scripted session (hover, drags, crossings, zoom) equal a full redraw") {
  for (const float scale : {1.0f, 1.5f}) {
    CAPTURE(scale);
    GpuRig g = makeRig(scale);
    const auto W = static_cast<uint32_t>(std::lround(800 * scale)), H = static_cast<uint32_t>(std::lround(600 * scale));
    UiTree tree;
    tree.setViewport(W, H, UiScale(scale));
    tree.setResources(&g.res);
    auto& c = tree.emplaceRoot<MosChart>("mos-chart", Chart::defaultConfig());
    c.setView(640.0, 760.0);
    auto partial = OffscreenTarget::create(*g.ctx, W, H);
    auto full = OffscreenTarget::create(*g.ctx, W, H);
    REQUIRE(partial.ok());
    REQUIRE(full.ok());
    DrawList dl;
    double now = 0.0;
    uint64_t partialFrames = 0;
    auto frame = [&](bool forceFull) {
      now += 1.0 / 60.0;
      tree.update(now);
      if (!tree.needsPaint()) return;
      const bool isFull = forceFull || tree.damage().full();
      std::vector<ClipRect> rects(tree.damage().rects().begin(), tree.damage().rects().end());
      IRect box{0, 0, static_cast<int32_t>(W), static_cast<int32_t>(H)};
      if (!isFull) {
        int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (const ClipRect& r : rects) {
          x0 = std::min(x0, r.x0), y0 = std::min(y0, r.y0), x1 = std::max(x1, r.x1), y1 = std::max(y1, r.y1);
        }
        box = {x0, y0, x1 - x0, y1 - y0};
      }
      g.tex.text->beginFrame();
      dl.reset(W, H);
      tree.paint(dl, g.res, now, box);
      const DrawLayer layer{&dl};
      FrameDesc fd;
      fd.layers = {&layer, 1};
      fd.clear = kSceneClearColor;
      fd.full = isFull;
      fd.damage = rects;
      REQUIRE((*partial)->draw(*g.renderer, fd).ok());
      if (!isFull) ++partialFrames;
    };
    auto send = [&](PointerType t, Vec2 p, Mods m = mod::kNone, PointerButton b = PointerButton::Left) {
      PointerEvent e;
      e.type = t;
      e.pos = p;
      e.mods = m;
      e.button = (t == PointerType::Down || t == PointerType::Up) ? b : PointerButton::None;
      e.clickCount = 1;
      tree.dispatchPointer(e);
      frame(false);
    };
    frame(true);
    // Hover across rows (card follows), select 5L 2s by a click, drag the generator with snapping across EDO
    // markers and over the band's end (a crossing), back, release; hover again; zoom; scroll sizes.
    const IRect r7 = c.rowRect(7), r9 = c.rowRect(9);
    for (int i = 0; i < 12; ++i) send(PointerType::Move, {c.xOfCents(650.0 + i * 9.0), static_cast<float>(r9.y + r9.h / 2)});
    send(PointerType::Move, {c.xOfCents(700.0), static_cast<float>(r7.y + r7.h / 2)});
    send(PointerType::Down, {c.xOfCents(700.0), static_cast<float>(r7.y + r7.h / 2)});
    send(PointerType::Up, {c.xOfCents(700.0), static_cast<float>(r7.y + r7.h / 2)});
    const IRect ax = c.axisRect();
    const float ay = static_cast<float>(ax.y + ax.h / 2);
    send(PointerType::Down, {c.xOfCents(700.0), ay});
    for (int i = 0; i < 30; ++i) send(PointerType::Move, {c.xOfCents(700.0 + i * 1.1), ay});
    for (int i = 0; i < 30; ++i) send(PointerType::Move, {c.xOfCents(733.0 - i * 1.7), ay}, mod::kAlt);
    send(PointerType::Up, {c.xOfCents(682.0), ay});
    for (int i = 0; i < 8; ++i) send(PointerType::Move, {c.xOfCents(690.0 + i * 4.0), static_cast<float>(r7.y + 4 + i)});
    PointerEvent wheel;
    wheel.type = PointerType::Wheel;
    wheel.pos = {c.xOfCents(700.0), static_cast<float>(r7.y)};
    wheel.wheel = {0.0f, 1.0f};
    tree.dispatchPointer(wheel);
    frame(false);
    wheel.mods = mod::kShift;
    wheel.wheel = {0.0f, -1.0f};
    tree.dispatchPointer(wheel);
    frame(false);
    send(PointerType::Move, {c.xOfCents(712.0), static_cast<float>(r7.y + 2)});

    // The same state painted in full.
    g.tex.text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, g.res, now);
    REQUIRE((*full)->draw(*g.renderer, dl, kSceneClearColor).ok());
    auto a = (*partial)->read();
    auto b = (*full)->read();
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    const ImageDiff d = compareImages(*a, *b, 0);
    if (d.pixelsOverTolerance != 0) {
      (void)writePng("mos-chart-partial.png", *a);
      (void)writePng("mos-chart-full.png", *b);
    }
    CHECK_MESSAGE(d.pixelsOverTolerance == 0, d.pixelsOverTolerance << " px differ (max " << d.maxChannelDiff << ")");
    CHECK(partialFrames > 50);
    CHECK(g.ctx->validationErrorCount() == 0);
  }
}

namespace {

void checkGolden(const char* scene) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, {}, w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(d));
}

}  // namespace

TEST_CASE("render: mos-chart-default (5L 2s at 7\\12, max EDO 31) matches the golden image") { checkGolden("mos-chart-default"); }

TEST_CASE("render: mos-chart-zoomed (655-745 cents, 18\\31, hover card) matches the golden image") { checkGolden("mos-chart-zoomed"); }
