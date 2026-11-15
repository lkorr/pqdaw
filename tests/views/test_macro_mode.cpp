// T8.1: macro mode (SPEC §6.2; plan M7 "Macro mode un-deferred" M1–M3). Through the real shell, key and pointer
// dispatch (TestDriver -> UiTree -> the key listener / the keymap / the roll):
//   Ctrl+CapsLock toggles it (CapsLock alone does nothing), the control bar's lamp shows and switches it, it takes the
//   computer MIDI keyboard's place as the tree's key listener and the two exclude each other;
//   tempered layer: 3/2 from a C in 12-EDO lands seven rows up as a Degree note, 3/2 · 5/4 = 15/8 lands on 11 steps
//   with the tempering error shown; 31-EDO maps by its val, a preset may override the val, a MOS takes the nearest degree;
//   JI layer: the same keys place exact Ratio notes anchored to the anchor note;
//   the user's rule: in the TEMPERED layer, pointing at a JI note makes the next note just, in that note's group;
//   reciprocals, Tab chaining, both Backspaces, the pointer / selection anchor, a click on empty space, a s d f;
//   b + number = Is Harmonic N on the anchor, then s / d step the harmonics; b 0 Enter removes the group (T7.16b: b on
//   a member renumbers its group from it, nothing is ever removed by b alone);
//   T7.16b: the pointer anchor follows only real pointer moves; keys only with the clip view focused / hovered (the
//   lamp says so); keys refused during a drag; the computer MIDI keyboard comes back;
//   macro undo / redo against Ctrl+Z; Ctrl+S, Space and unbound keys pass through; a text field keeps its keys;
//   presets (the keypad preset by scancode, load errors in the status line, the built-in table = trainer.json);
//   the fewest-keypress table equals the trainer's own search (tools/fixtures/macro_bfs.mjs), built once per
//   (EDO, preset).
// Goldens: pianoroll-macro-tempered, pianoroll-macro-ji.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pq/document/commands.h"
#include "pq/document/pitch_resolve.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/macro/macro_mode.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../../src/views/src/macro/macro_overlay.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using pq::tuning::PitchId;
using pq::tuning::Rational;

namespace {

constexpr Ticks kQ = kPpq;
const char* const kView = "main/body/bottom/detail/clip/clip-view";
const std::string kNotes = std::string(kView) + "/notes";
const std::string kLamp = "main/control-bar/macro";

// The shell over an untitled project with one MIDI track and a four-bar 12-EDO clip open in the clip view, a
// quarter-note grid.
struct Macro {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ClipView* view = nullptr;
  doc::ClipRef ref{};

  Macro() {
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    doc::DocumentStore& st = ctx.store();
    auto t = doc::ops::createTrack(*ctx.doc(), st.ids(), doc::TrackKind::Midi, {}, "Keys", doc::Colour{0x5FA8D8});
    REQUIRE(t.ok());
    REQUIRE(st.dispatch(t->command).ok());
    auto c = doc::ops::insertEmptyClip(*ctx.doc(), st.ids(), t->tracks.at(0), 0, 4 * kTicksPerWhole, "Clip");
    REQUIRE(c.ok());
    REQUIRE(st.dispatch(c->command).ok());
    ref = c->clips.at(0);
    GridSpec g;
    g.mode = GridMode::Fixed;
    g.fixedExp = 2;  // quarter notes
    ctx.setGrid(g);
    ctx.setDetailClip(ref);
    if (auto* body = dynamic_cast<Splitter*>(d.find("main/body"))) body->setPosition(480.0f);  // a tall detail view
    d.tree().layoutNow();
    view = dynamic_cast<ClipView*>(d.find(kView));
    REQUIRE(view != nullptr);
    REQUIRE(view->hasClip());
    view->setRowHeight(14);
    view->centreOnDegree(6);
    view->setZoom(60.0 / static_cast<double>(kQ), 0.0);
    view->scrollToTick(-0.5 * static_cast<double>(kQ));
    d.tree().layoutNow();
    // The pointer parked off the roll, the roll focused.
    d.move({2.0f, 2.0f});
    d.tree().setFocus(d.find(kNotes));
    d.step();
  }
  ~Macro() { d.tree().setRoot(std::make_unique<Widget>()); }

  MacroMode& m() { return ctx.macro(); }
  const doc::NoteClip& clip() const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), ref.track);
    REQUIRE(t != nullptr);
    const int32_t i = doc::findClipIndex(*t, ref.clip);
    REQUIRE(i >= 0);
    return *t->clips[static_cast<size_t>(i)].clip;
  }
  size_t noteCount() const { return clip().notes.size(); }
  // The JI group `anchor` anchors (nullptr: none).
  const doc::JiGroup* group(doc::NoteId anchor) const { return doc::findJiGroupByAnchor(clip(), anchor); }
  const doc::Note* find(const PitchId& p, Ticks start) const {
    for (const doc::Note& n : clip().notes)
      if (n.pitch == p && n.start == start) return &n;
    return nullptr;
  }
  doc::NoteId add(const PitchId& p, Ticks start = 0, Ticks length = 2 * kQ) {
    doc::Note n;
    n.start = start;
    n.length = length;
    n.pitch = p;
    auto e = doc::ops::addNotes(*ctx.doc(), ctx.store().ids(), ref, std::vector<doc::Note>{n});
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    d.step();
    return e->notes.at(0).id;
  }
  // The pointer onto a note (a real move: the roll's hover, then macro mode's pointer anchor).
  void hover(doc::NoteId id) {
    const Vec2 p = view->notePoint(id, 0.5);
    REQUIRE(p.x >= 0.0f);
    d.move(p);
    d.step();
  }
  void on() {
    CHECK(d.press("Ctrl+CapsLock") == KeyResult::Action);
    d.step();
    REQUIRE(m().active());
  }
  // Keys one after the other, each through the tree; every one must be taken by the listener.
  void keys(std::initializer_list<const char*> ks) {
    for (const char* k : ks) {
      CAPTURE(k);
      CHECK(d.press(k) == KeyResult::Widget);
      d.step();
    }
  }
  // The clip in an EDO / another tuning (a named tuning assigned to the clip).
  void retune(const doc::TuningDef& def, const std::string& name) {
    const doc::TuningId id = views::test::addTuning(ctx.store(), def, name);
    REQUIRE(ctx.store().dispatch(doc::SetClipTuning{ref, doc::TuningAssignment::mainTuning(id)}).ok());
    d.step();
  }
  void edo(uint32_t n) {
    doc::TuningDef def;
    def.system = *tuning::Edo::make(n);
    retune(def, std::to_string(n) + "-EDO");
  }
  const FakeTransport::Record* lastAudition() const {
    for (size_t i = port.calls.size(); i-- > 0;)
      if (port.calls[i].call == FakeTransport::Call::Audition) return &port.calls[i];
    return nullptr;
  }
};

double hzOfCents(double cents) { return tuning::kDefaultReferenceHz * std::exp2(cents / 1200.0); }

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

TEST_CASE("macro mode: Ctrl+CapsLock toggles it (CapsLock alone does nothing); the lamp shows and switches it; it is the key listener while on") {
  Macro f;
  auto* lamp = dynamic_cast<MacroLamp*>(f.d.find(kLamp));
  REQUIRE(lamp != nullptr);
  CHECK(lamp->text() == "MACRO");
  CHECK_FALSE(lamp->on());
  CHECK(f.d.tree().keyListener() == &f.ctx.computerKeyboard());
  // The overlays exist but are hidden: nothing of macro mode is painted while it is off.
  Widget* overlay = f.d.find(std::string(kView) + "/macro-overlay");
  Widget* gutter = f.d.find(std::string(kView) + "/macro-keys");
  REQUIRE(overlay != nullptr);
  REQUIRE(gutter != nullptr);
  CHECK_FALSE(overlay->visible());
  CHECK_FALSE(gutter->visible());
  CHECK(f.view->macroCursorRect().empty());
  CHECK(f.m().keypressBuilds() == 0);

  // CapsLock on its own, or with Shift, is no chord.
  CHECK(f.d.press(Chord{key::kCapsLock, mod::kNone}) == KeyResult::Unhandled);
  CHECK(f.d.press(Chord{key::kCapsLock, mod::kShift}) == KeyResult::Unhandled);
  CHECK_FALSE(f.m().on());
  // The interval keys belong to nobody yet.
  CHECK(f.d.press("2") == KeyResult::Unhandled);
  CHECK(f.noteCount() == 0);

  CHECK(f.d.press("Ctrl+CapsLock") == KeyResult::Action);
  f.d.step();
  CHECK(f.m().on());
  CHECK(f.m().active());
  CHECK(lamp->on());
  CHECK(lamp->text() == "MACRO \xC2\xB7 12-EDO");
  CHECK(f.d.tree().keyListener() == &f.m());
  CHECK(overlay->visible());
  CHECK(gutter->visible());
  CHECK_FALSE(f.view->macroCursorRect().empty());
  CHECK(f.ctx.status().find("Macro mode on") != std::string::npos);
  // A held chord does not flip it back.
  CHECK(f.d.press(Chord{key::kCapsLock, mod::kCtrl}, true) == KeyResult::Action);
  CHECK(f.m().on());

  CHECK(f.d.press("Ctrl+CapsLock") == KeyResult::Action);
  f.d.step();
  CHECK_FALSE(f.m().on());
  CHECK_FALSE(lamp->on());
  CHECK(lamp->text() == "MACRO");
  CHECK(f.d.tree().keyListener() == &f.ctx.computerKeyboard());
  CHECK_FALSE(overlay->visible());
  CHECK(f.ctx.status() == "Macro mode off");

  // The lamp is a switch.
  f.d.click(f.d.center(kLamp));
  f.d.step();
  CHECK(f.m().on());
  CHECK(lamp->on());
  f.d.click(f.d.center(kLamp));
  f.d.step();
  CHECK_FALSE(f.m().on());
}

