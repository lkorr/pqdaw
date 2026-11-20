// T7.13 piano roll: declaring JI on notes (the group UI — frames, Tune to JI, warps, walk, proportions, ratios — is
// test_piano_roll_groups.cpp). Every gesture goes
// through the real shell, pointer and key dispatch (TestDriver → UiTree → the note area / the keymap / the menus /
// the inline field), one undo step each:
//   "Is Harmonic…" in a note's menu (the note becomes the anchor of a JI group; the guides, the derived numbering,
//   drawing members on the guides, Up on a member = the next harmonic, the anchor moved, "Remove JI Group");
//   the active group follows the selection; a curved note is hit on its curve; copy / paste carries pitch curves and
//   a copied anchor with its members as a group of their own (a member alone pastes as the pitch it had).
// Goldens: pianoroll-curve-notes, pianoroll-fundamental-lock.
//
// Deleted with the JI object (T7.13): the JI tool (J, box, proportion field), undertone objects, moving / resizing /
// deleting / duplicating an object, the inner roll (enter / leave, voices as rows), stops, transitions, bezier
// handles, walking a term, object copy / paste, and the goldens pianoroll-ji-morph, -ji-undertone, -ji-tuning,
// -ji-entered, -tune-to-ji, -tune-to-ji-31edo. Tune to JI (in an Equidistant MOS roll too) and a paint budget for
// tuned chords are notes-first tests in test_piano_roll_groups.cpp (T7.16a).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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
#include "../ui/golden_check.h"
#include "views_test_util.h"
#include "../../src/views/src/piano_roll/roll_state.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using pq::tuning::PitchId;
using pq::tuning::PitchKind;
using pq::tuning::Rational;

namespace {

constexpr Ticks kQ = kPpq;

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)}; }

// The shell over the empty project with one new MIDI track and an empty four-bar clip open in the clip view.
struct Ji {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ClipView* view = nullptr;
  doc::ClipRef ref{};

