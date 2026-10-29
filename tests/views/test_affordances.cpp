// T4.16b interaction affordances in the arrangement, the piano roll and the automation lanes: every part shows the
// cursor of what a press there would do (checked by hovering it through TestDriver and then pressing there), the
// part a press would grab is highlighted (note / clip edges and bodies, breakpoints) and a hover change damages only
// that part; the keys Live users expect (Alt / Shift arrows, Esc) edit as one undo step each.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "pq/core/automation_curve.h"
#include "pq/document/commands.h"
#include "pq/ui/damage.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/fake_transport.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr Ticks kBar = kTicksPerWhole;  // 4/4 in the fixtures' first bars
constexpr Ticks kQ = kPpq;
constexpr const char* kArr = "main/body/top/track-area/arrangement";
constexpr const char* kClip = "main/body/bottom/detail/clip/clip-view";

void fixedGrid(ViewContext& ctx, int exp) {
  GridSpec g;
  g.mode = GridMode::Fixed;
  g.fixedExp = static_cast<int8_t>(exp);
  ctx.setGrid(g);
}

// Every damage rect inside `area` lies inside `allowed` (other widgets, e.g. the info view's readout, may repaint too).
bool damageWithin(UiTree& tree, const IRect& area, const IRect& allowed) {
  for (const ClipRect& c : tree.damage().rects()) {
    const IRect r{c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0};
    if (intersect(r, area).empty()) continue;
    if (c.x0 < allowed.x || c.x1 > allowed.right() || c.y0 < allowed.y || c.y1 > allowed.bottom()) return false;
  }
  return true;
}

IRect grow(const IRect& r, int32_t by) { return {r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by}; }

// Some damage rect meets `r` (the part did repaint).
bool damageMeets(UiTree& tree, const IRect& r) {
  for (const ClipRect& c : tree.damage().rects())
    if (!intersect(IRect{c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0}, r).empty()) return true;
  return false;
}

// ---- piano roll ------------------------------------------------------------------------------------------------

struct Roll {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ClipView* view = nullptr;
  doc::ClipRef ref{};

  explicit Roll(std::string_view fixture) {
    REQUIRE(ctx.openProject(views::test::fixtureDir(fixture)).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(520.0f);
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!ref.clip.valid() && t.kind == doc::TrackKind::Midi && !t.clips.empty()) ref = {t.id, t.clips.front().id};
    });
    REQUIRE(ref.clip.valid());
    ctx.setDetailClip(ref);
    d.tree().layoutNow();
    view = dynamic_cast<ClipView*>(d.find(kClip));
    REQUIRE(view != nullptr);
    REQUIRE(view->hasClip());
  }
  ~Roll() { d.tree().setRoot(std::make_unique<Widget>()); }

  const doc::NoteClip& clip() const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), ref.track);
    REQUIRE(t != nullptr);
    return *t->clips[static_cast<size_t>(doc::findClipIndex(*t, ref.clip))].clip;
  }
  const doc::Note& note(uint32_t id) const {
    const doc::Note* n = clip().notes.findById(doc::NoteId{id});
    REQUIRE(n != nullptr);
    return *n;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  IRect rect(uint32_t id) const { return view->noteRect(doc::NoteId{id}); }
  Vec2 body(uint32_t id) const {
    const IRect r = rect(id);
    return {static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f, static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f};
  }
  Vec2 endEdge(uint32_t id) const {
    const IRect r = rect(id);
    return {static_cast<float>(r.right()) - 2.0f, static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f};
  }
  Vec2 startEdge(uint32_t id) const {
    const IRect r = rect(id);
    return {static_cast<float>(r.x) + 1.0f, static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f};
  }
  Cursor cursorAt(Vec2 p, Mods m = mod::kNone) {
    d.move(p, m);
    return d.tree().cursor();
  }
  void focusNotes() { d.tree().setFocus(d.find(std::string(kClip) + "/notes")); }
};

}  // namespace

