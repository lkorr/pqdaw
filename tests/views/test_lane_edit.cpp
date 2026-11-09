// TX.lane-edit (bug report 2026-11-01_13-50-52): the project patch view's lane windows are an editing surface of their
// tracks, run by the ARRANGEMENT's own editor and automation (src/arrangement/edit_surface.h): a drag selects time and
// Ctrl+Shift+M (or the menu's Insert MIDI Clip) makes a clip there; clips move (Alt: copy), trim, delete and duplicate,
// audio clips too; in automation mode breakpoints are drawn, moved and deleted over the clips. One selection with the
// arrangement, one undo step per action. Golden: project-patch-lane-edit. Wall clock (64 tracks) in the perf suite.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/main_path.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../../src/views/src/project_patch/internal.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;

namespace {
// TL.4: a band title strip's buttons (fold, automation) take presses only where they are readable (≥ 8 px titles); the
// band fits (2× mixers make bands larger) may leave them smaller: zoom in on world point `w` first.
inline void readableAt(ProjectPatchView& v, graph::Point2 w) {
  ProjectCanvas& c = v.canvas();
  if (metrics::kTitle * c.deviceScale() >= 12.0f) return;
  const IRect r = c.rect();
  const double us = static_cast<double>(c.deviceScale()) / c.view().zoom;  // device px per logical px
  circuit::CircuitCanvas::View cv;
  cv.zoom = 0.75;
  cv.x = static_cast<double>(w.x) - static_cast<double>(r.w) * 0.5 / (cv.zoom * us);
  cv.y = static_cast<double>(w.y) - static_cast<double>(r.h) * 0.5 / (cv.zoom * us);
  c.setView(cv);
  if (ui::UiTree* t = v.tree()) t->layoutNow();
}

}  // namespace

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// The shell over the sketch project (Keys: Riff bars 1–4, Riff 2 bars 7–10; Pad: bars 3–12), the patch view shown, the
// windows framed (Keys above Pad), a fixed grid of one beat (so the expected ticks do not depend on the zoom).
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  Ticks bar = 0, beat = 0;

  explicit Ui(bool sketch = true) {
    ctx.setDeviceLibrary(&library());
    if (sketch) REQUIRE(buildSketchProject(ctx.store()).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    bar = ticksPerBar(ctx.doc()->timeline->sigMap.signatureAt(0));
    beat = ticksPerBeat(ctx.doc()->timeline->sigMap.signatureAt(0));
    GridSpec g = ctx.grid();
    g.mode = GridMode::Fixed;
    g.fixedExp = 2;  // 1/4: a beat
    g.off = false;
    g.snap = true;
    ctx.setGrid(g);
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  ArrangementView& arrangement() const {
    auto* a = dynamic_cast<ArrangementView*>(shell->slot(ShellSlot::TrackArea).find("arrangement"));
    REQUIRE(a != nullptr);
    return *a;
  }
  void show() {
    ctx.setDetailVisible(false);  // the canvas takes the window's height (the windows framed below stay on it)
    view().setShown(true, false);
    d.tree().layoutNow();
  }
  // Keys' and Pad's lanes framed (TL.1: across the canvas, both bands).
  void frame(std::initializer_list<std::string_view> names = {"Keys", "Pad"}) {
    const ProjectPatch& pp = view().patch();
    float x = 1e9f, y = 1e9f;
    for (std::string_view n : names) {
      const Band& s = pp.bands[pp.bandOf(track(n).id)];
      x = std::min(x, pp.laneX0);
      y = std::min(y, s.laneY);
    }
    // TL.1: the lanes span the bands' width: as close as fits them across the canvas (at most 1.5).
    const IRect cr0 = view().canvas().rect();
    const double fit = (static_cast<double>(cr0.w) - 64.0) / static_cast<double>(d.tree().scale()) / static_cast<double>(pp.laneX1 - pp.laneX0);
    circuit::CircuitCanvas::View v;
    v.zoom = std::min(1.5, fit);
    v.x = static_cast<double>(x) - 24.0;
    v.y = static_cast<double>(y) - 60.0 / v.zoom;  // below the ruler
    view().canvas().setView(v);
    d.tree().layoutNow();
    const IRect cr = view().canvas().rect();
    for (std::string_view n : names) {  // every framed window on the canvas (a gesture leaving it drags clips out)
      const Rect b = body(n);
      REQUIRE_MESSAGE((b.x0 >= static_cast<float>(cr.x) && b.x1 <= static_cast<float>(cr.right()) && b.y0 >= static_cast<float>(cr.y) &&
                       b.y1 <= static_cast<float>(cr.bottom())),
                      std::string(n) << "'s window is off the canvas");
    }
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Track& track(std::string_view name) const {
    const doc::Track* found = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!found && t.name == name) found = &t;
    });
    REQUIRE_MESSAGE(found != nullptr, "no track " << std::string(name));
    return *found;
  }
  // A window body (device px) and a point in it at `tick`, `yf` of the way down (0 top … 1 bottom).
  Rect body(std::string_view name) const {
    const ProjectPatch& pp = view().patch();
    const uint32_t b = pp.bandOf(track(name).id);
    REQUIRE(b != kNone);
    return laneBodyDevice(view().canvas(), pp, pp.bands[b]);
  }
  Vec2 at(std::string_view name, Ticks tick, float yf = 0.75f) const {
    const Rect r = body(name);
    const double u = (static_cast<double>(tick) - view().laneStart()) / view().laneSpan();
    return {r.x0 + static_cast<float>(u * static_cast<double>(r.x1 - r.x0)), r.y0 + yf * (r.y1 - r.y0)};
  }
  // On a clip's title strip (its move grip) at `tick`.
  Vec2 title(std::string_view name, Ticks tick) {
    const Rect r = body(name);
    const float th = laneClipTitleH(r.y1 - r.y0, d.tree().scale());
    REQUIRE(th >= 3.0f);
    Vec2 p = at(name, tick, 0.0f);
    p.y = r.y0 + th * 0.5f;
    return p;
  }
  Menu* openMenu() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  int32_t item(Menu& m, std::string_view label) const {
    for (int32_t i = 0; i < static_cast<int32_t>(m.items().size()); ++i)
      if (m.items()[static_cast<size_t>(i)].label == label) return i;
    return -1;
  }
  const doc::ClipSlot* clipAt(std::string_view name, Ticks start) const {
    for (const doc::ClipSlot& c : track(name).clips)
      if (c.start() == start) return &c;
    return nullptr;
  }
};

}  // namespace

TEST_CASE("lane edit: the windows are a surface of the arrangement's editor (its rows, axes, title strips)") {
  Ui u;
  u.show();
  u.frame();
  ProjectPatchView& v = u.view();
  arrangement::EditSurface* s = v.laneSurface();
  REQUIRE(s != nullptr);
  CHECK(s->set() == &u.arrangement().surfaces());
  // Its rows are the windows in band order (MIDI and audio tracks; groups, returns and the master have none).
  std::vector<doc::TrackId> want;
  for (const Band& b : v.patch().bands)
    if (hasClipRow(b)) want.push_back(b.track);
  REQUIRE(s->rows().size() == want.size());
  for (size_t i = 0; i < want.size(); ++i) CHECK(s->rows()[i].id == want[i]);
  // A row's axis maps the window's time span onto its body; its lane rect is the body (a pixel above and below).
  const int32_t keys = s->rowOf(u.track("Keys").id);
  REQUIRE(keys >= 0);
  const Rect kb = u.body("Keys");
  const arrangement::TimeAxis ax = s->axis(keys);
  CHECK(ax.x(v.laneStart()) == doctest::Approx(kb.x0));
  CHECK(ax.x(v.laneStart() + v.laneSpan()) == doctest::Approx(kb.x1));
  CHECK(s->rowAt((kb.y0 + kb.y1) * 0.5f) == keys);
  CHECK(s->rowLaneRect(keys).y + 1 == static_cast<int32_t>(std::floor(kb.y0)));
  // Embedded views (a group panel, the free rack) have no windows to edit.
  ProjectPatchView rack(u.ctx, ProjectPatchView::FreeRack{});
  CHECK(rack.laneSurface() == nullptr);
}

