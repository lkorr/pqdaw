// KB.help: the piano roll's help window (the `?` button beside Fold / Scale, at the right end of the clip box's title
// bar). KB.help2: a floating window the roll keeps working under. Through the real shell, pointer and key dispatch
// (TestDriver -> UiTree -> the corner's button / the overlay layer / the window / the keymap):
//   `?` opens the window over the roll (the button lit, the roll keeps the focus), `?` again and × close it, a click
//   outside does not; dragged by its title it moves (kept on screen, also when the main window shrinks); dragged by an
//   edge or corner it resizes (at least its minimum; narrow, it drops the section list and goes to one column); while
//   it is open the roll's clicks and keys (Up, Ctrl+Shift+J, a double-click) work and Esc in the roll leaves it open;
//   a click in it gives it the keys (the arrows scroll, Esc closes and the roll has its keys back); its place, size and
//   section survive closing and reopening (ViewContext::rollHelpView);
//   the wheel and the section list scroll it; every entry bound to an action shows the key that action has in the
//   LOADED keymap (a rebound action shows its new key, an unbound one "unbound"); every `{action}` of the table is a
//   registered action (an entry whose feature was removed fails here).
// Goldens: pianoroll-help, pianoroll-help-scrolled, pianoroll-help-moved.

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/keymap_file.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../../src/views/src/piano_roll/roll_help.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using pq::tuning::PitchId;
using pq::views::roll::RollHelpWindow;

namespace {

constexpr Ticks kQ = kPpq;
const char* const kView = "main/body/bottom/detail/clip/clip-view";
const std::string kHelp = std::string(kView) + "/help";
const std::string kNotes = std::string(kView) + "/notes";

bool holds(const Widget& w, const Widget* f) { return f != nullptr && (f == &w || w.isAncestorOf(*f)); }
Vec2 centreOf(const IRect& r) { return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)}; }

// The shell over the empty project with one MIDI track and a four-bar 12-EDO clip open in the clip view.
struct Help {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ClipView* view = nullptr;
  doc::ClipRef ref{};

  Help() {
    REQUIRE(ctx.openProject(views::test::fixtureDir("empty")).ok());
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
    ctx.setDetailClip(ref);
    d.tree().layoutNow();
    view = dynamic_cast<ClipView*>(d.find(kView));
    REQUIRE(view != nullptr);
    REQUIRE(view->hasClip());
    view->setRowHeight(14);
    view->centreOnDegree(6);
    view->setZoom(60.0 / static_cast<double>(kQ), 0.0);
    view->scrollToTick(-0.5 * static_cast<double>(kQ));
    d.tree().layoutNow();
  }
  ~Help() { d.tree().setRoot(std::make_unique<Widget>()); }

  RollHelpWindow* panel() { return roll::openRollHelpWindow(*view); }
  Toggle& button() {
    auto* b = dynamic_cast<Toggle*>(d.find(kHelp));
    REQUIRE(b != nullptr);
    return *b;
  }
  void clickHelp() {
    d.click(d.center(kHelp));
    d.step();
  }
  // A point on the window's title bar (left of ×).
  Vec2 titlePoint() {
    RollHelpWindow* p = panel();
    REQUIRE(p != nullptr);
    return {static_cast<float>(p->rect().x) + 80.0f, static_cast<float>(p->rect().y) + 0.5f * FloatingWindow::kTitleH};
  }
  const doc::NoteClip& clip() const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), ref.track);
    REQUIRE(t != nullptr);
    const int32_t i = doc::findClipIndex(*t, ref.clip);
    REQUIRE(i >= 0);
    return *t->clips[static_cast<size_t>(i)].clip;
  }
  const doc::Note& onlyNote() const {
    const doc::Note* found = nullptr;
    for (const doc::Note& n : clip().notes) found = &n;
    REQUIRE(found != nullptr);
    return *found;
  }
  // Notes on `degrees` at the clip's start, two beats long, all selected; the note area focused (a click on an empty
  // spot of it, at its top right).
  std::vector<doc::NoteId> notesSelected(std::vector<int> degrees) {
    const IRect a = d.rect(kNotes);
    REQUIRE_FALSE(a.empty());
    d.click({static_cast<float>(a.right()) - 6.0f, static_cast<float>(a.y) + 6.0f});
    d.step();
    REQUIRE(d.tree().focus() == d.find(kNotes));
    std::vector<doc::Note> add;
    for (int deg : degrees) {
      doc::Note n;
      n.start = 0;
      n.length = 2 * kQ;
      n.pitch = PitchId::fromDegree(deg, 0);
      n.velocity = 100.0f;
      add.push_back(n);
    }
    auto e = doc::ops::addNotes(*ctx.doc(), ctx.store().ids(), ref, add);
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    d.step();
    std::vector<doc::NoteId> ids;
    for (const doc::Note& n : clip().notes) ids.push_back(n.id);
    view->select(std::span<const doc::NoteId>(ids.data(), ids.size()));
    d.step();
    return ids;
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

// The `{action.id}` names of a gesture's markup.
std::vector<std::string> actionsOf(std::string_view markup) {
  std::vector<std::string> out;
  size_t i = 0;
  while ((i = markup.find('{', i)) != std::string_view::npos) {
    const size_t end = markup.find('}', i);
    REQUIRE(end != std::string_view::npos);
    out.emplace_back(markup.substr(i + 1, end - i - 1));
    i = end + 1;
  }
  return out;
}

}  // namespace

