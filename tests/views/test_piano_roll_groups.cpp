// T7.14 piano roll: JI groups as the user works with them (docs/plans/M7.md "Rework: notes-first JI" R3–R6). Every
// gesture goes through the real shell, pointer and key dispatch (piano_roll_fix.h), one undo step each:
//   Tune to JI as a switch (Ctrl+Shift+J / the menu: tune, the chooser of readings, back exactly to what was written;
//   the same notes: selection, lengths, curves survive); the frame (header, bounds; it takes no clicks: M10); "Is
//   Harmonic…" on a member (Q297 b); warps by dragging the frame's corner grips, the markers and the glide grips (the
//   members are drawn at their written pitch before a warp and glide into JI), their menu, Delete, "Back to Tuning
//   Here"; a member dragged vertically snaps to the harmonics; walk (Alt+Up / Down, the menu); the proportion tool (J
//   + a box + the field; "Insert Proportion…" in the empty-space menu); "Add Note at Ratio…" (the user's 5 → 10 / 15
//   example); two groups in one clip.
// Goldens: the pianoroll-group-* scenes.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "piano_roll_fix.h"

using namespace roll_fix;
using pq::tuning::AnchorKind;
using pq::tuning::PitchKind;
using pq::tuning::Rational;

namespace {

const double kThird = 1200.0 * std::log2(5.0 / 4.0), kFifth = 1200.0 * std::log2(3.0 / 2.0), kSeventh = 1200.0 * std::log2(7.0 / 4.0);

bool inside(const Rect& r, Vec2 p) { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }

// Tunes the selection with the key and closes the chooser that follows.
void tune(Roll& r) {
  CHECK(r.d.press("Ctrl+Shift+J") == KeyResult::Action);
  r.d.step();
  if (r.topMenu()) (void)r.d.press("Escape");
  r.d.step();
  REQUIRE(r.shell->overlay().openCount() == 0);
}

}  // namespace

