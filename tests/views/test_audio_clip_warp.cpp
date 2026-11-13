// T6.8: warping in the audio Clip View (tests/views/fixtures/audio), every edit through real pointer / key dispatch
// (TestDriver): the warp chooser's seven modes and each mode's rows, the mode settings (one undo step per drag), :2 / *2,
// warp markers (double-click adds at a transient within reach, drag moves in content time keeping the source frame,
// Ctrl-drag slides the source keeping the content tick, Esc cancels, Delete and double-click remove; each one undo
// step), the right-click commands (Warp From Here, Warp as N-Bar Loop, Delete Warp Marker), the Warp preferences tab,
// and the goldens audio-clip-warp-beats / audio-clip-warp-markers.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "../ui/golden_check.h"
#include "pq/document/audio_clip.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/audio_clip/audio_clip_view.h"
#include "pq/views/fake_transport.h"
#include "pq/views/prefs/audio_prefs.h"
#include "pq/views/sample_analysis.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr const char* kView = "main/body/bottom/detail/clip/audio-clip-view";

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)}; }

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 900};
  std::unique_ptr<Shell> shell;
  doc::ClipRef beat{};  // Drums' first clip: beat.wav (2 s, four kicks and hats), warp Off
  Fixture() {
    REQUIRE(ctx.openProject(views::test::fixtureDir("audio")).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    if (auto* body = dynamic_cast<Splitter*>(d.find("main/body"))) body->setPosition(420.0f);
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (t.name == "Drums" && !beat.clip.valid()) beat = {t.id, t.clips.at(0).id};
    });
    REQUIRE(beat.clip.valid());
    ctx.setDetailVisible(true);
    ctx.setDetailClip(beat);
    d.tree().layoutNow();
    d.step();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }
  size_t history() const { return ctx.store().history().cursor(); }
  AudioClipView& view() {
    AudioClipView* v = findAudioClipView(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const doc::AudioClip& clip() const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), beat.track);
    REQUIRE(t != nullptr);
    const int32_t i = doc::findClipIndex(*t, beat.clip);
    REQUIRE(i >= 0);
    return *t->clips[static_cast<size_t>(i)].audio;
  }
  Widget* box(std::string_view path) { return d.find(std::string(kView) + "/audio-box/props/" + std::string(path)); }
  // Opens `dd` with a click and clicks its item labelled `label`.
  void pick(std::string_view ddPath, std::string_view label) {
    auto* dd = dynamic_cast<Dropdown*>(box(ddPath));
    REQUIRE(dd != nullptr);
    d.tree().layoutNow();
    d.click(mid(dd->rect()));
    d.step();
    auto* menu = dynamic_cast<Menu*>(shell->overlay().top());
    REQUIRE(menu != nullptr);
    int32_t item = -1;
    for (size_t i = 0; i < menu->items().size(); ++i)
      if (menu->items()[i].label == label) item = static_cast<int32_t>(i);
    REQUIRE_MESSAGE(item >= 0, "no item " << label);
    d.click(mid(menu->itemRect(item)));
    d.step();
    d.tree().layoutNow();
  }
  bool shown(std::string_view rowPath) {
    Widget* w = box(rowPath);
    REQUIRE(w != nullptr);
    return w->visible();
  }
  // A point in the waveform at content tick t (warped clips), 30 % down.
  Vec2 wave(double t) {
    d.tree().layoutNow();
    const IRect w = view().sampleRect();
    return {view().xOfContent(t), static_cast<float>(w.y) + 0.3f * static_cast<float>(w.h)};
  }
  Vec2 stripAt(float x) {
    const IRect s = view().warpStripRect();
    return {x, static_cast<float>(s.y) + 0.5f * static_cast<float>(s.h)};
  }
  Menu* rightClick(Vec2 p) {
    d.click(p, mod::kNone, PointerButton::Right);
    d.step();
    return dynamic_cast<Menu*>(shell->overlay().top());
  }
  void chooseMenu(Menu* m, std::string_view label) {
    REQUIRE(m != nullptr);
    int32_t item = -1;
    for (size_t i = 0; i < m->items().size(); ++i)
      if (m->items()[i].label.rfind(label, 0) == 0) item = static_cast<int32_t>(i);
    REQUIRE_MESSAGE(item >= 0, "no menu item " << label);
    d.click(mid(m->itemRect(item)));
    d.step();
    d.tree().layoutNow();
  }
};

}  // namespace

