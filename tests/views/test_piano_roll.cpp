// T3.11 piano roll: the row model (EDO rows, equave markers, Q2 labels, placement of off-grid pitches, fold, the
// default highlight scale), the clip view scripted through the real shell and key routing (B-mode draw on a 5-tuplet
// grid lands on exact ticks; move / resize / Alt-copy / delete / velocity drag / Ctrl+U; Up on a 31edo Degree note
// = +1\31; Up on 5/4 → 6/4 → 7/4; Shift+Up = one equave; every gesture one undo step), audition through the port,
// partial redraw while dragging, and the three goldens.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <ostream>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/ui/damage.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/piano_roll/row_model.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using pq::tuning::PitchId;
using pq::tuning::Rational;

namespace {

tuning::Tuning edoTuning(uint32_t n) {
  auto e = tuning::Edo::make(n);
  REQUIRE(e.ok());
  auto t = tuning::Tuning::make(*e);
  REQUIRE(t.ok());
  return *t;
}

// The shell over a fixture project with its first clip in the clip view and the detail view raised.
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
    view = dynamic_cast<ClipView*>(d.find("main/body/bottom/detail/clip/clip-view"));
    REQUIRE(view != nullptr);
    REQUIRE(view->visible());
    REQUIRE(view->hasClip());
    port.calls.clear();
  }
  ~Roll() { d.tree().setRoot(std::make_unique<Widget>()); }

  const doc::NoteClip& clip() const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), ref.track);
    REQUIRE(t != nullptr);
    const int32_t i = doc::findClipIndex(*t, ref.clip);
    REQUIRE(i >= 0);
    return *t->clips[static_cast<size_t>(i)].clip;
  }
  const doc::Note* note(uint32_t id) const { return clip().notes.findById(doc::NoteId{id}); }
  std::vector<doc::Note> notes() const { return {clip().notes.begin(), clip().notes.end()}; }
  size_t history() const { return ctx.store().history().cursor(); }
  Vec2 centreOf(uint32_t id) {
    const IRect r = view->noteRect(doc::NoteId{id});
    REQUIRE_FALSE(r.empty());
    return {static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f, static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f};
  }
  // A point in the note area at content tick t on the row of absolute degree `degree`.
  Vec2 at(double t, int64_t degree) const { return {view->xOfTick(t), view->yOfDegree(degree)}; }
  void setGrid(int fixedExp, int tuplet) {
    GridSpec g;
    g.mode = GridMode::Fixed;
    g.fixedExp = static_cast<int8_t>(fixedExp);
    g.tuplet = static_cast<uint8_t>(tuplet);
    ctx.setGrid(g);
  }
};

constexpr Ticks kQ = kPpq;
constexpr Ticks kSixteenth = kPpq / 4;

}  // namespace

// ---- row model ------------------------------------------------------------------------------------------

TEST_CASE("row model: 12edo rows, equave markers, C-style labels (Q2), placement of off-grid pitches") {
  const tuning::Tuning t = edoTuning(12);
  roll::RowOptions o;
  o.rowPx = 10;
  o.lowCents = -1200.0;
  o.highCents = 1200.0;
  const roll::RowModel m = roll::RowModel::build(t, o);
  REQUIRE(m.size() == 25);  // degrees −12 … 12
  CHECK(m.row(0).degree == 12);
  CHECK(m.row(24).degree == -12);
  CHECK(m.contentHeight() == 250);
  for (size_t i = 0; i < m.size(); ++i) {
    CHECK(m.row(i).top == static_cast<int32_t>(i) * 10);
    CHECK(m.row(i).height() == 10);
  }
  const int64_t c3 = m.indexOfDegree(0);
  REQUIRE(c3 == 12);
  CHECK((m.row(12).flags & roll::kRowEquaveStart) != 0);
  CHECK((m.row(12).flags & roll::kRowNatural) != 0);
  CHECK(m.label(12) == "C3");
  CHECK(m.label(m.indexOfDegree(12)) == "C4");
  CHECK(m.label(m.indexOfDegree(1)) == "C#3");
  CHECK(m.label(m.indexOfDegree(-1)) == "B2");
  CHECK((m.row(m.indexOfDegree(1)).flags & roll::kRowNatural) == 0);
  CHECK(m.indexOfDegree(13) == -1);
  // 5/4 = 386.31 ¢: 13.69 ¢ below E (400 ¢), so 86.3 % of the way from D# up to E.
  const roll::CentsPlacement p = m.placeCents(tuning::cents(Rational(5, 4)));
  CHECK(p.nearestDegree == 4);
  CHECK(p.offsetCents == doctest::Approx(-13.686).epsilon(1e-4));
  const int32_t e = m.row(m.indexOfDegree(4)).centre, dsharp = m.row(m.indexOfDegree(3)).centre;
  CHECK(p.centre == static_cast<int32_t>(std::lround(dsharp + (e - dsharp) * 0.86314)));
  CHECK(p.bottom - p.top == 10);
  // A pitch on a row is on it.
  CHECK(std::fabs(m.placeCents(700.0).offsetCents) < roll::kOnGridToleranceCents);
  CHECK(m.indexAtY(0) == 0);
  CHECK(m.indexAtY(125) == 12);
  CHECK(m.indexAtY(-50) == 0);
  CHECK(m.indexAtY(10000) == 24);
  const auto [a, b] = m.rowsIn(15, 35);
  CHECK(a == 1);
  CHECK(b == 4);
}

