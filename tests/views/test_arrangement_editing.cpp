// T3.9 arrangement editing: selection (click, Shift / Ctrl, rubber band = time selection, Ctrl+A, track headers),
// clip move with relative snap on a 7-tuplet grid, Alt-drag copy, cross-track drag, edge resize, split, duplicate,
// consolidate, delete, the time edits, Ctrl+Shift+M, Ctrl+L, `0`, the clipboard, the right-click grid menu and
// Ctrl+1–5, double-click into the clip view; every edit exactly one undo step, a 60-frame drag one undo entry and no
// document change while it runs; a drag frame damages only the dragged clip's old and new rects; partial redraws
// during drags equal full ones; the `arrangement-selection` golden.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <ostream>
#include <string>

#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/overlay.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr Ticks kBar = kTicksPerWhole;  // 4/4 in the fixture's first 16 bars
constexpr const char* kArr = "main/body/top/track-area/arrangement";

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d;
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  explicit Fixture(uint32_t w = 1280, uint32_t h = 800) : d(w, h) {
    REQUIRE(ctx.openProject(views::test::fixtureDir("arrangement")).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    d.tree().layoutNow();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }  // widgets before the context they observe

  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Track& track(std::string_view name) const {
    const doc::Track* found = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!found && t.name == name) found = &t;
    });
    REQUIRE(found != nullptr);
    return *found;
  }
  int32_t row(std::string_view name) const {
    const auto rows = arr->rows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].track->name == name) return static_cast<int32_t>(i);
    FAIL("no row " << name);
    return -1;
  }
  // The i-th clip of a track (in time order).
  doc::ClipRef clip(std::string_view trackName, size_t i) const {
    const doc::Track& t = track(trackName);
    REQUIRE(i < t.clips.size());
    return {t.id, t.clips[i].id};
  }
  // The clip with this id wherever it is now (nullptr: gone).
  const doc::NoteClip* find(doc::ClipId id, doc::TrackId* on = nullptr) const {
    const doc::NoteClip* c = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      for (const doc::ClipSlot& s : t.clips)
        if (s.id == id) {
          c = s.clip.get();
          if (on) *on = t.id;
        }
    });
    return c;
  }
  const doc::NoteClip& get(const doc::ClipRef& r) const {
    const doc::NoteClip* c = find(r.clip);
    REQUIRE(c != nullptr);
    return *c;
  }
  // Points in a clip's title strip / body, `dt` ticks after its start; in a row's lane at a tick.
  Vec2 title(const doc::ClipRef& r, Ticks dt) {
    const IRect lane = arr->rowLaneRect(rowOfTrack(r.track));
    return {arr->xOfTick(static_cast<double>(get(r).start + dt)) + 0.5f, static_cast<float>(lane.y + 7)};
  }
  Vec2 body(const doc::ClipRef& r, Ticks dt) {
    const IRect lane = arr->rowLaneRect(rowOfTrack(r.track));
    return {arr->xOfTick(static_cast<double>(get(r).start + dt)) + 0.5f, static_cast<float>(lane.bottom() - 8)};
  }
  Vec2 lane(std::string_view rowName, Ticks t) {
    const IRect lane = arr->rowLaneRect(row(rowName));
    return {arr->xOfTick(static_cast<double>(t)) + 0.5f, static_cast<float>(lane.bottom() - 8)};
  }
  int32_t rowOfTrack(doc::TrackId id) const {
    const auto rows = arr->rows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].id == id) return static_cast<int32_t>(i);
    FAIL("track is not a row");
    return -1;
  }
  std::vector<Ticks> starts(std::string_view trackName) const {
    std::vector<Ticks> v;
    for (const doc::ClipSlot& s : track(trackName).clips) v.push_back(s.clip->start);
    return v;
  }
  bool selected(const doc::ClipRef& r) const { return arr->clipSelected(r.clip); }
};

}  // namespace