TEST_CASE("audio clip warp: the chooser's seven modes, each mode's rows, its settings one undo step each, :2 / *2") {
  Fixture f;
  auto* dd = dynamic_cast<Dropdown*>(f.box("sample/warp-row/warp"));
  REQUIRE(dd != nullptr);
  REQUIRE(dd->items().size() == 7);
  CHECK(dd->items()[6] == "Complex Pro");
  CHECK(f.clip().warp == doc::AudioWarp::Off);
  CHECK_FALSE(f.shown("sample/preserve-row"));
  CHECK(f.box("clip/transpose-row/transpose")->enabled());

  // Beats through the menu: one step; the tempo seeded from the detection (beat.wav has no tag: its kicks, 120 BPM);
  // its three rows.
  size_t h = f.history();
  f.pick("sample/warp-row/warp", "Beats");
  CHECK(f.clip().warp == doc::AudioWarp::Beats);
  CHECK(std::abs(f.clip().sourceBpm - 120.0) < 0.5);
  CHECK(f.history() == h + 1);
  CHECK(f.shown("sample/preserve-row"));
  CHECK(f.shown("sample/beats-loop-row"));
  CHECK(f.shown("sample/beats-env-row"));
  CHECK_FALSE(f.shown("sample/grain-row"));
  CHECK(f.box("clip/transpose-row/transpose")->enabled());  // time-stretch modes transpose
  h = f.history();
  f.pick("sample/preserve-row/preserve", "Transients");
  CHECK(f.clip().warpParams.beatsGranulation == 0);
  f.pick("sample/beats-loop-row/beats-loop", "Back-Fwd");
  CHECK(f.clip().warpParams.beatsLoop == doc::BeatsLoop::BackForward);
  CHECK(f.history() == h + 2);
  // The envelope box: a drag is one undo step.
  h = f.history();
  const Vec2 ec = mid(f.box("sample/beats-env-row/beats-env")->rect());
  f.d.drag(ec, {ec.x, ec.y + 40.0f}, 8);
  CHECK(f.clip().warpParams.beatsEnvelope < 100.0f);
  CHECK(f.history() == h + 1);

  // Texture: grain and flux; Tones: grain only; Complex Pro: formants and envelope; Re-Pitch greys the transpose.
  f.pick("sample/warp-row/warp", "Texture");
  CHECK(f.shown("sample/grain-row"));
  CHECK(f.shown("sample/flux-row"));
  CHECK_FALSE(f.shown("sample/preserve-row"));
  h = f.history();
  const Vec2 fc = mid(f.box("sample/flux-row/flux")->rect());
  f.d.drag(fc, {fc.x, fc.y - 30.0f}, 6);
  CHECK(f.clip().warpParams.flux > 0.0f);
  CHECK(f.history() == h + 1);
  CHECK(f.clip().warpParams.beatsGranulation == 0);  // every mode's settings are kept
  f.pick("sample/warp-row/warp", "Tones");
  CHECK(f.shown("sample/grain-row"));
  CHECK_FALSE(f.shown("sample/flux-row"));
  f.pick("sample/warp-row/warp", "Complex Pro");
  CHECK(f.shown("sample/formants-row"));
  CHECK(f.shown("sample/env-order-row"));
  CHECK_FALSE(f.shown("sample/grain-row"));
  h = f.history();
  const Vec2 oc = mid(f.box("sample/env-order-row/env-order")->rect());
  f.d.drag(oc, {oc.x, oc.y + 30.0f}, 6);
  CHECK(f.clip().warpParams.envelope < 128.0f);
  CHECK(f.history() == h + 1);
  f.pick("sample/warp-row/warp", "Complex");
  CHECK_FALSE(f.shown("sample/formants-row"));
  f.pick("sample/warp-row/warp", "Re-Pitch");
  CHECK_FALSE(f.box("clip/transpose-row/transpose")->enabled());

  // :2 and *2: one step each, exact.
  const double bpm = f.clip().sourceBpm;
  h = f.history();
  f.d.click(mid(f.box("sample/bpm-scale-row/halve")->rect()));
  CHECK(f.clip().sourceBpm == bpm / 2.0);
  f.d.click(mid(f.box("sample/bpm-scale-row/double")->rect()));
  f.d.click(mid(f.box("sample/bpm-scale-row/double")->rect()));
  CHECK(f.clip().sourceBpm == bpm * 2.0);
  CHECK(f.history() == h + 3);
  f.pick("sample/warp-row/warp", "Off");
  CHECK_FALSE(f.box("sample/bpm-scale-row/halve")->enabled());
}