TEST_CASE("row model: 31edo labels k\\31 with the equave number on degree 0; diatonic highlight scale; fold") {
  const tuning::Tuning t = edoTuning(31);
  CHECK(roll::twelveCompatibleEdo(t) == 0);
  CHECK(roll::twelveCompatibleEdo(edoTuning(24)) == 24);
  const std::vector<uint8_t> sc = roll::defaultHighlightScale(t);
  std::vector<int> in;
  for (size_t i = 0; i < sc.size(); ++i)
    if (sc[i]) in.push_back(static_cast<int>(i));
  CHECK(in == std::vector<int>{0, 5, 10, 13, 18, 23, 28});
  const std::vector<uint8_t> major = roll::defaultHighlightScale(edoTuning(12));
  CHECK(major == std::vector<uint8_t>{1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1});
  CHECK(roll::defaultHighlightScale(edoTuning(5)) == std::vector<uint8_t>(5, 1));

  roll::RowOptions o;
  o.rowPx = 8;
  o.lowCents = 0.0;
  o.highCents = 1200.0;
  const roll::RowModel m = roll::RowModel::build(t, o);
  CHECK(m.label(m.indexOfDegree(7)) == "7\\31");
  CHECK(m.label(m.indexOfDegree(0)) == "0\\31");
  CHECK(m.equaveLabel(m.indexOfDegree(0)) == "3");   // octave EDOs number equaves like Live's octaves
  CHECK(m.equaveLabel(m.indexOfDegree(31)) == "4");
  CHECK(m.equaveLabel(m.indexOfDegree(7)).empty());
  CHECK((m.row(m.indexOfDegree(18)).flags & roll::kRowNatural) != 0);
  CHECK((m.row(m.indexOfDegree(19)).flags & roll::kRowNatural) == 0);

  // Fold to used notes: exactly the used rows, packed, in pitch order; a hidden degree is placed between them.
  const std::vector<int64_t> used{-31, 0, 10, 18};
  o.fold = roll::Fold::UsedNotes;
  o.used = used;
  const roll::RowModel f = roll::RowModel::build(t, o);
  REQUIRE(f.size() == 4);
  CHECK(f.row(0).degree == 18);
  CHECK(f.row(3).degree == -31);
  CHECK(f.row(3).top == 24);
  CHECK(f.indexOfDegree(10) == 1);
  CHECK(f.indexOfDegree(11) == -1);
  const roll::CentsPlacement p = f.placeCents(t.edo()->centsOfStep(14));  // between rows 10 and 18
  CHECK(p.centre > f.row(0).centre);
  CHECK(p.centre < f.row(1).centre);
  // Fold to scale (over 0 … 1200 ¢ now that no used degree extends the range).
  o.fold = roll::Fold::Scale;
  o.used = {};
  o.scale = sc;
  const roll::RowModel fs = roll::RowModel::build(t, o);
  for (const roll::PitchRow& r : fs.rows()) CHECK(sc[static_cast<size_t>(((r.degree % 31) + 31) % 31)] != 0);
  CHECK(fs.size() == 8);  // 0 … 31 in scale: 7 + the octave
}

// ---- clip view: editing ---------------------------------------------------------------------------------