TEST_CASE("roll groups: Tune to JI is a switch — Ctrl+Shift+J tunes the selected chord to its best reading, again goes back exactly to what was written; the chooser offers the other readings") {
  Roll r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.chord({0, 4, 7, 10});  // C E G B♭ in 12-EDO
  const double hz[4] = {r.hzOf(ids[0]), r.hzOf(ids[1]), r.hzOf(ids[2]), r.hzOf(ids[3])};
  // The E carries a vibrato and a drawn curve: they are the note's, and survive both ways.
  doc::NoteVibrato vib;
  vib.depthCents = 20.0f;
  vib.rate = 6.0f;
  const doc::NoteCurves curves{ids[1], {{0, 0.0f, 0.0f, {}}, {kQ, 15.0f, 0.0f, {}}}, {}, {}, vib};
  REQUIRE(r.ctx.store().dispatch(doc::SetNoteCurves{r.ref, {curves}}).ok());
  r.d.step();
  r.select(ids);
  size_t h = r.history();

  // One note: nothing to tune to.
  r.select({ids[0]});
  (void)r.d.press("Ctrl+Shift+J");
  CHECK(r.history() == h);
  CHECK(r.clip().groups == nullptr);
  r.select(ids);

  CHECK(r.d.press("Ctrl+Shift+J") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  const doc::JiGroupId group = r.onlyGroup().id;
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{4, 5, 6, 7});
  CHECK(r.onlyGroup().anchor == ids[0]);
  CHECK(r.onlyGroup().warps.empty());  // just throughout
  CHECK(r.note(ids[0]).pitch == PitchId::fromDegree(0, 0));  // the lowest note is frozen as it was written
  CHECK(r.note(ids[1]).pitch == PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, ids[0].value));
  CHECK(r.note(ids[3]).pitch == PitchId::fromRatio(Rational(7, 4), AnchorKind::Note, ids[0].value));
  REQUIRE(doc::findNoteWritten(r.clip(), ids[1]) != nullptr);
  CHECK(doc::findNoteWritten(r.clip(), ids[1])->written == PitchId::fromDegree(4, 0));
  CHECK(r.hzOf(ids[0]) == hz[0]);
  CHECK(r.hzOf(ids[2]) == doctest::Approx(hz[0] * 1.5).epsilon(1e-12));
  // The same notes: the selection, the lengths, the curves.
  CHECK(std::vector<uint32_t>(r.view->selection().begin(), r.view->selection().end()).size() == 4);
  CHECK(r.note(ids[1]).length == 4 * kQ);
  CHECK(*doc::findNoteCurves(r.clip(), ids[1]) == curves);
  CHECK(r.ctx.status().find("Tuned to 4:5:6:7") != std::string::npos);
  CHECK(r.ctx.status().find("bottom-left corner") != std::string::npos);  // how to add a warp

  // The chooser opened under the header: the readings by simplicity and error, the current one ticked. Esc keeps.
  Menu* chooser = r.topMenu();
  REQUIRE(chooser != nullptr);
  REQUIRE(chooser->items().size() >= 2);
  CHECK(chooser->items()[0].label == "4:5:6:7");
  CHECK(chooser->items()[0].checked);
  CHECK(chooser->items()[0].shortcut == "47 \xC2\xA2");
  CHECK(chooser->items()[1].label == "20:25:30:36");
  CHECK_FALSE(chooser->items()[1].checked);
  (void)r.d.press("Escape");
  r.d.step();
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.history() == h + 1);

  // The frame: the header is the proportion, the box holds every note of the group.
  ClipView::GroupGeometry g = r.view->groupGeometry(group);
  REQUIRE(g.valid);
  CHECK(g.headerText == "4:5:6:7");
  CHECK(g.tuned);
  for (const doc::NoteId id : ids) {
    CHECK(inside(g.box, r.view->notePoint(id, 0.02)));
    CHECK(inside(g.box, r.view->notePoint(id, 0.98)));
  }
  CHECK(g.header.y1 <= g.box.y0);  // above the frame
  CHECK(r.view->harmonicOf(ids[0]) == 4u);
  CHECK(r.view->harmonicOf(ids[3]) == 7u);

  // The same key again: back to the tuning, exactly (identities, Hz), in one step; the notes stay selected.
  CHECK(r.d.press("Ctrl+Shift+J") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 2);
  CHECK(r.clip().groups == nullptr);
  CHECK(r.clip().written == nullptr);
  for (size_t i = 0; i < 4; ++i) {
    CHECK(r.note(ids[i]).pitch.kind() == PitchKind::Degree);
    CHECK(r.hzOf(ids[i]) == hz[i]);
  }
  CHECK(r.view->selection().size() == 4);
  CHECK(*doc::findNoteCurves(r.clip(), ids[1]) == curves);
  CHECK_FALSE(r.view->groupGeometry(group).valid);
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.ctx.status().find("Back to 12-EDO") != std::string::npos);
  // Undo / redo walk the two states.
  REQUIRE(r.ctx.undo());
  r.d.step();
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{4, 5, 6, 7});
  REQUIRE(r.ctx.undo());
  r.d.step();
  CHECK(r.clip().groups == nullptr);

  // The menu: Tune to JI; a click in the chooser picks another reading (one more step).
  h = r.history();
  r.choose(r.rightClick(r.view->notePoint(ids[2], 0.5)), "Tune to JI");
  CHECK(r.history() == h + 1);
  chooser = r.topMenu();
  REQUIRE(chooser != nullptr);
  r.choose(chooser, "20:25:30:36");
  CHECK(r.history() == h + 2);
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{20, 25, 30, 36});
  CHECK(r.note(ids[3]).pitch == PitchId::fromRatio(Rational(9, 5), AnchorKind::Note, ids[0].value));
  CHECK(doc::findNoteWritten(r.clip(), ids[3])->written == PitchId::fromDegree(10, 0));  // what was written is remembered still
  CHECK(r.view->selection().size() == 4);
  g = r.view->groupGeometry(r.onlyGroup().id);
  CHECK(g.headerText == "20:25:30:36");
  // A click on the header's proportion opens the chooser again; Up + Enter picks the reading above.
  r.d.click(g.proportion);
  r.d.step();
  chooser = r.topMenu();
  REQUIRE(chooser != nullptr);
  CHECK(chooser->items()[1].checked);
  CHECK(chooser->highlighted() == 1);
  (void)r.d.press("Up");
  (void)r.d.press("Enter");
  r.d.step();
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.history() == h + 3);
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{4, 5, 6, 7});
  // The menu reads Back to Tuning now; with the chooser open the key still switches back.
  Menu* menu = r.rightClick(r.view->notePoint(ids[2], 0.5));
  CHECK(Roll::itemOf(menu, "Back to Tuning") >= 0);
  CHECK(Roll::itemOf(menu, "Tune to JI") < 0);
  (void)r.d.press("Escape");
  r.d.step();
  r.d.click(r.view->groupGeometry(r.onlyGroup().id).proportion);
  r.d.step();
  REQUIRE(r.topMenu() != nullptr);
  (void)r.d.press("Ctrl+Shift+J");
  r.d.step();
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.clip().groups == nullptr);
  CHECK(r.history() == h + 4);
  for (size_t i = 0; i < 4; ++i) CHECK(r.hzOf(ids[i]) == hz[i]);
}