TEST_CASE("macro mode: without a note clip open it is on but takes no key, and says so") {
  Macro f;
  f.ctx.setDetailClip({});
  f.d.step();
  CHECK(f.d.press("Ctrl+CapsLock") == KeyResult::Action);
  CHECK(f.m().on());
  CHECK_FALSE(f.m().active());
  CHECK(f.ctx.status() == "Macro mode on: open a note clip in the clip view to use it");
  auto* lamp = dynamic_cast<MacroLamp*>(f.d.find(kLamp));
  REQUIRE(lamp != nullptr);
  CHECK(lamp->on());
  CHECK(lamp->text() == "MACRO");
  CHECK(f.d.press("2") == KeyResult::Unhandled);
  CHECK(f.d.press("`") == KeyResult::Unhandled);
  // The clip opened: active, a fresh cursor; the keys are macro's once the notes have the focus (T7.16b, review M11).
  f.ctx.setDetailClip(f.ref);
  f.d.step();
  CHECK(f.m().active());
  CHECK(lamp->text() == "MACRO (click the notes)");
  CHECK(f.d.press("2") != KeyResult::Widget);
  f.d.click(f.d.center(kNotes));
  f.d.move({2.0f, 2.0f});
  f.d.step();
  CHECK(lamp->text() == "MACRO \xC2\xB7 12-EDO");
  CHECK(f.d.press("2") == KeyResult::Widget);
}

TEST_CASE("macro mode, tempered layer in 12-EDO: 2 then ` from a C places a G seven rows up (Degree); 2 3 = 15/8 lands on 11 steps with the error shown") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  CHECK(f.m().layer() == MacroMode::Layer::Tempered);
  f.hover(c);  // the pointer anchor
  CHECK(f.m().anchorNote() == c);
  CHECK(f.m().tick() == 0);
  CHECK_FALSE(f.m().composing());
  CHECK(f.m().compositionText() == "1/1");

  const size_t auditions = f.port.calls.size();
  f.keys({"2"});
  CHECK(f.m().composition() == Rational(3, 2));
  CHECK(f.m().compositionText() == "3/2");
  MacroMode::Target t = f.m().target();
  REQUIRE(t.ok);
  CHECK_FALSE(t.just);
  CHECK(t.pitch == PitchId::fromDegree(7, 0));
  CHECK(t.steps == 7);
  CHECK(t.errorCents == doctest::Approx(1.955).epsilon(0.001));
  CHECK(f.m().detailText() == "\xE2\x86\x92 7 steps, +2.0 \xC2\xA2");
  // The move was auditioned at the cursor's pitch.
  REQUIRE(f.port.calls.size() > auditions);
  REQUIRE(f.lastAudition() != nullptr);
  CHECK(f.lastAudition()->hz == doctest::Approx(hzOfCents(700.0)).epsilon(1e-5));
  // The cursor is drawn seven rows above the anchor, at the anchor's start.
  const IRect cur = f.view->macroCursorRect();
  REQUIRE_FALSE(cur.empty());
  CHECK(std::abs((cur.y + cur.h / 2) - static_cast<int32_t>(f.view->yOfDegree(7))) <= 1);
  CHECK(std::abs(cur.x - static_cast<int32_t>(f.view->xOfTick(0.0))) <= 1);

  const size_t undoSteps = f.ctx.store().history().cursor();
  f.keys({"`"});
  CHECK(f.noteCount() == 2);
  const doc::Note* g = f.find(PitchId::fromDegree(7, 0), 0);
  REQUIRE(g != nullptr);
  CHECK(g->length == 2 * kQ);  // the anchor note's length
  CHECK(f.ctx.store().history().cursor() == undoSteps + 1);  // one undo step
  // The composition is cleared and the cursor is back on the anchor.
  CHECK_FALSE(f.m().composing());
  CHECK(f.m().anchorNote() == c);
  CHECK(f.m().target().pitch == PitchId::fromDegree(0, 0));

  // 3/2 · 5/4 = 15/8: composed exactly, mapped once: 11 steps, 11.7 cents sharp of just.
  f.keys({"2", "3"});
  CHECK(f.m().composition() == Rational(15, 8));
  CHECK(f.m().compositionText() == "3/2 \xC2\xB7 5/4 = 15/8");
  t = f.m().target();
  CHECK(t.steps == 11);
  CHECK(t.errorCents == doctest::Approx(-11.731).epsilon(0.001));
  CHECK(f.m().detailText() == "\xE2\x86\x92 11 steps, \xE2\x88\x92" "11.7 \xC2\xA2");
  f.keys({"`"});
  CHECK(f.find(PitchId::fromDegree(11, 0), 0) != nullptr);
  // 5/4 = 4 steps.
  f.keys({"3"});
  CHECK(f.m().target().steps == 4);
  f.keys({"`"});
  CHECK(f.find(PitchId::fromDegree(4, 0), 0) != nullptr);
  CHECK(f.noteCount() == 4);
  // Ctrl+Z is the document's undo: one note per step.
  CHECK(f.d.press("Ctrl+Z") == KeyResult::Action);
  CHECK(f.noteCount() == 3);
}

TEST_CASE("macro mode, tempered layer: 31-EDO maps by its val, a preset may override it, a MOS takes the nearest degree") {
  Macro f;
  f.edo(31);
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  f.hover(c);
  auto* lamp = dynamic_cast<MacroLamp*>(f.d.find(kLamp));
  REQUIRE(lamp != nullptr);
  CHECK(lamp->text() == "MACRO \xC2\xB7 31-EDO");
  const std::pair<const char*, int64_t> steps[] = {{"1", 31}, {"2", 18}, {"3", 10}, {"4", 25}, {"5", 14}, {"q", -31}, {"w", -18}, {"e", -10}};
  for (const auto& [key, want] : steps) {
    CAPTURE(key);
    f.keys({key});
    CHECK(f.m().target().steps == want);
    CHECK(std::fabs(f.m().target().errorCents) < 1200.0 / 31.0);
    f.keys({"Backspace"});
  }
  // 81/80 is tempered out by 31-EDO's val: four fifths up, two octaves and a third down = 0 steps.
  f.keys({"2", "2", "2", "2", "q", "q", "e"});
  CHECK(f.m().composition() == Rational(81, 80));
  CHECK(f.m().target().steps == 0);
  CHECK(f.m().target().errorCents == doctest::Approx(21.506).epsilon(0.001));
  f.keys({"Backspace"});

  // A preset's val for this EDO replaces the patent val (here: 5 mapped one step higher).
  MacroPreset p = f.m().preset();
  MacroPreset::EdoVal v;
  v.edo = 31;
  v.val = tuning::patentVal(*tuning::Edo::make(31));
  v.val.steps[2] += 1;
  p.vals.push_back(v);
  f.m().setPreset(p);
  f.keys({"3"});
  CHECK(f.m().target().steps == 11);
  f.keys({"Backspace"});

  // A MOS (5L 2s, generator 696 cents): the nearest degree to the interval from the anchor.
  tuning::MosParams mos;
  mos.generator = 696.0;
  mos.n = 7;
  mos.period = 1200.0;
  mos.rotation = 4;
  mos.region = tuning::RegionHint{7, 1200.0 * 4 / 7, 720.0};
  doc::TuningDef def;
  def.system = mos;
  f.retune(def, "Meantone 7");
  REQUIRE(f.m().active());
  f.hover(c);
  f.keys({"2"});
  MacroMode::Target t = f.m().target();
  REQUIRE(t.ok);
  CHECK(t.pitch.kind() == tuning::PitchKind::Degree);
  CHECK(t.steps == 4);                                      // the fifth is four degrees up in a diatonic scale
  CHECK(t.cents == doctest::Approx(696.0).epsilon(1e-6));
  CHECK(t.errorCents == doctest::Approx(701.955 - 696.0).epsilon(0.001));
  f.keys({"`"});
  CHECK(f.find(PitchId::fromDegree(4, 0), 0) != nullptr);
}