TEST_CASE("audio clip warp: markers through real dispatch — add at a transient, drag, Ctrl-drag, Esc, Delete, "
          "double-click remove; each one undo step") {
  Fixture f;
  f.pick("sample/warp-row/warp", "Beats");
  AudioClipView& v = f.view();
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), f.clip().sample);
  REQUIRE(s != nullptr);
  const double rate = s->rate;
  CHECK(v.warpStripRect().h > 0);
  CHECK(f.clip().markers.empty());

  // The sample's transients (beat.wav: a kick every half second): the analysis ran (synchronously in tests).
  const SampleAnalysisResult* an = f.ctx.sampleAnalysis().get(f.clip().sample);
  REQUIRE(an != nullptr);
  REQUIRE(an->transients != nullptr);
  REQUIRE(an->transients->onsets.size() >= 4);
  // The kick nearest 1 s.
  double kick = an->transients->onsets[0];
  for (const double o : an->transients->onsets)
    if (std::abs(o - rate) < std::abs(kick - rate)) kick = o;
  CHECK(std::abs(kick - rate) < 0.02 * rate);
  const double kickContent = doc::warpContentAt(f.clip(), rate, kick);

  // Double-click 2 px off the kick: the marker lands on the transient. One step.
  size_t h = f.history();
  const Vec2 near = f.wave(kickContent);
  f.d.doubleClick({near.x + 2.0f, near.y});
  REQUIRE(f.clip().markers.size() == 1);
  CHECK(f.clip().markers[0].content == static_cast<Ticks>(std::llround(kickContent)));
  CHECK(f.clip().markers[0].sourceFrame == doctest::Approx(doc::warpFrameAt(f.clip(), rate, static_cast<double>(std::llround(kickContent)))));
  CHECK(f.history() == h + 1);
  CHECK(v.selectedWarpMarker() == 0);
  // Two more by double-click, near 0.3 s and 1.7 s of the source.
  f.d.doubleClick(f.wave(doc::warpContentAt(f.clip(), rate, 0.30 * rate)));
  f.d.doubleClick(f.wave(doc::warpContentAt(f.clip(), rate, 1.70 * rate)));
  REQUIRE(f.clip().markers.size() == 3);
  CHECK(f.history() == h + 3);

  // Drag the middle marker (the kick) half a beat later: its content tick moves (1/16 snapped), its frame stays, the
  // others unchanged. One step.
  const std::vector<doc::WarpMarker> m0 = f.clip().markers;
  h = f.history();
  const float x1 = v.warpMarkerX(1);
  const float dx = v.xOfContent(static_cast<double>(m0[1].content + kPpq / 2)) - x1;
  f.d.drag(f.stripAt(x1), f.stripAt(x1 + dx), 6);
  REQUIRE(f.clip().markers.size() == 3);
  CHECK(f.clip().markers[1].sourceFrame == m0[1].sourceFrame);
  CHECK(f.clip().markers[1].content != m0[1].content);
  CHECK(f.clip().markers[1].content % (kPpq / 4) == 0);
  CHECK(std::abs(f.clip().markers[1].content - (m0[1].content + kPpq / 2)) <= kPpq / 4);
  CHECK(f.clip().markers[0] == m0[0]);
  CHECK(f.clip().markers[2] == m0[2]);
  CHECK(f.history() == h + 1);

  // Ctrl-drag it left: the audio slides left with the pointer, so later source sits under the marker (its frame grows);
  // its content tick stays. One step.
  const doc::WarpMarker m1 = f.clip().markers[1];
  h = f.history();
  const float x1b = v.warpMarkerX(1);
  f.d.drag(f.stripAt(x1b), f.stripAt(x1b - 25.0f), 5, mod::kCtrl);
  CHECK(f.clip().markers[1].content == m1.content);
  CHECK(f.clip().markers[1].sourceFrame > m1.sourceFrame);
  CHECK(f.history() == h + 1);

  // Esc during a drag: back as it was, no step, capture released.
  const std::vector<doc::WarpMarker> m2 = f.clip().markers;
  h = f.history();
  const float x2 = v.warpMarkerX(2);
  f.d.move(f.stripAt(x2));
  f.d.down(f.stripAt(x2));
  f.d.move(f.stripAt(x2 + 20.0f));
  f.d.move(f.stripAt(x2 + 40.0f));
  CHECK(v.dragging());
  CHECK(f.clip().markers != m2);
  CHECK(f.d.press("Escape") == KeyResult::Widget);
  CHECK(f.clip().markers == m2);
  CHECK(f.history() == h);
  CHECK_FALSE(f.d.tree().captured());
  f.d.up(f.stripAt(x2 + 40.0f));
  CHECK(f.history() == h);

  // A click selects marker 2; Delete removes it. One step.
  f.d.click(f.stripAt(v.warpMarkerX(2)));
  CHECK(v.selectedWarpMarker() == 2);
  CHECK(f.history() == h);  // a click without a move records nothing
  f.d.press("Delete");
  CHECK(f.clip().markers.size() == 2);
  CHECK(f.history() == h + 1);
  // Double-click on marker 0 removes it. One step.
  f.d.doubleClick(f.stripAt(v.warpMarkerX(0)));
  CHECK(f.clip().markers.size() == 1);
  CHECK(f.history() == h + 2);
  // Undo brings each back.
  REQUIRE(f.ctx.undo());
  CHECK(f.clip().markers.size() == 2);
}

