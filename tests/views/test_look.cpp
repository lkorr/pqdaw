// T3.x-fix-look (M3 review, "UI and aesthetic"): the control bar on the window colour with the selected track's tuning
// chip and a CPU meter that polls only while playing; the detail view collapsed until a clip is opened; arrow-key
// navigation in the arrangement and the piano roll; the label-collision helper; compact tuning labels for narrow
// chips; the piano roll's JI guide switch; the movable MOS wheel.

#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>

#include "pq/document/edit_ops.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/prefs/audio_prefs.h"
#include "pq/views/shell/shell.h"
#include "../../src/views/src/common/labels.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr const char* kArr = "main/body/top/track-area/arrangement";

struct Shellf {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d;
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  explicit Shellf(const char* fixture, uint32_t w = 1280, uint32_t h = 800) : d(w, h) {
    if (fixture) REQUIRE(ctx.openProject(views::test::fixtureDir(fixture)).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    d.tree().layoutNow();
  }
  ~Shellf() { d.tree().setRoot(std::make_unique<Widget>()); }
  const doc::Track& track(std::string_view name) const {
    const doc::Track* found = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!found && t.name == name) found = &t;
    });
    REQUIRE_MESSAGE(found != nullptr, name);
    return *found;
  }
};

}  // namespace

TEST_CASE("look: LabelSpace places a label only where it touches none already placed") {
  paint::LabelSpace s(2);
  CHECK(s.place(0, 10));
  CHECK_FALSE(s.place(5, 15));   // overlaps
  CHECK_FALSE(s.place(11, 20));  // within the gap
  CHECK(s.place(12, 20));
  CHECK(s.place(-30, -3));
  CHECK_FALSE(s.free(-4, 0));
  CHECK(s.free(40, 50));
  CHECK(s.size() == 3);
  s.clear();
  CHECK(s.place(5, 15));
}

TEST_CASE("look: compact tuning labels for narrow chips (12-EDO → 12, a MOS → its family)") {
  auto e = tuning::Edo::make(12);
  REQUIRE(e.ok());
  CHECK(compactTuningLabel(doc::TuningDef{*e, tuning::kDefaultReferenceHz}) == "12");
  tuning::MosParams m;
  m.n = 7;
  m.generator = 696.77;
  m.period = 1200.0;
  doc::TuningDef mos;
  mos.system = m;
  CHECK(compactTuningLabel(mos) == "5L 2s");
}

TEST_CASE("look: the control bar shows the selected track's tuning, else the first main tuning") {
  Shellf f("mixer");
  auto* chip = dynamic_cast<TuningChip*>(f.d.find("main/control-bar/tuning"));
  REQUIRE(chip != nullptr);
  CHECK(chip->text() == "12-EDO");  // nothing selected: the project's first main tuning
  f.arr->selectTrack(f.track("Bass").id, mod::kNone);
  CHECK(f.ctx.selectedTrack() == f.track("Bass").id);
  CHECK(chip->text() == "31-EDO");
  f.arr->selectTrack(f.track("Pad").id, mod::kNone);
  CHECK(chip->text() == "22-EDO");  // a private tuning: its summary
  f.arr->clearSelection();
  CHECK(chip->text() == "12-EDO");
}

TEST_CASE("look: the CPU meter polls the audio port only while playing; a stopped session has no timer") {
  Shellf f(nullptr);
  FakeAudioPrefsPort audio;
  audio.load = 13.4;
  f.ctx.setAudioPrefs(&audio);
  f.d.advance(1.0);
  CHECK(std::isinf(f.d.tree().nextWakeTime()));  // idle: nothing scheduled
  CHECK(f.shell->cpuText() == "CPU \xE2\x80\x93");  // never read while stopped
  CHECK(f.d.press("Space") == KeyResult::Action);  // play
  f.d.advance(0.1);
  CHECK(f.shell->cpuText() == "CPU 13 %");
  audio.load = 27.0;
  f.d.advance(0.6);
  CHECK(f.shell->cpuText() == "CPU 27 %");
  audio.load = 3.0;
  CHECK(f.d.press("Space") == KeyResult::Action);  // stop: one last reading, then no timer
  f.d.advance(0.1);
  CHECK(f.shell->cpuText() == "CPU 3 %");
  audio.load = 50.0;
  f.d.advance(2.0);
  CHECK(f.shell->cpuText() == "CPU 3 %");  // not read again while stopped
  CHECK(std::isinf(f.d.tree().nextWakeTime()));
  f.ctx.setAudioPrefs(nullptr);
}