TEST_CASE("clip view: B-mode draw on a 5-tuplet grid lands on exact ticks; a drawn run is one undo step") {
  Roll r("pianoroll-12edo");
  r.setGrid(4, 5);  // 1/16 quintuplets: 5 lines per quarter
  constexpr Ticks kCell = kQ / 5;
  static_assert(kCell * 5 == kQ);
  const Grid g = r.view->grid();
  CHECK(g.lineAfter(0) == kCell);
  CHECK(r.d.press("B") == KeyResult::Action);
  CHECK(r.ctx.drawMode());
  const size_t n0 = r.clip().notes.size(), h0 = r.history();
  // Click inside the 4th cell of beat 3 (degree 2 = D3, a free row there).
  const Ticks inCell = 2 * kQ + 3 * kCell + kCell / 4;
  r.d.click(r.at(static_cast<double>(inCell), 2));
  REQUIRE(r.clip().notes.size() == n0 + 1);
  CHECK(r.history() == h0 + 1);
  const doc::Note* drawn = nullptr;
  for (const doc::Note& n : r.clip().notes)
    if (n.pitch == PitchId::fromDegree(2, 0) && n.start > 2 * kQ && n.start < 3 * kQ) drawn = &n;
  REQUIRE(drawn != nullptr);
  CHECK(drawn->start == 2 * kQ + 3 * kCell);
  CHECK(drawn->length == kCell);
  CHECK(drawn->start % kCell == 0);
  // Audition of the drawn note through the port.
  REQUIRE(r.port.count(FakeTransport::Call::Audition) >= 1);
  CHECK(r.port.calls.back().hz == doctest::Approx(261.626 * std::exp2(2.0 / 12.0)).epsilon(1e-5));

  // A drag across three cells on the D#3 row: three notes, one undo step.
  const size_t n1 = r.clip().notes.size(), h1 = r.history();
  r.d.drag(r.at(4 * kQ + 100.0, 3), r.at(4 * kQ + 2 * kCell + 100.0, 3), 8);
  CHECK(r.clip().notes.size() == n1 + 3);
  CHECK(r.history() == h1 + 1);
  std::vector<Ticks> starts;
  for (const doc::Note& n : r.clip().notes)
    if (n.pitch == PitchId::fromDegree(3, 0) && n.start >= 4 * kQ && n.start < 5 * kQ) starts.push_back(n.start);
  CHECK(starts == std::vector<Ticks>{4 * kQ, 4 * kQ + kCell, 4 * kQ + 2 * kCell});
  REQUIRE(r.ctx.undo());
  CHECK(r.clip().notes.size() == n1);

  // Draw mode click on a note deletes it.
  const size_t n2 = r.clip().notes.size();
  r.d.click(r.centreOf(100));
  CHECK(r.clip().notes.size() == n2 - 1);
  CHECK(r.note(100) == nullptr);
  CHECK(r.d.press("B") == KeyResult::Action);
  CHECK_FALSE(r.ctx.drawMode());
}

TEST_CASE("clip view: move, resize, Alt-drag copy, delete, rubber band — each gesture one undo step") {
  Roll r("pianoroll-12edo");
  r.setGrid(4, 1);  // 1/16
  const doc::Note n100 = *r.note(100);  // C3, 0 … 2 beats − 1/16
  REQUIRE(n100.pitch == PitchId::fromDegree(0, 0));

  // Move: one beat later, two rows up (C3 → D3).
  size_t h = r.history();
  Vec2 from = r.centreOf(100);
  Vec2 to{from.x + static_cast<float>(kQ * r.view->pxPerTick()), from.y - 2.0f * static_cast<float>(r.view->rowHeight())};
  r.d.drag(from, to, 6);
  CHECK(r.history() == h + 1);
  REQUIRE(r.note(100) != nullptr);
  CHECK(r.note(100)->start == kQ);
  CHECK(r.note(100)->pitch == PitchId::fromDegree(2, 0));
  CHECK(r.note(100)->length == n100.length);
  CHECK(r.view->selection().size() == 1);

  // Resize the end one beat longer (relative snap from the note's end, which is on the 1/16 grid).
  h = r.history();
  const IRect nr = r.view->noteRect(doc::NoteId{100});
  from = {static_cast<float>(nr.right()) - 2.0f, static_cast<float>(nr.y) + static_cast<float>(nr.h) * 0.5f};
  to = {from.x + static_cast<float>(kQ * r.view->pxPerTick()) + 3.0f, from.y};
  r.d.drag(from, to, 5);
  CHECK(r.history() == h + 1);
  CHECK(r.note(100)->start == kQ);
  CHECK(r.note(100)->length == n100.length + kQ);

  // Alt-drag copy: the original stays, the copy lands two beats later on the same row.
  h = r.history();
  const size_t count = r.clip().notes.size();
  from = r.centreOf(100);
  to = {from.x + static_cast<float>(2 * kQ * r.view->pxPerTick()), from.y};
  r.d.drag(from, to, 6, mod::kAlt);
  CHECK(r.history() == h + 1);
  CHECK(r.clip().notes.size() == count + 1);
  CHECK(r.note(100)->start == kQ);
  int copies = 0;
  for (const doc::Note& n : r.clip().notes)
    if (n.id.value != 100 && n.pitch == PitchId::fromDegree(2, 0) && n.start == 3 * kQ && n.length == n100.length + kQ) ++copies;
  CHECK(copies == 1);
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.view->selection()[0] != 100);  // the copy is selected

  // Delete the copy with the Delete key (pianoroll scope: focus is in the note area after the drag).
  h = r.history();
  CHECK(r.d.press("Delete") == KeyResult::Action);
  CHECK(r.history() == h + 1);
  CHECK(r.clip().notes.size() == count);
  CHECK(r.view->selection().empty());

  // Rubber band over the bass notes of beats 1–2 (C2 row) selects only them.
  const Vec2 a = r.at(-0.25 * static_cast<double>(kQ), -12), b = r.at(3.5 * static_cast<double>(kQ), -12);
  r.d.drag({a.x + 20.0f, a.y - 3.0f}, {b.x, b.y + 3.0f}, 5);
  // Bass notes: C2 at 0 (id 103) and A1 at 2q (id 107) are on rows −12 and −15; only C2 is inside.
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.view->selection()[0] == 103);
  CHECK(r.history() == h + 1);  // selecting is not an edit

  // Undo walks back one gesture at a time.
  REQUIRE(r.ctx.undo());  // delete
  REQUIRE(r.ctx.undo());  // copy
  REQUIRE(r.ctx.undo());  // resize
  CHECK(r.note(100)->length == n100.length);
  REQUIRE(r.ctx.undo());  // move
  CHECK(*r.note(100) == n100);
}