  Ji() {
    REQUIRE(ctx.openProject(views::test::fixtureDir("empty")).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(480.0f);  // the detail view at its tallest
    doc::DocumentStore& st = ctx.store();
    auto t = doc::ops::createTrack(*ctx.doc(), st.ids(), doc::TrackKind::Midi, {}, "JI", doc::Colour{0x5FA8D8});
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
    view->centreOnDegree(8);
    view->setZoom(60.0 / static_cast<double>(kQ), 0.0);  // 60 px per beat
    view->scrollToTick(-0.5 * static_cast<double>(kQ));
    GridSpec g;
    g.mode = GridMode::Fixed;
    g.fixedExp = 2;  // 1/4
    ctx.setGrid(g);
    d.tree().layoutNow();
  }
  ~Ji() { d.tree().setRoot(std::make_unique<Widget>()); }

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
  size_t history() const { return ctx.store().history().cursor(); }
  Vec2 at(double tick, int64_t degree) const { return {view->xOfTick(tick), view->yOfDegree(degree)}; }
  double hzOf(doc::NoteId id) const {
    auto res = doc::resolveClipPitches(*ctx.doc(), ref.track, ref.clip);
    REQUIRE(res.ok());
    for (const doc::NotePitch& np : res->notes)
      if (np.note == id) {
        REQUIRE(np.hz.has_value());
        return *np.hz;
      }
    FAIL("note not resolved");
    return 0.0;
  }
  // A click in the note area's top-right corner gives it the focus (the pianoroll key scope).
  void focusNotes() {
    const IRect a = d.rect("main/body/bottom/detail/clip/clip-view/notes");
    REQUIRE_FALSE(a.empty());
    d.click({static_cast<float>(a.right()) - 6.0f, static_cast<float>(a.y) + 6.0f});
    d.step();
    REQUIRE(notesFocused());
  }
  bool notesFocused() { return d.tree().focus() == d.find("main/body/bottom/detail/clip/clip-view/notes"); }
  std::vector<doc::NoteId> addNotes(const std::vector<doc::Note>& notes) {
    auto e = doc::ops::addNotes(*ctx.doc(), ctx.store().ids(), ref, notes);
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    d.step();
    std::vector<doc::NoteId> ids;
    for (const doc::NoteKey& k : e->notes) ids.push_back(k.id);
    return ids;
  }
  static doc::Note plain(Ticks start, Ticks len, PitchId p) {
    doc::Note n;
    n.start = start;
    n.length = len;
    n.pitch = p;
    return n;
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
  void typeEnter(std::string_view text) {
    REQUIRE(field() != nullptr);
    REQUIRE(d.type(text));
    (void)d.press("Enter");
    d.step();
  }
  // "Is Harmonic… n" on a note, through its menu and the field.
  void declare(doc::NoteId id, std::string_view n) {
    choose(rightClick(view->notePoint(id, 0.5)), "Is Harmonic");
    typeEnter(n);
    REQUIRE(shell->overlay().openCount() == 0);
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

TEST_CASE("roll JI: parseProportion — whole ascending terms, and what it refuses") {
  auto p = ClipView::parseProportion("8:9:10:12:15");
  REQUIRE(p.ok());
  CHECK(*p == std::vector<int64_t>{8, 9, 10, 12, 15});
  p = ClipView::parseProportion(" 4 : 5:6 ");
  REQUIRE(p.ok());
  CHECK(*p == std::vector<int64_t>{4, 5, 6});
  p = ClipView::parseProportion("7");
  REQUIRE(p.ok());
  CHECK(*p == std::vector<int64_t>{7});
  // No subharmonics (1/4:1/5:1/6 is refused), nothing but whole ascending numbers.
  for (const char* bad : {"", "  ", "4:5:", "4::5", "4:x", "5:4", "4:4:5", "4:1/5", "1/4:1/5:1/6", "0:1", "5/4:3/2", "4;5", "-4:5"})
    CHECK_MESSAGE(!ClipView::parseProportion(bad).ok(), bad);
}

TEST_CASE("roll JI: a note with a pitch curve is hit on its curve, not on its row") {
  Ji r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Ji::plain(kQ, 4 * kQ, PitchId::fromDegree(0, 0))});
  auto e = doc::ops::setNoteCurve(*r.ctx.doc(), r.ref, doc::keyOf(r.note(ids[0])),
                                  {{0, 0.0f, 0.0f, {}}, {kQ, 0.0f, 0.0f, {}}, {2 * kQ, 300.0f, 0.0f, {}}, {4 * kQ, 300.0f, 0.0f, {}}});
  REQUIRE(e.ok());
  REQUIRE(r.ctx.store().dispatch(e->command).ok());
  r.d.step();
  // Its second half sounds (and is drawn) three rows up.
  const Vec2 bent = r.view->notePoint(ids[0], 0.75);
  CHECK(bent.y == doctest::Approx(r.view->yOfDegree(3)).epsilon(0.02));
  r.d.click(r.at(8.5 * static_cast<double>(kQ), 20));
  REQUIRE(r.view->selection().empty());
  r.d.click({bent.x, r.view->yOfDegree(0)});  // the row it is written on, where nothing is drawn
  CHECK(r.view->selection().empty());
  r.d.click(bent);
  REQUIRE(r.view->selection().size() == 1);
  const size_t h = r.history();
  r.d.drag(bent, {bent.x + static_cast<float>(kQ * r.view->pxPerTick()), bent.y}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.note(ids[0]).start == 2 * kQ);
}

TEST_CASE("roll JI: Is Harmonic… — the note sits exactly on guide N, drawn notes are exact ratios to it, the numbering is derived; Remove JI Group") {
  Ji r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Ji::plain(kQ, 2 * kQ, PitchId::fromDegree(4, 0))});  // E, 400 cents
  const doc::NoteId e = ids[0];
  const double eHz = r.hzOf(e);
  size_t h = r.history();
  CHECK(r.clip().groups == nullptr);
  CHECK_FALSE(r.view->activeJiGroup().valid());