TEST_CASE("lane edit: a drag selects time; Ctrl+Shift+M (the arrangement's chord) and the menu make a MIDI clip there") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  v.setLaneTime(static_cast<double>(4 * u.bar), static_cast<double>(4 * u.bar));  // bars 5–8: Keys is empty until Riff 2 at bar 7
  u.frame();
  const size_t clips0 = u.track("Keys").clips.size();
  // Drag from bar 5 + 1/8 beat to bar 6 + 1/8 beat: snapped to beats, a range on Keys only.
  u.d.drag(u.at("Keys", 4 * u.bar + u.beat / 8), u.at("Keys", 5 * u.bar + u.beat / 8), 6);
  u.d.step();
  const ArrangementView::Selection& sel = u.arrangement().selection();
  REQUIRE(sel.time.has_value());
  CHECK(sel.time->start == 4 * u.bar);
  CHECK(sel.time->end == 5 * u.bar);
  CHECK(sel.range);
  CHECK(sel.tracks == std::vector<doc::TrackId>{u.track("Keys").id});
  CHECK(sel.clips.empty());
  const size_t h0 = u.history();
  // The arrangement's chord for "Insert MIDI Clip" (the user guessed Ctrl+Shift+T: that is New MIDI Track, globally).
  CHECK(u.d.press("Ctrl+Shift+M") == KeyResult::Widget);
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.track("Keys").clips.size() == clips0 + 1);
  const doc::ClipSlot* made = u.clipAt("Keys", 4 * u.bar);
  REQUIRE(made != nullptr);
  CHECK(made->end() == 5 * u.bar);
  CHECK(!made->isAudio());
  CHECK(u.arrangement().clipSelected(made->id));  // the new clip is selected (in both views)
  REQUIRE(u.ctx.undo());
  CHECK(u.track("Keys").clips.size() == clips0);
  CHECK(u.history() == h0);
  // The right-click menu is the arrangement's edit menu (plus the window's automation entries).
  u.d.drag(u.at("Keys", 4 * u.bar + 2 * u.beat), u.at("Keys", 5 * u.bar + 2 * u.beat), 6);
  u.d.click(u.at("Keys", 5 * u.bar), mod::kNone, PointerButton::Right);
  u.d.step();
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  CHECK(u.item(*m, "Duplicate") >= 0);
  CHECK(u.item(*m, "Automation Mode") >= 0);
  CHECK(u.item(*m, "Draw Automation") >= 0);
  const int32_t ins = u.item(*m, "Insert MIDI Clip");
  REQUIRE(ins >= 0);
  m->choose(ins);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const doc::ClipSlot* menuClip = u.clipAt("Keys", 4 * u.bar + 2 * u.beat);
  REQUIRE(menuClip != nullptr);
  CHECK(menuClip->end() == 5 * u.bar + 2 * u.beat);
  // A drag across both windows selects time on both tracks (one range over the rows it crosses).
  v.setLaneTime(0.0, static_cast<double>(4 * u.bar));
  u.frame();
  u.d.drag(u.at("Keys", u.bar, 0.8f), u.at("Pad", 3 * u.bar, 0.8f), 8);
  u.d.step();
  const ArrangementView::Selection& both = u.arrangement().selection();
  REQUIRE(both.tracks.size() == 2);
  CHECK(both.tracks[0] == u.track("Keys").id);
  CHECK(both.tracks[1] == u.track("Pad").id);
  CHECK(both.clips.size() == 2);  // Riff and Pad touch bars 2–3
}

TEST_CASE("lane edit: clips move (Alt copies), trim, delete and duplicate — one undo step each, the arrangement agrees") {
  Ui u;
  u.show();
  u.frame();
  ProjectPatchView& v = u.view();
  const doc::TrackId pad = u.track("Pad").id;
  const doc::ClipId padClip = u.track("Pad").clips.at(0).id;
  REQUIRE(u.track("Pad").clips.at(0).start() == 2 * u.bar);
  size_t h = u.history();
  // Move: its title strip one bar left (relative snap from the clip's start).
  u.d.drag(u.title("Pad", 3 * u.bar), u.title("Pad", 2 * u.bar), 6);
  u.d.step();
  CHECK(u.history() == h + 1);
  REQUIRE(u.track("Pad").clips.size() == 1);
  CHECK(u.track("Pad").clips[0].start() == u.bar);
  CHECK(u.track("Pad").clips[0].id == padClip);
  CHECK(u.arrangement().clipSelected(padClip));
  CHECK(u.ctx.detailClip().clip == padClip);  // shown in the clip view, as an arrangement click does
  // The arrangement sees the same document: its rows have the moved clip; the window shows it.
  CHECK(v.laneClips(v.patch().bandOf(pad)).at(0).clip == padClip);
  REQUIRE(u.ctx.undo());
  CHECK(u.track("Pad").clips[0].start() == 2 * u.bar);
  h = u.history();
  // Alt+drag copies (the original stays).
  u.d.drag(u.title("Pad", 3 * u.bar), u.title("Pad", 2 * u.bar), 6, mod::kAlt);
  u.d.step();
  CHECK(u.history() == h + 1);
  CHECK(u.track("Pad").clips.size() == 2);
  REQUIRE(u.ctx.undo());
  CHECK(u.track("Pad").clips.size() == 1);
  h = u.history();
  // Trim: the left edge one bar later (the window's edge grab, as in the arrangement).
  const Vec2 edge = u.at("Pad", 2 * u.bar, 0.6f);
  u.d.drag({edge.x + 1.0f, edge.y}, {u.at("Pad", 3 * u.bar).x + 1.0f, edge.y}, 6);
  u.d.step();
  CHECK(u.history() == h + 1);
  CHECK(u.track("Pad").clips[0].start() == 3 * u.bar);
  CHECK(u.track("Pad").clips[0].end() == 12 * u.bar);
  REQUIRE(u.ctx.undo());
  h = u.history();
  // Delete: a click selects it (the title strip), Delete removes it.
  u.d.click(u.title("Pad", 3 * u.bar));
  CHECK(u.d.press("Delete") == KeyResult::Widget);
  CHECK(u.history() == h + 1);
  CHECK(u.track("Pad").clips.empty());
  REQUIRE(u.ctx.undo());
  REQUIRE(u.track("Pad").clips.size() == 1);
  h = u.history();
  // Duplicate (Ctrl+D): a copy right after it, selected.
  u.d.click(u.title("Pad", 3 * u.bar));
  CHECK(u.d.press("Ctrl+D") == KeyResult::Widget);
  CHECK(u.history() == h + 1);
  REQUIRE(u.track("Pad").clips.size() == 2);
  CHECK(u.track("Pad").clips[1].start() == 12 * u.bar);
  CHECK_FALSE(u.arrangement().clipSelected(padClip));
  REQUIRE(u.ctx.undo());
  h = u.history();
  // Onto another window (Keys: another MIDI track) by dragging down… up: Pad's clip onto Keys at bar 3.
  u.d.drag(u.title("Pad", 3 * u.bar), u.title("Keys", 3 * u.bar), 8);
  u.d.step();
  CHECK(u.history() == h + 1);
  CHECK(u.track("Pad").clips.empty());
  const doc::ClipSlot* moved = u.clipAt("Keys", 2 * u.bar);
  REQUIRE(moved != nullptr);
  CHECK(moved->end() == 12 * u.bar);
  CHECK(moved->name() == "Pad");
  REQUIRE(u.ctx.undo());
  // Esc during a drag: nothing changes.
  h = u.history();
  u.d.down(u.title("Pad", 3 * u.bar));
  u.d.move(u.title("Pad", 2 * u.bar));
  u.d.move(u.title("Pad", u.bar));
  CHECK(v.laneDragging());
  CHECK(v.canvas().drag() == ProjectCanvas::Drag::Lane);
  (void)u.d.press("Escape");
  CHECK_FALSE(v.laneDragging());
  u.d.up(u.title("Pad", u.bar));
  CHECK(u.history() == h);
  CHECK(u.track("Pad").clips[0].start() == 2 * u.bar);
}

