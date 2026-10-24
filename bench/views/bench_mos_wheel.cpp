// T3.14 benchmarks: the MOS circle (440×560 logical px at 1× and 2×) and the MOS wheel popover over the shell, CPU
// only (text through the CPU atlas).
//   mos_circle/build: a new value (selection change) → scale model rebuild + full paint of the circle.
//   mos_circle/paint(full): the circle repainted with nothing changed (5L 2s; 7L 5s with all intervals; MODMOS).
//   mos_circle/slider frame: a hardness-slider drag frame (input → generatorForHardnessIn → model → damage → paint).
//   mos_circle/hardness solve: generatorForHardnessIn alone (CLI bisection seed + exact refinement).
//   mos_wheel/drag frame: the popover open over a 1600×900 shell whose clip view shows ~3000 notes; a chart generator
//     drag frame = pointer move → chart Live → circle model → ViewContext preview → piano-roll tuning + row rebuild →
//     update → paint of the damage's bounding box, as the window loop does.
// Run the release build; results → bench/RESULTS.md via tools/bench/append_results.py.

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mos_wheel/mos_chart.h"
#include "pq/views/mos_wheel/mos_circle.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/shell/shell.h"
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

MosSelection selectionFor(tuning::MosKey key, double gen, int32_t rotation = 0) {
  MosChartConfig cfg;
  cfg.maxEdo = 31;
  MosChart chart("chart", cfg);
  if (!chart.select(key, gen, rotation) || !chart.selection()) {
    std::printf("no region for the bench selection\n");
    std::exit(1);
  }
  return *chart.selection();
}

struct Rig {
  SceneTextures tex;
  PaintResources res;
  UiTree tree;
  DrawList dl;
  uint32_t w = 0, h = 0;
  double now = 1.0;