TEST_CASE("look: the detail view starts collapsed and opens with the first clip shown in it") {
  Shellf f("arrangement");
  CHECK_FALSE(f.ctx.detailVisible());
  CHECK_FALSE(f.d.find("main/body/bottom")->visible());
  doc::ClipRef clip{};
  doc::forEachTrack(*f.ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (!clip.clip.valid() && !t.clips.empty()) clip = {t.id, t.clips.front().id};
  });
  REQUIRE(clip.clip.valid());
  f.arr->selectClips(std::span<const doc::ClipRef>(&clip, 1), true);
  CHECK(f.ctx.detailVisible());
  f.d.tree().layoutNow();
  CHECK(f.d.find("main/body/bottom")->visible());
  // After the user hides it, selecting another clip leaves it hidden (their choice stands).
  f.ctx.setDetailVisible(false);
  f.arr->clearSelection();
  f.arr->selectClips(std::span<const doc::ClipRef>(&clip, 1), true);
  CHECK_FALSE(f.ctx.detailVisible());
}

TEST_CASE("look: arrow keys move the arrangement's insert marker by a grid cell and the track selection by a row") {
  Shellf f("arrangement");
  GridSpec g;
  g.mode = GridMode::Fixed;
  g.fixedExp = 2;  // 1/4
  f.ctx.setGrid(g);
  f.d.tree().setFocus(f.arr);
  const auto rows = f.arr->rows();
  REQUIRE(rows.size() >= 3);
  f.arr->selectTrack(rows[1].id, mod::kNone);
  f.d.tree().setFocus(f.arr);
  CHECK(f.d.press("Right") == KeyResult::Action);
  REQUIRE(f.arr->selection().time.has_value());
  CHECK(f.arr->selection().time->start == kTicksPerWhole / 4);
  CHECK(f.arr->selection().time->end == kTicksPerWhole / 4);
  CHECK(f.d.press("Right") == KeyResult::Action);
  CHECK(f.arr->selection().time->start == kTicksPerWhole / 2);
  CHECK(f.d.press("Left") == KeyResult::Action);
  CHECK(f.arr->selection().time->start == kTicksPerWhole / 4);
  REQUIRE(f.arr->selection().tracks.size() == 1);
  CHECK(f.arr->selection().tracks[0] == rows[1].id);  // the marker keeps the track
  CHECK(f.d.press("Down") == KeyResult::Action);
  CHECK(f.arr->selection().tracks[0] == rows[2].id);
  CHECK(f.d.press("Up") == KeyResult::Action);
  CHECK(f.d.press("Up") == KeyResult::Action);
  CHECK(f.arr->selection().tracks[0] == rows[0].id);
  CHECK(f.d.press("Up") == KeyResult::Action);  // stays on the first row
  CHECK(f.arr->selection().tracks[0] == rows[0].id);
  CHECK(f.arr->selection().time->start == kTicksPerWhole / 4);  // the marker stays
  // Left at 0 stays at 0.
  for (int i = 0; i < 4; ++i) f.d.press("Left");
  CHECK(f.arr->selection().time->start == 0);
}