TEST_CASE("affordances: note body / start / end / empty space / Draw Mode show what a press does, highlighted") {
  Roll r("pianoroll-12edo");
  fixedGrid(r.ctx, 4);  // 1/16
  const doc::Note n0 = r.note(100);
  REQUIRE(r.rect(100).w > 40);

  // Body: Move, the body highlighted; a drag there moves the note.
  CHECK(r.cursorAt(r.body(100)) == Cursor::Move);
  CHECK(r.view->noteHover().note == 100);
  CHECK(r.view->noteHover().zone == 0);
  // End edge: ↔, the end highlighted; start edge likewise.
  CHECK(r.cursorAt(r.endEdge(100)) == Cursor::ResizeEW);
  CHECK(r.view->noteHover().zone == 1);
  CHECK(r.cursorAt(r.startEdge(100)) == Cursor::ResizeEW);
  CHECK(r.view->noteHover().zone == -1);
  // Empty space: the selection rectangle (Arrow), nothing highlighted.
  const IRect notes = r.d.rect(std::string(kClip) + "/notes");
  const Vec2 empty{static_cast<float>(r.rect(100).x) + 3.0f, static_cast<float>(notes.y) + 3.0f};
  REQUIRE(r.view->noteHover().note != 0);
  CHECK(r.cursorAt(empty) == Cursor::Arrow);
  CHECK(r.view->noteHover().note == 0);

  // Draw Mode (B): a crosshair everywhere, over notes too (a click there deletes it); only the body is highlighted.
  r.ctx.setDrawMode(true);
  CHECK(r.cursorAt(r.endEdge(100)) == Cursor::Crosshair);
  CHECK(r.view->noteHover().note == 100);
  CHECK(r.view->noteHover().zone == 0);
  CHECK(r.cursorAt(empty) == Cursor::Crosshair);
  r.ctx.setDrawMode(false);
  CHECK(r.cursorAt(empty) == Cursor::Arrow);
  // Leaving the note area clears the highlight.
  r.d.move(r.body(100));
  REQUIRE(r.view->noteHover().note == 100);
  r.d.leave();
  CHECK(r.view->noteHover().note == 0);

  // The cursor predicts the press: a drag from the end edge keeps ↔ and resizes (half a beat shorter: note 105 starts
  // on the same row two beats in); the start edge moves the start; the body moves the note.
  size_t h = r.history();
  const Vec2 from = r.endEdge(100);
  const float beat = static_cast<float>(static_cast<double>(kQ) * r.view->pxPerTick());
  r.d.move(from);
  r.d.down(from);
  r.d.move({from.x - beat * 0.25f, from.y});
  r.d.move({from.x - beat * 0.5f + 1.0f, from.y + 30.0f});  // off the note: the drag keeps its cursor
  CHECK(r.d.tree().cursor() == Cursor::ResizeEW);
  CHECK(r.view->noteHover().note == 0);  // a gesture shows its own preview
  r.d.up({from.x - beat * 0.5f + 1.0f, from.y});
  CHECK(r.history() == h + 1);
  CHECK(r.note(100).start == n0.start);
  CHECK(r.note(100).length == n0.length - kQ / 2);
  CHECK(r.view->noteHover().note == 100);  // after the release: what the next press would grab
  CHECK(r.view->noteHover().zone == 1);
  h = r.history();
  r.d.drag(r.startEdge(100), {r.startEdge(100).x + beat * 0.5f + 1.0f, r.startEdge(100).y}, 5);
  CHECK(r.history() == h + 1);
  CHECK(r.note(100).start == n0.start + kQ / 2);
  CHECK(r.note(100).length == n0.length - kQ);
  h = r.history();
  const Vec2 b = r.body(100);
  r.d.move(b);
  r.d.down(b);
  r.d.move({b.x + beat * 0.25f, b.y});
  r.d.move({b.x + beat * 0.5f + 1.0f, b.y});
  CHECK(r.d.tree().cursor() == Cursor::Move);
  r.d.up({b.x + beat * 0.5f + 1.0f, b.y});
  CHECK(r.history() == h + 1);
  CHECK(r.note(100).start == n0.start + kQ);
  CHECK(r.view->noteHover().note == 100);
  CHECK(r.view->noteHover().zone == 0);
}

