// T3.7 benchmarks: the shell's build, full paint and the per-frame cost while playing (position readout: timer →
// playhead read → text → damage → partial paint), and a document edit's notification cost through the control bar.
// CPU only (text through the CPU atlas). Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <memory>
#include <tuple>

#include "pq/document/commands.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scenes.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq_bench.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

int main() {
  for (const auto [lw, lh, scale] : {std::tuple{1280u, 800u, 1.0f}, std::tuple{1280u, 720u, 2.0f}}) {
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
    char name[96];

    // Build: shell widgets + keymap file load and bind + first layout.
    std::snprintf(name, sizeof name, "shell.build %ux%u@%.0fx", w, h, static_cast<double>(scale));
    pq::bench::report(name, pq::bench::measure(
                                [&] {
                                  UiTree tree;
                                  tree.setViewport(w, h, s);
                                  tree.setResources(&res);
                                  auto shell = Shell::create(tree, ctx);
                                  pq::bench::doNotOptimize(shell.ok());
                                  tree.setRoot(std::make_unique<Widget>());
                                },
                                1, 9, 2),
                      "ns/build");

    UiTree tree;
    tree.setViewport(w, h, s);
    tree.setResources(&res);
    auto shell = Shell::create(tree, ctx);
    if (!shell) return 1;
    DrawList dl;
    tex->text->beginFrame();
    dl.reset(w, h);
    tree.update(0.0);
    tree.paint(dl, res, 0.0);
    std::printf("shell %ux%u: %llu widgets painted, %u vertices, %u draw commands\n", w, h,
                static_cast<unsigned long long>(tree.stats().widgetsPainted), static_cast<unsigned>(dl.vertices().size()),
                static_cast<unsigned>(dl.commands().size()));
    std::snprintf(name, sizeof name, "shell.paint(full) %ux%u", w, h);
    pq::bench::report(name, pq::bench::measure(
                                [&] {
                                  tex->text->beginFrame();
                                  dl.reset(w, h);
                                  tree.paint(dl, res, 0.0);
                                  pq::bench::doNotOptimize(dl.indices().size());
                                },
                                1),
                      "ns/frame");

    // Playing: each iteration moves the playhead one sixteenth, runs the due position timer and paints the damage.
    ctx.togglePlay();
    double now = 1.0;
    Ticks tick = 0;
    uint64_t verts = 0, frames = 0;
    std::snprintf(name, sizeof name, "shell.playing frame %ux%u", w, h);
    pq::bench::report(name, pq::bench::measure(
                                [&] {
                                  tick += kTicksPerWhole / 16;
                                  port.setPlayheadTick(tick);
                                  now += 0.2;
                                  tree.update(now);
                                  const auto rects = tree.damage().rects();
                                  IRect box{};
                                  if (!rects.empty()) {
                                    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
                                    for (const ClipRect& r : rects) {
                                      x0 = std::min(x0, r.x0);
                                      y0 = std::min(y0, r.y0);
                                      x1 = std::max(x1, r.x1);
                                      y1 = std::max(y1, r.y1);
                                    }
                                    box = {x0, y0, x1 - x0, y1 - y0};
                                  }
                                  tex->text->beginFrame();
                                  dl.reset(w, h);
                                  tree.paint(dl, res, now, box);
                                  verts += dl.vertices().size();
                                  ++frames;
                                },
                                1),
                      "ns/frame");
    std::printf("  playing frame: %.0f vertices on average (the position readout only)\n",
                frames ? static_cast<double>(verts) / static_cast<double>(frames) : 0.0);
    ctx.togglePlay();

    // A document edit while the shell listens (control bar refresh): dispatch + notify, alternating loop on/off.
    bool on = false;
    std::snprintf(name, sizeof name, "shell.dispatch SetLoop + listeners %ux%u", w, h);
    pq::bench::report(name, pq::bench::measure(
                                [&] {
                                  on = !on;
                                  (void)ctx.store().dispatch(doc::SetLoop{{0, 4 * kTicksPerWhole, on}});
                                  tree.update(now);
                                },
                                1),
                      "ns/edit");
    tree.setRoot(std::make_unique<Widget>());
  }
  return 0;
}