TEST_CASE("arrangement editing: click selects and shows the clip; Shift adds, Ctrl toggles; marker; Ctrl+A; headers") {
  Fixture f;
  const size_t h0 = f.history();
  const doc::ClipRef bassA = f.clip("Bass", 0), bassB = f.clip("Bass", 1);
  f.d.click(f.title(bassA, kBar));
  REQUIRE(f.arr->selection().clips.size() == 1);
  CHECK(f.selected(bassA));
  CHECK(f.ctx.detailClip().clip == bassA.clip);  // the clip view shows it (Live)
  REQUIRE(f.arr->timeSelection().has_value());
  CHECK(*f.arr->timeSelection() == TickRange{0, 4 * kBar});
  CHECK(f.arr->selection().tracks == std::vector<doc::TrackId>{bassA.track});
  CHECK(f.d.tree().focus() == f.arr);

  f.d.click(f.title(bassB, kBar), mod::kShift);
  CHECK(f.arr->selection().clips.size() == 2);
  CHECK(*f.arr->timeSelection() == TickRange{0, 8 * kBar});
  CHECK(f.ctx.detailClip().clip == bassB.clip);
  f.d.click(f.title(bassA, kBar), mod::kCtrl);
  CHECK_FALSE(f.selected(bassA));
  CHECK(f.selected(bassB));
  f.d.click(f.title(bassA, kBar), mod::kCtrl);
  CHECK(f.selected(bassA));
  // A click on one clip of a multiple selection selects just it.
  f.d.click(f.title(bassB, kBar / 2));
  CHECK(f.arr->selection().clips.size() == 1);
  CHECK(f.selected(bassB));

  // Empty lane space: the insert marker (snapped: the adaptive 1/4 grid at the default zoom) on that track.
  f.d.click(f.lane("Strings", kBar + 300));
  CHECK(f.arr->selection().clips.empty());
  REQUIRE(f.arr->selection().time.has_value());
  CHECK(*f.arr->selection().time == TickRange{kBar, kBar});
  CHECK(f.arr->selection().tracks == std::vector<doc::TrackId>{f.track("Strings").id});
  CHECK_FALSE(f.arr->timeSelection().has_value());  // a marker is not a range (Z fits the song)

  // A click in a clip's body selects it and puts the marker there.
  f.d.click(f.body(bassB, kBar + 200));
  CHECK(f.selected(bassB));
  CHECK(*f.arr->selection().time == TickRange{5 * kBar, 5 * kBar});

  // Ctrl+A: every clip (the folded group's too).
  CHECK(f.d.press("Ctrl+A") == KeyResult::Action);
  size_t all = 0;
  doc::forEachTrack(*f.ctx.doc(), [&](const doc::Track& t, const doc::Track*) { all += t.clips.size(); });
  CHECK(f.arr->selection().clips.size() == all);
  CHECK(all == 12);

  // A track header click selects the track (and nothing else).
  const IRect hdr = f.d.rect(std::string(kArr) + "/tracks/track[3]");  // Piano
  f.d.click({static_cast<float>(hdr.x + 40), static_cast<float>(hdr.y + 5)});
  CHECK(f.arr->selection().clips.empty());
  CHECK(f.arr->selection().tracks == std::vector<doc::TrackId>{f.track("Piano").id});
  CHECK(f.history() == h0);  // selection is not undoable (Q15)
}

TEST_CASE("arrangement editing: drag-move with snap on a 7-tuplet grid lands on exact ticks; 60 frames = one undo step") {
  Fixture f;
  GridSpec g;
  g.mode = GridMode::Fixed;
  g.fixedExp = 4;  // 1/16
  g.tuplet = 7;    // 7 lines per quarter: every kPpq / 7 ticks
  f.ctx.setGrid(g);
  constexpr Ticks kCell = kPpq / 7;
  static_assert(kCell * 7 == kPpq);
  const doc::ClipRef bassB = f.clip("Bass", 1);
  const Ticks start0 = f.get(bassB).start;
  REQUIRE(start0 == 4 * kBar);
  f.arr->setTimeView(static_cast<double>(start0 - 2000), 24.0 / static_cast<double>(kCell));  // a cell = 24 px
  f.d.step();

  for (const float dx : {137.0f, -61.0f, 300.0f}) {
    const Ticks before = f.get(bassB).start;
    const size_t h0 = f.history();
    const doc::Snapshot snap0 = f.ctx.doc();
    const Vec2 from = f.title(bassB, 5000);
    const Vec2 to{from.x + dx, from.y};
    f.d.move(from);
    f.d.down(from);
    for (int i = 1; i <= 60; ++i) {
      f.d.move({from.x + dx * static_cast<float>(i) / 60.0f, from.y});
      f.d.step();
      CHECK(f.ctx.doc() == snap0);  // previewed, not dispatched
    }
    CHECK(f.arr->dragging());
    f.d.up(to);
    CHECK_FALSE(f.arr->dragging());
    CHECK(f.history() == h0 + 1);
    const Grid grid(g, f.ctx.doc()->timeline->sigMap, f.arr->pxPerTick());
    const auto raw = static_cast<Ticks>(std::llround(f.arr->tickAtX(to.x) - f.arr->tickAtX(from.x)));
    const Ticks expected = before + grid.snapDelta(before, raw);
    CHECK(f.get(bassB).start == expected);
    CHECK(expected != before);
    CHECK(expected % kCell == 0);  // on a 7-tuplet line (bars are whole quarters here)
    CHECK(f.selected(bassB));      // the moved clip stays selected
  }
  // Undo three times: back where it was.
  for (int i = 0; i < 3; ++i) REQUIRE(f.ctx.undo());
  CHECK(f.get(bassB).start == start0);

  // Esc cancels a drag: nothing dispatched, the release does nothing.
  const size_t h1 = f.history();
  const doc::Snapshot snap1 = f.ctx.doc();
  const Vec2 from = f.title(bassB, 5000);
  f.d.move(from);
  f.d.down(from);
  f.d.move({from.x + 50.0f, from.y});
  f.d.move({from.x + 90.0f, from.y});
  CHECK(f.arr->dragging());
  CHECK(f.d.press("Escape") == KeyResult::Widget);
  CHECK_FALSE(f.arr->dragging());
  f.d.up({from.x + 90.0f, from.y});
  CHECK(f.history() == h1);
  CHECK(f.ctx.doc() == snap1);
}