TEST_CASE("roll help: ? opens a floating window over the roll; ? again and its x close it; a click outside does not") {
  Help h;
  Toggle& btn = h.button();
  // Beside Fold / Scale: in the same header row, just left of Fold (the right end of the clip box's title bar).
  const IRect fold = h.d.rect(std::string(kView) + "/corner/fold");
  const IRect box = h.d.rect(std::string(kView) + "/clip-box");
  CHECK(btn.rect().right() <= fold.x);
  CHECK(fold.x - btn.rect().right() <= 12);
  CHECK(btn.rect().right() <= box.right());
  CHECK(std::abs(btn.rect().y - fold.y) <= 4);
  CHECK(h.d.widgetAt(h.d.center(kHelp)) == &btn);
  CHECK_FALSE(btn.on());
  CHECK(h.panel() == nullptr);
  CHECK_FALSE(h.view->helpOpen());
  CHECK_FALSE(h.ctx.rollHelpView().open);

  h.clickHelp();
  RollHelpWindow* p = h.panel();
  REQUIRE(p != nullptr);
  CHECK(btn.on());
  CHECK(h.view->helpOpen());
  CHECK(h.ctx.rollHelpView().open);
  CHECK(h.d.tree().focus() == h.d.find(kNotes));  // the roll keeps the keys
  // The first time over the roll: inside the panels (below the control bar), its bottom on the roll's, right of the
  // keys so the button stays reachable; readable: at least 600 px wide at 1280 x 800.
  const IRect r = p->rect(), body = h.d.rect("main/body"), notes = h.d.rect(kNotes);
  CHECK(r.x >= notes.x);
  CHECK(r.y >= body.y);
  CHECK(r.bottom() <= body.bottom());
  CHECK(r.bottom() >= notes.y + notes.h / 2);
  CHECK(r.w >= 600);
  CHECK_FALSE(r.contains(h.d.center(kHelp)));
  CHECK(p->sectionCount() >= 6);
  CHECK(p->maxScrollY() > 0);  // more text than fits: it scrolls
  CHECK(p->sectionListShown());
  CHECK_FALSE(p->singleColumn());

  // A click outside (on the arrangement) leaves it open, and reaches what is there.
  h.d.click({20.0f, static_cast<float>(body.y) + 20.0f});
  h.d.step();
  REQUIRE(h.panel() == p);
  CHECK(btn.on());
  CHECK_FALSE(holds(*p, h.d.tree().focus()));

  // A second click on the button closes it.
  h.clickHelp();
  CHECK(h.panel() == nullptr);
  CHECK_FALSE(btn.on());
  CHECK_FALSE(h.ctx.rollHelpView().open);

  // × in its title bar.
  h.clickHelp();
  p = h.panel();
  REQUIRE(p != nullptr);
  const IRect x = p->closeButton().rect();
  CHECK(x.y < p->rect().y + 26);
  CHECK(x.right() <= p->rect().right());
  CHECK(x.x > p->rect().x + p->rect().w / 2);
  h.d.click(centreOf(x));
  h.d.step();
  CHECK(h.panel() == nullptr);
  CHECK_FALSE(btn.on());
  CHECK(h.d.tree().focus() == h.d.find(kNotes));  // the click in the window took the focus; it goes back to the roll
}

