// T3.2 benchmarks: paint cost of the `widgets` gallery (every control, menus, popover, tooltip), drag-box
// interaction cost (pointer move → value → text → damage), meter updates, theme and keymap loading.
// CPU only (text through the CPU atlas). Run from the release build; results → bench/RESULTS.md.

#include <cstdio>
#include <memory>

#include "pq/ui/atlas.h"
#include "pq/ui/controls.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/keymap_file.h"
#include "pq/ui/scenes.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"
#include "pq_bench.h"

using namespace pq::ui;

int main() {
  for (const float scale : {1.0f, 1.5f}) {
    auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(scale));
    if (!tex) {
      std::printf("scene text: %s\n", tex.error().message.c_str());
      return 1;
    }
    const PaintResources res = paintResources(*tex);
    uint32_t lw = 0, lh = 0;
    sceneDefaultSize("widgets", lw, lh);
    const UiScale s(scale);
    const auto w = static_cast<uint32_t>(s.px(static_cast<float>(lw))), h = static_cast<uint32_t>(s.px(static_cast<float>(lh)));
    UiTree tree;
    tree.setViewport(w, h, s);
    tree.setResources(&res);
    buildWidgetsGallery(tree);
    DrawList dl;
    // Warm: every label shaped once (a running UI repaints cached runs).
    tex->text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, res, 0.0);
    std::printf("widgets gallery @%.1fx: %u x %u px, %llu widgets painted, %u vertices, %u indices, %u draw commands\n",
                static_cast<double>(scale), w, h, static_cast<unsigned long long>(tree.stats().widgetsPainted), static_cast<unsigned>(dl.vertices().size()),
                static_cast<unsigned>(dl.indices().size()), static_cast<unsigned>(dl.commands().size()));
    char name[64];
    std::snprintf(name, sizeof name, "widgets.paint@%.1fx", static_cast<double>(scale));
    pq::bench::report(name, pq::bench::measure(
                                [&] {
                                  tex->text->beginFrame();
                                  dl.reset(w, h);
                                  tree.paint(dl, res, 0.0);
                                  pq::bench::doNotOptimize(dl.indices().size());
                                },
                                1),
                      "ns/frame");
    // The same with square controls (radius.control = 0): what the AA rounded corners cost (Q11).
    if (scale == 1.0f) {
      auto square = std::make_shared<Theme>(*tex->theme);
      square->radius.control = 0;
      square->radius.menu = 0;
      square->radius.panel = 0;
      const std::shared_ptr<const Theme> original = tex->theme;
      if (setSceneTheme(*tex, square).ok()) {
        const PaintResources sq = paintResources(*tex);
        pq::bench::report("widgets.paint@1.0x (square corners)", pq::bench::measure(
                                                                        [&] {
                                                                          tex->text->beginFrame();
                                                                          dl.reset(w, h);
                                                                          tree.paint(dl, sq, 0.0);
                                                                        },
                                                                        1),
                          "ns/frame");
        std::printf("  square corners: %u vertices\n", static_cast<unsigned>(dl.vertices().size()));
        (void)setSceneTheme(*tex, original);
      }
    }
    // Building the whole scene (tree + overlays + paint), what `--screenshot widgets` does once.
    std::snprintf(name, sizeof name, "widgets.build+paint@%.1fx", static_cast<double>(scale));
    pq::bench::report(name, pq::bench::measure(
                                [&] {
                                  tex->text->beginFrame();
                                  dl.reset(w, h);
                                  buildScene("widgets", dl, *tex, 0.0);
                                  pq::bench::doNotOptimize(dl.indices().size());
                                },
                                1, 9, 2),
                      "ns/frame");
  }

  // Drag box: one pointer move of a drag (value change, quantise, format, damage), and that plus a repaint of a
  // small tree (what a dragged control costs per frame without the rest of the UI).
  {
    auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(1.0f));
    if (!tex) return 1;
    const PaintResources res = paintResources(*tex);
    UiTree tree;
    tree.setViewport(400, 200);
    tree.setResources(&res);
    auto root = std::make_unique<Widget>();
    root->setLayout(LayoutKind::Column).setPadding(Insets::all(10));
    ValueSpec spec;
    spec.min = 20;
    spec.max = 999;
    spec.def = 120;
    spec.step = 0.01;
    spec.dragPixels = 600;
    auto& box = root->add<DragBox>("tempo", spec);
    box.setSize(Length::fixed(64), Length::fixed(18));
    double sink = 0;
    box.onChange = [&](double v) { sink += v; };
    tree.setRoot(std::move(root));
    tree.layoutNow();
    const Vec2 c{static_cast<float>(box.rect().x + 30), static_cast<float>(box.rect().y + 9)};
    PointerEvent e;
    e.type = PointerType::Move;
    e.pos = c;
    tree.dispatchPointer(e);
    e.type = PointerType::Down;
    e.button = PointerButton::Left;
    e.clickCount = 1;
    tree.dispatchPointer(e);
    e.type = PointerType::Move;
    e.button = PointerButton::None;
    float y = c.y, dir = -1.0f;
    auto step = [&] {
      y += dir;
      if (y < c.y - 300.0f || y > c.y + 300.0f) dir = -dir;
      e.pos.y = y;
      tree.dispatchPointer(e);
    };
    pq::bench::report("dragbox.move (dispatch+value+text+damage)", pq::bench::measure(step, 1), "ns/event");
    DrawList dl;
    pq::bench::report("dragbox.move + repaint (1-control tree)", pq::bench::measure(
                                                                      [&] {
                                                                        step();
                                                                        tree.update(0.0);
                                                                        tex->text->beginFrame();
                                                                        dl.reset(400, 200);
                                                                        tree.paint(dl, res, 0.0);
                                                                      },
                                                                      1),
                      "ns/frame");
    pq::bench::doNotOptimize(sink);
  }

  // Meter: a steady signal fed every frame costs a comparison, no damage.
  {
    UiTree tree;
    tree.setViewport(100, 200);
    auto root = std::make_unique<Widget>();
    auto& m = root->add<Meter>("m");
    m.setSize(Length::fixed(11), Length::fixed(150));
    tree.setRoot(std::move(root));
    tree.layoutNow();
    MeterLevels l;
    l.rmsDb[0] = -20, l.rmsDb[1] = -21, l.peakDb[0] = -12, l.peakDb[1] = -13;
    m.setLevels(l);
    DrawList dl;
    dl.reset(100, 200);
    tree.paint(dl, PaintResources{}, 0.0);  // the meter remembers what it drew; clears the damage
    float jitter = 0.0f;
    pq::bench::report("meter.setLevels (steady, no damage)", pq::bench::measure(
                                                                  [&] {
                                                                    jitter = jitter > 0.0f ? -0.001f : 0.001f;
                                                                    l.rmsDb[0] = -20.0f + jitter;
                                                                    m.setLevels(l);
                                                                  },
                                                                  1),
                      "ns/call");
    std::printf("meter damage after steady updates: %s\n", tree.needsPaint() ? "yes (BUG)" : "none");
  }

  // Theme and keymap files (startup / hot reload).
  pq::bench::report("theme.load (ableton-dark.json)", pq::bench::measure([] { pq::bench::doNotOptimize(loadTheme(defaultThemePath()).ok()); }, 1, 9, 2),
                    "ns/load");
  pq::bench::report("keymap.load (ableton.json)",
                    pq::bench::measure([] { pq::bench::doNotOptimize(loadKeymapFile(defaultKeymapPath()).ok()); }, 1, 9, 2), "ns/load");
  return 0;
}