TEST_CASE("lane edit: one selection with the arrangement; the canvas's own keys when the last press was elsewhere") {
  Ui u;
  u.show();
  u.frame();
  ProjectPatchView& v = u.view();
  const doc::ClipRef riff{u.track("Keys").id, u.track("Keys").clips.at(0).id};
  // Selected in the arrangement: outlined in the window (its damage reaches the window). The track selected first (a
  // new selected track repaints the views showing it in full).
  u.ctx.setSelectedTrack(riff.track);
  u.d.step();
  u.arrangement().selectClips(std::span<const doc::ClipRef>(&riff, 1), false);
  REQUIRE_FALSE(u.d.tree().damage().full());
  const Rect kb = u.body("Keys");
  bool damaged = false;
  for (const ClipRect& r : u.d.tree().damage().rects())
    damaged = damaged || (static_cast<float>(r.x1) > kb.x0 && static_cast<float>(r.x0) < kb.x1 && static_cast<float>(r.y1) > kb.y0 &&
                          static_cast<float>(r.y0) < kb.y1);
  CHECK(damaged);
  u.d.step();
  // Selected in the window: the arrangement's selection (and its clip view).
  u.d.click(u.title("Pad", 3 * u.bar));
  CHECK(u.arrangement().selection().clips.size() == 1);
  CHECK(u.arrangement().clipSelected(u.track("Pad").clips[0].id));
  CHECK(v.canvas().laneFocused());
  // A press on a device block: the canvas's keys again (Delete deletes the block, not the clip).
  v.focusBand(u.track("Keys").id, false);
  u.d.tree().layoutNow();
  const doc::DeviceId sat = doc::mainPath(*u.ctx.doc(), u.track("Keys")).blocks.at(1)->id;  // (GU.11: the Saturator module)
  const uint32_t n = v.patch().nodeOf(sat);
  REQUIRE(n != kNone);
  const Node& nd = v.patch().nodes[n];
  u.d.click(v.canvas().toDevice({nd.x + nd.w * 0.5f, nd.y + (nd.panel ? 10.0f : nd.h * 0.5f)}));
  CHECK_FALSE(v.canvas().laneFocused());
  const size_t clips = u.track("Pad").clips.size(), blocks = doc::mainPath(*u.ctx.doc(), u.track("Keys")).blocks.size();
  (void)u.d.press("Delete");
  CHECK(u.track("Pad").clips.size() == clips);
  CHECK(doc::findDevice(*u.ctx.doc(), sat).device == nullptr);  // the module with its blocks
  CHECK(doc::mainPath(*u.ctx.doc(), u.track("Keys")).blocks.size() == blocks - 1);
}

TEST_CASE("lane edit: audio clips move and trim in their windows like MIDI clips") {
  Ui u;
  doc::DocumentStore& st = u.ctx.store();
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, u.track("Mod").id, "Loop");
  REQUIRE(t.ok());
  REQUIRE(st.dispatch(t->command).ok());
  const doc::TrackId loop = t->tracks.at(0);
  doc::SampleRef sr;
  sr.id = st.ids().sample();
  sr.path = "loop.wav";  // missing on disk: no handles, but every clip edit applies (warp Off trims stop at the file)
  sr.rate = 48000.0;
  sr.frames = 8 * 48000;
  sr.channels = 2;
  const doc::ops::NewAudioClip nc{loop, u.bar, sr.id, {}};
  auto a = doc::ops::addAudioClips(*st.current(), st.ids(), std::span<const doc::SampleRef>(&sr, 1), std::span<const doc::ops::NewAudioClip>(&nc, 1));
  REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.error().message));
  REQUIRE(st.dispatch(a->command).ok());
  u.show();
  u.frame({"Loop"});
  ProjectPatchView& v = u.view();
  REQUIRE(v.laneSurface()->rowOf(loop) >= 0);
  const doc::ClipId id = u.track("Loop").clips.at(0).id;
  const Ticks len = u.track("Loop").clips[0].length();
  size_t h = u.history();
  u.d.drag(u.title("Loop", u.bar + u.beat), u.title("Loop", 2 * u.bar + u.beat), 6);
  u.d.step();
  CHECK(u.history() == h + 1);
  REQUIRE(u.track("Loop").clips.size() == 1);
  CHECK(u.track("Loop").clips[0].id == id);
  CHECK(u.track("Loop").clips[0].start() == 2 * u.bar);
  CHECK(u.track("Loop").clips[0].length() == len);
  // Trimmed from its start (the left edge a beat later; the end stays).
  h = u.history();
  const Ticks end = u.track("Loop").clips[0].end();
  const Vec2 e = u.at("Loop", 2 * u.bar, 0.6f);
  u.d.drag({e.x + 1.0f, e.y}, {u.at("Loop", 2 * u.bar + u.beat).x + 1.0f, e.y}, 6);
  u.d.step();
  CHECK(u.history() == h + 1);
  CHECK(u.track("Loop").clips[0].start() == 2 * u.bar + u.beat);
  CHECK(u.track("Loop").clips[0].end() == end);
  REQUIRE(u.ctx.undo());
  CHECK(u.track("Loop").clips[0].start() == 2 * u.bar);
}