TEST_CASE("roll groups: a frame takes no clicks — inside it a double-click adds a note and a click on empty space deselects (M10); the header selects the group's notes") {
  Roll r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.chord({0, 4, 7});
  r.select(ids);
  tune(r);
  const doc::JiGroupId group = r.onlyGroup().id;
  const ClipView::GroupGeometry g = r.view->groupGeometry(group);
  REQUIRE(g.valid);

  // Empty space inside the box, between the notes: a click starts a selection rectangle (it deselects) …
  const Vec2 inBox = r.at(2.0 * kQ, 2);
  REQUIRE(inside(g.box, inBox));
  size_t h = r.history();
  r.d.click(inBox);
  r.d.step();
  CHECK(r.view->selection().empty());
  CHECK(r.history() == h);
  CHECK(r.shell->overlay().openCount() == 0);
  // … and a double-click adds a note there (on the group's nearest harmonic: the group is still the active one).
  r.d.doubleClick(inBox);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.noteCount() == 4);
  CHECK(r.shell->overlay().openCount() == 0);
  REQUIRE(r.view->selection().size() == 1);
  const doc::Note& added = r.note(doc::NoteId{r.view->selection()[0]});
  CHECK(doc::noteAnchorOf(added.pitch) == ids[0]);  // a member: an exact ratio to the frozen note
  CHECK(added.start == 2 * kQ);
  // A rubber band from inside the box selects the notes it crosses, as anywhere.
  r.d.drag(r.at(0.5 * kQ, 9), r.at(3.5 * kQ, 6), 5);
  r.d.step();
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.view->selection()[0] == ids[2].value);

  // A group declared by hand (no written identities): its header is no chooser, a click on it selects its notes.
  const std::vector<doc::NoteId> lone = r.addNotes({Roll::plain(6 * kQ, 2 * kQ, 2)});
  REQUIRE(r.view->declareHarmonic(lone[0], 4));
  const doc::NoteId five = r.addNotes({[&] {
    doc::Note n = Roll::plain(6 * kQ, 2 * kQ, 2);
    n.pitch = PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, lone[0].value);
    return n;
  }()})[0];
  const doc::JiGroup* declared = doc::findJiGroupByAnchor(r.clip(), lone[0]);
  REQUIRE(declared != nullptr);
  r.d.click(r.at(12.0 * kQ, 12));  // empty space, outside every frame
  r.d.step();
  REQUIRE(r.view->selection().empty());
  const ClipView::GroupGeometry dg = r.view->groupGeometry(declared->id);
  REQUIRE(dg.valid);
  CHECK(dg.headerText == "4:5");
  CHECK_FALSE(dg.tuned);
  h = r.history();
  r.d.click(mid(dg.header));
  r.d.step();
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.history() == h);
  REQUIRE(r.view->selection().size() == 2);
  CHECK(std::binary_search(r.view->selection().begin(), r.view->selection().end(), lone[0].value));
  CHECK(std::binary_search(r.view->selection().begin(), r.view->selection().end(), five.value));
  CHECK(r.view->activeJiGroup() == declared->id);
  // The tuned group's header is its proportion: the click selects its notes and opens the chooser.
  r.d.click(g.proportion);
  r.d.step();
  CHECK(r.topMenu() != nullptr);
  CHECK(r.view->selection().size() == 4);
  CHECK(r.view->activeJiGroup() == group);
}

TEST_CASE("roll groups: Is Harmonic… on a member makes it the group's frozen note — every pitch stays, the numbers re-derive, no second group (Q297)") {
  Roll r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.chord({0, 4, 7});
  r.select(ids);
  tune(r);
  const doc::JiGroupId group = r.onlyGroup().id;
  const double hz[3] = {r.hzOf(ids[0]), r.hzOf(ids[1]), r.hzOf(ids[2])};
  const size_t h = r.history();
  r.choose(r.rightClick(r.view->notePoint(ids[1], 0.5)), "Is Harmonic");
  r.typeEnter("10");
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.history() == h + 1);
  CHECK(r.onlyGroup().id == group);  // the same group, re-anchored: not a chain
  CHECK(r.onlyGroup().anchor == ids[1]);
  CHECK(r.onlyGroup().index == 10);
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{8, 10, 12});
  CHECK(r.view->harmonicOf(ids[0]) == 8u);
  CHECK(r.view->harmonicOf(ids[1]) == 10u);
  CHECK(r.view->harmonicOf(ids[2]) == 12u);
  for (size_t i = 0; i < 3; ++i) CHECK(r.hzOf(ids[i]) == doctest::Approx(hz[i]).epsilon(1e-12));
  CHECK(r.view->groupGeometry(group).headerText == "8:10:12");
  REQUIRE(r.ctx.undo());
  r.d.step();
  CHECK(r.onlyGroup().anchor == ids[0]);
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{4, 5, 6});
}