TEST_CASE("arrangement editing: Alt-drag copies, a vertical drag moves across tracks, edges resize (one undo step each)") {
  Fixture f;
  // Alt-drag copy of Bass A to bar 17 (empty), zoomed out to 40 px per bar (1-bar adaptive cells).
  f.arr->setTimeView(0.0, 40.0 / static_cast<double>(kBar));
  f.d.step();
  const doc::ClipRef bassA = f.clip("Bass", 0);
  const size_t clips0 = f.track("Bass").clips.size();
  const size_t notes0 = f.get(bassA).notes.size();
  size_t h = f.history();
  Vec2 from = f.title(bassA, kBar / 2);
  f.d.drag(from, {from.x + 16.0f * 40.0f, from.y}, 12, mod::kAlt);
  CHECK(f.history() == h + 1);
  REQUIRE(f.track("Bass").clips.size() == clips0 + 1);
  CHECK(f.get(bassA).start == 0);  // the original stays
  const doc::ClipSlot& copy = f.track("Bass").clips.back();
  CHECK(copy.clip->start == 16 * kBar);
  CHECK(copy.id != bassA.clip);
  CHECK(copy.clip->notes.size() == notes0);
  CHECK(f.selected({f.track("Bass").id, copy.id}));  // the copy is selected (Live)
  REQUIRE(f.ctx.undo());
  CHECK(f.track("Bass").clips.size() == clips0);

  // Cross-track: the Drums' Fill (bars 9–10) dragged one row down onto Bass at the same time.
  f.arr->setTimeView(0.0, arrangement_metrics::kDefaultPxPerTick);
  f.d.step();
  const doc::ClipRef fill = f.clip("Drums", 1);
  REQUIRE(f.get(fill).start == 8 * kBar);
  h = f.history();
  from = f.title(fill, kBar / 2);
  const IRect bassLane = f.arr->rowLaneRect(f.row("Bass"));
  f.d.drag(from, {from.x, static_cast<float>(bassLane.y + 7)}, 6);
  CHECK(f.history() == h + 1);
  doc::TrackId on;
  REQUIRE(f.find(fill.clip, &on) != nullptr);
  CHECK(on == f.track("Bass").id);
  CHECK(f.get(fill).start == 8 * kBar);
  CHECK(f.track("Drums").clips.size() == 2);
  REQUIRE(f.ctx.undo());
  REQUIRE(f.find(fill.clip, &on) != nullptr);
  CHECK(on == f.track("Drums").id);

  // A drag onto a group row is not a move target: the clip stays on its track.
  h = f.history();
  const IRect keysLane = f.arr->rowLaneRect(f.row("Keys"));
  const doc::ClipRef bassB = f.clip("Bass", 1);
  from = f.title(bassB, kBar);
  f.d.drag(from, {from.x, static_cast<float>(keysLane.y + 7)}, 6);
  CHECK(f.history() == h);  // no move at all (time unchanged, row rejected)

  // Resize: Bass B's end one bar earlier (1/4 adaptive grid at 96 px / bar).
  h = f.history();
  Vec2 edge{f.arr->xOfTick(static_cast<double>(8 * kBar)) - 2.0f, f.title(bassB, 0).y};
  f.d.drag(edge, {edge.x - 96.0f, edge.y}, 8);
  CHECK(f.history() == h + 1);
  CHECK(f.get(bassB).start == 4 * kBar);
  CHECK(f.get(bassB).length == 3 * kBar);
  // Its start one bar later: notes keep their time (the start marker moves with the edge).
  const Ticks cs0 = f.get(bassB).contentStart;
  edge = {f.arr->xOfTick(static_cast<double>(4 * kBar)) + 2.0f, edge.y};
  f.d.drag(edge, {edge.x + 96.0f, edge.y}, 8);
  CHECK(f.history() == h + 2);
  CHECK(f.get(bassB).start == 5 * kBar);
  CHECK(f.get(bassB).length == 2 * kBar);
  CHECK(f.get(bassB).contentStart == cs0 + kBar);
}

