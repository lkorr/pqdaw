// T3.19 Tunings and identities UI (M3 review H7), through the real shell:
//   Menu bar: Live's File / Edit / Create / View / Options / Help; items run registered actions with the keymap's chords,
//   greyed while unavailable; hovering another title while a menu is open switches to it, clicking the open title
//   closes it; Edit acts on the focused editor.
//   Scripted acceptance (TASKS T3.19): create "Tuning B: 22edo" (Create > New Main Tuning, the name typed, Steps 22),
//   assign a track (its header chip's menu), automate it (the panel's Show Automation Lane + a click on the lane; and a
//   MOS tuning's generator: the panel box is a Param-only gesture, then its lane), enter 5/4 → Up → 6/4 → 7/4, convert a
//   Degree note to Ratio and back (the note menu) — each exactly one undo step, undone in order.
//   Shift-draw on the virtual fundamental's harmonics; row label modes; ratio note labels; the pitch type-in; the hover
//   readout in the info view repainting only the info view; panel: reference (Param only), an exact 3/1 equave (13ed3),
//   remove with reassignment, colour.
//   Goldens: menu-bar, tunings-panel, pianoroll-ratios, pianoroll-note-menu.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/tuning/scala.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/tunings/tunings_panel.h"
#include "pq/views/view_scenes.h"
#include "../../src/views/src/piano_roll/roll_state.h"
#include "../../src/views/src/shell/shell_widgets.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
namespace tu = pq::tuning;

namespace {

constexpr Ticks kBar = kTicksPerWhole;

template <class W>
W* findWidget(Widget& root, const std::function<bool(W&)>& pred) {
  if (auto* w = dynamic_cast<W*>(&root); w && pred(*w)) return w;
  for (size_t i = 0; i < root.childCount(); ++i)
    if (W* f = findWidget<W>(root.child(i), pred)) return f;
  return nullptr;
}

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + static_cast<float>(r.w) / 2.0f, static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f}; }

// The shell over a project with track Bass (clip Riff: 12-EDO degrees 0, 4, 7 and 9), the clip in the clip view.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  doc::TrackId bass;
  doc::ClipRef riff;
  std::vector<doc::NoteId> notes;

  Ui() {
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE(e.ok());
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    bass = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Bass")).tracks[0];
    riff = run(doc::ops::insertEmptyClip(*st.current(), st.ids(), bass, 0, 2 * kBar, "Riff")).clips[0];
    std::vector<doc::Note> ns(4);
    const int32_t degrees[] = {0, 4, 7, 9};
    for (size_t i = 0; i < 4; ++i) {
      ns[i].start = static_cast<Ticks>(i) * kPpq;
      ns[i].length = kPpq;
      ns[i].pitch = tu::PitchId::fromDegree(degrees[i], 0);
    }
    const auto added = run(doc::ops::addNotes(*st.current(), st.ids(), riff, ns));
    for (const doc::NoteKey& k : added.notes) notes.push_back(k.id);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(320.0f);
    ctx.setDetailClip(riff);
    d.tree().layoutNow();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  shell::MenuBar& bar() const {
    auto* b = dynamic_cast<shell::MenuBar*>(d.find("main/menu-bar"));
    REQUIRE(b != nullptr);
    return *b;
  }
  TuningsPanel& panel() const {
    auto* p = dynamic_cast<TuningsPanel*>(shell->slot(ShellSlot::Tunings).find("tunings-panel"));
    REQUIRE(p != nullptr);
    return *p;
  }
  ClipView& view() const {
    auto* v = dynamic_cast<ClipView*>(d.find("main/body/bottom/detail/clip/clip-view"));
    REQUIRE(v != nullptr);
    return *v;
  }
  ArrangementView& arrangement() const {
    auto* a = dynamic_cast<ArrangementView*>(shell->slot(ShellSlot::TrackArea).find("arrangement"));
    REQUIRE(a != nullptr);
    return *a;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::ChangeSet& lastChanges() const { return ctx.store().history().entries()[history() - 1].changes; }
  const doc::Note& note(doc::NoteId id) const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), riff.track);
    REQUIRE(t != nullptr);
    const doc::Note* n = t->clips[static_cast<size_t>(doc::findClipIndex(*t, riff.clip))].clip->notes.findById(id);
    REQUIRE(n != nullptr);
    return *n;
  }
  const doc::NoteClip& clip() const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), riff.track);
    return *t->clips[static_cast<size_t>(doc::findClipIndex(*t, riff.clip))].clip;
  }
  // The open menu (the overlay's newest popup) and the index of the item labelled `label` (prefix match).
  Menu& openMenu() const {
    auto* m = dynamic_cast<Menu*>(shell->overlay().top());
    REQUIRE(m != nullptr);
    return *m;
  }
  int32_t itemIndex(const Menu& m, std::string_view label) const {
    for (size_t i = 0; i < m.items().size(); ++i)
      if (m.items()[i].label.rfind(label, 0) == 0) return static_cast<int32_t>(i);
    FAIL("no menu item " << label);
    return -1;
  }
  void choose(std::string_view label) {
    INFO("choosing " << label);
    Menu& m = openMenu();
    const int32_t i = itemIndex(m, label);
    REQUIRE(m.selectable(i));
    m.choose(i);
    d.step();
  }
  // Types into a DragBox (as a double-click and typing do) and commits with Enter.
  void typeInto(DragBox& b, std::string_view text) {
    d.tree().setFocus(&b);
    b.beginEdit();
    d.type(text);
    d.press("Return");
  }
};

}  // namespace