TEST_CASE("roll groups: warps — drag the frame's corner inward and the chord plays as written, then glides into JI; markers move, glides stretch, a second warp goes back; menu, Delete, Back to Tuning Here") {
  Roll r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.chord({0, 4, 7, 10});
  const double hzE = r.hzOf(ids[1]);
  r.select(ids);
  tune(r);
  const doc::JiGroupId group = r.onlyGroup().id;
  const double root = r.cents(ids[0], 0.0);
  // Just throughout: the E is drawn (and sounds) a syntonic comma's worth under its row.
  CHECK(r.cents(ids[1], 0.0) - root == doctest::Approx(kThird));
  CHECK(r.hzOf(ids[1]) == doctest::Approx(r.hzOf(ids[0]) * 1.25).epsilon(1e-12));
  ClipView::GroupGeometry g = r.view->groupGeometry(group);
  REQUIRE(g.valid);
  REQUIRE(g.gripsShown);
  CHECK(std::fabs(g.leftGrip.y - g.box.y1) < 0.5f);  // on the bottom border
  CHECK(g.markers.empty());
  size_t h = r.history();

  // The bottom-left corner dragged one beat inward: a warp into JI there, previewed while dragging.
  const Vec2 to{r.view->xOfTick(1.0 * kQ), g.leftGrip.y};
  REQUIRE(r.d.down(g.leftGrip));
  r.d.move({0.5f * (g.leftGrip.x + to.x), to.y});
  r.d.move(to);
  r.d.step();
  CHECK(r.history() == h);
  CHECK(r.cents(ids[1], 0.0) - root == doctest::Approx(400.0));  // the preview already draws the E on its row
  CHECK(r.ctx.status().find("Warp into 4:5:6:7") != std::string::npos);
  r.d.up(to);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.onlyGroup().warps.size() == 1);
  CHECK(r.onlyGroup().warps[0] == doc::JiWarp{kQ, kQ, doc::GlideShape{}, true});
  // Before the marker the members are drawn where they were written, then they glide (an S) and hold JI. The frozen
  // note does not move.
  CHECK(r.cents(ids[1], 0.0) - root == doctest::Approx(400.0));
  CHECK(r.cents(ids[1], 0.25) - root == doctest::Approx(400.0));
  CHECK(r.cents(ids[1], 0.375) - root == doctest::Approx(0.5 * (400.0 + kThird)).epsilon(1e-4));
  CHECK(r.cents(ids[1], 0.5) - root == doctest::Approx(kThird));
  CHECK(r.cents(ids[1], 1.0) - root == doctest::Approx(kThird));
  CHECK(r.cents(ids[3], 0.0) - root == doctest::Approx(1000.0));
  CHECK(r.cents(ids[3], 1.0) - root == doctest::Approx(kSeventh));
  CHECK(r.cents(ids[0], 0.3) == root);
  CHECK(r.hzOf(ids[1]) == doctest::Approx(hzE).epsilon(1e-12));  // it starts on the tempered pitch
  CHECK(r.view->notePoint(ids[1], 0.0).y == doctest::Approx(r.view->yOfDegree(4)).epsilon(0.002));
  CHECK(r.view->notePoint(ids[1], 0.9).y > r.view->yOfDegree(4) + 1.0f);  // 13.7 ¢ lower: about 2 px
  g = r.view->groupGeometry(group);
  REQUIRE(g.markers.size() == 1);
  CHECK(g.markers[0].x == doctest::Approx(r.view->xOfTick(1.0 * kQ)).epsilon(0.002));
  CHECK(g.glideGrips[0].x == doctest::Approx(r.view->xOfTick(2.0 * kQ)).epsilon(0.002));
  CHECK(g.headerText == "12-EDO \xE2\x86\x92 4:5:6:7");
  CHECK(r.view->selectedWarp() == 0);
  CHECK(r.view->selection().size() == 4);  // the notes stay selected

  // The marker a beat later.
  h = r.history();
  r.d.drag(g.markers[0], {g.markers[0].x + kBeat, g.markers[0].y}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.onlyGroup().warps[0] == doc::JiWarp{2 * kQ, kQ, doc::GlideShape{}, true});
  CHECK(r.cents(ids[1], 0.5) - root == doctest::Approx(400.0));
  // Its menu: Straight.
  g = r.view->groupGeometry(group);
  Menu* menu = r.rightClick(g.markers[0]);
  REQUIRE(Roll::itemOf(menu, "Remove Warp") >= 0);
  r.choose(menu, "Straight");
  CHECK(r.history() == h + 2);
  CHECK(r.onlyGroup().warps[0].shape.kind == doc::GlideShape::LogLinear);
  CHECK(r.cents(ids[1], 0.5625) - root == doctest::Approx(400.0 + 0.25 * (kThird - 400.0)).epsilon(1e-4));

  // The bottom-right corner dragged a beat inward: a second warp, back to the tuning.
  g = r.view->groupGeometry(group);
  REQUIRE(g.gripsShown);
  h = r.history();
  r.d.drag(g.rightGrip, {r.view->xOfTick(3.0 * kQ), g.rightGrip.y}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.onlyGroup().warps.size() == 2);
  CHECK(r.onlyGroup().warps[1] == doc::JiWarp{3 * kQ, kQ, doc::GlideShape{}, false});
  CHECK(r.cents(ids[1], 0.75) - root == doctest::Approx(kThird));
  CHECK(r.cents(ids[1], 1.0) - root == doctest::Approx(400.0));
  g = r.view->groupGeometry(group);
  CHECK(g.headerText == "12-EDO \xE2\x86\x92 4:5:6:7 \xE2\x86\x92 12-EDO");
  REQUIRE(g.glideGrips.size() == 2);
  // Its glide grip dragged back onto the marker: a jump (the grip stays grabbable beside the marker).
  r.d.drag(g.glideGrips[1], {g.glideGrips[1].x - kBeat, g.glideGrips[1].y}, 5);
  r.d.step();
  CHECK(r.history() == h + 2);
  CHECK(r.onlyGroup().warps[1].glide == 0);
  CHECK(r.onlyGroup().warps[1].at == 3 * kQ);
  g = r.view->groupGeometry(group);
  CHECK(g.glideGrips[1].x - g.markers[1].x >= 10.0f);
  // … and out again by two beats: it stops at the frame's end.
  r.d.drag(g.glideGrips[1], {g.glideGrips[1].x + 2.0f * kBeat, g.glideGrips[1].y}, 5);
  r.d.step();
  CHECK(r.onlyGroup().warps[1].glide == kQ);

  // A click on a marker selects it; Delete removes that warp (not the notes).
  g = r.view->groupGeometry(group);
  h = r.history();
  r.d.click(g.markers[0]);
  r.d.step();
  REQUIRE(r.view->selectedWarp() == 0);
  CHECK(r.history() == h);
  CHECK(r.view->selection().size() == 4);  // the notes are selected still: the marker, clicked last, is what Delete means
  CHECK(r.d.press("Delete") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.noteCount() == 4);
  REQUIRE(r.onlyGroup().warps.size() == 1);
  CHECK_FALSE(r.onlyGroup().warps[0].toJi);  // JI, then back to the tuning at beat 3
  CHECK(r.cents(ids[1], 0.0) - root == doctest::Approx(kThird));

  // The menu on empty space inside the frame: the transition back moves to the pointer.
  menu = r.rightClick(r.at(1.02 * kQ, 2));
  REQUIRE(menu != nullptr);
  CHECK(Roll::itemOf(menu, "Into JI Here") < 0);
  r.choose(menu, "Back to Tuning Here");
  CHECK(r.history() == h + 2);
  REQUIRE(r.onlyGroup().warps.size() == 1);
  CHECK(r.onlyGroup().warps[0].at == kQ);
  CHECK_FALSE(r.onlyGroup().warps[0].toJi);
  CHECK(r.cents(ids[1], 1.0) - root == doctest::Approx(400.0));
  // After it the chord is tempered: there the menu offers the way back in.
  menu = r.rightClick(r.at(3.02 * kQ, 2));
  CHECK(Roll::itemOf(menu, "Into JI Here") >= 0);
  (void)r.d.press("Escape");
  r.d.step();
  // Esc mid-drag leaves the warps alone.
  g = r.view->groupGeometry(group);
  h = r.history();
  REQUIRE(r.d.down(g.markers[0]));
  r.d.move({g.markers[0].x + kBeat, g.markers[0].y});
  (void)r.d.press("Escape");
  r.d.up({g.markers[0].x + kBeat, g.markers[0].y});
  r.d.step();
  CHECK(r.history() == h);
  CHECK(r.onlyGroup().warps[0].at == kQ);
}

