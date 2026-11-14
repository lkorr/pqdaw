// T7.6 piano roll: bends between notes, pen modes, breakpoints, vibrato, ride curves. Every gesture goes through the
// real shell, pointer and key dispatch (TestDriver → UiTree → the note area / the keymap / the menus / the inline
// field), one undo step each:
//   Alt + a note's left / right edge (bend in / out, the nearest neighbour in pitch, removing it, its shape by menu);
//   Ctrl+G (the selection glides); Draw Mode: a stroke on a note (bounded points; a click still deletes the note),
//   Ctrl + click (a breakpoint), Alt + drag (vibrato; "Edit Vibrato…", "Add Vibrato…"); breakpoints (drag with row
//   snapping, Shift free, in time, the diamond's curvature, double-click deletes); the ride curve of a JI object on
//   its lowest voice (stroke, breakpoint, vibrato; a click there still draws a note); Shift + body (free detune,
//   Shift + click deselects); a resize keeps a bend on its edge; duplicate carries the curve; Clear Bends.
// Goldens: pianoroll-bends, pianoroll-vibrato, pianoroll-ride.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "pq/document/automation_curve.h"
#include "pq/document/commands.h"
#include "pq/document/ji_object.h"
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
  const doc::JiObject& object(doc::JiObjectId id) const {
    const doc::JiObject* o = doc::findJiObject(clip(), id);
    REQUIRE(o != nullptr);
    return *o;
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
  // 4:5:6:7 over [start, start + len), its lowest voice on the row `degree`.
  doc::JiObjectId makeObject(Ticks start, Ticks len, int32_t degree) {
    const int64_t terms[] = {4, 5, 6, 7};
    auto e = doc::ops::createJiObject(*ctx.doc(), ctx.store().ids(), ref, start, len, PitchId::fromDegree(degree, 0), terms);
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    const doc::JiObjectId id = e->jiObjects.at(0);
    std::vector<doc::JiStop> stops = object(id).stops;
    stops[0].scale = tuning::Rational(1, 4);
    auto s = doc::ops::setJiStops(*ctx.doc(), ctx.store().ids(), ref, id, stops);
    REQUIRE(s.ok());
    REQUIRE(ctx.store().dispatch(s->command).ok());
    d.tree().layoutNow();
    d.step();
    return id;
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

TEST_CASE("roll pen: Alt + a note's left edge glides it in from the previous note; a short drag removes the bend; the menu makes it straight") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 2 * kQ, 0), Pen::plain(2 * kQ, 2 * kQ, 4)});
  const doc::NoteId b = ids[1];
  r.d.click(r.at(3.0 * kQ, 4));
  REQUIRE(r.view->selection().size() == 1);
  size_t h = r.history();

  // One beat inward from its left edge, Alt held.
  const Vec2 edge{r.view->xOfTick(2.0 * kQ) + 1.5f, r.view->yOfDegree(4)};
  r.d.drag(edge, {edge.x + kBeat, edge.y}, 6, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 1);
  std::vector<doc::Breakpoint> c = r.curve(b);
  REQUIRE(c.size() > 2);  // an S-curve
  CHECK(c.front().tick == 0);
  CHECK(c.front().value == doctest::Approx(-400.0));  // from the note before it, four steps below
  CHECK(c.back().tick == kQ);
  CHECK(c.back().value == 0.0f);
  CHECK(r.st().bendLength(b, -1) == kQ);
  CHECK(r.curveAt(b, kQ / 2) == doctest::Approx(-200.0).epsilon(0.02));  // half way through the S
  CHECK(r.note(b).start == 2 * kQ);  // the edge did not move
  CHECK(r.note(b).length == 2 * kQ);
  CHECK(r.view->selection().size() == 1);
  // It is drawn where it sounds: it starts on the previous note's row.
  CHECK(r.view->notePoint(b, 0.0).y == doctest::Approx(r.view->yOfDegree(0)).epsilon(0.01));

  // The note's menu: the bend straight (two points), one undo step.
  h = r.history();
  r.chooseSub(r.rightClick(r.view->notePoint(b, 0.6)), "Bend In", "Straight");
  CHECK(r.history() == h + 1);
  c = r.curve(b);
  REQUIRE(c.size() == 2);
  CHECK(c[0].value == doctest::Approx(-400.0));
  CHECK(c[1].tick == kQ);
  CHECK(r.st().bendIsStraight(b, -1));

  // Alt + the edge again, back to the edge (under half a grid step): the bend is gone.
  h = r.history();
  const Vec2 start = r.view->notePoint(b, 0.004);
  r.d.drag(start, {start.x + 9.0f, start.y}, 4, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.curve(b).empty());

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