TEST_CASE("affordances: a note hover change damages only the two notes' boxes; idle draws nothing") {
  Roll r("pianoroll-12edo");
  const IRect notes = r.d.rect(std::string(kClip) + "/notes");
  const Vec2 empty{static_cast<float>(r.rect(100).x) + 3.0f, static_cast<float>(notes.y) + 3.0f};
  r.d.move(empty);
  r.d.advance(0.1);
  const uint64_t frames = r.d.framesDrawn();
  r.d.advance(2.0);
  CHECK(r.d.framesDrawn() == frames);  // idle: no frames
  r.d.move(r.body(100));
  REQUIRE(r.view->noteHover().note == 100);
  CHECK(damageMeets(r.d.tree(), r.rect(100)));
  CHECK(damageWithin(r.d.tree(), notes, grow(r.rect(100), 2)));
  r.d.advance(0.1);
  // From one note to another: both boxes, nothing else in the note area.
  r.d.move(r.body(116));
  REQUIRE(r.view->noteHover().note == 116);
  CHECK(damageMeets(r.d.tree(), r.rect(100)));
  CHECK(damageMeets(r.d.tree(), r.rect(116)));
  IRect both = grow(r.rect(100), 2);
  const IRect b2 = grow(r.rect(116), 2);
  const int32_t x0 = std::min(both.x, b2.x), y0 = std::min(both.y, b2.y);
  both = {x0, y0, std::max(both.right(), b2.right()) - x0, std::max(both.bottom(), b2.bottom()) - y0};
  CHECK(damageWithin(r.d.tree(), notes, both));
  // Moving within the same part changes nothing: no damage in the note area.
  r.d.advance(0.1);
  r.d.move({r.body(116).x + 1.0f, r.body(116).y});
  CHECK(damageWithin(r.d.tree(), notes, IRect{}));
}

TEST_CASE("affordances: velocity stems, the keys and the clip ruler show their drag cursors") {
  Roll r("pianoroll-12edo");
  // A stem: ↕, and a drag there changes the velocity (one undo step).
  const Vec2 head = r.view->stemHead(doc::NoteId{100});
  REQUIRE(head.x >= 0.0f);
  CHECK(r.cursorAt(head) == Cursor::ResizeNS);
  const float v0 = r.note(100).velocity;
  const size_t h = r.history();
  r.d.move(head);
  r.d.down(head);
  r.d.move({head.x, head.y - 10.0f});
  r.d.move({head.x + 40.0f, head.y - 20.0f});  // away from the stem: the drag keeps its cursor
  CHECK(r.d.tree().cursor() == Cursor::ResizeNS);
  r.d.up({head.x + 40.0f, head.y - 20.0f});
  CHECK(r.history() == h + 1);
  CHECK(r.note(100).velocity > v0);
  const IRect lane = r.d.rect(std::string(kClip) + "/velocity");
  CHECK(r.cursorAt({static_cast<float>(lane.right()) - 3.0f, static_cast<float>(lane.y) + 4.0f}) == Cursor::Arrow);

  // The keys: a click auditions (Pointer).
  CHECK(r.cursorAt(r.d.center(std::string(kClip) + "/keys")) == Cursor::Pointer);

  // The ruler: the start flag and the loop brace's ends ↔, the brace itself moves, the time strip zooms (↕).
  const IRect ruler = r.d.rect(std::string(kClip) + "/ruler");
  const doc::ClipLoop loop = r.clip().loop;
  const float yBrace = static_cast<float>(ruler.y) + 5.0f, yTime = static_cast<float>(ruler.bottom()) - 6.0f;
  const float lx1 = r.view->xOfTick(static_cast<double>(loop.end));
  CHECK(r.cursorAt({lx1 - 1.0f, yBrace}) == Cursor::ResizeEW);
  CHECK(r.cursorAt({r.view->xOfTick(static_cast<double>(r.clip().contentStart)) + 2.0f, yBrace}) == Cursor::ResizeEW);
  CHECK(r.cursorAt({r.view->xOfTick(static_cast<double>(loop.start + loop.end) * 0.5), yBrace}) == Cursor::Move);
  CHECK(r.cursorAt({r.view->xOfTick(static_cast<double>(loop.start + loop.end) * 0.5), yTime}) == Cursor::ResizeNS);
}