TEST_CASE("audio clip warp: right-click — Warp From Here, Warp as N-Bar Loop (an Off clip in the default mode), "
          "Delete Warp Marker; each one undo step") {
  Fixture f;
  AudioClipView& v = f.view();
  ViewPrefs prefs = f.ctx.viewPrefs();
  prefs.defaultWarpMode = doc::AudioWarp::Tones;
  f.ctx.setViewPrefs(prefs);
  // Off: Warp From Here is disabled; Warp as N-Bar Loop warps the clip's 2 s as one bar at 120 BPM in Tones.
  const IRect w = v.sampleRect();
  Menu* m = f.rightClick({static_cast<float>(w.x + w.w / 3), static_cast<float>(w.y + w.h / 2)});
  REQUIRE(m != nullptr);
  REQUIRE(m->items().size() >= 2);
  CHECK(m->items()[0].label == "Warp From Here");
  CHECK_FALSE(m->items()[0].enabled);
  CHECK(m->items()[1].label == "Warp as 1-Bar Loop");
  size_t h = f.history();
  f.chooseMenu(m, "Warp as");
  CHECK(f.clip().warp == doc::AudioWarp::Tones);
  CHECK(f.clip().loop.enabled);
  CHECK(f.clip().loop.end - f.clip().loop.start == kTicksPerWhole);
  CHECK(f.clip().sourceBpm == doctest::Approx(120.0).epsilon(1e-6));
  CHECK(f.history() == h + 1);

  // A marker, then Warp From Here on it at the detected tempo (beat.wav's kicks: 120 BPM).
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), f.clip().sample);
  REQUIRE(s != nullptr);
  const double c1 = doc::warpContentAt(f.clip(), s->rate, 0.5 * s->rate);
  f.d.doubleClick(f.wave(c1));
  REQUIRE(f.clip().markers.size() >= 1);
  const size_t before = f.clip().markers.size();
  h = f.history();
  const int32_t sel = v.selectedWarpMarker();
  REQUIRE(sel >= 0);
  m = f.rightClick(f.stripAt(v.warpMarkerX(static_cast<size_t>(sel))));
  REQUIRE(m != nullptr);
  CHECK(m->items().back().label == "Delete Warp Marker");
  const double detected = v.detectedBpm();
  MESSAGE("beat.wav detected at ", detected, " BPM");
  f.chooseMenu(m, "Warp From Here");
  CHECK(f.history() == h + 1);
  CHECK(f.clip().markers.size() >= 1);
  (void)before;
  // Delete Warp Marker from the menu.
  h = f.history();
  const size_t n = f.clip().markers.size();
  m = f.rightClick(f.stripAt(v.warpMarkerX(0)));
  f.chooseMenu(m, "Delete Warp Marker");
  CHECK(f.clip().markers.size() == n - 1);
  CHECK(f.history() == h + 1);
}