TEST_CASE("arrangement editing: split, duplicate, consolidate, Ctrl+Shift+D (one undo step each)") {
  Fixture f;
  const doc::ClipRef bassA = f.clip("Bass", 0), bassB = f.clip("Bass", 1);
  // Split at the insert marker (a click in the body at bar 3).
  size_t h = f.history();
  f.d.click(f.body(bassA, 2 * kBar + 300));
  CHECK(*f.arr->selection().time == TickRange{2 * kBar, 2 * kBar});
  CHECK(f.d.press("Ctrl+E") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.starts("Bass") == std::vector<Ticks>{0, 2 * kBar, 4 * kBar, 8 * kBar, 12 * kBar});
  CHECK(f.get(bassA).length == 2 * kBar);
  REQUIRE(f.ctx.undo());
  CHECK(f.starts("Bass") == std::vector<Ticks>{0, 4 * kBar, 8 * kBar, 12 * kBar});

  // Split at a time selection's edges (a body drag over bars 2–3 of Bass A).
  h = f.history();
  f.d.drag(f.body(bassA, kBar + 200), f.body(bassA, 3 * kBar + 200), 6);
  REQUIRE(f.arr->selection().range);
  CHECK(*f.arr->selection().time == TickRange{kBar, 3 * kBar});
  f.d.press("Ctrl+E");
  CHECK(f.history() == h + 1);
  CHECK(f.starts("Bass") == std::vector<Ticks>{0, kBar, 3 * kBar, 4 * kBar, 8 * kBar, 12 * kBar});
  REQUIRE(f.ctx.undo());

  // Duplicate: Bass B copied right after itself (over the deactivated clip there).
  h = f.history();
  f.d.click(f.title(bassB, kBar));
  CHECK(f.d.press("Ctrl+D") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.starts("Bass") == std::vector<Ticks>{0, 4 * kBar, 8 * kBar, 12 * kBar});
  const doc::ClipSlot& dup = f.track("Bass").clips[2];
  CHECK(dup.id != bassB.clip);
  CHECK(dup.clip->name == "Bass B");
  CHECK(dup.clip->active);
  CHECK(f.selected({bassB.track, dup.id}));
  REQUIRE(f.ctx.undo());

  // Consolidate Bass A + Bass B into one clip over bars 1–9.
  h = f.history();
  f.d.click(f.title(bassA, kBar));
  f.d.click(f.title(bassB, kBar), mod::kShift);
  CHECK(f.d.press("Ctrl+J") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.starts("Bass") == std::vector<Ticks>{0, 8 * kBar, 12 * kBar});
  CHECK(f.track("Bass").clips[0].clip->length == 8 * kBar);
  CHECK(f.track("Bass").clips[0].clip->notes.size() == 64);
  REQUIRE(f.ctx.undo());

  // Ctrl+Shift+D: a time selection over bars 2–3 duplicated after itself on every track; the copy is selected.
  h = f.history();
  f.d.drag(f.body(bassA, kBar + 200), f.body(bassA, 3 * kBar + 200), 6);
  CHECK(f.d.press("Ctrl+Shift+D") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.get(bassB).start == 6 * kBar);                           // later material moves by the range
  CHECK(f.track("Piano").clips.back().clip->start == 6 * kBar);    // on every track (Just Chords was at bar 5)
  CHECK(*f.arr->selection().time == TickRange{3 * kBar, 5 * kBar});
  CHECK(f.arr->selection().range);
  REQUIRE(f.ctx.undo());
  CHECK(f.get(bassB).start == 4 * kBar);
}