TEST_CASE("affordances: piano roll keys — Shift+arrows change the length by a grid cell, Alt+arrows nudge off the grid, Esc deselects") {
  Roll r("pianoroll-12edo");
  fixedGrid(r.ctx, 4);  // 1/16
  const doc::Note n0 = r.note(100);
  REQUIRE((n0.start + n0.length) % (kTicksPerWhole / 16) == 0);
  const doc::NoteId one[] = {doc::NoteId{100}};
  r.view->select(one);
  r.focusNotes();
  size_t h = r.history();
  CHECK(r.d.press("Shift+Right") == KeyResult::Action);
  CHECK(r.note(100).length == n0.length + kTicksPerWhole / 16);
  CHECK(r.note(100).start == n0.start);
  CHECK(r.history() == h + 1);
  CHECK(r.d.press("Shift+Left") == KeyResult::Action);
  CHECK(r.d.press("Shift+Left") == KeyResult::Action);
  CHECK(r.note(100).length == n0.length - kTicksPerWhole / 16);
  CHECK(r.history() == h + 3);
  // Shortening stops at one step: with a whole-bar grid the note (shorter than two bars) cannot shrink.
  fixedGrid(r.ctx, 0);
  h = r.history();
  CHECK(r.d.press("Shift+Left") == KeyResult::Action);
  CHECK(r.history() == h);
  CHECK(r.note(100).length == n0.length - kTicksPerWhole / 16);
  // Alt+Right / Left: a 1/128 note off the grid; the earliest note stops at the clip start.
  fixedGrid(r.ctx, 4);
  h = r.history();
  CHECK(r.d.press("Alt+Right") == KeyResult::Action);
  CHECK(r.note(100).start == n0.start + kTicksPerWhole / 128);
  CHECK(r.history() == h + 1);
  CHECK(r.d.press("Alt+Left") == KeyResult::Action);
  CHECK(r.note(100).start == n0.start);
  // Esc deselects; with nothing selected it falls through (no action).
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.d.press("Escape") == KeyResult::Action);
  CHECK(r.view->selection().empty());
  CHECK(r.d.press("Escape") != KeyResult::Action);
}

// ---- arrangement --------------------------------------------------------------------------------------------------

namespace {

struct Arr {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  explicit Arr(std::string_view fixture = "arrangement") {
    REQUIRE(ctx.openProject(views::test::fixtureDir(fixture)).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    ctx.setDetailVisible(false);
    fixedGrid(ctx, 2);  // 1/4
    d.tree().layoutNow();
    arr->setTimeView(0.0, 60.0 / static_cast<double>(kBar));  // 60 logical px per bar
    d.tree().layoutNow();
    d.step();
  }
  ~Arr() { d.tree().setRoot(std::make_unique<Widget>()); }

  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Track& track(std::string_view name) const {
    const doc::Track* found = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!found && t.name == name) found = &t;
    });
    REQUIRE_MESSAGE(found != nullptr, name);
    return *found;
  }
  int32_t row(doc::TrackId id) const {
    const auto rows = arr->rows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].id == id) return static_cast<int32_t>(i);
    return -1;
  }
  doc::ClipRef clip(std::string_view trackName, size_t i) const {
    const doc::Track& t = track(trackName);
    REQUIRE(i < t.clips.size());
    return {t.id, t.clips[i].id};
  }
  const doc::NoteClip& get(const doc::ClipRef& r) const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), r.track);
    REQUIRE(t != nullptr);
    const int32_t i = doc::findClipIndex(*t, r.clip);
    REQUIRE(i >= 0);
    return *t->clips[static_cast<size_t>(i)].clip;
  }
  float x(Ticks t) const { return arr->xOfTick(static_cast<double>(t)); }
  Vec2 title(const doc::ClipRef& r, Ticks dt) const {
    return {x(get(r).start + dt) + 0.5f, static_cast<float>(arr->rowLaneRect(row(r.track)).y + 7)};
  }
  Vec2 body(const doc::ClipRef& r, Ticks dt) const {
    return {x(get(r).start + dt) + 0.5f, static_cast<float>(arr->rowLaneRect(row(r.track)).bottom() - 8)};
  }
  Cursor cursorAt(Vec2 p, Mods m = mod::kNone) {
    d.move(p, m);
    return d.tree().cursor();
  }
};

}  // namespace