  Menu* menu = r.rightClick(r.view->notePoint(e, 0.5));
  CHECK(Ji::itemOf(menu, "Is Subharmonic") < 0);  // not offered
  REQUIRE(Ji::itemOf(menu, "Tune to JI") >= 0);  // T7.14: a switch for a chord; one note has nothing to tune to
  CHECK_FALSE(menu->items()[static_cast<size_t>(Ji::itemOf(menu, "Tune to JI"))].enabled);
  REQUIRE(Ji::itemOf(menu, "Remove JI Group") >= 0);
  CHECK_FALSE(menu->items()[static_cast<size_t>(Ji::itemOf(menu, "Remove JI Group"))].enabled);
  r.choose(menu, "Is Harmonic");
  REQUIRE(r.field() != nullptr);
  REQUIRE(r.d.type("x"));
  (void)r.d.press("Enter");
  r.d.step();
  CHECK(r.clip().groups == nullptr);
  REQUIRE(r.shell->overlay().openCount() == 1);  // the field stays with its error
  (void)r.d.press("Backspace");
  r.typeEnter("5");
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.history() == h + 1);
  REQUIRE(doc::jiGroupsOf(r.clip()).size() == 1);
  const doc::JiGroupId group = doc::jiGroupsOf(r.clip())[0].id;
  CHECK(doc::jiGroupsOf(r.clip())[0].anchor == e);
  CHECK(doc::jiGroupsOf(r.clip())[0].index == 5);
  CHECK(r.view->activeJiGroup() == group);
  CHECK(r.view->showHarmonics());
  // The note is not changed and guide 5 passes through it.
  CHECK(r.note(e).pitch == PitchId::fromDegree(4, 0));
  CHECK(r.hzOf(e) == eHz);
  REQUIRE(r.view->harmonicGuideY(5).has_value());
  CHECK(std::fabs(*r.view->harmonicGuideY(5) - r.view->yOfDegree(4)) <= 0.5f);
  CHECK(r.view->harmonicOf(e) == 5u);

  // Drawing snaps to the series inside its time (T7.16a, M7 review H1: the note's span): a click on guide 6 is an
  // exact 6/5 of the anchor (a member); Shift: the row.
  REQUIRE(r.notesFocused());
  CHECK(r.d.press("B") == KeyResult::Action);
  r.d.click({r.view->xOfTick(2.5 * static_cast<double>(kQ)), *r.view->harmonicGuideY(6)});
  r.d.step();
  r.d.click(r.at(1.5 * static_cast<double>(kQ), 7), mod::kShift);
  r.d.step();
  (void)r.d.press("B");
  CHECK(r.history() == h + 3);
  // T7.14: the row note drawn last is selected and belongs to no group: no group is active (no guides). A click on
  // a note of the group brings its guides back.
  CHECK_FALSE(r.view->activeJiGroup().valid());
  r.d.click(r.view->notePoint(e, 0.5));
  REQUIRE(r.view->activeJiGroup() == group);
  doc::NoteId six;
  int tempered = 0;
  for (const doc::Note& n : r.clip().notes) {
    if (n.pitch == PitchId::fromRatio(Rational(6, 5), tuning::AnchorKind::Note, e.value) && n.start == 2 * kQ) six = n.id;
    if (n.pitch == PitchId::fromDegree(7, 0) && n.start == kQ) ++tempered;
  }
  REQUIRE(six.valid());
  CHECK(tempered == 1);
  CHECK(r.hzOf(six) == doctest::Approx(eHz * 6.0 / 5.0).epsilon(1e-12));
  CHECK(r.view->harmonicOf(six) == 6u);
  CHECK(std::fabs(r.view->notePoint(six, 0.5).y - *r.view->harmonicGuideY(6)) <= 1.5f);  // drawn on its guide

  // A note 3/2 above the anchor falls between the harmonics: the fundamental halves, everything renumbers.
  const doc::NoteId fifth = r.addNotes({Ji::plain(4 * kQ, kQ, PitchId::fromRatio(Rational(3, 2), tuning::AnchorKind::Note, e.value))})[0];
  r.d.click(r.view->notePoint(e, 0.5));  // T7.14: the guides are the selected note's group's (the row note drawn last has none)
  REQUIRE(r.view->activeJiGroup() == group);
  CHECK(r.view->harmonicOf(e) == 10u);
  CHECK(r.view->harmonicOf(fifth) == 15u);
  CHECK(r.view->harmonicOf(six) == 12u);
  CHECK(std::fabs(*r.view->harmonicGuideY(10) - r.view->yOfDegree(4)) <= 0.5f);
  CHECK(r.hzOf(e) == eHz);
  CHECK(r.hzOf(fifth) == doctest::Approx(eHz * 1.5).epsilon(1e-12));
  // Deleting it (click, Delete): 5 again; undo: 10; undo: the note was never there.
  h = r.history();
  r.d.click(r.view->notePoint(fifth, 0.5));
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.d.press("Delete") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.view->harmonicOf(e) == 5u);
  CHECK(r.view->harmonicOf(six) == 6u);
  REQUIRE(r.ctx.undo());
  CHECK(r.view->harmonicOf(e) == 10u);
  REQUIRE(r.ctx.undo());
  CHECK(r.view->harmonicOf(e) == 5u);

  // Up on the drawn note: the next harmonic of the group, still a member.
  h = r.history();
  r.d.click(r.view->notePoint(six, 0.5));
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.d.press("Up") == KeyResult::Action);
  CHECK(r.note(six).pitch == PitchId::fromRatio(Rational(7, 5), tuning::AnchorKind::Note, e.value));
  CHECK(r.view->harmonicOf(six) == 7u);
  CHECK(r.view->harmonicOf(e) == 5u);
  // bug-ji-input: Up on the anchor ALONE moves only it, to its next harmonic (5 -> 6); the member keeps its pitch and
  // carries the group now (the series stays). (The whole group selected moves as a chord: test_piano_roll_groups.)
  const double sevenHz = r.hzOf(six);
  r.d.click(r.view->notePoint(e, 0.5));
  CHECK(r.d.press("Up") == KeyResult::Action);
  CHECK(r.hzOf(six) == doctest::Approx(sevenHz).epsilon(1e-12));
  CHECK(r.hzOf(e) == doctest::Approx(sevenHz * 6.0 / 7.0).epsilon(1e-12));
  CHECK(doc::jiGroupsOf(r.clip())[0].anchor == six);
  CHECK(r.view->harmonicOf(e) == 6u);
  CHECK(r.view->harmonicOf(six) == 7u);
  CHECK(r.history() == h + 2);

  // Remove JI Group, from the same menu: the member keeps sounding where it is; undo brings the group back.
  const double sixHz = r.hzOf(six);
  h = r.history();
  r.choose(r.rightClick(r.view->notePoint(e, 0.5)), "Remove JI Group");
  CHECK(r.history() == h + 1);
  CHECK(r.clip().groups == nullptr);
  CHECK_FALSE(r.view->harmonicOf(e).has_value());
  CHECK(r.note(six).pitch.kind() == PitchKind::Cents);
  CHECK(r.hzOf(six) == doctest::Approx(sixHz).epsilon(1e-12));
  REQUIRE(r.ctx.undo());
  REQUIRE(doc::jiGroupsOf(r.clip()).size() == 1);
  CHECK(doc::jiGroupsOf(r.clip())[0].id == group);
  CHECK(r.view->harmonicOf(six) == 7u);
}