TEST_CASE("arrangement editing: delete, delete / insert time, Ctrl+L, Ctrl+Shift+M, 0, clipboard (one undo step each)") {
  Fixture f;
  const doc::ClipRef bassA = f.clip("Bass", 0), bassB = f.clip("Bass", 1);
  // Delete a clip.
  size_t h = f.history();
  f.d.click(f.title(bassB, kBar));
  CHECK(f.d.press("Delete") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.find(bassB.clip) == nullptr);
  CHECK(f.arr->selection().clips.empty());
  REQUIRE(f.ctx.undo());
  // Delete inside a time selection: the clip is cut around it.
  h = f.history();
  f.d.drag(f.body(bassA, kBar + 200), f.body(bassA, 3 * kBar + 200), 6);
  f.d.press("Delete");
  CHECK(f.history() == h + 1);
  CHECK(f.starts("Bass") == std::vector<Ticks>{0, 3 * kBar, 4 * kBar, 8 * kBar, 12 * kBar});
  CHECK(f.get(bassA).length == kBar);
  REQUIRE(f.ctx.undo());

  // Delete time (Ctrl+Shift+Delete) and insert time (Ctrl+I) over bars 2–3.
  f.d.drag(f.body(bassA, kBar + 200), f.body(bassA, 3 * kBar + 200), 6);
  h = f.history();
  CHECK(f.d.press("Ctrl+Shift+Delete") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.get(bassB).start == 2 * kBar);
  CHECK(*f.arr->selection().time == TickRange{kBar, kBar});
  REQUIRE(f.ctx.undo());
  f.d.drag(f.body(bassA, kBar + 200), f.body(bassA, 3 * kBar + 200), 6);
  h = f.history();
  CHECK(f.d.press("Ctrl+I") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.get(bassB).start == 6 * kBar);
  REQUIRE(f.ctx.undo());

  // Ctrl+L: the arrangement loop = the time selection.
  f.d.drag(f.body(bassA, kBar + 200), f.body(bassA, 3 * kBar + 200), 6);
  h = f.history();
  CHECK(f.d.press("Ctrl+L") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.ctx.doc()->loop.start == kBar);
  CHECK(f.ctx.doc()->loop.end == 3 * kBar);
  CHECK(f.ctx.doc()->loop.enabled);

  // Ctrl+Shift+M: at the insert marker one bar, over a time selection exactly it.
  const size_t strings0 = f.track("Strings").clips.size();
  f.d.click(f.lane("Strings", kBar));
  h = f.history();
  CHECK(f.d.press("Ctrl+Shift+M") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  REQUIRE(f.track("Strings").clips.size() == strings0 + 1);
  CHECK(f.track("Strings").clips[0].clip->start == kBar);
  CHECK(f.track("Strings").clips[0].clip->length == kBar);
  CHECK(f.track("Strings").clips[0].clip->notes.size() == 0);
  REQUIRE(f.ctx.undo());
  f.d.drag(f.lane("Strings", 200), f.lane("Strings", 3 * kBar + 200), 6);
  f.d.press("Ctrl+Shift+M");
  REQUIRE(f.track("Strings").clips.size() == strings0 + 1);
  CHECK(f.track("Strings").clips[0].clip->length == 3 * kBar);
  REQUIRE(f.ctx.undo());

  // 0 (Q15): deactivates the selected clip, again reactivates it; with a track selected, its activator, which is the
  // track's mute since T3.10 (Q15's answer: the activator mutes, a Param; it was SetTrackActive, a recompile).
  f.d.click(f.title(bassA, kBar));
  h = f.history();
  CHECK(f.d.press("0") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK_FALSE(f.get(bassA).active);
  f.d.press("0");
  CHECK(f.history() == h + 2);
  CHECK(f.get(bassA).active);
  const IRect hdr = f.d.rect(std::string(kArr) + "/tracks/track[1]");  // Bass
  f.d.click({static_cast<float>(hdr.x + 40), static_cast<float>(hdr.y + 5)});
  f.d.press("0");
  CHECK(f.history() == h + 3);
  CHECK(f.track("Bass").mixer.mute);
  CHECK(f.track("Bass").active);
  CHECK(f.ctx.store().history().entries()[f.history() - 1].changes.paramOnly());
  REQUIRE(f.ctx.undo());
  CHECK_FALSE(f.track("Bass").mixer.mute);

  // Clipboard: copy Bass A, paste on Strings at bar 1 (twice: the second goes after the first), cut.
  f.d.click(f.title(bassA, kBar));
  h = f.history();
  CHECK(f.d.press("Ctrl+C") == KeyResult::Action);
  CHECK(f.history() == h);
  f.d.click(f.lane("Strings", 0));
  CHECK(f.d.press("Ctrl+V") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  REQUIRE(f.track("Strings").clips.size() == strings0 + 1);
  const doc::NoteClip& pasted = *f.track("Strings").clips[0].clip;
  CHECK(pasted.start == 0);
  CHECK(pasted.length == 4 * kBar);
  CHECK(pasted.notes.size() == f.get(bassA).notes.size());
  CHECK(pasted.id != bassA.clip);
  CHECK(pasted.notes.begin()->id != f.get(bassA).notes.begin()->id);  // fresh note ids
  f.d.press("Ctrl+V");
  CHECK(f.history() == h + 2);
  CHECK(f.starts("Strings") == std::vector<Ticks>{0, 4 * kBar, 8 * kBar});  // the second overwrote Pad 31's start
  h = f.history();
  f.d.click(f.title(bassB, kBar));
  CHECK(f.d.press("Ctrl+X") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.find(bassB.clip) == nullptr);
  f.arr->setTimeView(static_cast<double>(10 * kBar), f.arr->pxPerTick());
  f.d.step();
  f.d.click(f.lane("Bass", 16 * kBar));
  f.d.press("Ctrl+V");
  CHECK(f.history() == h + 2);
  CHECK(f.starts("Bass").back() == 16 * kBar);
}

TEST_CASE("arrangement editing: the right-click grid menu (every item) and Ctrl+1–5 edit the grid, not the document") {
  Fixture f;
  const size_t h0 = f.history();
  const Vec2 p = f.lane("Strings", kBar);
  // Opens the menu, highlights `top` (a submenu when `sub` is non-empty) and chooses the item labelled `label`.
  auto choose = [&](std::string_view top, std::string_view sub = {}) {
    f.d.click(p, mod::kNone, PointerButton::Right);
    OverlayLayer* layer = OverlayLayer::of(*f.arr);
    REQUIRE(layer != nullptr);
    auto* menu = dynamic_cast<Menu*>(layer->top());
    REQUIRE(menu != nullptr);
    auto index = [](const Menu& m, std::string_view label) {
      for (size_t i = 0; i < m.items().size(); ++i)
        if (m.items()[i].label == label) return static_cast<int32_t>(i);
      FAIL("no menu item " << label);
      return -1;
    };
    const int32_t i = index(*menu, top);
    if (sub.empty()) {
      menu->choose(i);
    } else {
      menu->setHighlighted(i);
      Menu* s = menu->openSubmenu();
      REQUIRE(s != nullptr);
      s->choose(index(*s, sub));
    }
    f.d.step();
    CHECK(layer->openCount() == 0);
  };
  for (int l = kGridLevelMin; l <= kGridLevelMax; ++l) {
    choose("Adaptive Grid", adaptiveLevelName(l));
    CHECK(f.ctx.grid().mode == GridMode::Adaptive);
    CHECK(f.ctx.grid().level == l);
    CHECK_FALSE(f.ctx.grid().off);
  }
  for (int x = kGridFixedExpMin; x <= kGridFixedExpMax; ++x) {
    choose("Fixed Grid", fixedValueName(x));
    CHECK(f.ctx.grid().mode == GridMode::Fixed);
    CHECK(f.ctx.grid().fixedExp == x);
  }
  for (const auto& [n, label] : {std::pair{3, "Triplet"}, std::pair{5, "5-Tuplet"}, std::pair{7, "7-Tuplet"},
                                 std::pair{11, "11-Tuplet"}, std::pair{1, "Straight"}}) {
    choose(label);
    CHECK(f.ctx.grid().tuplet == n);
  }
  const bool snap0 = f.ctx.grid().snap;
  choose("Snap to Grid");
  CHECK(f.ctx.grid().snap == !snap0);
  choose("Snap to Grid");
  CHECK(f.ctx.grid().snap == snap0);
  choose("Off");
  CHECK(f.ctx.grid().off);
  // The menu shows the state: Off checked now.
  f.d.click(p, mod::kNone, PointerButton::Right);
  auto* menu = dynamic_cast<Menu*>(OverlayLayer::of(*f.arr)->top());
  REQUIRE(menu != nullptr);
  bool offChecked = false;
  for (const MenuItem& it : menu->items())
    if (it.label == "Off") offChecked = it.checked;
  CHECK(offChecked);
  OverlayLayer::of(*f.arr)->closeAll();
  f.d.step();
  choose("Off");
  CHECK_FALSE(f.ctx.grid().off);
  // The menu's edit items run the arrangement's actions (Delete on a selected clip).
  const doc::ClipRef bassA = f.clip("Bass", 0);
  f.d.click(f.title(bassA, kBar));
  f.d.click(f.title(bassA, kBar), mod::kNone, PointerButton::Right);
  auto* m2 = dynamic_cast<Menu*>(OverlayLayer::of(*f.arr)->top());
  REQUIRE(m2 != nullptr);
  CHECK(f.history() == h0);  // the grid is view state
  for (size_t i = 0; i < m2->items().size(); ++i)
    if (m2->items()[i].label == "Delete") m2->choose(static_cast<int32_t>(i));
  f.d.step();
  CHECK(f.history() == h0 + 1);
  CHECK(f.find(bassA.clip) == nullptr);
  REQUIRE(f.ctx.undo());

  // Ctrl+1–5 with the arrangement focused.
  f.d.tree().setFocus(f.arr);
  GridSpec g = f.ctx.grid();
  f.d.press("Ctrl+1");
  CHECK(f.ctx.grid() == g.narrowed());
  g = f.ctx.grid();
  f.d.press("Ctrl+2");
  CHECK(f.ctx.grid() == g.widened());
  g = f.ctx.grid();
  f.d.press("Ctrl+3");
  CHECK(f.ctx.grid() == g.tripletToggled());
  g = f.ctx.grid();
  f.d.press("Ctrl+4");
  CHECK(f.ctx.grid() == g.snapToggled());
  g = f.ctx.grid();
  f.d.press("Ctrl+5");
  CHECK(f.ctx.grid() == g.modeToggled());
  CHECK(f.history() == h0);
}

TEST_CASE("arrangement editing: double-click opens the clip in the detail view and focuses the clip view") {
  Fixture f;
  f.ctx.setDetailVisible(false);
  f.ctx.setDetailMode(DetailMode::Device);
  const doc::ClipRef bassB = f.clip("Bass", 1);
  f.d.doubleClick(f.title(bassB, kBar));
  CHECK(f.ctx.detailVisible());
  CHECK(f.ctx.detailMode() == DetailMode::Clip);
  CHECK(f.ctx.detailClip().clip == bassB.clip);
  Widget* notes = f.d.find("main/body/bottom/detail/clip/clip-view/notes");
  REQUIRE(notes != nullptr);
  CHECK(f.d.tree().focus() == notes);
}

TEST_CASE("arrangement editing: at 150 % UI scale a title drag moves by grid cells and an edge drag resizes") {
  FakeTransport port;
  ViewContext ctx(port);
  REQUIRE(ctx.openProject(views::test::fixtureDir("arrangement")).ok());
  TestDriver d(1280, 800, 1.5f);
  auto shell = Shell::create(d.tree(), ctx);
  REQUIRE(shell.ok());
  d.tree().layoutNow();
  auto* arr = dynamic_cast<ArrangementView*>(d.find(kArr));
  REQUIRE(arr != nullptr);
  const doc::Track& bassT = *ctx.doc()->tracks[1];
  REQUIRE(bassT.name == "Bass");
  const doc::ClipRef bassB{bassT.id, bassT.clips[1].id};
  const IRect box = arr->clipRect(bassB);
  REQUIRE_FALSE(box.empty());
  const float beatPx = static_cast<float>(kPpq * arr->devicePxPerTick());  // the adaptive 1/4 grid at this zoom
  const Vec2 grab{static_cast<float>(box.x) + 40.0f, static_cast<float>(box.y) + 9.0f};  // title strip (21 device px)
  d.drag(grab, {grab.x + 2.0f * beatPx + 3.0f, grab.y}, 6);
  auto start = [&] {
    const doc::Track& t = *ctx.doc()->tracks[1];
    return t.clips[static_cast<size_t>(doc::findClipIndex(t, bassB.clip))].clip->start;
  };
  CHECK(start() == 4 * kBar + 2 * kPpq);
  // Bass A's end (bar 5, visible: Bass B's end is past the view at this scale) one beat earlier.
  const doc::ClipRef bassA{bassT.id, bassT.clips[0].id};
  const IRect a = arr->clipRect(bassA);
  REQUIRE(a.right() < arr->lanesViewport().right());
  const Vec2 edge{static_cast<float>(a.right()) - 2.0f, grab.y};  // inside the 6-device-px end zone
  d.drag(edge, {edge.x - beatPx, edge.y}, 6);
  const doc::Track& t = *ctx.doc()->tracks[1];
  CHECK(t.clips[static_cast<size_t>(doc::findClipIndex(t, bassA.clip))].clip->length == 4 * kBar - kPpq);
  CHECK(ctx.store().history().cursor() == 2);
  d.tree().setRoot(std::make_unique<Widget>());
}

TEST_CASE("arrangement editing: a drag frame damages only the dragged clip's old and new rects") {
  Fixture f;
  f.d.step();
  const doc::ClipRef bassB = f.clip("Bass", 1);
  f.d.click(f.title(bassB, kBar));
  f.d.step();
  const Vec2 from = f.title(bassB, kBar);
  f.d.move(from);
  f.d.down(from);
  f.d.move({from.x + 30.0f, from.y});  // first frame: the original's columns and the time overlay go
  f.d.step();
  const IRect lv = f.arr->lanesViewport();
  const IRect lane = f.arr->rowLaneRect(f.row("Bass"));
  const int64_t clipArea = int64_t{4} * 96 * lane.h;  // a 4-bar clip at 96 px per bar
  for (int i = 2; i <= 10; ++i) {
    f.d.move({from.x + 30.0f + 24.0f * static_cast<float>(i), from.y});
    const Damage& dm = f.d.tree().damage();
    REQUIRE_FALSE(dm.full());
    int64_t area = 0;
    for (const ClipRect& r : dm.rects()) {
      CHECK(r.x0 >= lv.x);
      CHECK(r.x1 <= lv.right());
      CHECK(r.y0 >= lane.y);
      CHECK(r.y1 <= lane.bottom());
      area += static_cast<int64_t>(r.x1 - r.x0) * (r.y1 - r.y0);
    }
    CHECK(area > 0);
    CHECK(area <= 2 * clipArea + 8 * lane.h);  // old + new rect (they overlap: usually less)
    f.d.step();
  }
  f.d.up({from.x + 270.0f, from.y});
}

TEST_CASE("render: arrangement drags repaint only damage, and equal a full repaint at every step") {
  GpuOptions go;
  go.validation = true;
  auto gpu = GpuContext::create(go);
  REQUIRE_MESSAGE(gpu.ok(), (gpu.ok() ? "" : gpu.error().message));
  auto renderer = Renderer::create(**gpu, OffscreenTarget::kFormat, 3);
  REQUIRE(renderer.ok());
  auto tex = createSceneTextures(**renderer);
  REQUIRE(tex.ok());
  const PaintResources res = paintResources(*tex);
  constexpr uint32_t W = 1280, H = 800;

  FakeTransport port;
  ViewContext ctx(port);
  REQUIRE(ctx.openProject(views::test::fixtureDir("arrangement")).ok());
  UiTree tree;
  tree.setViewport(W, H);
  tree.setResources(&res);
  auto shell = Shell::create(tree, ctx);
  REQUIRE(shell.ok());
  tree.layoutNow();
  auto* arr = dynamic_cast<ArrangementView*>(tree.find(kArr));
  REQUIRE(arr != nullptr);

  auto partial = OffscreenTarget::create(**gpu, W, H, 1);
  auto full = OffscreenTarget::create(**gpu, W, H, 1);
  REQUIRE((partial.ok() && full.ok()));
  DrawList dl;
  const DrawLayer layer{&dl, kTransientLayer, 0};
  FrameDesc ff;
  ff.layers = {&layer, 1};
  ff.clear = kSceneClearColor;
  double now = 0.0;
  // Settle (note previews are built over the first frames), then one full paint into the partial target.
  for (int i = 0; i < 64 && (tree.needsFrame() || i == 0); ++i) {
    tree.update(now += 1.0 / 144.0);
    tex->text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, res, now);
  }
  REQUIRE((*partial)->draw(**renderer, ff).ok());

  uint64_t partialFrames = 0;
  auto frame = [&] {
    now += 1.0 / 144.0;
    tree.update(now);
    const Damage& d = tree.damage();
    if (!d.needsFrame()) return;
    tex->text->beginFrame();
    dl.reset(W, H);
    if (d.full()) {
      tree.paint(dl, res, now);
      REQUIRE((*partial)->draw(**renderer, ff).ok());
      return;
    }
    ClipRect rects[Damage::kMaxRects];
    uint32_t n = 0;
    ClipRect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (const ClipRect& r : d.rects()) {
      rects[n++] = r;
      box = Damage::unite(box, r);
    }
    tree.paint(dl, res, now, {box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0});
    FrameDesc pf = ff;
    pf.full = false;
    pf.damage = {rects, n};
    REQUIRE((*partial)->draw(**renderer, pf).ok());
    ++partialFrames;
  };
  auto compare = [&](const std::string& what) {
    tex->text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, res, now);
    REQUIRE((*full)->draw(**renderer, ff).ok());
    auto a = (*partial)->read();
    auto b = (*full)->read();
    REQUIRE((a.ok() && b.ok()));
    const ImageDiff diff = compareImages(*a, *b, 0);
    if (diff.pixelsOverTolerance != 0) {
      (void)writePng(what + ".partial.png", *a);
      (void)writePng(what + ".full.png", *b);
    }
    CHECK_MESSAGE(diff.pixelsOverTolerance == 0, what << ": " << diff.pixelsOverTolerance << " px differ");
  };
  auto pointer = [&](PointerType t, Vec2 p, Mods m = mod::kNone) {
    PointerEvent e;
    e.type = t;
    e.pos = p;
    e.mods = m;
    e.button = (t == PointerType::Down || t == PointerType::Up) ? PointerButton::Left : PointerButton::None;
    e.clickCount = 1;
    tree.dispatchPointer(e);
    frame();
  };
  auto rowNamed = [&](std::string_view n) {
    const auto rows = arr->rows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].track->name == n) return static_cast<int32_t>(i);
    return -1;
  };
  const IRect bass = arr->rowLaneRect(rowNamed("Bass"));
  const IRect piano = arr->rowLaneRect(rowNamed("Piano"));
  const IRect strings = arr->rowLaneRect(rowNamed("Strings"));
  auto x = [&](double bars) { return arr->xOfTick(bars * static_cast<double>(kBar)) + 0.5f; };
  struct Gesture {
    const char* what;
    Vec2 from;
    Vec2 step;  // per move
    Mods mods;
  };
  // Range (rubber band) over Piano … Strings; move Bass B; Alt-copy it a row up; resize its end; cross-track move.
  const Gesture gestures[] = {
      {"range", {x(4.1), static_cast<float>(piano.bottom() - 8)}, {14.0f, 6.0f}, mod::kNone},
      {"move", {x(5.0), static_cast<float>(bass.y + 7)}, {9.0f, 0.0f}, mod::kNone},
      {"copy", {x(5.5), static_cast<float>(bass.y + 7)}, {-7.0f, -4.0f}, mod::kAlt},
      {"resize", {x(8.0) - 3.0f, static_cast<float>(bass.y + 7)}, {-8.0f, 0.0f}, mod::kNone},
      {"cross-track", {x(4.5), static_cast<float>(piano.y + 7)}, {5.0f, 7.0f}, mod::kNone},
  };
  (void)strings;
  for (const Gesture& g : gestures) {
    pointer(PointerType::Move, g.from, g.mods);
    pointer(PointerType::Down, g.from, g.mods);
    for (int i = 1; i <= 12; ++i)
      pointer(PointerType::Move, {g.from.x + g.step.x * static_cast<float>(i), g.from.y + g.step.y * static_cast<float>(i)}, g.mods);
    compare(std::string(g.what) + "-mid");
    pointer(PointerType::Up, {g.from.x + g.step.x * 12.0f, g.from.y + g.step.y * 12.0f}, g.mods);
    compare(std::string(g.what) + "-done");
  }
  // A selection change by clicks (selected rows' tint, the outline, the marker).
  pointer(PointerType::Down, {x(1.3), static_cast<float>(strings.bottom() - 8)});
  pointer(PointerType::Up, {x(1.3), static_cast<float>(strings.bottom() - 8)});
  compare("marker");
  CHECK(partialFrames > 30);
  CHECK((*gpu)->validationErrorCount() == 0);
  tree.setRoot(std::make_unique<Widget>());
}