TEST_CASE("affordances: clip title / ends / body / empty lane show what a press does; the grabbed part is highlighted") {
  Arr f;
  const doc::ClipRef bassB = f.clip("Bass", 1);  // bars 4–8
  const doc::NoteClip c0 = f.get(bassB);
  CHECK(f.cursorAt(f.title(bassB, kBar)) == Cursor::Move);
  CHECK(f.arr->clipHover().clip.clip == bassB.clip);
  CHECK(f.arr->clipHover().zone == 0);
  const IRect box = f.arr->clipRect(bassB);
  const Vec2 endEdge{static_cast<float>(box.right()) - 2.0f, f.body(bassB, kBar).y};
  CHECK(f.cursorAt(endEdge) == Cursor::ResizeEW);
  CHECK(f.arr->clipHover().zone == 1);
  CHECK(f.cursorAt({static_cast<float>(box.x) + 1.0f, f.title(bassB, 0).y}) == Cursor::ResizeEW);
  CHECK(f.arr->clipHover().zone == -1);
  // The body selects time (Live): no move cursor, no highlight.
  CHECK(f.cursorAt(f.body(bassB, kBar)) == Cursor::Arrow);
  CHECK_FALSE(f.arr->clipHover().clip.clip.valid());
  // Empty lane space.
  const IRect strings = f.arr->rowLaneRect(f.row(f.track("Strings").id));
  CHECK(f.cursorAt({f.x(kBar), static_cast<float>(strings.y + strings.h / 2)}) == Cursor::Arrow);

  // A hover change damages only that clip's box in the lanes.
  f.d.move(f.body(bassB, kBar));
  f.d.advance(0.1);
  f.d.move(f.title(bassB, kBar));
  REQUIRE(f.arr->clipHover().zone == 0);
  CHECK(damageMeets(f.d.tree(), box));
  CHECK(damageWithin(f.d.tree(), f.arr->lanesViewport(), grow(box, 2)));

  // The cursor predicts the press: the end edge drags ↔ and resizes the clip (one undo step).
  const size_t h = f.history();
  f.d.move(endEdge);
  f.d.down(endEdge);
  f.d.move({endEdge.x + 20.0f, endEdge.y});
  f.d.move({endEdge.x + 31.0f, endEdge.y + 30.0f});
  CHECK(f.d.tree().cursor() == Cursor::ResizeEW);
  CHECK_FALSE(f.arr->clipHover().clip.clip.valid());
  f.d.up({endEdge.x + 31.0f, endEdge.y + 30.0f});
  CHECK(f.history() == h + 1);
  CHECK(f.get(bassB).start == c0.start);
  CHECK(f.get(bassB).length == c0.length + kBar / 2);  // 31 px at 60 px per bar, snapped to the quarter grid
  // The title drags the clip (Move).
  const Vec2 t = f.title(bassB, kBar);
  f.d.move(t);
  f.d.down(t);
  f.d.move({t.x + 20.0f, t.y});
  f.d.move({t.x + 30.0f, t.y});
  CHECK(f.d.tree().cursor() == Cursor::Move);
  f.d.up({t.x + 30.0f, t.y});
  CHECK(f.history() == h + 2);
  CHECK(f.get(bassB).start == c0.start + kBar / 2);
}

TEST_CASE("affordances: arrangement keys — arrows nudge selected clips (Alt off the grid), Shift extends time, Esc deselects") {
  Arr f;
  const doc::ClipRef strings = f.clip("Strings", 0);  // bars 4–12, alone on its track
  const Ticks s0 = f.get(strings).start;
  f.d.click(f.title(strings, kBar));
  REQUIRE(f.arr->selection().clips.size() == 1);
  REQUIRE(f.d.tree().focus() == f.arr);
  size_t h = f.history();
  CHECK(f.d.press("Right") == KeyResult::Action);
  CHECK(f.get(strings).start == s0 + kBar / 4);
  CHECK(f.history() == h + 1);
  CHECK(f.arr->clipSelected(strings.clip));  // still selected: the arrows keep nudging it
  CHECK(f.d.press("Left") == KeyResult::Action);
  CHECK(f.d.press("Left") == KeyResult::Action);
  CHECK(f.get(strings).start == s0 - kBar / 4);
  CHECK(f.d.press("Alt+Right") == KeyResult::Action);
  CHECK(f.get(strings).start == s0 - kBar / 4 + kTicksPerWhole / 128);
  CHECK(f.history() == h + 4);
  // Esc drops the clips and the time selection; the track stays selected.
  CHECK(f.d.press("Escape") == KeyResult::Action);
  CHECK(f.arr->selection().clips.empty());
  CHECK_FALSE(f.arr->selection().time.has_value());
  REQUIRE(f.arr->selection().tracks.size() == 1);
  CHECK(f.arr->selection().tracks[0] == strings.track);
  CHECK(f.d.press("Escape") != KeyResult::Action);  // nothing left to drop: falls through
  // Without clips, Left / Right move the insert marker (T3.x-fix-look) and Alt+arrows do nothing.
  f.ctx.setStartMarker(4 * kBar);
  CHECK(f.d.press("Alt+Right") != KeyResult::Action);
  // Shift+Right from the start marker: a range one grid cell long on the selected track, its clips selected.
  CHECK(f.d.press("Shift+Right") == KeyResult::Action);
  REQUIRE(f.arr->selection().time.has_value());
  CHECK(*f.arr->selection().time == TickRange{4 * kBar, 4 * kBar + kBar / 4});
  CHECK(f.arr->selection().range);
  CHECK(f.d.press("Shift+Right") == KeyResult::Action);
  CHECK(f.arr->selection().time->end == 4 * kBar + kBar / 2);
  REQUIRE(f.arr->selection().clips.size() == 1);
  CHECK(f.arr->selection().clips[0].clip == strings.clip);
  CHECK(f.d.press("Shift+Left") == KeyResult::Action);
  CHECK(f.d.press("Shift+Left") == KeyResult::Action);
  CHECK(f.d.press("Shift+Left") == KeyResult::Action);  // never before its start
  CHECK(*f.arr->selection().time == TickRange{4 * kBar, 4 * kBar});
  CHECK_FALSE(f.arr->selection().range);
  CHECK(f.history() == h + 4);  // selecting is not an edit
}