TEST_CASE("lane edit: automation mode draws, moves and deletes breakpoints over a window's clips — one undo step each") {
  Ui u;
  u.show();
  u.frame();
  ProjectPatchView& v = u.view();
  ArrangementAutomation& au = u.arrangement().automation();
  arrangement::EditSurface& s = *v.laneSurface();
  const doc::TrackId pad = u.track("Pad").id;
  const doc::ParamRef vol = doc::ParamRef::track(doc::ParamKind::TrackVolume, pad);
  // A press in a window, then A (the arrangement's chord): automation mode, Pad's volume (its default) over its clips.
  u.d.click(u.at("Pad", u.bar, 0.8f));
  CHECK(u.d.press("A") == KeyResult::Widget);
  REQUIRE(au.mode());
  CHECK(au.chosen(pad) == vol);
  const int32_t row = s.rowOf(pad);
  auto band = [&] {
    const std::optional<AutomationBand> b = au.rowBandOn(s, row);
    REQUIRE(b.has_value());
    return *b;
  };
  const AutomationBand b0 = band();
  CHECK(b0.lane == nullptr);
  CHECK(b0.overClips);
  // A click on the (dashed) line at bar 2 + 1/16 beat: a breakpoint there (snapped), the lane created in the same step.
  const double v0 = currentParamValue(*u.ctx.doc(), vol);
  const Vec2 on{s.axis(row).x(static_cast<double>(u.bar + u.beat / 16)), au.yOfValue(b0, v0)};
  const size_t h0 = u.history();
  u.d.click(on);
  CHECK(u.history() == h0 + 1);
  LaneLookup l = findAutomationLane(*u.ctx.doc(), vol);
  REQUIRE(l.lane != nullptr);
  REQUIRE(l.lane->points.size() == 1);
  CHECK(l.lane->points[0].tick == u.bar);
  CHECK(l.lane->points[0].value == doctest::Approx(v0).epsilon(1e-3));
  // Drag it up and a beat later: one step, value up, time on the grid.
  const AutomationBand b1 = band();
  const Vec2 p0{s.axis(row).x(static_cast<double>(u.bar)), au.yOfValue(b1, l.lane->points[0].value)};
  const float up = std::min(20.0f, (p0.y - b1.top) * 0.5f);
  REQUIRE(up > 2.0f);
  u.d.drag(p0, {s.axis(row).x(static_cast<double>(u.bar + u.beat)), p0.y - up}, 6);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  l = findAutomationLane(*u.ctx.doc(), vol);
  REQUIRE(l.lane->points.size() == 1);
  CHECK(l.lane->points[0].tick == u.bar + u.beat);
  CHECK(l.lane->points[0].value > v0);
  // Double-click deletes it.
  const AutomationBand b2 = band();
  u.d.doubleClick({s.axis(row).x(static_cast<double>(u.bar + u.beat)), au.yOfValue(b2, l.lane->points[0].value)});
  u.d.step();
  CHECK(u.history() == h0 + 3);
  l = findAutomationLane(*u.ctx.doc(), vol);
  REQUIRE(l.lane != nullptr);
  CHECK(l.lane->points.empty());
  // Undo, one step at a time: the point back where it was dragged to, then where it was made.
  REQUIRE(u.ctx.undo());
  CHECK(findAutomationLane(*u.ctx.doc(), vol).lane->points.at(0).tick == u.bar + u.beat);
  REQUIRE(u.ctx.undo());
  CHECK(findAutomationLane(*u.ctx.doc(), vol).lane->points.at(0).tick == u.bar);
  // The clips' title strips stay the clips' in automation mode (a drag there moves the clip, not the envelope).
  const size_t h1 = u.history();
  u.d.drag(u.title("Pad", 2 * u.bar + 2 * u.beat), u.title("Pad", 3 * u.bar + 2 * u.beat), 6);
  CHECK(u.history() == h1 + 1);
  CHECK(u.track("Pad").clips[0].start() == 3 * u.bar);
  // The window menu's Draw Automation offers the track's other parameters (its panning) for its clips.
  u.d.click(u.at("Pad", 2 * u.bar, 0.95f), mod::kNone, PointerButton::Right);
  u.d.step();
  CHECK(u.openMenu() != nullptr);
  const std::vector<MenuItem> params = au.parameterItems(pad);
  CHECK(std::any_of(params.begin(), params.end(), [](const MenuItem& m) { return m.label == "Track Panning"; }));
}

// TL.2 (docs/plans/TL.md 8, 9): a band lane's automation sub-lanes are the arrangement's shown lanes of its track (one
// state), edited by the arrangement's ArrangementAutomation on the lane surface (one selection, one undo history).
TEST_CASE("lane edit (TL.2): the band's toggle shows its sub-lanes; a point drawn there is the arrangement's; undo") {
  Ui u;
  u.show();
  u.frame();
  ProjectPatchView& v = u.view();
  ArrangementAutomation& au = u.arrangement().automation();
  arrangement::EditSurface& s = *v.laneSurface();
  const doc::TrackId keys = u.track("Keys").id;
  const doc::ParamRef vol = doc::ParamRef::track(doc::ParamKind::TrackVolume, keys);
  REQUIRE(v.subLanes().empty());
  REQUIRE(s.laneRows().empty());
  const float h0 = v.patch().bands[v.patch().bandOf(keys)].h;
  // The automation toggle in Keys' title strip (after the fold triangle), through the window's dispatch.
  v.focusBand(keys, false);
  u.d.tree().layoutNow();
  const LaneRect tr = ProjectCanvas::subLaneButtonRect(v.patch().bands[v.patch().bandOf(keys)]);
  readableAt(v, {(tr.x0 + tr.x1) * 0.5f, (tr.y0 + tr.y1) * 0.5f});
  u.d.click(v.canvas().toDevice({(tr.x0 + tr.x1) * 0.5f, (tr.y0 + tr.y1) * 0.5f}));
  u.d.step();
  CHECK(au.lanesShown(keys));
  CHECK(v.subLanesShown(keys));
  REQUIRE(v.subLanes().size() == 1);  // the chosen parameter (volume) the first time, as the arrangement's header toggle
  CHECK(v.subLanes()[0].param == vol);
  const Band& kb = v.patch().bands[v.patch().bandOf(keys)];
  CHECK(v.subLanes()[0].band == v.patch().bandOf(keys));
  CHECK(kb.subH == doctest::Approx(au.laneHeight(keys, vol)));
  CHECK(kb.h == doctest::Approx(h0 + kb.subH));
  CHECK(v.subLanes()[0].y == doctest::Approx(kb.laneY + kb.laneH));  // right under the lane
  // The arrangement shows the same lane under Keys (one state).
  const std::vector<doc::ParamRef> shown = au.shownLanes(keys);
  CHECK(std::find(shown.begin(), shown.end(), vol) != shown.end());
  u.frame();
  // It is a lane of the surface: its rect is the sub-lane's, on the lanes' axis.
  REQUIRE(s.laneRows().size() == 1);
  const Rect sr = v.subLaneDevice(0);
  CHECK(s.laneRect(0).y == static_cast<int32_t>(sr.y0));
  CHECK(s.laneRows()[0].param == vol);
  CHECK(s.laneRows()[0].track == keys);
  REQUIRE(v.subLaneRect(keys, vol).has_value());
  // A click on its (dashed) line at bar 2: a breakpoint there, the lane made in the same step.
  std::optional<AutomationBand> b0;
  {
    arrangement::Surfaces::Bind bind(u.arrangement().surfaces(), &s);
    b0 = au.band(keys, vol, false);
  }
  REQUIRE(b0.has_value());
  CHECK(b0->rect.y == s.laneRect(0).y);
  const double v0 = currentParamValue(*u.ctx.doc(), vol);
  const Vec2 on{s.axis(s.laneRows()[0].row).x(static_cast<double>(u.bar)), au.yOfValue(*b0, v0)};
  CHECK(v.subLaneAt(on) == 0u);
  const size_t hist = u.history();
  u.d.click(on);
  u.d.step();
  CHECK(u.history() == hist + 1);
  LaneLookup l = findAutomationLane(*u.ctx.doc(), vol);
  REQUIRE(l.lane != nullptr);
  REQUIRE(l.lane->points.size() == 1);
  CHECK(l.lane->points[0].tick == u.bar);
  // The same envelope in the arrangement: its lane under Keys shows the point (its own surface and axis).
  const std::optional<AutomationBand> ab = au.band(keys, vol, false);
  REQUIRE(ab.has_value());
  CHECK(ab->lane == l.lane);
  CHECK(au.pointPos(*ab, l.lane->points[0].id).has_value());
  // One selection: the point drawn in the band is the arrangement's selected breakpoint.
  CHECK(au.selectedParam() == vol);
  CHECK(au.selectedPoints().size() == 1);
  // Undo: the point and the lane gone, in both views.
  REQUIRE(u.ctx.undo());
  CHECK(findAutomationLane(*u.ctx.doc(), vol).lane == nullptr);
  u.d.step();
  // The toggle again: hidden (both views), the band back to its height.
  v.focusBand(keys, false);
  u.d.tree().layoutNow();
  const LaneRect tr2 = ProjectCanvas::subLaneButtonRect(v.patch().bands[v.patch().bandOf(keys)]);
  readableAt(v, {(tr2.x0 + tr2.x1) * 0.5f, (tr2.y0 + tr2.y1) * 0.5f});
  u.d.click(v.canvas().toDevice({(tr2.x0 + tr2.x1) * 0.5f, (tr2.y0 + tr2.y1) * 0.5f}));
  u.d.step();
  CHECK_FALSE(au.lanesShown(keys));
  CHECK(v.subLanes().empty());
  CHECK(v.patch().bands[v.patch().bandOf(keys)].h == doctest::Approx(h0));
}