TEST_CASE("roll help: dragged by its title it moves, kept on screen (also when the main window shrinks)") {
  Help h;
  h.clickHelp();
  RollHelpWindow* p = h.panel();
  REQUIRE(p != nullptr);
  const IRect r0 = p->rect();
  CHECK(p->cursorAt(h.titlePoint()) == Cursor::Move);
  h.d.drag(h.titlePoint(), {h.titlePoint().x - 300.0f, h.titlePoint().y - 40.0f}, 6);
  h.d.step();
  REQUIRE(h.panel() == p);
  const IRect r1 = p->rect();
  CHECK(r1.x == r0.x - 300);
  CHECK(r1.y == r0.y - 40);
  CHECK(r1.w == r0.w);
  CHECK(r1.h == r0.h);
  // The session's view state follows (logical px; the scale is 1 here).
  CHECK(h.ctx.rollHelpView().x == doctest::Approx(r1.x));
  CHECK(h.ctx.rollHelpView().y == doctest::Approx(r1.y));
  CHECK(h.ctx.rollHelpView().w == doctest::Approx(r1.w));

  // Far past the top left: stopped at the window's edges; past the bottom right likewise.
  h.d.drag(h.titlePoint(), {h.titlePoint().x - 2000.0f, h.titlePoint().y - 2000.0f}, 4);
  h.d.step();
  CHECK(p->rect().x == 0);
  CHECK(p->rect().y == 0);
  h.d.drag(h.titlePoint(), {h.titlePoint().x + 3000.0f, h.titlePoint().y + 3000.0f}, 4);
  h.d.step();
  CHECK(p->rect().right() == 1280);
  CHECK(p->rect().bottom() == 800);
  CHECK(p->rect().w == r0.w);

  // The main window shrinks: the window stays wholly on it (moved in, then capped at its size), and comes back to
  // its size when there is room again.
  h.d.resize(1000, 700);
  h.d.step();
  CHECK(p->rect().x >= 0);
  CHECK(p->rect().y >= 0);
  CHECK(p->rect().right() <= 1000);
  CHECK(p->rect().bottom() <= 700);
  CHECK(p->rect().w == r0.w);
  h.d.resize(600, 500);
  h.d.step();
  CHECK(p->rect().x == 0);
  CHECK(p->rect().right() <= 600);
  CHECK(p->rect().bottom() <= 500);
  h.d.resize(1280, 800);
  h.d.step();
  CHECK(p->rect().w == r0.w);
  CHECK(p->rect().h == r0.h);
  CHECK(p->rect().right() <= 1280);
}

TEST_CASE("roll help: dragged by an edge or corner it resizes, never below its minimum; the text reflows") {
  Help h;
  h.clickHelp();
  RollHelpWindow* p = h.panel();
  REQUIRE(p != nullptr);
  const IRect r0 = p->rect();
  const int32_t contentH = p->maxScrollY();
  // The bottom-right corner (the grip): both sizes, the top left stays.
  const Vec2 corner{static_cast<float>(r0.right()) - 3.0f, static_cast<float>(r0.bottom()) - 3.0f};
  CHECK(p->cursorAt(corner) == Cursor::ResizeNWSE);
  h.d.drag(corner, {corner.x - 200.0f, corner.y - 100.0f}, 5);
  h.d.step();
  const IRect r1 = p->rect();
  CHECK(r1.x == r0.x);
  CHECK(r1.y == r0.y);
  CHECK(r1.w == r0.w - 200);
  CHECK(r1.h == r0.h - 100);
  CHECK(h.ctx.rollHelpView().w == doctest::Approx(r1.w));
  CHECK(h.ctx.rollHelpView().h == doctest::Approx(r1.h));
  CHECK(p->maxScrollY() > contentH);  // narrower and shorter: more to scroll
  CHECK(p->sectionListShown());       // 634 px: still the list

  // The left edge: the right edge stays; narrower than 600 px the section list goes.
  const Vec2 left{static_cast<float>(r1.x) + 2.0f, static_cast<float>(r1.y) + 0.5f * static_cast<float>(r1.h)};
  CHECK(p->cursorAt(left) == Cursor::ResizeEW);
  h.d.drag(left, {left.x + 100.0f, left.y}, 4);
  h.d.step();
  const IRect r2 = p->rect();
  CHECK(r2.right() == r1.right());
  CHECK(r2.w == r1.w - 100);
  CHECK_FALSE(p->sectionListShown());

  // Far past the minimum: stops at it (360 x 220), and the text goes to one column.
  const Vec2 c2{static_cast<float>(r2.right()) - 3.0f, static_cast<float>(r2.bottom()) - 3.0f};
  h.d.drag(c2, {c2.x - 2000.0f, c2.y - 2000.0f}, 4);
  h.d.step();
  const IRect r3 = p->rect();
  CHECK(r3.w == 360);
  CHECK(r3.h == 220);
  CHECK(r3.x == r2.x);
  CHECK(r3.y == r2.y);
  CHECK(p->singleColumn());
  CHECK(p->maxScrollY() > 0);
  // The top edge, past the minimum likewise: the bottom stays.
  const Vec2 top{static_cast<float>(r3.x) + 0.5f * static_cast<float>(r3.w), static_cast<float>(r3.y) + 2.0f};
  CHECK(p->cursorAt(top) == Cursor::ResizeNS);
  h.d.drag(top, {top.x, top.y + 500.0f}, 4);
  h.d.step();
  CHECK(p->rect().h == 220);
  CHECK(p->rect().bottom() == r3.bottom());
  // Wide again: the list.
  const Vec2 c3{static_cast<float>(p->rect().right()) - 3.0f, static_cast<float>(p->rect().bottom()) - 3.0f};
  h.d.drag(c3, {c3.x + 500.0f, c3.y + 200.0f}, 4);
  h.d.step();
  CHECK(p->rect().right() == 1280);  // inside the main window
  CHECK(p->rect().w >= 600);
  CHECK(p->sectionListShown());
}

