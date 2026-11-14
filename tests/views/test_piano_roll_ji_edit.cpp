// T7.5 piano roll: the inner roll and morph editing of JI objects, Tune to JI, the fundamental with the mouse. Every
// gesture goes through the real shell, pointer and key dispatch (TestDriver → UiTree → the note area / the keymap /
// the menus / the inline fields), one undo step each:
//   enter (double-click) / leave (Esc, double-click outside); inner notes drawn on the voice under the pointer, hit
//   where they are drawn (click, drag, resize, Up / Down, a vertical drag between voices); a curved note hit on its
//   curve; stops (typed proportion at the pointer, T, drag a label, drag the glide grip, Delete, retype); a voice's
//   transition (shape, bezier handles, its own glide / delay, the same for all); walking a harmonic (Alt+Up / Down);
//   the fundamental with Alt (free cents); undertone; Tune to JI (chord + menu), back to the tuning, removing it; in
//   31-EDO and in an Equidistant MOS roll; "Is Harmonic…" / "Is Subharmonic…" / Unlock; Ctrl+C / Ctrl+V / Ctrl+X of
//   an object.
// Goldens: pianoroll-ji-entered, pianoroll-tune-to-ji, pianoroll-tune-to-ji-31edo, pianoroll-fundamental-lock.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using pq::tuning::PitchId;
using pq::tuning::PitchKind;
using pq::tuning::Rational;
using JiPart = pq::views::ClipView::JiPart;

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
  std::span<const doc::JiObject> objects() const { return doc::jiObjectsOf(clip()); }
  const doc::JiObject& object(doc::JiObjectId id) const {
    const doc::JiObject* o = doc::findJiObject(clip(), id);
    REQUIRE(o != nullptr);
    return *o;
  }
  const doc::Note& note(doc::NoteId id) const {
    const doc::Note* n = clip().notes.findById(id);
    REQUIRE(n != nullptr);
    return *n;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  Vec2 at(double tick, int64_t degree) const { return {view->xOfTick(tick), view->yOfDegree(degree)}; }
  tuning::Tuning tuning() const {
    const std::optional<doc::TrackPath> path = doc::findTrack(*ctx.doc(), ref.track);
    REQUIRE(path.has_value());
    auto t = doc::buildTuning(*doc::effectiveTuningRef(*ctx.doc(), *path, &clip()).def);
    REQUIRE(t.ok());
    return *t;
  }
  // The clip on its own named tuning.
  void setTuning(std::string name, doc::TuningDef def) {
    auto tb = doc::ops::addMainTuning(*ctx.doc(), ctx.store().ids(), std::move(name), std::move(def), doc::Colour{0x8FC24A});
    REQUIRE(tb.ok());
    REQUIRE(ctx.store().dispatch(tb->command).ok());
    REQUIRE(ctx.store().dispatch(doc::SetClipTuning{ref, doc::TuningAssignment::mainTuning(tb->tunings.at(0))}).ok());
    d.tree().layoutNow();
    d.step();
  }
  // A click in the note area's top-right corner gives it the focus (the pianoroll key scope).
  void focusNotes() {
    const IRect a = d.rect("main/body/bottom/detail/clip/clip-view/notes");
    REQUIRE_FALSE(a.empty());
    d.click({static_cast<float>(a.right()) - 6.0f, static_cast<float>(a.y) + 6.0f});
    d.step();
    REQUIRE(d.tree().focus() == d.find("main/body/bottom/detail/clip/clip-view/notes"));
  }
  bool notesFocused() { return d.tree().focus() == d.find("main/body/bottom/detail/clip/clip-view/notes"); }
  // 4:5:6:7 over [start, start + len) on the row `degree`, its lowest voice pinned there as the JI tool does.
  doc::JiObjectId makeObject(Ticks start, Ticks len, int32_t degree) {
    const int64_t terms[] = {4, 5, 6, 7};
    auto e = doc::ops::createJiObject(*ctx.doc(), ctx.store().ids(), ref, start, len, PitchId::fromDegree(degree, 0), terms);
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    const doc::JiObjectId id = e->jiObjects.at(0);
    std::vector<doc::JiStop> stops = object(id).stops;
    stops[0].scale = Rational(1, 4);
    auto s = doc::ops::setJiStops(*ctx.doc(), ctx.store().ids(), ref, id, stops);
    REQUIRE(s.ok());
    REQUIRE(ctx.store().dispatch(s->command).ok());
    d.tree().layoutNow();
    d.step();
    return id;
  }
  void addStop(doc::JiObjectId id, Ticks at, std::vector<int64_t> terms, Ticks glide) {
    auto e = doc::ops::addJiStop(*ctx.doc(), ctx.store().ids(), ref, id, at, terms, glide);
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    d.step();
  }
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
  Vec2 header(doc::JiObjectId id) {
    const IRect r = view->jiObjectRect(id);
    REQUIRE_FALSE(r.empty());
    return {static_cast<float>(r.x) + 24.0f, static_cast<float>(r.y) - 8.0f};
  }
  // A point inside the object's box between voices `voice` and `voice + 1` at object tick `tick` (on no voice line).
  Vec2 between(doc::JiObjectId id, uint32_t voice, Ticks tick) {
    const Vec2 a = view->jiVoicePoint(id, voice, tick), b = view->jiVoicePoint(id, voice + 1, tick);
    return {a.x, 0.5f * (a.y + b.y)};
  }
  Vec2 part(doc::JiObjectId id, JiPart p, int32_t index) {
    const IRect r = view->jiPartRect(id, p, index);
    REQUIRE_MESSAGE(!r.empty(), "the part is on screen");
    return mid(r);
  }
  void enter(doc::JiObjectId id) {
    d.doubleClick(header(id));
    d.step();
    REQUIRE(view->jiEntered() == id);
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
  // Right-click `p`, open the "Voice …" submenu and choose `label` in it.
  void chooseVoiceItem(Vec2 p, std::string_view label) {
    Menu* m = rightClick(p);
    REQUIRE(m != nullptr);
    const int32_t voice = itemOf(m, "Voice");
    REQUIRE_MESSAGE(voice >= 0, "the menu has the voice's transition");
    d.click(mid(m->itemRect(voice)));
    d.step();
    Menu* sub = m->submenu();
    REQUIRE(sub != nullptr);
    choose(sub, label);
  }
  void typeEnter(std::string_view text) {
    REQUIRE(field() != nullptr);
    REQUIRE(d.type(text));
    (void)d.press("Enter");
    d.step();
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

TEST_CASE("roll JI edit: a double-click enters an object, Esc or a double-click outside leaves; no edit") {
  Ji r;
  r.focusNotes();
  const doc::JiObjectId id = r.makeObject(2 * kQ, 4 * kQ, 0);
  const size_t h = r.history();

  r.d.doubleClick(r.header(id));
  r.d.step();
  CHECK(r.view->jiEntered() == id);
  CHECK(r.d.press("Escape") == KeyResult::Action);
  CHECK_FALSE(r.view->jiEntered().valid());
  CHECK(r.view->jiSelection() == id);  // as before entering

  // Inside the box, on no note: enters (and adds no plain note there).
  r.d.doubleClick(r.between(id, 0, kQ));
  r.d.step();
  CHECK(r.view->jiEntered() == id);
  CHECK(r.clip().notes.size() == 0);

  // A single click outside changes nothing; a double-click outside leaves.
  r.d.click(r.at(8.5 * static_cast<double>(kQ), 20));
  CHECK(r.view->jiEntered() == id);
  r.d.doubleClick(r.at(8.5 * static_cast<double>(kQ), 20));
  r.d.step();
  CHECK_FALSE(r.view->jiEntered().valid());
  CHECK(r.clip().notes.size() == 0);  // the double-click that leaves adds no note
  CHECK(r.history() == h);
}

TEST_CASE("roll JI edit: inner notes go on the voice under the pointer and are hit where they are drawn (click, Up / Down, drag, resize)") {
  Ji r;
  r.focusNotes();
  const doc::JiObjectId id = r.makeObject(2 * kQ, 4 * kQ, 0);
  r.enter(id);
  size_t h = r.history();

  // A double-click on voice 2 in the object's second beat: a Harmonic note filling that grid cell.
  r.d.doubleClick(r.view->jiVoicePoint(id, 2, 3 * kQ / 2));
  r.d.step();
  REQUIRE(r.clip().notes.size() == 1);
  CHECK(r.history() == h + 1);
  const doc::NoteId n = r.clip().notes.begin()->id;
  CHECK(r.note(n).pitch == PitchId::fromHarmonic(id.value, 2));
  CHECK(r.note(n).start == 3 * kQ);
  CHECK(r.note(n).length == kQ);
  CHECK(r.view->jiEntered() == id);
  REQUIRE(r.view->selection().size() == 1);

  // Up / Down: the next voice.
  CHECK(r.d.press("Up") == KeyResult::Action);
  CHECK(r.note(n).pitch == PitchId::fromHarmonic(id.value, 3));
  CHECK(r.d.press("Down") == KeyResult::Action);
  CHECK(r.d.press("Down") == KeyResult::Action);
  CHECK(r.note(n).pitch == PitchId::fromHarmonic(id.value, 1));
  CHECK(r.history() == h + 4);

  // A click on empty space deselects; a click on the note where it is drawn (on voice 1's line) selects it.
  r.d.click(r.between(id, 2, kQ / 2));
  CHECK(r.view->selection().empty());
  r.d.click(r.view->notePoint(n, 0.5));
  REQUIRE(r.view->selection().size() == 1);
  CHECK(r.view->selection()[0] == n.value);
  CHECK(r.view->jiEntered() == id);

  // Drag it one beat later; drag its end one beat longer; drag it up onto voice 3.
  h = r.history();
  const float beat = static_cast<float>(kQ * r.view->pxPerTick());
  const Vec2 body = r.view->notePoint(n, 0.5);
  r.d.drag(body, {body.x + beat, body.y}, 5);
  r.d.step();
  CHECK(r.note(n).start == 4 * kQ);
  CHECK(r.note(n).pitch == PitchId::fromHarmonic(id.value, 1));
  const Vec2 end = r.view->notePoint(n, 1.0);
  r.d.drag({end.x - 2.0f, end.y}, {end.x - 2.0f + beat, end.y}, 5);
  r.d.step();
  CHECK(r.note(n).start == 4 * kQ);
  CHECK(r.note(n).length == 2 * kQ);
  const Vec2 from = r.view->notePoint(n, 0.25);
  const Vec2 to = r.view->jiVoicePoint(id, 3, r.note(n).start - 2 * kQ + kQ / 2);
  r.d.drag(from, {from.x, to.y}, 6);
  r.d.step();
  CHECK(r.note(n).pitch == PitchId::fromHarmonic(id.value, 3));
  CHECK(r.note(n).start == 4 * kQ);
  CHECK(r.history() == h + 3);

  // Draw Mode inside the object: a click puts a note on the voice under the pointer.
  h = r.history();
  CHECK(r.d.press("B") == KeyResult::Action);
  REQUIRE(r.ctx.drawMode());
  r.d.click(r.view->jiVoicePoint(id, 0, kQ / 2));
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.clip().notes.size() == 2);
  int onVoice0 = 0;
  for (const doc::Note& m : r.clip().notes)
    if (m.pitch == PitchId::fromHarmonic(id.value, 0) && m.start == 2 * kQ && m.length == kQ) ++onVoice0;
  CHECK(onVoice0 == 1);
  (void)r.d.press("B");
  CHECK(doc::validateClipJi(r.clip()).ok());

  REQUIRE(r.ctx.undo());
  CHECK(r.clip().notes.size() == 1);
}

TEST_CASE("roll JI edit: a note with a pitch curve is hit on its curve, not on its row") {
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

TEST_CASE("roll JI edit: stops — a typed proportion at the pointer, T, dragging a label and the glide grip, Delete, retyping") {
  Ji r;
  r.focusNotes();
  const doc::JiObjectId id = r.makeObject(2 * kQ, 8 * kQ, 0);
  r.enter(id);
  size_t h = r.history();

  // The pointer rests four beats into the object; a digit opens the field; Enter adds the stop there.
  r.d.move(r.between(id, 0, 4 * kQ));
  CHECK(r.d.press("5") == KeyResult::Widget);
  r.d.step();
  REQUIRE(r.field() != nullptr);
  REQUIRE(r.d.type("5:x"));
  (void)r.d.press("Enter");
  r.d.step();
  CHECK(r.object(id).stops.size() == 1);  // not a proportion: the field stays, nothing changed
  REQUIRE(r.shell->overlay().openCount() == 1);
  auto* err = dynamic_cast<Label*>(r.shell->overlay().top()->find("error"));
  REQUIRE(err != nullptr);
  CHECK_FALSE(err->text().empty());
  for (int i = 0; i < 3; ++i) (void)r.d.press("Backspace");
  r.typeEnter("5:6:7:8");
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.history() == h + 1);
  {
    const doc::JiObject& o = r.object(id);
    REQUIRE(o.stops.size() == 2);
    CHECK(o.stops[1].at == 4 * kQ);
    CHECK(o.stops[1].terms == std::vector<int64_t>{5, 6, 7, 8});
    CHECK(o.stops[1].scale == Rational(1, 4));  // the same fundamental
    CHECK(o.stops[1].timing.glide == kQ);       // one grid step
  }
  CHECK(r.view->jiStopSelection() == 1);
  CHECK(r.view->jiEntered() == id);
  REQUIRE(r.notesFocused());  // the keys are back with the notes

  // T: back to the tuning at the pointer.
  r.d.move(r.between(id, 0, 6 * kQ));
  CHECK(r.d.press("T") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 2);
  {
    const doc::JiObject& o = r.object(id);
    REQUIRE(o.stops.size() == 3);
    CHECK(o.stops[2].at == 6 * kQ);
    CHECK(o.stops[2].isTuning());
    CHECK(o.stops[2].pitches.size() == 4);
  }

  // Drag stop 1's label one beat later.
  const float beat = static_cast<float>(kQ * r.view->pxPerTick());
  Vec2 p = r.part(id, JiPart::StopLabel, 1);
  r.d.drag(p, {p.x + beat, p.y}, 5);
  r.d.step();
  CHECK(r.history() == h + 3);
  CHECK(r.object(id).stops[1].at == 5 * kQ);
  CHECK(r.view->jiStopSelection() == 1);

  // Drag its glide grip one beat to the left: the glide is two beats.
  p = r.part(id, JiPart::Glide, 1);
  r.d.drag(p, {p.x - beat, p.y}, 5);
  r.d.step();
  CHECK(r.history() == h + 4);
  CHECK(r.object(id).stops[1].timing.glide == 2 * kQ);
  CHECK(r.object(id).stops[1].at == 5 * kQ);

  // A click on a label selects the stop; Delete removes it (not the object).
  r.d.click(r.part(id, JiPart::StopLabel, 2));
  CHECK(r.view->jiStopSelection() == 2);
  CHECK(r.history() == h + 4);
  CHECK(r.d.press("Delete") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 5);
  REQUIRE(r.objects().size() == 1);
  CHECK(r.object(id).stops.size() == 2);
  CHECK(r.view->jiStopSelection() == -1);

  // A double-click on a label retypes it.
  r.d.doubleClick(r.part(id, JiPart::StopLabel, 1));
  r.d.step();
  REQUIRE(r.field() != nullptr);
  CHECK(r.field()->text() == "5:6:7:8");
  r.typeEnter("6:7:8:9");  // the old text is selected: typing replaces it
  CHECK(r.history() == h + 6);
  CHECK(r.object(id).stops[1].terms == std::vector<int64_t>{6, 7, 8, 9});
  CHECK(r.object(id).stops[1].at == 5 * kQ);

  for (int i = 0; i < 6; ++i) REQUIRE(r.ctx.undo());
  CHECK(r.object(id).stops.size() == 1);
  CHECK(r.history() == h);
}

TEST_CASE("roll JI edit: a voice's transition — shape from the menu, bezier handles, its own glide and delay, the same for all") {
  Ji r;
  r.focusNotes();
  const doc::JiObjectId id = r.makeObject(2 * kQ, 8 * kQ, 0);
  r.addStop(id, 4 * kQ, {5, 6, 7, 8}, 2 * kQ);
  size_t h = r.history();
  // Voice 1 mid-glide (it glides over object ticks 2 … 4 beats).
  const Vec2 onVoice = r.view->jiVoicePoint(id, 1, 3 * kQ);

  r.chooseVoiceItem(onVoice, "Straight");
  CHECK(r.history() == h + 1);
  {
    const doc::JiStop& s = r.object(id).stops[1];
    REQUIRE(s.voiceTiming.size() == 4);
    REQUIRE(s.voiceTiming[1].has_value());
    CHECK(s.voiceTiming[1]->shape.kind == doc::JiShape::LogLinear);
    CHECK_FALSE(s.voiceTiming[0].has_value());
    CHECK(s.timing.shape.kind == doc::JiShape::SCurve);  // the others keep the stop's default
  }
  r.chooseVoiceItem(r.view->jiVoicePoint(id, 1, 3 * kQ), "Bezier");
  CHECK(r.history() == h + 2);
  CHECK(r.object(id).stops[1].voiceTiming[1]->shape.kind == doc::JiShape::Bezier);

  // Inside the object the bezier's handles drag.
  r.enter(id);
  const Vec2 hd = r.view->jiBezierHandle(id, 1, 1, 0);
  REQUIRE(hd.x >= 0.0f);
  const doc::JiShape before = r.object(id).stops[1].voiceTiming[1]->shape;
  r.d.drag(hd, {hd.x + 24.0f, hd.y - 12.0f}, 6);
  r.d.step();
  CHECK(r.history() == h + 3);
  {
    const doc::JiShape now = r.object(id).stops[1].voiceTiming[1]->shape;
    CHECK(now.kind == doc::JiShape::Bezier);
    CHECK(now.x1 == doctest::Approx(before.x1 + 24.0f / 120.0f).epsilon(0.05));  // the glide is 120 px wide
    CHECK(now.y1 > before.y1 + 0.1f);
    CHECK(now.x2 == before.x2);
    CHECK(now.y2 == before.y2);
  }

  // Its own glide and delay, typed in beats.
  r.chooseVoiceItem(r.view->jiVoicePoint(id, 1, 3 * kQ), "Glide");
  REQUIRE(r.field() != nullptr);
  CHECK(r.field()->text() == "2");
  r.typeEnter("0.5");
  CHECK(r.history() == h + 4);
  CHECK(r.object(id).stops[1].voiceTiming[1]->glide == kQ / 2);
  CHECK(r.object(id).stops[1].timing.glide == 2 * kQ);
  r.chooseVoiceItem(r.view->jiVoicePoint(id, 1, 7 * kQ / 2 + kQ / 4), "Delay");
  REQUIRE(r.field() != nullptr);
  r.typeEnter("0.25");
  CHECK(r.history() == h + 5);
  CHECK(r.object(id).stops[1].voiceTiming[1]->delay == kQ / 4);

  // The same for all voices: the override becomes the stop's timing.
  r.chooseVoiceItem(r.view->jiVoicePoint(id, 1, 4 * kQ), "Same for All");
  CHECK(r.history() == h + 6);
  CHECK(r.object(id).stops[1].voiceTiming.empty());
  CHECK(r.object(id).stops[1].timing.glide == kQ / 2);
  CHECK(r.object(id).stops[1].timing.delay == kQ / 4);
  CHECK(r.object(id).stops[1].timing.shape.kind == doc::JiShape::Bezier);
}

TEST_CASE("roll JI edit: a harmonic number is selected and walked with Alt+Up / Down; Alt-drag bends the fundamental; undertone") {
  Ji r;
  r.focusNotes();
  const doc::JiObjectId id = r.makeObject(2 * kQ, 4 * kQ, 0);
  size_t h = r.history();
  r.d.click(r.header(id));
  REQUIRE(r.view->jiSelection() == id);
  CHECK(r.d.press("Alt+Up") != KeyResult::Action);  // no harmonic selected: the chord falls through

  r.d.click(r.part(id, JiPart::VoiceLabel, 3));
  CHECK(r.view->jiTermSelection() == 3);
  CHECK(r.history() == h);
  CHECK(r.d.press("Alt+Up") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.object(id).stops[0].terms == std::vector<int64_t>{4, 5, 6, 8});
  CHECK(r.object(id).stops[0].scale == Rational(1, 4));  // the lowest voice stays
  CHECK(r.view->jiTermSelection() == 3);
  // Shift: the highest voice is pinned — walking it moves every other voice instead (the scale changes).
  const tuning::Tuning t = r.tuning();
  const double top = *doc::jiVoiceCents(r.object(id), t, 3, 0.0);
  CHECK(r.d.press("Alt+Shift+Down") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 2);
  CHECK(r.object(id).stops[0].terms == std::vector<int64_t>{4, 5, 6, 7});
  CHECK(r.object(id).stops[0].scale == Rational(2, 7));
  CHECK(*doc::jiVoiceCents(r.object(id), t, 3, 0.0) == doctest::Approx(top));
  // A walk that would break the order is refused: nothing changes.
  r.d.click(r.part(id, JiPart::VoiceLabel, 1));
  REQUIRE(r.view->jiTermSelection() == 1);
  (void)r.d.press("Alt+Up");  // 5 -> 6 = the next term
  CHECK(r.history() == h + 2);
  REQUIRE(r.ctx.undo());
  REQUIRE(r.ctx.undo());

  // Alt-drag from the header: the fundamental moves freely (half a row = 50 cents), not by rows.
  h = r.history();
  const Vec2 from = r.header(id);
  r.d.drag(from, {from.x, from.y - 7.0f}, 6, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.object(id).fundamental.kind() == PitchKind::Cents);
  CHECK(r.object(id).fundamental.cents() == doctest::Approx(50.0).epsilon(0.2));
  CHECK(r.object(id).start == 2 * kQ);
  REQUIRE(r.ctx.undo());
  CHECK(r.object(id).fundamental == PitchId::fromDegree(0, 0));

  // Undertone from the object's menu: the lowest voice stays where it is.
  h = r.history();
  auto lowest = [&] {
    double lo = 1e300;
    for (uint32_t v = 0; v < 4; ++v) lo = std::min(lo, *doc::jiVoiceCents(r.object(id), t, v, 0.0));
    return lo;
  };
  const double low = lowest();
  r.choose(r.rightClick(r.header(id)), "Undertone");
  CHECK(r.history() == h + 1);
  CHECK(r.object(id).undertone);
  CHECK(lowest() == doctest::Approx(low).epsilon(1e-9));
  r.choose(r.rightClick(r.header(id)), "Undertone");
  CHECK_FALSE(r.object(id).undertone);
  CHECK(r.object(id).stops[0].scale == Rational(1, 4));
}

TEST_CASE("roll JI edit: Tune to JI — the selected chord glides from 12-EDO to 4:5:6; back to the tuning; removing it gives the notes back") {
  Ji r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Ji::plain(kQ, 4 * kQ, PitchId::fromDegree(0, 0)), Ji::plain(kQ, 4 * kQ, PitchId::fromDegree(4, 0)),
                                                   Ji::plain(kQ, 4 * kQ, PitchId::fromDegree(7, 0))});
  size_t h = r.history();
  CHECK(r.d.press("Ctrl+Shift+J") != KeyResult::Action);  // nothing selected: the chord falls through
  CHECK(r.d.press("Ctrl+A") == KeyResult::Action);
  CHECK(r.d.press("Ctrl+Shift+J") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.objects().size() == 1);
  const doc::JiObjectId id = r.objects()[0].id;
  {
    const doc::JiObject& o = r.object(id);
    CHECK_FALSE(o.sounds);  // the notes play, bent by their voices (Q275)
    CHECK(o.start == kQ);
    CHECK(o.length == 4 * kQ);
    REQUIRE(o.stops.size() == 2);
    CHECK(o.stops[0].isTuning());
    CHECK(o.stops[0].pitches == std::vector<PitchId>{PitchId::fromDegree(0, 0), PitchId::fromDegree(4, 0), PitchId::fromDegree(7, 0)});
    CHECK(o.stops[1].terms == std::vector<int64_t>{4, 5, 6});
    CHECK(o.stops[1].at == kQ);            // one grid step of glide
    CHECK(o.stops[1].timing.glide == kQ);
    const tuning::Tuning t = r.tuning();
    CHECK(*doc::jiVoiceCents(o, t, 1, 0.0) == doctest::Approx(400.0));
    CHECK(*doc::jiVoiceCents(o, t, 1, static_cast<double>(2 * kQ)) == doctest::Approx(tuning::cents(Rational(5, 4))));
    CHECK(*doc::jiVoiceCents(o, t, 0, static_cast<double>(2 * kQ)) == doctest::Approx(0.0));  // the lowest note stays
  }
  for (size_t i = 0; i < ids.size(); ++i) CHECK(r.note(ids[i]).pitch == PitchId::fromHarmonic(id.value, static_cast<uint32_t>(i)));
  CHECK(r.view->jiSelection() == id);
  // The notes are drawn (and hit) on their voices: E has come down from its 12-EDO row after the glide.
  CHECK(r.view->notePoint(ids[1], 0.0).y == doctest::Approx(r.view->yOfDegree(4)).epsilon(0.001));
  CHECK(r.view->notePoint(ids[1], 0.9).y > r.view->yOfDegree(4) + 1.0f);

  // Back to the tuning from the object's menu, three beats in.
  r.choose(r.rightClick(r.between(id, 0, 3 * kQ)), "Back to Tuning");
  CHECK(r.history() == h + 2);
  {
    const doc::JiObject& o = r.object(id);
    REQUIRE(o.stops.size() == 3);
    CHECK(o.stops[2].at == 3 * kQ);
    CHECK(o.stops[2].isTuning());
    CHECK(o.stops[2].pitches == o.stops[0].pitches);
    CHECK(*doc::jiVoiceCents(o, r.tuning(), 1, static_cast<double>(4 * kQ)) == doctest::Approx(400.0));
  }

  // Removing the JI tuning gives the plain notes back.
  r.choose(r.rightClick(r.header(id)), "Remove JI Tuning");
  CHECK(r.history() == h + 3);
  CHECK(r.objects().empty());
  CHECK(r.note(ids[0]).pitch == PitchId::fromDegree(0, 0));
  CHECK(r.note(ids[1]).pitch == PitchId::fromDegree(4, 0));
  CHECK(r.note(ids[2]).pitch == PitchId::fromDegree(7, 0));

  // The same from the notes' right-click menu.
  CHECK(r.d.press("Ctrl+A") == KeyResult::Action);
  r.choose(r.rightClick(r.view->notePoint(ids[1], 0.5)), "Tune to JI");
  CHECK(r.history() == h + 4);
  REQUIRE(r.objects().size() == 1);
  CHECK(r.objects()[0].stops[1].terms == std::vector<int64_t>{4, 5, 6});
  REQUIRE(r.ctx.undo());
  CHECK(r.objects().empty());
  CHECK(r.note(ids[1]).pitch == PitchId::fromDegree(4, 0));
}

