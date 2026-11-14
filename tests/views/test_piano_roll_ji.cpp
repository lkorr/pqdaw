// T7.4 piano roll: JI objects. Every gesture goes through the real shell, pointer and key dispatch (TestDriver →
// UiTree → the note area / the keymap), never by calling the handlers:
//   J (the JI tool) → drag a box → the proportion field → Enter creates the object (invalid text shows an error and
//   creates nothing; "1/4:1/5:1/6" is an undertone object; Esc drops the box); a drag from the header moves it in
//   time and by rows with its inner notes; the left / right border resizes; Delete; Ctrl+D; each one undo step.
//   Copy / paste carries pitch curves and turns a JI object's inner note into the pitch it had.
// Goldens: pianoroll-ji-morph, pianoroll-ji-undertone, pianoroll-ji-tuning, pianoroll-curve-notes.
// Perf (label "perf"): a 200-object clip paints within 2 ms.

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
using pq::tuning::Rational;

namespace {

constexpr Ticks kQ = kPpq;

// The shell over the empty project with one new MIDI track and an empty four-bar clip open in the clip view.
struct JiRoll {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ClipView* view = nullptr;
  doc::ClipRef ref{};

  JiRoll() {
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
  ~JiRoll() { d.tree().setRoot(std::make_unique<Widget>()); }

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
  size_t history() const { return ctx.store().history().cursor(); }
  Vec2 at(double tick, int64_t degree) const { return {view->xOfTick(tick), view->yOfDegree(degree)}; }
  tuning::Tuning tuning() const {
    const std::optional<doc::TrackPath> path = doc::findTrack(*ctx.doc(), ref.track);
    REQUIRE(path.has_value());
    auto t = doc::buildTuning(*doc::effectiveTuningRef(*ctx.doc(), *path, &clip()).def);
    REQUIRE(t.ok());
    return *t;
  }
  // A click in the note area gives it the focus (the pianoroll key scope).
  void focusNotes() {
    const IRect a = d.rect("main/body/bottom/detail/clip/clip-view/notes");
    REQUIRE_FALSE(a.empty());
    d.click({static_cast<float>(a.right()) - 6.0f, static_cast<float>(a.y) + 6.0f});
    d.step();
    REQUIRE(d.tree().focus() == d.find("main/body/bottom/detail/clip/clip-view/notes"));
  }
  // An object made with the document op (the gestures under test start from it): 4:5:6:7 over [start, start + len)
  // on the row `degree`, its lowest voice pinned there as the tool does.
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
  // An inner note of the object (Harmonic identity) through the document.
  doc::NoteId addInner(doc::JiObjectId id, uint32_t voice, Ticks start, Ticks len) {
    doc::Note n;
    n.start = start;
    n.length = len;
    n.pitch = PitchId::fromHarmonic(id.value, voice);
    auto e = doc::ops::addNotes(*ctx.doc(), ctx.store().ids(), ref, std::span<const doc::Note>(&n, 1));
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    d.step();
    return e->notes.at(0).id;
  }
  Vec2 header(doc::JiObjectId id) {
    const IRect r = view->jiObjectRect(id);
    REQUIRE_FALSE(r.empty());
    return {static_cast<float>(r.x) + 24.0f, static_cast<float>(r.y) - 8.0f};
  }
  TextEdit* field() { return dynamic_cast<TextEdit*>(d.tree().focus()); }
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

TEST_CASE("roll JI: parseProportion — overtone, undertone, and what it refuses") {
  auto p = ClipView::parseProportion("8:9:10:12:15");
  REQUIRE(p.ok());
  CHECK(p->terms == std::vector<int64_t>{8, 9, 10, 12, 15});
  CHECK_FALSE(p->undertone);
  p = ClipView::parseProportion(" 1/4 : 1/5:1/6 ");
  REQUIRE(p.ok());
  CHECK(p->terms == std::vector<int64_t>{4, 5, 6});
  CHECK(p->undertone);
  p = ClipView::parseProportion("1/6:1/5:1/4");  // the same chord written downwards
  REQUIRE(p.ok());
  CHECK(p->terms == std::vector<int64_t>{4, 5, 6});
  for (const char* bad : {"", "  ", "4:5:", "4::5", "4:x", "5:4", "4:4:5", "4:1/5", "0:1", "5/4:3/2", "4;5", "-4:5", "1/4:1/4"})
    CHECK_MESSAGE(!ClipView::parseProportion(bad).ok(), bad);
}

TEST_CASE("roll JI: J, drag a box, type a proportion, Enter — one object, one undo step; invalid text creates nothing") {
  JiRoll r;
  r.focusNotes();
  CHECK_FALSE(r.view->jiTool());
  CHECK(r.d.press("J") == KeyResult::Action);
  CHECK(r.view->jiTool());
  const size_t h = r.history();

  // A box from beat 1 to beat 5 (the 1/4 cells under the press and the release), rows D3 (2) … D4 (14).
  r.d.drag(r.at(1.2 * static_cast<double>(kQ), 14), r.at(4.7 * static_cast<double>(kQ), 2), 6);
  r.d.step();
  TextEdit* f = r.field();
  REQUIRE_MESSAGE(f != nullptr, "the proportion field has the focus after the drag");
  CHECK(r.shell->overlay().openCount() == 1);
  CHECK(r.objects().empty());
  CHECK(r.history() == h);

  // Not a proportion: an error in the field's popover, nothing created, the field stays.
  REQUIRE(r.d.type("4:x"));
  (void)r.d.press("Enter");
  r.d.step();
  CHECK(r.objects().empty());
  CHECK(r.history() == h);
  REQUIRE(r.shell->overlay().openCount() == 1);
  auto* err = dynamic_cast<Label*>(r.shell->overlay().top()->find("error"));
  REQUIRE(err != nullptr);
  CHECK_FALSE(err->text().empty());
  REQUIRE(r.field() == f);

  for (int i = 0; i < 3; ++i) (void)r.d.press("Backspace");
  REQUIRE(r.d.type("4:5:6:7"));
  (void)r.d.press("Enter");
  r.d.step();
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.history() == h + 1);
  REQUIRE(r.objects().size() == 1);
  const doc::JiObject o = r.objects()[0];
  CHECK(o.start == kQ);
  CHECK(o.length == 4 * kQ);
  CHECK(o.fundamental == PitchId::fromDegree(2, 0));  // the row at the box's bottom edge, a Degree identity
  CHECK_FALSE(o.undertone);
  REQUIRE(o.stops.size() == 1);
  CHECK(o.stops[0].terms == std::vector<int64_t>{4, 5, 6, 7});
  CHECK(o.stops[0].scale == Rational(1, 4));  // the lowest voice IS the bottom row
  CHECK(o.voices() == 4);
  const tuning::Tuning t = r.tuning();
  CHECK(*doc::jiVoiceCents(o, t, 0, 0.0) == doctest::Approx(200.0));
  CHECK(*doc::jiVoiceCents(o, t, 2, 0.0) == doctest::Approx(200.0 + tuning::cents(Rational(3, 2))));
  CHECK(r.view->jiSelection() == o.id);  // the new object is selected
  CHECK(r.view->jiTool());               // the tool stays on, as Draw Mode does

  // Undo removes it in one step; redo brings it back.
  REQUIRE(r.ctx.undo());
  CHECK(r.objects().empty());
  CHECK(r.history() == h);
  CHECK_FALSE(r.view->jiSelection().valid());
  REQUIRE(r.ctx.redo());
  CHECK(r.objects().size() == 1);

  // J again leaves the tool; B (Draw Mode) and the JI tool exclude each other.
  CHECK(r.d.press("J") == KeyResult::Action);
  CHECK_FALSE(r.view->jiTool());
  (void)r.d.press("J");
  (void)r.d.press("B");
  CHECK(r.ctx.drawMode());
  CHECK_FALSE(r.view->jiTool());
  (void)r.d.press("J");
  CHECK(r.view->jiTool());
  CHECK_FALSE(r.ctx.drawMode());
}

TEST_CASE("roll JI: 1/4:1/5:1/6 makes an undertone object in the box; Esc drops a box") {
  JiRoll r;
  r.focusNotes();
  (void)r.d.press("J");
  const size_t h = r.history();
  r.d.drag(r.at(0.3 * static_cast<double>(kQ), 0), r.at(1.6 * static_cast<double>(kQ), 9), 5);  // dragged upwards
  r.d.step();
  REQUIRE(r.field() != nullptr);
  (void)r.d.press("Escape");
  r.d.step();
  CHECK(r.shell->overlay().openCount() == 0);
  CHECK(r.objects().empty());
  CHECK(r.history() == h);

  r.d.drag(r.at(0.3 * static_cast<double>(kQ), 0), r.at(1.6 * static_cast<double>(kQ), 9), 5);
  r.d.step();
  REQUIRE(r.field() != nullptr);
  REQUIRE(r.d.type("1/4:1/5:1/6"));
  (void)r.d.press("Enter");
  r.d.step();
  REQUIRE(r.objects().size() == 1);
  CHECK(r.history() == h + 1);
  const doc::JiObject o = r.objects()[0];
  CHECK(o.undertone);
  CHECK(o.start == 0);
  CHECK(o.length == 2 * kQ);
  CHECK(o.fundamental == PitchId::fromDegree(0, 0));
  CHECK(o.stops[0].terms == std::vector<int64_t>{4, 5, 6});
  CHECK(o.stops[0].scale == Rational(6));
  // 1/6 (the lowest pitch) on the bottom row, 1/4 a fifth above it.
  const tuning::Tuning t = r.tuning();
  double lo = 1e9, hi = -1e9;
  for (uint32_t v = 0; v < o.voices(); ++v) {
    const double c = *doc::jiVoiceCents(o, t, v, 0.0);
    lo = std::min(lo, c);
    hi = std::max(hi, c);
  }
  CHECK(lo == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(hi == doctest::Approx(tuning::cents(Rational(3, 2))));
  REQUIRE(r.ctx.undo());
  CHECK(r.objects().empty());
}

TEST_CASE("roll JI: a click on the header selects; a drag moves the object in time and by rows with its inner notes") {
  JiRoll r;
  r.focusNotes();
  const doc::JiObjectId id = r.makeObject(2 * kQ, 4 * kQ, 0);
  const doc::NoteId inner = r.addInner(id, 1, 3 * kQ, kQ);
  const doc::JiObject before = r.object(id);
  size_t h = r.history();

  // A click selects without editing; Esc deselects.
  r.d.click(r.header(id));
  CHECK(r.view->jiSelection() == id);
  CHECK(r.view->selection().empty());
  CHECK(r.history() == h);
  CHECK(r.d.press("Escape") == KeyResult::Action);
  CHECK_FALSE(r.view->jiSelection().valid());

  // Drag the header two beats later and three rows up.
  const Vec2 from = r.header(id);
  const Vec2 to{from.x + 2.0f * static_cast<float>(kQ * r.view->pxPerTick()), from.y - 3.0f * static_cast<float>(r.view->rowHeight())};
  r.d.drag(from, to, 6);
  r.d.step();
  CHECK(r.history() == h + 1);
  const doc::JiObject moved = r.object(id);
  CHECK(moved.start == 4 * kQ);
  CHECK(moved.length == before.length);
  CHECK(moved.fundamental == PitchId::fromDegree(3, 0));
  CHECK(moved.stops == before.stops);
  CHECK(moved.voiceNotes == before.voiceNotes);
  REQUIRE(r.clip().notes.findById(inner) != nullptr);
  CHECK(r.clip().notes.findById(inner)->start == 5 * kQ);  // the inner note moved with it
  CHECK(r.clip().notes.findById(inner)->pitch == PitchId::fromHarmonic(id.value, 1));
  CHECK(r.view->jiSelection() == id);

  // Time only (the pointer stays on its row): the fundamental is untouched.
  h = r.history();
  const Vec2 f2 = r.header(id);
  r.d.drag(f2, {f2.x - static_cast<float>(kQ * r.view->pxPerTick()), f2.y}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.object(id).start == 3 * kQ);
  CHECK(r.object(id).fundamental == PitchId::fromDegree(3, 0));

  // A press and release without moving is no edit.
  h = r.history();
  r.d.click(r.header(id));
  CHECK(r.history() == h);

  REQUIRE(r.ctx.undo());
  CHECK(r.object(id).start == 4 * kQ);
  REQUIRE(r.ctx.undo());
  CHECK(r.object(id) == before);
  CHECK(r.clip().notes.findById(inner)->start == 3 * kQ);
}

TEST_CASE("roll JI: the right border resizes the end, the left border the start — one undo step each") {
  JiRoll r;
  r.focusNotes();
  const doc::JiObjectId id = r.makeObject(2 * kQ, 4 * kQ, 0);
  const doc::JiObject before = r.object(id);
  const float beat = static_cast<float>(kQ * r.view->pxPerTick());

  size_t h = r.history();
  IRect box = r.view->jiObjectRect(id);
  Vec2 from{static_cast<float>(box.right()) - 1.0f, static_cast<float>(box.y) + 0.5f * static_cast<float>(box.h)};
  r.d.drag(from, {from.x + 2.0f * beat, from.y}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.object(id).start == 2 * kQ);
  CHECK(r.object(id).length == 6 * kQ);

  h = r.history();
  box = r.view->jiObjectRect(id);
  from = {static_cast<float>(box.x) + 1.0f, static_cast<float>(box.y) + 0.5f * static_cast<float>(box.h)};
  r.d.drag(from, {from.x + beat, from.y}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.object(id).start == 3 * kQ);
  CHECK(r.object(id).length == 5 * kQ);
  CHECK(r.view->jiSelection() == id);

  // Esc during a drag cancels it: nothing is dispatched.
  h = r.history();
  box = r.view->jiObjectRect(id);
  from = {static_cast<float>(box.right()) - 1.0f, static_cast<float>(box.y) + 0.5f * static_cast<float>(box.h)};
  r.d.move(from);
  r.d.down(from);
  r.d.move({from.x + beat, from.y});
  (void)r.d.press("Escape");
  r.d.up({from.x + beat, from.y});
  CHECK(r.history() == h);
  CHECK(r.object(id).length == 5 * kQ);

  REQUIRE(r.ctx.undo());
  CHECK(r.object(id).start == 2 * kQ);
  CHECK(r.object(id).length == 6 * kQ);
  REQUIRE(r.ctx.undo());
  CHECK(r.object(id) == before);
}

TEST_CASE("roll JI: Delete removes the selected object (inner notes stay as plain notes); Ctrl+D duplicates it") {
  JiRoll r;
  r.focusNotes();
  const doc::JiObjectId id = r.makeObject(kQ, 2 * kQ, 0);
  const doc::NoteId inner = r.addInner(id, 2, kQ, kQ);
  const doc::JiObject before = r.object(id);

  // Ctrl+D: a copy right after the object with its own ids, its inner note copied onto the copy; the copy is selected.
  size_t h = r.history();
  r.d.click(r.header(id));
  REQUIRE(r.view->jiSelection() == id);
  CHECK(r.d.press("Ctrl+D") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  REQUIRE(r.objects().size() == 2);
  const doc::JiObjectId copyId = r.view->jiSelection();
  REQUIRE(copyId.valid());
  CHECK(copyId != id);
  const doc::JiObject copy = r.object(copyId);
  CHECK(copy.start == 3 * kQ);
  CHECK(copy.length == before.length);
  CHECK(copy.fundamental == before.fundamental);
  CHECK(copy.stops == before.stops);
  for (doc::NoteId v : copy.voiceNotes) CHECK(std::find(before.voiceNotes.begin(), before.voiceNotes.end(), v) == before.voiceNotes.end());
  REQUIRE(r.clip().notes.size() == 2);
  int onCopy = 0;
  for (const doc::Note& n : r.clip().notes)
    if (n.pitch == PitchId::fromHarmonic(copyId.value, 2) && n.start == 3 * kQ && n.length == kQ) ++onCopy;
  CHECK(onCopy == 1);
  CHECK(doc::validateClipJi(r.clip()).ok());
  REQUIRE(r.ctx.undo());
  CHECK(r.objects().size() == 1);
  CHECK(r.clip().notes.size() == 1);
  CHECK(r.object(id) == before);

  // Delete: the object goes, its inner note stays at the pitch it had (no longer Harmonic).
  h = r.history();
  r.d.click(r.header(id));
  REQUIRE(r.view->jiSelection() == id);
  CHECK(r.d.press("Delete") == KeyResult::Action);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.objects().empty());
  CHECK_FALSE(r.view->jiSelection().valid());
  REQUIRE(r.clip().notes.findById(inner) != nullptr);
  CHECK(r.clip().notes.findById(inner)->pitch.kind() != tuning::PitchKind::Harmonic);
  REQUIRE(r.ctx.undo());
  REQUIRE(r.objects().size() == 1);
  CHECK(r.object(id) == before);
  CHECK(r.clip().notes.findById(inner)->pitch == PitchId::fromHarmonic(id.value, 2));
}

TEST_CASE("roll JI: copy / paste carries a note's pitch curve; a JI object's inner note pastes as the pitch it had") {
  JiRoll r;
  r.focusNotes();
  // A Degree note with a bend.
  doc::Note n;
  n.start = 0;
  n.length = 2 * kQ;
  n.pitch = PitchId::fromDegree(5, 0);
  auto e = doc::ops::addNotes(*r.ctx.doc(), r.ctx.store().ids(), r.ref, std::span<const doc::Note>(&n, 1));
  REQUIRE(e.ok());
  REQUIRE(r.ctx.store().dispatch(e->command).ok());
  const doc::NoteKey key = e->notes.at(0);
  const std::vector<doc::Breakpoint> bend = {{0, 0.0f, 0.0f, {}}, {kQ, 150.0f, 0.0f, {}}, {2 * kQ, 150.0f, 0.0f, {}}};
  auto c = doc::ops::setNoteCurve(*r.ctx.doc(), r.ref, key, bend);
  REQUIRE(c.ok());
  REQUIRE(r.ctx.store().dispatch(c->command).ok());
  r.d.step();

  r.view->select(std::span<const doc::NoteId>(&key.id, 1));
  CHECK(r.d.press("Ctrl+C") == KeyResult::Action);
  size_t h = r.history();
  CHECK(r.d.press("Ctrl+V") == KeyResult::Action);
  CHECK(r.history() == h + 1);
  REQUIRE(r.clip().notes.size() == 2);
  REQUIRE(r.view->selection().size() == 1);
  const doc::NoteId pasted{r.view->selection()[0]};
  CHECK(pasted != key.id);
  CHECK(r.clip().notes.findById(pasted)->start == 2 * kQ);  // right after the copied note
  const doc::NoteCurves* pc = doc::findNoteCurves(r.clip(), pasted);
  REQUIRE(pc != nullptr);
  CHECK(pc->pitch == bend);
  REQUIRE(r.ctx.undo());
  CHECK(r.clip().notes.size() == 1);

  // An inner note of an object: the paste is a Cents note at the voice's pitch at the note's start.
  const doc::JiObjectId id = r.makeObject(4 * kQ, 2 * kQ, 0);
  const doc::NoteId inner = r.addInner(id, 3, 4 * kQ, kQ);
  r.view->select(std::span<const doc::NoteId>(&inner, 1));
  (void)r.d.press("Ctrl+C");
  h = r.history();
  (void)r.d.press("Ctrl+V");
  CHECK(r.history() == h + 1);
  REQUIRE(r.view->selection().size() == 1);
  const doc::Note* p = r.clip().notes.findById(doc::NoteId{r.view->selection()[0]});
  REQUIRE(p != nullptr);
  REQUIRE(p->pitch.kind() == tuning::PitchKind::Cents);
  CHECK(p->pitch.cents() == doctest::Approx(tuning::cents(Rational(7, 4))));
  CHECK(r.objects().size() == 1);
}

TEST_CASE("render: JI pianoroll-ji-morph (a 4-stop morph riding a curve) matches the golden image") { checkGolden("pianoroll-ji-morph"); }
TEST_CASE("render: JI pianoroll-ji-undertone (an undertone object, a selected one) matches the golden image") { checkGolden("pianoroll-ji-undertone"); }
TEST_CASE("render: JI pianoroll-ji-tuning (a tuning stop gliding to a proportion, inner notes) matches the golden image") { checkGolden("pianoroll-ji-tuning"); }
TEST_CASE("render: JI pianoroll-curve-notes (notes along their pitch curves) matches the golden image") { checkGolden("pianoroll-curve-notes"); }

TEST_CASE("roll JI perf: a 200-object clip paints within 2 ms" * doctest::test_suite("perf")) {
  JiRoll r;
  doc::DocumentStore& st = r.ctx.store();
  // 200 objects, 50 per pitch band: 4:5:6:7:8 morphing to 5:6:7:8:9 half way, every fifth one riding a curve.
  const int64_t a[] = {4, 5, 6, 7, 8}, b[] = {5, 6, 7, 8, 9};
  for (int i = 0; i < 200; ++i) {
    const Ticks start = (i % 50) * 2 * kQ;
    auto e = doc::ops::createJiObject(*r.ctx.doc(), st.ids(), r.ref, start, 2 * kQ - kQ / 8, PitchId::fromDegree((i / 50) * 7 - 12, 0), a);
    REQUIRE(e.ok());
    REQUIRE(st.dispatch(e->command).ok());
    const doc::JiObjectId id = e->jiObjects.at(0);
    auto s = doc::ops::addJiStop(*r.ctx.doc(), st.ids(), r.ref, id, kQ, b, kQ / 2);
    REQUIRE(s.ok());
    REQUIRE(st.dispatch(s->command).ok());
    if (i % 5 == 0) {
      doc::JiObject o = r.object(id);
      o.ride = {{0, 0.0f, 0.0f, {}}, {kQ, 30.0f, 0.0f, {}}, {2 * kQ - kQ / 8, -10.0f, 0.0f, {}}};
      auto ride = doc::ops::setJiObject(*r.ctx.doc(), st.ids(), r.ref, o);
      REQUIRE(ride.ok());
      REQUIRE(st.dispatch(ride->command).ok());
    }
  }
  REQUIRE(r.objects().size() == 200);
  REQUIRE(r.d.enableGpu().ok());
  Widget* notes = r.d.find("main/body/bottom/detail/clip/clip-view/notes");
  REQUIRE(notes != nullptr);
  r.view->setRowHeight(5);
  r.view->centreOnDegree(32);
  auto measure = [&](const char* what, double pxPerBeat, uint32_t atLeast) {
    r.view->setZoom(pxPerBeat / static_cast<double>(kQ), 0.0);
    r.view->scrollToTick(0.0);
    r.d.tree().layoutNow();
    for (int i = 0; i < 20 && r.d.tree().needsFrame(); ++i) r.d.step();
    std::vector<double> ms, ji;
    for (int i = 0; i < 60; ++i) {
      notes->markDirty();
      r.d.step();
      ms.push_back(r.view->paintStats().notesMs);
      ji.push_back(r.view->paintStats().jiMs);
    }
    std::sort(ms.begin(), ms.end());
    std::sort(ji.begin(), ji.end());
    MESSAGE(what << ": note area paint median " << ms[ms.size() / 2] << " ms (max " << ms.back() << "), of which JI objects "
                 << ji[ji.size() / 2] << " ms; objects drawn " << r.view->paintStats().jiPainted);
    CHECK(r.view->paintStats().jiPainted >= atLeast);
    CHECK(ms[ms.size() / 2] <= 2.0);
  };
  measure("200 objects in view", static_cast<double>(notes->rect().w) / 100.0, 200);  // the whole clip: 100 beats
  measure("zoomed in (60 px per beat)", 60.0, 20);
  MESSAGE("note area " << notes->rect().w << " x " << notes->rect().h);
}