// ---- menu bar ---------------------------------------------------------------------------------------------------------

TEST_CASE("menu bar: Live's six menus; items run actions with their chords; hover switches, a click on the open title closes") {
  Ui u;
  shell::MenuBar& bar = u.bar();
  for (int32_t i = 1; i < shell::MenuBar::kMenus; ++i) CHECK(bar.titleRect(i).x > bar.titleRect(i - 1).x);
  CHECK(std::string(shell::MenuBar::kTitles[0]) == "File");
  CHECK(std::string(shell::MenuBar::kTitles[5]) == "Help");
  // Closed menus do not exist.
  CHECK(u.shell->overlay().openCount() == 0);
  u.d.click(mid(bar.titleRect(0)));
  CHECK(bar.openIndex() == 0);
  REQUIRE(u.shell->overlay().openCount() == 1);
  Menu& file = u.openMenu();
  const MenuItem& newSet = file.items()[static_cast<size_t>(u.itemIndex(file, "New Set"))];
  CHECK(newSet.action == "file.new");
  CHECK(newSet.shortcut == "Ctrl+N");
  // The pointer moving over Edit switches menus; a click on Edit (open) closes it.
  u.d.move(mid(bar.titleRect(1)));
  CHECK(bar.openIndex() == 1);
  CHECK(u.shell->overlay().openCount() == 1);
  u.d.click(mid(bar.titleRect(1)));
  CHECK(bar.openIndex() == -1);
  u.d.step();
  CHECK(u.shell->overlay().openCount() == 0);
  // Create > Insert MIDI Track: one undo step; Edit then offers to undo it by name.
  const size_t h0 = u.history();
  const size_t tracks = u.ctx.doc()->tracks.size();
  u.d.click(mid(bar.titleRect(2)));
  CHECK(u.openMenu().items()[static_cast<size_t>(u.itemIndex(u.openMenu(), "Insert MIDI Track"))].shortcut == "Ctrl+Shift+T");
  u.choose("Insert MIDI Track");
  CHECK(u.history() == h0 + 1);
  CHECK(u.ctx.doc()->tracks.size() == tracks + 1);
  const std::vector<MenuItem> edit = bar.items(1);
  CHECK(edit[0].label == "Undo Insert MIDI Track");
  CHECK(edit[0].enabled);
  CHECK_FALSE(edit[1].enabled);  // nothing to redo
  // View > Tunings shows the panel (checked next time); Options holds Preferences (greyed without an audio port).
  u.d.click(mid(bar.titleRect(3)));
  u.choose("Tunings");
  CHECK(u.ctx.tuningsVisible());
  CHECK(u.shell->slot(ShellSlot::Tunings).visible());
  const std::vector<MenuItem> view = bar.items(3);
  CHECK(std::find_if(view.begin(), view.end(), [](const MenuItem& m) { return m.label == "Tunings" && m.checked; }) != view.end());
  const std::vector<MenuItem> options = bar.items(4);
  const auto prefs = std::find_if(options.begin(), options.end(), [](const MenuItem& m) { return m.action == "app.preferences"; });
  REQUIRE(prefs != options.end());
  CHECK_FALSE(prefs->enabled);
  // Edit acts on the focused editor: the clip view's note area → the pianoroll actions.
  auto* notesArea = u.d.find("main/body/bottom/detail/clip/clip-view/notes");
  REQUIRE(notesArea != nullptr);
  u.d.tree().setFocus(notesArea);
  const std::vector<MenuItem> rollEdit = bar.items(1);
  CHECK(std::any_of(rollEdit.begin(), rollEdit.end(), [](const MenuItem& m) { return m.action == "pianoroll.copy"; }));
  CHECK(std::none_of(rollEdit.begin(), rollEdit.end(), [](const MenuItem& m) { return m.action == "arrangement.copy"; }));
}