TEST_CASE("audio clip warp: the Warp preferences tab sets ViewPrefs through its controls") {
  Fixture f;
  FakeAudioPrefsPort port = FakeAudioPrefsPort::studio();
  PreferencesWindow* w = openPreferences(f.shell->overlay(), port, &f.ctx);
  REQUIRE(w != nullptr);
  f.d.tree().layoutNow();
  Widget* tab = w->find("body/tabs/warp");
  REQUIRE(tab != nullptr);
  f.d.click(mid(tab->rect()));
  f.d.tree().layoutNow();
  CHECK(w->tab() == PreferencesWindow::Tab::Warp);
  CHECK_FALSE(f.ctx.viewPrefs().autoWarpLongSamples);
  REQUIRE(w->autoWarpToggle() != nullptr);
  f.d.click(mid(w->autoWarpToggle()->rect()));
  CHECK(f.ctx.viewPrefs().autoWarpLongSamples);
  Dropdown* dd = w->defaultWarpChooser();
  REQUIRE(dd != nullptr);
  CHECK(dd->items().at(static_cast<size_t>(dd->index())) == "Beats");
  f.d.click(mid(dd->rect()));
  f.d.step();
  auto* menu = dynamic_cast<Menu*>(f.shell->overlay().top());
  REQUIRE(menu != nullptr);
  int32_t complexItem = -1;
  for (size_t i = 0; i < menu->items().size(); ++i)
    if (menu->items()[i].label == "Complex") complexItem = static_cast<int32_t>(i);
  REQUIRE(complexItem >= 0);
  f.d.click(mid(menu->itemRect(complexItem)));
  f.d.step();
  CHECK(f.ctx.viewPrefs().defaultWarpMode == doc::AudioWarp::Complex);
}

TEST_CASE("T6.7b: the Record preferences tab sets ViewPrefs::recordOffsetMs (typed, clamped to +-100 ms)") {
  Fixture f;
  FakeAudioPrefsPort port = FakeAudioPrefsPort::studio();
  PreferencesWindow* w = openPreferences(f.shell->overlay(), port, &f.ctx);
  REQUIRE(w != nullptr);
  f.d.tree().layoutNow();
  Widget* tab = w->find("body/tabs/record");
  REQUIRE(tab != nullptr);
  f.d.click(mid(tab->rect()));
  f.d.tree().layoutNow();
  CHECK(w->tab() == PreferencesWindow::Tab::Record);
  DragBox* box = w->recordOffsetBox();
  REQUIRE(box != nullptr);
  CHECK(box->value() == 0.0);
  auto type = [&](const char* text) {
    f.d.doubleClick(mid(box->rect()));
    REQUIRE(box->editing());
    f.d.type(text);  // the type-in opens with its text selected
    f.d.press("Enter");
  };
  type("2.5");
  CHECK(f.ctx.viewPrefs().recordOffsetMs == doctest::Approx(2.5f));
  type("-1.25");
  CHECK(f.ctx.viewPrefs().recordOffsetMs == doctest::Approx(-1.25f));
  type("500");
  CHECK(f.ctx.viewPrefs().recordOffsetMs == doctest::Approx(100.0f));
}

TEST_CASE("AS.1: the Record tab's Autosave switch sets ViewPrefs::autosave (on by default)") {
  Fixture f;
  FakeAudioPrefsPort port = FakeAudioPrefsPort::studio();
  PreferencesWindow* w = openPreferences(f.shell->overlay(), port, &f.ctx);
  REQUIRE(w != nullptr);
  f.d.tree().layoutNow();
  Widget* tab = w->find("body/tabs/record");
  REQUIRE(tab != nullptr);
  f.d.click(mid(tab->rect()));
  f.d.tree().layoutNow();
  Toggle* t = w->autosaveToggle();
  REQUIRE(t != nullptr);
  CHECK(f.ctx.viewPrefs().autosave);
  CHECK(t->on());
  f.d.click(mid(t->rect()));
  CHECK_FALSE(f.ctx.viewPrefs().autosave);
  CHECK_FALSE(t->on());
  f.d.click(mid(t->rect()));
  CHECK(f.ctx.viewPrefs().autosave);
}

namespace {

void checkGolden(const char* scene) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, views::test::fixtureDir("audio"), w, h, {}, o);
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

TEST_CASE("render: audio-clip-warp-beats (Beats, Preserve Transients: the mode rows, transients, beat grid) matches the golden") {
  checkGolden("audio-clip-warp-beats");
}

TEST_CASE("render: audio-clip-warp-markers (Complex Pro, three markers, the middle one moved) matches the golden") {
  checkGolden("audio-clip-warp-markers");
}