TEST_CASE("roll help: while it is open the roll's clicks and keys work (Up, Ctrl+Shift+J, a double-click), Esc in the roll leaves it open") {
  Help h;
  const std::vector<doc::NoteId> ids = h.notesSelected({0, 4, 7});
  h.clickHelp();
  RollHelpWindow* p = h.panel();
  REQUIRE(p != nullptr);
  // Moved off the notes by its title and made small by its corner (as the user would, to try what it describes).
  h.d.drag(h.titlePoint(), {h.titlePoint().x - 2000.0f, h.titlePoint().y - 2000.0f}, 4);
  h.d.step();
  const Vec2 corner{static_cast<float>(p->rect().right()) - 3.0f, static_cast<float>(p->rect().bottom()) - 3.0f};
  h.d.drag(corner, {corner.x - 2000.0f, corner.y - 2000.0f}, 4);
  h.d.step();
  const IRect notes = h.d.rect(kNotes);
  REQUIRE(intersect(p->rect(), notes).empty());
  // The drags gave the window the focus; a click in the roll gives it back.
  h.d.click({static_cast<float>(notes.right()) - 6.0f, static_cast<float>(notes.y) + 6.0f});
  h.d.step();
  REQUIRE(h.d.tree().focus() == h.d.find(kNotes));
  h.view->select(std::span<const doc::NoteId>(ids.data(), ids.size()));
  h.d.step();

  // Up: the selected notes a step up.
  CHECK(h.d.press("Up") == KeyResult::Action);
  for (const doc::Note& n : h.clip().notes) CHECK((n.pitch == PitchId::fromDegree(1, 0) || n.pitch == PitchId::fromDegree(5, 0) || n.pitch == PitchId::fromDegree(8, 0)));
  CHECK(h.d.press("Down") == KeyResult::Action);
  // Ctrl+Shift+J: the chord tuned to JI; its chooser opens over everything, Esc closes the chooser, not the help.
  CHECK(h.clip().groups == nullptr);
  CHECK(h.d.press("Ctrl+Shift+J") == KeyResult::Action);
  h.d.step();
  CHECK(h.clip().groups != nullptr);
  auto* chooser = dynamic_cast<Menu*>(h.shell->overlay().top());
  CHECK(chooser != nullptr);
  CHECK(h.d.press("Escape") != KeyResult::Unhandled);
  h.d.step();
  CHECK(dynamic_cast<Menu*>(h.shell->overlay().top()) == nullptr);
  REQUIRE(h.panel() == p);
  CHECK(h.d.tree().focus() == h.d.find(kNotes));
  // Esc with the roll focused: the roll's, the help stays.
  (void)h.d.press("Escape");
  h.d.step();
  CHECK(h.panel() == p);
  // A double-click on an empty spot of the roll adds a note there.
  const size_t before = h.clip().notes.size();
  h.d.doubleClick({static_cast<float>(notes.x) + 0.6f * static_cast<float>(notes.w), static_cast<float>(notes.y) + 0.5f * static_cast<float>(notes.h)});
  h.d.step();
  CHECK(h.clip().notes.size() == before + 1);
  CHECK(h.panel() == p);
  CHECK(h.button().on());
}