TEST_CASE("roll groups: editing is note editing — Up / Down and a vertical drag move a member along its group's harmonics; Alt+Up / Alt+Down walk the group") {
  Roll r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.chord({0, 4, 7});
  r.select(ids);
  tune(r);
  const doc::JiGroupId group = r.onlyGroup().id;
  const double rootHz = r.hzOf(ids[0]);

  // Up on the 6: the 7.
  r.d.click(r.view->notePoint(ids[2], 0.5));
  REQUIRE(r.view->selection().size() == 1);
  size_t h = r.history();
  CHECK(r.d.press("Up") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.note(ids[2]).pitch == PitchId::fromRatio(Rational(7, 4), AnchorKind::Note, ids[0].value));
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{4, 5, 7});
  CHECK(r.d.press("Down") == KeyResult::Action);
  r.d.step();
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{4, 5, 6});

  // The 5 dragged up to where harmonic 7 lies: it snaps to it (the guides of its group show while dragging).
  r.d.click(r.view->notePoint(ids[1], 0.5));
  REQUIRE(r.view->activeJiGroup() == group);
  const Vec2 from = r.view->notePoint(ids[1], 0.5);
  const std::optional<float> y7 = r.view->harmonicGuideY(7);
  REQUIRE(y7.has_value());
  h = r.history();
  r.d.drag(from, {from.x, *y7 + 3.0f}, 6);  // 3 px off the guide: still the 7
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.note(ids[1]).pitch == PitchId::fromRatio(Rational(7, 4), AnchorKind::Note, ids[0].value));
  CHECK(r.note(ids[1]).start == 0);
  CHECK(std::fabs(r.view->notePoint(ids[1], 0.5).y - *y7) <= 1.0f);
  // … and back down to the 5.
  const Vec2 up = r.view->notePoint(ids[1], 0.5);
  r.d.drag(up, {up.x, *r.view->harmonicGuideY(5) - 2.0f}, 6);
  r.d.step();
  CHECK(r.note(ids[1]).pitch == PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, ids[0].value));

  // Walk: Alt+Up shifts every number by one with the frozen note's pitch fixed: 4:5:6 → 5:6:7.
  h = r.history();
  CHECK(r.d.press("Alt+Up") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{5, 6, 7});
  CHECK(r.onlyGroup().index == 5);
  CHECK(r.hzOf(ids[0]) == rootHz);
  CHECK(r.hzOf(ids[2]) == doctest::Approx(rootHz * 7.0 / 5.0).epsilon(1e-12));
  CHECK(r.view->groupGeometry(group).headerText == "5:6:7");
  CHECK(r.ctx.status().find("5:6:7") != std::string::npos);
  CHECK(r.view->selection().size() == 1);
  CHECK(r.d.press("Alt+Down") == KeyResult::Action);
  CHECK(r.d.press("Alt+Down") == KeyResult::Action);
  r.d.step();
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{3, 4, 5});
  CHECK(r.hzOf(ids[0]) == rootHz);
  // The menu walks too (the mouse path, M11).
  h = r.history();
  r.choose(r.rightClick(r.view->notePoint(ids[1], 0.5)), "Walk Up");
  CHECK(r.history() == h + 1);
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{4, 5, 6});
  // At the first harmonic the walk stops and says so.
  for (int i = 0; i < 3; ++i) (void)r.d.press("Alt+Down");
  r.d.step();
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{1, 2, 3});
  h = r.history();
  (void)r.d.press("Alt+Down");
  CHECK(r.history() == h);
  CHECK(r.ctx.status().find("first harmonic") != std::string::npos);
  // With no group note selected (and none active) Alt+Up is not the walk.
  (void)r.d.press("Escape");
  r.d.step();
  CHECK(r.d.press("Alt+Up") != KeyResult::Action);
}

