// T5.6: audio clips in the arrangement and the audio Clip View (tests/views/fixtures/audio) — painting (waveforms,
// overlays, missing), the generic clip edits on audio clips (move / copy / cross-track / trim limits / split / delete /
// clipboard / range delete), the handles (fades, crossfade by dragging a fade past the edge, gain, double-click reset,
// Alt curve, Esc), the clip view's edits (gain, transpose entry, warp + BPM seed, reverse, start marker, loop brace),
// each one undo step; the arrangement-audio and clip-view-audio goldens; (perf) the 64 × 200 audio arrangement.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "../ui/thread_cpu.h"
#include "audio_fixture.h"
#include "pq/core/clip_envelope.h"
#include "pq/document/audio_clip.h"
#include "pq/document/audio_sequence.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/tile_atlas.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/audio_clip/audio_clip_view.h"
#include "pq/views/fake_transport.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "pq/views/waveforms.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr Ticks kBar = kTicksPerWhole;
constexpr const char* kArr = "main/body/top/track-area/arrangement";
constexpr double kPxPerBar = 300.0;  // the fixture's zoom: bars 0 … 3 in view
using Handle = ArrangementView::AudioHandle;

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d;
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  explicit Fixture(uint32_t w = 1280, uint32_t h = 800) : d(w, h) {
    REQUIRE(ctx.openProject(views::test::fixtureDir("audio")).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    arr->setTimeView(-0.05 * static_cast<double>(kBar), kPxPerBar / static_cast<double>(kBar));
    d.tree().layoutNow();
    d.step();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }

  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Track& track(std::string_view name) const {
    const doc::Track* found = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!found && t.name == name) found = &t;
    });
    REQUIRE(found != nullptr);
    return *found;
  }
  doc::ClipRef clip(std::string_view trackName, size_t i) const {
    const doc::Track& t = track(trackName);
    REQUIRE(i < t.clips.size());
    return {t.id, t.clips[i].id};
  }
  const doc::AudioClip* find(doc::ClipId id, doc::TrackId* on = nullptr) const {
    const doc::AudioClip* c = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      for (const doc::ClipSlot& s : t.clips)
        if (s.id == id && s.audio) {
          c = s.audio.get();
          if (on) *on = t.id;
        }
    });
    return c;
  }
  const doc::AudioClip& get(const doc::ClipRef& r) const {
    const doc::AudioClip* c = find(r.clip);
    REQUIRE(c != nullptr);
    return *c;
  }
  Vec2 title(const doc::ClipRef& r, Ticks dt) {
    const IRect box = arr->clipRect(r);
    return {arr->xOfTick(static_cast<double>(get(r).start + dt)) + 0.5f, static_cast<float>(box.y + 7)};
  }
  Vec2 body(const doc::ClipRef& r, Ticks dt) {
    const IRect box = arr->clipRect(r);
    return {arr->xOfTick(static_cast<double>(get(r).start + dt)) + 0.5f, static_cast<float>(box.bottom() - 6)};
  }
  Vec2 edge(const doc::ClipRef& r, bool right) {
    const IRect box = arr->clipRect(r);
    return {static_cast<float>(right ? box.right() - 2 : box.x + 1), static_cast<float>(box.bottom() - 6)};
  }
  Vec2 handle(const doc::ClipRef& r, Handle h) {
    const Rect hr = arr->audioHandleRect(r, h);
    REQUIRE(hr.x1 > hr.x0);
    return {0.5f * (hr.x0 + hr.x1), 0.5f * (hr.y0 + hr.y1)};
  }
  Vec2 lane(std::string_view rowName, Ticks t) {
    const auto rows = arr->rows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].track->name == rowName) {
        const IRect l = arr->rowLaneRect(static_cast<int32_t>(i));
        return {arr->xOfTick(static_cast<double>(t)) + 0.5f, static_cast<float>(l.bottom() - 6)};
      }
    FAIL("no row " << rowName);
    return {};
  }
  AudioClipView& view() {
    AudioClipView* v = findAudioClipView(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
};

}  // namespace

TEST_CASE("arrangement audio: clips draw waveforms (or the missing overlay); the audio clip view replaces the piano roll") {
  Fixture f;
  REQUIRE(f.d.enableGpu().ok());
  f.d.tree().invalidateAll();
  f.d.step();
  const ArrangementView::Stats& st = f.arr->stats();
  CHECK(st.clipsPainted >= 6);
  CHECK(st.tilesDrawn + st.wavesDirect >= 4);  // every audio clip with its sample has a waveform
  CHECK(f.ctx.missingSamples().size() == 1);
  // Selecting an audio clip shows it in the audio clip view; the piano roll hides.
  const doc::ClipRef beat = f.clip("Drums", 0);
  f.d.click(f.title(beat, kBar / 4));
  CHECK(f.arr->clipSelected(beat.clip));
  CHECK(f.ctx.detailClip().clip == beat.clip);
  CHECK(f.view().hasClip());
  CHECK(f.view().visible());
  auto* roll = dynamic_cast<ClipView*>(f.d.find("main/body/bottom/detail/clip/clip-view"));
  REQUIRE(roll != nullptr);
  CHECK_FALSE(roll->visible());
  // A MIDI clip: the piano roll again. (SX.6: the clip popup covers Keys' clip; Esc hides it first.)
  CHECK(f.arr->clipPopupShown());
  CHECK(f.d.press("Escape") == KeyResult::Action);
  const IRect keys = f.arr->clipRect(f.clip("Keys", 0));
  f.d.click({static_cast<float>(keys.x + 20), static_cast<float>(keys.y + 7)});
  CHECK(roll->visible());
  CHECK_FALSE(f.view().visible());
}

TEST_CASE("arrangement audio: move, Alt-copy, across audio tracks, not onto MIDI; each one undo step") {
  Fixture f;
  const doc::ClipRef rev = f.clip("Drums", 2);  // "beat rev" at bar 2.5
  const Ticks s0 = f.get(rev).start;
  size_t h = f.history();
  const Vec2 a = f.title(rev, kBar / 8);
  f.d.drag(a, {a.x + static_cast<float>(kPxPerBar / 2), a.y});  // half a bar later
  CHECK(f.get(rev).start == s0 + kBar / 2);
  CHECK(f.history() == h + 1);
  CHECK(f.ctx.undo());
  CHECK(f.get(rev).start == s0);
  // Alt-copy: the original stays.
  h = f.history();
  const size_t n0 = f.track("Drums").clips.size();
  f.d.drag(f.title(rev, kBar / 8), {f.title(rev, kBar / 8).x + static_cast<float>(kPxPerBar), a.y}, 4, mod::kAlt);
  CHECK(f.track("Drums").clips.size() == n0 + 1);
  CHECK(f.history() == h + 1);
  // Onto the Vox row (an audio track): moves there.
  const Vec2 t0 = f.title(rev, kBar / 8);
  const Vec2 voxPt = f.lane("Vox", f.get(rev).start + kBar / 8);
  h = f.history();
  f.d.drag(t0, {t0.x, voxPt.y - 20.0f});
  doc::TrackId on{};
  REQUIRE(f.find(rev.clip, &on) != nullptr);
  CHECK(on == f.track("Vox").id);
  CHECK(f.history() == h + 1);
  // Onto the Keys row (MIDI): refused, nothing changes.
  const doc::Snapshot before = f.ctx.doc();
  const doc::ClipRef moved{on, rev.clip};
  const Vec2 m0 = f.title(moved, kBar / 8);
  const Vec2 keysPt = f.lane("Keys", f.get(moved).start);
  f.d.drag(m0, {m0.x, keysPt.y - 10.0f});
  CHECK(f.ctx.doc() == before);
}