TEST_CASE("macro mode, JI layer: the same keys place exact ratios anchored to the anchor note; the lamp and the overlay name the layer") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  f.hover(c);
  f.keys({"g"});
  CHECK(f.m().layer() == MacroMode::Layer::Ji);
  auto* lamp = dynamic_cast<MacroLamp*>(f.d.find(kLamp));
  REQUIRE(lamp != nullptr);
  CHECK(lamp->text() == "MACRO \xC2\xB7 JI");
  f.keys({"2"});
  MacroMode::Target t = f.m().target();
  REQUIRE(t.ok);
  CHECK(t.just);
  CHECK(t.pitch == PitchId::fromRatio(Rational(3, 2), tuning::AnchorKind::Note, c.value));
  CHECK(t.cents == doctest::Approx(701.955).epsilon(1e-6));
  CHECK(f.m().detailText() == "just");
  REQUIRE(f.lastAudition() != nullptr);
  CHECK(f.lastAudition()->hz == doctest::Approx(hzOfCents(701.955)).epsilon(1e-5));
  f.keys({"`"});
  CHECK(f.find(PitchId::fromRatio(Rational(3, 2), tuning::AnchorKind::Note, c.value), 0) != nullptr);
  // The plan's sequence: 1 1 2 ` Tab 3 `: 2·2·3/2 = 6/1 over the anchor; Tab with nothing composed doubles the anchor
  // as a 1/1 and chains from it; then 5/4 above that.
  f.keys({"1", "1", "2", "`"});
  CHECK(f.find(PitchId::fromRatio(Rational(6), tuning::AnchorKind::Note, c.value), 0) != nullptr);
  f.keys({"Tab"});
  const doc::Note* unison = f.find(PitchId::fromRatio(Rational(1), tuning::AnchorKind::Note, c.value), 0);
  REQUIRE(unison != nullptr);
  CHECK(f.m().anchorNote() == unison->id);
  f.keys({"3", "`"});
  // The new anchor is a member of C's group: the 5/4 is anchored to C, not to the member.
  CHECK(f.find(PitchId::fromRatio(Rational(5, 4), tuning::AnchorKind::Note, c.value), 0) != nullptr);
  // g again: tempered.
  f.keys({"g"});
  CHECK(f.m().layer() == MacroMode::Layer::Tempered);
  CHECK(lamp->text() == "MACRO \xC2\xB7 12-EDO");
}

TEST_CASE("macro mode: in the TEMPERED layer, 3/2 from a tempered note goes 7 steps, and from a JI note it is an exact 3/2 in that note's group") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0), 0);
  // A just major third on C (a member of C's group), and a lone Ratio note over the reference, later in the clip.
  const doc::NoteId e = f.add(PitchId::fromRatio(Rational(5, 4), tuning::AnchorKind::Note, c.value), 0);
  const doc::NoteId lone = f.add(PitchId::fromRatio(Rational(7, 4)), 4 * kQ);
  f.on();
  REQUIRE(f.m().layer() == MacroMode::Layer::Tempered);

  // Pointing at the tempered C: the nearest step.
  f.hover(c);
  REQUIRE(f.m().anchorNote() == c);
  f.keys({"2", "`"});
  CHECK(f.find(PitchId::fromDegree(7, 0), 0) != nullptr);

  // Pointing at the JI note: just, a Ratio anchored to the group's anchor (C) with the composed ratio from it.
  f.hover(e);
  REQUIRE(f.m().anchorNote() == e);
  f.keys({"2"});
  MacroMode::Target t = f.m().target();
  REQUIRE(t.ok);
  CHECK(t.just);
  CHECK(t.pitch == PitchId::fromRatio(Rational(15, 8), tuning::AnchorKind::Note, c.value));
  CHECK(t.cents == doctest::Approx(1088.269).epsilon(1e-6));  // not the row at 1100
  CHECK(f.m().detailText() == "just");
  f.keys({"`"});
  const doc::Note* b = f.find(PitchId::fromRatio(Rational(15, 8), tuning::AnchorKind::Note, c.value), 0);
  REQUIRE(b != nullptr);
  CHECK(f.find(PitchId::fromDegree(11, 0), 0) == nullptr);
  // The cursor is back on the JI note; a reciprocal from it stays in the group too.
  CHECK(f.m().anchorNote() == e);
  f.keys({"w", "`"});
  CHECK(f.find(PitchId::fromRatio(Rational(5, 6), tuning::AnchorKind::Note, c.value), 0) != nullptr);

  // A lone Ratio note over the reference: the exact product, over the reference.
  f.hover(lone);
  REQUIRE(f.m().anchorNote() == lone);
  CHECK(f.m().tick() == 4 * kQ);
  f.keys({"2", "`"});
  CHECK(f.find(PitchId::fromRatio(Rational(21, 8)), 4 * kQ) != nullptr);

  // The same from a MEMBER of a declared group (C is harmonic 4, the 5/4 its member): still in the tempered layer, the
  // new note joins that group, anchored to the group's anchor with the composed ratio from it.
  const doc::Note* cNote = f.clip().notes.findById(c);
  REQUIRE(cNote != nullptr);
  auto g = doc::ops::makeJiGroup(*f.ctx.doc(), f.ctx.store().ids(), f.ref, doc::keyOf(*cNote), 4);
  REQUIRE(g.ok());
  REQUIRE(f.ctx.store().dispatch(g->command).ok());
  f.d.step();
  const doc::JiGroup* group = f.group(c);
  REQUIRE(group != nullptr);
  const doc::JiGroupId gid = group->id;
  f.d.move({2.0f, 2.0f});
  f.d.step();
  f.hover(e);
  REQUIRE(f.m().anchorNote() == e);
  CHECK(f.m().locked());
  REQUIRE(f.m().layer() == MacroMode::Layer::Tempered);
  f.keys({"2", "1"});  // 3/2 and an octave above the member: 5/4 * 3 = 15/4 over C
  CHECK(f.m().target().just);
  CHECK(f.m().target().harmonic > 0);  // a whole harmonic of the group's fundamental
  f.keys({"`"});
  const doc::Note* placed = f.find(PitchId::fromRatio(Rational(15, 4), tuning::AnchorKind::Note, c.value), 0);
  REQUIRE(placed != nullptr);
  const doc::JiGroup* of = doc::jiGroupOfNote(f.clip(), *placed);
  REQUIRE(of != nullptr);
  CHECK(of->id == gid);
  // And from the group's anchor itself (a tempered Degree note that anchors a group): just, not seven rows.
  f.hover(c);
  REQUIRE(f.m().anchorNote() == c);
  f.keys({"4", "`"});
  CHECK(f.find(PitchId::fromRatio(Rational(7, 4), tuning::AnchorKind::Note, c.value), 0) != nullptr);
  CHECK(f.find(PitchId::fromDegree(10, 0), 0) == nullptr);
}