TEST_CASE("roll pen: Alt + a note's right edge glides it out to the next note, the nearest in pitch when several start there") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 2 * kQ, 0), Pen::plain(2 * kQ, 2 * kQ, 4), Pen::plain(2 * kQ, 2 * kQ, 9)});
  const doc::NoteId a = ids[0];
  r.d.click(r.at(1.0 * kQ, 0));
  const size_t h = r.history();
  const Vec2 edge{r.view->xOfTick(2.0 * kQ) - 2.0f, r.view->yOfDegree(0)};
  r.d.drag(edge, {edge.x - kBeat, edge.y}, 6, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 1);
  const std::vector<doc::Breakpoint> c = r.curve(a);
  REQUIRE(c.size() > 2);
  CHECK(c.front().tick == kQ);
  CHECK(c.front().value == 0.0f);
  CHECK(c.back().tick == 2 * kQ);
  CHECK(c.back().value == doctest::Approx(400.0));  // the E, not the A above it
  CHECK(r.st().bendLength(a, 1) == kQ);
  CHECK(r.note(a).length == 2 * kQ);
  // Undo gives the plain note back.
  REQUIRE(r.ctx.store().undo());
  r.d.step();
  CHECK(r.curve(a).empty());
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
  CHECK(r.st().bendLength(ids[0], 1) == kQ);
  CHECK(r.curve(ids[0]).back().value == doctest::Approx(400.0));
  CHECK(r.st().bendLength(ids[1], 1) == kQ);
  CHECK(r.curve(ids[1]).back().value == doctest::Approx(-200.0));
  CHECK(r.curve(ids[2]).empty());  // the last has nothing to glide into
  CHECK(r.view->selection().size() == 3);

  // The menu of the selection.
  h = r.history();
  r.choose(r.rightClick(r.view->notePoint(ids[0], 0.2)), "Clear Bends");
  CHECK(r.history() == h + 1);
  for (const doc::NoteId id : ids) CHECK(r.curve(id).empty());
}

TEST_CASE("roll pen: Draw Mode — a stroke on a note is its pitch curve (thinned, bounded); a click still deletes the note") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 4 * kQ, 0), Pen::plain(4 * kQ, 4 * kQ, 0)});
  r.drawMode(true);
  size_t h = r.history();

  // From half a beat in, two rows up over one beat.
  const Vec2 from = r.at(0.5 * kQ, 0);
  r.d.drag(from, {from.x + kBeat, from.y - 2.0f * kRow}, 20);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.noteCount() == 2);  // the drag drew on the note, it did not delete it
  std::vector<doc::Breakpoint> c = r.curve(ids[0]);
  REQUIRE(c.size() >= 2);
  CHECK(c.size() <= 6);  // a straight stroke thins to its ends
  CHECK(c.front().tick == kQ / 2);
  CHECK(c.front().value == 0.0f);  // it starts on the note's pitch exactly
  CHECK(r.curveAt(ids[0], 3 * kQ / 2) == doctest::Approx(200.0).epsilon(0.02));
  CHECK(r.curveAt(ids[0], kQ) == doctest::Approx(100.0).epsilon(0.05));
  CHECK(r.curveAt(ids[0], 3 * kQ) == doctest::Approx(200.0).epsilon(0.02));  // held to the note's end
  CHECK(r.curveAt(ids[0], 0) == 0.0);

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
  const CurveTarget t{n, {}};
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