// ---- the scripted acceptance ----------------------------------------------------------------------------------------------

TEST_CASE("T3.19 acceptance: Tuning B: 22edo, assign, automate, 5/4 -> Up -> 6/4 -> 7/4, Degree -> Ratio -> Degree; one undo step each") {
  Ui u;
  // The fixture's own edits are history entries too: count from here. snaps[i] = the document after i entries.
  const size_t base = u.history();
  std::vector<doc::Snapshot> snaps(base + 1);
  snaps[base] = u.ctx.doc();
  auto stepDone = [&](const char* what) {
    INFO(std::string(what));
    REQUIRE(u.history() == snaps.size());  // exactly one more history entry than before
    snaps.push_back(u.ctx.doc());
  };

  // 1. Create > New Main Tuning: "Tuning B" (12-EDO), the panel shown with the cursor in its name.
  u.d.click(mid(u.bar().titleRect(2)));
  u.choose("New Main Tuning");
  stepDone("new tuning");
  REQUIRE(u.ctx.doc()->tunings->size() == 2);
  const doc::TuningId B = u.ctx.doc()->tunings->back()->id;
  CHECK(u.ctx.doc()->tunings->back()->name == "Tuning B");
  CHECK(u.ctx.tuningsVisible());
  TuningCard* card = u.panel().card(B);
  REQUIRE(card != nullptr);
  CHECK(u.d.tree().focus() == &card->nameField());
  // 2. The name typed over the selection.
  u.d.type("Tuning B: 22edo");
  u.d.press("Return");
  stepDone("rename");
  CHECK(doc::findTuning(*u.ctx.doc(), B)->name == "Tuning B: 22edo");
  // 3. Steps: 22 (SetTuningDef, once at the end of the type-in).
  card = u.panel().card(B);
  REQUIRE(card->row(std::nullopt) != nullptr);
  u.typeInto(card->row(std::nullopt)->box(), "22");
  stepDone("22 steps");
  REQUIRE(doc::findTuning(*u.ctx.doc(), B)->def.edo() != nullptr);
  CHECK(doc::findTuning(*u.ctx.doc(), B)->def.edo()->n() == 22u);
  CHECK(*doc::findTuning(*u.ctx.doc(), B)->def.edo()->equaveRatio() == tu::Rational(2));
  CHECK(u.panel().card(B)->summary() == "22-EDO");

  // 4. Assign Bass: its header chip's menu (right-click) → "Tuning B: 22edo".
  u.d.step();
  TuningChip* chip = findWidget<TuningChip>(*u.d.root(), [&](TuningChip& c) {
    return c.target() && c.target()->kind == TuningTarget::Kind::Track && c.target()->track == u.bass && c.rect().w > 0 &&
           c.parent() && c.parent()->name() != "control-bar";
  });
  REQUIRE(chip != nullptr);
  u.d.click(mid(chip->rect()), mod::kNone, PointerButton::Right);
  u.choose("Tuning B: 22edo");
  stepDone("assign");
  CHECK(doc::findTrackPtr(*u.ctx.doc(), u.bass)->tuning == doc::TuningAssignment::mainTuning(B));
  CHECK(u.panel().card(B)->usage() == "1 track");

  // 5. Automate: the panel's Show Automation Lane for the equave (an EDO's continuous parameter; 22-EDO has no
  //    generator), then a click on the lane's line creates the lane and its first point in one step.
  u.panel().reveal(B);
  card = u.panel().card(B);
  TuningParamRow* eqRow = card->row(doc::ParamKind::TuningEquave);
  REQUIRE(eqRow != nullptr);
  u.d.click({static_cast<float>(eqRow->rect().x) + 20.0f, mid(eqRow->rect()).y}, mod::kNone, PointerButton::Right);
  u.choose("Show Automation Lane");
  CHECK(u.history() == snaps.size() - 1);  // showing a lane is view state
  const doc::ParamRef eqRef = doc::ParamRef::tuning(doc::ParamKind::TuningEquave, B);
  ArrangementAutomation& au = u.arrangement().automation();
  const std::vector<doc::ParamRef> shown = au.shownLanes(u.bass);
  CHECK(std::find(shown.begin(), shown.end(), eqRef) != shown.end());
  u.d.tree().layoutNow();
  const std::optional<AutomationBand> band = au.band(u.bass, eqRef, false);
  REQUIRE(band.has_value());
  const double tick = 1.0 * static_cast<double>(kBar);
  u.d.click({u.arrangement().xOfTick(tick), au.yOfValue(*band, currentParamValue(*u.ctx.doc(), eqRef))});
  stepDone("automate equave");
  CHECK(findAutomationLane(*u.ctx.doc(), eqRef).lane != nullptr);
  CHECK(u.lastChanges().structural());  // AddAutomationLane (the parameter becomes a signal)
  u.d.step();
  CHECK(u.panel().card(B)->row(doc::ParamKind::TuningEquave)->automated());

  // 5b. A MOS main tuning's generator: New, the wheel turns it into 5L 2s (one step), the Generator box steps it
  //     (Param only: no recompile), Show Automation Lane + a click.
  u.d.click(mid(u.panel().find("header/new")->rect()));
  stepDone("new tuning C");
  const doc::TuningId C = u.ctx.doc()->tunings->back()->id;
  u.d.press("Escape");  // leave the name field
  MosWheelPopover* wheel = openMosWheel(*u.d.root(), u.ctx, TuningTarget::forMain(C));
  REQUIRE(wheel != nullptr);
  REQUIRE(wheel->chooseMos({5, 2}, 696.77));
  wheel->apply();
  u.d.step();
  stepDone("C = 5L 2s");
  TuningCard* cardC = u.panel().card(C);
  REQUIRE(cardC != nullptr);
  REQUIRE(cardC->row(doc::ParamKind::MosGenerator) != nullptr);
  DragBox& gen = cardC->row(doc::ParamKind::MosGenerator)->box();
  const double g0 = doc::findTuning(*u.ctx.doc(), C)->def.mos()->generator;
  u.d.tree().setFocus(&gen);
  u.d.press("Up");
  stepDone("generator step");
  CHECK(u.lastChanges().paramOnly());
  CHECK(doc::findTuning(*u.ctx.doc(), C)->def.mos()->generator == doctest::Approx(g0 + 0.1));
  const doc::ParamRef genRef = doc::ParamRef::tuning(doc::ParamKind::MosGenerator, C);
  REQUIRE(u.panel().showAutomationLane(genRef));
  u.d.tree().layoutNow();
  const std::optional<AutomationBand> gband = au.band(u.bass, genRef, false);
  REQUIRE(gband.has_value());
  u.d.click({u.arrangement().xOfTick(2.0 * static_cast<double>(kBar)), au.yOfValue(*gband, currentParamValue(*u.ctx.doc(), genRef))});
  stepDone("automate generator");
  CHECK(findAutomationLane(*u.ctx.doc(), genRef).lane != nullptr);

  // 6. Ratio entry: the first note (Degree 0) typed as 5/4, then Up → 6/4 (= 3/2 over F = 1/4) → 7/4.
  ClipView& v = u.view();
  v.fitToClip();
  u.d.tree().layoutNow();
  REQUIRE_FALSE(v.noteRect(u.notes[0]).empty());
  u.d.click(mid(v.noteRect(u.notes[0])));  // selects it, focus in the note area
  u.d.step();
  REQUIRE(v.selection().size() == 1);
  TextEdit* entry = v.openPitchEntry();
  REQUIRE(entry != nullptr);
  CHECK(u.d.tree().focus() == entry);
  u.d.type("5/4");
  u.d.press("Return");
  stepDone("5/4");
  CHECK(u.note(u.notes[0]).pitch == tu::PitchId::fromRatio(tu::Rational(5, 4)));
  CHECK(v.virtualFundamental() == tu::Rational(1, 4));
  u.d.step();
  u.d.press("Up");
  stepDone("Up");
  CHECK(u.note(u.notes[0]).pitch == tu::PitchId::fromRatio(tu::Rational(3, 2)));  // 6/4, stored reduced
  CHECK(v.virtualFundamental() == tu::Rational(1, 4));                          // the step memory keeps 1/4
  u.d.press("Up");
  stepDone("Up again");
  CHECK(u.note(u.notes[0]).pitch == tu::PitchId::fromRatio(tu::Rational(7, 4)));

  // 7. The note menu: the third note (22-EDO degree 7 = 381.8 ¢) → Ratio (5/4) → Degree (7) again.
  u.d.step();
  REQUIRE_FALSE(v.noteRect(u.notes[2]).empty());
  u.d.click(mid(v.noteRect(u.notes[2])), mod::kNone, PointerButton::Right);
  u.choose("Ratio");
  stepDone("to ratio");
  CHECK(u.note(u.notes[2]).pitch == tu::PitchId::fromRatio(tu::Rational(5, 4)));
  u.d.click(mid(v.noteRect(u.notes[2])), mod::kNone, PointerButton::Right);
  u.choose("Degree");
  stepDone("to degree");
  CHECK(u.note(u.notes[2]).pitch == tu::PitchId::fromDegree(7, 0));

  // Every step undoes alone, in order, back to the start; redo replays them.
  for (size_t i = snaps.size() - 1; i > base; --i) {
    REQUIRE(u.ctx.undo());
    CHECK(u.ctx.doc() == snaps[i - 1]);
  }
  for (size_t i = base + 1; i < snaps.size(); ++i) {
    REQUIRE(u.ctx.redo());
    CHECK(u.ctx.doc() == snaps[i]);
  }
}