TEST_CASE("macro mode: q w e go down by the reciprocals; Tab places and chains; a f s d move the cursor") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0), 0, kQ);
  f.on();
  f.hover(c);
  f.keys({"q"});
  CHECK(f.m().composition() == Rational(1, 2));
  CHECK(f.m().target().pitch == PitchId::fromDegree(0, -1));
  CHECK(f.m().target().steps == -12);
  f.keys({"Backspace", "w"});
  CHECK(f.m().target().pitch == PitchId::fromDegree(5, -1));  // a fifth down: F
  f.keys({"Backspace", "e"});
  CHECK(f.m().target().pitch == PitchId::fromDegree(8, -1));  // a major third down
  f.keys({"Backspace", "1", "q"});
  CHECK(f.m().compositionText() == "2/1 \xC2\xB7 1/2 = 1/1");
  CHECK(f.m().target().pitch == PitchId::fromDegree(0, 0));
  f.keys({"Backspace"});

  // Tab: the placed note is the new anchor: a chain of fifths C G D, moving right between them.
  f.keys({"2", "Tab"});
  const doc::Note* g = f.find(PitchId::fromDegree(7, 0), 0);
  REQUIRE(g != nullptr);
  CHECK(f.m().anchorNote() == g->id);
  CHECK_FALSE(f.m().composing());
  CHECK(f.m().target().pitch == PitchId::fromDegree(7, 0));
  f.keys({"f"});
  CHECK(f.m().tick() == kQ);
  f.keys({"2", "Tab"});
  const doc::Note* dd = f.find(PitchId::fromDegree(2, 1), kQ);
  REQUIRE(dd != nullptr);
  CHECK(dd->length == kQ);
  CHECK(f.m().anchorNote() == dd->id);

  // a / f: one grid step; s / d: one row (kept until the note is placed; ` returns to the anchor).
  f.keys({"f", "f", "a"});
  CHECK(f.m().tick() == 2 * kQ);
  f.keys({"s", "s", "d"});
  CHECK(f.m().composing());
  CHECK(f.m().target().pitch == PitchId::fromDegree(3, 1));
  CHECK(f.m().target().steps == 1);
  CHECK(f.m().detailText() == "\xE2\x86\x92 1 step");
  f.keys({"`"});
  CHECK(f.find(PitchId::fromDegree(3, 1), 2 * kQ) != nullptr);
  CHECK(f.m().target().pitch == PitchId::fromDegree(2, 1));
  CHECK(f.m().anchorNote() == dd->id);
  // Held: the cursor keys repeat, an interval key does not stack.
  CHECK(f.d.press(Chord{'f', mod::kNone}, true) == KeyResult::Widget);
  CHECK(f.m().tick() == 3 * kQ);
  CHECK(f.d.press(Chord{'2', mod::kNone}, true) == KeyResult::Widget);
  CHECK_FALSE(f.m().composing());
  // The cursor never leaves the clip's start.
  f.keys({"a", "a", "a", "a", "a"});
  CHECK(f.m().tick() == 0);
}

TEST_CASE("macro mode: Backspace clears the composition, or deletes the note at the cursor when nothing is composed") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  f.hover(c);
  f.keys({"2", "3"});
  CHECK(f.m().composing());
  f.keys({"Backspace"});
  CHECK_FALSE(f.m().composing());
  CHECK(f.m().composition() == Rational(1));
  CHECK(f.noteCount() == 1);
  // Place a fifth and chain to it, then delete it from under the cursor: the cursor stays on its row, free.
  f.keys({"2", "Tab"});
  REQUIRE(f.noteCount() == 2);
  const size_t undoSteps = f.ctx.store().history().cursor();
  f.keys({"Backspace"});
  CHECK(f.noteCount() == 1);
  CHECK(f.find(PitchId::fromDegree(7, 0), 0) == nullptr);
  CHECK(f.ctx.store().history().cursor() == undoSteps + 1);
  CHECK_FALSE(f.m().anchorNote().valid());
  CHECK(f.m().target().pitch == PitchId::fromDegree(7, 0));
  // Nothing there now.
  f.keys({"Backspace"});
  CHECK(f.noteCount() == 1);
  CHECK(f.ctx.status() == "Macro: no note at the cursor");
}

TEST_CASE("macro mode: the anchor follows the pointer and the selection only while nothing is composed; a click on empty space puts a free cursor there") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0), 0);
  const doc::NoteId g = f.add(PitchId::fromDegree(7, 0), 2 * kQ);
  f.on();
  // Nothing under the pointer, nothing selected: a free cursor.
  CHECK_FALSE(f.m().anchorNote().valid());
  // The single selected note is the anchor (no pointer over a note).
  f.view->select(std::span<const doc::NoteId>(&g, 1));
  f.d.step();
  f.d.step();  // the overlay adopts a selection it was not told about on the next frame
  CHECK(f.m().anchorNote() == g);
  CHECK(f.m().tick() == 2 * kQ);
  // The pointer arriving on a note wins.
  f.hover(c);
  CHECK(f.m().anchorNote() == c);
  CHECK(f.m().tick() == 0);
  // While composing, the pointer moving onto another note changes nothing.
  f.keys({"2"});
  f.hover(g);
  CHECK(f.m().anchorNote() == c);
  CHECK(f.m().composition() == Rational(3, 2));
  // Tab chains to the new note although the pointer still rests on G.
  f.keys({"Tab"});
  const doc::Note* placed = f.find(PitchId::fromDegree(7, 0), 0);
  REQUIRE(placed != nullptr);
  CHECK(f.m().anchorNote() == placed->id);
  f.d.move(f.view->notePoint(g, 0.6));  // the same note still: no new arrival
  f.d.step();
  CHECK(f.m().anchorNote() == placed->id);

  // A click on empty space: the cursor in that grid cell, on that row.
  const IRect a = f.d.rect(kNotes);
  const Vec2 at{f.view->xOfTick(static_cast<double>(5 * kQ) + 0.4 * static_cast<double>(kQ)), f.view->yOfDegree(3)};
  REQUIRE(at.x < static_cast<float>(a.right()));
  REQUIRE(at.y > static_cast<float>(a.y));
  f.d.click(at);
  f.d.step();
  CHECK_FALSE(f.m().anchorNote().valid());
  CHECK(f.m().tick() == 5 * kQ);
  CHECK(f.m().anchorDegree() == 3);
  CHECK(f.m().target().pitch == PitchId::fromDegree(3, 0));
  CHECK(f.m().noteLength() == kQ);  // the grid step
  // s / d move the free cursor itself; ` puts a note there and it becomes the anchor.
  f.keys({"s", "`"});
  const doc::Note* root = f.find(PitchId::fromDegree(4, 0), 5 * kQ);
  REQUIRE(root != nullptr);
  CHECK(root->length == kQ);
  CHECK(f.m().anchorNote() == root->id);
}

TEST_CASE("macro mode: b + number = Is Harmonic N on the anchor; s / d then step its harmonics and notes from it are just; b 0 Enter removes it") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  f.hover(c);
  CHECK_FALSE(f.m().locked());
  // b 5 Enter.
  f.keys({"b"});
  CHECK(f.m().lockPending());
  CHECK(f.m().detailText() == "Is Harmonic _");
  f.keys({"5"});
  CHECK(f.m().detailText() == "Is Harmonic 5_");
  CHECK_FALSE(f.m().composing());  // the digit was no interval
  f.keys({"Enter"});
  CHECK_FALSE(f.m().lockPending());
  REQUIRE(f.group(c) != nullptr);
  CHECK(f.group(c)->index == 5);
  CHECK(f.m().locked());
  CHECK(f.view->harmonicOf(c) == std::optional<uint64_t>(5));
  // s: harmonic 6; d d: harmonic 4.
  f.keys({"s"});
  CHECK(f.m().composition() == Rational(6, 5));
  CHECK(f.m().target().harmonic == 6);
  CHECK(f.m().target().just);
  CHECK(f.m().detailText() == "just \xC2\xB7 harmonic 6");
  f.keys({"d", "d"});
  CHECK(f.m().composition() == Rational(4, 5));
  CHECK(f.m().target().harmonic == 4);
  // In the tempered layer too, the note placed from the declared anchor is just.
  REQUIRE(f.m().layer() == MacroMode::Layer::Tempered);
  f.keys({"`"});
  CHECK(f.find(PitchId::fromRatio(Rational(4, 5), tuning::AnchorKind::Note, c.value), 0) != nullptr);
  // Number keys keep their interval meaning: 3/2 above harmonic 5 (the numbering refines: 10 and 15).
  f.keys({"2"});
  CHECK(f.m().target().pitch == PitchId::fromRatio(Rational(3, 2), tuning::AnchorKind::Note, c.value));
  f.keys({"Backspace"});
  // T7.16b (review H7): b on the declared anchor removes nothing; it waits for the number. b 0 Enter removes the group.
  const doc::JiGroupId first = f.group(c)->id;
  f.keys({"b"});
  CHECK(f.m().lockPending());
  REQUIRE(f.group(c) != nullptr);
  f.keys({"0"});
  CHECK(f.m().detailText() == "Remove the JI group: Enter");
  f.keys({"7"});  // nothing follows a 0
  CHECK(f.m().detailText() == "Remove the JI group: Enter");
  REQUIRE(f.group(c) != nullptr);
  f.keys({"Enter"});
  CHECK(f.group(c) == nullptr);
  CHECK_FALSE(f.m().locked());
  CHECK(f.ctx.status().find("Removed the JI group") != std::string::npos);

  // Several digits, committed by the next macro key, which then acts: b 1 5 s = harmonic 15, then up to 16.
  f.keys({"b", "1", "5", "s"});
  REQUIRE(f.group(c) != nullptr);
  CHECK(f.group(c)->index == 15);
  CHECK(f.m().composition() == Rational(16, 15));
  // b 3 Enter on the declared anchor re-declares it: the same group, the new number.
  const doc::JiGroupId again = f.group(c)->id;
  f.keys({"Backspace", "b", "3", "Enter"});
  REQUIRE(f.group(c) != nullptr);
  CHECK(f.group(c)->id == again);
  CHECK(f.group(c)->index == 3);
  CHECK(again != first);
  // b alone reuses the last index.
  f.keys({"b", "0", "Enter", "b", "Enter"});
  REQUIRE(f.group(c) != nullptr);
  CHECK(f.group(c)->index == 3);
  // b then b: cancelled, nothing changed.
  f.keys({"b", "b"});
  CHECK_FALSE(f.m().lockPending());
  REQUIRE(f.group(c) != nullptr);
  CHECK(f.group(c)->index == 3);
  // Review L10: a fifth digit is refused (said so); a key macro mode does not bind cancels the number and then does
  // its own work (Space plays), nothing is declared.
  f.keys({"b", "1", "2", "3", "4", "5"});
  CHECK(f.m().detailText() == "Is Harmonic 1234_");
  CHECK(f.ctx.status().find("at most four digits") != std::string::npos);
  CHECK(f.d.press("Space") == KeyResult::Action);
  CHECK_FALSE(f.m().lockPending());
  CHECK(f.group(c)->index == 3);
  REQUIRE_FALSE(f.port.calls.empty());
  CHECK(f.port.calls.back().call == FakeTransport::Call::Play);
  CHECK(f.d.press("Space") == KeyResult::Action);
  f.keys({"b", "0", "Enter"});
  CHECK(f.group(c) == nullptr);
  // No anchor note: refused with a hint.
  const IRect a = f.d.rect(kNotes);
  f.d.click({static_cast<float>(a.right()) - 30.0f, static_cast<float>(a.y) + 30.0f});
  f.d.step();
  REQUIRE_FALSE(f.m().anchorNote().valid());
  f.keys({"b"});
  CHECK_FALSE(f.m().lockPending());
  CHECK(f.ctx.status().find("needs an anchor note") != std::string::npos);
}