  Rig(float scale, float lw, float lh) {
    auto t = createSceneText(std::make_shared<Atlas>(), UiScale(scale));
    if (!t) {
      std::printf("scene text: %s\n", t.error().message.c_str());
      std::exit(1);
    }
    tex = *t;
    res = paintResources(tex);
    const UiScale s(scale);
    w = static_cast<uint32_t>(s.px(lw));
    h = static_cast<uint32_t>(s.px(lh));
    tree.setViewport(w, h, s);
    tree.setResources(&res);
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
  pq::bench::Runner r("pq_bench_mos_wheel");
  for (const float scale : {1.0f, 2.0f}) {
    char tag[32];
    std::snprintf(tag, sizeof tag, "440x560@%.0fx", static_cast<double>(scale));
    const std::string t = tag;
    MosChartConfig cfg;
    cfg.maxEdo = 31;

    r.add("mos_circle/build " + t, [scale, cfg, t](pq::bench::Context& c) {
      Rig rig(scale, 440.0f, 560.0f);
      auto* circle = &rig.tree.emplaceRoot<MosCircle>("mos-circle", cfg);
      rig.tree.layoutNow();
      const MosSelection a = selectionFor({5, 2}, 696.0, 1), b = selectionFor({5, 2}, 697.0, 1);
      int flip = 0;
      c.measure(
          [&] {
            circle->setSelection((flip ^= 1) ? a : b);
            rig.paintFull();
          },
          1, "ns/build", "(model + full paint, 5L 2s)");
      std::printf("  %s: %llu models, %u JI labels, %u vertices, %u commands per paint\n", t.c_str(),
                  static_cast<unsigned long long>(circle->stats().models), circle->stats().jiLabels,
                  static_cast<unsigned>(rig.dl.vertices().size()), static_cast<unsigned>(rig.dl.commands().size()));
    });

    r.add("mos_circle/paint(full) " + t, [scale, cfg](pq::bench::Context& c) {
      Rig rig(scale, 440.0f, 560.0f);
      auto* circle = &rig.tree.emplaceRoot<MosCircle>("mos-circle", cfg);
      rig.tree.layoutNow();
      circle->setSelection(selectionFor({5, 2}, 696.0, 1));
      rig.paintFull();
      c.measure([&] { rig.paintFull(); }, 1, "ns/frame", "5L 2s");
      circle->setSelection(selectionFor({7, 5}, 696.0, 3));
      circle->setShowAllIntervals(true);
      rig.paintFull();
      c.measure([&] { rig.paintFull(); }, 1, "ns/frame", "7L 5s + all intervals");
      circle->setShowAllIntervals(false);
      circle->setSelection(selectionFor({5, 2}, 700.0, 2));
      circle->setModmos({0, 0, 0, 0, 0, 0, 1});
      rig.paintFull();
      c.measure([&] { rig.paintFull(); }, 1, "ns/frame", "MODMOS");
    });

    r.add("mos_circle/slider frame " + t, [scale, cfg](pq::bench::Context& c) {
      Rig rig(scale, 440.0f, 560.0f);
      auto* circle = &rig.tree.emplaceRoot<MosCircle>("mos-circle", cfg);
      rig.tree.layoutNow();
      circle->setSelection(selectionFor({5, 2}, 696.0, 1));
      rig.paintFull();
      const IRect sl = circle->geometry().slider;
      const float y = static_cast<float>(sl.y) + static_cast<float>(sl.h) / 2.0f;
      rig.send(PointerType::Down, {static_cast<float>(sl.x) + 5.0f, y}, mod::kAlt);
      rig.frame();
      int i = 0;
      c.measure(
          [&] {
            const float x = static_cast<float>(sl.x) + static_cast<float>(sl.w) * (0.1f + 0.02f * static_cast<float>(i++ % 40));
            rig.send(PointerType::Move, {x, y}, mod::kAlt);
            rig.frame();
          },
          1, "ns/frame", "(input + solve + model + paint)");
      rig.send(PointerType::Up, {static_cast<float>(sl.x) + 5.0f, y});
    });
  }

  r.add("mos_circle/hardness solve", [](pq::bench::Context& c) {
    const MosSelection s7 = selectionFor({5, 2}, 696.0), s12 = selectionFor({7, 5}, 696.0);
    int i = 0;
    c.measure(
        [&] { pq::bench::doNotOptimize(generatorForHardnessIn(s7, 1.2 + 0.1 * static_cast<double>(i++ % 30))); }, 1, "ns/op",
        "5L 2s");
    c.measure(
        [&] { pq::bench::doNotOptimize(generatorForHardnessIn(s12, 1.2 + 0.1 * static_cast<double>(i++ % 30))); }, 1, "ns/op",
        "7L 5s");
  });

  r.add("mos_wheel/drag frame 1600x900@1x", [](pq::bench::Context& c) {
    Rig rig(1.0f, 1600.0f, 900.0f);
    FakeTransport port;
    ViewContext ctx(port);
    doc::DocumentStore& st = ctx.store();
    auto tr = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi);
    if (!tr || !st.dispatch(tr->command)) std::exit(1);
    const Ticks bars = 30;
    auto cl = doc::ops::insertEmptyClip(*st.current(), st.ids(), tr->tracks[0], 0, bars * kTicksPerWhole);
    if (!cl || !st.dispatch(cl->command)) std::exit(1);
    // ~3000 notes over 30 bars on 24 rows (12edo degrees 0 … 23).
    std::vector<doc::Note> notes(3000);
    uint32_t seed = 777;
    auto rnd = [&] { return seed = seed * 1664525u + 1013904223u; };
    for (size_t i = 0; i < notes.size(); ++i) {
      doc::Note& n = notes[i];
      n.start = static_cast<Ticks>(i) * (bars * kTicksPerWhole) / static_cast<Ticks>(notes.size());
      n.start -= n.start % (kPpq / 4);
      n.length = kPpq / 4;
      const int32_t d = static_cast<int32_t>(rnd() % 24);
      n.pitch = tuning::PitchId::fromDegree(d % 12, d / 12);
    }
    auto add = doc::ops::addNotes(*st.current(), st.ids(), cl->clips[0], notes);
    if (!add || !st.dispatch(add->command)) std::exit(1);
    auto shell = Shell::create(rig.tree, ctx);
    if (!shell) std::exit(1);
    dynamic_cast<Splitter&>(*rig.tree.find("main/body")).setPosition(640.0f);
    ctx.setDetailClip(cl->clips[0]);
    rig.tree.layoutNow();
    auto* view = dynamic_cast<ClipView*>(rig.tree.find("main/body/bottom/detail/clip/clip-view"));
    if (!view) std::exit(1);
    view->fitToClip();
    MosWheelPopover* pop = openMosWheel((*shell)->overlay(), ctx, TuningTarget::forClip(cl->clips[0]), {660, 36, 1, 1});
    if (!pop || !pop->chooseMos({5, 2}, 696.0, 1)) std::exit(1);
    rig.tree.layoutNow();
    rig.frame();
    rig.paintFull();
    MosChart& chart = pop->chart();
    const IRect ax = chart.axisRect();
    const float y = static_cast<float>(ax.y) + static_cast<float>(ax.h) / 2.0f;
    rig.send(PointerType::Down, {chart.xOfCents(696.0), y}, mod::kAlt);
    rig.frame();
    int i = 0;
    c.measure(
        [&] {
          const double cents = 688.0 + static_cast<double>(i++ % 40) * 0.75;  // inside 5L 2s: no region crossing
          rig.send(PointerType::Move, {chart.xOfCents(cents), y}, mod::kAlt);
          rig.frame();
        },
        1, "ns/frame", "(chart drag → circle + roll preview, ~3000 notes)");
    std::printf("  roll rows %u, preview %s\n", static_cast<unsigned>(view->rows().size()), ctx.tuningPreview() ? "on" : "off");
    rig.send(PointerType::Up, {chart.xOfCents(696.0), y});
    pop->cancel();
    rig.frame();
    shell->reset();
  });
  return r.main(argc, argv);
}