// ---- ratio entry, labels, readout -------------------------------------------------------------------------------------

TEST_CASE("roll: Shift-draw puts Ratio notes on the virtual fundamental's harmonics (else the simplest ratio of the row)") {
  Ui u;
  ClipView& v = u.view();
  u.d.step();
  // No ratio notes yet: Shift+double-click on 12-EDO's degree 4 row (400 ¢) → 5/4.
  const float x = v.xOfTick(static_cast<double>(6 * kPpq));
  u.d.move({x, v.yOfDegree(4)}, mod::kShift);
  u.d.doubleClick({x, v.yOfDegree(4)}, mod::kShift);
  auto find = [&](tu::PitchId p) {
    for (const doc::Note& n : u.clip().notes)
      if (n.pitch == p) return true;
    return false;
  };
  CHECK(find(tu::PitchId::fromRatio(tu::Rational(5, 4))));
  CHECK(v.virtualFundamental() == tu::Rational(1, 4));
  // Draw mode + Shift on the 700-¢ row: the harmonic of F = 1/4 nearest it is 6 → 6/4 = 3/2; on 1000 ¢: 7/4.
  u.ctx.setDrawMode(true);
  const float x2 = v.xOfTick(static_cast<double>(5 * kPpq) + 10.0);
  u.d.move({x2, v.yOfDegree(7)}, mod::kShift);
  u.d.click({x2, v.yOfDegree(7)}, mod::kShift);
  CHECK(find(tu::PitchId::fromRatio(tu::Rational(3, 2))));
  const float x3 = v.xOfTick(static_cast<double>(7 * kPpq) + 10.0);
  u.d.click({x3, v.yOfDegree(10)}, mod::kShift);
  CHECK(find(tu::PitchId::fromRatio(tu::Rational(7, 4))));
}