TEST_CASE("roll JI: two groups — the guides and the plain draw follow the selected note's group") {
  Ji r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids =
      r.addNotes({Ji::plain(0, 2 * kQ, PitchId::fromDegree(0, 0)), Ji::plain(4 * kQ, 2 * kQ, PitchId::fromDegree(9, 0))});
  r.declare(ids[0], "4");
  r.declare(ids[1], "3");
  REQUIRE(doc::jiGroupsOf(r.clip()).size() == 2);
  const doc::JiGroup* a = doc::findJiGroupByAnchor(r.clip(), ids[0]);
  const doc::JiGroup* b = doc::findJiGroupByAnchor(r.clip(), ids[1]);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK(r.view->activeJiGroup() == b->id);  // the note just declared
  CHECK(std::fabs(*r.view->harmonicGuideY(3) - r.view->yOfDegree(9)) <= 0.5f);
  r.d.click(r.view->notePoint(ids[0], 0.5));
  CHECK(r.view->activeJiGroup() == a->id);
  CHECK(std::fabs(*r.view->harmonicGuideY(4) - r.view->yOfDegree(0)) <= 0.5f);
  CHECK(r.view->harmonicOf(ids[0]) == 4u);  // every group's notes carry their numbers, active or not
  CHECK(r.view->harmonicOf(ids[1]) == 3u);
  // A double-click on guide 5 of the first group's series, inside its time, adds a member of THAT group.
  const size_t h = r.history();
  r.d.doubleClick({r.view->xOfTick(1.5 * static_cast<double>(kQ)), *r.view->harmonicGuideY(5)});
  r.d.step();
  CHECK(r.history() == h + 1);
  bool found = false;
  for (const doc::Note& n : r.clip().notes)
    found = found || n.pitch == PitchId::fromRatio(Rational(5, 4), tuning::AnchorKind::Note, ids[0].value);
  CHECK(found);
  // A click on empty space deselects; the group stays active (its guides stay), so the next double-click still lands
  // on its series.
  r.d.click(r.at(10.5 * static_cast<double>(kQ), 12));
  r.d.step();
  REQUIRE(r.view->selection().empty());
  CHECK(r.view->activeJiGroup() == a->id);
  // T7.14: Esc puts the group away: no guides, the roll is back on the tuning's rows — a double-click adds a plain
  // Degree note. (T7.13 kept the last group active for ever; then nothing but Shift could leave JI.)
  CHECK(r.d.press("Escape") == KeyResult::Action);
  r.d.step();
  REQUIRE(r.view->selection().empty());
  CHECK_FALSE(r.view->activeJiGroup().valid());
  r.d.doubleClick(r.at(8.5 * static_cast<double>(kQ), 5));
  r.d.step();
  bool row = false;
  for (const doc::Note& n : r.clip().notes) row = row || (n.start == 8 * kQ && n.pitch == PitchId::fromDegree(5, 0));
  CHECK(row);
  // A note of a group deleted (an edit, not a deselect): its group stays active for the next note.
  r.d.click(r.view->notePoint(ids[1], 0.5));
  REQUIRE(r.view->activeJiGroup() == b->id);
  const doc::NoteId extra = r.addNotes({Ji::plain(6 * kQ, kQ, PitchId::fromRatio(Rational(4, 3), tuning::AnchorKind::Note, ids[1].value))})[0];
  r.d.click(r.view->notePoint(extra, 0.5));
  CHECK(r.d.press("Delete") == KeyResult::Action);
  r.d.step();
  REQUIRE(r.view->selection().empty());
  CHECK(r.view->activeJiGroup() == b->id);
}