TEST_CASE("affordances: the arrangement ruler's loop brace and locators drag (one undo step each); the numbers zoom") {
  Arr f;
  namespace am = arrangement_metrics;
  const IRect ruler = f.d.rect(std::string(kArr) + "/ruler");
  const float yNum = static_cast<float>(ruler.y) + 6.0f;
  const float yLoop = static_cast<float>(ruler.y) + am::kRulerNumberRow + am::kRulerLoopRow * 0.5f;
  const float yMark = static_cast<float>(ruler.y) + am::kRulerNumberRow + am::kRulerLoopRow + 5.0f;
  const doc::LoopRegion loop0 = f.ctx.doc()->loop;
  REQUIRE(loop0 == doc::LoopRegion{4 * kBar, 8 * kBar, true});
  const float bar = f.x(kBar) - f.x(0);
  CHECK(f.cursorAt({f.x(4 * kBar) + 1.0f, yLoop}) == Cursor::ResizeEW);
  CHECK(f.cursorAt({f.x(8 * kBar) - 1.0f, yLoop}) == Cursor::ResizeEW);
  CHECK(f.cursorAt({f.x(6 * kBar), yLoop}) == Cursor::Move);
  CHECK(f.cursorAt({f.x(6 * kBar), yNum}) == Cursor::ResizeNS);
  CHECK(f.cursorAt({f.x(8 * kBar) + 3.0f, yMark}) == Cursor::ResizeEW);  // the Chorus locator's flag
  CHECK(f.cursorAt({f.x(6 * kBar), yMark}) == Cursor::ResizeNS);

  // The loop end two bars later: previewed, then one SetLoop.
  size_t h = f.history();
  const Vec2 e0{f.x(8 * kBar) - 1.0f, yLoop};
  f.d.move(e0);
  f.d.down(e0);
  f.d.move({e0.x + bar, yLoop});
  f.d.move({e0.x + 2.0f * bar + 2.0f, yLoop + 20.0f});
  CHECK(f.d.tree().cursor() == Cursor::ResizeEW);
  CHECK(f.history() == h);  // nothing dispatched while dragging
  f.d.up({e0.x + 2.0f * bar + 2.0f, yLoop + 20.0f});
  CHECK(f.history() == h + 1);
  CHECK(f.ctx.doc()->loop == doc::LoopRegion{4 * kBar, 10 * kBar, true});
  // The whole brace one bar earlier.
  f.d.drag({f.x(6 * kBar), yLoop}, {f.x(6 * kBar) - bar - 2.0f, yLoop}, 5);
  CHECK(f.history() == h + 2);
  CHECK(f.ctx.doc()->loop == doc::LoopRegion{3 * kBar, 9 * kBar, true});
  REQUIRE(f.ctx.undo());
  CHECK(f.ctx.doc()->loop == doc::LoopRegion{4 * kBar, 10 * kBar, true});
  // Esc cancels a brace drag: nothing changes.
  const Vec2 s0{f.x(4 * kBar) + 1.0f, yLoop};
  f.d.move(s0);
  f.d.down(s0);
  f.d.move({s0.x + bar, yLoop});
  f.d.move({s0.x + bar + 3.0f, yLoop});
  CHECK(f.d.press("Escape") == KeyResult::Widget);
  f.d.up({s0.x + bar + 3.0f, yLoop});
  CHECK(f.ctx.doc()->loop == doc::LoopRegion{4 * kBar, 10 * kBar, true});
  CHECK(f.history() == h + 1);

  // A locator: dragged one bar later (UpdateLocator, one undo step); a click on one jumps there.
  const auto locTick = [&](std::string_view name) {
    for (const doc::Locator& l : *f.ctx.doc()->locators)
      if (l.name == name) return l.tick;
    return Ticks{-1};
  };
  REQUIRE(locTick("Chorus") == 8 * kBar);
  h = f.history();
  f.d.drag({f.x(8 * kBar) + 3.0f, yMark}, {f.x(8 * kBar) + 3.0f + bar + 2.0f, yMark}, 5);
  CHECK(f.history() == h + 1);
  CHECK(locTick("Chorus") == 9 * kBar);
  f.d.click({f.x(4 * kBar) + 3.0f, yMark});  // Verse
  CHECK(f.ctx.startMarker() == 4 * kBar);
  CHECK(f.history() == h + 1);
  // The numbers row: a click still locates (snapped).
  f.d.click({f.x(2 * kBar) + 1.0f, yNum});
  CHECK(f.ctx.startMarker() == 2 * kBar);
}