TEST_CASE("roll: row label modes (Auto = ratios for a just Scala scale), ratio note labels, the pitch type-in") {
  Ui u;
  ClipView& v = u.view();
  const roll::RowModel& rows = v.rows();
  auto labelOf = [&](int64_t degree) {
    const int64_t i = v.rows().indexOfDegree(degree);
    REQUIRE(i >= 0);
    return v.rows().label(static_cast<size_t>(i));
  };
  CHECK(labelOf(0) == "C3");  // Auto on 12-EDO = names
  v.setRowLabels(roll::RowLabels::Ratio);
  CHECK(labelOf(0) == "1/1");
  CHECK(labelOf(4) == "~5/4");
  CHECK(labelOf(7) == "~3/2");
  CHECK(labelOf(9) == "~5/3");
  v.setRowLabels(roll::RowLabels::Cents);
  CHECK(labelOf(4) == "400.0");
  CHECK(labelOf(16) == "400.0");  // within its equave
  v.setRowLabels(roll::RowLabels::Degree);
  CHECK(labelOf(4) == "4\\12");
  CHECK(rows.labelMode() == roll::RowLabels::Degree);
  // A just Scala scale: Auto reads its ratios (M3 review: 1/1 9/8 5/4 …, not 0…6).
  const char* scl = "! just.scl\nJust major\n 7\n!\n 9/8\n 5/4\n 4/3\n 3/2\n 5/3\n 15/8\n 2/1\n";
  auto parsed = tu::parseScl(scl, "just.scl");
  REQUIRE(parsed.ok());
  auto def = doc::makeScaleDef(std::move(*parsed), std::nullopt);
  REQUIRE(def.ok());
  REQUIRE(u.ctx.store().dispatch(doc::SetClipTuning{u.riff, doc::TuningAssignment::privateTuning(*def)}).ok());
  v.setRowLabels(roll::RowLabels::Auto);
  CHECK(v.rows().labelMode() == roll::RowLabels::Ratio);
  CHECK(labelOf(1) == "9/8");
  CHECK(labelOf(2) == "5/4");
  CHECK(labelOf(6) == "15/8");
  CHECK(labelOf(7) == "1/1");

  // The type-in's syntax.
  CHECK(v.parsePitch("7/4") == tu::PitchId::fromRatio(tu::Rational(7, 4)));
  CHECK(v.parsePitch(" 14:8 ") == tu::PitchId::fromRatio(tu::Rational(7, 4)));
  CHECK(v.parsePitch("386.3c") == tu::PitchId::fromCents(386.3));
  CHECK(v.parsePitch("-50\xC2\xA2") == tu::PitchId::fromCents(-50.0));
  CHECK(v.parsePitch("4\\7") == tu::PitchId::fromDegree(4, 0));
  CHECK(v.parsePitch("4\\7+1") == tu::PitchId::fromDegree(4, 1));
  CHECK(v.parsePitch("4\\7-2") == tu::PitchId::fromDegree(4, -2));
  CHECK_FALSE(v.parsePitch("4\\12").has_value());  // the clip's scale has 7 degrees
  CHECK_FALSE(v.parsePitch("0/4").has_value());
  CHECK_FALSE(v.parsePitch("seven").has_value());

  // Ratio notes carry "n/d" (and their harmonic over F) when painted.
  REQUIRE(u.d.enableGpu().ok());
  const doc::NoteId first = u.notes[0];
  v.select(std::span<const doc::NoteId>(&first, 1));
  REQUIRE(v.enterPitch("7/4"));
  const doc::NoteId second = u.notes[1];
  v.select(std::span<const doc::NoteId>(&second, 1));
  REQUIRE(v.enterPitch("5/4"));
  v.setZoom(v.pxPerTick() * 3.0, 0.0);
  u.d.step();
  const std::vector<roll::NoteLabel>& labels = v.state().noteLabels;
  auto has = [&](int64_t n, int64_t d, uint32_t k) {
    return std::any_of(labels.begin(), labels.end(), [&](const roll::NoteLabel& l) { return l.num == n && l.den == d && l.harmonic == k; });
  };
  CHECK(has(7, 4, 7));
  CHECK(has(5, 4, 5));
}