TEST_CASE("clip view: a click selects without editing; Esc cancels a drag; drags preview without dispatching") {
  Roll r("pianoroll-12edo");
  const doc::Snapshot before = r.ctx.doc();
  const size_t h = r.history();
  r.d.click(r.centreOf(116));
  CHECK(r.ctx.doc() == before);
  CHECK(r.history() == h);
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.view->selection()[0] == 116);
  // Audition on click: the note's pitch (degree 12 = C4) for its duration at 120 BPM (a quarter − 1/16 = 0.375 s).
  REQUIRE(r.port.count(FakeTransport::Call::Audition) == 1);
  CHECK(r.port.calls.back().hz == doctest::Approx(2.0 * 261.626).epsilon(1e-5));
  CHECK(r.port.calls.back().velocity == doctest::Approx(120.0f));
  CHECK(r.port.calls.back().durationMs == doctest::Approx(375.0f).epsilon(1e-4));

  // Mid-drag: nothing dispatched; Esc cancels and leaves no history.
  const Vec2 from = r.centreOf(116);
  r.d.move(from);
  r.d.down(from);
  r.d.move({from.x + 80.0f, from.y - 30.0f});
  r.d.move({from.x + 120.0f, from.y - 40.0f});
  CHECK(r.ctx.doc() == before);
  CHECK(r.d.press("Escape") == KeyResult::Widget);
  r.d.up({from.x + 120.0f, from.y - 40.0f});
  CHECK(r.ctx.doc() == before);
  CHECK(r.history() == h);
}

TEST_CASE("clip view: Up on a 31edo Degree note = +1\\31, Shift+Up = one equave, Down carries below 0") {
  Roll r("pianoroll-31edo");
  REQUIRE(r.note(200) != nullptr);
  REQUIRE(r.note(200)->pitch == PitchId::fromDegree(0, 0));
  r.d.click(r.centreOf(200));
  REQUIRE(r.view->selection().size() == 1);
  size_t h = r.history();
  CHECK(r.d.press("Up") == KeyResult::Action);
  CHECK(r.note(200)->pitch == PitchId::fromDegree(1, 0));
  CHECK(r.history() == h + 1);
  CHECK(r.d.press("Shift+Up") == KeyResult::Action);
  CHECK(r.note(200)->pitch == PitchId::fromDegree(1, 1));
  CHECK(r.d.press("Shift+Down") == KeyResult::Action);
  CHECK(r.d.press("Down") == KeyResult::Action);
  CHECK(r.d.press("Down") == KeyResult::Action);
  CHECK(r.note(200)->pitch == PitchId::fromDegree(30, -1));
  CHECK(r.history() == h + 5);
  // The row model follows: the note's rect is one row below the C3 equave marker row.
  CHECK(r.view->noteRect(doc::NoteId{200}).y == static_cast<int32_t>(r.view->yOfDegree(-1)) - r.view->rowHeight() / 2);
}