TEST_CASE("lane edit (TL.2): a press on a band's control shows its sub-lane (mixer, a device's control); master; folded") {
  Ui u;
  u.show();
  u.frame();
  ProjectPatchView& v = u.view();
  ArrangementAutomation& au = u.arrangement().automation();
  const doc::TrackId keys = u.track("Keys").id;
  auto panAt = [&] {
    const Node& mx = v.patch().nodes[v.patch().bands[v.patch().bandOf(keys)].mixer];
    return v.canvas().toDevice({mx.x + 27.0f * mx.scale, mx.y + 45.0f * mx.scale});  // TX.mixer-look: the pan knob's centre in its face
  };
  // The mixer face's pan knob, pressed through the window.
  {
    v.focusBand(keys, false);
    u.d.tree().layoutNow();
    u.d.click(panAt());
    u.d.step();
    const doc::ParamRef pr = doc::ParamRef::track(doc::ParamKind::TrackPan, keys);
    CHECK(au.lanesShown(keys));
    const std::vector<doc::ParamRef> shown = au.shownLanes(keys);
    CHECK(std::find(shown.begin(), shown.end(), pr) != shown.end());  // the arrangement's lane too
    CHECK(au.chosen(keys) == pr);
    CHECK(v.subLaneRect(keys, pr).has_value());
    // Pressed again: nothing new (no re-layout).
    const uint64_t rebuilds = v.stats().rebuilds;
    const size_t n = v.subLanes().size();
    u.d.click(panAt());
    u.d.step();
    CHECK(v.subLanes().size() == n);
    CHECK(v.stats().rebuilds == rebuilds);
  }
  // A device's control (a face's, a panel's or a value block's: the first one found over Keys' blocks): its sub-lane.
  {
    std::optional<std::pair<Vec2, doc::Binding>> hit;
    const Band& kb = v.patch().bands[v.patch().bandOf(keys)];
    const IRect cr = v.canvas().rect();
    for (const uint32_t d : kb.devices) {
      const Node& n = v.patch().nodes[d];
      if (n.folded) continue;
      for (float y = n.y + 4.0f; y < n.y + n.h && !hit; y += 2.0f)
        for (float x = n.x + 4.0f; x < n.x + n.w && !hit; x += 2.0f) {
          const Vec2 p = v.canvas().toDevice({x, y});
          if (!cr.contains(p) || p.y < static_cast<float>(cr.y) + 60.0f || v.canvas().pinAt(p).valid() || v.laneEditAt(p)) continue;
          const doc::Binding b = v.controlAt(p);
          const doc::DeviceLoc dl = doc::findDevice(*u.ctx.doc(), b.device);
          if (b.kind == doc::BindingKind::Control && b.meta.empty() && dl.track != nullptr && dl.track->id == keys &&
              paramValid(*u.ctx.doc(), doc::ParamRef::device(b.device, b.name)))
            hit = std::make_pair(p, b);
        }
    }
    REQUIRE_MESSAGE(hit.has_value(), "no control of Keys' blocks found on the canvas");
    const doc::ParamRef dr = doc::ParamRef::device(hit->second.device, hit->second.name);
    u.d.click(hit->first);
    u.d.step();
    CHECK(v.selectedControl() == hit->second);
    CHECK(v.subLaneRect(keys, dr).has_value());
    const std::vector<doc::ParamRef> shown = au.shownLanes(keys);
    CHECK(std::find(shown.begin(), shown.end(), dr) != shown.end());
  }
  // The master (a lane with no clip row) gets sub-lanes too; a point drawn there is the master's lane (one step).
  const doc::TrackId master = u.ctx.doc()->master->id;
  const doc::ParamRef mv = doc::ParamRef::track(doc::ParamKind::TrackVolume, master);
  {
    v.showSubLanes(master, true);
    u.d.step();
    REQUIRE(v.subLaneRect(master, mv).has_value());
    const uint32_t mb = v.patch().bandOf(master);
    circuit::CircuitCanvas::View cv = v.canvas().view();
    cv.zoom = 1.0;
    cv.x = static_cast<double>(v.patch().laneX0) - 24.0;
    cv.y = static_cast<double>(v.patch().bands[mb].laneY) - 120.0;
    v.canvas().setView(cv);
    u.d.tree().layoutNow();
    const Rect r = *v.subLaneRect(master, mv);
    REQUIRE(r.y1 < static_cast<float>(v.canvas().rect().bottom()));
    arrangement::EditSurface& s = *v.laneSurface();
    std::optional<AutomationBand> b;
    {
      arrangement::Surfaces::Bind bind(u.arrangement().surfaces(), &s);
      b = au.band(master, mv, false);
    }
    REQUIRE(b.has_value());
    const size_t hist = u.history();
    const Vec2 on{s.axis(-1).x(static_cast<double>(2 * u.bar)), au.yOfValue(*b, currentParamValue(*u.ctx.doc(), mv))};
    u.d.click(on);
    u.d.step();
    CHECK(u.history() == hist + 1);
    const LaneLookup l = findAutomationLane(*u.ctx.doc(), mv);
    REQUIRE(l.lane != nullptr);
    CHECK(l.owner == master);
    CHECK(l.lane->points.size() == 1);
  }
  // A sub-lane's bottom edge resizes it (the arrangement's lane height: one height for both views).
  {
    const Rect r = *v.subLaneRect(master, mv);
    const float before = au.laneHeight(master, mv);
    const float x = std::max(r.x0, static_cast<float>(v.canvas().rect().x)) + 60.0f;  // on the canvas
    CHECK(v.canvas().laneEdgeAt({x, r.y1 - 0.5f}).kind == ProjectCanvas::LaneEdgeHit::Kind::SubBottom);
    CHECK(u.d.down({x, r.y1 - 0.5f}));
    CHECK(v.canvas().drag() == ProjectCanvas::Drag::LaneSize);
    CHECK(u.d.move({x, r.y1 - 0.5f + 10.0f * v.canvas().deviceScale()}));
    CHECK(u.d.move({x, r.y1 - 0.5f + 20.0f * v.canvas().deviceScale()}));
    CHECK(u.d.up({x, r.y1 - 0.5f + 20.0f * v.canvas().deviceScale()}));
    u.d.step();
    CHECK(au.laneHeight(master, mv) == doctest::Approx(before + 20.0f).epsilon(0.05));
    CHECK(v.subLaneRect(master, mv)->y1 - v.subLaneRect(master, mv)->y0 > r.y1 - r.y0);
  }
  // A folded band keeps its lane and its sub-lanes.
  {
    v.setBandFolded(keys, true);
    u.d.step();
    const Band& kb = v.patch().bands[v.patch().bandOf(keys)];
    CHECK(kb.folded);
    CHECK(kb.subH > 0.0f);
    CHECK(kb.h == doctest::Approx(metrics::kTitle + laneBlock(kb)));
    bool any = false;
    for (const ProjectPatchView::SubLane& sl : v.subLanes()) any = any || sl.band == v.patch().bandOf(keys);
    CHECK(any);
  }
}

