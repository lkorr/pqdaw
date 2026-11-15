// T7.6 / T7.14 piano roll: bends between notes (on the derived API: a bend names its neighbour), pen modes,
// breakpoints, free detune. Every gesture goes through the real shell, pointer and key dispatch (TestDriver → UiTree
// → the note area / the keymap / the menus), one undo step each:
//   Alt + a note's left / right edge (bend in / out: ops NoteBend, nothing baked; the neighbour — M9: the nearest in
//   pitch among the notes that end / start within a grid step of the nearest; it is outlined while dragging; the
//   bend follows it when it moves; removing it; its shape by menu); Ctrl+G (the selection glides); Draw Mode: a stroke
//   on a note (bounded points; a click — or a drag of a few px, M7 — still deletes the note; a stroke is never a
//   bend, H4), Ctrl + click (a breakpoint), Alt + drag (the note's vibrato property); breakpoints (drag with row
//   snapping, Shift free, in time, the diamond's curvature, double-click deletes); Shift + body (free detune past a
//   dead zone with a live status line, H5; Shift + click only selects); a resize leaves bends on their edges and a
//   drawn curve where it is; duplicate carries the record; Clear Bends.
// Goldens: pianoroll-bends, pianoroll-note-lines. Vibrato by dragging: test_piano_roll_vibrato.cpp.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "pq/document/automation_curve.h"
#include "pq/document/commands.h"
#include "pq/document/pitch_resolve.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../../src/views/src/piano_roll/roll_state.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using pq::tuning::PitchId;
using pq::tuning::PitchKind;
using pq::views::roll::CurveTarget;
using pq::views::roll::RollState;

namespace {

constexpr Ticks kQ = kPpq;
constexpr float kRow = 14.0f;   // px per 12-EDO step (100 ¢)
constexpr float kBeat = 60.0f;  // px per quarter note

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)}; }

// The shell over the empty project with one new MIDI track and an empty four-bar 12-EDO clip open in the clip view, a
// quarter-note grid, 60 px per beat, 14 px rows.
struct Pen {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ClipView* view = nullptr;
  doc::ClipRef ref{};

  Pen() {
    REQUIRE(ctx.openProject(views::test::fixtureDir("empty")).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(480.0f);  // the detail view at its tallest
    doc::DocumentStore& st = ctx.store();
    auto t = doc::ops::createTrack(*ctx.doc(), st.ids(), doc::TrackKind::Midi, {}, "Pen", doc::Colour{0x5FA8D8});
    REQUIRE(t.ok());
    REQUIRE(st.dispatch(t->command).ok());
    auto c = doc::ops::insertEmptyClip(*ctx.doc(), st.ids(), t->tracks.at(0), 0, 4 * kTicksPerWhole, "Clip");
    REQUIRE(c.ok());
    REQUIRE(st.dispatch(c->command).ok());
    ref = c->clips.at(0);
    ctx.setDetailClip(ref);
    d.tree().layoutNow();
    view = dynamic_cast<ClipView*>(d.find("main/body/bottom/detail/clip/clip-view"));
    REQUIRE(view != nullptr);
    REQUIRE(view->hasClip());
    view->setRowHeight(14);
    view->centreOnDegree(6);
    view->setZoom(static_cast<double>(kBeat) / static_cast<double>(kQ), 0.0);
    view->scrollToTick(-0.5 * static_cast<double>(kQ));
    GridSpec g;
    g.mode = GridMode::Fixed;
    g.fixedExp = 2;  // 1/4
    ctx.setGrid(g);
    d.tree().layoutNow();
  }
  ~Pen() { d.tree().setRoot(std::make_unique<Widget>()); }

  RollState& st() { return view->state(); }
  const doc::NoteClip& clip() const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), ref.track);
    REQUIRE(t != nullptr);
    const int32_t i = doc::findClipIndex(*t, ref.clip);
    REQUIRE(i >= 0);
    return *t->clips[static_cast<size_t>(i)].clip;
  }
  const doc::Note& note(doc::NoteId id) const {
    const doc::Note* n = clip().notes.findById(id);
    REQUIRE(n != nullptr);
    return *n;
  }
  size_t noteCount() const {
    size_t n = 0;
    for (const doc::Note& x : clip().notes) {
      (void)x;
      ++n;
    }
    return n;
  }
  // The note's pitch curve (empty: none).
  std::vector<doc::Breakpoint> curve(doc::NoteId id) const {
    const doc::NoteCurves* nc = doc::findNoteCurves(clip(), id);
    return nc ? nc->pitch : std::vector<doc::Breakpoint>{};
  }
  double curveAt(doc::NoteId id, Ticks t) const {
    const std::vector<doc::Breakpoint> c = curve(id);
    return c.empty() ? 0.0 : doc::automationValueAt(c, t);
  }
  size_t history() const { return ctx.store().history().cursor(); }
  Vec2 at(double tick, int64_t degree) const { return {view->xOfTick(tick), view->yOfDegree(degree)}; }
  void focusNotes() {
    const IRect a = d.rect("main/body/bottom/detail/clip/clip-view/notes");
    REQUIRE_FALSE(a.empty());
    d.click({static_cast<float>(a.right()) - 6.0f, static_cast<float>(a.y) + 6.0f});
    d.step();
    REQUIRE(d.tree().focus() == d.find("main/body/bottom/detail/clip/clip-view/notes"));
  }
  std::vector<doc::NoteId> addNotes(const std::vector<doc::Note>& notes) {
    auto e = doc::ops::addNotes(*ctx.doc(), ctx.store().ids(), ref, notes);
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    d.step();
    std::vector<doc::NoteId> ids;
    for (const doc::Note& n : notes)  // in the order given (the edit's keys are sorted)
      for (const doc::Note& have : clip().notes)
        if (have.start == n.start && have.pitch == n.pitch) ids.push_back(have.id);
    REQUIRE(ids.size() == notes.size());
    return ids;
  }
  static doc::Note plain(Ticks start, Ticks len, int32_t degree) {
    doc::Note n;
    n.start = start;
    n.length = len;
    n.pitch = PitchId::fromDegree(degree, 0);
    return n;
  }
  void drawMode(bool on) {
    if (ctx.drawMode() != on) (void)d.press("B");
    d.step();
    REQUIRE(ctx.drawMode() == on);
  }
  TextEdit* field() { return dynamic_cast<TextEdit*>(d.tree().focus()); }
  Menu* rightClick(Vec2 p) {
    d.click(p, mod::kNone, PointerButton::Right);
    d.step();
    return dynamic_cast<Menu*>(shell->overlay().top());
  }
  static int32_t itemOf(Menu* m, std::string_view label) {
    if (!m) return -1;
    for (size_t i = 0; i < m->items().size(); ++i)
      if (m->items()[i].label.rfind(label, 0) == 0) return static_cast<int32_t>(i);
    return -1;
  }
  void choose(Menu* m, std::string_view label) {
    REQUIRE(m != nullptr);
    const int32_t item = itemOf(m, label);
    REQUIRE_MESSAGE(item >= 0, "no menu item " << label);
    REQUIRE_MESSAGE(m->items()[static_cast<size_t>(item)].enabled, "menu item disabled: " << label);
    d.click(mid(m->itemRect(item)));
    d.step();
    d.tree().layoutNow();
  }
  void chooseSub(Menu* m, std::string_view parent, std::string_view label) {
    REQUIRE(m != nullptr);
    const int32_t item = itemOf(m, parent);
    REQUIRE_MESSAGE(item >= 0, "no submenu " << parent);
    d.click(mid(m->itemRect(item)));
    d.step();
    Menu* sub = m->submenu();
    REQUIRE(sub != nullptr);
    choose(sub, label);
  }
  // The handle of point `index` of a curve (mid: the diamond after it), where the roll shows it.
  Vec2 handle(const CurveTarget& t, int32_t index, bool diamond = false) {
    std::vector<RollState::CurveHandle> hs;
    st().curveHandles(t, hs);
    for (const RollState::CurveHandle& h : hs)
      if (h.index == index && h.mid == diamond) return h.pos;
    FAIL("the curve shows no such handle");
    return {};
  }
};