TEST_CASE("roll groups: the proportion tool — J, drag a box, type 8:9:10:12:15, Enter: regular notes in one group, the lowest frozen on the box's bottom row; also from the empty-space menu") {
  Roll r;
  r.focusNotes();
  size_t h = r.history();
  CHECK(r.d.press("J") == KeyResult::Action);
  CHECK(r.view->proportionArmed());
  // Esc puts the tool away.
  (void)r.d.press("Escape");
  CHECK_FALSE(r.view->proportionArmed());
  CHECK(r.d.press("J") == KeyResult::Action);
  REQUIRE(r.view->proportionArmed());

  // A box from beat 2 to beat 6, its bottom on row 0 (C).
  r.d.drag(r.at(2.1 * kQ, 12), r.at(5.9 * kQ, 0), 6);
  r.d.step();
  CHECK_FALSE(r.view->proportionArmed());
  CHECK(r.history() == h);
  REQUIRE(r.field() != nullptr);
  // A text that is no proportion keeps the field open with the reason.
  REQUIRE(r.d.type("8:9:x"));
  (void)r.d.press("Enter");
  r.d.step();
  CHECK(r.history() == h);
  REQUIRE(r.field() != nullptr);
  for (int i = 0; i < 5; ++i) (void)r.d.press("Backspace");
  r.typeEnter("8:9:10:12:15");
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.history() == h + 1);
  REQUIRE(r.noteCount() == 5);
  const doc::JiGroup& g = r.onlyGroup();
  CHECK(g.index == 8);
  CHECK(r.proportion(g) == std::vector<int64_t>{8, 9, 10, 12, 15});
  const doc::Note& anchor = r.note(g.anchor);
  CHECK(anchor.pitch == PitchId::fromDegree(0, 0));
  CHECK(anchor.start == 2 * kQ);
  CHECK(anchor.length == 4 * kQ);
  int members = 0;
  for (const doc::Note& n : r.clip().notes) {
    CHECK(n.start == 2 * kQ);
    CHECK(n.length == 4 * kQ);
    members += doc::noteAnchorOf(n.pitch) == g.anchor ? 1 : 0;
  }
  CHECK(members == 4);
  CHECK(r.view->selection().size() == 5);
  CHECK(r.view->groupGeometry(g.id).headerText == "8:9:10:12:15");
  CHECK_FALSE(r.view->groupGeometry(g.id).tuned);  // nothing was written tempered: no way "back"
  REQUIRE(r.ctx.undo());
  r.d.step();
  CHECK(r.noteCount() == 0);
  CHECK(r.clip().groups == nullptr);

  // The mouse path: right-click empty space > Insert Proportion… (one bar from the cell under the pointer).
  h = r.history();
  r.choose(r.rightClick(r.at(8.3 * kQ, 5)), "Insert Proportion");
  REQUIRE(r.field() != nullptr);
  r.typeEnter("4:5:6");
  CHECK(r.history() == h + 1);
  REQUIRE(r.noteCount() == 3);
  CHECK(r.note(r.onlyGroup().anchor).pitch == PitchId::fromDegree(5, 0));
  CHECK(r.note(r.onlyGroup().anchor).start == 8 * kQ);
  CHECK(r.note(r.onlyGroup().anchor).length == 4 * kQ);
}