TEST_CASE("clip view: Up on 5/4 → 6/4 → 7/4 keeps the fundamental (Q40); Shift+Up multiplies by the equave") {
  Roll r("pianoroll-12edo");
  REQUIRE(r.note(125) != nullptr);
  REQUIRE(r.note(125)->pitch == PitchId::fromRatio(Rational(5, 4)));
  // Off-grid: drawn at its true pitch, between D#3 and E3, nearer E3.
  const IRect nr = r.view->noteRect(doc::NoteId{125});
  const float cy = static_cast<float>(nr.y) + static_cast<float>(nr.h) * 0.5f;
  CHECK(cy > r.view->yOfDegree(4));
  CHECK(cy < r.view->yOfDegree(3));
  CHECK(cy - r.view->yOfDegree(4) < r.view->yOfDegree(3) - cy);

  r.d.click(r.centreOf(125));
  CHECK(r.d.press("Up") == KeyResult::Action);
  CHECK(r.note(125)->pitch == PitchId::fromRatio(Rational(6, 4)));  // stored reduced: 3/2
  CHECK(r.d.press("Up") == KeyResult::Action);
  CHECK(r.note(125)->pitch == PitchId::fromRatio(Rational(7, 4)));  // not 2/1: the fundamental 1/4 is kept
  CHECK(r.d.press("Down") == KeyResult::Action);
  CHECK(r.note(125)->pitch == PitchId::fromRatio(Rational(6, 4)));
  CHECK(r.d.press("Down") == KeyResult::Action);
  CHECK(r.note(125)->pitch == PitchId::fromRatio(Rational(5, 4)));
  CHECK(r.d.press("Shift+Up") == KeyResult::Action);
  CHECK(r.note(125)->pitch == PitchId::fromRatio(Rational(5, 2)));
  CHECK(r.d.press("Shift+Down") == KeyResult::Action);
  CHECK(r.note(125)->pitch == PitchId::fromRatio(Rational(5, 4)));
  CHECK(r.view->virtualFundamental() == 4);

  // Entering 4/3 refines the virtual fundamental to 1/12 (lcm 4, 3); Up on 4/3 = 16/12 → 17/12, and Up on 5/4 =
  // 15/12 → 16/12 = 4/3. A note stepped on 1/12 keeps that fundamental while its pitch is the stepped one, so after
  // the 17/12 note is deleted Down still lands on 15/12; deleting every Ratio note leaves no fundamental.
  doc::Note third;
  third.start = 6 * kQ;
  third.length = kQ;
  third.pitch = PitchId::fromRatio(Rational(4, 3));
  auto add = doc::ops::addNotes(*r.ctx.doc(), r.ctx.store().ids(), r.ref, std::vector<doc::Note>{third});
  REQUIRE(add.ok());
  REQUIRE(r.ctx.store().dispatch(add->command).ok());
  CHECK(r.view->virtualFundamental() == 12);
  REQUIRE(r.ctx.undo());  // removing the 4/3 moves it back, redo forward again
  CHECK(r.view->virtualFundamental() == 4);
  REQUIRE(r.ctx.redo());
  CHECK(r.view->virtualFundamental() == 12);
  const uint32_t thirdId = add->notes[0].id.value;
  r.d.click(r.centreOf(thirdId));
  CHECK(r.d.press("Up") == KeyResult::Action);
  CHECK(r.note(thirdId)->pitch == PitchId::fromRatio(Rational(17, 12)));
  r.d.click(r.centreOf(125));
  CHECK(r.d.press("Up") == KeyResult::Action);
  CHECK(r.note(125)->pitch == PitchId::fromRatio(Rational(4, 3)));
  CHECK(r.view->virtualFundamental() == 12);
  r.d.click(r.centreOf(thirdId));
  CHECK(r.d.press("Delete") == KeyResult::Action);
  r.d.click(r.centreOf(125));
  CHECK(r.d.press("Down") == KeyResult::Action);  // 16/12 → 15/12 = 5/4 on the remembered 1/12
  CHECK(r.note(125)->pitch == PitchId::fromRatio(Rational(5, 4)));
  CHECK(r.view->virtualFundamental() == 12);
  CHECK(r.d.press("Delete") == KeyResult::Action);
  CHECK(r.view->virtualFundamental() == 0);
}

TEST_CASE("clip view: velocity drag over a selection is one undo step; chance lane; Ctrl+U; 0; Ctrl+D; Ctrl+A") {
  Roll r("pianoroll-velocity");
  // Select two notes (click, Shift+click), then drag one's stem up: both change by the same amount.
  r.d.click(r.centreOf(300));
  r.d.click(r.centreOf(301), mod::kShift);
  REQUIRE(r.view->selection().size() == 2);
  const float v0 = r.note(300)->velocity, v1 = r.note(301)->velocity;
  size_t h = r.history();
  const Vec2 head = r.view->stemHead(doc::NoteId{301});
  r.d.drag(head, {head.x, head.y - 20.0f}, 4);
  CHECK(r.history() == h + 1);
  const float dv = r.note(301)->velocity - v1;
  CHECK(dv > 10.0f);
  CHECK(r.note(300)->velocity - v0 == doctest::Approx(std::min(dv, 127.0f - v0)));
  CHECK(r.view->selection().size() == 2);

  // Chance lane: the same gesture edits chance.
  r.view->setShowChance(true);
  r.d.tree().layoutNow();
  h = r.history();
  const Vec2 ch = r.view->stemHead(doc::NoteId{301});
  r.d.drag(ch, {ch.x, ch.y + 30.0f}, 4);
  CHECK(r.history() == h + 1);
  CHECK(r.note(301)->chance < 1.0f);
  CHECK(r.note(300)->chance == doctest::Approx(r.note(301)->chance));

  // Ctrl+U quantizes the selection to the grid (a 1/8 grid: the 1/16 at 1/16 moves to 1/8).
  r.setGrid(3, 1);
  h = r.history();
  CHECK(r.d.press("Ctrl+U") == KeyResult::Action);
  CHECK(r.history() == h + 1);
  CHECK(r.note(300)->start == 0);
  CHECK(r.note(301)->start == 2 * kSixteenth);

  // 0 deactivates, Ctrl+D duplicates, Ctrl+A selects everything.
  CHECK(r.d.press("0") == KeyResult::Action);
  CHECK_FALSE(r.note(300)->enabled());
  const size_t count = r.clip().notes.size();
  CHECK(r.d.press("Ctrl+D") == KeyResult::Action);
  CHECK(r.clip().notes.size() == count + 2);
  CHECK(r.d.press("Ctrl+A") == KeyResult::Action);
  CHECK(r.view->selection().size() == r.clip().notes.size());
  CHECK(r.history() == h + 3);
}