TEST_CASE("arrangement audio: trimming stops at the file's ends; split, delete, duplicate; clipboard by kind; range delete") {
  Fixture f;
  const doc::ClipRef beat = f.clip("Drums", 0);  // the whole file, 1 bar
  const doc::AudioClip b0 = f.get(beat);
  // The right edge cannot grow past the file's end; it can shrink.
  const Vec2 e = f.edge(beat, true);
  f.d.drag(e, {e.x + 150.0f, e.y});
  CHECK(f.get(beat).start + f.get(beat).length == b0.start + b0.length);
  f.d.drag(f.edge(beat, true), {f.edge(beat, true).x - 100.0f, e.y});
  CHECK(f.get(beat).length < b0.length);
  CHECK(f.ctx.undo());
  // The left edge cannot move left of the file's start either (contentStartFrame 0).
  const doc::ClipRef rev = f.clip("Drums", 2);
  const doc::AudioClip r0 = f.get(rev);
  const Vec2 l = f.edge(rev, false);
  // Reversed: the left edge shows the window's end (1.5 s of a 2 s file at +7 st): it may grow by the rest (0.5 s of
  // file = 1/3 s of clip time) but no further.
  f.d.drag(l, {l.x - 300.0f, l.y});
  const doc::AudioClip& r1 = f.get(rev);
  CHECK(r1.start < r0.start);
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), r1.sample);
  REQUIRE(s != nullptr);
  const double speed = s->rate * std::exp2(700.0 / 1200.0);
  CHECK(r1.contentStartFrame + r1.seconds * speed <= static_cast<double>(s->frames) + 1.0);
  CHECK(r1.contentStartFrame >= -1e-6);
  CHECK(f.ctx.undo());

  // Split at the insert marker (Ctrl+E): two pieces playing what the clip played.
  f.d.click(f.body(beat, kBar / 2));
  REQUIRE(f.arr->selection().time.has_value());
  const size_t n0 = f.track("Drums").clips.size();
  size_t h = f.history();
  CHECK(f.d.press("Ctrl+E") == KeyResult::Action);
  CHECK(f.track("Drums").clips.size() == n0 + 1);
  CHECK(f.history() == h + 1);
  // Delete the first piece, duplicate the second.
  const doc::ClipRef p0 = f.clip("Drums", 0), p1 = f.clip("Drums", 1);
  f.d.click(f.title(p0, 100));
  CHECK(f.d.press("Delete") == KeyResult::Action);
  CHECK(f.find(p0.clip) == nullptr);
  f.d.click(f.title(p1, 100));
  h = f.history();
  CHECK(f.d.press("Ctrl+D") == KeyResult::Action);
  CHECK(f.history() == h + 1);

  // Copy an audio clip; paste on the Keys (MIDI) row: dropped; on Vox: pasted.
  f.d.click(f.title(f.clip("Drums", 0), 100));
  CHECK(f.d.press("Ctrl+C") == KeyResult::Action);
  f.d.click(f.lane("Keys", 2 * kBar));
  const doc::Snapshot before = f.ctx.doc();
  (void)f.d.press("Ctrl+V");
  CHECK(f.ctx.doc() == before);
  CHECK(f.ctx.status().find("kind") != std::string::npos);
  f.d.click(f.lane("Vox", 2 * kBar + kBar / 2));
  const size_t v0 = f.track("Vox").clips.size();
  CHECK(f.d.press("Ctrl+V") == KeyResult::Action);
  CHECK(f.track("Vox").clips.size() == v0 + 1);

  // A range over the Drums row, deleted: clips cut at its edges, nothing inside.
  const Vec2 r0p = f.lane("Drums", kBar / 4), r1p = f.lane("Drums", 3 * kBar / 2);
  f.d.drag({r0p.x, r0p.y + 2.0f}, {r1p.x, r1p.y + 2.0f});
  REQUIRE(f.arr->selection().range);
  h = f.history();
  CHECK(f.d.press("Delete") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  for (const doc::ClipSlot& c : f.track("Drums").clips) CHECK((c.end() <= kBar / 4 || c.start() >= 3 * kBar / 2));
}

TEST_CASE("arrangement audio: fade handles (length, Alt curve, Esc), crossfade past the edge, gain handle; one undo step each") {
  Fixture f;
  const doc::ClipRef beat = f.clip("Drums", 0), beat2 = f.clip("Drums", 1);
  f.d.click(f.title(beat, kBar / 4));  // selected: handles shown
  // Fade-in: 60 px right = 60 / 300 bar longer.
  const doc::AudioClip c0 = f.get(beat);
  size_t h = f.history();
  const Vec2 fi = f.handle(beat, Handle::FadeIn);
  f.d.move(fi);
  CHECK(f.d.tree().cursor() == Cursor::ResizeEW);
  f.d.drag(fi, {fi.x + 60.0f, fi.y}, 6);
  const Ticks want = c0.fadeIn.length + static_cast<Ticks>(60.0 / kPxPerBar * static_cast<double>(kBar));
  CHECK(std::abs(f.get(beat).fadeIn.length - want) < kBar / 200);
  CHECK(f.history() == h + 1);
  // Alt-drag: the curve (up = positive for a fade-in).
  const Vec2 fi2 = f.handle(beat, Handle::FadeIn);
  f.d.drag(fi2, {fi2.x, fi2.y - 40.0f}, 4, mod::kAlt);
  CHECK(f.get(beat).fadeIn.curve > 0.2f);
  CHECK(f.history() == h + 2);
  // Esc during a drag: back to before it, nothing recorded.
  const doc::Snapshot before = f.ctx.doc();
  const Vec2 fo = f.handle(beat, Handle::FadeOut);
  f.d.move(fo);
  f.d.down(fo);
  f.d.move({fo.x - 50.0f, fo.y});
  CHECK(f.ctx.store().inGesture());
  CHECK(f.d.press("Escape") != KeyResult::Unhandled);
  CHECK_FALSE(f.ctx.store().inGesture());
  f.d.up({fo.x - 50.0f, fo.y});
  CHECK(f.ctx.doc() == before);
  CHECK(f.history() == h + 2);

  // The second clip's crossfade removed, then the first clip's fade-out dragged past its end onto it: a crossfade.
  {
    auto e = doc::ops::setAudioClipCrossfade(*f.ctx.doc(), beat2, 0);
    REQUIRE(e.ok());
    REQUIRE(f.ctx.store().dispatch(e->command).ok());
  }
  f.d.click(f.title(beat, kBar / 4));
  h = f.history();
  const Vec2 fo2 = f.handle(beat, Handle::FadeOut);
  const float endX = f.arr->xOfTick(static_cast<double>(kBar));
  f.d.drag(fo2, {endX + 25.0f, fo2.y}, 6);
  const Ticks x = f.get(beat2).crossfadeIn;
  CHECK(std::abs(x - static_cast<Ticks>(2.0 * 25.0 / kPxPerBar * static_cast<double>(kBar))) < kBar / 100);
  CHECK(f.get(beat).fadeOut.length == 0);
  CHECK(f.history() == h + 1);

  // Gain: 30 px up = +6 dB (0.2 dB per px); Shift: fine; double-click: 0 dB.
  h = f.history();
  const float g0 = f.get(beat).gainDb;
  const Vec2 g = f.handle(beat, Handle::Gain);
  f.d.move(g);
  CHECK(f.d.tree().cursor() == Cursor::ResizeNS);
  f.d.drag(g, {g.x, g.y - 30.0f}, 5);
  CHECK(f.get(beat).gainDb == doctest::Approx(g0 + 6.0f).epsilon(0.02));
  CHECK(f.history() == h + 1);
  const Vec2 g2 = f.handle(beat, Handle::Gain);
  f.d.drag(g2, {g2.x, g2.y - 30.0f}, 5, mod::kShift);
  CHECK(f.get(beat).gainDb == doctest::Approx(g0 + 6.6f).epsilon(0.02));
  f.d.doubleClick(f.handle(beat, Handle::Gain));
  CHECK(f.get(beat).gainDb == 0.0f);
  CHECK(f.history() == h + 3);
}