TEST_CASE("roll JI: copy / paste carries a note's pitch curve; an anchor pastes with its members as a new group, a member alone as the pitch it had") {
  Ji r;
  r.focusNotes();
  // A Degree note with a bend.
  const doc::NoteId bentNote = r.addNotes({Ji::plain(0, 2 * kQ, PitchId::fromDegree(5, 0))})[0];
  const std::vector<doc::Breakpoint> bend = {{0, 0.0f, 0.0f, {}}, {kQ, 150.0f, 0.0f, {}}, {2 * kQ, 150.0f, 0.0f, {}}};
  auto c = doc::ops::setNoteCurve(*r.ctx.doc(), r.ref, doc::keyOf(r.note(bentNote)), bend);
  REQUIRE(c.ok());
  REQUIRE(r.ctx.store().dispatch(c->command).ok());
  r.d.step();

  r.view->select(std::span<const doc::NoteId>(&bentNote, 1));
  CHECK(r.d.press("Ctrl+C") == KeyResult::Action);
  // (BR.D: a paste in place overwrites the original; these pastes go after it, where nothing is.)
  auto pasteAfter = [&](Ticks t) { r.view->state().setInsertTick(t); };
  pasteAfter(2 * kQ);
  size_t h = r.history();
  CHECK(r.d.press("Ctrl+V") == KeyResult::Action);
  CHECK(r.history() == h + 1);
  REQUIRE(r.clip().notes.size() == 2);
  REQUIRE(r.view->selection().size() == 1);
  const doc::NoteId pasted{r.view->selection()[0]};
  CHECK(pasted != bentNote);
  CHECK(r.note(pasted).start == 2 * kQ);
  const doc::NoteCurves* pc = doc::findNoteCurves(r.clip(), pasted);
  REQUIRE(pc != nullptr);
  CHECK(pc->pitch == bend);
  REQUIRE(r.ctx.undo());
  CHECK(r.clip().notes.size() == 1);

  // A group: an anchor (harmonic 4) and its 7/4 member.
  const doc::NoteId anchor = r.addNotes({Ji::plain(4 * kQ, kQ, PitchId::fromDegree(0, 0))})[0];
  r.declare(anchor, "4");
  const doc::NoteId member = r.addNotes({Ji::plain(4 * kQ, kQ, PitchId::fromRatio(Rational(7, 4), tuning::AnchorKind::Note, anchor.value))})[0];
  REQUIRE(r.view->harmonicOf(member) == 7u);
  // The member alone: the paste is a Cents note at the pitch it had (it depends on nothing).
  r.view->select(std::span<const doc::NoteId>(&member, 1));
  (void)r.d.press("Ctrl+C");
  pasteAfter(6 * kQ);
  h = r.history();
  (void)r.d.press("Ctrl+V");
  CHECK(r.history() == h + 1);
  REQUIRE(r.view->selection().size() == 1);
  const doc::Note& alone = r.note(doc::NoteId{r.view->selection()[0]});
  REQUIRE(alone.pitch.kind() == PitchKind::Cents);
  CHECK(alone.pitch.cents() == doctest::Approx(tuning::cents(Rational(7, 4))));
  CHECK(doc::jiGroupsOf(r.clip()).size() == 1);
  REQUIRE(r.ctx.undo());
  // Both: the copies are a group of their own with the same index.
  const doc::NoteId both[] = {anchor, member};
  r.view->select(std::span<const doc::NoteId>(both, 2));
  (void)r.d.press("Ctrl+C");
  pasteAfter(6 * kQ);
  h = r.history();
  (void)r.d.press("Ctrl+V");
  CHECK(r.history() == h + 1);
  REQUIRE(r.view->selection().size() == 2);
  REQUIRE(doc::jiGroupsOf(r.clip()).size() == 2);
  const doc::NoteId p0{r.view->selection()[0]}, p1{r.view->selection()[1]};
  const doc::JiGroup* copy = doc::findJiGroupByAnchor(r.clip(), p0);
  REQUIRE(copy != nullptr);
  CHECK(copy->index == 4);
  CHECK(r.note(p1).pitch == PitchId::fromRatio(Rational(7, 4), tuning::AnchorKind::Note, p0.value));
  CHECK(r.view->harmonicOf(p1) == 7u);
  CHECK(r.hzOf(p1) == r.hzOf(member));
  REQUIRE(r.ctx.undo());
  CHECK(doc::jiGroupsOf(r.clip()).size() == 1);
}

TEST_CASE("render: JI pianoroll-curve-notes (notes along their pitch curves) matches the golden image") { checkGolden("pianoroll-curve-notes"); }
TEST_CASE("render: JI pianoroll-fundamental-lock (Is Harmonic 5: the group's guides, members on the harmonics) matches the golden image") { checkGolden("pianoroll-fundamental-lock"); }
