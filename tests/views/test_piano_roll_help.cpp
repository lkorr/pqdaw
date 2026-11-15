// KB.help: the piano roll's help panel (the `?` button beside Fold / Scale, at the right end of the clip
// box's title bar). Through the real shell, pointer and key
// dispatch (TestDriver -> UiTree -> the corner's button / the overlay layer / the popover / the keymap):
//   a click on `?` opens the panel over the roll (the button lit), a second click, Esc or a click outside closes it;
//   closed, the roll's keys reach the roll; open, the arrows scroll the panel and the roll's notes stay put;
//   the wheel and the section list scroll it; every entry bound to an action shows the key that action has in the
//   LOADED keymap (a rebound action shows its new key, an unbound one "unbound"); every `{action}` of the table is a
//   registered action (an entry whose feature was removed fails here).
// Goldens: pianoroll-help, pianoroll-help-scrolled.

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
using pq::views::roll::RollHelpPopover;

namespace {

constexpr Ticks kQ = kPpq;
const char* const kView = "main/body/bottom/detail/clip/clip-view";
const std::string kHelp = std::string(kView) + "/help";
const std::string kNotes = std::string(kView) + "/notes";

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

  RollHelpPopover* panel() { return dynamic_cast<RollHelpPopover*>(shell->overlay().top()); }
  Toggle& button() {
    auto* b = dynamic_cast<Toggle*>(d.find(kHelp));
    REQUIRE(b != nullptr);
    return *b;
  }
  void clickHelp() {
    d.click(d.center(kHelp));
    d.step();
  }
  const doc::Note& onlyNote() const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), ref.track);
    REQUIRE(t != nullptr);
    const int32_t i = doc::findClipIndex(*t, ref.clip);
    REQUIRE(i >= 0);
    const doc::NoteClip& clip = *t->clips[static_cast<size_t>(i)].clip;
    const doc::Note* found = nullptr;
    for (const doc::Note& n : clip.notes) found = &n;
    REQUIRE(found != nullptr);
    return *found;
  }
  // One selected note on degree 0, the note area focused.
  void noteSelected() {
    const IRect a = d.rect(kNotes);
    REQUIRE_FALSE(a.empty());
    d.click({static_cast<float>(a.right()) - 6.0f, static_cast<float>(a.y) + 6.0f});
    d.step();
    REQUIRE(d.tree().focus() == d.find(kNotes));
    doc::Note n;
    n.start = 0;
    n.length = 2 * kQ;
    n.pitch = PitchId::fromDegree(0, 0);
    n.velocity = 100.0f;
    auto e = doc::ops::addNotes(*ctx.doc(), ctx.store().ids(), ref, std::vector<doc::Note>{n});
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    d.step();
    const doc::NoteId id = onlyNote().id;
    view->select(std::span<const doc::NoteId>(&id, 1));
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

TEST_CASE("roll help: a click on ? opens the panel over the roll; a second click, Esc and a click outside close it") {
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

  h.clickHelp();
  RollHelpPopover* p = h.panel();
  REQUIRE(p != nullptr);
  CHECK(btn.on());
  CHECK(h.view->helpOpen());
  // Over the roll: inside the panels (below the control bar), its bottom on the roll's, right of the keys so the
  // button stays reachable; readable: at least 600 px wide at 1280 x 800.
  const IRect r = p->rect(), body = h.d.rect("main/body"), notes = h.d.rect(kNotes);
  CHECK(r.x >= notes.x);
  CHECK(r.y >= body.y);
  CHECK(r.bottom() <= body.bottom());
  CHECK(r.bottom() >= notes.y + notes.h / 2);
  CHECK(r.w >= 600);
  CHECK_FALSE(r.contains(h.d.center(kHelp)));
  CHECK(p->sectionCount() >= 6);
  CHECK(p->maxScrollY() > 0);  // more text than fits: it scrolls

  // A second click on the button closes it (the press outside the panel dismisses it; it does not reopen).
  h.clickHelp();
  CHECK(h.panel() == nullptr);
  CHECK_FALSE(btn.on());

  // Esc.
  h.clickHelp();
  REQUIRE(h.panel() != nullptr);
  CHECK(h.d.press("Escape") == KeyResult::Widget);
  h.d.step();
  CHECK(h.panel() == nullptr);
  CHECK_FALSE(btn.on());

  // A click outside (on the arrangement).
  h.clickHelp();
  REQUIRE(h.panel() != nullptr);
  h.d.click({20.0f, static_cast<float>(body.y) + 20.0f});
  h.d.step();
  CHECK(h.panel() == nullptr);
  CHECK_FALSE(btn.on());
}

TEST_CASE("roll help: closed, the roll's keys reach the roll; open, the arrows scroll the panel and the notes stay put") {
  Help h;
  h.noteSelected();
  // Closed (never opened): Up moves the selected note a step, B toggles Draw Mode.
  CHECK(h.d.press("Up") == KeyResult::Action);
  CHECK(h.onlyNote().pitch == PitchId::fromDegree(1, 0));
  CHECK_FALSE(h.ctx.drawMode());
  CHECK(h.d.press("B") == KeyResult::Action);
  CHECK(h.ctx.drawMode());
  CHECK(h.d.press("B") == KeyResult::Action);
  CHECK_FALSE(h.ctx.drawMode());
  // The button takes no focus: after a click on it and Esc the roll has its keys back.
  h.clickHelp();
  RollHelpPopover* p = h.panel();
  REQUIRE(p != nullptr);
  CHECK(p->scrollY() == 0);
  CHECK(h.d.press("Down") == KeyResult::Widget);  // scrolls the panel
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
  CHECK(h.d.tree().focus() == h.d.find(kNotes));
  CHECK(h.d.press("Down") == KeyResult::Action);
  CHECK(h.onlyNote().pitch == PitchId::fromDegree(0, 0));
  CHECK(h.d.press("Shift+Up") == KeyResult::Action);
  CHECK(h.onlyNote().pitch != PitchId::fromDegree(0, 0));  // an equave up
}

TEST_CASE("roll help: the wheel scrolls the panel (not the roll under it); a click in the section list jumps to that section") {
  Help h;
  h.clickHelp();
  RollHelpPopover* p = h.panel();
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
  // At the top a further notch up is swallowed by the panel.
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
  auto checkShown = [&](RollHelpPopover& p) {
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
  auto shownFor = [&](RollHelpPopover& p, std::string_view action) -> std::string {
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
  h.d.press("Escape");
  h.d.step();
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

TEST_CASE("render: pianoroll-help (the help panel open over the roll, ? lit) matches the golden image") { checkGolden("pianoroll-help"); }
TEST_CASE("render: pianoroll-help-scrolled (the help panel at Harmonics and the virtual fundamental) matches the golden image") { checkGolden("pianoroll-help-scrolled"); }