TEST_CASE("audio clip view: transpose entry, gain, warp (from its shape), reverse, start marker and loop brace drags") {
  Fixture f(1280, 900);
  if (auto* body = dynamic_cast<Splitter*>(f.d.find("main/body"))) body->setPosition(420.0f);
  const doc::ClipRef vox = f.clip("Vox", 0), rev = f.clip("Drums", 2);
  f.ctx.setDetailVisible(true);
  f.ctx.setDetailClip(vox);
  f.d.tree().layoutNow();
  AudioClipView& v = f.view();
  REQUIRE(v.hasClip());
  CHECK(v.clip()->warp == doc::AudioWarp::RePitch);
  CHECK_FALSE(v.box().find("props/clip/transpose")->enabled());  // greyed in Re-Pitch (Q145)

  // Transpose parsing.
  CHECK(parseTranspose("7")->unit == doc::TransposeUnit::Steps);
  CHECK(parseTranspose("7")->steps == 7);
  CHECK(parseTranspose("-700c")->cents == -700.0);
  CHECK(parseTranspose("+7st")->cents == 700.0);
  CHECK(parseTranspose("3/2")->ratio == tuning::Rational(3, 2));
  CHECK_FALSE(parseTranspose("abc").has_value());
  CHECK_FALSE(parseTranspose("0/2").has_value());
  CHECK(formatTranspose(*parseTranspose("3/2")) == "3/2");
  // M6 review L19: cents read as semitones when whole ("0 st", not "0c"), else with the cent sign; both parse back.
  CHECK(formatTranspose(doc::AudioTranspose{}) == "0 st");
  CHECK(formatTranspose(*parseTranspose("-700c")) == "-7 st");
  CHECK(formatTranspose(*parseTranspose("386.3c")) == "386.3 \xC2\xA2");
  for (const char* t : {"0 st", "-7 st", "386.3 \xC2\xA2"}) CHECK(formatTranspose(*parseTranspose(t)) == t);
  // AC.look: the transpose knob's readout (a ratio with its true size).
  CHECK(transposeReadout(*parseTranspose("5/4"), tuning::cents(tuning::Rational(5, 4))) == "5/4 \xC2\xB7 386.3 \xC2\xA2");
  CHECK(transposeReadout(*parseTranspose("7"), 700.0) == "+7 stp");
  CHECK(transposeReadout(*parseTranspose("-700c"), -700.0) == "-7 st");
  CHECK(transposeReadout(*parseTranspose("386.3c"), 386.3) == "+386.3 \xC2\xA2");
  CHECK(transposeReadout(doc::AudioTranspose{}, 0.0) == "0 st");
  // M6 review M11: Warp as N-Bar Loop's count in the clip's signature, any whole number with a detected tempo.
  CHECK(loopBarsFor(4.0, 0.0) == 2);             // no detection: the power of two nearest 120 BPM (2 bars of 4/4)
  CHECK(loopBarsFor(6.0, 120.0) == 3);           // 3 bars of 4/4 at 120: no longer forced to 2 or 4
  CHECK(loopBarsFor(6.0, 90.0, 3.0) == 3);       // 3/4 at 90 BPM: 3 bars
  CHECK(loopBarsFor(3.5, 120.0, 3.5) == 2);      // 7/8 at 120: two bars of 3.5 quarters
  CHECK(loopBarsFor(6.0, 2000.0) <= 1024);

  size_t h = f.history();
  CHECK(v.setWarp(doc::AudioWarp::Off));
  CHECK(v.clip()->warp == doc::AudioWarp::Off);
  CHECK(v.box().find("props/clip/transpose")->enabled());
  CHECK(v.setTranspose("3/2"));
  CHECK(v.clip()->transpose.unit == doc::TransposeUnit::Ratio);
  CHECK_FALSE(v.setTranspose("nonsense"));
  CHECK(v.setGain(-3.0f));
  CHECK(v.clip()->gainDb == -3.0f);
  CHECK(v.setReverse(true));
  CHECK(v.clip()->reverse);
  CHECK(f.history() == h + 4);
  // WB: warped from the shape it has: the Off clip played 3/2 fast (its transpose is a varispeed), so Re-Pitch reads
  // the source at the same speed (the source BPM = the tempo / 1.5); the same span, start and source frame.
  const doc::AudioClip off = f.get(vox);
  const double tempo = f.ctx.doc()->timeline->tempoMap.bpmAt(off.start);
  CHECK(v.setWarp(doc::AudioWarp::RePitch));
  CHECK(v.clip()->sourceBpm == doctest::Approx(tempo / 1.5));
  CHECK(v.clip()->start == off.start);
  CHECK(v.clip()->length == off.length);
  CHECK(f.history() == h + 5);

  // The gain knob: a drag is one undo step.
  h = f.history();
  const IRect gb = f.d.rect(std::string("main/body/bottom/detail/clip/audio-clip-view/audio-box/props/clip/gain"));
  const Vec2 gc{static_cast<float>(gb.x + gb.w / 2), static_cast<float>(gb.y + gb.h / 2)};
  f.d.drag(gc, {gc.x, gc.y - 40.0f}, 8);
  CHECK(f.get(vox).gainDb > -3.0f);
  CHECK(f.history() == h + 1);

  // The loop brace's end, dragged left: the loop shrinks (1/16 notes), one undo step. (Switching warps left the loop
  // off: on again first. WB: warped from its shape the clip plays the whole file, so its end is the file's: no room
  // to the right.)
  CHECK(v.setLoopEnabled(true));
  h = f.history();
  const doc::AudioClip& a = f.get(vox);
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), a.sample);
  REQUIRE(s != nullptr);
  const double fpt = doc::framesPerContentTick(s->rate, a.sourceBpm);
  const Ticks end0 = a.loop.end;
  const IRect ru = v.rulerRect();
  const float ex = std::min(v.xOfFrame(static_cast<double>(end0) * fpt), static_cast<float>(ru.right() - 2));  // at the file's end: the ruler's edge
  f.d.drag({ex, static_cast<float>(ru.y + ru.h / 2)}, {ex - 60.0f, static_cast<float>(ru.y + ru.h / 2)}, 6);
  CHECK(f.get(vox).loop.end < end0);
  CHECK(f.get(vox).loop.end % (kPpq / 4) == 0);
  CHECK(f.history() == h + 1);

  // The start marker of a warp-Off clip, dragged right: the source window moves, the clip stays where it is.
  f.ctx.setDetailClip(rev);
  f.d.tree().layoutNow();
  REQUIRE(v.clip() != nullptr);
  const doc::AudioClip r0 = f.get(rev);
  h = f.history();
  const float mx = v.xOfFrame(v.startMarkerFrame());
  f.d.drag({mx, static_cast<float>(ru.y + 3)}, {mx + 40.0f, static_cast<float>(ru.y + 3)}, 5);
  CHECK(f.get(rev).contentStartFrame > r0.contentStartFrame);
  CHECK(f.get(rev).start == r0.start);
  CHECK(f.get(rev).length == r0.length);
  CHECK(f.history() == h + 1);

  // M5 review M8: Esc during a marker drag cancels it (the clip back as it was, no undo step, capture released).
  const doc::AudioClip r1 = f.get(rev);
  h = f.history();
  const float mx2 = v.xOfFrame(v.startMarkerFrame());
  f.d.move({mx2, static_cast<float>(ru.y + 3)});
  f.d.down({mx2, static_cast<float>(ru.y + 3)});
  f.d.move({mx2 + 25.0f, static_cast<float>(ru.y + 3)});
  f.d.move({mx2 + 50.0f, static_cast<float>(ru.y + 3)});
  CHECK(v.dragging());
  CHECK(f.get(rev).contentStartFrame != r1.contentStartFrame);
  CHECK(f.d.press("Escape") == KeyResult::Widget);
  CHECK_FALSE(v.dragging());
  CHECK(f.get(rev).contentStartFrame == r1.contentStartFrame);
  CHECK(f.history() == h);
  CHECK_FALSE(f.d.tree().captured());
  f.d.up({mx2 + 50.0f, static_cast<float>(ru.y + 3)});
  CHECK(f.history() == h);
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