void checkGolden(const char* scene) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, views::test::fixtureDir("empty"), w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff diff;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &diff));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(diff));
}

}  // namespace

TEST_CASE("roll pen: Alt + a note's left edge glides it in from the previous note — a derived bend that follows its neighbour; a short drag removes it; the menu makes it straight") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 2 * kQ, 0), Pen::plain(2 * kQ, 2 * kQ, 4)});
  const doc::NoteId a = ids[0], b = ids[1];
  r.d.click(r.at(3.0 * kQ, 4));
  REQUIRE(r.view->selection().size() == 1);
  size_t h = r.history();

  // One beat inward from its left edge, Alt held. While the drag runs the neighbour is outlined.
  const Vec2 edge{r.view->xOfTick(2.0 * kQ) + 1.5f, r.view->yOfDegree(4)};
  REQUIRE(r.d.down(edge, PointerButton::Left, mod::kAlt));
  r.d.move({edge.x + 0.5f * kBeat, edge.y}, mod::kAlt);
  r.d.move({edge.x + kBeat, edge.y}, mod::kAlt);
  r.d.step();
  CHECK(r.view->bendHighlights() == std::vector<doc::NoteId>{a});
  CHECK(r.history() == h);  // a preview so far
  CHECK(r.view->notePoint(b, 0.0).y == doctest::Approx(r.view->yOfDegree(0)).epsilon(0.01));  // … of what the release stores
  r.d.up({edge.x + kBeat, edge.y}, PointerButton::Left, mod::kAlt);
  r.d.step();
  CHECK(r.view->bendHighlights().empty());
  CHECK(r.history() == h + 1);
  // What is stored is the neighbour's name, a length and a shape: no breakpoints.
  const doc::NoteCurves* rec = doc::findNoteCurves(r.clip(), b);
  REQUIRE(rec != nullptr);
  CHECK(rec->pitch.empty());
  CHECK(rec->bendIn == doc::NoteBend{kQ, doc::GlideShape{}, a});
  CHECK_FALSE(rec->bendOut.set());
  CHECK(r.st().bendLength(b, -1) == kQ);
  CHECK(r.note(b).start == 2 * kQ);  // the edge did not move
  CHECK(r.note(b).length == 2 * kQ);
  CHECK(r.view->selection().size() == 1);
  // It is drawn where it sounds: from the previous note's row, half way through the S at half the glide.
  CHECK(r.view->notePoint(b, 0.0).y == doctest::Approx(r.view->yOfDegree(0)).epsilon(0.01));
  CHECK(*r.view->noteCentsAt(b, 0.25) - *r.view->noteCentsAt(b, 1.0) == doctest::Approx(-200.0).epsilon(0.02));
  CHECK(*r.view->noteCentsAt(b, 0.5) == doctest::Approx(*r.view->noteCentsAt(b, 1.0)));

  // The neighbour moves a row up (click it, Up): the glide starts there now. Nothing was re-done.
  r.d.click(r.at(1.0 * kQ, 0));
  CHECK(r.d.press("Up") == KeyResult::Action);
  r.d.step();
  CHECK(*doc::findNoteCurves(r.clip(), b) == *rec);
  CHECK(r.view->notePoint(b, 0.0).y == doctest::Approx(r.view->yOfDegree(1)).epsilon(0.01));
  CHECK(*r.view->noteCentsAt(b, 0.0) - *r.view->noteCentsAt(b, 1.0) == doctest::Approx(-300.0).epsilon(1e-6));
  CHECK(r.d.press("Down") == KeyResult::Action);
  r.d.step();

  // The note's menu: the bend straight, one undo step.
  r.d.click(r.view->notePoint(b, 0.8));
  h = r.history();
  r.chooseSub(r.rightClick(r.view->notePoint(b, 0.6)), "Bend In", "Straight");
  CHECK(r.history() == h + 1);
  CHECK(doc::findNoteCurves(r.clip(), b)->bendIn.shape.kind == doc::GlideShape::LogLinear);
  CHECK(doc::findNoteCurves(r.clip(), b)->bendIn.other == a);
  CHECK(r.st().bendIsStraight(b, -1));
  CHECK(*r.view->noteCentsAt(b, 0.25) - *r.view->noteCentsAt(b, 1.0) == doctest::Approx(-200.0).epsilon(1e-3));
  CHECK(*r.view->noteCentsAt(b, 0.125) - *r.view->noteCentsAt(b, 1.0) == doctest::Approx(-300.0).epsilon(1e-3));

  // Alt + the edge again, back to the edge (under half a grid step): the bend is gone.
  h = r.history();
  const Vec2 start = r.view->notePoint(b, 0.004);
  r.d.drag(start, {start.x + 9.0f, start.y}, 4, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(doc::findNoteCurves(r.clip(), b) == nullptr);

  // Without a note before it there is nothing to glide from: no edit, the status says so.
  r.d.click(r.at(1.0 * kQ, 0));
  h = r.history();
  const Vec2 first{r.view->xOfTick(0.0) + 1.5f, r.view->yOfDegree(0)};
  r.d.drag(first, {first.x + kBeat, first.y}, 4, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h);
  CHECK(r.ctx.status().find("no note before") != std::string::npos);
  CHECK(r.note(ids[0]).length == 2 * kQ);  // and it was no resize
}