TEST_CASE("roll groups: Add Note at Ratio… — the user's example: 3/2 above harmonic 5 and the labels read 10 and 15; a ratio is to the clicked note; a harmonic number") {
  Roll r;
  r.focusNotes();
  const doc::NoteId e = r.addNotes({Roll::plain(0, 4 * kQ, 4)})[0];
  const double eHz = r.hzOf(e);
  r.choose(r.rightClick(r.view->notePoint(e, 0.5)), "Is Harmonic");
  r.typeEnter("5");
  REQUIRE(r.view->harmonicOf(e) == 5u);
  const doc::JiGroupId group = r.onlyGroup().id;
  CHECK(r.view->groupGeometry(group).headerText == "5");
  size_t h = r.history();

  Menu* menu = r.rightClick(r.view->notePoint(e, 0.5));
  REQUIRE(Roll::itemOf(menu, "Add Note at Ratio") >= 0);
  r.choose(menu, "Add Note at Ratio");
  REQUIRE(r.field() != nullptr);
  REQUIRE(r.d.type("three halves"));
  (void)r.d.press("Enter");
  r.d.step();
  CHECK(r.history() == h);
  REQUIRE(r.field() != nullptr);  // refused, with the reason; nothing changed
  for (int i = 0; i < 12; ++i) (void)r.d.press("Backspace");
  r.typeEnter("3/2");
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.history() == h + 1);
  REQUIRE(r.noteCount() == 2);
  REQUIRE(r.view->selection().size() == 1);
  const doc::NoteId fifth{r.view->selection()[0]};
  CHECK(r.note(fifth).pitch == PitchId::fromRatio(Rational(3, 2), AnchorKind::Note, e.value));
  CHECK(r.note(fifth).start == 0);
  CHECK(r.note(fifth).length == 4 * kQ);
  // "that harmonic 5 is now harmonic 10, and the 3/2 is at 15 since the virtual fundamental shifts."
  CHECK(r.view->harmonicOf(e) == 10u);
  CHECK(r.view->harmonicOf(fifth) == 15u);
  CHECK(r.view->groupGeometry(group).headerText == "10:15");
  CHECK(r.hzOf(e) == eHz);  // nothing moved: only the numbers
  CHECK(r.hzOf(fifth) == doctest::Approx(eHz * 1.5).epsilon(1e-12));
  CHECK(r.ctx.status().find("10:15") != std::string::npos);

  // On the new note: 7:4 is a ratio to THAT note (21/8 of the frozen one).
  h = r.history();
  r.choose(r.rightClick(r.view->notePoint(fifth, 0.5)), "Add Note at Ratio");
  r.typeEnter("7:4");
  CHECK(r.history() == h + 1);
  REQUIRE(r.view->selection().size() == 1);
  const doc::NoteId seventh{r.view->selection()[0]};
  CHECK(r.note(seventh).pitch == PitchId::fromRatio(Rational(21, 8), AnchorKind::Note, e.value));
  CHECK(r.hzOf(seventh) == doctest::Approx(r.hzOf(fifth) * 1.75).epsilon(1e-12));
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{40, 60, 105});
  REQUIRE(r.ctx.undo());
  r.d.step();
  // A whole number is a harmonic of the group as it is numbered now: 20 = an octave above the 10.
  r.choose(r.rightClick(r.view->notePoint(e, 0.5)), "Add Note at Ratio");
  r.typeEnter("20");
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.note(doc::NoteId{r.view->selection()[0]}).pitch == PitchId::fromRatio(Rational(2, 1), AnchorKind::Note, e.value));
  CHECK(r.proportion(r.onlyGroup()) == std::vector<int64_t>{10, 15, 20});

  // A note of no group has nothing to be a ratio of: the entry is greyed.
  const doc::NoteId plain = r.addNotes({Roll::plain(8 * kQ, kQ, 1)})[0];
  menu = r.rightClick(r.view->notePoint(plain, 0.5));
  REQUIRE(Roll::itemOf(menu, "Add Note at Ratio") >= 0);
  CHECK_FALSE(menu->items()[static_cast<size_t>(Roll::itemOf(menu, "Add Note at Ratio"))].enabled);
}