// TL.5 (Q239 option 1): one FOLLOW lane per track that shows the last pressed control's parameter; pinning keeps a
// parameter as a lane of its own (pinned lanes above the follow lane); the arrangement's Show Automation re-points the
// follow lane (a pinned parameter: its lane revealed); the automation wire lands on whichever lane shows the parameter.
// All view state: no undo steps.
TEST_CASE("lane edit (TL.5): presses re-point the one follow lane; pin keeps a lane; Show Automation re-points; unpin; the wire") {
  Ui u;
  u.show();
  u.frame();
  ProjectPatchView& v = u.view();
  ArrangementAutomation& au = u.arrangement().automation();
  const doc::TrackId keys = u.track("Keys").id;
  const uint32_t kb = v.patch().bandOf(keys);
  auto lanesOf = [&] {  // Keys' sub-lanes in the band view, top to bottom
    std::vector<doc::ParamRef> out;
    for (const ProjectPatchView::SubLane& sl : v.subLanes())
      if (sl.band == v.patch().bandOf(keys)) out.push_back(sl.param);
    return out;
  };
  v.focusBand(keys, false);
  u.d.tree().layoutNow();
  // Knob A: the mixer face's pan. Knob B: a control of one of Keys' blocks (a face's, a panel's or a value block's).
  const doc::ParamRef a = doc::ParamRef::track(doc::ParamKind::TrackPan, keys);
  auto panAt = [&] {
    const Node& mx = v.patch().nodes[v.patch().bands[v.patch().bandOf(keys)].mixer];
    return v.canvas().toDevice({mx.x + 27.0f * mx.scale, mx.y + 45.0f * mx.scale});
  };
  std::optional<std::pair<doc::Binding, graph::Point2>> hitB;  // (world: the canvas may scroll)
  {
    const IRect cr = v.canvas().rect();
    for (const uint32_t d : v.patch().bands[kb].devices) {
      const Node& n = v.patch().nodes[d];
      if (n.folded) continue;
      for (float y = n.y + 4.0f; y < n.y + n.h && !hitB; y += 2.0f)
        for (float x = n.x + 4.0f; x < n.x + n.w && !hitB; x += 2.0f) {
          const Vec2 p = v.canvas().toDevice({x, y});
          if (!cr.contains(p) || p.y < static_cast<float>(cr.y) + 60.0f || v.canvas().pinAt(p).valid() || v.laneEditAt(p)) continue;
          const doc::Binding b = v.controlAt(p);
          const doc::DeviceLoc dl = doc::findDevice(*u.ctx.doc(), b.device);
          if (b.kind == doc::BindingKind::Control && b.meta.empty() && dl.track != nullptr && dl.track->id == keys &&
              paramValid(*u.ctx.doc(), doc::ParamRef::device(b.device, b.name)))
            hitB = std::make_pair(b, graph::Point2{x, y});
        }
    }
  }
  REQUIRE_MESSAGE(hitB.has_value(), "no control of Keys' blocks found on the canvas");
  const doc::ParamRef b = doc::ParamRef::device(hitB->first.device, hitB->first.name);
  auto pressB = [&] {
    u.d.click(v.canvas().toDevice(hitB->second));
    u.d.step();
  };
  auto pressA = [&] {
    u.d.click(panAt());
    u.d.step();
  };
  auto clickPin = [&](const doc::ParamRef& p) {
    size_t i = v.subLanes().size();
    for (size_t k = 0; k < v.subLanes().size(); ++k)
      if (v.subLanes()[k].band == v.patch().bandOf(keys) && v.subLanes()[k].param == p) i = k;
    REQUIRE(i < v.subLanes().size());
    // Zoomed in on the sub-lane's left end (the pin is drawn and taken at the near level of detail), then back.
    const circuit::CircuitCanvas::View keep = v.canvas().view();
    circuit::CircuitCanvas::View cv = keep;
    cv.zoom = 1.0;
    cv.x = static_cast<double>(v.patch().laneX0) - 24.0;
    cv.y = static_cast<double>(v.subLanes()[i].y) - 120.0;
    v.canvas().setView(cv);
    u.d.tree().layoutNow();
    const std::optional<Rect> pin = v.subLanePinDevice(i);
    REQUIRE_MESSAGE(pin.has_value(), "the sub-lane's pin is not drawn at this zoom");
    u.d.click({(pin->x0 + pin->x1) * 0.5f, (pin->y0 + pin->y1) * 0.5f});
    u.d.step();
    v.canvas().setView(keep);
    u.d.tree().layoutNow();
  };
  const size_t hist = u.history();

  // A then B: one lane, showing B (re-pointed in place: the same height, no re-layout).
  pressA();
  REQUIRE(lanesOf() == std::vector<doc::ParamRef>{a});
  CHECK(au.isFollowLane(keys, a));
  const uint64_t rebuilds = v.stats().rebuilds;
  pressB();
  CHECK(v.selectedControl() == hitB->first);
  REQUIRE(lanesOf() == std::vector<doc::ParamRef>{b});
  CHECK(au.shownLanes(keys) == std::vector<doc::ParamRef>{b});  // the arrangement's lanes too (one state)
  CHECK(au.followParam(keys) == b);
  CHECK(v.stats().rebuilds == rebuilds);
  {  // the wire lands on B's (follow) lane
    const std::optional<ProjectPatchView::AutomationWire> w = v.automationWire();
    REQUIRE(w.has_value());
    REQUIRE(v.subLaneRect(keys, b).has_value());
    CHECK(w->lane.y0 == doctest::Approx(v.subLaneRect(keys, b)->y0));
  }

  // Pin B (the pin on its label, through the window): still one lane, now pinned.
  clickPin(b);
  CHECK(au.pinned(keys, b));
  CHECK_FALSE(au.isFollowLane(keys, b));
  CHECK(lanesOf() == std::vector<doc::ParamRef>{b});
  // A again: B stays pinned, the follow lane below it shows A.
  pressA();
  REQUIRE(lanesOf() == (std::vector<doc::ParamRef>{b, a}));
  CHECK(au.pinned(keys, b));
  CHECK(au.isFollowLane(keys, a));
  CHECK(v.subLaneRect(keys, b)->y1 <= v.subLaneRect(keys, a)->y0 + 0.5f);  // pinned above the follow lane
  {  // the wire lands on A's (follow) lane
    const std::optional<ProjectPatchView::AutomationWire> w = v.automationWire();
    REQUIRE(w.has_value());
    CHECK(w->lane.y0 == doctest::Approx(v.subLaneRect(keys, a)->y0));
  }
  // B again: its pinned lane (nothing added, the follow lane keeps A); the wire lands on the pinned lane.
  pressB();
  CHECK(lanesOf() == (std::vector<doc::ParamRef>{b, a}));
  {
    const std::optional<ProjectPatchView::AutomationWire> w = v.automationWire();
    REQUIRE(w.has_value());
    CHECK(w->lane.y0 == doctest::Approx(v.subLaneRect(keys, b)->y0));
  }

  // The arrangement's Show Automation (a knob's menu: ViewContext::showAutomation) points the follow lane at its
  // parameter; on a pinned parameter it only reveals that lane.
  const doc::ParamRef vol = doc::ParamRef::track(doc::ParamKind::TrackVolume, keys);
  u.ctx.showAutomation(keys, vol);
  u.d.step();
  CHECK(lanesOf() == (std::vector<doc::ParamRef>{b, vol}));
  CHECK(au.chosen(keys) == vol);
  u.ctx.showAutomation(keys, b);
  u.d.step();
  CHECK(lanesOf() == (std::vector<doc::ParamRef>{b, vol}));
  CHECK(au.followParam(keys) == vol);
  // Hidden lanes: Show Automation shows them again with the follow lane on it.
  v.showSubLanes(keys, false);
  u.d.step();
  CHECK(lanesOf().empty());
  u.ctx.showAutomation(keys, a);
  u.d.step();
  CHECK(lanesOf() == (std::vector<doc::ParamRef>{b, a}));

  // Unpin B (its pin again): its lane goes, the follow lane stays.
  clickPin(b);
  CHECK_FALSE(au.pinned(keys, b));
  CHECK(lanesOf() == std::vector<doc::ParamRef>{a});
  // Pin / unpin from the envelope menu (a right-click in the sub-lane): "Pin Lane" on the follow lane, then "Unpin Lane".
  auto menuOnA = [&](std::string_view label) {
    const Rect r = *v.subLaneRect(keys, a);
    const float x = std::max(r.x0, static_cast<float>(v.canvas().rect().x)) + 80.0f;
    u.d.click({x, (r.y0 + r.y1) * 0.5f + 6.0f}, mod::kNone, PointerButton::Right);
    u.d.step();
    Menu* m = u.openMenu();
    REQUIRE(m != nullptr);
    const int32_t i = u.item(*m, label);
    REQUIRE(i >= 0);
    m->choose(i);
    u.d.step();
  };
  menuOnA("Pin Lane");
  CHECK(au.pinned(keys, a));
  CHECK(lanesOf() == std::vector<doc::ParamRef>{a});
  menuOnA("Unpin Lane");
  CHECK_FALSE(au.pinned(keys, a));
  CHECK(lanesOf() == std::vector<doc::ParamRef>{a});  // still the follow lane's parameter
  CHECK(au.isFollowLane(keys, a));

  // View state only: no undo steps.
  CHECK(u.history() == hist);
}