TEST_CASE("roll pen: Alt + a note's right edge glides it out to the next note — the nearest in pitch of the notes that start there (M9), never a note at its own pitch") {
  Pen r;
  r.focusNotes();
  // After the C: an E and an A on the beat, and a bass note that starts EXACTLY on the C's end while the E starts a
  // 32nd late. The melody's E is the neighbour, not the bass two octaves down (nearest in time).
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 2 * kQ, 0), Pen::plain(2 * kQ + kQ / 8, 2 * kQ, 4), Pen::plain(2 * kQ + kQ / 8, 2 * kQ, 9),
                                                   Pen::plain(2 * kQ, 2 * kQ, -24)});
  const doc::NoteId a = ids[0];
  r.d.click(r.at(1.0 * kQ, 0));
  const size_t h = r.history();
  const Vec2 edge{r.view->xOfTick(2.0 * kQ) - 2.0f, r.view->yOfDegree(0)};
  r.d.drag(edge, {edge.x - kBeat, edge.y}, 6, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 1);
  const doc::NoteCurves* rec = doc::findNoteCurves(r.clip(), a);
  REQUIRE(rec != nullptr);
  CHECK(rec->pitch.empty());
  CHECK(rec->bendOut == doc::NoteBend{kQ, doc::GlideShape{}, ids[1]});  // the E: not the A above it, not the bass
  CHECK(r.st().bendLength(a, 1) == kQ);
  CHECK(r.note(a).length == 2 * kQ);
  CHECK(*r.view->noteCentsAt(a, 1.0) - *r.view->noteCentsAt(a, 0.0) == doctest::Approx(400.0).epsilon(1e-6));
  CHECK(*r.view->noteCentsAt(a, 0.5) == doctest::Approx(*r.view->noteCentsAt(a, 0.0)));
  // Undo gives the plain note back.
  REQUIRE(r.ctx.store().undo());
  r.d.step();
  CHECK(doc::findNoteCurves(r.clip(), a) == nullptr);

  // A repeated note: the only note after it is at its own pitch — nothing to bend to, and the status says why.
  const std::vector<doc::NoteId> rep = r.addNotes({Pen::plain(8 * kQ, kQ, 2), Pen::plain(9 * kQ, kQ, 2)});
  r.d.click(r.at(8.5 * kQ, 2));
  const size_t h2 = r.history();
  const Vec2 e2{r.view->xOfTick(9.0 * kQ) - 2.0f, r.view->yOfDegree(2)};
  r.d.drag(e2, {e2.x - 0.5f * kBeat, e2.y}, 6, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h2);
  CHECK(r.ctx.status().find("same pitch") != std::string::npos);
  CHECK(doc::findNoteCurves(r.clip(), rep[0]) == nullptr);
}