namespace {

// A real range drag over the lanes, from row `from` at tick a to row `to` at tick b (the clip bodies: a time selection).
void rangeDrag(Fixture& f, std::string_view from, Ticks a, std::string_view to, Ticks b) {
  const Vec2 p0 = f.lane(from, a), p1 = f.lane(to, b);
  f.d.drag({p0.x, p0.y + 2.0f}, {p1.x, p1.y + 2.0f}, 6);
  REQUIRE(f.arr->selection().range);
  REQUIRE(f.arr->selection().time.has_value());
  REQUIRE(f.arr->selection().time->start == a);
  REQUIRE(f.arr->selection().time->end == b);
}

// The track's clips as [start, end, active, reverse] (reverse false for MIDI clips).
struct Piece {
  Ticks start, end;
  bool active, reverse;
  friend bool operator==(const Piece&, const Piece&) = default;
};
std::vector<Piece> pieces(const Fixture& f, std::string_view track) {
  std::vector<Piece> out;
  for (const doc::ClipSlot& c : f.track(track).clips) out.push_back({c.start(), c.end(), c.active(), c.audio && c.audio->reverse});
  return out;
}

}  // namespace

// BR.E (bug 2026-11-19_20-32-35): `0` with a time range over part of a clip deactivated the whole clip. Now the clips
// crossing the range's edges are split there and only the part inside changes — audio and MIDI, across tracks; one undo
// step with the cuts.
TEST_CASE("arrangement audio (BR.E): 0 over a partial range splits at its edges and deactivates only the part inside") {
  Fixture f;
  const doc::AudioClip whole = f.get(f.clip("Drums", 0));
  rangeDrag(f, "Drums", kBar / 4, "Drums", kBar / 2);
  size_t h = f.history();
  CHECK(f.d.press("0") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(pieces(f, "Drums") == std::vector<Piece>{{0, kBar / 4, true, false},
                                                 {kBar / 4, kBar / 2, false, false},
                                                 {kBar / 2, kBar, true, false},
                                                 {kBar, 2 * kBar, true, false},
                                                 {5 * kBar / 2, 3 * kBar, true, true}});
  // The deactivated piece plays (would play) the frames the clip played there: a quarter bar = 0.5 s into the file.
  const doc::AudioClip& mid = f.get(f.clip("Drums", 1));
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), mid.sample);
  REQUIRE(s != nullptr);
  CHECK(mid.contentStartFrame == doctest::Approx(0.5 * s->rate));
  CHECK(mid.seconds == doctest::Approx(0.5));
  // The range still selects what it covers: the piece. `0` again reactivates it (nothing more is cut).
  CHECK(f.arr->selection().clips.size() == 1);
  CHECK(f.d.press("0") == KeyResult::Action);
  CHECK(f.track("Drums").clips.size() == 5);
  CHECK(f.track("Drums").clips[1].active());
  // One undo step each: back to the whole clip.
  CHECK(f.ctx.undo());
  CHECK(f.ctx.undo());
  CHECK(f.track("Drums").clips.size() == 3);
  CHECK(f.get(f.clip("Drums", 0)) == whole);

  // Across tracks (Drums, Vox — Re-Pitch —, Keys — MIDI —) and across a clip boundary: every clip cut at the edges.
  rangeDrag(f, "Drums", 3 * kBar / 4, "Keys", 5 * kBar / 4);
  h = f.history();
  CHECK(f.d.press("0") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(pieces(f, "Drums") == std::vector<Piece>{{0, 3 * kBar / 4, true, false},
                                                 {3 * kBar / 4, kBar, false, false},
                                                 {kBar, 5 * kBar / 4, false, false},
                                                 {5 * kBar / 4, 2 * kBar, true, false},
                                                 {5 * kBar / 2, 3 * kBar, true, true}});
  CHECK(pieces(f, "Vox") == std::vector<Piece>{{kBar / 2, 3 * kBar / 4, true, false},
                                               {3 * kBar / 4, 5 * kBar / 4, false, false},
                                               {5 * kBar / 4, 5 * kBar / 2, true, false}});
  CHECK(pieces(f, "Keys") == std::vector<Piece>{{0, 3 * kBar / 4, true, false}, {3 * kBar / 4, kBar, false, false}});
  CHECK(f.ctx.undo());
  CHECK(f.track("Vox").clips.size() == 1);
  CHECK(f.track("Keys").clips.size() == 1);

  // Whole clips selected (a click, no range): the whole clip, as before.
  if (f.arr->clipPopupShown()) CHECK(f.d.press("Escape") == KeyResult::Action);  // SX.6: the first Esc hides the clip popup
  CHECK(f.d.press("Escape") == KeyResult::Action);  // (a click on a clip inside the range would keep the range)
  f.d.click(f.title(f.clip("Drums", 1), kBar / 8));
  REQUIRE_FALSE(f.arr->selection().range);
  CHECK(f.d.press("0") == KeyResult::Action);
  CHECK(f.track("Drums").clips.size() == 3);
  CHECK_FALSE(f.track("Drums").clips[1].active());
}