TEST_CASE("roll: the hover readout fills the info view and repaints only the info view") {
  Ui u;
  ClipView& v = u.view();
  REQUIRE(u.d.enableGpu().ok());
  u.d.step();
  const float x = v.xOfTick(static_cast<double>(7 * kPpq) + 10.0);  // past the notes
  u.d.move({x, v.yOfDegree(4)});
  u.d.step();
  CHECK(u.shell->infoTitle() == "Row E3");
  CHECK(u.shell->infoBody().find("400.00 \xC2\xA2 \xC2\xB7 329.63 Hz \xC2\xB7 \xE2\x89\x88 5/4 (+13.69 \xC2\xA2)") == 0);
  // Another row: only the info view is damaged (the note area kept its hover state).
  u.d.move({x, v.yOfDegree(7)});
  IRect rects[Damage::kMaxRects];
  int64_t area = 0;
  const uint32_t n = u.d.tree().damageRects(rects, area);
  const IRect info = u.d.rect("main/body/bottom/info");
  REQUIRE(n >= 1);
  for (uint32_t i = 0; i < n; ++i) CHECK(intersect(rects[i], info) == rects[i]);
  CHECK(u.shell->infoTitle() == "Row G3");
  // Cost of a row change (format + the info view's re-read), measured: informational, not asserted (load-sensitive).
  {
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kMoves = 2000;
    for (int i = 0; i < kMoves; ++i) (void)v.readoutAt({x, v.yOfDegree(i % 2 ? 7 : 9)}, false);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kMoves;
    MESSAGE("hover readout: " << us << " us per row change (format + info view re-read)");
  }
  // On a note: its identity; harmonic number for a reference ratio.
  const doc::NoteId first = u.notes[0];
  v.select(std::span<const doc::NoteId>(&first, 1));
  REQUIRE(v.enterPitch("5/4"));
  u.d.step();
  const InfoText t = v.readoutAt(mid(v.noteRect(first)), false);
  CHECK(std::string(t.title) == "Ratio Note 5/4");
  CHECK(std::string(t.body).find("harmonic 5 of F = 1/4") != std::string::npos);
  // With a background EDO… (degree layouts only show it in scale views; the readout names it for any tuning).
  u.d.leave();
  u.d.step();
  CHECK(u.shell->infoTitle() != "Row G3");
}