TEST_CASE("roll groups: several groups in one clip each have their frame; the guides are the active group's") {
  Roll r;
  r.focusNotes();
  const std::vector<doc::NoteId> a = r.chord({0, 4, 7}, 0, 2 * kQ);
  const std::vector<doc::NoteId> b = r.chord({2, 5, 9}, 4 * kQ, 2 * kQ);  // D F A: a minor triad
  r.select(a);
  tune(r);
  r.select(b);
  tune(r);
  REQUIRE(doc::jiGroupsOf(r.clip()).size() == 2);
  const doc::JiGroup* ga = doc::findJiGroupByAnchor(r.clip(), a[0]);
  const doc::JiGroup* gb = doc::findJiGroupByAnchor(r.clip(), b[0]);
  REQUIRE((ga != nullptr && gb != nullptr));
  const ClipView::GroupGeometry fa = r.view->groupGeometry(ga->id), fb = r.view->groupGeometry(gb->id);
  REQUIRE((fa.valid && fb.valid));
  CHECK(fa.headerText == "4:5:6");
  CHECK(fb.headerText == "10:12:15");
  CHECK(fa.box.x1 < fb.box.x0);  // side by side, not one box
  CHECK(r.view->activeJiGroup() == gb->id);
  CHECK(fb.gripsShown);
  CHECK_FALSE(fa.gripsShown);  // the corner grips are the active group's (or the one under the pointer)
  CHECK(std::fabs(*r.view->harmonicGuideY(10) - r.view->notePoint(b[0], 0.5).y) <= 0.75f);
  r.d.click(r.view->notePoint(a[1], 0.5));
  r.d.step();
  CHECK(r.view->activeJiGroup() == ga->id);
  CHECK(std::fabs(*r.view->harmonicGuideY(4) - r.view->notePoint(a[0], 0.5).y) <= 0.75f);
  // The pointer over the other frame shows its grips too.
  r.d.move(mid(fb.box));
  r.d.step();
  CHECK(r.view->groupGeometry(gb->id).gripsShown);
}

TEST_CASE("render: pianoroll-group-triad (a tuned triad: its frame, header 4:5:6, numbers, guides) matches the golden image") { checkGolden("pianoroll-group-triad"); }
TEST_CASE("render: pianoroll-group-chooser (the readings of a tuned seventh chord) matches the golden image") { checkGolden("pianoroll-group-chooser"); }
TEST_CASE("render: pianoroll-group-warp (tempered, warped into JI and back: the members glide, the states are named) matches the golden image") { checkGolden("pianoroll-group-warp"); }
TEST_CASE("render: pianoroll-group-two (two groups in one clip, each in its frame) matches the golden image") { checkGolden("pianoroll-group-two"); }
TEST_CASE("render: pianoroll-group-walk (22:26:30:33 duplicated and walked to 21: and 20:) matches the golden image") { checkGolden("pianoroll-group-walk"); }
TEST_CASE("render: pianoroll-group-proportion (a typed 8:9:10:12:15) matches the golden image") { checkGolden("pianoroll-group-proportion"); }
TEST_CASE("render: pianoroll-group-ratio (3/2 above harmonic 5: the labels read 10 and 15) matches the golden image") { checkGolden("pianoroll-group-ratio"); }
TEST_CASE("render: pianoroll-group-31edo (a tuned chord with a warp in 31-EDO: the labels name the clip's tuning) matches the golden image") { checkGolden("pianoroll-group-31edo"); }