TEST_CASE("roll pen: Alt + the edge of a note with no neighbour there bends from / to the pitch dragged to — snapped, Shift free, a plain vertical drag over one grid cell; written into its free curve") {
  Pen r;
  r.focusNotes();
  // A chord: no note before or after any of them (the bug report's 4:5:6).
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 4 * kQ, 0), Pen::plain(0, 4 * kQ, 4), Pen::plain(0, 4 * kQ, 7)});
  const doc::NoteId n = ids[1];
  r.d.click(r.view->notePoint(n, 0.5));
  REQUIRE(r.view->selection().size() == 1);
  const double own = *r.view->noteCentsAt(n, 0.5);

  // The left edge: a beat inward and two rows (and a bit) up: it slides in from two rows above, snapped to the row.
  size_t h = r.history();
  const Vec2 edge{r.view->xOfTick(0.0) + 1.5f, r.view->yOfDegree(4)};
  REQUIRE(r.d.down(edge, PointerButton::Left, mod::kAlt));
  r.d.move({edge.x + 0.5f * kBeat, edge.y - kRow}, mod::kAlt);
  r.d.move({edge.x + kBeat, edge.y - 2.0f * kRow - 3.0f}, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h);  // a preview so far
  CHECK(r.ctx.status() == "Bend In from +200 \xC2\xA2");
  CHECK(*r.view->noteCentsAt(n, 0.0) == doctest::Approx(own + 200.0).epsilon(1e-9));  // … of what the release stores
  r.d.up({edge.x + kBeat, edge.y - 2.0f * kRow - 3.0f}, PointerButton::Left, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 1);
  const doc::NoteCurves* rec = doc::findNoteCurves(r.clip(), n);
  REQUIRE(rec != nullptr);
  CHECK_FALSE(rec->bendIn.set());  // no neighbour to name: a free curve
  REQUIRE(rec->pitch.size() == 2);
  CHECK(rec->pitch[0].tick == 0);
  CHECK(rec->pitch[0].value == 200.0f);
  CHECK(rec->pitch[1].tick == kQ);
  CHECK(rec->pitch[1].value == 0.0f);
  CHECK(r.note(n).start == 0);  // the edge did not move
  CHECK(r.note(n).length == 4 * kQ);
  CHECK(*r.view->noteCentsAt(n, 0.125) == doctest::Approx(own + 100.0).epsilon(1e-9));
  CHECK(*r.view->noteCentsAt(n, 0.5) == doctest::Approx(own).epsilon(1e-9));

  // The right edge, only down (Shift once the drag runs: free cents, 5 px = 36 ¢; Shift on the press would toggle
  // the selection): it slides out over one grid cell (a beat here).
  h = r.history();
  const Vec2 end{r.view->xOfTick(4.0 * kQ) - 2.0f, r.view->yOfDegree(4)};
  REQUIRE(r.d.down(end, PointerButton::Left, mod::kAlt));
  r.d.move({end.x, end.y + 3.0f}, mod::kAlt | mod::kShift);
  r.d.move({end.x, end.y + 5.0f}, mod::kAlt | mod::kShift);
  r.d.up({end.x, end.y + 5.0f}, PointerButton::Left, mod::kAlt | mod::kShift);
  r.d.step();
  CHECK(r.history() == h + 1);
  rec = doc::findNoteCurves(r.clip(), n);
  REQUIRE(rec != nullptr);
  CHECK(rec->pitch.front().value == 200.0f);  // the bend in stays
  CHECK(rec->pitch.back().tick == 4 * kQ);
  CHECK(rec->pitch.back().value == -36.0f);
  CHECK(*r.view->noteCentsAt(n, 0.75) == doctest::Approx(own).epsilon(1e-9));  // the slide starts a beat before the end
  CHECK(*r.view->noteCentsAt(n, 1.0) == doctest::Approx(own - 36.0).epsilon(1e-6));
  CHECK(r.note(n).length == 4 * kQ);

  // Alt + an edge dragged only sideways: still nothing to glide with, and the status says what to do.
  h = r.history();
  const Vec2 low{r.view->xOfTick(0.0) + 1.5f, r.view->yOfDegree(0)};
  r.d.click(r.view->notePoint(ids[0], 0.5));
  r.d.drag(low, {low.x + kBeat, low.y}, 4, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h);
  CHECK(r.ctx.status().find("drag up or down") != std::string::npos);
}