// ---- the panel --------------------------------------------------------------------------------------------------------

TEST_CASE("tunings panel: reference is a Param edit, an exact 3/1 equave (13ed3), colour, remove with reassignment") {
  Ui u;
  u.ctx.setTuningsVisible(true);
  u.d.step();
  const doc::TuningId B = u.panel().addTuning();
  REQUIRE(B.valid());
  u.d.press("Escape");
  TuningCard* card = u.panel().card(B);
  REQUIRE(card != nullptr);
  // Reference: typed Hz, a Param-only change (no recompile), one step.
  size_t h = u.history();
  u.typeInto(card->row(doc::ParamKind::TuningReference)->box(), "440");
  CHECK(u.history() == h + 1);
  CHECK(u.lastChanges().paramOnly());
  CHECK(doc::findTuning(*u.ctx.doc(), B)->def.referenceHz == doctest::Approx(440.0).epsilon(1e-12));
  // Bohlen–Pierce: 13 steps of an exact 3/1 (H7: the wheel's EDO field forced 2/1).
  u.typeInto(card->row(std::nullopt)->box(), "13");
  card = u.panel().card(B);
  h = u.history();
  u.typeInto(card->row(doc::ParamKind::TuningEquave)->box(), "3/1");
  CHECK(u.history() == h + 1);
  const tu::Edo* e = doc::findTuning(*u.ctx.doc(), B)->def.edo();
  REQUIRE(e != nullptr);
  CHECK(e->n() == 13u);
  REQUIRE(e->equaveRatio().has_value());
  CHECK(*e->equaveRatio() == tu::Rational(3));
  CHECK(u.panel().card(B)->row(doc::ParamKind::TuningEquave)->box().valueText() == "3/1");
  // Rename through the field, recolour through the palette.
  REQUIRE(u.ctx.store().dispatch(doc::SetTuningColour{B, doc::Colour{0x112233}}).ok());
  u.d.step();
  CHECK(u.panel().card(B)->nameField().text() == "Tuning B");
  // Remove while Bass follows it: Bass moves to the first tuning, one step; the last tuning cannot be removed.
  REQUIRE(u.ctx.store().dispatch(doc::SetTrackTuning{u.bass, doc::TuningAssignment::mainTuning(B)}).ok());
  h = u.history();
  u.d.step();
  u.panel().reveal(B);
  u.d.click(mid(u.panel().card(B)->removeButton().rect()));
  CHECK(u.history() == h + 1);
  CHECK(doc::findTuning(*u.ctx.doc(), B) == nullptr);
  CHECK(doc::findTrackPtr(*u.ctx.doc(), u.bass)->tuning == doc::TuningAssignment::mainTuning(u.ctx.doc()->tunings->front()->id));
  u.d.step();
  REQUIRE(u.panel().cardCount() == 1);
  CHECK_FALSE(u.panel().cardAt(0)->removeButton().enabled());
  CHECK_FALSE(u.panel().removeTuning(u.ctx.doc()->tunings->front()->id));
  // Undo brings it back with Bass on it.
  REQUIRE(u.ctx.undo());
  u.d.step();
  CHECK(u.panel().cardCount() == 2);
  CHECK(doc::findTrackPtr(*u.ctx.doc(), u.bass)->tuning == doc::TuningAssignment::mainTuning(B));
}