TEST_CASE("clip view: copy / paste, fold to used notes, keys click selects a row, ruler brace drag") {
  Roll r("pianoroll-31edo");
  // Fold: only the used rows (Degree notes and the nearest rows of the ratio / cents notes).
  r.view->setFold(true);
  r.d.tree().layoutNow();
  std::vector<int64_t> used;
  for (const doc::Note& n : r.clip().notes)
    if (n.pitch.kind() == tuning::PitchKind::Degree) used.push_back(31LL * n.pitch.equave() + n.pitch.degree());
  std::sort(used.begin(), used.end());
  used.erase(std::unique(used.begin(), used.end()), used.end());
  for (int64_t d : used) CHECK_MESSAGE(r.view->rows().indexOfDegree(d) >= 0, d);
  CHECK(r.view->rows().size() <= used.size() + 3);
  r.view->setFold(false);
  r.d.tree().layoutNow();

  // Clicking a key auditions it and selects the notes on its row.
  const IRect keys = r.d.rect("main/body/bottom/detail/clip/clip-view/keys");
  REQUIRE(r.view->yOfDegree(18) > static_cast<float>(keys.y));  // unfolding kept the folded rows in view
  REQUIRE(r.view->yOfDegree(18) < static_cast<float>(keys.bottom()));
  r.d.click({static_cast<float>(keys.x) + 10.0f, r.view->yOfDegree(18)});
  REQUIRE(r.port.count(FakeTransport::Call::Audition) == 1);
  CHECK(r.port.calls.back().hz == doctest::Approx(261.626 * std::exp2(18.0 / 31.0)).epsilon(1e-5));
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.view->selection()[0] == 202);

  // Ctrl+C, click empty space at beat 3 (sets the insert position), Ctrl+V: one undo step.
  r.d.tree().setFocus(r.d.find("main/body/bottom/detail/clip/clip-view/notes"));
  CHECK(r.d.press("Ctrl+C") == KeyResult::Action);
  r.setGrid(2, 1);
  r.d.click(r.at(2.0 * static_cast<double>(kQ) + static_cast<double>(kQ / 4), 18));  // empty there; snaps to beat 3
  REQUIRE(r.d.tree().focus() == r.d.find("main/body/bottom/detail/clip/clip-view/notes"));
  const size_t count = r.clip().notes.size(), h = r.history();
  CHECK(r.d.press("Ctrl+V") == KeyResult::Action);
  CHECK(r.history() == h + 1);
  CHECK(r.clip().notes.size() == count + 1);
  bool pasted = false;
  for (const doc::Note& n : r.clip().notes)
    if (n.id.value != 202 && n.pitch == PitchId::fromDegree(18, 0) && n.start == 2 * kQ) pasted = true;
  CHECK(pasted);

  // The loop brace's end dragged one bar earlier: one SetClipLoop, one undo step.
  const doc::ClipLoop loop = r.clip().loop;
  const IRect ruler = r.d.rect("main/body/bottom/detail/clip/clip-view/ruler");
  r.setGrid(-0, 1);  // 1/1 = one bar
  const float ex = r.view->xOfTick(static_cast<double>(loop.end));
  const float y = static_cast<float>(ruler.y) + 5.0f;
  const size_t h2 = r.history();
  r.d.drag({ex - 1.0f, y}, {ex - 1.0f - static_cast<float>(kTicksPerWhole * r.view->pxPerTick()), y}, 6);
  CHECK(r.history() == h2 + 1);
  CHECK(r.clip().loop.start == loop.start);
  CHECK(r.clip().loop.end == loop.end - kTicksPerWhole);
}

