// T3.11 benchmarks: the clip view over a 100,000-note clip with ~3000 notes in view (1280×800 shell, detail view
// raised): full redraw of the window, and a drag frame (pointer move → preview → damage → partial paint) moving one
// note and moving all ~3000 visible notes. Also the virtual-fundamental scan and the commit of a one-note move.
// CPU only (text through the CPU atlas). Run from the release build; results → bench/RESULTS.md.

#include <algorithm>
#include <climits>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/tuning/mos_regions.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mos_wheel/mos_wheel.h"
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

// T3.12: a live morph frame. A 24-degree MOS clip (per-period size 24, generator in the middle of its region) with
// 3000 Degree notes over 10 equaves (degrees 0 … 239) in an 8-bar view, 3-px lanes (≈ 6 equaves on screen,
// Proportional). One frame = a generator step through the context's tuning preview (as the MOS wheel and the clip
// box do) → the clip view's refresh (in place: MosLinearTable form + RowModel::retune) → layout → paint of the damage.
// A second measure crosses a MosLinearTable interval every frame (full rebuild: buildMOSScale + RowModel::build).
int morphBench(const PaintResources& res, SceneTextures& tex) {
  FakeTransport port;
  ViewContext ctx(port);
  doc::DocumentStore& st = ctx.store();
  const tuning::MosRegionSet regions = tuning::buildMosRegions(24, 24, 1200.0, 1);
  const tuning::MosRegion* region = nullptr;
  for (const tuning::MosRegion& r : regions.regions)
    if (r.perPeriodSize == 24 && (!region || std::fabs(r.lowCents + r.highCents - 1400.0) < std::fabs(region->lowCents + region->highCents - 1400.0))) region = &r;  // nearest 700 c (a meantone-family 24) 
  if (!region) return 1;
  doc::TuningDef def;
  tuning::MosParams mp;
  mp.n = 24;
  mp.generator = 0.5 * (region->lowCents + region->highCents);
  mp.region = region->hint();
  def.system = mp;
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi);
  if (!t || !st.dispatch(t->command)) return 1;
  // TU.3: the project's default tuning is the bench's tuning (no private tunings); the track inherits it.
  if (!st.dispatch(doc::SetTuningDef{st.current()->defaultTuning, def})) return 1;
  const Ticks bars = 8;
  auto c = doc::ops::insertEmptyClip(*st.current(), st.ids(), t->tracks[0], 0, bars * kTicksPerWhole);
  if (!c || !st.dispatch(c->command)) return 1;
  std::vector<doc::Note> notes(3000);
  uint32_t seed = 777;
  auto rnd = [&] { return seed = seed * 1664525u + 1013904223u; };
  for (size_t i = 0; i < notes.size(); ++i) {
    doc::Note& n = notes[i];
    n.start = static_cast<Ticks>(i) * (bars * kTicksPerWhole) / static_cast<Ticks>(notes.size());
    n.start -= n.start % (kPpq / 4);
    n.length = (kPpq / 4) * (1 + rnd() % 4);
    const int32_t d = static_cast<int32_t>(rnd() % 240);
    n.pitch = tuning::PitchId::fromDegree(d % 24, d / 24);
    n.velocity = static_cast<float>(1 + rnd() % 127);
  }
  auto add = doc::ops::addNotes(*st.current(), st.ids(), c->clips[0], notes);
  if (!add || !st.dispatch(add->command)) return 1;

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
  view->setRowHeight(3);
  view->centreOnDegree(120);
  tree.layoutNow();
  DrawList dl;
  double now = 0.0;
  tree.update(now);
  tex.text->beginFrame();
  dl.reset(w, h);
  tree.paint(dl, res, now);
  const TuningTarget target = TuningTarget::forClip(c->clips[0]);
  const double g0 = mp.generator, span = region->highCents - region->lowCents;
  std::printf("morph: 24-degree MOS (region %.3f-%.3f), 3000 notes over 10 equaves, %zu rows, lane %d px, %s\n",
              region->lowCents, region->highCents, view->rows().size(), view->rows().laneHeight(),
              view->rows().layout() == roll::RowLayout::Proportional ? "proportional" : "other");
  auto frameBench = [&](const char* name, bool cross, bool paint) {
    int i = 0;
    uint64_t verts = 0, frames = 0;
    const uint64_t lin0 = view->morphLinearFrames(), reb0 = view->morphRebuilds();
    // Inside one MosLinearTable interval: tiny steps around the centre (an interval is ≥ ~P/n² wide); crossing:
    // alternate between generators beyond either end of the region (its collapsed end is an interval boundary).
    pq::bench::reportStats(name,
                           pq::bench::measure(
                               [&] {
                                 ++i;
                                 tuning::MosParams p = mp;
                                 p.generator = cross ? g0 + ((i & 1) ? 1.5 : -1.5) * span
                                                     : g0 + 1e-3 * static_cast<double>(i % 50);
                                 doc::TuningDef d = def;
                                 d.system = p;
                                 // TU.1: a morph frame is a document change (the wheel / Generator box apply live).
                                 (void)ctx.store().dispatch(tuningCommand(*ctx.doc(), target, std::move(d)));
                                 if (!paint) return;  // the clip view's refresh alone (store event → retune / rebuild)
                                 now += 1.0 / 144.0;
                                 tree.update(now);
                                 const IRect box = bounds(tree.damage().rects());
                                 tex.text->beginFrame();
                                 dl.reset(w, h);
                                 tree.paint(dl, res, now, box);
                                 verts += dl.vertices().size();
                                 ++frames;
                               },
                               1),
                           "ns/frame");
    std::printf("  %.0f vertices per frame; %llu in place, %llu rebuilt\n",
                frames ? static_cast<double>(verts) / static_cast<double>(frames) : 0.0,
                static_cast<unsigned long long>(view->morphLinearFrames() - lin0),
                static_cast<unsigned long long>(view->morphRebuilds() - reb0));
  };
  frameBench("pianoroll.morph frame: 24-degree MOS, 10 equaves, 3000 notes (in place, MosLinearTable)", false, true);
  frameBench("pianoroll.morph frame: 24-degree MOS, crossing an interval every frame (rebuild)", true, true);
  frameBench("pianoroll.morph refresh only (no paint): in place", false, false);
  frameBench("pianoroll.morph refresh only (no paint): rebuild", true, false);
  // T7.16a (M7 review M16): the same morph with 2000 of the notes vibrating (their paths must not all be re-flattened
  // per frame: only where they start moves).
  {
    std::vector<doc::NoteCurves> recs;
    for (const doc::Note& n : ctx.doc()->tracks[0]->clips[0].clip->notes) {
      if (recs.size() >= 2000) break;
      doc::NoteCurves r;
      r.note = n.id;
      r.vibrato.depthCents = 35.0f;
      r.vibrato.rate = 6.0f;
      recs.push_back(r);
    }
    std::sort(recs.begin(), recs.end(), [](const doc::NoteCurves& a, const doc::NoteCurves& b) { return a.note < b.note; });
    if (!ctx.store().dispatch(doc::SetNoteCurves{c->clips[0], recs})) return 1;
    tree.update(now += 0.5);
    tex.text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, res, now);
    const uint64_t built0 = view->pathBuilds();
    frameBench("pianoroll.morph frame, 2000 of the notes vibrating (in place)", false, true);
    std::printf("  %llu paths flattened in all\n", static_cast<unsigned long long>(view->pathBuilds() - built0));
  }
  tree.update(now + 1.0);
  tree.setRoot(std::make_unique<Widget>());
  return 0;
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
  // TU.3: the project's default tuning is the bench's tuning (no private tunings); the track inherits it.
  if (!st.dispatch(doc::SetTuningDef{st.current()->defaultTuning, def})) return 1;
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
  // T7.14 (M7 review M14): notes drawn along a pitch path. 2,000 of the ~3000 visible notes get a 35 c / 6 Hz wobble:
  // "baked" as a free curve of breakpoints (16 per cycle: what a pen stroke or the old baked vibrato stores; the case
  // that exists before and after T7.14), and as the note's vibrato PROPERTY (T7.14: flattened once per clip snapshot
  // into the roll's path cache, RollState::pathOf). Reported: a repaint of the unchanged clip (the cache is warm) and
  // the first paint after an edit (every visible path is flattened again).
  auto curveBench = [&](const char* name, bool property, bool warp = false) {
    // T7.16a (M7 review M16): with `warp`, the clip also holds a small JI group with a warp (far from the view): an
    // edit must still re-flatten only what it touched.
    std::optional<doc::JiGroupId> warped;
    if (warp) {
      const int64_t terms[] = {4, 5, 6};
      auto made = doc::ops::proportionNotes(*ctx.doc(), st.ids(), c->clips[0], 900 * kTicksPerWhole, kTicksPerWhole, tuning::PitchId::fromDegree(0), terms);
      if (!made || !st.dispatch(made->command)) return;
      warped = made->jiGroups.at(0);
      auto wp = doc::ops::setJiWarps(*ctx.doc(), c->clips[0], *warped, {doc::JiWarp{kPpq, kPpq, doc::GlideShape{}, true}});
      if (!wp || !st.dispatch(wp->command)) return;
    }
    const doc::NoteClip& clip = *ctx.doc()->tracks[0]->clips[0].clip;
    const double t0 = view->scrollTick(), t1 = t0 + static_cast<double>(area.w) / view->pxPerTick();
    std::vector<doc::NoteCurves> recs;
    for (const doc::Note& n : clip.notes) {
      if (recs.size() >= 2000) break;
      if (static_cast<double>(n.start) < t0 || static_cast<double>(n.start) >= t1) continue;
      doc::NoteCurves rec;
      rec.note = n.id;
      bool ok = false;
      auto set = [&](auto& r) {
        if constexpr (requires { r.vibrato; }) {
          if (property) {
            r.vibrato.depthCents = 35.0f;
            r.vibrato.rate = 6.0f;
            r.vibrato.fadeIn = n.length / 4;
            ok = true;
          }
        }
      };
      set(rec);
      if (!ok) {
        if (property) return;  // this build has no vibrato property
        const double period = static_cast<double>(kPpq) * 2.0 / 6.0;  // 6 Hz at 120 bpm
        for (Ticks t = 0; t <= n.length; t += static_cast<Ticks>(period / 16.0))
          rec.pitch.push_back({t, static_cast<float>(35.0 * std::sin(6.283185307179586 * static_cast<double>(t) / period)), 0.0f, {}});
      }
      recs.push_back(std::move(rec));
    }
    std::sort(recs.begin(), recs.end(), [](const doc::NoteCurves& a, const doc::NoteCurves& b) { return a.note < b.note; });
    std::vector<doc::NoteCurves> none;
    for (const doc::NoteCurves& r : recs) {
      doc::NoteCurves e;
      e.note = r.note;
      none.push_back(std::move(e));
    }
    const size_t count = recs.size();
    if (!st.dispatch(doc::SetNoteCurves{c->clips[0], recs})) return;
    tree.update(2.0);
    auto paintOnce = [&] {
      tex->text->beginFrame();
      dl.reset(w, h);
      tree.paint(dl, res, 2.0);
      pq::bench::doNotOptimize(dl.indices().size());
    };
    const auto a0 = std::chrono::steady_clock::now();
    paintOnce();
    const double first = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a0).count();
    std::printf("%s: %zu curved notes; first paint after the edit %.3f ms, %u vertices\n", name, count, first, static_cast<unsigned>(dl.vertices().size()));
    pq::bench::reportStats(name, pq::bench::measure(paintOnce, 1), "ns/frame");
    // bug-curve-render: the same view zoomed out 16× (every curved note narrower than its level of detail) and in 8×
    // (a few hundred of them, each tens of px wide: the waves drawn): the paint and its vertices.
    for (const double f : {1.0 / 16.0, 8.0}) {
      const double z = view->pxPerTick(), at = view->scrollTick();
      view->setZoom(z * f, at);
      tree.update(2.2);
      paintOnce();
      const std::string zoomed = std::string(name) + (f < 1.0 ? ", zoomed out 16x" : ", zoomed in 8x");
      std::printf("%s: %u vertices\n", zoomed.c_str(), static_cast<unsigned>(dl.vertices().size()));
      pq::bench::reportStats(zoomed.c_str(), pq::bench::measure(paintOnce, 1), "ns/frame");
      view->setZoom(z, at);
      view->scrollToTick(at);
      tree.update(2.3);
      paintOnce();
    }
    // One of the curved notes moves a row up / down: the edit, the clip view's refresh and a full paint. The other
    // notes' paths must not be flattened again.
    if (!recs.empty()) {
      const doc::NoteId moved = recs[recs.size() / 2].note;
      int dir = 1;
      const std::string edit = std::string(name) + ": one note transposed + refresh + paint";
      pq::bench::reportStats(edit.c_str(),
                             pq::bench::measure(
                                 [&] {
                                   const doc::NoteClip& cl = *ctx.doc()->tracks[0]->clips[0].clip;
                                   const doc::Note* note = nullptr;
                                   for (const doc::Note& x : cl.notes)
                                     if (x.id == moved) {
                                       note = &x;
                                       break;
                                     }
                                   if (!note) return;
                                   const doc::NoteKey key = doc::keyOf(*note);
                                   const auto e = doc::ops::transposeNotes(*ctx.doc(), c->clips[0], std::span<const doc::NoteKey>(&key, 1), dir, doc::ops::StepUnit::Grid);
                                   dir = -dir;
                                   if (e) (void)st.dispatch(e->command);
                                   tree.update(2.5);
                                   paintOnce();
                                 },
                                 1, 9, 2),
                             "ns/edit");
    }
    (void)st.dispatch(doc::SetNoteCurves{c->clips[0], none});
    if (warped) (void)st.undo(), (void)st.undo();  // the group and its warp
    tree.update(3.0);
  };
  curveBench("pianoroll.paint, 2000 notes with a baked 35 c / 6 Hz curve (16 points per cycle)", false);
  curveBench("pianoroll.paint, 2000 notes with a vibrato property (35 c / 6 Hz, fade-in)", true);
  curveBench("pianoroll.paint, 2000 notes with a vibrato property, a JI group with a warp in the clip", true, true);

  // T7.16a (M7 review M15): a ~ grip drag over N selected vibrating notes. One frame = a pointer move (every selected
  // note's vibrato previewed) + update + the repaint of the damage.
  auto vibratoDragBench = [&](const char* name, size_t count) {
    const doc::NoteClip& clip = *ctx.doc()->tracks[0]->clips[0].clip;
    const double t0 = view->scrollTick() + 2.0 * kPpq, t1 = view->scrollTick() + static_cast<double>(area.w) / view->pxPerTick() - 2.0 * kPpq;
    std::vector<doc::NoteCurves> recs;
    std::vector<doc::NoteId> ids;
    for (const doc::Note& n : clip.notes) {
      if (recs.size() >= count) break;
      if (static_cast<double>(n.start) < t0 || static_cast<double>(n.start) >= t1 || n.length < kPpq / 2) continue;
      doc::NoteCurves rec;
      rec.note = n.id;
      rec.vibrato.depthCents = 35.0f;
      rec.vibrato.rate = 6.0f;
      recs.push_back(rec);
      ids.push_back(n.id);
    }
    std::sort(recs.begin(), recs.end(), [](const doc::NoteCurves& a, const doc::NoteCurves& b) { return a.note < b.note; });
    if (recs.empty() || !st.dispatch(doc::SetNoteCurves{c->clips[0], recs})) return;
    view->select(ids);
    tree.update(4.0);
    const doc::NoteId first = ids.front();
    const Vec2 body = view->notePoint(first, 0.5);
    PointerEvent e;
    e.type = PointerType::Move;
    e.pos = body;
    tree.dispatchPointer(e);
    const std::optional<Vec2> grip = view->vibratoGrip(first);
    if (!grip) return;
    e.pos = *grip;
    tree.dispatchPointer(e);
    e.type = PointerType::Down;
    e.button = PointerButton::Left;
    e.clickCount = 1;
    tree.dispatchPointer(e);
    double now = 4.0;
    int i = 0;
    const uint64_t built0 = view->pathBuilds();
    uint64_t frames = 0;
    pq::bench::reportStats(name,
                           pq::bench::measure(
                               [&] {
                                 PointerEvent m;
                                 m.type = PointerType::Move;
                                 m.pos = {grip->x + ((++i & 1) ? 30.0f : 12.0f), grip->y - ((i & 1) ? 20.0f : 10.0f)};
                                 tree.dispatchPointer(m);
                                 now += 1.0 / 144.0;
                                 tree.update(now);
                                 const IRect box = bounds(tree.damage().rects());
                                 tex->text->beginFrame();
                                 dl.reset(w, h);
                                 tree.paint(dl, res, now, box);
                                 ++frames;
                               },
                               1),
                           "ns/frame");
    std::printf("  %zu vibrating notes selected, %.1f paths flattened per frame\n", recs.size(),
                frames ? static_cast<double>(view->pathBuilds() - built0) / static_cast<double>(frames) : 0.0);
    e.type = PointerType::Up;
    tree.dispatchPointer(e);
    view->select({});
    std::vector<doc::NoteCurves> none;
    for (const doc::NoteCurves& r : recs) none.push_back(doc::NoteCurves{r.note, {}, {}, {}, {}});
    (void)st.dispatch(doc::SetNoteCurves{c->clips[0], none});
    tree.update(now + 1.0);
  };
  vibratoDragBench("pianoroll.vibrato grip drag frame: 16 vibrating notes selected", 16);
  vibratoDragBench("pianoroll.vibrato grip drag frame: 150 vibrating notes selected", 150);

  tree.setRoot(std::make_unique<Widget>());
  return morphBench(res, *tex);
}