TEST_CASE("affordances: track and lane header edges resize (↕); header buttons are pointers") {
  Arr f;
  const TrackHeader* bass = f.arr->header(f.track("Bass").id);
  REQUIRE(bass != nullptr);
  const IRect r = bass->rect();
  CHECK(f.cursorAt({static_cast<float>(r.x) + 20.0f, static_cast<float>(r.bottom()) - 1.0f}) == Cursor::ResizeNS);
  // The resize keeps ↕ while it runs, and resizes the row.
  const float h0 = f.arr->trackHeight(f.track("Bass").id);
  const Vec2 e{static_cast<float>(r.x) + 20.0f, static_cast<float>(r.bottom()) - 1.0f};
  f.d.move(e);
  f.d.down(e);
  f.d.move({e.x + 50.0f, e.y + 20.0f});
  CHECK(f.d.tree().cursor() == Cursor::ResizeNS);
  f.d.up({e.x + 50.0f, e.y + 20.0f});
  CHECK(f.arr->trackHeight(f.track("Bass").id) == doctest::Approx(h0 + 20.0f).epsilon(0.05));
  // A group's fold triangle.
  const TrackHeader* keys = f.arr->header(f.track("Keys").id);
  REQUIRE(keys != nullptr);
  const Widget* fold = keys->find("fold");
  REQUIRE(fold != nullptr);
  const IRect fr = fold->rect();
  CHECK(f.cursorAt({static_cast<float>(fr.x + fr.w / 2), static_cast<float>(fr.y + fr.h / 2)}) == Cursor::Pointer);
  // The overview strip drags the view.
  CHECK(f.cursorAt(f.d.center(std::string(kArr) + "/overview")) == Cursor::Move);
}

// ---- automation ---------------------------------------------------------------------------------------------------

