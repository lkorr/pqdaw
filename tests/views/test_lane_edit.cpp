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
  // Keys' and Pad's windows framed at zoom 1.5 (the windows' column, both bands).
  void frame(std::initializer_list<std::string_view> names = {"Keys", "Pad"}) {
    const ProjectPatch& pp = view().patch();
    float x = 1e9f, y = 1e9f;
    for (std::string_view n : names) {
      const Node& s = pp.nodes[pp.bands[pp.bandOf(track(n).id)].source];
      x = std::min(x, s.x);
      y = std::min(y, s.y);
    }
    circuit::CircuitCanvas::View v;
    v.zoom = 1.5;
    v.x = static_cast<double>(x) - 24.0;
    v.y = static_cast<double>(y) - 24.0;
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
    return laneBodyDevice(view().canvas(), pp.nodes[pp.bands[b].source]);
  }
  Vec2 at(std::string_view name, Ticks tick, float yf = 0.75f) const {
    const Rect r = body(name);
    const double u = static_cast<double>(tick - view().laneStart()) / static_cast<double>(view().laneSpan());
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
    if (hasLaneWindow(b)) want.push_back(b.track);
  REQUIRE(s->rows().size() == want.size());
  for (size_t i = 0; i < want.size(); ++i) CHECK(s->rows()[i].id == want[i]);
  // A row's axis maps the window's time span onto its body; its lane rect is the body (a pixel above and below).
  const int32_t keys = s->rowOf(u.track("Keys").id);
  REQUIRE(keys >= 0);
  const Rect kb = u.body("Keys");
  const arrangement::TimeAxis ax = s->axis(keys);
  CHECK(ax.x(static_cast<double>(v.laneStart())) == doctest::Approx(kb.x0));
  CHECK(ax.x(static_cast<double>(v.laneStart() + v.laneSpan())) == doctest::Approx(kb.x1));
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
  v.setLaneTime(4 * u.bar, 4 * u.bar);  // bars 5–8: Keys is empty until Riff 2 at bar 7
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
  v.setLaneTime(0, 4 * u.bar);
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
  const doc::DeviceId sat = u.track("Keys").devices.at(1)->id;
  const uint32_t n = v.patch().nodeOf(sat);
  REQUIRE(n != kNone);
  const Node& nd = v.patch().nodes[n];
  u.d.click(v.canvas().toDevice({nd.x + nd.w * 0.5f, nd.y + (nd.panel ? 10.0f : nd.h * 0.5f)}));
  CHECK_FALSE(v.canvas().laneFocused());
  const size_t clips = u.track("Pad").clips.size(), devs = u.track("Keys").devices.size();
  (void)u.d.press("Delete");
  CHECK(u.track("Pad").clips.size() == clips);
  CHECK(u.track("Keys").devices.size() == devs - 1);
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
