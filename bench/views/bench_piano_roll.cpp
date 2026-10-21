// T3.11 benchmarks: the clip view over a 100,000-note clip with ~3000 notes in view (1280×800 shell, detail view
// raised): full redraw of the window, and a drag frame (pointer move → preview → damage → partial paint) moving one
// note and moving all ~3000 visible notes. Also the virtual-fundamental scan and the commit of a one-note move.
// CPU only (text through the CPU atlas). Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <climits>
#include <cstdio>
#include <memory>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"
#include "pq/views/fake_transport.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq_bench.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

IRect bounds(std::span<const ClipRect> rects) {
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
  auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(1.0f));
  if (!tex) {
    std::printf("scene text: %s\n", tex.error().message.c_str());
    return 1;
  }
  const PaintResources res = paintResources(*tex);
  FakeTransport port;
  ViewContext ctx(port);
  doc::DocumentStore& st = ctx.store();

  // 100,000 notes over 1000 bars of 4/4: 100 per bar (a sixteenth grid, 6–7 notes per step) on 48 rows (31edo
  // degrees 0 … 47), velocities 1 … 127.
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi);
  if (!t || !st.dispatch(t->command)) return 1;
  doc::TuningDef def;
  def.system = *tuning::Edo::make(31);
  if (!st.dispatch(doc::SetTrackTuning{t->tracks[0], doc::TuningAssignment::privateTuning(def)})) return 1;
  const Ticks bars = 1000;
  auto c = doc::ops::insertEmptyClip(*st.current(), st.ids(), t->tracks[0], 0, bars * kTicksPerWhole);
  if (!c || !st.dispatch(c->command)) return 1;
  std::vector<doc::Note> notes(100000);
  uint32_t seed = 12345;
  auto rnd = [&] { return seed = seed * 1664525u + 1013904223u; };
  for (size_t i = 0; i < notes.size(); ++i) {
    doc::Note& n = notes[i];
    n.start = static_cast<Ticks>(i) * (bars * kTicksPerWhole) / static_cast<Ticks>(notes.size());
    n.start -= n.start % (kPpq / 4);
    n.length = (kPpq / 4) * (1 + rnd() % 4);
    n.pitch = tuning::PitchId::fromDegree(static_cast<int32_t>(rnd() % 48), 0);
    n.velocity = static_cast<float>(1 + rnd() % 127);
  }
  auto add = doc::ops::addNotes(*st.current(), st.ids(), c->clips[0], notes);
  if (!add || !st.dispatch(add->command)) return 1;
  st.view() = {};

  const uint32_t w = 1280, h = 800;
  UiTree tree;
  tree.setViewport(w, h, UiScale(1.0f));
  tree.setResources(&res);
  auto shell = Shell::create(tree, ctx);
  if (!shell) return 1;
  dynamic_cast<Splitter&>(*tree.find("main/body")).setPosition(520.0f);
  ctx.setDetailClip(c->clips[0]);
  tree.layoutNow();
  auto* view = dynamic_cast<ClipView*>(tree.find("main/body/bottom/detail/clip/clip-view"));
  if (!view) return 1;
  view->setRowHeight(8);
  // 30 bars in view: ≈ 3000 notes.
  const IRect area = tree.find("main/body/bottom/detail/clip/clip-view/notes")->rect();
  view->setZoom(static_cast<double>(area.w) / static_cast<double>(30 * kTicksPerWhole), 0.0);
  view->scrollToTick(static_cast<double>(400 * kTicksPerWhole));
  view->centreOnDegree(24);
  tree.layoutNow();
  size_t visible = 0;
  {
    const double t0 = view->scrollTick(), t1 = t0 + static_cast<double>(area.w) / view->pxPerTick();
    for (const doc::Note& n : ctx.doc()->tracks[0]->clips[0].clip->notes)
      if (static_cast<double>(n.start) < t1 && static_cast<double>(n.start + n.length) > t0) ++visible;
  }

  DrawList dl;
  tex->text->beginFrame();
  dl.reset(w, h);
  tree.update(0.0);
  tree.paint(dl, res, 0.0);
  std::printf("piano roll: 100000 notes, %zu in the visible time range, %u rows of %d px; full frame %u vertices, %u commands\n",
              visible, static_cast<unsigned>(view->rows().size()), view->rows().row(0).height(),
              static_cast<unsigned>(dl.vertices().size()), static_cast<unsigned>(dl.commands().size()));
  pq::bench::reportStats("pianoroll.paint(full window) 1280x800, ~3000 notes in view",
                         pq::bench::measure(
                             [&] {
                               tex->text->beginFrame();
                               dl.reset(w, h);
                               tree.paint(dl, res, 0.0);
                               pq::bench::doNotOptimize(dl.indices().size());
                             },
                             1),
                         "ns/frame");

  // Drag frames: press a visible note, then alternate the pointer between two positions; each iteration is one
  // event dispatch + update (layout, damage) + partial paint of the damage, as the window loop does.
  auto dragBench = [&](const char* name, bool all) {
    const doc::NoteClip& clip = *ctx.doc()->tracks[0]->clips[0].clip;
    const doc::Note* target = nullptr;
    for (const doc::Note& n : clip.notes) {
      const IRect r = view->noteRect(n.id);
      if (!r.empty() && r.w > 6 && r.x > area.x + 200 && r.x < area.right() - 200) {
        target = &n;
        break;
      }
    }
    if (!target) return;
    const IRect r = view->noteRect(target->id);
    const Vec2 from{static_cast<float>(r.x + 3), static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f};
    if (all) view->selectAll();  // before the press: pressing a selected note drags the whole selection
    PointerEvent e;
    e.type = PointerType::Move;
    e.pos = from;
    tree.dispatchPointer(e);
    e.type = PointerType::Down;
    e.button = PointerButton::Left;
    e.clickCount = 1;
    tree.dispatchPointer(e);
    double now = 1.0;
    tree.update(now);
    tex->text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, res, now);
    int i = 0;
    uint64_t verts = 0, frames = 0, area2 = 0;
    pq::bench::reportStats(name,
                           pq::bench::measure(
                               [&] {
                                 PointerEvent m;
                                 m.type = PointerType::Move;
                                 m.pos = {from.x + ((++i & 1) ? 90.0f : 40.0f), from.y - ((i & 1) ? 16.0f : 8.0f)};
                                 tree.dispatchPointer(m);
                                 now += 1.0 / 144.0;
                                 tree.update(now);
                                 const IRect box = bounds(tree.damage().rects());
                                 tex->text->beginFrame();
                                 dl.reset(w, h);
                                 tree.paint(dl, res, now, box);
                                 verts += dl.vertices().size();
                                 area2 += static_cast<uint64_t>(box.w) * static_cast<uint64_t>(box.h);
                                 ++frames;
                               },
                               1),
                           "ns/frame");
    std::printf("  %.0f vertices, %.0f px² damaged per frame on average\n",
                frames ? static_cast<double>(verts) / static_cast<double>(frames) : 0.0,
                frames ? static_cast<double>(area2) / static_cast<double>(frames) : 0.0);
    e.type = PointerType::Up;
    e.pos = from;  // released at the press point: a click, no edit
    tree.dispatchPointer(e);
    view->select({});
    tree.update(now + 1.0);
  };
  dragBench("pianoroll.drag frame: one note", false);
  dragBench("pianoroll.drag frame: all ~3000 visible notes selected (100k selected)", true);

  pq::bench::report("pianoroll.edit refresh: SetClipLoop + clip view refresh (chunks, used rows, fundamental), 100k notes",
                    pq::bench::measure(
                        [&] {
                          // A no-op-free edit that changes the clip pointer: toggle the clip loop.
                          const doc::NoteClip& cl = *ctx.doc()->tracks[0]->clips[0].clip;
                          doc::ClipLoop l = cl.loop;
                          l.enabled = !l.enabled;
                          (void)st.dispatch(doc::SetClipLoop{c->clips[0], l});
                          pq::bench::doNotOptimize(view->virtualFundamental());
                        },
                        1, 9, 2),
                    "ns/edit");
  tree.setRoot(std::make_unique<Widget>());
  return 0;
}