TEST_CASE("roll help: a click in it gives it the keys (the arrows scroll, Esc closes); then the roll has its keys back") {
  Help h;
  h.notesSelected({0});
  h.clickHelp();
  RollHelpWindow* p = h.panel();
  REQUIRE(p != nullptr);
  // Closed or open, the roll's: Up moves the selected note a step, B toggles Draw Mode.
  CHECK(h.d.press("Up") == KeyResult::Action);
  CHECK(h.onlyNote().pitch == PitchId::fromDegree(1, 0));
  CHECK_FALSE(h.ctx.drawMode());
  CHECK(h.d.press("B") == KeyResult::Action);
  CHECK(h.ctx.drawMode());
  CHECK(h.d.press("B") == KeyResult::Action);
  CHECK_FALSE(h.ctx.drawMode());
  CHECK(p->scrollY() == 0);
  // A click on its title bar: its keys.
  h.d.click(h.titlePoint());
  h.d.step();
  CHECK(holds(*p, h.d.tree().focus()));
  CHECK(h.d.press("Down") == KeyResult::Widget);  // scrolls the help
  h.d.step();
  CHECK(p->scrollY() > 0);
  CHECK(h.onlyNote().pitch == PitchId::fromDegree(1, 0));  // the note did not move
  CHECK(h.d.press("End") == KeyResult::Widget);
  h.d.step();
  CHECK(p->scrollY() == p->maxScrollY());
  CHECK(h.d.press("Home") == KeyResult::Widget);
  h.d.step();
  CHECK(p->scrollY() == 0);
  CHECK(h.d.press("Escape") == KeyResult::Widget);
  h.d.step();
  REQUIRE(h.panel() == nullptr);
  CHECK_FALSE(h.button().on());
  CHECK(h.d.tree().focus() == h.d.find(kNotes));
  CHECK(h.d.press("Down") == KeyResult::Action);
  CHECK(h.onlyNote().pitch == PitchId::fromDegree(0, 0));
  CHECK(h.d.press("Shift+Up") == KeyResult::Action);
  CHECK(h.onlyNote().pitch != PitchId::fromDegree(0, 0));  // an equave up
}

TEST_CASE("roll help: its place, size and section survive closing and reopening") {
  Help h;
  h.clickHelp();
  RollHelpWindow* p = h.panel();
  REQUIRE(p != nullptr);
  h.d.drag(h.titlePoint(), {h.titlePoint().x - 40.0f, h.titlePoint().y - 100.0f}, 4);  // (the ? button stays clear)
  h.d.step();
  const Vec2 corner{static_cast<float>(p->rect().right()) - 3.0f, static_cast<float>(p->rect().bottom()) - 3.0f};
  h.d.drag(corner, {corner.x - 150.0f, corner.y - 90.0f}, 4);
  h.d.step();
  const IRect placed = p->rect();
  p->showSection(4);
  h.d.step();
  REQUIRE(p->currentSection() == 4);
  h.d.click(centreOf(p->closeButton().rect()));
  h.d.step();
  REQUIRE(h.panel() == nullptr);
  CHECK(h.ctx.rollHelpView().section == 4);
  CHECK_FALSE(h.ctx.rollHelpView().open);

  h.clickHelp();
  p = h.panel();
  REQUIRE(p != nullptr);
  h.d.step();
  CHECK(p->rect() == placed);
  CHECK(p->currentSection() == 4);
  CHECK(p->scrollY() > 0);
  // Closed by ? at the first section: reopens there.
  p->showSection(0);
  h.d.step();
  h.clickHelp();
  REQUIRE(h.panel() == nullptr);
  h.clickHelp();
  REQUIRE(h.panel() != nullptr);
  h.d.step();
  CHECK(h.panel()->currentSection() == 0);
  CHECK(h.panel()->scrollY() == 0);
  CHECK(h.panel()->rect() == placed);
}