TEST_CASE("macro mode (T7.16b, review H7): b 1 0 Enter over a member of a tuned 12-EDO chord renumbers the group from that note; every pitch and the group stay") {
  Macro f;
  std::vector<doc::Note> chord;
  for (const int32_t d : {0, 4, 7}) {
    doc::Note n;
    n.start = 0;
    n.length = 2 * kQ;
    n.pitch = PitchId::fromDegree(d, 0);
    chord.push_back(n);
  }
  auto added = doc::ops::addNotes(*f.ctx.doc(), f.ctx.store().ids(), f.ref, chord);
  REQUIRE(added.ok());
  REQUIRE(f.ctx.store().dispatch(added->command).ok());
  auto tuned = doc::ops::tuneToJi(*f.ctx.doc(), f.ctx.store().ids(), f.ref, added->notes, doc::ops::TuneToJiOptions{});
  REQUIRE(tuned.ok());
  REQUIRE(f.ctx.store().dispatch(tuned->command).ok());
  f.d.step();
  REQUIRE(doc::jiGroupsOf(f.clip()).size() == 1);
  const doc::JiGroup g0 = doc::jiGroupsOf(f.clip())[0];
  auto hzOf = [&] {
    auto cp = doc::resolveClipPitches(*f.ctx.doc(), f.ref.track, f.ref.clip);
    REQUIRE(cp.ok());
    std::vector<std::pair<uint32_t, double>> out;
    for (const doc::NotePitch& np : cp->notes) out.emplace_back(np.note.value, np.hz.value_or(-1.0));
    std::sort(out.begin(), out.end());
    return out;
  };
  const auto before = hzOf();
  REQUIRE(before.size() == 3);
  // The E: a member (not the group's anchor).
  const doc::NoteId e = added->notes.at(1).id;
  REQUIRE(g0.anchor != e);
  f.on();
  f.hover(e);
  REQUIRE(f.m().anchorNote() == e);
  const size_t base = f.ctx.store().history().cursor();
  f.keys({"b", "1", "0", "Enter"});
  // One group still, the same id; the E is its anchor, harmonic 10; every note is in it; nothing moved.
  REQUIRE(doc::jiGroupsOf(f.clip()).size() == 1);
  const doc::JiGroup& g1 = doc::jiGroupsOf(f.clip())[0];
  CHECK(g1.id == g0.id);
  CHECK(g1.anchor == e);
  CHECK(g1.index == 10);
  for (const doc::Note& n : f.clip().notes) {
    const doc::JiGroup* of = doc::jiGroupOfNote(f.clip(), n);
    REQUIRE(of != nullptr);
    CHECK(of->id == g0.id);
  }
  const auto after = hzOf();
  REQUIRE(after.size() == before.size());
  for (size_t i = 0; i < after.size(); ++i) {
    CHECK(after[i].first == before[i].first);
    CHECK(after[i].second == doctest::Approx(before[i].second).epsilon(1e-12));
  }
  CHECK(f.ctx.store().history().cursor() == base + 1);  // one undo step
  CHECK(f.ctx.status().find("its group's anchor now, harmonic 10") != std::string::npos);
  // A habitual b, then Esc: nothing about the group changed.
  f.keys({"b"});
  CHECK(f.d.press("Escape") == KeyResult::Widget);
  REQUIRE(doc::jiGroupsOf(f.clip()).size() == 1);
  CHECK(doc::jiGroupsOf(f.clip())[0].index == 10);
}

TEST_CASE("macro mode (T7.16b, review H8): a note the cursor's auto-scroll slides under a resting pointer does not become the anchor; a real move does") {
  Macro f;
  f.view->setZoom(120.0 / static_cast<double>(kQ), 0.0);
  f.view->scrollToTick(-0.5 * static_cast<double>(kQ));
  f.d.tree().layoutNow();
  // A row of quarter notes on C over the whole clip.
  std::vector<doc::NoteId> row;
  for (Ticks k = 0; k < 16; ++k) row.push_back(f.add(PitchId::fromDegree(0, 0), k * kQ, kQ));
  f.on();
  f.hover(row[1]);
  REQUIRE(f.m().anchorNote() == row[1]);
  REQUIRE(f.m().tick() == kQ);
  const float x0 = f.view->xOfTick(static_cast<double>(kQ));
  const Vec2 parked = f.d.tree().pointerPos();
  // f f f …: the cursor walks right; the view scrolls to keep it in sight, sliding later notes under the pointer.
  for (int i = 0; i < 12; ++i) f.keys({"f"});
  CHECK(f.d.tree().pointerPos().x == parked.x);
  REQUIRE(f.view->xOfTick(static_cast<double>(kQ)) < x0 - 130.0f);  // it scrolled by more than a note
  f.d.step();
  f.d.step();
  CHECK(f.m().anchorNote() == row[1]);
  CHECK(f.m().tick() == 13 * kQ);
  // The wheel slides notes under it too: still no new anchor.
  f.d.wheel(parked, 0.0f, -1.0f);
  f.d.step();
  CHECK(f.m().anchorNote() == row[1]);
  // A real pointer move (one pixel, inside the note now under it) adopts that note.
  f.d.move({parked.x + 1.0f, parked.y});
  f.d.step();
  CHECK(f.m().anchorNote().valid());
  CHECK(f.m().anchorNote() != row[1]);
}