TEST_CASE("clip view: a note drag damages only the note area and the velocity lane; idle draws nothing") {
  Roll r("pianoroll-12edo");
  r.d.advance(0.1);
  const uint64_t frames = r.d.framesDrawn();
  r.d.advance(5.0);
  CHECK(r.d.framesDrawn() == frames);
  const Vec2 from = r.centreOf(116);
  r.d.move(from);
  r.d.down(from);
  r.d.advance(0.05);
  r.d.move({from.x + 60.0f, from.y});
  r.d.tree().update(r.d.now() + 0.01);
  const IRect notes = r.d.rect("main/body/bottom/detail/clip/clip-view/notes");
  const IRect lane = r.d.rect("main/body/bottom/detail/clip/clip-view/velocity");
  const IRect box = r.d.rect("main/body/bottom/detail/clip/clip-view/clip-box");
  for (const ClipRect& c : r.d.tree().damage().rects()) {
    const bool inNotes = c.x0 >= notes.x && c.x1 <= notes.right() && c.y0 >= notes.y && c.y1 <= notes.bottom();
    const bool inLane = c.x0 >= lane.x && c.x1 <= lane.right() && c.y0 >= lane.y && c.y1 <= lane.bottom();
    const bool inBox = c.x0 >= box.x && c.x1 <= box.right() && c.y0 >= box.y && c.y1 <= box.bottom();
    CHECK_MESSAGE((inNotes || inLane || inBox), c.x0 << "," << c.y0 << " " << c.x1 << "," << c.y1);
  }
  r.d.up({from.x + 60.0f, from.y});
}

TEST_CASE("clip view: hidden without a clip (the slot's hint shows); follows undo of the clip's creation") {
  FakeTransport port;
  ViewContext ctx(port);
  TestDriver d(1280, 800);
  auto s = Shell::create(d.tree(), ctx);
  REQUIRE(s.ok());
  auto* view = dynamic_cast<ClipView*>(d.find("main/body/bottom/detail/clip/clip-view"));
  REQUIRE(view != nullptr);
  CHECK_FALSE(view->visible());
  auto t = doc::ops::createTrack(*ctx.doc(), ctx.store().ids(), doc::TrackKind::Midi);
  REQUIRE(t.ok());
  REQUIRE(ctx.store().dispatch(t->command).ok());
  auto c = doc::ops::insertEmptyClip(*ctx.doc(), ctx.store().ids(), t->tracks[0], 0, kTicksPerWhole);
  REQUIRE(c.ok());
  REQUIRE(ctx.store().dispatch(c->command).ok());
  ctx.setDetailClip(c->clips[0]);
  d.tree().layoutNow();
  CHECK(view->visible());
  CHECK(view->hasClip());
  CHECK(view->rows().size() > 100);  // 12edo over the audible range
  REQUIRE(ctx.undo());
  CHECK_FALSE(view->hasClip());
  CHECK_FALSE(view->visible());
  REQUIRE(ctx.redo());
  CHECK(view->hasClip());
  d.tree().setRoot(std::make_unique<Widget>());
}

// ---- goldens --------------------------------------------------------------------------------------------

TEST_CASE("render: piano roll goldens (12edo, 31edo, velocity)") {
  for (const std::string scene : {"pianoroll-12edo", "pianoroll-31edo", "pianoroll-velocity"}) {
    CAPTURE(scene);
    uint32_t w = 0, h = 0;
    viewSceneDefaultSize(scene, w, h);
    GpuOptions o;
    o.validation = true;
    auto img = renderViewScene(scene, views::test::fixtureDir(scene), w, h, {}, o);
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png");
    if (!ref) (void)writePng(scene + ".actual.png", *img);
    REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
    ImageDiff diff;
    CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &diff));
    MESSAGE(scene << ": " << pq::ui::test::describe(diff));
  }
}

// ---- partial redraw ---------------------------------------------------------------------------------------