TEST_CASE("roll JI edit: Tune to JI in 31-EDO, and in an Equidistant MOS roll where the inner notes are entered, hit and moved") {
  SUBCASE("31-EDO") {
    Ji r;
    doc::TuningDef def;
    def.system = *tuning::Edo::make(31);
    r.setTuning("31-EDO", def);
    r.view->centreOnDegree(14);
    r.d.tree().layoutNow();
    r.focusNotes();
    const std::vector<doc::NoteId> ids = r.addNotes({Ji::plain(kQ, 4 * kQ, PitchId::fromDegree(0, 0)), Ji::plain(kQ, 4 * kQ, PitchId::fromDegree(10, 0)),
                                                     Ji::plain(kQ, 4 * kQ, PitchId::fromDegree(18, 0))});
    const size_t h = r.history();
    (void)r.d.press("Ctrl+A");
    CHECK(r.d.press("Ctrl+Shift+J") == KeyResult::Action);
    r.d.step();
    CHECK(r.history() == h + 1);
    REQUIRE(r.objects().size() == 1);
    const doc::JiObject& o = r.objects()[0];
    CHECK(o.stops[0].pitches == std::vector<PitchId>{PitchId::fromDegree(0, 0), PitchId::fromDegree(10, 0), PitchId::fromDegree(18, 0)});
    CHECK(o.stops[1].terms == std::vector<int64_t>{4, 5, 6});
    const tuning::Tuning t = r.tuning();
    CHECK(*doc::jiVoiceCents(o, t, 1, 0.0) == doctest::Approx(1200.0 * 10 / 31));
    CHECK(*doc::jiVoiceCents(o, t, 1, static_cast<double>(3 * kQ)) == doctest::Approx(tuning::cents(Rational(5, 4))));
    CHECK(r.note(ids[2]).pitch == PitchId::fromHarmonic(o.id.value, 2));
  }
  SUBCASE("5L 2s, Equidistant rows") {
    Ji r;
    tuning::MosParams p;
    p.generator = 1200.0 * 18 / 31;
    p.n = 7;
    p.period = 1200.0;
    p.rotation = 4;
    p.region = tuning::RegionHint{7, 1200.0 * 4 / 7, 720.0};
    doc::TuningDef def;
    def.system = p;
    r.setTuning("Meantone[7]", def);
    r.view->setScaleLayout(roll::RowLayout::Equidistant);
    r.view->centreOnDegree(4);
    r.d.tree().layoutNow();
    r.focusNotes();
    // C E G of the scale (degrees 0, 2, 4): 0, 387, 697 cents.
    const std::vector<doc::NoteId> ids = r.addNotes({Ji::plain(kQ, 2 * kQ, PitchId::fromDegree(0, 0)), Ji::plain(kQ, 2 * kQ, PitchId::fromDegree(2, 0)),
                                                     Ji::plain(kQ, 4 * kQ, PitchId::fromDegree(4, 0))});
    size_t h = r.history();
    (void)r.d.press("Ctrl+A");
    CHECK(r.d.press("Ctrl+Shift+J") == KeyResult::Action);
    r.d.step();
    CHECK(r.history() == h + 1);
    REQUIRE(r.objects().size() == 1);
    const doc::JiObjectId id = r.objects()[0].id;
    CHECK(r.object(id).stops[1].terms == std::vector<int64_t>{4, 5, 6});
    CHECK(r.object(id).length == 4 * kQ);

    // Enter it; the middle note is hit where it is drawn in the Equidistant layout, steps to the next voice and drags.
    r.enter(id);
    r.d.click(r.between(id, 1, 7 * kQ / 2));
    REQUIRE(r.view->selection().empty());
    r.d.click(r.view->notePoint(ids[1], 0.8));
    REQUIRE(r.view->selection().size() == 1);
    CHECK(r.view->selection()[0] == ids[1].value);
    h = r.history();
    const Vec2 body = r.view->notePoint(ids[1], 0.8);
    r.d.drag(body, {body.x + static_cast<float>(2 * kQ * r.view->pxPerTick()), body.y}, 5);
    r.d.step();
    CHECK(r.history() == h + 1);
    CHECK(r.note(ids[1]).start == 3 * kQ);
    CHECK(r.note(ids[1]).pitch == PitchId::fromHarmonic(id.value, 1));
    // A double-click on the lowest voice's free end adds a note there.
    r.d.doubleClick(r.view->jiVoicePoint(id, 0, 5 * kQ / 2));
    r.d.step();
    CHECK(r.history() == h + 2);
    int added = 0;
    for (const doc::Note& n : r.clip().notes)
      if (n.pitch == PitchId::fromHarmonic(id.value, 0) && n.start == 3 * kQ && n.length == kQ) ++added;
    CHECK(added == 1);
    CHECK(doc::validateClipJi(r.clip()).ok());
  }
}