TEST_CASE("lane edit (TL.3): the automation wire from a pressed control to its sub-lane; it follows a block drag; Esc, a press elsewhere, hiding the sub-lane end it") {
  Ui u;
  u.show();
  u.frame();
  ProjectPatchView& v = u.view();
  const doc::TrackId keys = u.track("Keys").id;
  CHECK_FALSE(v.automationWire().has_value());
  v.focusBand(keys, false);
  u.d.tree().layoutNow();
  // A device's control on Keys' blocks with a rim (a face's or a panel's knob), pressed through the window.
  std::optional<std::pair<Vec2, doc::Binding>> hit;
  const Band& kb = v.patch().bands[v.patch().bandOf(keys)];
  const IRect cr = v.canvas().rect();
  for (const uint32_t d : kb.devices) {
    const Node& n = v.patch().nodes[d];
    if (n.folded) continue;
    for (float y = n.y + 4.0f; y < n.y + n.h && !hit; y += 2.0f)
      for (float x = n.x + 4.0f; x < n.x + n.w && !hit; x += 2.0f) {
        const Vec2 p = v.canvas().toDevice({x, y});
        if (!cr.contains(p) || p.y < static_cast<float>(cr.y) + 60.0f || v.canvas().pinAt(p).valid() || v.laneEditAt(p)) continue;
        const doc::Binding b = v.controlAt(p);
        const doc::DeviceLoc dl = doc::findDevice(*u.ctx.doc(), b.device);
        const auto cd = v.controlDevice(b);
        if (b.kind == doc::BindingKind::Control && b.meta.empty() && dl.track != nullptr && dl.track->id == keys && cd &&
            cd->second > 2.0f && paramValid(*u.ctx.doc(), doc::ParamRef::device(b.device, b.name)))
          hit = std::make_pair(p, b);
      }
  }
  REQUIRE_MESSAGE(hit.has_value(), "no knob of Keys' blocks found on the canvas");
  const doc::Binding ctl = hit->second;
  const doc::ParamRef pr = doc::ParamRef::device(ctl.device, ctl.name);
  u.d.click(hit->first);
  u.d.step();
  REQUIRE(v.subLaneRect(keys, pr).has_value());
  // The envelope drawn in its sub-lane at device x `x` (the arrangement's own evaluation on the band lanes' surface).
  auto envelopeY = [&](const doc::ParamRef& p, float x) -> std::optional<float> {
    const arrangement::EditSurface* sf = v.laneSurface();
    if (sf == nullptr) return std::nullopt;
    const auto rows = sf->laneRows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].track == keys && rows[i].param == p) {
        Vec2 out[2];
        if (!u.arrangement().automation().envelopePoints(*sf, i, x, x, out)) return std::nullopt;
        return out[0].y;
      }
    return std::nullopt;
  };
  // Its ends: on the control's rim; TL.4: ON the envelope drawn in the sub-lane (the flat line of the current value: no
  // points), landing beside the control (offset along x: a slant, never a straight drop).
  auto checkEnds = [&](const char* when) {
    INFO(when);
    const std::optional<ProjectPatchView::AutomationWire> w = v.automationWire();
    REQUIRE(w.has_value());
    const auto cd = v.controlDevice(ctl);
    REQUIRE(cd.has_value());
    const Rect lane = *v.subLaneRect(keys, pr);
    const graph::Point2 off = v.canvas().drag() == ProjectCanvas::Drag::MoveDevice ? v.canvas().nodeDragOffset() : graph::Point2{0.0f, 0.0f};
    const float s = v.canvas().deviceScale();
    const Vec2 c{cd->first.x + off.x * s, cd->first.y + off.y * s};
    CHECK(w->centre.x == doctest::Approx(c.x).epsilon(1e-4));
    CHECK(w->centre.y == doctest::Approx(c.y).epsilon(1e-4));
    CHECK(w->radius == doctest::Approx(cd->second));
    CHECK(std::hypot(w->from.x - c.x, w->from.y - c.y) == doctest::Approx(cd->second).epsilon(1e-3));
    REQUIRE(c.y < lane.y0);
    CHECK(w->to.y > lane.y0);
    CHECK(w->to.y < lane.y1);
    const std::optional<float> ey = envelopeY(pr, w->to.x);
    REQUIRE(ey.has_value());
    CHECK(w->to.y == doctest::Approx(*ey).epsilon(1e-3));
    CHECK(w->normal.y == 1.0f);
    const float vx0 = std::max(lane.x0, static_cast<float>(cr.x)), vx1 = std::min(lane.x1, static_cast<float>(cr.right()));
    CHECK(w->to.x >= vx0);
    CHECK(w->to.x <= vx1);
    CHECK(std::fabs(w->to.x - c.x) >= 10.0f);  // beside the control, not under it
    return *w;
  };
  const ProjectPatchView::AutomationWire w0 = checkEnds("pressed");
  // Drag its block by a grip on it that is no control (its title bar, its body): the wire follows the control drawn live.
  const uint32_t mn = v.patch().nodeOf(ctl.device);
  REQUIRE(mn != kNone);
  std::optional<Vec2> grip;
  {
    const Node& n = v.patch().nodes[mn];
    for (float y = n.y + 2.0f; y < n.y + n.h && !grip; y += 2.0f)
      for (float x = n.x + 4.0f; x < n.x + n.w && !grip; x += 2.0f) {
        const Vec2 p = v.canvas().toDevice({x, y});
        if (cr.contains(p) && v.canvas().nodeAt(p) == mn && !v.controlAt(p).bound() && !v.canvas().pinAt(p).valid() &&
            !v.laneEditAt(p))
          grip = p;
      }
  }
  REQUIRE_MESSAGE(grip.has_value(), "no grip on the control's block");
  const float s = v.canvas().deviceScale();
  const Vec2 to{grip->x + 30.0f * s, grip->y};
  u.d.move(*grip);
  u.d.down(*grip);
  for (int k = 1; k <= 6; ++k) {
    u.d.move({grip->x + (to.x - grip->x) * static_cast<float>(k) / 6.0f, grip->y});
    u.d.step();
  }
  REQUIRE(v.canvas().drag() == ProjectCanvas::Drag::MoveDevice);
  const ProjectPatchView::AutomationWire w1 = checkEnds("dragged");
  CHECK(w1.centre.x == doctest::Approx(w0.centre.x + 30.0f * s).epsilon(1e-3));
  CHECK(w1.to.x == doctest::Approx(w0.to.x + 30.0f * s).epsilon(1e-3));
  u.d.up(to);
  u.d.step();
  CHECK(v.canvas().drag() == ProjectCanvas::Drag::None);
  (void)checkEnds("dropped");  // still there: the press was on its own block
  // Esc: gone.
  u.d.press("Escape");
  u.d.step();
  CHECK_FALSE(v.automationWire().has_value());
  // Pressed again: back; a press on empty canvas ends it.
  const auto cd = v.controlDevice(ctl);
  REQUIRE(cd.has_value());
  u.d.click(cd->first);
  u.d.step();
  REQUIRE(v.automationWire().has_value());
  {
    std::optional<Vec2> empty;
    for (float y = static_cast<float>(cr.y) + 70.0f; y < static_cast<float>(cr.bottom()) && !empty; y += 7.0f)
      for (float x = static_cast<float>(cr.x) + 10.0f; x < static_cast<float>(cr.right()) && !empty; x += 7.0f)
        if (v.canvas().nodeAt({x, y}) == kNone && !v.laneEditAt({x, y}) && v.subLaneAt({x, y}) == kNone &&
            v.canvas().laneEdgeAt({x, y}).kind == ProjectCanvas::LaneEdgeHit::Kind::None && !v.canvas().pinAt({x, y}).valid() &&
            v.canvas().edgeAt({x, y}) == kNone && v.panelWindowAt({x, y}) == nullptr)
          empty = Vec2{x, y};
    REQUIRE(empty.has_value());
    u.d.click(*empty);
    u.d.step();
    CHECK_FALSE(v.automationWire().has_value());
  }
  // Pressed again, then its sub-lane hidden (the band's toggle): gone with it.
  u.d.click(v.controlDevice(ctl)->first);
  u.d.step();
  REQUIRE(v.automationWire().has_value());
  const LaneRect tr = ProjectCanvas::subLaneButtonRect(v.patch().bands[v.patch().bandOf(keys)]);
  readableAt(v, {(tr.x0 + tr.x1) * 0.5f, (tr.y0 + tr.y1) * 0.5f});
  u.d.click(v.canvas().toDevice({(tr.x0 + tr.x1) * 0.5f, (tr.y0 + tr.y1) * 0.5f}));
  u.d.step();
  CHECK_FALSE(v.subLanesShown(keys));
  CHECK_FALSE(v.automationWire().has_value());
  // The mixer's pan knob: a wire from its rim to the pan's sub-lane.
  {
    const Node& mx = v.patch().nodes[v.patch().bands[v.patch().bandOf(keys)].mixer];
    const Vec2 pan = v.canvas().toDevice({mx.x + 27.0f * mx.scale, mx.y + 45.0f * mx.scale});
    u.d.click(pan);
    u.d.step();
    const std::optional<ProjectPatchView::AutomationWire> w = v.automationWire();
    REQUIRE(w.has_value());
    CHECK(std::hypot(w->centre.x - pan.x, w->centre.y - pan.y) < 0.5f);
    const doc::ParamRef panRef = doc::ParamRef::track(doc::ParamKind::TrackPan, keys);
    const Rect lane = *v.subLaneRect(keys, panRef);
    CHECK(w->to.y > lane.y0);
    CHECK(w->to.y < lane.y1);
    const std::optional<float> ey = envelopeY(panRef, w->to.x);
    REQUIRE(ey.has_value());
    CHECK(w->to.y == doctest::Approx(*ey).epsilon(1e-3));
  }
}