TEST_CASE("look: Left / Right nudge the selected notes in the piano roll by a grid cell (one undo step each)") {
  Shellf f("pianoroll-12edo");
  doc::ClipRef ref{};
  doc::forEachTrack(*f.ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (!ref.clip.valid() && t.kind == doc::TrackKind::Midi && !t.clips.empty()) ref = {t.id, t.clips.front().id};
  });
  f.ctx.setDetailClip(ref);
  f.d.tree().layoutNow();
  auto* view = dynamic_cast<ClipView*>(f.d.find("main/body/bottom/detail/clip/clip-view"));
  REQUIRE(view != nullptr);
  GridSpec g;
  g.mode = GridMode::Fixed;
  g.fixedExp = 4;  // 1/16
  f.ctx.setGrid(g);
  auto noteStart = [&](uint32_t id) {
    const doc::Track* t = doc::findTrackPtr(*f.ctx.doc(), ref.track);
    const doc::NoteClip& c = *t->clips[static_cast<size_t>(doc::findClipIndex(*t, ref.clip))].clip;
    return c.notes.findById(doc::NoteId{id})->start;
  };
  const Ticks s0 = noteStart(116);
  view->selectAll();
  f.d.tree().setFocus(f.d.find("main/body/bottom/detail/clip/clip-view/notes"));
  const size_t h = f.ctx.store().history().cursor();
  CHECK(f.d.press("Right") == KeyResult::Action);
  CHECK(noteStart(116) == s0 + kTicksPerWhole / 16);
  CHECK(f.d.press("Right") == KeyResult::Action);
  CHECK(f.d.press("Left") == KeyResult::Action);
  CHECK(noteStart(116) == s0 + kTicksPerWhole / 16);
  CHECK(f.ctx.store().history().cursor() == h + 3);
  // At the clip's start the earliest note cannot move earlier: nothing happens.
  CHECK(f.d.press("Left") == KeyResult::Action);
  CHECK(f.d.press("Left") == KeyResult::Action);
  const size_t h2 = f.ctx.store().history().cursor();
  CHECK(f.d.press("Left") == KeyResult::Action);
  CHECK(f.ctx.store().history().cursor() == h2);
}

TEST_CASE("look: the clip box's JI switch shows / hides the virtual fundamental's guide") {
  Shellf f("pianoroll-31edo");
  doc::ClipRef ref{};
  doc::forEachTrack(*f.ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (!ref.clip.valid() && t.kind == doc::TrackKind::Midi && !t.clips.empty()) ref = {t.id, t.clips.front().id};
  });
  f.ctx.setDetailClip(ref);
  f.d.tree().layoutNow();
  auto* view = dynamic_cast<ClipView*>(f.d.find("main/body/bottom/detail/clip/clip-view"));
  REQUIRE(view != nullptr);
  Widget* row = f.d.find("main/body/bottom/detail/clip/clip-view/clip-box/ji-row");
  REQUIRE(row != nullptr);
  CHECK(row->visible());  // the Septimal clip has reference-anchored ratios: it has a virtual fundamental
  CHECK(view->showHarmonics());
  f.d.click(f.d.center("main/body/bottom/detail/clip/clip-view/clip-box/ji-row/ji"));
  CHECK_FALSE(view->showHarmonics());
  f.d.click(f.d.center("main/body/bottom/detail/clip/clip-view/clip-box/ji-row/ji"));
  CHECK(view->showHarmonics());
}

TEST_CASE("look: the MOS wheel moves by its title band and reopens where it was left") {
  Shellf f("pianoroll-5L2s");
  MosWheelPopover::forgetPosition();
  doc::ClipRef ref{};
  doc::forEachTrack(*f.ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (!ref.clip.valid() && t.kind == doc::TrackKind::Midi && !t.clips.empty()) ref = {t.id, t.clips.front().id};
  });
  MosWheelPopover* w = openMosWheel(f.shell->overlay(), f.ctx, TuningTarget::forClip(ref));
  REQUIRE(w != nullptr);
  f.d.tree().layoutNow();
  const IRect r0 = w->rect();
  const Vec2 grab{static_cast<float>(r0.x + r0.w / 2), static_cast<float>(r0.y + 8)};
  f.d.drag(grab, {grab.x - 150.0f, grab.y + 60.0f}, 6);
  f.d.tree().layoutNow();
  const IRect r1 = w->rect();
  CHECK(r1.x == r0.x - 150);
  CHECK(r1.y == r0.y + 60);
  REQUIRE(MosWheelPopover::rememberedPosition().has_value());
  w->cancel();
  f.d.step();
  MosWheelPopover* w2 = openMosWheel(f.shell->overlay(), f.ctx, TuningTarget::forClip(ref));
  REQUIRE(w2 != nullptr);
  f.d.tree().layoutNow();
  CHECK(w2->rect().x == r1.x);
  CHECK(w2->rect().y == r1.y);
  w2->cancel();
  f.d.step();
  MosWheelPopover::forgetPosition();  // the goldens open it centred
}