TEST_CASE("roll pen: a slow stroke is smoothed — the one-pixel steps of the pointer become a slope, the ends stay where they were drawn") {
  Pen r;
  r.focusNotes();
  const doc::NoteId n = r.addNotes({Pen::plain(0, 4 * kQ, 0)})[0];
  r.drawMode(true);
  // 125 px to the right, one pixel up every 10 px (then level): a staircase of twelve 7 ¢ steps.
  const Vec2 p0 = r.at(0.5 * kQ, 0);
  REQUIRE(r.d.down(p0));
  for (int i = 1; i <= 125; ++i) r.d.move({p0.x + static_cast<float>(i), p0.y - static_cast<float>(std::min(i / 10, 12))});
  r.d.up({p0.x + 125.0f, p0.y - 12.0f});
  r.d.step();
  const double step = 100.0 / kRow;  // one pixel in cents
  const Ticks px = kQ / static_cast<Ticks>(kBeat);
  const Ticks t0 = kQ / 2;
  CHECK(r.curveAt(n, t0) == doctest::Approx(0.0).epsilon(1e-6));                      // where it was pressed
  CHECK(r.curveAt(n, t0 + 125 * px) == doctest::Approx(12.0 * step).epsilon(1e-4));  // where it was let go
  // Nowhere a pixel's step at once: over any 1 px the curve rises well under a step, and it never falls.
  double prev = r.curveAt(n, t0);
  double steepest = 0.0;
  for (int i = 1; i <= 125; ++i) {
    const double v = r.curveAt(n, t0 + i * px);
    CHECK(v >= prev - 0.6);  // (thinning keeps it within 0.5 ¢)
    steepest = std::max(steepest, v - prev);
    prev = v;
  }
  MESSAGE("steepest rise over 1 px: " << steepest << " cents (a raw step: " << step << ")");
  CHECK(steepest < 0.5 * step);
}

TEST_CASE("roll pen: Ctrl+G glides each selected note into the next selected one over a grid step; Clear Bends removes them") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 2 * kQ, 0), Pen::plain(2 * kQ, 2 * kQ, 4), Pen::plain(4 * kQ, 2 * kQ, 2)});
  // One note selected: the chord does not apply.
  r.d.click(r.at(1.0 * kQ, 0));
  size_t h = r.history();
  CHECK(r.d.press("Ctrl+G") != KeyResult::Action);
  CHECK(r.history() == h);

  CHECK(r.d.press("Ctrl+A") == KeyResult::Action);
  REQUIRE(r.view->selection().size() == 3);
  CHECK(r.d.press("Ctrl+G") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(doc::findNoteCurves(r.clip(), ids[0])->bendOut == doc::NoteBend{kQ, doc::GlideShape{}, ids[1]});
  CHECK(doc::findNoteCurves(r.clip(), ids[1])->bendOut == doc::NoteBend{kQ, doc::GlideShape{}, ids[2]});
  CHECK(doc::findNoteCurves(r.clip(), ids[2]) == nullptr);  // the last has nothing to glide into
  CHECK(*r.view->noteCentsAt(ids[0], 1.0) - *r.view->noteCentsAt(ids[0], 0.0) == doctest::Approx(400.0));
  CHECK(*r.view->noteCentsAt(ids[1], 1.0) - *r.view->noteCentsAt(ids[1], 0.0) == doctest::Approx(-200.0));
  CHECK(r.view->selection().size() == 3);
  // A second Ctrl+G changes nothing (no undo step).
  h = r.history();
  (void)r.d.press("Ctrl+G");
  CHECK(r.history() == h);

  // The menu of the selection.
  r.choose(r.rightClick(r.view->notePoint(ids[0], 0.2)), "Clear Curves");
  CHECK(r.history() == h + 1);
  CHECK(r.clip().curves == nullptr);
}