// BR.E (bug 2026-11-19_20-34-17): `R` reverses the selected audio — whole clips, or with a time range only the part
// inside it (split at its edges first); a reversed clip plays forwards again; MIDI clips are left alone. One undo step.
TEST_CASE("arrangement audio (BR.E): R reverses the selected audio clips, or only the part a range covers") {
  Fixture f;
  // Whole clips: the reversed one plays forwards, the forward one backwards.
  const doc::ClipRef rev = f.clip("Drums", 2), beat2 = f.clip("Drums", 1);
  f.d.click(f.title(rev, kBar / 8));
  f.d.click(f.title(beat2, kBar / 8), mod::kShift);
  REQUIRE(f.arr->selection().clips.size() == 2);
  size_t h = f.history();
  CHECK(f.d.press("R") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK_FALSE(f.get(rev).reverse);
  CHECK(f.get(beat2).reverse);
  CHECK(f.ctx.undo());
  CHECK(f.get(rev).reverse);

  // A range over part of the first clip and the MIDI clip under it: only the audio part inside, reversed.
  const doc::AudioClip whole = f.get(f.clip("Drums", 0));
  rangeDrag(f, "Drums", kBar / 4, "Keys", kBar / 2);
  h = f.history();
  CHECK(f.d.press("R") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(pieces(f, "Drums") == std::vector<Piece>{{0, kBar / 4, true, false},
                                                 {kBar / 4, kBar / 2, true, true},
                                                 {kBar / 2, kBar, true, false},
                                                 {kBar, 2 * kBar, true, false},
                                                 {5 * kBar / 2, 3 * kBar, true, true}});
  CHECK(f.track("Keys").clips.size() == 1);  // not cut
  CHECK(pieces(f, "Vox").size() == 1);       // between the two, but its clip starts after the range
  // The reversed piece reads its own window (0.5 s … 1 s of the file) backwards: the same frames, mirrored in time.
  const doc::AudioClip& mid = f.get(f.clip("Drums", 1));
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), mid.sample);
  REQUIRE(s != nullptr);
  CHECK(mid.contentStartFrame == doctest::Approx(0.5 * s->rate));
  CHECK(mid.seconds == doctest::Approx(0.5));
  CHECK(f.ctx.undo());
  CHECK(f.get(f.clip("Drums", 0)) == whole);

  // Only MIDI selected: nothing to reverse, the status says why, no undo step.
  CHECK(f.d.press("Escape") == KeyResult::Action);
  const IRect keys = f.arr->clipRect(f.clip("Keys", 0));
  f.d.click({static_cast<float>(keys.x + 20), static_cast<float>(keys.y + 7)});
  REQUIRE(f.arr->selection().clips.size() == 1);
  h = f.history();
  (void)f.d.press("R");
  CHECK(f.history() == h);
  CHECK(f.ctx.status().find("Reverse") != std::string::npos);
}

// BR.E audit: the arrangement's clip keys on audio clips with a partial range — each acts on exactly the range, every
// piece playing what the clip played there (split Ctrl+E at both edges, copy / paste, duplicate, cut), and the clip
// keys on whole audio clips (nudge Alt+Right, Up onto the next audio track). A double-click on an audio clip keeps the
// keys in the arrangement (it used to focus the hidden piano roll: Ctrl+E, 0, R did nothing).
TEST_CASE("arrangement audio (BR.E): clip keys on audio clips and partial ranges — split, copy / paste, duplicate, cut, nudge, track move") {
  Fixture f;
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), f.get(f.clip("Drums", 0)).sample);
  REQUIRE(s != nullptr);
  const double rate = s->rate;
  // Ctrl+E with a range: cut at both edges.
  rangeDrag(f, "Drums", kBar / 4, "Drums", kBar / 2);
  size_t h = f.history();
  CHECK(f.d.press("Ctrl+E") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.track("Drums").clips.size() == 5);
  CHECK(f.get(f.clip("Drums", 2)).contentStartFrame == doctest::Approx(1.0 * rate));
  CHECK(f.ctx.undo());

  // Ctrl+C over the range, Ctrl+V in the gap after bar 2: the piece plays 0.5 s … 1 s of the file.
  rangeDrag(f, "Drums", kBar / 4, "Drums", kBar / 2);
  CHECK(f.d.press("Ctrl+C") == KeyResult::Action);
  CHECK(f.track("Drums").clips.size() == 3);  // copying cuts nothing
  const Ticks at = 2 * kBar + kBar / 8;
  f.d.click(f.lane("Drums", at));
  REQUIRE(f.arr->selection().time.has_value());
  REQUIRE(f.arr->selection().time->start == at);
  CHECK(f.d.press("Ctrl+V") == KeyResult::Action);
  REQUIRE(f.track("Drums").clips.size() == 4);
  const doc::ClipSlot& pasted = f.track("Drums").clips[2];
  REQUIRE(pasted.audio != nullptr);
  CHECK(pasted.start() == at);
  CHECK(pasted.audio->contentStartFrame == doctest::Approx(0.5 * rate));
  CHECK(pasted.audio->seconds == doctest::Approx(0.5));
  CHECK(f.ctx.undo());

  // Ctrl+D over the range: the part right after it (overwriting what lay there), the range moved along.
  rangeDrag(f, "Drums", kBar / 4, "Drums", kBar / 2);
  h = f.history();
  CHECK(f.d.press("Ctrl+D") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.arr->selection().time->start == kBar / 2);
  bool found = false;
  for (const doc::ClipSlot& c : f.track("Drums").clips)
    if (c.start() == kBar / 2 && c.end() == 3 * kBar / 4) {
      found = true;
      CHECK(c.audio->contentStartFrame == doctest::Approx(0.5 * rate));
    }
  CHECK(found);
  CHECK(f.ctx.undo());

  // Ctrl+X over the range: the part removed, the rest stays where it played.
  rangeDrag(f, "Drums", kBar / 4, "Drums", kBar / 2);
  CHECK(f.d.press("Ctrl+X") == KeyResult::Action);
  for (const doc::ClipSlot& c : f.track("Drums").clips) CHECK((c.end() <= kBar / 4 || c.start() >= kBar / 2));
  CHECK(f.get(f.clip("Drums", 1)).contentStartFrame == doctest::Approx(1.0 * rate));
  CHECK(f.ctx.undo());

  // Whole audio clips: Alt+Right nudges off the grid, Up moves onto the audio track above (Drums ← Vox).
  const doc::ClipRef rev = f.clip("Drums", 2);
  const Ticks r0 = f.get(rev).start;
  f.d.click(f.title(rev, kBar / 8));
  CHECK(f.d.press("Alt+Right") == KeyResult::Action);
  CHECK(f.get(rev).start > r0);
  CHECK(f.ctx.undo());
  const doc::ClipRef vox = f.clip("Vox", 0);
  f.d.click(f.title(vox, kBar / 8));
  CHECK(f.d.press("Up") == KeyResult::Action);
  doc::TrackId on{};
  REQUIRE(f.find(vox.clip, &on) != nullptr);
  CHECK(on == f.track("Drums").id);
  CHECK(f.ctx.undo());

  // A double-click on an audio clip opens its view and leaves the keys with the arrangement: R reverses it.
  f.d.doubleClick(f.title(rev, kBar / 8));
  CHECK(f.view().visible());
  const Widget* focus = f.d.tree().focus();
  REQUIRE(focus != nullptr);
  CHECK((focus == f.arr || f.arr->isAncestorOf(*focus)));
  CHECK(f.d.press("R") == KeyResult::Action);
  CHECK_FALSE(f.get(rev).reverse);
}