TEST_CASE("roll pen: Alt paints vibrato in Draw Mode (depth from the stroke's height, rate from the grid); Edit / Add Vibrato by menu") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 8 * kQ, 0), Pen::plain(0, 8 * kQ, 7)});
  const doc::NoteId n = ids[0];
  const CurveTarget t{n, {}};
  r.drawMode(true);
  size_t h = r.history();
  const double rate = r.st().defaultVibratoRate(kQ);
  CHECK(rate >= 4.2);
  CHECK(rate < 8.5);

  // From beat 1 to beat 5, half a row high.
  const Vec2 from = r.at(1.0 * kQ, 0);
  r.d.drag(from, {from.x + 4.0f * kBeat, from.y - 0.5f * kRow}, 8, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.noteCount() == 2);
  std::vector<doc::Breakpoint> c = r.curve(n);
  REQUIRE(c.size() > 16);
  CHECK(c.size() <= RollState::kMaxVibratoPoints);
  CHECK(c.front().tick == kQ);
  CHECK(c.front().value == 0.0f);
  CHECK(c.back().value == 0.0f);
  CHECK(c.back().tick <= 5 * kQ);
  std::optional<RollState::Vibrato> v = r.st().vibratoAt(t, 3 * kQ);
  REQUIRE(v.has_value());
  CHECK(v->depth == doctest::Approx(50.0).epsilon(0.03));
  CHECK(v->rateHz == doctest::Approx(rate).epsilon(0.02));
  CHECK(v->from == kQ);
  CHECK(r.ctx.status().find("Vibrato 50") != std::string::npos);
  float peak = 0.0f;
  for (const doc::Breakpoint& b : c) peak = std::max(peak, std::fabs(b.value));
  CHECK(peak == doctest::Approx(50.0).epsilon(0.03));

  // Its menu: Edit Vibrato… opens the field with what is there; another depth and rate, one undo step.
  r.drawMode(false);
  h = r.history();
  r.choose(r.rightClick(r.view->notePoint(n, 0.375)), "Edit Vibrato");
  REQUIRE(r.field() != nullptr);
  CHECK(r.field()->text().find("50") == 0);
  REQUIRE(r.d.type("20 5"));
  (void)r.d.press("Enter");
  r.d.step();
  CHECK(r.history() == h + 1);
  v = r.st().vibratoAt(t, 3 * kQ);
  REQUIRE(v.has_value());
  CHECK(v->depth == doctest::Approx(20.0).epsilon(0.01));
  CHECK(v->rateHz == doctest::Approx(5.0).epsilon(0.02));
  CHECK(r.field() == nullptr);

  // A text that is no vibrato keeps the field open and changes nothing.
  h = r.history();
  r.choose(r.rightClick(r.view->notePoint(n, 0.375)), "Edit Vibrato");
  REQUIRE(r.field() != nullptr);
  REQUIRE(r.d.type("wide"));
  (void)r.d.press("Enter");
  r.d.step();
  CHECK(r.history() == h);
  REQUIRE(r.field() != nullptr);
  (void)r.d.press("Escape");
  r.d.step();

  // Add Vibrato… on the other note: the whole note, with the depth last used.
  h = r.history();
  r.choose(r.rightClick(r.at(2.0 * kQ, 7)), "Add Vibrato");
  REQUIRE(r.field() != nullptr);
  CHECK(r.field()->text().find("20") == 0);
  (void)r.d.press("Enter");
  r.d.step();
  CHECK(r.history() == h + 1);
  const std::optional<RollState::Vibrato> w = r.st().vibratoAt(CurveTarget{ids[1], {}}, 4 * kQ);
  REQUIRE(w.has_value());
  CHECK(w->depth == doctest::Approx(20.0).epsilon(0.01));
  CHECK(w->from == 0);
  CHECK(w->to > 7 * kQ);
}