TEST_CASE("roll pen: Draw Mode — a stroke on a note is its pitch curve (thinned, bounded), never a bend (H4); a click, also one that wanders a few px (M7), deletes the note") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 4 * kQ, 0), Pen::plain(4 * kQ, 4 * kQ, 0), Pen::plain(8 * kQ, 2 * kQ, 3)});
  r.drawMode(true);
  size_t h = r.history();

  // From half a beat in, two rows up over one beat.
  const Vec2 from = r.at(0.5 * kQ, 0);
  r.d.drag(from, {from.x + kBeat, from.y - 2.0f * kRow}, 20);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.noteCount() == 3);  // the drag drew on the note, it did not delete it
  std::vector<doc::Breakpoint> c = r.curve(ids[0]);
  REQUIRE(c.size() >= 2);
  CHECK(c.size() <= 6);  // a straight stroke thins to its ends
  CHECK(c.front().tick == kQ / 2);
  CHECK(c.front().value == 0.0f);  // it starts on the note's pitch exactly
  CHECK(r.curveAt(ids[0], 3 * kQ / 2) == doctest::Approx(200.0).epsilon(0.02));
  CHECK(r.curveAt(ids[0], kQ) == doctest::Approx(100.0).epsilon(0.05));
  CHECK(r.curveAt(ids[0], 3 * kQ) == doctest::Approx(200.0).epsilon(0.02));  // held to the note's end
  CHECK(r.curveAt(ids[0], 0) == 0.0);
  // H4: the stroke starts on 0 and ends away from it — the shape the old roll took for a "Bend Out". It is a drawn
  // curve: the record has no bend, the menu offers none, and a resize leaves every point where it is.
  CHECK_FALSE(doc::findNoteCurves(r.clip(), ids[0])->bendOut.set());
  r.drawMode(false);
  r.d.click(r.view->notePoint(ids[0], 0.05));
  REQUIRE(r.view->selection().size() == 1);
  Menu* menu = r.rightClick(r.view->notePoint(ids[0], 0.05));
  CHECK(Pen::itemOf(menu, "Bend Out") < 0);
  CHECK(Pen::itemOf(menu, "Bend In") < 0);
  (void)r.d.press("Escape");
  r.d.step();
  const std::vector<doc::Breakpoint> before = r.curve(ids[0]);
  const Vec2 end = r.view->notePoint(ids[0], 0.998);
  h = r.history();
  r.d.drag(end, {end.x - kBeat, end.y}, 6);  // a beat shorter: the note still reaches every point
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.note(ids[0]).length == 3 * kQ);
  CHECK(r.curve(ids[0]) == before);
  r.drawMode(true);

  // A long wiggly stroke: the stored curve is bounded.
  h = r.history();
  const Vec2 p0 = r.at(4.2 * kQ, 0);
  REQUIRE(r.d.down(p0));
  for (int i = 1; i <= 700; ++i) {
    const float x = p0.x + static_cast<float>(i) * 0.3f;
    r.d.move({x, p0.y + 20.0f * std::sin(static_cast<float>(i) * 0.9f)});
  }
  r.d.up({p0.x + 210.0f, p0.y});
  r.d.step();
  CHECK(r.history() == h + 1);
  c = r.curve(ids[1]);
  CHECK(c.size() > 8);
  CHECK(c.size() <= RollState::kMaxPenPoints + 4);
  for (size_t i = 1; i < c.size(); ++i) CHECK(c[i].tick >= c[i - 1].tick);

  // M7: a click whose hand slips 5 px is still a click: the note is deleted, no stray curve is stored.
  h = r.history();
  const Vec2 slip = r.at(9.0 * kQ, 3);
  r.d.drag(slip, {slip.x + 5.0f, slip.y + 4.0f}, 3);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.noteCount() == 2);
  CHECK(r.clip().notes.findById(ids[2]) == nullptr);
  // A click (no drag) on a note deletes it, as Draw Mode always did.
  h = r.history();
  r.d.click(r.view->notePoint(ids[0], 0.05));
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.noteCount() == 1);
}

TEST_CASE("roll pen: breakpoints — Ctrl + click adds one, drag snaps to rows (Shift: free) and the grid, the diamond bends, double-click deletes") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 4 * kQ, 0)});
  const doc::NoteId n = ids[0];
  const CurveTarget t{n};
  r.drawMode(true);
  size_t h = r.history();

  // Ctrl + press one beat in, dragged a row up: the note bends up to there and back.
  const Vec2 p = r.at(1.0 * kQ, 0);
  r.d.drag(p, {p.x, p.y - kRow}, 4, mod::kCtrl);
  r.d.step();
  CHECK(r.history() == h + 1);
  std::vector<doc::Breakpoint> c = r.curve(n);
  REQUIRE(c.size() == 3);
  CHECK(c[0].tick == 0);
  CHECK(c[0].value == 0.0f);
  CHECK(c[1].tick == kQ);
  CHECK(c[1].value == doctest::Approx(100.0));
  CHECK(c[2].tick == 4 * kQ);
  CHECK(c[2].value == 0.0f);
  REQUIRE(r.noteCount() == 1);

  // Pointer mode: the selected note shows its breakpoints.
  r.drawMode(false);
  r.d.click(r.view->notePoint(n, 0.7));
  REQUIRE(r.view->selection().size() == 1);
  // Up another row and a third of a row: it snaps to the row.
  h = r.history();
  Vec2 hp = r.handle(t, 1);
  r.d.drag(hp, {hp.x, hp.y - kRow - 4.0f}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.curve(n)[1].value == doctest::Approx(200.0));
  CHECK(r.curve(n)[1].tick == kQ);
  // Shift: free.
  h = r.history();
  hp = r.handle(t, 1);
  r.d.drag(hp, {hp.x, hp.y - 5.0f}, 5, mod::kShift);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.curve(n)[1].value == doctest::Approx(200.0 + 500.0 / 14.0).epsilon(0.01));
  // In time: a beat later (the grid), between its neighbours.
  h = r.history();
  hp = r.handle(t, 1);
  r.d.drag(hp, {hp.x + kBeat + 5.0f, hp.y}, 5, mod::kShift);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.curve(n)[1].tick == 2 * kQ);
  // The diamond between the first two points bends the segment.
  h = r.history();
  const Vec2 dia = r.handle(t, 0, true);
  r.d.drag(dia, {dia.x, dia.y + 24.0f}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.curve(n)[0].curve > 0.2f);
  CHECK(r.curve(n)[1].tick == 2 * kQ);  // the points stay
  // A double-click deletes the point: nothing is left to bend, the note is plain again.
  h = r.history();
  r.d.doubleClick(r.handle(t, 1));
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.curve(n).empty());
  CHECK(r.noteCount() == 1);
}