TEST_CASE("render: arrangement-selection over tests/views/fixtures/arrangement matches the golden image") {
  constexpr const char* scene = "arrangement-selection";
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, views::test::fixtureDir("arrangement"), w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(d));
}

TEST_CASE("arrangement editing: the arrangement-selection scene is a range selection and an Alt-drag in progress") {
  FakeTransport port;
  ViewContext ctx(port);
  REQUIRE(ctx.openProject(views::test::fixtureDir("arrangement")).ok());
  TestDriver d(1280, 800);
  auto shell = buildViewScene("arrangement-selection", d.tree(), ctx);
  REQUIRE_MESSAGE(shell.ok(), (shell.ok() ? "" : shell.error().message));
  auto* arr = dynamic_cast<ArrangementView*>(d.find(kArr));
  REQUIRE(arr != nullptr);
  const ArrangementView::Selection& s = arr->selection();
  // The band over Piano … Strings, bars 5–7, selected the clips it touches.
  REQUIRE(s.time.has_value());
  CHECK(*s.time == TickRange{4 * kBar, 6 * kBar});
  CHECK(s.range);
  REQUIRE(s.tracks.size() == 3);
  std::vector<std::string> names;
  for (const doc::ClipRef& r : s.clips) {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), r.track);
    REQUIRE(t != nullptr);
    names.push_back(t->name + "/" + t->clips[static_cast<size_t>(doc::findClipIndex(*t, r.clip))].clip->name);
  }
  std::sort(names.begin(), names.end());
  CHECK(names == std::vector<std::string>{"Piano/Just Chords", "Strings/Pad 31"});
  // The Alt-drag is still running; nothing dispatched. Releasing it copies both clips two rows up, two bars later.
  REQUIRE(arr->dragging());
  const size_t h = ctx.store().history().cursor();
  d.up({arr->xOfTick(6.5 * static_cast<double>(kBar)), static_cast<float>(arr->rowLaneRect(1).y + 7)}, PointerButton::Left, mod::kAlt);
  CHECK(ctx.store().history().cursor() == h + 1);
  auto startsOn = [&](std::string_view name) {
    std::vector<std::pair<Ticks, std::string>> v;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (t.name == name)
        for (const doc::ClipSlot& c : t.clips) v.emplace_back(c.clip->start, c.clip->name);
    });
    return v;
  };
  const auto bass = startsOn("Bass");
  CHECK(std::find(bass.begin(), bass.end(), std::pair<Ticks, std::string>{6 * kBar, "Just Chords"}) != bass.end());
  const auto piano = startsOn("Piano");
  CHECK(std::find(piano.begin(), piano.end(), std::pair<Ticks, std::string>{6 * kBar, "Pad 31"}) != piano.end());
  CHECK(std::find(piano.begin(), piano.end(), std::pair<Ticks, std::string>{4 * kBar, "Just Chords"}) != piano.end());
  d.tree().setRoot(std::make_unique<Widget>());
}