TEST_CASE("render: arrangement-audio (waveforms, fades, crossfade, gain, reverse, missing, MIDI between) matches the golden") {
  checkGolden("arrangement-audio");
}

TEST_CASE("render: clip-view-audio (the audio clip view on a Re-Pitch clip) matches the golden") { checkGolden("clip-view-audio"); }

TEST_CASE("render: arrangement-audio-warp (SX.3: warped tags, a marker clip, a corner mark, fade curves on the waveform) "
          "matches the golden") {
  checkGolden("arrangement-audio-warp");
}

// ---- SX.3: the waveform shows what plays ----------------------------------------------------------------------------

namespace {

// The gain the clip player applies at tick t to lowered clip i of `tr` (doc::lowerAudioTrack: its clamped fades and
// crossfade), from the shared curves (pq/core/clip_envelope.h): the crossfade windows centred on the boundary, equal
// power unless `linear`, replacing the fade on their side; elsewhere the fade or, where it is shorter than 2 ms, the
// linear edge declick.
double loweredGain(const doc::AudioTrackDesc& tr, size_t i, const TempoMap& tm, double t, bool linear) {
  const doc::AudioClipDesc& d = tr.clips[i];
  const double s0 = static_cast<double>(d.start), s1 = static_cast<double>(d.start + d.length);
  const double dIn = d.declick ? tm.secondsToTicks(tm.ticksToSeconds(d.start) + kClipEdgeDeclickSeconds) - s0 : 0.0;
  const double dOut = d.declick ? s1 - tm.secondsToTicks(tm.ticksToSeconds(d.start + d.length) - kClipEdgeDeclickSeconds) : 0.0;
  double g = d.gain;
  if (d.crossfadeIn > 0) {
    const double a = s0 - static_cast<double>(d.crossfadeIn / 2), b = a + static_cast<double>(d.crossfadeIn);
    if (t >= a && t < b) g *= clipCrossfadeInShape((t - a) / (b - a), linear);
  } else {
    const bool declick = static_cast<double>(d.fadeIn) < dIn;
    const double f = declick ? dIn : static_cast<double>(d.fadeIn);
    if (t < s0 + f) g *= clipFadeInShape((t - s0) / f, declick ? 0.0 : d.fadeInCurve);
  }
  if (i + 1 < tr.clips.size() && tr.clips[i + 1].crossfadeIn > 0) {
    const doc::AudioClipDesc& n = tr.clips[i + 1];
    const double a = static_cast<double>(n.start) - static_cast<double>(n.crossfadeIn / 2), b = a + static_cast<double>(n.crossfadeIn);
    if (t >= a && t < b) g *= clipCrossfadeOutShape((t - a) / (b - a), linear);
  } else {
    const bool declick = static_cast<double>(d.fadeOut) < dOut;
    const double f = declick ? dOut : static_cast<double>(d.fadeOut);
    if (t >= s1 - f) g *= clipFadeOutShape((t - (s1 - f)) / f, declick ? 0.0 : d.fadeOutCurve);
  }
  return g;
}

// Every clip of track `name` drawn with the lowered gain: 400 ticks across each clip plus its edges' neighbourhoods.
void checkEnvelopes(Fixture& f, std::string_view name, bool linear) {
  const doc::Track& t = f.track(name);
  const std::optional<doc::TrackPath> path = doc::findTrack(*f.ctx.doc(), t.id);
  REQUIRE(path.has_value());
  const doc::AudioTrackDesc tr = doc::lowerAudioTrack(*f.ctx.doc(), *path);
  REQUIRE(tr.clips.size() == t.clips.size());
  const TempoMap& tm = f.ctx.doc()->timeline->tempoMap;
  f.d.step();  // (the view shows this snapshot)
  for (size_t i = 0; i < tr.clips.size(); ++i) {
    const doc::AudioClipDesc& d = tr.clips[i];
    const doc::ClipRef ref{t.id, t.clips[i].id};
    std::vector<double> ts;
    for (int k = 0; k < 400; ++k) ts.push_back(static_cast<double>(d.start) + static_cast<double>(d.length) * (k + 0.37) / 400.0);
    for (const double e : {1.0, 300.0, 900.0, 5000.0, 30000.0, 60000.0}) {
      ts.push_back(static_cast<double>(d.start) + e);
      ts.push_back(static_cast<double>(d.start + d.length) - e);
    }
    for (const double x : ts) {
      CAPTURE(i);
      CAPTURE(x);
      CHECK(f.arr->audioEnvelopeAt(ref, x) == doctest::Approx(loweredGain(tr, i, tm, x, linear)).epsilon(1e-6));
    }
  }
}

}  // namespace

TEST_CASE("arrangement audio (SX.3): the waveform is drawn with the lowered clip's gain law — fades (curves -1 / 0 / 1), "
          "the edge declick, crossfades (equal power; equal gain across a split)") {
  Fixture f;
  const doc::ClipRef beat = f.clip("Drums", 0), beat2 = f.clip("Drums", 1);
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE(e.ok());
    REQUIRE(f.ctx.store().dispatch(e->command).ok());
  };
  // The fixture: a fade-in, a fade-out the crossfade into the -6 dB second clip replaces, a reversed clip (declicks).
  checkEnvelopes(f, "Drums", false);
  CHECK(f.arr->audioEnvelopeAt(beat2, static_cast<double>(f.get(beat2).start)) ==
        doctest::Approx(doc::audioClipGain(-6.0f) * std::sin(0.25 * std::numbers::pi)));
  // Without the crossfade the fade-out plays; each curve.
  run(doc::ops::setAudioClipCrossfade(*f.ctx.doc(), beat2, 0));
  for (const float curve : {-1.0f, 0.0f, 1.0f}) {
    run(doc::ops::setAudioClipFades(*f.ctx.doc(), beat, {kBar / 4, curve}, {kBar / 3, curve}));
    CAPTURE(curve);
    checkEnvelopes(f, "Drums", false);
  }
  // Half way through a linear fade-in: half the gain.
  run(doc::ops::setAudioClipFades(*f.ctx.doc(), beat, {kBar / 4, 0.0f}, {}));
  f.d.step();
  CHECK(f.arr->audioEnvelopeAt(beat, static_cast<double>(kBar / 8)) == doctest::Approx(0.5));
  // A split crossfaded back together continues one source law: equal gain (half each at the boundary, not 0.707).
  const Ticks at = kBar / 2;
  run(doc::ops::splitClips(*f.ctx.doc(), f.ctx.store().ids(), std::span<const doc::ClipRef>(&beat, 1), std::span<const Ticks>(&at, 1)));
  const doc::ClipRef right = f.clip("Drums", 1);
  REQUIRE(f.get(right).start == at);
  run(doc::ops::setAudioClipCrossfade(*f.ctx.doc(), right, kBar / 8));
  checkEnvelopes(f, "Drums", true);
  f.d.step();
  CHECK(f.arr->audioEnvelopeAt(right, static_cast<double>(at)) == doctest::Approx(0.5));
  CHECK(f.arr->audioEnvelopeAt(beat, static_cast<double>(at) - 1e-3) == doctest::Approx(0.5).epsilon(1e-6));
}