TEST_CASE("roll pen: Alt paints vibrato in Draw Mode — the note's vibrato property over the dragged span (depth from the stroke's height, 6 Hz), nothing baked, no menu entry") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 8 * kQ, 0), Pen::plain(0, 8 * kQ, 7)});
  const doc::NoteId n = ids[0];
  r.drawMode(true);
  size_t h = r.history();
  const double rate = RollState::kDefaultVibratoHz;
  CHECK(rate == 6.0);

  // From beat 1 to beat 5, half a row high.
  const Vec2 from = r.at(1.0 * kQ, 0);
  r.d.drag(from, {from.x + 4.0f * kBeat, from.y - 0.5f * kRow}, 8, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.noteCount() == 2);
  const doc::NoteCurves* rec = doc::findNoteCurves(r.clip(), n);
  REQUIRE(rec != nullptr);
  CHECK(rec->pitch.empty());  // a property, not breakpoints
  CHECK(rec->vibrato.depthCents == doctest::Approx(50.0).epsilon(0.03));
  CHECK(rec->vibrato.rate == doctest::Approx(rate).epsilon(0.001));
  CHECK(rec->vibrato.unit == doc::NoteVibrato::RateUnit::Hz);
  CHECK(rec->vibrato.onset == kQ);
  CHECK(rec->vibrato.end == 5 * kQ);
  CHECK(r.ctx.status().find("Vibrato 50") != std::string::npos);
  // The wave is drawn on the note: silent before the onset, swinging 50 ¢ inside it.
  const double base = *r.view->noteCentsAt(n, 0.0);
  CHECK(*r.view->noteCentsAt(n, 0.1) == base);
  double peak = 0.0;
  for (int i = 0; i <= 400; ++i) peak = std::max(peak, std::fabs(*r.view->noteCentsAt(n, 0.125 + 0.5 * i / 400.0) - base));
  CHECK(peak == doctest::Approx(50.0).epsilon(0.03));
  CHECK(*r.view->noteCentsAt(n, 0.8) == base);

  // No typed values anywhere: the note's menu has no vibrato entry.
  r.drawMode(false);
  Menu* menu = r.rightClick(r.view->notePoint(n, 0.05));
  REQUIRE(menu != nullptr);
  for (const MenuItem& item : menu->items()) CHECK(item.label.find("Vibrato") == std::string::npos);
}

TEST_CASE("roll pen: Shift + a note's body detunes it freely past a dead zone, with the amount in the status line (H5); Shift + click, also with a slip, only selects") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 2 * kQ, 0), Pen::plain(2 * kQ, 2 * kQ, 4)});
  r.d.click(r.at(1.0 * kQ, 0));
  REQUIRE(r.view->selection().size() == 1);
  size_t h = r.history();
  const Vec2 p = r.at(1.0 * kQ, 0);
  // 20 px up: 6 px of dead zone, then 14 px = one row = 100 ¢. The status line says so while dragging.
  REQUIRE(r.d.down(p, PointerButton::Left, mod::kShift));
  r.d.move({p.x, p.y - 10.0f}, mod::kShift);
  r.d.move({p.x, p.y - 20.0f}, mod::kShift);
  CHECK(r.ctx.status() == "Detune +100 \xC2\xA2");
  r.d.up({p.x, p.y - 20.0f}, PointerButton::Left, mod::kShift);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.note(ids[0]).pitch.kind() == PitchKind::Cents);
  CHECK(r.note(ids[0]).pitch.cents() == doctest::Approx(100.0));
  CHECK(r.note(ids[0]).start == 0);
  CHECK(r.view->selection().size() == 1);
  CHECK(r.ctx.status().find("Detuned +100") != std::string::npos);

  // Shift + click: add the other note, then take it off again; no edit.
  h = r.history();
  r.d.click(r.at(3.0 * kQ, 4), mod::kShift);
  CHECK(r.view->selection().size() == 2);
  r.d.click(r.at(3.0 * kQ, 4), mod::kShift);
  CHECK(r.view->selection().size() == 1);
  CHECK(r.history() == h);
  CHECK(r.note(ids[1]).pitch.kind() == PitchKind::Degree);
  // H5: Shift + click to add a note with the hand slipping 5 px (past the click slop, inside the dead zone): the
  // note joins the selection and nothing is detuned.
  const Vec2 q = r.at(3.0 * kQ, 4);
  r.d.drag(q, {q.x + 1.0f, q.y + 5.0f}, 3, mod::kShift);
  r.d.step();
  CHECK(r.view->selection().size() == 2);
  CHECK(r.history() == h);
  CHECK(r.note(ids[1]).pitch == PitchId::fromDegree(4, 0));
  CHECK(r.note(ids[0]).pitch.cents() == doctest::Approx(100.0));
  // A Shift drag along the time moves the notes on the grid and detunes nothing.
  r.d.drag(q, {q.x + kBeat, q.y + 4.0f}, 5, mod::kShift);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.note(ids[1]).start == 3 * kQ);
  CHECK(r.note(ids[1]).pitch == PitchId::fromDegree(4, 0));
}