TEST_CASE("affordances: breakpoints move, the line adds, Alt bends a segment (also when Alt is pressed with the pointer still)") {
  Arr f("automation");
  ArrangementAutomation& au = f.arr->automation();
  const doc::Track& drums = f.track("Drums");
  au.showLanes(drums.id, true);
  f.d.tree().layoutNow();
  const doc::ParamRef vol = doc::ParamRef::track(doc::ParamKind::TrackVolume, drums.id);
  const auto band = au.band(drums.id, vol, false);
  REQUIRE(band.has_value());
  REQUIRE(band->lane != nullptr);
  const std::vector<doc::Breakpoint> pts = band->lane->points;
  REQUIRE(pts.size() >= 3);
  REQUIRE(pts[1].tick == 2 * kBar);
  const Vec2 p1 = *au.pointPos(*band, pts[1].id);

  // A breakpoint: Move, and it is highlighted.
  CHECK(f.cursorAt(p1) == Cursor::Move);
  CHECK(au.hoveredPoint() == pts[1].id);
  // Alt pressed with the pointer still: the press would bend the segment (↕), no breakpoint highlighted.
  f.d.tree().setFocus(f.arr);
  (void)f.d.keyDown(key::kLAlt, mod::kAlt);
  CHECK(f.d.tree().cursor() == Cursor::ResizeNS);
  CHECK_FALSE(au.hoveredPoint().valid());
  f.d.keyUp(key::kLAlt, mod::kNone);
  CHECK(f.d.tree().cursor() == Cursor::Move);
  CHECK(au.hoveredPoint() == pts[1].id);

  // The line between points: a click adds a breakpoint there (Pointer); Shift turns that into a selection, Alt bends.
  const double t = static_cast<double>(kBar);
  const Vec2 line{f.x(kBar), au.yOfValue(*band, pq::evalAutomation(std::span<const doc::Breakpoint>(pts), t))};
  CHECK(f.cursorAt(line) == Cursor::Pointer);
  CHECK_FALSE(au.hoveredPoint().valid());
  CHECK(f.cursorAt(line, mod::kShift) == Cursor::Arrow);
  CHECK(f.cursorAt(line, mod::kAlt) == Cursor::ResizeNS);
  // Away from the line: a time selection.
  const float away = std::fabs(line.y - band->top) > std::fabs(line.y - band->bottom) ? band->top + 2.0f : band->bottom - 2.0f;
  CHECK(f.cursorAt({line.x, away}) == Cursor::Arrow);
  // The cursor's promise: a click on the line adds one breakpoint (one undo step); dragging a breakpoint keeps Move.
  const size_t h = f.history();
  f.d.click(line);
  CHECK(f.history() == h + 1);
  const auto band2 = au.band(drums.id, vol, false);
  REQUIRE(band2.has_value());
  CHECK(band2->lane->points.size() == pts.size() + 1);
  const Vec2 q = *au.pointPos(*band2, pts[1].id);
  f.d.move(q);
  f.d.down(q);
  f.d.move({q.x, q.y + 6.0f});
  f.d.move({q.x + 40.0f, q.y + 12.0f});
  CHECK(f.d.tree().cursor() == Cursor::Move);
  f.d.up({q.x + 40.0f, q.y + 12.0f});
  CHECK(f.history() == h + 2);
}

TEST_CASE("affordances: a breakpoint hover damages only its columns; lane headers resize (↕) and choose (Pointer)") {
  Arr f("automation");
  ArrangementAutomation& au = f.arr->automation();
  const doc::Track& drums = f.track("Drums");
  au.showLanes(drums.id, true);
  f.d.tree().layoutNow();
  const doc::ParamRef vol = doc::ParamRef::track(doc::ParamKind::TrackVolume, drums.id);
  const auto band = au.band(drums.id, vol, false);
  REQUIRE(band.has_value());
  const doc::Breakpoint pt = band->lane->points[1];
  const Vec2 p = *au.pointPos(*band, pt.id);
  f.d.move({p.x, band->top + 1.0f});
  f.d.advance(0.1);
  f.d.move(p);
  REQUIRE(au.hoveredPoint() == pt.id);
  const IRect lv = f.arr->lanesViewport();
  CHECK(damageWithin(f.d.tree(), lv, IRect{static_cast<int32_t>(p.x) - 12, lv.y, 25, lv.h}));
  CHECK(damageMeets(f.d.tree(), IRect{static_cast<int32_t>(p.x) - 1, static_cast<int32_t>(p.y) - 1, 2, 2}));

  const IRect lh = f.d.rect(std::string(kArr) + "/tracks/lane[0]");
  REQUIRE_FALSE(lh.empty());
  CHECK(f.cursorAt({static_cast<float>(lh.x + lh.w / 2), static_cast<float>(lh.bottom()) - 1.0f}) == Cursor::ResizeNS);
  CHECK(f.cursorAt({static_cast<float>(lh.x + lh.w / 2), static_cast<float>(lh.y) + 8.0f}) == Cursor::Pointer);
}

// ---- goldens ------------------------------------------------------------------------------------------------------

namespace {

void checkGolden(const char* scene, const char* fixture) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, views::test::fixtureDir(fixture), w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(d));
}

}  // namespace

TEST_CASE("render: arrangement-hover (a clip's end edge under the pointer) matches the golden image") {
  checkGolden("arrangement-hover", "arrangement");
}

TEST_CASE("render: pianoroll-hover (a note's end under the pointer) matches the golden image") {
  checkGolden("pianoroll-hover", "pianoroll-12edo");
}