TEST_CASE("roll pen: the ride curve of a JI object is drawn on its lowest voice — stroke, breakpoint, vibrato; every voice follows") {
  Pen r;
  r.focusNotes();
  const doc::JiObjectId id = r.makeObject(2 * kQ, 4 * kQ, 0);
  const CurveTarget t{{}, id};
  const tuning::Tuning tun = [&] {
    const std::optional<doc::TrackPath> path = doc::findTrack(*r.ctx.doc(), r.ref.track);
    REQUIRE(path.has_value());
    auto built = doc::buildTuning(*doc::effectiveTuningRef(*r.ctx.doc(), *path, &r.clip()).def);
    REQUIRE(built.ok());
    return *built;
  }();
  const double third = *doc::jiVoiceCents(r.object(id), tun, 2, 3.5 * kQ);
  r.drawMode(true);
  size_t h = r.history();

  // A stroke along the lowest voice from one beat in, a row up over a beat.
  const Vec2 from = r.view->jiVoicePoint(id, 0, kQ);
  r.d.drag(from, {from.x + kBeat, from.y - kRow}, 12);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.noteCount() == 0);  // the stroke drew no notes
  std::vector<doc::Breakpoint> ride = r.object(id).ride;
  REQUIRE(ride.size() >= 2);
  CHECK(ride.front().tick == kQ);
  CHECK(ride.front().value == 0.0f);
  CHECK(doc::automationValueAt(ride, 2 * kQ) == doctest::Approx(100.0).epsilon(0.03));
  // Every voice rides it.
  CHECK(*doc::jiVoiceCents(r.object(id), tun, 2, 3.5 * kQ) == doctest::Approx(third + 100.0).epsilon(0.001));
  CHECK(r.view->jiSelection() == id);

  // "Clear Ride Curve" in the object's menu.
  h = r.history();
  r.drawMode(false);
  const IRect box = r.view->jiObjectRect(id);
  r.choose(r.rightClick({static_cast<float>(box.x) + 30.0f, static_cast<float>(box.y) - 8.0f}), "Clear Ride Curve");
  CHECK(r.history() == h + 1);
  CHECK(r.object(id).ride.empty());

  // Ctrl + press on the lowest voice adds a breakpoint; dragged up a row.
  r.drawMode(true);
  h = r.history();
  const Vec2 p = r.view->jiVoicePoint(id, 0, 2 * kQ);
  r.d.drag(p, {p.x, p.y - kRow}, 4, mod::kCtrl);
  r.d.step();
  CHECK(r.history() == h + 1);
  ride = r.object(id).ride;
  REQUIRE(ride.size() == 3);
  CHECK(ride[1].tick == 2 * kQ);
  CHECK(ride[1].value == doctest::Approx(100.0).epsilon(0.01));
  // The selected object shows it on the lowest voice; dragged another row up in pointer mode.
  r.drawMode(false);
  h = r.history();
  const Vec2 hp = r.handle(t, 1);
  CHECK(hp.y == doctest::Approx(r.view->jiVoicePoint(id, 0, 2 * kQ).y).epsilon(0.001));
  r.d.drag(hp, {hp.x, hp.y - kRow}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.object(id).ride[1].value == doctest::Approx(200.0).epsilon(0.01));
  // A double-click deletes it: no ride is left.
  h = r.history();
  r.d.doubleClick(r.handle(t, 1));
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.object(id).ride.empty());

  // Alt paints vibrato on the ride.
  r.drawMode(true);
  h = r.history();
  const Vec2 v0 = r.view->jiVoicePoint(id, 0, kQ / 2);
  r.d.drag(v0, {v0.x + 3.0f * kBeat, v0.y - 6.0f}, 8, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.object(id).ride.size() > 16);
  CHECK(r.st().vibratoAt(t, 2 * kQ).has_value());

  // A click (no drag) on the lowest voice still draws the cell's note there.
  h = r.history();
  r.d.click(r.view->jiVoicePoint(id, 0, 7 * kQ / 2 + kQ / 4));
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.noteCount() == 1);
}

