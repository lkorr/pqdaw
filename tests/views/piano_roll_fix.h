#pragma once

// T7.14: the fixture of the piano roll's group and vibrato tests: the shell over the empty project with one new MIDI
// track and an empty four-bar 12-EDO clip open in the clip view (a quarter-note grid, 60 px per beat, 14 px rows = 100 ¢
// per row), driven through the real pointer and key dispatch (TestDriver → UiTree → the note area / the keymap / the
// menus / the inline fields).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
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

namespace roll_fix {

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using pq::tuning::PitchId;

inline constexpr Ticks kQ = kPpq;
inline constexpr float kRow = 14.0f;   // px per 12-EDO step (100 ¢)
inline constexpr float kBeat = 60.0f;  // px per quarter note

inline Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)}; }
inline Vec2 mid(const Rect& r) { return {0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)}; }

struct Roll {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ClipView* view = nullptr;
  doc::ClipRef ref{};

  explicit Roll(int64_t centre = 6) {
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
    view->centreOnDegree(centre);
    view->setZoom(static_cast<double>(kBeat) / static_cast<double>(kQ), 0.0);
    view->scrollToTick(-1.0 * static_cast<double>(kQ));
    GridSpec g;
    g.mode = GridMode::Fixed;
    g.fixedExp = 2;  // 1/4
    ctx.setGrid(g);
    d.tree().layoutNow();
  }
  ~Roll() { d.tree().setRoot(std::make_unique<Widget>()); }

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
  size_t noteCount() const { return clip().notes.size(); }
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
  // The pitch the roll draws the note at, `frac` of the way through it (cents from the reference).
  double cents(doc::NoteId id, double frac) const {
    const std::optional<double> c = view->noteCentsAt(id, frac);
    REQUIRE(c.has_value());
    return *c;
  }
  void focusNotes() {
    const IRect a = d.rect("main/body/bottom/detail/clip/clip-view/notes");
    REQUIRE_FALSE(a.empty());
    d.click({static_cast<float>(a.right()) - 6.0f, static_cast<float>(a.y) + 6.0f});
    d.step();
    REQUIRE(d.tree().focus() == d.find("main/body/bottom/detail/clip/clip-view/notes"));
  }
  // Adds the notes; returns their ids in the order given.
  std::vector<doc::NoteId> addNotes(const std::vector<doc::Note>& notes) {
    auto e = doc::ops::addNotes(*ctx.doc(), ctx.store().ids(), ref, notes);
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    d.step();
    std::vector<doc::NoteId> ids;
    for (const doc::Note& n : notes)
      for (const doc::Note& have : clip().notes)
        if (have.start == n.start && have.pitch == n.pitch && have.length == n.length) ids.push_back(have.id);
    REQUIRE(ids.size() == notes.size());
    return ids;
  }
  static doc::Note plain(Ticks start, Ticks len, int32_t degree) {
    doc::Note n;
    n.start = start;
    n.length = len;
    n.pitch = PitchId::fromDegree(degree, 0);
    return n;
  }
  // A 12-EDO chord at `start`.
  std::vector<doc::NoteId> chord(std::vector<int32_t> degrees, Ticks start = 0, Ticks length = 4 * kQ) {
    std::vector<doc::Note> notes;
    for (const int32_t deg : degrees) notes.push_back(plain(start, length, deg));
    return addNotes(notes);
  }
  void select(const std::vector<doc::NoteId>& ids) {
    view->select(std::span<const doc::NoteId>(ids.data(), ids.size()));
    d.step();
  }
  TextEdit* field() { return dynamic_cast<TextEdit*>(d.tree().focus()); }
  Menu* topMenu() { return shell->overlay().openCount() ? dynamic_cast<Menu*>(shell->overlay().top()) : nullptr; }
  Menu* rightClick(Vec2 p) {
    d.click(p, mod::kNone, PointerButton::Right);
    d.step();
    return topMenu();
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
  const doc::JiGroup& onlyGroup() const {
    REQUIRE(doc::jiGroupsOf(clip()).size() == 1);
    return doc::jiGroupsOf(clip())[0];
  }
  std::vector<int64_t> proportion(const doc::JiGroup& g) const {
    const std::optional<doc::GroupHarmonics> gh = doc::groupHarmonics(clip(), g);
    REQUIRE(gh.has_value());
    return gh->proportion;
  }
};

inline void checkGolden(const char* scene) {
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

}  // namespace roll_fix