TEST_CASE("roll JI edit: Is Harmonic… locks the virtual fundamental at note / N; drawing snaps to its harmonics; Unlock") {
  Ji r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Ji::plain(kQ, 2 * kQ, PitchId::fromDegree(4, 0))});  // E, 400 cents
  size_t h = r.history();
  CHECK_FALSE(r.clip().fundamental.lock.has_value());

  r.choose(r.rightClick(r.view->notePoint(ids[0], 0.5)), "Is Harmonic");
  REQUIRE(r.field() != nullptr);
  REQUIRE(r.d.type("x"));
  (void)r.d.press("Enter");
  r.d.step();
  CHECK_FALSE(r.clip().fundamental.lock.has_value());
  REQUIRE(r.shell->overlay().openCount() == 1);  // the field stays with its error
  (void)r.d.press("Backspace");
  r.typeEnter("5");
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.history() == h + 1);
  REQUIRE(r.clip().fundamental.lock.has_value());
  CHECK(*r.clip().fundamental.lock == Rational(1, 4));  // the just third 5/4 nearest E, / 5
  CHECK(r.view->virtualFundamental() == Rational(1, 4));
  CHECK(r.view->showHarmonics());
  CHECK(r.note(ids[0]).pitch == PitchId::fromDegree(4, 0));  // the note itself is not changed

  // Drawing snaps to the harmonics: a click near G gives 3/2 (harmonic 6); Shift draws on the row.
  REQUIRE(r.notesFocused());
  CHECK(r.d.press("B") == KeyResult::Action);
  r.d.click(r.at(4.5 * static_cast<double>(kQ), 7));
  r.d.step();
  r.d.click(r.at(6.5 * static_cast<double>(kQ), 7), mod::kShift);
  r.d.step();
  (void)r.d.press("B");
  CHECK(r.history() == h + 3);
  int just = 0, tempered = 0;
  for (const doc::Note& n : r.clip().notes) {
    if (n.pitch == PitchId::fromRatio(Rational(3, 2)) && n.start == 4 * kQ) ++just;
    if (n.pitch == PitchId::fromDegree(7, 0) && n.start == 6 * kQ) ++tempered;
  }
  CHECK(just == 1);
  CHECK(tempered == 1);

  // Is Subharmonic…: the fundamental N times above the note.
  r.choose(r.rightClick(r.view->notePoint(ids[0], 0.5)), "Is Subharmonic");
  r.typeEnter("3");
  CHECK(r.history() == h + 4);
  CHECK(*r.clip().fundamental.lock == Rational(15, 4));

  // Unlock, from the same menu.
  r.choose(r.rightClick(r.view->notePoint(ids[0], 0.5)), "Unlock Fundamental");
  CHECK(r.history() == h + 5);
  CHECK_FALSE(r.clip().fundamental.lock.has_value());
  REQUIRE(r.ctx.undo());
  CHECK(*r.clip().fundamental.lock == Rational(15, 4));
}