TEST_CASE("tunings panel: hidden it does no work; Create > New Main Tuning is greyed only without the panel") {
  Ui u;
  CHECK_FALSE(u.ctx.tuningsVisible());
  // A tuning edit while hidden leaves the cards as they were until the panel is shown.
  doc::TuningDef d;
  d.system = tu::Edo::make(19).value();
  REQUIRE(u.ctx.store().dispatch(doc::ops::addMainTuning(*u.ctx.doc(), u.ctx.store().ids(), "19", d, {})->command).ok());
  CHECK(u.panel().cardCount() == 1);
  u.ctx.setTuningsVisible(true);
  CHECK(u.panel().cardCount() == 2);
  const std::vector<MenuItem> create = u.bar().items(2);
  const auto it = std::find_if(create.begin(), create.end(), [](const MenuItem& m) { return m.action == "tuning.newMain"; });
  REQUIRE(it != create.end());
  CHECK(it->enabled);
}

// ---- goldens ----------------------------------------------------------------------------------------------------------

namespace {

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
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(d));
}

}  // namespace

TEST_CASE("render: menu-bar (the Create menu open) matches the golden image") { checkGolden("menu-bar"); }
TEST_CASE("render: tunings-panel (three main tunings, Lead's generator lane) matches the golden image") { checkGolden("tunings-panel"); }
TEST_CASE("render: pianoroll-ratios (ratio labels, ratio rows, the readout) matches the golden image") { checkGolden("pianoroll-ratios"); }
TEST_CASE("render: pianoroll-note-menu (the identity menu) matches the golden image") { checkGolden("pianoroll-note-menu"); }