TEST_CASE("macro mode (T7.16b, review M11): keys act only while the clip view has the focus or the pointer; the lamp says when they do not") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  auto* lamp = dynamic_cast<MacroLamp*>(f.d.find(kLamp));
  REQUIRE(lamp != nullptr);
  CHECK(lamp->text() == "MACRO \xC2\xB7 12-EDO");
  CHECK(f.m().takingKeys());
  // A click in the arrangement: the focus and the pointer are there. Digits, ` and q pass through, Tab is the focus
  // traversal again; nothing is placed.
  const IRect ar = f.d.rect("main/body/top/track-area/arrangement/tracks");
  REQUIRE_FALSE(ar.empty());
  const Vec2 inArrangement{static_cast<float>(ar.x + ar.w / 3), static_cast<float>(ar.bottom()) - 10.0f};
  f.d.click(inArrangement);
  f.d.step();
  f.d.step();
  REQUIRE(f.d.tree().hovered() != nullptr);
  CHECK(f.d.tree().hovered()->path().find("clip-view") == std::string::npos);
  CHECK_FALSE(f.m().takingKeys());
  CHECK(f.m().on());
  CHECK(lamp->text() == "MACRO (click the notes)");
  for (const char* k : {"2", "`", "q"}) {
    CAPTURE(k);
    CHECK(f.d.press(k) != KeyResult::Widget);
  }
  Widget* before = f.d.tree().focus();
  CHECK(f.d.press("Tab") != KeyResult::Widget);
  CHECK(f.d.tree().focus() != before);  // traversal moved it
  CHECK_FALSE(f.m().composing());
  CHECK(f.noteCount() == 1);
  f.d.click(inArrangement);
  f.d.step();
  // The pointer over the roll (the focus still in the arrangement): the keys are macro's.
  f.d.move(f.d.center(kNotes));
  f.d.step();
  CHECK(f.m().takingKeys());
  CHECK(lamp->text() == "MACRO \xC2\xB7 12-EDO");
  f.keys({"2"});
  CHECK(f.m().composing());
  f.keys({"Backspace"});
  // The pointer leaves again: passed through.
  f.d.move({2.0f, 2.0f});
  f.d.step();
  CHECK(lamp->text() == "MACRO (click the notes)");
  CHECK(f.d.press("2") != KeyResult::Widget);
  CHECK_FALSE(f.m().composing());
  // A click on the notes gives the roll the focus: the keys stay macro's wherever the pointer goes.
  f.hover(c);
  f.d.click(f.view->notePoint(c, 0.5));
  f.d.move({2.0f, 2.0f});
  f.d.step();
  CHECK(f.m().takingKeys());
  CHECK(lamp->text() == "MACRO \xC2\xB7 12-EDO");
  f.keys({"2", "`"});
  CHECK(f.noteCount() == 2);
}

TEST_CASE("macro mode (T7.16b, review M19): a macro key during a pointer drag is refused, so Esc on the drag leaves the document as it was") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  f.hover(c);
  const size_t base = f.ctx.store().history().cursor();
  const Vec2 p = f.view->notePoint(c, 0.5);
  f.d.down(p);
  f.d.move({p.x + 70.0f, p.y});
  f.d.move({p.x + 130.0f, p.y});
  f.d.step();
  // Mid-drag: the keys are taken (they must not reach the keymap either) but do nothing.
  CHECK(f.d.press("2") == KeyResult::Widget);
  CHECK(f.d.press("`") == KeyResult::Widget);
  CHECK(f.d.press("b") == KeyResult::Widget);
  CHECK_FALSE(f.m().composing());
  CHECK_FALSE(f.m().lockPending());
  CHECK(f.ctx.status() == "Macro: finish the drag first");
  CHECK(f.noteCount() == 1);
  // Esc cancels the drag: the note is back, no history entry.
  CHECK(f.d.press("Escape") == KeyResult::Widget);
  f.d.up({p.x + 130.0f, p.y});
  f.d.step();
  CHECK(f.noteCount() == 1);
  REQUIRE(f.clip().notes.findById(c) != nullptr);
  CHECK(f.clip().notes.findById(c)->start == 0);
  CHECK(f.ctx.store().history().cursor() == base);
  // After the drag the keys work again.
  f.keys({"2", "`"});
  CHECK(f.noteCount() == 2);
}

TEST_CASE("macro mode (review L14): turning it off gives the computer MIDI keyboard back when it had taken it over") {
  Macro f;
  ComputerKeyboard& kb = f.ctx.computerKeyboard();
  kb.setActive(true);
  f.on();
  CHECK_FALSE(kb.active());
  CHECK(f.d.press("Ctrl+CapsLock") == KeyResult::Action);
  CHECK_FALSE(f.m().on());
  CHECK(kb.active());
  CHECK(f.d.tree().keyListener() == &kb);
  // It was off: it stays off.
  kb.setActive(false);
  f.on();
  CHECK(f.d.press("Ctrl+CapsLock") == KeyResult::Action);
  CHECK_FALSE(kb.active());
}

TEST_CASE("macro mode: - / = undo and redo macro steps with their edits; Ctrl+Z stays the document's undo") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  f.hover(c);
  const size_t base = f.ctx.store().history().cursor();
  f.keys({"2", "`"});
  REQUIRE(f.noteCount() == 2);
  CHECK(f.ctx.store().history().cursor() == base + 1);
  // - : the placement goes (the note with it) and the composition is back as it was before `.
  f.keys({"-"});
  CHECK(f.noteCount() == 1);
  CHECK(f.ctx.store().history().cursor() == base);
  CHECK(f.m().composition() == Rational(3, 2));
  // - : the interval key.
  f.keys({"-"});
  CHECK_FALSE(f.m().composing());
  f.keys({"-"});
  CHECK(f.ctx.status() == "Macro: nothing to undo");
  // = = : both again, the same note back.
  f.keys({"=", "="});
  CHECK(f.noteCount() == 2);
  CHECK(f.find(PitchId::fromDegree(7, 0), 0) != nullptr);
  CHECK(f.ctx.store().history().cursor() == base + 1);
  CHECK_FALSE(f.m().composing());
  f.keys({"="});
  CHECK(f.ctx.status() == "Macro: nothing to redo");

  // Ctrl+Z undoes the document; the macro state is untouched. A macro undo of that placement then leaves the
  // document alone (it is no longer as the step left it) and only takes the cursor back.
  CHECK(f.d.press("Ctrl+Z") == KeyResult::Action);
  CHECK(f.noteCount() == 1);
  CHECK(f.ctx.store().history().cursor() == base);
  f.keys({"-"});
  CHECK(f.noteCount() == 1);
  CHECK(f.ctx.store().history().cursor() == base);
  CHECK(f.m().composition() == Rational(3, 2));
  CHECK(f.ctx.status().find("only the cursor went back") != std::string::npos);
  // A new step drops the macro redo.
  f.keys({"3", "="});
  CHECK(f.ctx.status() == "Macro: nothing to redo");
  CHECK(f.m().composition() == Rational(15, 8));
}

TEST_CASE("macro mode: Ctrl / Alt chords, Space and unbound keys pass through; a focused text field and an open menu keep their keys") {
  Macro f;
  views::test::ScriptedDialogs dialogs;
  f.ctx.setFileDialogs(&dialogs);
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  f.hover(c);
  // Ctrl+S saves (an untitled project asks for a place: the dialog was asked); S did not move the cursor.
  CHECK(f.d.press("Ctrl+S") == KeyResult::Action);
  CHECK(dialogs.saves == 1);
  CHECK_FALSE(f.m().composing());
  // Space plays.
  CHECK(f.d.press("Space") == KeyResult::Action);
  REQUIRE_FALSE(f.port.calls.empty());
  CHECK(f.port.calls.back().call == FakeTransport::Call::Play);
  CHECK(f.d.press("Space") == KeyResult::Action);
  // Ctrl+A is the roll's select all; Alt+2 and Shift+2 are not macro keys.
  CHECK(f.d.press("Ctrl+A") == KeyResult::Action);
  CHECK(f.d.press("Alt+2") != KeyResult::Widget);
  CHECK(f.d.press("Shift+2") != KeyResult::Widget);
  CHECK_FALSE(f.m().composing());
  // A key the preset does not bind goes on to the keymap: B is Draw Mode.
  MacroPreset p = f.m().preset();
  std::erase_if(p.bindings, [](const MacroBinding& b) { return b.op == MacroOp::Lock; });
  f.m().setPreset(p);
  CHECK_FALSE(f.ctx.drawMode());
  CHECK(f.d.press("B") == KeyResult::Action);
  CHECK(f.ctx.drawMode());
  CHECK(f.d.press("B") == KeyResult::Action);
  // A focused canvas does not eat the macro's letters: the note area has the focus and S / F still move the cursor.
  REQUIRE(f.d.tree().focus() == f.d.find(kNotes));
  f.keys({"f"});
  CHECK(f.m().tick() == kQ);
  f.keys({"a"});

  // A text field (the roll's pitch type-in): its keys are its own.
  f.view->select(std::span<const doc::NoteId>(&c, 1));
  TextEdit* field = f.view->openPitchEntry();
  REQUIRE(field != nullptr);
  f.d.step();
  REQUIRE(f.d.tree().focus() == field);
  CHECK(f.d.press("2") == KeyResult::TextSwallowed);
  CHECK(f.d.press("q") == KeyResult::TextSwallowed);
  CHECK(f.d.press("Tab") != KeyResult::Widget);
  CHECK_FALSE(f.m().composing());
  CHECK(f.noteCount() == 1);
  f.shell->overlay().closeAll();
  f.d.step();
  REQUIRE(f.shell->overlay().top() == nullptr);
  // The help panel open over the roll: the keys are not macro's while it is up.
  f.d.tree().setFocus(f.d.find(kNotes));
  REQUIRE(f.view->toggleHelp());
  f.d.step();
  (void)f.d.press("2");
  CHECK_FALSE(f.m().composing());
  (void)f.view->toggleHelp();
  f.d.step();
  f.d.tree().setFocus(f.d.find(kNotes));
  f.keys({"2"});
  CHECK(f.m().composing());
}