TEST_CASE("roll JI edit: Ctrl+C / Ctrl+V / Ctrl+X of a selected object take its inner notes along") {
  Ji r;
  r.focusNotes();
  const doc::JiObjectId id = r.makeObject(kQ, 2 * kQ, 0);
  doc::Note inner = Ji::plain(kQ, kQ, PitchId::fromHarmonic(id.value, 2));
  const doc::NoteId innerId = r.addNotes({inner})[0];
  const doc::JiObject before = r.object(id);
  size_t h = r.history();

  r.d.click(r.header(id));
  REQUIRE(r.view->jiSelection() == id);
  CHECK(r.d.press("Ctrl+C") == KeyResult::Action);
  CHECK(r.history() == h);
  CHECK(r.d.press("Ctrl+V") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.objects().size() == 2);
  const doc::JiObjectId copyId = r.view->jiSelection();
  REQUIRE(copyId.valid());
  CHECK(copyId != id);
  CHECK(r.object(copyId).start == 3 * kQ);  // right after the copied object
  CHECK(r.object(copyId).stops == before.stops);
  REQUIRE(r.clip().notes.size() == 2);
  int onCopy = 0;
  for (const doc::Note& n : r.clip().notes)
    if (n.pitch == PitchId::fromHarmonic(copyId.value, 2) && n.start == 3 * kQ && n.length == kQ) ++onCopy;
  CHECK(onCopy == 1);
  CHECK(doc::validateClipJi(r.clip()).ok());

  // Cut: the copy goes with its inner note (not left behind as a plain note).
  CHECK(r.d.press("Ctrl+X") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 2);
  REQUIRE(r.objects().size() == 1);
  CHECK(r.clip().notes.size() == 1);
  CHECK(r.note(innerId).pitch == PitchId::fromHarmonic(id.value, 2));
  // And pastes back.
  CHECK(r.d.press("Ctrl+V") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 3);
  CHECK(r.objects().size() == 2);
  CHECK(r.clip().notes.size() == 2);
  CHECK(doc::validateClipJi(r.clip()).ok());
  for (int i = 0; i < 3; ++i) REQUIRE(r.ctx.undo());
  CHECK(r.objects().size() == 1);
  CHECK(r.object(id) == before);
}

TEST_CASE("render: JI pianoroll-ji-entered (an entered object: its voices as rows, inner notes, a selected stop) matches the golden image") { checkGolden("pianoroll-ji-entered"); }
TEST_CASE("render: JI pianoroll-tune-to-ji (12-EDO chords tuned to JI, one back to the tuning) matches the golden image") { checkGolden("pianoroll-tune-to-ji"); }
TEST_CASE("render: JI pianoroll-tune-to-ji-31edo (a 31-EDO chord tuned to JI) matches the golden image") { checkGolden("pianoroll-tune-to-ji-31edo"); }
TEST_CASE("render: JI pianoroll-fundamental-lock (a locked fundamental, its guides, notes on the harmonics) matches the golden image") { checkGolden("pianoroll-fundamental-lock"); }
