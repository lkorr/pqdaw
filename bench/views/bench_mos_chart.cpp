// T3.13 benchmarks: the MOS region chart at 800×600 logical px (1× and 2× UI scale), CPU only (text through the CPU
// atlas). "build" = what a new chart (or a config change) costs before its first frame: regions + EDO generators
// (rebuild), layout (bands, EDO markers, temperament rows) and a full paint into a draw list. Also each part alone,
// the full paint idle / selected / zoomed, a generator-drag frame (input → damage → partial paint of the damage's
// bounding box) and a hover frame. Acceptance: build < 1 ms CPU at 800×600. Run the release build; results →
// bench/RESULTS.md via tools/bench/append_results.py.

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"
#include "pq/views/mos_wheel/mos_chart.h"
#include "pq_bench.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

IRect damageBox(UiTree& tree) {
  if (tree.damage().full()) return {0, 0, static_cast<int32_t>(tree.width()), static_cast<int32_t>(tree.height())};
  int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
  for (const ClipRect& r : tree.damage().rects()) {
    x0 = std::min(x0, r.x0), y0 = std::min(y0, r.y0), x1 = std::max(x1, r.x1), y1 = std::max(y1, r.y1);
  }
  if (x0 > x1) return {};
  return {x0, y0, x1 - x0, y1 - y0};
}

struct Rig {
  SceneTextures tex;
  PaintResources res;
  UiTree tree;
  MosChart* chart = nullptr;
  DrawList dl;
  uint32_t w = 0, h = 0;
  double now = 1.0;

  explicit Rig(float scale) {
    auto t = createSceneText(std::make_shared<Atlas>(), UiScale(scale));
    if (!t) {
      std::printf("scene text: %s\n", t.error().message.c_str());
      std::exit(1);
    }
    tex = *t;
    res = paintResources(tex);
    const UiScale s(scale);
    w = static_cast<uint32_t>(s.px(800.0f));
    h = static_cast<uint32_t>(s.px(600.0f));
    tree.setViewport(w, h, s);
    tree.setResources(&res);
    MosChartConfig cfg;
    cfg.maxEdo = 31;
    chart = &tree.emplaceRoot<MosChart>("mos-chart", cfg);
    tree.layoutNow();
  }
  void paintFull() {
    tex.text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, res, now);
  }
  void frame() {
    now += 1.0 / 144.0;
    tree.update(now);
    const IRect box = damageBox(tree);
    tex.text->beginFrame();
    dl.reset(w, h);
    if (!box.empty()) tree.paint(dl, res, now, box);
  }
  void send(PointerType t, Vec2 p, Mods m = mod::kNone) {
    PointerEvent e;
    e.type = t;
    e.pos = p;
    e.mods = m;
    e.button = (t == PointerType::Down || t == PointerType::Up) ? PointerButton::Left : PointerButton::None;
    e.clickCount = 1;
    tree.dispatchPointer(e);
  }
};

}  // namespace

int main(int argc, char** argv) {
  pq::bench::Runner r("pq_bench_mos_chart");
  for (const float scale : {1.0f, 2.0f}) {
    char tag[32];
    std::snprintf(tag, sizeof tag, "800x600@%.0fx", static_cast<double>(scale));
    const std::string t = tag;

    r.add("mos_chart/build " + t, [scale, t](pq::bench::Context& c) {
      Rig rig(scale);
      rig.chart->select({5, 2}, 700.0);
      rig.paintFull();
      // A config change (alternating max EDO 31/30 so it is never a no-op) forces rebuild + layout + full paint.
      int flip = 0;
      c.measure(
          [&] {
            MosChartConfig cfg = rig.chart->config();
            cfg.maxEdo = (flip ^= 1) ? 30 : 31;
            rig.chart->setConfig(cfg);
            rig.paintFull();
          },
          1, "ns/build", "(rebuild+layout+paint, 5L 2s selected)");
      c.measure(
          [&] {
            MosChartConfig cfg = rig.chart->config();
            cfg.maxEdo = (flip ^= 1) ? 30 : 31;
            rig.chart->setConfig(cfg);
            pq::bench::doNotOptimize(rig.chart->plotRect());
          },
          1, "ns/op", "rebuild+layout only");
      double v = 0.0;
      c.measure(
          [&] {
            v = v > 0.5 ? 0.0 : 1.0;  // a view change: layout only
            rig.chart->setView(v, 1200.0);
            pq::bench::doNotOptimize(rig.chart->plotRect());
          },
          1, "ns/op", "layout only");
      std::printf("  %s: %u bands, %u labels, %u vertices, %u commands per full paint\n", t.c_str(), rig.chart->stats().bandsPainted,
                  rig.chart->stats().labelsPainted, static_cast<unsigned>(rig.dl.vertices().size()),
                  static_cast<unsigned>(rig.dl.commands().size()));
    });

    r.add("mos_chart/paint(full) " + t, [scale](pq::bench::Context& c) {
      Rig rig(scale);
      rig.paintFull();
      c.measure([&] { rig.paintFull(); }, 1, "ns/frame", "idle");
      rig.chart->select({5, 2}, 700.0);
      rig.paintFull();
      c.measure([&] { rig.paintFull(); }, 1, "ns/frame", "selected");
      rig.chart->setView(655.0, 745.0);
      rig.paintFull();
      c.measure([&] { rig.paintFull(); }, 1, "ns/frame", "zoomed 655-745");
    });

    r.add("mos_chart/drag frame " + t, [scale](pq::bench::Context& c) {
      Rig rig(scale);
      rig.chart->setView(600.0, 800.0);
      rig.chart->select({5, 2}, 700.0);
      rig.paintFull();
      const IRect a = rig.chart->axisRect();
      const float y = static_cast<float>(a.y + a.h / 2);
      rig.send(PointerType::Down, {rig.chart->xOfCents(700.0), y}, mod::kAlt);
      rig.frame();
      int i = 0;
      c.measure(
          [&] {
            // Back and forth inside 5L 2s (no region crossing): input → damage → partial paint.
            const double cents = 690.0 + static_cast<double>(i++ % 40) * 0.6;
            rig.send(PointerType::Move, {rig.chart->xOfCents(cents), y}, mod::kAlt);
            rig.frame();
          },
          1, "ns/frame", "(Alt: no snap)");
      c.measure(
          [&] {
            const double cents = 690.0 + static_cast<double>(i++ % 40) * 0.6;
            rig.send(PointerType::Move, {rig.chart->xOfCents(cents), y});
            rig.frame();
          },
          1, "ns/frame", "(snapping)");
      rig.send(PointerType::Up, {rig.chart->xOfCents(700.0), y});
    });

    r.add("mos_chart/hover frame " + t, [scale](pq::bench::Context& c) {
      Rig rig(scale);
      rig.paintFull();
      const IRect r9 = rig.chart->rowRect(9);
      int i = 0;
      c.measure(
          [&] {
            // Across the size-9 row: band changes, hover card text rebuilt and moved every frame.
            const double cents = 100.0 + static_cast<double>(i++ % 100) * 10.0;
            rig.send(PointerType::Move, {rig.chart->xOfCents(cents), static_cast<float>(r9.y + r9.h / 2)});
            rig.frame();
          },
          1, "ns/frame", "(card on)");
    });
  }
  return r.main(argc, argv);
}