TEST_CASE("roll pen: Shift + a note's body detunes it freely (a Cents note); Shift + click on a selected note deselects it") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 2 * kQ, 0), Pen::plain(2 * kQ, 2 * kQ, 4)});
  r.d.click(r.at(1.0 * kQ, 0));
  REQUIRE(r.view->selection().size() == 1);
  size_t h = r.history();
  const Vec2 p = r.at(1.0 * kQ, 0);
  r.d.drag(p, {p.x, p.y - 5.0f}, 5, mod::kShift);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.note(ids[0]).pitch.kind() == PitchKind::Cents);
  CHECK(r.note(ids[0]).pitch.cents() == doctest::Approx(500.0 / 14.0).epsilon(0.01));
  CHECK(r.note(ids[0]).start == 0);
  CHECK(r.view->selection().size() == 1);

  // Shift + click: add the other note, then take it off again; no edit.
  h = r.history();
  r.d.click(r.at(3.0 * kQ, 4), mod::kShift);
  CHECK(r.view->selection().size() == 2);
  r.d.click(r.at(3.0 * kQ, 4), mod::kShift);
  CHECK(r.view->selection().size() == 1);
  CHECK(r.history() == h);
  CHECK(r.note(ids[1]).pitch.kind() == PitchKind::Degree);
}

TEST_CASE("roll pen: a resize keeps a bend on its edge; move and duplicate carry the curve") {
  Pen r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Pen::plain(0, 2 * kQ, 0), Pen::plain(4 * kQ, 2 * kQ, 4)});
  const doc::NoteId a = ids[0];
  CHECK(r.d.press("Ctrl+A") == KeyResult::Action);
  CHECK(r.d.press("Ctrl+G") == KeyResult::Action);
  r.d.step();
  REQUIRE(r.st().bendLength(a, 1) == kQ);
  r.d.click(r.view->notePoint(a, 0.2));
  REQUIRE(r.view->selection().size() == 1);

  // Its right edge a beat later (no Alt: a resize). The glide still ends on the note's end.
  size_t h = r.history();
  const Vec2 end = r.view->notePoint(a, 0.996);
  r.d.drag(end, {end.x + kBeat, end.y}, 6);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.note(a).length == 3 * kQ);
  std::vector<doc::Breakpoint> c = r.curve(a);
  REQUIRE(c.size() > 2);
  CHECK(c.back().tick == 3 * kQ);
  CHECK(c.back().value == doctest::Approx(400.0));
  CHECK(c.front().tick == 2 * kQ);
  CHECK(r.st().bendLength(a, 1) == kQ);
  // Undo: the note and its curve as before, in one step.
  REQUIRE(r.ctx.store().undo());
  r.d.step();
  CHECK(r.note(a).length == 2 * kQ);
  CHECK(r.curve(a).back().tick == 2 * kQ);

  // Shift+Right lengthens by a grid cell: the same.
  r.d.click(r.view->notePoint(a, 0.2));
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.view->selection()[0] == a.value);  // a click on the selected note keeps it selected
  CHECK(r.d.press("Shift+Right") == KeyResult::Action);
  r.d.step();
  CHECK(r.note(a).length == 3 * kQ);
  CHECK(r.curve(a).back().tick == 3 * kQ);

  // Moved a beat later, the curve goes with it; Ctrl+D copies it.
  const Vec2 body = r.view->notePoint(a, 0.3);
  r.d.drag(body, {body.x + kBeat, body.y}, 6);
  r.d.step();
  CHECK(r.note(a).start == kQ);
  CHECK(r.curve(a).back().value == doctest::Approx(400.0));
  CHECK(r.d.press("Ctrl+D") == KeyResult::Action);
  r.d.step();
  REQUIRE(r.noteCount() == 3);
  size_t curved = 0;
  for (const doc::Note& n : r.clip().notes)
    if (!r.curve(n.id).empty() && r.curve(n.id).back().value == doctest::Approx(400.0)) ++curved;
  CHECK(curved == 2);
}