TEST_CASE("render: project-patch-auto-wire matches the golden image") {
  uint32_t w = 0, h = 0;
  const char* name = "project-patch-auto-wire";
  viewSceneDefaultSize(name, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(name, {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(name) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  CHECK(pq::ui::test::matchesGolden(name, *img, *ref, true));
}

TEST_CASE("render: project-patch-lane-edit matches the golden image") {
  uint32_t w = 0, h = 0;
  const char* name = "project-patch-lane-edit";
  viewSceneDefaultSize(name, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(name, {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(name) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  CHECK(pq::ui::test::matchesGolden(name, *img, *ref, true));
}

// ---- wall clock (perf suite: load-sensitive) --------------------------------------------------------------------------

TEST_CASE("lane edit: 64 tracks — the windows painted by the arrangement's painters, a range and automation, within a frame" *
          doctest::test_suite("perf")) {
  Ui u(false);
  REQUIRE(buildWideProject(u.ctx.store(), 64).ok());
  REQUIRE(u.d.enableGpu().ok());
  u.show();
  ProjectPatchView& v = u.view();
  v.fitAll(false);
  u.d.advance(1.0);
  // Zoomed onto the windows' column (Mid / Near), every window with a range over all tracks and automation mode.
  const ProjectPatch& pp = v.patch();
  const Node& first = pp.nodes[pp.bands[pp.bandOf(u.track("Track 1").id)].source];
  circuit::CircuitCanvas::View view;
  view.zoom = 0.8;
  view.x = static_cast<double>(first.x) - 24.0;
  view.y = static_cast<double>(first.y) - 24.0;
  v.canvas().setView(view);
  u.d.advance(0.5);
  auto frames = [&](const char* what) {
    std::vector<double> ms;
    for (int i = 0; i < 30; ++i) {
      v.canvas().markDirty();
      const auto t0 = std::chrono::steady_clock::now();
      u.d.step();
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    MESSAGE("64 tracks, windows framed (" << what << "): median full frame " << ms[ms.size() / 2] << " ms, "
                                          << v.stats().lanesPainted << " windows painted in all");
    return ms[ms.size() / 2];
  };
  const double plain = frames("no selection");
  ArrangementView::Selection sel;
  doc::forEachTrack(*u.ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (t.kind == doc::TrackKind::Midi) sel.tracks.push_back(t.id);
  });
  sel.time = TickRange{u.bar, 3 * u.bar};
  sel.range = true;
  u.arrangement().setSelection(sel, false);
  u.arrangement().automation().setMode(true);
  const double edited = frames("a range on every track, automation mode");
  CHECK(edited < 1000.0 / 60.0);
  // A drag frame: a time selection dragged in one window (the selection's damage over the windows it covers).
  const Vec2 a = u.at("Track 1", u.bar / 2), b = u.at("Track 1", 2 * u.bar);
  u.d.down(a);
  std::vector<double> dm;
  for (int i = 1; i <= 20; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    u.d.move({a.x + (b.x - a.x) * static_cast<float>(i) / 20.0f, a.y});
    u.d.step();
    dm.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  u.d.up(b);
  std::sort(dm.begin(), dm.end());
  MESSAGE("64 tracks: a time-selection drag frame in a window, median " << dm[dm.size() / 2] << " ms (plain full frame " << plain << " ms)");
  CHECK(dm[dm.size() / 2] < 1000.0 / 60.0);
}