TEST_CASE("macro mode and the computer MIDI keyboard exclude each other: one key listener") {
  Macro f;
  ComputerKeyboard& kb = f.ctx.computerKeyboard();
  const std::string kKb = "main/control-bar/computer-keyboard";
  f.d.click(f.d.center(kKb));
  REQUIRE(kb.active());
  // A held note ends when macro mode takes over.
  CHECK(f.d.keyDown('a') == KeyResult::Widget);
  REQUIRE(kb.heldKeys() == 1);
  CHECK(f.d.keyDown(key::kCapsLock, mod::kCtrl) == KeyResult::Action);
  CHECK(f.m().on());
  CHECK_FALSE(kb.active());
  CHECK(kb.heldKeys() == 0);
  CHECK(f.d.tree().keyListener() == &f.m());
  f.d.keyUp('a');
  f.d.keyUp(key::kCapsLock, mod::kCtrl);
  // The letters are macro keys now (the pointer over the roll: the click on the button took the focus): A moves the
  // cursor, it plays no note.
  f.d.move(f.d.center(kNotes));
  f.d.step();
  const size_t calls = f.port.calls.size();
  f.keys({"f", "a"});
  for (size_t i = calls; i < f.port.calls.size(); ++i) CHECK(f.port.calls[i].call != FakeTransport::Call::LiveKey);
  // The keyboard's button: macro mode goes off, the keyboard is the listener again.
  f.d.click(f.d.center(kKb));
  CHECK(kb.active());
  CHECK_FALSE(f.m().on());
  CHECK(f.d.tree().keyListener() == &kb);
  auto* lamp = dynamic_cast<MacroLamp*>(f.d.find(kLamp));
  REQUIRE(lamp != nullptr);
  CHECK_FALSE(lamp->on());
  CHECK(f.d.keyDown('a') == KeyResult::Widget);
  CHECK(kb.heldKeys() == 1);
  f.d.keyUp('a');
}

TEST_CASE("macro mode presets: trainer.json is the built-in table; the keypad preset works by scancode; load errors go to the status line") {
  // The built-in table (what runs when a preset cannot be loaded) and the shipped default say the same.
  const MacroPreset builtin = builtinMacroPreset();
  auto trainer = loadMacroPreset("trainer");
  REQUIRE_MESSAGE(trainer.ok(), (trainer.ok() ? "" : trainer.error().message));
  CHECK(trainer->name == builtin.name);
  CHECK(trainer->primeLimit == builtin.primeLimit);
  REQUIRE(trainer->bindings.size() == builtin.bindings.size());
  CHECK(trainer->bindings.size() == 31);
  for (const MacroBinding& b : builtin.bindings) {
    CAPTURE(b.key.name);
    KeyEvent e;
    e.key = b.key.key;
    const MacroBinding* t = trainer->find(e);
    REQUIRE(t != nullptr);
    CHECK(t->op == b.op);
    CHECK(t->ratio == b.ratio);
  }
  // SPEC §6.2's table.
  const std::pair<char, Rational> table[] = {{'1', Rational(2)},      {'2', Rational(3, 2)},   {'3', Rational(5, 4)},   {'4', Rational(7, 4)},
                                             {'5', Rational(11, 8)},  {'6', Rational(13, 8)},  {'7', Rational(17, 16)}, {'8', Rational(19, 16)},
                                             {'9', Rational(23, 16)}, {'0', Rational(29, 16)}, {'q', Rational(1, 2)},   {'w', Rational(2, 3)},
                                             {'e', Rational(4, 5)},   {'r', Rational(4, 7)},   {'t', Rational(8, 11)},  {'y', Rational(8, 13)},
                                             {'u', Rational(16, 17)}, {'i', Rational(16, 19)}, {'o', Rational(16, 23)}, {'p', Rational(16, 29)}};
  for (const auto& [k, r] : table) {
    KeyEvent e;
    e.key = static_cast<Key>(k);
    const MacroBinding* b = trainer->find(e);
    REQUIRE(b != nullptr);
    CHECK(b->op == MacroOp::Interval);
    CHECK(b->ratio == r);
  }
  CHECK(listMacroPresets() == std::vector<std::string>{"ahk-numpad", "trainer"});
  // Parse errors name what is wrong.
  CHECK_FALSE(parseMacroPreset("{", "x").ok());
  CHECK(parseMacroPreset(R"({"intervals": []})", "x").error().message.find("intervals") != std::string::npos);
  CHECK(parseMacroPreset(R"({"intervals": [{"key": "1", "ratio": "3/2"}, {"key": "1", "ratio": "5/4"}]})", "x").error().message.find("bound twice") !=
        std::string::npos);
  CHECK(parseMacroPreset(R"({"intervals": [{"key": "1", "ratio": "0/2"}]})", "x").error().message.find("ratio") != std::string::npos);
  CHECK(parseMacroPreset(R"({"intervals": [{"key": "Ctrl+1", "ratio": "3/2"}]})", "x").error().message.find("modifiers") != std::string::npos);
  CHECK(parseMacroPreset(R"({"intervals": [{"key": "1", "ratio": "3/2"}], "keys": {"jump": "j"}})", "x").error().message.find("jump") != std::string::npos);

  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  f.hover(c);
  CHECK(f.m().preset().id == "trainer");
  // The preferences pick the preset.
  ViewPrefs prefs = f.ctx.viewPrefs();
  prefs.macroPreset = "ahk-numpad";
  f.ctx.setViewPrefs(prefs);
  CHECK(f.m().preset().id == "ahk-numpad");
  CHECK(f.m().preset().name == "AHK Numpad");
  // The number row is free again (2 is nobody's key in the roll); the keypad composes, whatever key code Num Lock
  // gives its keys.
  CHECK(f.d.press("2") == KeyResult::Unhandled);
  CHECK_FALSE(f.m().composing());
  auto pad = [&](Scancode sc, Key keyCode) {
    KeyEvent e;
    e.scancode = sc;
    e.key = keyCode;
    const KeyResult r = f.d.tree().dispatchKey(e);
    e.down = false;
    (void)f.d.tree().dispatchKey(e);
    f.d.step();
    return r;
  };
  CHECK(pad(Scancode::KpDivide, keyFromScancode(Scancode::KpDivide)) == KeyResult::Widget);  // 3/2
  CHECK(f.m().composition() == Rational(3, 2));
  CHECK(pad(Scancode::Kp8, key::kUp) == KeyResult::Widget);  // Num Lock off: the key code is Up; still 5/4
  CHECK(f.m().composition() == Rational(15, 8));
  CHECK(pad(Scancode::Kp9, keyFromScancode(Scancode::Kp9)) == KeyResult::Widget);  // 4/5
  CHECK(f.m().composition() == Rational(3, 2));
  CHECK(pad(Scancode::KpMinus, keyFromScancode(Scancode::KpMinus)) == KeyResult::Widget);  // undo
  CHECK(f.m().composition() == Rational(15, 8));
  CHECK(pad(Scancode::KpPlus, keyFromScancode(Scancode::KpPlus)) == KeyResult::Widget);  // redo
  CHECK(f.m().composition() == Rational(3, 2));
  CHECK(pad(Scancode::NumLock, keyFromScancode(Scancode::NumLock)) == KeyResult::Widget);  // 2/1
  CHECK(pad(Scancode::Kp7, key::kHome) == KeyResult::Widget);                               // 1/2
  CHECK(f.m().composition() == Rational(3, 2));
  f.keys({"`"});  // the other commands keep the trainer's keys
  CHECK(f.find(PitchId::fromDegree(7, 0), 0) != nullptr);

  // A preset that does not exist: the status line says so and the keys stay.
  prefs.macroPreset = "no-such-preset";
  f.ctx.setViewPrefs(prefs);
  CHECK(f.m().preset().id == "ahk-numpad");
  CHECK(f.ctx.status().find("no-such-preset") != std::string::npos);
  CHECK(f.ctx.status().find("cannot open") != std::string::npos);
  prefs.macroPreset = "trainer";
  f.ctx.setViewPrefs(prefs);
  CHECK(f.m().preset().id == "trainer");
  f.keys({"2"});
  CHECK(f.m().composition() == Rational(3, 2));
}