TEST_CASE("roll pen: curve arithmetic — retiming on a resize, parsing a typed vibrato") {
  using B = doc::Breakpoint;
  // A lead-in (0 … 100), a point in the middle, a lead-out (300 … 400).
  const std::vector<B> c = {{0, -200.0f, 0.0f, {}}, {100, 0.0f, 0.0f, {}}, {200, 50.0f, 0.0f, {}}, {300, 0.0f, 0.0f, {}}, {400, 300.0f, 0.0f, {}}};
  // The end 100 later: the lead-out moves with it, the middle stays.
  std::vector<B> r = RollState::retimeCurve(c, 400, 500, 0);
  REQUIRE(r.size() == 5);
  CHECK(r[2].tick == 200);
  CHECK(r[3].tick == 400);
  CHECK(r[4].tick == 500);
  CHECK(r[4].value == 300.0f);
  // The start 50 later: the lead-in stays on the start, the middle keeps its place in time.
  r = RollState::retimeCurve(c, 400, 350, 50);
  REQUIRE(r.size() == 5);
  CHECK(r[0].tick == 0);
  CHECK(r[1].tick == 100);
  CHECK(r[2].tick == 150);
  CHECK(r[3].tick == 250);
  CHECK(r[4].tick == 350);
  // Shorter than its two bends: they are squeezed, nothing is left between.
  r = RollState::retimeCurve(c, 400, 150, 0);
  REQUIRE(r.size() >= 3);
  CHECK(r.front().value == -200.0f);
  CHECK(r.back().tick == 150);
  CHECK(r.back().value == 300.0f);
  // No bends: the curve is cut at the new end, where it was.
  const std::vector<B> plain = {{0, 0.0f, 0.0f, {}}, {200, 100.0f, 0.0f, {}}, {400, 0.0f, 0.0f, {}}};
  r = RollState::retimeCurve(plain, 400, 300, 0);
  REQUIRE(r.size() == 3);
  CHECK(r[2].tick == 300);
  CHECK(r[2].value == doctest::Approx(50.0));

  double depth = 30.0, rate = 6.0;
  std::string why;
  CHECK(RollState::parseVibrato("45 \xC2\xA2 \xC2\xB7 5.5 Hz", depth, rate, why));
  CHECK(depth == 45.0);
  CHECK(rate == 5.5);
  CHECK(RollState::parseVibrato("12", depth, rate, why));  // the rate stays
  CHECK(depth == 12.0);
  CHECK(rate == 5.5);
  CHECK_FALSE(RollState::parseVibrato("deep", depth, rate, why));
  CHECK_FALSE(RollState::parseVibrato("30 90", depth, rate, why));
  CHECK(RollState::vibratoText(30.0, 6.0) == "Vibrato 30 \xC2\xA2 \xC2\xB7 6 Hz");
}

TEST_CASE("render: pianoroll-bends (a melody gliding between its notes, a straight scoop with its breakpoints) matches the golden image") { checkGolden("pianoroll-bends"); }
TEST_CASE("render: pianoroll-vibrato (a 30 ¢ / 6 Hz vibrato, a scoop into a 60 ¢ / 4 Hz one) matches the golden image") { checkGolden("pianoroll-vibrato"); }
TEST_CASE("render: pianoroll-ride (ride curves on JI objects: breakpoints on the lowest voice, a vibrato) matches the golden image") { checkGolden("pianoroll-ride"); }