TEST_CASE("arrangement audio (SX.3): a warped clip's waveform follows its warp markers, as the clip view maps them") {
  Fixture f;
  const doc::ClipRef beat = f.clip("Drums", 0);
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE(e.ok());
    REQUIRE(f.ctx.store().dispatch(e->command).ok());
  };
  run(doc::ops::setAudioClipWarp(*f.ctx.doc(), std::span<const doc::ClipRef>(&beat, 1), doc::AudioWarp::ComplexPro, 120.0));
  const Ticks c0 = f.get(beat).contentStart;
  for (const Ticks t : {c0 + kPpq, c0 + 2 * kPpq, c0 + 3 * kPpq}) run(doc::ops::addWarpMarker(*f.ctx.doc(), beat, t));
  run(doc::ops::moveWarpMarker(*f.ctx.doc(), beat, 1, c0 + 2 * kPpq + kPpq / 2));
  f.d.step();
  const doc::AudioClip& c = f.get(beat);
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), c.sample);
  REQUIRE(s != nullptr);
  const double fpt = doc::framesPerContentTick(s->rate, c.sourceBpm);
  double bent = 0.0;
  for (int k = 0; k < 200; ++k) {
    const double t = static_cast<double>(c.start) + static_cast<double>(c.length) * (k + 0.5) / 200.0;
    const double content = static_cast<double>(c.contentStart) + (t - static_cast<double>(c.start));
    // The clip view's mapping (AudioClipView::frameOf): doc::warpFrameAt through the markers.
    const double want = doc::warpFrameAt(c, s->rate, content);
    CAPTURE(t);
    CHECK(f.arr->audioDrawnFrameAt(beat, t) == doctest::Approx(want).epsilon(1e-9));
    bent = std::max(bent, std::abs(want - content * fpt));
  }
  CHECK(bent > 1000.0);  // the markers moved the audio (the one-slope mapping was off by this much)
}

// ---- WF.1: zoom to the samples ---------------------------------------------------------------------------------------

TEST_CASE("audio clip view (WF.1, real dispatch): the wheel zooms about the pointer down to single samples (the body gives "
          "way to the signal: the curve, a dot per sample), Shift + wheel scrolls, the time ruler drags the view and a "
          "double-click on it shows the whole sample; nothing is an undo step") {
  Fixture f(1280, 900);
  if (auto* body = dynamic_cast<Splitter*>(f.d.find("main/body"))) body->setPosition(420.0f);
  const doc::ClipRef rev = f.clip("Drums", 2);  // warp Off: the file's frames on the axis
  f.ctx.setDetailVisible(true);
  f.ctx.setDetailClip(rev);
  f.d.tree().layoutNow();
  REQUIRE(f.d.enableGpu().ok());
  f.d.tree().invalidateAll();
  f.d.step();
  AudioClipView& v = f.view();
  REQUIRE(v.hasClip());
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), v.clip()->sample);
  REQUIRE(s != nullptr);
  const auto frames = static_cast<double>(s->frames);
  const IRect w = v.sampleRect();
  REQUIRE(w.w > 200);
  AudioClipView::SampleView sv = v.sampleView();
  CHECK(sv.z0 == 0.0);
  CHECK(sv.z1 == 1.0);
  CHECK(sv.signalMix == 0.0f);  // the whole file: the peak / RMS body
  const size_t h = f.history();
  const Vec2 p{static_cast<float>(w.x) + 0.3f * static_cast<float>(w.w), static_cast<float>(w.y + w.h / 2)};
  for (int i = 0; i < 70; ++i) (void)f.d.wheel(p, 1.0f);
  f.d.step();
  sv = v.sampleView();
  const double pxPerSample = static_cast<double>(w.w) / ((sv.z1 - sv.z0) * frames);
  CHECK(pxPerSample >= 32.0);
  CHECK(pxPerSample <= 64.01);
  CHECK(sv.z0 + 0.3 * (sv.z1 - sv.z0) == doctest::Approx(0.3).epsilon(1e-4));  // the frame under the pointer stays
  CHECK(sv.signalMix == 1.0f);
  CHECK(sv.signal.level == -1);
  CHECK(sv.signal.curve);
  CHECK_FALSE(sv.signal.approx);
  CHECK(sv.signal.dots >= 4);
  // Two neighbouring samples are that many px apart on the axis the markers use too.
  const double f0 = std::floor((sv.z0 + 0.5 * (sv.z1 - sv.z0)) * frames);
  CHECK(static_cast<double>(v.xOfFrame(f0 + 1.0) - v.xOfFrame(f0)) == doctest::Approx(pxPerSample).epsilon(0.01));
  // Shift + wheel down: right by an eighth of the view.
  const double span = sv.z1 - sv.z0;
  CHECK(f.d.wheel(p, -1.0f, 0.0f, mod::kShift));
  CHECK(v.sampleView().z0 == doctest::Approx(sv.z0 + span * 0.125).epsilon(1e-9));
  // The time ruler: a drag to the left scrolls right by as much; a double-click shows the whole sample.
  sv = v.sampleView();
  const IRect tr = v.timeRulerRect();
  REQUIRE(tr.h > 0);
  const float ty = static_cast<float>(tr.y + tr.h / 2);
  f.d.drag({static_cast<float>(tr.x + 300), ty}, {static_cast<float>(tr.x + 200), ty}, 5);
  CHECK(v.sampleView().z0 == doctest::Approx(sv.z0 + span * 100.0 / w.w).epsilon(1e-6));
  CHECK(v.sampleView().z1 - v.sampleView().z0 == doctest::Approx(span).epsilon(1e-9));
  f.d.doubleClick({static_cast<float>(tr.x + 300), ty});
  CHECK(v.sampleView().z0 == 0.0);
  CHECK(v.sampleView().z1 == 1.0);
  CHECK(f.history() == h);
  // Half way in (12 frames per px: between the body at 16 and the signal at 8) both are drawn, cross-faded.
  for (int i = 0; i < 40 && static_cast<double>(w.w) / ((v.sampleView().z1 - v.sampleView().z0) * frames) < 1.0 / 12.5; ++i) (void)f.d.wheel(p, 1.0f);
  f.d.step();
  sv = v.sampleView();
  const double fpp = (sv.z1 - sv.z0) * frames / w.w;
  if (fpp > 8.0 && fpp < 16.0) {
    CHECK(sv.signalMix > 0.0f);
    CHECK(sv.signalMix < 1.0f);
  }
}