TEST_CASE("macro mode: the fewest-keypress table equals the trainer's own search for both presets and six EDOs; built once per (EDO, preset)") {
  using Json = nlohmann::json;
  std::ifstream in(std::string(PQ_TEST_DATA_DIR) + "/views/fixtures/macro_bfs.json", std::ios::binary);
  REQUIRE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  const Json fx = Json::parse(ss.str());
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  f.hover(c);
  size_t cases = 0;
  double worstMs = 0.0;
  for (const Json& k : fx.at("cases")) {
    const std::string presetId = k.at("preset").get<std::string>();
    const uint32_t edo = k.at("edo").get<uint32_t>();
    CAPTURE(presetId);
    CAPTURE(edo);
    if (f.m().preset().id != presetId) REQUIRE(f.m().usePreset(presetId).ok());
    CHECK(f.m().preset().primeLimit == k.at("limit").get<uint64_t>());
    f.edo(edo);
    const uint64_t builds = f.m().keypressBuilds();
    const tuning::FewestKeypressMap* map = f.m().keypressMap();
    REQUIRE(map != nullptr);
    CHECK(f.m().keypressBuilds() == builds + 1);
    worstMs = std::max(worstMs, f.m().lastKeypressBuildMs());
    // The order the search iterated the keys in is the trainer's (a JS object's property order).
    const std::vector<tuning::KeyRatio> order = tuning::jsObjectKeyOrder(f.m().preset().intervalKeys());
    const Json& jo = k.at("order");
    REQUIRE(order.size() == jo.size());
    for (size_t i = 0; i < order.size(); ++i) {
      CHECK(order[i].key == jo[i][0].get<std::string>());
      CHECK(order[i].ratio == Rational(jo[i][1].get<int64_t>(), jo[i][2].get<int64_t>()));
    }
    // Every step's ratio.
    const Json& labels = k.at("labels");
    CHECK(map->size() == labels.size());
    const bool oneCharKeys = presetId == "trainer";
    for (auto it = labels.begin(); it != labels.end(); ++it) {
      const int64_t step = std::stoll(it.key());
      CAPTURE(step);
      CHECK(map->ratioLabel(step) == it.value().get<std::string>());
      // What the keys show: the keys to press (one-character keys) or the ratio; pressing them gives that ratio and
      // that many steps.
      const std::string_view shown = f.m().keypressLabel(step);
      if (step == 0) {
        CHECK(shown.empty());
        continue;
      }
      if (!oneCharKeys) {
        CHECK(shown == it.value().get<std::string>());
        continue;
      }
      const tuning::FewestKeypressMap::Entry* e = map->find(step);
      REQUIRE(e != nullptr);
      CHECK(shown.size() == static_cast<size_t>(e->presses));
      Rational product{1};
      int64_t sum = 0;
      for (const char ch : shown) {
        KeyEvent ke;
        ke.key = static_cast<Key>(ch);
        const MacroBinding* b = f.m().preset().find(ke);
        REQUIRE(b != nullptr);
        product = *tuning::mul(product, b->ratio);
        sum += tuning::ratioToEdoSteps(b->ratio, edo);
      }
      CHECK(product == e->ratio);
      CHECK(sum == step);
    }
    // Asked again (every repaint of the keys does): no rebuild.
    (void)f.m().keypressMap();
    (void)f.m().keypressLabel(7);
    CHECK(f.m().keypressBuilds() == builds + 1);
    ++cases;
  }
  CHECK(cases == 12);
  // Back in a tuning seen before with the same preset: still cached.
  const uint64_t builds = f.m().keypressBuilds();
  f.edo(53);
  (void)f.m().keypressMap();
  CHECK(f.m().keypressBuilds() == builds);
  MESSAGE("fewest-keypress table: slowest build of the 12 (EDO, preset) pairs " << worstMs << " ms");
  CHECK(worstMs < 50.0);
  // In 12-EDO with the trainer's keys: a fifth is `2`, a major seventh `23`, an octave down `q`.
  REQUIRE(f.m().usePreset("trainer").ok());
  f.edo(12);
  CHECK(f.m().keypressLabel(7) == "2");
  CHECK(f.m().keypressLabel(11) == "23");
  CHECK(f.m().keypressLabel(-12) == "q");
  CHECK(f.m().keypressLabel(0).empty());
  CHECK(f.m().keypressLabel(49).empty());  // beyond two equaves
}

TEST_CASE("macro mode: the view scrolls to keep the cursor visible; the JI layer from a free cursor; a deleted anchor leaves a free cursor") {
  Macro f;
  const doc::NoteId c = f.add(PitchId::fromDegree(0, 0));
  f.on();
  f.hover(c);
  f.d.move({2.0f, 2.0f});
  const IRect area = f.d.rect(kNotes);
  // Three octaves up is far above the view: the roll scrolls and the cursor box is inside the note area.
  f.keys({"1", "1", "1"});
  IRect cur = f.view->macroCursorRect();
  REQUIRE_FALSE(cur.empty());
  CHECK(cur.y >= area.y);
  CHECK(cur.bottom() <= area.bottom());
  CHECK(f.m().target().pitch == PitchId::fromDegree(0, 3));
  f.keys({"Backspace"});
  cur = f.view->macroCursorRect();
  REQUIRE_FALSE(cur.empty());
  CHECK(cur.y >= area.y);
  CHECK(cur.bottom() <= area.bottom());
  // Far to the right.
  for (int i = 0; i < 14; ++i) f.keys({"f"});
  cur = f.view->macroCursorRect();
  REQUIRE_FALSE(cur.empty());
  CHECK(cur.x >= area.x);
  CHECK(cur.right() <= area.right());
  for (int i = 0; i < 14; ++i) f.keys({"a"});
  CHECK(f.m().tick() == 0);

  // The anchor note deleted behind macro mode's back (the roll's Delete): a free cursor on its row.
  f.view->select(std::span<const doc::NoteId>(&c, 1));
  f.view->deleteSelection();
  f.d.step();
  CHECK(f.noteCount() == 0);
  CHECK_FALSE(f.m().anchorNote().valid());
  CHECK(f.m().target().pitch == PitchId::fromDegree(0, 0));
  // JI layer from a free cursor on the reference's row: a Ratio over the reference; with nothing composed: a root on
  // the row, which becomes the anchor.
  f.keys({"g", "2"});
  CHECK(f.m().target().pitch == PitchId::fromRatio(Rational(3, 2)));
  f.keys({"`"});
  CHECK(f.find(PitchId::fromRatio(Rational(3, 2)), 0) != nullptr);
  f.keys({"s", "s"});  // the free cursor two rows up: no reference row
  f.keys({"3"});
  MacroMode::Target t = f.m().target();
  CHECK(t.pitch.kind() == tuning::PitchKind::Cents);
  CHECK(t.pitch.cents() == doctest::Approx(200.0 + 386.3137).epsilon(1e-6));
  CHECK(f.m().detailText() == "exact");
  f.keys({"Backspace", "`"});
  const doc::Note* root = f.find(PitchId::fromDegree(2, 0), 0);
  REQUIRE(root != nullptr);
  CHECK(f.m().anchorNote() == root->id);
  f.keys({"3", "`"});
  CHECK(f.find(PitchId::fromRatio(Rational(5, 4), tuning::AnchorKind::Note, root->id.value), 0) != nullptr);
}

TEST_CASE("macro mode: off costs nothing per frame; on, an idle frame draws nothing") {
  Macro f;
  (void)f.add(PitchId::fromDegree(0, 0));
  f.d.step();
  // Off: idle frames are not drawn, the overlays are hidden.
  const uint64_t frames = f.d.framesDrawn();
  for (int i = 0; i < 10; ++i) f.d.step();
  CHECK(f.d.framesDrawn() == frames);
  f.on();
  f.d.step();
  const uint64_t onFrames = f.d.framesDrawn();
  for (int i = 0; i < 10; ++i) f.d.step();
  CHECK(f.d.framesDrawn() == onFrames);  // no timer, no frame request while nothing changes
  CHECK(f.m().keypressBuilds() <= 1);
}

TEST_CASE("render: pianoroll-macro-tempered (macro mode in 12-EDO: anchor on C, 3/2 · 5/4 = 15/8 → 11 steps, the keys' labels) matches the golden image") {
  checkGolden("pianoroll-macro-tempered");
}
TEST_CASE("render: pianoroll-macro-ji (macro mode's JI layer on a declared series: just · harmonic 7) matches the golden image") {
  checkGolden("pianoroll-macro-ji");
}
