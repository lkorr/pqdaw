// T6.8: warping in the audio Clip View (tests/views/fixtures/audio), every edit through real pointer / key dispatch
// (TestDriver): the warp chips' seven modes and each mode's card, the mode settings (one undo step per drag), :2 / x2,
// warp markers (double-click adds at a transient within reach, drag moves in content time keeping the source frame,
// Ctrl-drag slides the source keeping the content tick, Esc cancels, Delete and double-click remove; each one undo
// step), the right-click commands (Warp From Here, Warp as N-Bar Loop, Delete Warp Marker), the Warp preferences tab,
// and the goldens audio-clip-warp-beats / audio-clip-warp-markers. AC.look: the transpose knob (steps, periods, typed
// cents and exact ratios, the wheel, drags) and a percentage knob's lit arc.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "../ui/golden_check.h"
#include "pq/document/audio_clip.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/theme.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/audio_clip/audio_clip_view.h"
#include "pq/views/audio_clip/clip_controls.h"
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
  audio_clip::Chip* chipAt(std::string_view path) { return dynamic_cast<audio_clip::Chip*>(box(path)); }
  // AC.look: clicks the chip at `path` (a real pointer press and release at its centre).
  void chip(std::string_view path) {
    d.tree().layoutNow();
    audio_clip::Chip* c = chipAt(path);
    REQUIRE_MESSAGE(c != nullptr, "no chip " << path);
    REQUIRE(isShown(c));
    d.click(mid(c->rect()));
    d.step();
    d.tree().layoutNow();
  }
  // The warp chip of a mode.
  void pick(std::string_view mode) {
    static constexpr const char* kPaths[][2] = {{"Off", "warp/modes-1/off"},         {"Re-Pitch", "warp/modes-1/repitch"},
                                                {"Beats", "warp/modes-1/beats"},     {"Tones", "warp/modes-1/tones"},
                                                {"Texture", "warp/modes-2/texture"}, {"Complex", "warp/modes-2/complex"},
                                                {"Complex Pro", "warp/modes-2/pro"}};
    for (const auto& p : kPaths)
      if (mode == p[0]) return chip(p[1]);
    FAIL("no warp mode " << mode);
  }
  static bool isShown(const Widget* w) {
    for (; w != nullptr; w = w->parent())
      if (!w->visible()) return false;
    return true;
  }
  // Shown: the widget and every ancestor visible (a mode's controls live in the mode card).
  bool shown(std::string_view path) {
    Widget* w = box(path);
    REQUIRE(w != nullptr);
    return isShown(w);
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

TEST_CASE("audio clip warp: the warp chips' seven modes, each mode's card, its settings one undo step each, :2 / x2") {
  Fixture f;
  static constexpr const char* kChips[] = {"warp/modes-1/off",     "warp/modes-1/repitch", "warp/modes-1/beats", "warp/modes-1/tones",
                                           "warp/modes-2/texture", "warp/modes-2/complex", "warp/modes-2/pro"};
  for (size_t i = 0; i < std::size(kChips); ++i) {
    audio_clip::Chip* c = f.chipAt(kChips[i]);
    REQUIRE(c != nullptr);
    CHECK(c->text() == kWarpModeNames[i]);
    CHECK(c->on() == (i == 0));  // Off lit
  }
  CHECK(f.clip().warp == doc::AudioWarp::Off);
  CHECK_FALSE(f.shown("mode"));  // Off has no settings: no mode card
  CHECK(f.box("clip/transpose")->enabled());

  // Beats by its chip: one step; the tempo seeded from the detection (beat.wav has no tag: its kicks, 120 BPM); its
  // card (titled Beats) with Preserve, Transient Loop and Envelope; the chip lit, Off dark.
  size_t h = f.history();
  f.pick("Beats");
  CHECK(f.clip().warp == doc::AudioWarp::Beats);
  CHECK(std::abs(f.clip().sourceBpm - 120.0) < 0.5);
  CHECK(f.history() == h + 1);
  CHECK(f.chipAt("warp/modes-1/beats")->on());
  CHECK_FALSE(f.chipAt("warp/modes-1/off")->on());
  CHECK(f.shown("mode/beats/preserve"));
  CHECK(f.shown("mode/beats/beats-loop"));
  CHECK(f.shown("mode/beats-env"));
  CHECK(dynamic_cast<audio_clip::Section*>(f.box("mode"))->title() == "Beats");
  CHECK_FALSE(f.shown("mode/grain"));
  CHECK(f.box("clip/transpose")->enabled());  // time-stretch modes transpose
  h = f.history();
  f.chip("mode/beats/preserve/transients");
  CHECK(f.clip().warpParams.beatsGranulation == 0);
  CHECK(f.chipAt("mode/beats/preserve/transients")->on());
  CHECK_FALSE(f.chipAt("mode/beats/preserve/1-16")->on());
  f.chip("mode/beats/beats-loop/back-fwd");
  CHECK(f.clip().warpParams.beatsLoop == doc::BeatsLoop::BackForward);
  CHECK(f.chipAt("mode/beats/beats-loop/back-fwd")->on());
  CHECK(f.history() == h + 2);
  f.chip("mode/beats/preserve/1-8");
  CHECK(f.clip().warpParams.beatsGranulation == kPpq / 2);
  f.chip("mode/beats/preserve/transients");
  // The envelope knob: a drag is one undo step.
  h = f.history();
  const Vec2 ec = mid(f.box("mode/beats-env")->rect());
  f.d.drag(ec, {ec.x, ec.y + 40.0f}, 8);
  CHECK(f.clip().warpParams.beatsEnvelope < 100.0f);
  CHECK(f.history() == h + 1);

  // Texture: grain and flux; Tones: grain only; Complex Pro: formants and envelope; Complex: no card; Re-Pitch greys
  // the transpose.
  f.pick("Texture");
  CHECK(f.shown("mode/grain"));
  CHECK(f.shown("mode/flux"));
  CHECK_FALSE(f.shown("mode/beats/preserve"));
  h = f.history();
  const Vec2 fc = mid(f.box("mode/flux")->rect());
  f.d.drag(fc, {fc.x, fc.y - 30.0f}, 6);
  CHECK(f.clip().warpParams.flux > 0.0f);
  CHECK(f.history() == h + 1);
  CHECK(f.clip().warpParams.beatsGranulation == 0);  // every mode's settings are kept
  f.pick("Tones");
  CHECK(f.shown("mode/grain"));
  CHECK_FALSE(f.shown("mode/flux"));
  f.pick("Complex Pro");
  CHECK(f.shown("mode/formants"));
  CHECK(f.shown("mode/env-order"));
  CHECK_FALSE(f.shown("mode/grain"));
  h = f.history();
  const Vec2 oc = mid(f.box("mode/env-order")->rect());
  f.d.drag(oc, {oc.x, oc.y + 30.0f}, 6);
  CHECK(f.clip().warpParams.envelope < 128.0f);
  CHECK(f.history() == h + 1);
  f.pick("Complex");
  CHECK(f.clip().warp == doc::AudioWarp::Complex);
  CHECK_FALSE(f.shown("mode"));
  f.pick("Re-Pitch");
  CHECK_FALSE(f.box("clip/transpose")->enabled());

  // :2 and x2: one step each, exact; Loop lights its chip.
  const double bpm = f.clip().sourceBpm;
  h = f.history();
  f.chip("warp/tempo/halve");
  CHECK(f.clip().sourceBpm == bpm / 2.0);
  f.chip("warp/tempo/double");
  f.chip("warp/tempo/double");
  CHECK(f.clip().sourceBpm == bpm * 2.0);
  CHECK(f.history() == h + 3);
  const bool looped = f.clip().loop.enabled;
  f.chip("warp/tempo/loop");
  CHECK(f.clip().loop.enabled == !looped);
  CHECK(f.chipAt("warp/tempo/loop")->on() == !looped);
  f.pick("Off");
  CHECK_FALSE(f.box("warp/tempo/halve")->enabled());
  CHECK_FALSE(f.box("warp/tempo/bpm")->enabled());
}

TEST_CASE("AC.look: the transpose knob - Up / Down one step of the clip's tuning, Shift one period, typed cents and "
          "exact ratios, the wheel, drags; each one undo step") {
  Fixture f;
  auto* knob = dynamic_cast<audio_clip::ClipKnob*>(f.box("clip/transpose"));
  REQUIRE(knob != nullptr);
  CHECK(f.clip().warp == doc::AudioWarp::Off);  // Off plays the transpose
  f.d.tree().layoutNow();
  // A click focuses it (no change, no step).
  size_t h = f.history();
  f.d.click(mid(knob->dialRect()));
  REQUIRE(knob->focused());
  CHECK(f.history() == h);

  // Up: one step of the clip's tuning (the fixture's 12-EDO: 100 c), stored as Steps. One undo step.
  CHECK(f.d.press("Up") == KeyResult::Widget);
  CHECK(f.clip().transpose.unit == doc::TransposeUnit::Steps);
  CHECK(f.clip().transpose.steps == 1);
  CHECK(knob->value() == doctest::Approx(100.0));
  CHECK(knob->readout() == "+1 stp");
  CHECK(f.history() == h + 1);
  // Shift+Up: one period (12 steps).
  f.d.press("Shift+Up");
  CHECK(f.clip().transpose.steps == 13);
  CHECK(knob->value() == doctest::Approx(1300.0));
  f.d.press("Shift+Down");
  f.d.press("Down");
  CHECK(f.clip().transpose.steps == 0);
  CHECK(f.history() == h + 4);

  // Typed while focused: "5/4" is stored as the exact ratio and reads with its true size (386.31 c).
  h = f.history();
  f.d.press("5");
  REQUIRE(knob->typingIn());
  f.d.type("/4");
  f.d.press("Enter");
  CHECK_FALSE(knob->typingIn());
  CHECK(f.clip().transpose.unit == doc::TransposeUnit::Ratio);
  CHECK(f.clip().transpose.ratio == tuning::Rational(5, 4));
  CHECK(knob->value() == doctest::Approx(386.3137).epsilon(1e-6));
  CHECK(knob->readout() == "5/4 \xC2\xB7 386.3 \xC2\xA2");
  CHECK(f.history() == h + 1);
  // Up from 5/4 (386.3 c): the next degree above, 4 steps (400 c); Down from there: 3 steps.
  f.d.press("Up");
  CHECK(f.clip().transpose.unit == doc::TransposeUnit::Steps);
  CHECK(f.clip().transpose.steps == 4);
  f.d.press("Down");
  CHECK(f.clip().transpose.steps == 3);
  // Shift+Up on a ratio keeps it exact: 5/4 -> 5/2.
  f.d.press("5");
  f.d.type("/4");
  f.d.press("Enter");
  f.d.press("Shift+Up");
  CHECK(f.clip().transpose.unit == doc::TransposeUnit::Ratio);
  CHECK(f.clip().transpose.ratio == tuning::Rational(5, 2));

  // Double-click opens the type-in with the current value selected; "386.3c" stores cents.
  h = f.history();
  f.d.doubleClick(mid(knob->dialRect()));
  REQUIRE(knob->typingIn());
  CHECK(knob->typeInBuffer().text() == "5/2");
  f.d.type("386.3c");
  f.d.press("Enter");
  CHECK(f.clip().transpose.unit == doc::TransposeUnit::Cents);
  CHECK(f.clip().transpose.cents == doctest::Approx(386.3));
  CHECK(knob->readout() == "+386.3 \xC2\xA2");
  CHECK(f.history() == h + 1);
  // Nonsense changes nothing; Esc cancels the type-in.
  h = f.history();
  f.d.press("7");
  f.d.type("x");
  f.d.press("Enter");
  CHECK(f.clip().transpose.cents == doctest::Approx(386.3));
  f.d.press("7");
  f.d.press("Escape");
  CHECK_FALSE(knob->typingIn());
  CHECK(f.history() == h);
  // Delete: back to 0.
  f.d.press("Delete");
  CHECK(knob->value() == 0.0);
  CHECK(f.history() == h + 1);

  // The wheel: one step per notch, a run of notches one undo step.
  h = f.history();
  f.d.wheel(mid(knob->dialRect()), 1.0f);
  f.d.wheel(mid(knob->dialRect()), 1.0f);
  f.d.advance(1.0);  // the run ends
  CHECK(f.clip().transpose.unit == doc::TransposeUnit::Steps);
  CHECK(f.clip().transpose.steps == 2);
  CHECK(f.history() == h + 1);

  // A drag moves in steps of the tuning (whole Steps); Shift-drag is fine, in free cents. One step each.
  h = f.history();
  const Vec2 c = mid(knob->dialRect());
  f.d.drag(c, {c.x, c.y - 40.0f}, 8);
  CHECK(f.clip().transpose.unit == doc::TransposeUnit::Steps);
  CHECK(f.clip().transpose.steps > 2);
  CHECK(f.history() == h + 1);
  const double before = knob->value();
  f.d.drag(c, {c.x, c.y - 7.0f}, 7, mod::kShift);
  CHECK(f.clip().transpose.unit == doc::TransposeUnit::Cents);
  CHECK(knob->value() > before);
  CHECK(knob->value() < before + 100.0);
  CHECK(f.history() == h + 2);
}

TEST_CASE("AC.look: a percentage knob lights its arc in proportion to its value (the fill indicator)") {
  Fixture f;
  REQUIRE(f.d.enableGpu().ok());
  f.pick("Beats");
  auto* env = dynamic_cast<audio_clip::ClipKnob*>(f.box("mode/beats-env"));
  REQUIRE(env != nullptr);
  // The pixels in the dial lit in the theme's face.value (the arc; the value text is below the dial).
  auto lit = [&](float pct) {
    doc::WarpParams p = f.clip().warpParams;
    p.beatsEnvelope = pct;
    (void)f.view().setWarpParams(p);
    CHECK(env->value() == doctest::Approx(pct));
    CHECK(env->normalized() == doctest::Approx(pct / 100.0));
    f.d.tree().layoutNow();
    auto img = f.d.render();
    REQUIRE(img.ok());
    const IRect r = env->dialRect();
    const Rgba accent = f.d.resources().theme->face.value;
    int n = 0;
    for (int32_t y = r.y; y < r.bottom(); ++y)
      for (int32_t x = r.x; x < r.right(); ++x) {
        const uint8_t* px = img->pixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
        if (std::abs(int(px[0]) - int(redOf(accent))) <= 12 && std::abs(int(px[1]) - int(greenOf(accent))) <= 12 &&
            std::abs(int(px[2]) - int(blueOf(accent))) <= 12)
          ++n;
      }
    return n;
  };
  const int full = lit(100.0f), half = lit(50.0f), quarter = lit(25.0f), none = lit(0.0f);
  MESSAGE("lit px: 100 % ", full, ", 50 % ", half, ", 25 % ", quarter, ", 0 % ", none);
  CHECK(full > 50);
  CHECK(half == doctest::Approx(0.5 * full).epsilon(0.2));
  CHECK(quarter == doctest::Approx(0.25 * full).epsilon(0.35));
  CHECK(none < full / 20);
}

TEST_CASE("audio clip warp: markers through real dispatch — add at a transient, drag, Ctrl-drag, Esc, Delete, "
          "double-click remove; each one undo step") {
  Fixture f;
  f.pick("Beats");
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