TEST_CASE("roll help: the wheel scrolls the window (not the roll under it); a click in the section list jumps to that section") {
  Help h;
  h.clickHelp();
  RollHelpWindow* p = h.panel();
  REQUIRE(p != nullptr);
  const double rollScroll = h.view->scrollTick();
  const int32_t rowsBefore = h.view->rowHeight();
  const IRect r = p->rect();
  const Vec2 mid{static_cast<float>(r.x) + 0.6f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
  h.d.wheel(mid, -3.0f);
  h.d.step();
  const int32_t y1 = p->scrollY();
  CHECK(y1 > 0);
  h.d.wheel(mid, 1.0f);
  h.d.step();
  CHECK(p->scrollY() < y1);
  // At the top a further notch up is swallowed by the window.
  h.d.wheel(mid, 50.0f);
  h.d.wheel(mid, 1.0f);
  h.d.step();
  CHECK(p->scrollY() == 0);
  CHECK(h.view->scrollTick() == rollScroll);
  CHECK(h.view->rowHeight() == rowsBefore);
  REQUIRE(h.panel() == p);

  // The section list: one row per section; a click on the fifth scrolls its heading to the top.
  const Widget* rail = p->find("sections");
  REQUIRE(rail != nullptr);
  const IRect rr = rail->rect();
  const float rowH = 24.0f;
  h.d.click({static_cast<float>(rr.x) + 20.0f, static_cast<float>(rr.y) + 4.5f * rowH});
  h.d.step();
  const int32_t y5 = p->scrollY();
  CHECK(y5 > 0);
  CHECK(p->currentSection() == 4);
  REQUIRE(h.panel() == p);  // a click inside does not close it
  h.d.click({static_cast<float>(rr.x) + 20.0f, static_cast<float>(rr.y) + 1.5f * rowH});
  h.d.step();
  CHECK(p->scrollY() > 0);
  CHECK(p->scrollY() < y5);
  p->showSection(0);
  h.d.step();
  CHECK(p->scrollY() == 0);
}

TEST_CASE("roll help: every entry shows the key its action has in the loaded keymap; a rebound action its new key, an unbound one \"unbound\"") {
  Help h;
  const std::span<const roll::HelpSection> sections = roll::rollHelpSections();
  // The table itself: sections with entries, every entry complete, every {action} a registered action with a key in
  // the default keymap.
  REQUIRE(sections.size() >= 6);
  size_t bound = 0;
  for (const roll::HelpSection& s : sections) {
    INFO(std::string(s.title));
    CHECK_FALSE(s.title.empty());
    CHECK_FALSE(s.entries.empty());
    for (const roll::HelpEntry& e : s.entries) {
      INFO(std::string(e.title));
      CHECK_FALSE(e.gesture.empty());
      CHECK_FALSE(e.title.empty());
      CHECK_FALSE(e.text.empty());
      for (const std::string& a : actionsOf(e.gesture)) {
        INFO(a);
        CHECK(h.ctx.actions().find(a) != kNoAction);
        CHECK_FALSE(roll::helpActionKeys(a, h.ctx.keymap()).empty());
        ++bound;
      }
    }
  }
  CHECK(bound >= 20);

  // As shown: each entry's gesture holds its actions' current chords.
  auto checkShown = [&](RollHelpWindow& p) {
    REQUIRE(p.sectionCount() == sections.size());
    for (size_t si = 0; si < sections.size(); ++si) {
      REQUIRE(p.entryCount(si) == sections[si].entries.size());
      for (size_t ei = 0; ei < sections[si].entries.size(); ++ei) {
        const std::string shown = p.gestureText(si, ei);
        INFO(std::string(sections[si].entries[ei].title) << ": " << shown);
        for (const std::string& a : actionsOf(sections[si].entries[ei].gesture)) {
          const std::string keys = roll::helpActionKeys(a, h.ctx.keymap());
          CHECK(shown.find(keys.empty() ? std::string("unbound") : keys) != std::string::npos);
        }
        CHECK(shown.find('{') == std::string::npos);
        CHECK(shown.find('[') == std::string::npos);
      }
    }
  };
  // An action's own entry (T7.16a: the Quick start section, first, repeats gestures of the sections after it).
  REQUIRE(sections[0].title == std::string_view("Quick start"));
  auto shownFor = [&](RollHelpWindow& p, std::string_view action) -> std::string {
    for (size_t si = 1; si < sections.size(); ++si)
      for (size_t ei = 0; ei < sections[si].entries.size(); ++ei)
        if (sections[si].entries[ei].gesture.find("{" + std::string(action) + "}") != std::string_view::npos) return p.gestureText(si, ei);
    FAIL("no entry for " << action);
    return {};
  };
  h.clickHelp();
  REQUIRE(h.panel() != nullptr);
  checkShown(*h.panel());
  CHECK(shownFor(*h.panel(), "pianoroll.glide") == "Ctrl+G");
  CHECK(shownFor(*h.panel(), "pianoroll.equaveUp") == "Shift+Up / Shift+Down");
  CHECK(shownFor(*h.panel(), "pianoroll.duplicate") == "Ctrl+D");
  h.clickHelp();  // closes
  REQUIRE(h.panel() == nullptr);

  // Rebind: Glide to Ctrl+Shift+G, Draw Mode to P, Duplicate to nothing.
  auto file = loadKeymapFile(defaultKeymapPath());
  REQUIRE(file.ok());
  std::vector<KeymapEntry> entries;
  for (KeymapEntry e : file->entries) {
    if (e.action == "pianoroll.duplicate") continue;
    if (e.action == "pianoroll.glide") e.keys = "Ctrl+Shift+G";
    if (e.action == "edit.toggleDrawMode") e.keys = "P";
    auto chord = parseChord(e.keys);
    REQUIRE(chord.ok());
    e.chord = *chord;
    entries.push_back(std::move(e));
  }
  file->entries = std::move(entries);
  const Status applied = h.ctx.applyKeymap(*file);
  REQUIRE_MESSAGE(applied.ok(), (applied.ok() ? "" : applied.error().message));
  h.d.tree().setKeymap(&h.ctx.actions(), &h.ctx.keymap());
  h.clickHelp();
  REQUIRE(h.panel() != nullptr);
  checkShown(*h.panel());
  CHECK(shownFor(*h.panel(), "pianoroll.glide") == "Ctrl+Shift+G");
  CHECK(shownFor(*h.panel(), "pianoroll.duplicate") == "unbound");
  CHECK(shownFor(*h.panel(), "edit.toggleDrawMode").find('P') != std::string::npos);
  CHECK(shownFor(*h.panel(), "edit.toggleDrawMode").find('B') == std::string::npos);
  CHECK(shownFor(*h.panel(), "pianoroll.stepUp") == "Up / Down");  // untouched
}

TEST_CASE("roll help: gesture markup - chords are glued caps, [Key] a literal cap, an unknown action reads unbound") {
  Keymap km;
  REQUIRE(km.bind("pianoroll", "Ctrl+Shift+J", "a.one").ok());
  REQUIRE(km.bind("global", "F", "a.two").ok());
  REQUIRE(km.bind("pianoroll", "G", "a.two").ok());  // the piano roll's own binding wins
  CHECK(roll::helpActionKeys("a.one", km) == "Ctrl+Shift+J");
  CHECK(roll::helpActionKeys("a.two", km) == "G");
  CHECK(roll::helpActionKeys("a.none", km).empty());
  using Kind = roll::HelpToken::Kind;
  const std::vector<roll::HelpToken> t = roll::parseHelpGesture("[Alt] + drag an edge\n{a.one} {a.none}", km);
  REQUIRE(t.size() == 12);
  CHECK(t[0].kind == Kind::Key);
  CHECK(t[0].text == "Alt");
  CHECK(t[1].text == "+");
  CHECK_FALSE(t[1].glue);
  CHECK(t[4].text == "edge");
  CHECK(t[5].kind == Kind::Break);
  CHECK(t[6].kind == Kind::Key);
  CHECK(t[6].text == "Ctrl");
  CHECK(t[7].glue);
  CHECK(t[8].text == "Shift");
  CHECK(t[8].glue);
  CHECK(t[10].kind == Kind::Key);
  CHECK(t[10].text == "J");
  CHECK(t[11].kind == Kind::Unbound);
  CHECK(roll::helpGestureText(t) == "Alt + drag an edge\nCtrl+Shift+J unbound");
}
TEST_CASE("render: pianoroll-help (the help window where it first opens, over the roll, ? lit) matches the golden image") { checkGolden("pianoroll-help"); }
TEST_CASE("render: pianoroll-help-scrolled (the help window at Harmonics and the virtual fundamental) matches the golden image") { checkGolden("pianoroll-help-scrolled"); }
TEST_CASE("render: pianoroll-help-moved (the help window moved to the top right and made narrow: no section list, one column) matches the golden image") { checkGolden("pianoroll-help-moved"); }