TEST_CASE("render: dragging in the clip view repaints only damage, and equals a full repaint at every step") {
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
  REQUIRE(ctx.openProject(views::test::fixtureDir("pianoroll-12edo")).ok());
  UiTree tree;
  tree.setViewport(W, H);
  tree.setResources(&res);
  auto shell = Shell::create(tree, ctx);
  REQUIRE(shell.ok());
  dynamic_cast<Splitter&>(*tree.find("main/body")).setPosition(520.0f);
  const doc::Track& track = *ctx.doc()->tracks[0];
  ctx.setDetailClip({track.id, track.clips[0].id});
  tree.layoutNow();
  auto* view = dynamic_cast<ClipView*>(tree.find("main/body/bottom/detail/clip/clip-view"));
  REQUIRE(view != nullptr);

  auto partial = OffscreenTarget::create(**gpu, W, H, 1);
  auto full = OffscreenTarget::create(**gpu, W, H, 1);
  REQUIRE((partial.ok() && full.ok()));
  DrawList dl;
  const DrawLayer layer{&dl, kTransientLayer, 0};
  FrameDesc ff;
  ff.layers = {&layer, 1};
  ff.clear = kSceneClearColor;
  double now = 0.0;
  tree.update(now);
  tex->text->beginFrame();
  dl.reset(W, H);
  tree.paint(dl, res, now);
  REQUIRE((*partial)->draw(**renderer, ff).ok());

  uint64_t partialFrames = 0;
  int64_t damagedArea = 0;
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
      damagedArea += static_cast<int64_t>(r.x1 - r.x0) * (r.y1 - r.y0);
    }
    tree.paint(dl, res, now, {box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0});
    FrameDesc pf = ff;
    pf.full = false;
    pf.damage = {rects, n};
    REQUIRE((*partial)->draw(**renderer, pf).ok());
    ++partialFrames;
  };
  auto compare = [&](const char* what) {
    tex->text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, res, now);
    REQUIRE((*full)->draw(**renderer, ff).ok());
    auto a = (*partial)->read();
    auto b = (*full)->read();
    REQUIRE((a.ok() && b.ok()));
    const ImageDiff diff = compareImages(*a, *b, 0);
    if (diff.pixelsOverTolerance != 0) {
      (void)writePng(std::string(what) + ".partial.png", *a);
      (void)writePng(std::string(what) + ".full.png", *b);
    }
    CHECK_MESSAGE(diff.pixelsOverTolerance == 0, std::string(what) << ": " << diff.pixelsOverTolerance << " px differ");
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
  auto noteCentre = [&](uint32_t id) {
    const IRect r = view->noteRect(doc::NoteId{id});
    REQUIRE_FALSE(r.empty());
    return Vec2{static_cast<float>(r.x) + 5.0f, static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f};
  };
  // Move a note, Alt-copy one, move the off-grid 5/4; then a velocity drag, a rubber band and a draw-mode run.
  struct Step {
    const char* what;
    uint32_t note;
    Mods mods;
    float dx, dy;
  };
  for (const Step& s : {Step{"move", 116, mod::kNone, 1.0f, -1.0f}, Step{"copy", 104, mod::kAlt, 1.0f, 1.0f},
                        Step{"move-offgrid", 125, mod::kNone, -1.0f, 1.0f}}) {
    const Vec2 from = noteCentre(s.note);
    pointer(PointerType::Move, from, s.mods);
    pointer(PointerType::Down, from, s.mods);
    for (int i = 1; i <= 12; ++i)
      pointer(PointerType::Move, {from.x + s.dx * 17.0f * static_cast<float>(i), from.y + s.dy * 5.0f * static_cast<float>(i)},
              s.mods);
    compare(s.what);  // mid-drag: the preview
    pointer(PointerType::Up, {from.x + s.dx * 17.0f * 12.0f, from.y + s.dy * 60.0f}, s.mods);
    compare(s.what);  // committed
  }
  {
    const Vec2 head = view->stemHead(doc::NoteId{116});
    pointer(PointerType::Move, head);
    pointer(PointerType::Down, head);
    for (int i = 1; i <= 8; ++i) pointer(PointerType::Move, {head.x, head.y + 4.0f * static_cast<float>(i)});
    compare("velocity");
    pointer(PointerType::Up, {head.x, head.y + 32.0f});
    compare("velocity");
  }
  {
    const Vec2 a{view->xOfTick(1.1 * static_cast<double>(kQ)), view->yOfDegree(9)};
    pointer(PointerType::Move, a);
    pointer(PointerType::Down, a);
    for (int i = 1; i <= 8; ++i)
      pointer(PointerType::Move, {a.x + 30.0f * static_cast<float>(i), a.y + 20.0f * static_cast<float>(i)});
    compare("rubber");
    pointer(PointerType::Up, {a.x + 240.0f, a.y + 160.0f});
    ctx.setDrawMode(true);
    frame();
    const Vec2 b{view->xOfTick(4.1 * static_cast<double>(kQ)), view->yOfDegree(10)};
    pointer(PointerType::Move, b);
    pointer(PointerType::Down, b);
    for (int i = 1; i <= 6; ++i) pointer(PointerType::Move, {b.x + 25.0f * static_cast<float>(i), b.y});
    compare("draw");
    pointer(PointerType::Up, {b.x + 150.0f, b.y});
    compare("draw");
  }
  CHECK(partialFrames > 30);
  MESSAGE(partialFrames << " partial frames, " << damagedArea / static_cast<int64_t>(std::max<uint64_t>(partialFrames, 1))
                        << " px² damaged per frame on average (window " << W * H << ")");
  CHECK((*gpu)->validationErrorCount() == 0);
  tree.setRoot(std::make_unique<Widget>());
}