// WF.2 (WF.1's leftover): no edge scroll while dragging in the clip view.
TEST_CASE("audio clip view (WF.2, real dispatch): the start marker held past the display's side scrolls the zoomed view, "
          "frame by frame, and stays under the pointer (one undo step); at fit nothing scrolls") {
  Fixture f(1280, 900);
  if (auto* body = dynamic_cast<Splitter*>(f.d.find("main/body"))) body->setPosition(420.0f);
  const doc::ClipRef rev = f.clip("Drums", 2);  // warp Off: the file's frames on the axis; 1.5 s of a 2 s file
  f.ctx.setDetailVisible(true);
  f.ctx.setDetailClip(rev);
  f.d.tree().layoutNow();
  REQUIRE(f.d.enableGpu().ok());
  f.d.tree().invalidateAll();
  f.d.step();
  AudioClipView& v = f.view();
  REQUIRE(v.hasClip());
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), v.clip()->sample);
  REQUIRE(s != nullptr);
  const IRect w = v.sampleRect(), ru = v.rulerRect();
  REQUIRE(w.w > 200);
  REQUIRE(ru.h > 0);
  const float my = static_cast<float>(ru.y + ru.h / 2);
  auto frameAtX = [&](float x) {
    const double x0 = v.xOfFrame(0.0), x1 = v.xOfFrame(1000.0);
    return (static_cast<double>(x) - x0) / (x1 - x0) * 1000.0;
  };
  const double start0 = v.startMarkerFrame();
  // At fit: the marker dragged past the right side and held — the view stays the whole sample.
  {
    const Vec2 a{v.xOfFrame(start0), my}, out{static_cast<float>(w.right()) + 30.0f, my};
    f.d.move(a);
    f.d.down(a);
    f.d.move({out.x - 1.0f, out.y});
    f.d.move(out);
    f.d.advance(0.2);
    CHECK(v.sampleView().z0 == 0.0);
    CHECK(v.sampleView().z1 == 1.0);
    f.d.up(out);
    f.d.step();
    REQUIRE(f.ctx.undo());
    f.d.step();
    CHECK(v.startMarkerFrame() == doctest::Approx(start0));
  }
  // Zoomed in ten notches about the marker: it sits in view, a tenth of the sample shown.
  const Vec2 at{std::clamp(v.xOfFrame(start0), static_cast<float>(w.x), static_cast<float>(w.right()) - 1.0f), static_cast<float>(w.y + w.h / 2)};
  for (int i = 0; i < 10; ++i) (void)f.d.wheel(at, 1.0f);
  f.d.step();
  const AudioClipView::SampleView z = v.sampleView();
  const double span = z.z1 - z.z0;
  REQUIRE(span < 0.12);
  const float mx = v.xOfFrame(start0);
  REQUIRE(mx >= static_cast<float>(w.x) - 1.0f);
  REQUIRE(mx < static_cast<float>(w.right()));
  const size_t h = f.history();
  // Inside the display nothing scrolls: the marker moves with the pointer.
  const Vec2 a{mx, my}, in{mx + 40.0f, my};
  f.d.move(a);
  f.d.down(a);
  f.d.move({in.x - 1.0f, in.y});
  f.d.move(in);
  f.d.advance(0.1);
  CHECK(v.sampleView().z0 == z.z0);
  const double limit = static_cast<double>(s->frames) - v.clip()->seconds * s->rate * std::exp2(700.0 / 1200.0);  // (+7 st)
  CHECK(v.startMarkerFrame() == doctest::Approx(std::min(limit, start0 + frameAtX(in.x) - frameAtX(a.x))).epsilon(1e-3));
  // Past the right side, 8 % of the display out, and held there: the view scrolls right 8 % of itself per 1 / 60 s.
  const Vec2 out{static_cast<float>(w.right()) + 0.08f * static_cast<float>(w.w), my};
  f.d.move(out);
  CHECK(v.sampleView().z0 == z.z0);  // (a move alone scrolls nothing: the frames do)
  f.d.advance(6.0 / 60.0);
  const AudioClipView::SampleView z2 = v.sampleView();
  CHECK(z2.z0 > z.z0 + 0.3 * span);
  CHECK(z2.z0 < z.z0 + 0.7 * span);
  CHECK(z2.z1 - z2.z0 == doctest::Approx(span).epsilon(1e-9));
  // The marker is where the pointer is on the scrolled view (or at the end of what the file allows).
  const double want = std::min(limit, start0 + frameAtX(out.x) - (start0));
  CHECK(v.startMarkerFrame() == doctest::Approx(std::min(limit, frameAtX(out.x))).epsilon(2e-3));
  (void)want;
  // Back inside: the scrolling stops.
  f.d.move({static_cast<float>(w.x) + 0.5f * static_cast<float>(w.w), my});
  const double held = v.sampleView().z0;
  f.d.advance(0.1);
  CHECK(v.sampleView().z0 == held);
  f.d.up({static_cast<float>(w.x) + 0.5f * static_cast<float>(w.w), my});
  f.d.step();
  CHECK(f.history() == h + 1);  // the whole drag: one step
  REQUIRE(f.ctx.undo());
  f.d.step();
  CHECK(v.startMarkerFrame() == doctest::Approx(start0));
  CHECK(v.sampleView().z0 == held);  // (the view is view state)
}

TEST_CASE("arrangement audio (WF.1): the arrangement zooms in to the samples (>= 32 px per sample) — the curve with a dot "
          "per sample, from the samples, a second into the clip") {
  Fixture f;
  REQUIRE(f.d.enableGpu().ok());
  const doc::ClipRef beat = f.clip("Drums", 0);
  const doc::AudioClip c = f.get(beat);
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), c.sample);
  REQUIRE(s != nullptr);
  namespace am = pq::views::arrangement_metrics;
  // The deepest zoom, the view starting a quarter note into the clip (its left edge is then 14 million px to the left:
  // far past the whole pixels a float holds).
  f.arr->setTimeView(static_cast<double>(c.start + kPpq), am::kMaxPxPerTick);
  f.d.tree().invalidateAll();
  f.d.step();
  CHECK(f.arr->pxPerTick() == doctest::Approx(am::kMaxPxPerTick));
  const TempoMap& tm = f.ctx.doc()->timeline->tempoMap;
  const double secondsPerTick = (tm.ticksToSeconds(c.start + 2 * kPpq) - tm.ticksToSeconds(c.start + kPpq)) / static_cast<double>(kPpq);
  const double pxPerSample = f.arr->devicePxPerTick() / (secondsPerTick * s->rate);
  MESSAGE("deepest zoom: " << pxPerSample << " px per sample");
  CHECK(pxPerSample >= 32.0);
  const ArrangementView::Stats& st = f.arr->stats();
  CHECK(st.wavesDirect >= 1);
  CHECK(st.waveSampleDraws >= 1);
  CHECK(st.waveCurveDraws >= 1);
  CHECK(st.waveApprox == 0);
  // A dot per sample in view (the lanes' width over the px per sample, give or take the ends).
  const double expect = static_cast<double>(f.arr->lanesViewport().w) / pxPerSample;
  CHECK(static_cast<double>(st.waveDots) >= expect - 1.0);
  CHECK(static_cast<double>(st.waveDots) <= expect + 4.0);
  // 16 px per sample, further along: still the curve and the dots, nothing stretched.
  f.arr->setTimeView(static_cast<double>(c.start + kPpq + kPpq / 3), am::kMaxPxPerTick * 16.0 / pxPerSample);
  f.d.tree().invalidateAll();
  f.d.step();
  CHECK(st.waveCurveDraws >= 1);
  CHECK(st.waveApprox == 0);
  CHECK(st.waveDots > 0);
}