TEST_CASE("roll pen: a resize leaves a bend on its edge (a property) and moves no drawn curve; move and duplicate carry the record") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 2 * kQ, 0), Pen::plain(4 * kQ, 2 * kQ, 4)});
  const doc::NoteId a = ids[0];
  CHECK(r.d.press("Ctrl+A") == KeyResult::Action);
  CHECK(r.d.press("Ctrl+G") == KeyResult::Action);
  r.d.step();
  REQUIRE(r.st().bendLength(a, 1) == kQ);
  const doc::NoteCurves rec = *doc::findNoteCurves(r.clip(), a);
  r.d.click(r.view->notePoint(a, 0.2));
  REQUIRE(r.view->selection().size() == 1);

  // Its right edge a beat later (no Alt: a resize). The glide still ends on the note's end: nothing was rewritten.
  size_t h = r.history();
  const Vec2 end = r.view->notePoint(a, 0.996);
  r.d.drag(end, {end.x + kBeat, end.y}, 6);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.note(a).length == 3 * kQ);
  CHECK(*doc::findNoteCurves(r.clip(), a) == rec);
  CHECK(r.st().bendLength(a, 1) == kQ);
  CHECK(*r.view->noteCentsAt(a, 1.0) - *r.view->noteCentsAt(a, 0.0) == doctest::Approx(400.0));
  CHECK(*r.view->noteCentsAt(a, 0.66) == doctest::Approx(*r.view->noteCentsAt(a, 0.0)));  // steady until the last beat
  // Undo: the note as before, in one step.
  REQUIRE(r.ctx.store().undo());
  r.d.step();
  CHECK(r.note(a).length == 2 * kQ);

  // Shift+Right lengthens by a grid cell: the same.
  r.d.click(r.view->notePoint(a, 0.2));
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.view->selection()[0] == a.value);  // a click on the selected note keeps it selected
  CHECK(r.d.press("Shift+Right") == KeyResult::Action);
  r.d.step();
  CHECK(r.note(a).length == 3 * kQ);
  CHECK(*doc::findNoteCurves(r.clip(), a) == rec);

  // Moved a beat later, the bend goes with it; Ctrl+D copies it (the copy bends to the same neighbour).
  const Vec2 body = r.view->notePoint(a, 0.3);
  r.d.drag(body, {body.x + kBeat, body.y}, 6);
  r.d.step();
  CHECK(r.note(a).start == kQ);
  CHECK(*doc::findNoteCurves(r.clip(), a) == rec);
  CHECK(r.d.press("Ctrl+D") == KeyResult::Action);
  r.d.step();
  REQUIRE(r.noteCount() == 3);
  size_t bent = 0;
  for (const doc::Note& n : r.clip().notes)
    if (const doc::NoteCurves* nc = doc::findNoteCurves(r.clip(), n.id); nc && nc->bendOut.other == ids[1]) ++bent;
  CHECK(bent == 2);
}

TEST_CASE("roll pen: curve arithmetic — a resized note's drawn curve keeps its place in time and is cut where the note no longer reaches") {
  using B = doc::Breakpoint;
  const std::vector<B> c = {{0, -200.0f, 0.0f, {}}, {100, 0.0f, 0.0f, {}}, {200, 50.0f, 0.0f, {}}, {300, 0.0f, 0.0f, {}}, {400, 300.0f, 0.0f, {}}};
  // The end 100 later: nothing moves.
  std::vector<B> r = RollState::retimeCurve(c, 500, 0);
  CHECK(r == c);
  // The start 50 later: every point 50 ticks earlier in the note (the same place in time), cut at the new start.
  r = RollState::retimeCurve(c, 350, 50);
  REQUIRE(r.size() == 5);
  CHECK(r[0].tick == 0);
  CHECK(r[0].value == doctest::Approx(-100.0));
  CHECK(r[1].tick == 50);
  CHECK(r[2].tick == 150);
  CHECK(r[3].tick == 250);
  CHECK(r[4].tick == 350);
  CHECK(r[4].value == 300.0f);
  // Shorter: cut at the new end, where the curve was.
  r = RollState::retimeCurve(c, 150, 0);
  REQUIRE(r.size() == 3);
  CHECK(r.back().tick == 150);
  CHECK(r.back().value == doctest::Approx(25.0));
  const std::vector<B> plain = {{0, 0.0f, 0.0f, {}}, {200, 100.0f, 0.0f, {}}, {400, 0.0f, 0.0f, {}}};
  r = RollState::retimeCurve(plain, 300, 0);
  REQUIRE(r.size() == 3);
  CHECK(r[2].tick == 300);
  CHECK(r[2].value == doctest::Approx(50.0));
  CHECK(RollState::vibratoText(30.0, 6.0) == "Vibrato 30 \xC2\xA2 \xC2\xB7 6 Hz");
  CHECK(RollState::vibratoText(35.0, 5.5) == "Vibrato 35 \xC2\xA2 \xC2\xB7 5.5 Hz");
}

TEST_CASE("render: pianoroll-note-lines (T7.12: repeated 16ths, a selected run, soft / deactivated / Cents / Ratio notes as lines) matches the golden image") { checkGolden("pianoroll-note-lines"); }
TEST_CASE("render: pianoroll-bends (a melody gliding between its notes, a straight scoop with its breakpoints) matches the golden image") { checkGolden("pianoroll-bends"); }

