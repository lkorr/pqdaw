// T5.6: audio clips in the arrangement and the audio Clip View (tests/views/fixtures/audio) — painting (waveforms,
// overlays, missing), the generic clip edits on audio clips (move / copy / cross-track / trim limits / split / delete /
// clipboard / range delete), the handles (fades, crossfade by dragging a fade past the edge, gain, double-click reset,
// Alt curve, Esc), the clip view's edits (gain, transpose entry, warp + BPM seed, reverse, start marker, loop brace),
// each one undo step; the arrangement-audio and clip-view-audio goldens; (perf) the 64 × 200 audio arrangement.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include "../ui/golden_check.h"
#include "../ui/thread_cpu.h"
#include "audio_fixture.h"
#include "pq/document/audio_clip.h"
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
  // A MIDI clip: the piano roll again.
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

TEST_CASE("audio clip view: transpose entry, gain, warp (BPM seeded), reverse, start marker and loop brace drags") {
  Fixture f(1280, 900);
  if (auto* body = dynamic_cast<Splitter*>(f.d.find("main/body"))) body->setPosition(420.0f);
  const doc::ClipRef vox = f.clip("Vox", 0), rev = f.clip("Drums", 2);
  f.ctx.setDetailVisible(true);
  f.ctx.setDetailClip(vox);
  f.d.tree().layoutNow();
  AudioClipView& v = f.view();
  REQUIRE(v.hasClip());
  CHECK(v.clip()->warp == doc::AudioWarp::RePitch);
  CHECK_FALSE(v.box().find("transpose-row/transpose")->enabled());  // greyed in Re-Pitch (Q145)

  // Transpose parsing.
  CHECK(parseTranspose("7")->unit == doc::TransposeUnit::Steps);
  CHECK(parseTranspose("7")->steps == 7);
  CHECK(parseTranspose("-700c")->cents == -700.0);
  CHECK(parseTranspose("+7st")->cents == 700.0);
  CHECK(parseTranspose("3/2")->ratio == tuning::Rational(3, 2));
  CHECK_FALSE(parseTranspose("abc").has_value());
  CHECK_FALSE(parseTranspose("0/2").has_value());
  CHECK(formatTranspose(*parseTranspose("3/2")) == "3/2");
  CHECK(sourceBpmFor(0.0, 1.5) == doctest::Approx(160.0));  // one bar of 4/4 in 1.5 s
  CHECK(sourceBpmFor(98.0, 1.5) == 98.0);

  size_t h = f.history();
  CHECK(v.setWarp(doc::AudioWarp::Off));
  CHECK(v.clip()->warp == doc::AudioWarp::Off);
  CHECK(v.box().find("transpose-row/transpose")->enabled());
  CHECK(v.setTranspose("3/2"));
  CHECK(v.clip()->transpose.unit == doc::TransposeUnit::Ratio);
  CHECK_FALSE(v.setTranspose("nonsense"));
  CHECK(v.setGain(-3.0f));
  CHECK(v.clip()->gainDb == -3.0f);
  CHECK(v.setReverse(true));
  CHECK(v.clip()->reverse);
  CHECK(f.history() == h + 4);
  CHECK(v.setWarp(doc::AudioWarp::RePitch));
  CHECK(v.clip()->sourceBpm == doctest::Approx(160.0));  // vox.wav: 1.5 s, no tempo tag → one bar
  CHECK(f.history() == h + 5);

  // The gain box: a drag is one undo step.
  h = f.history();
  const IRect gb = f.d.rect(std::string("main/body/bottom/detail/clip/audio-clip-view/audio-box/gain-row/gain"));
  const Vec2 gc{static_cast<float>(gb.x + gb.w / 2), static_cast<float>(gb.y + gb.h / 2)};
  f.d.drag(gc, {gc.x, gc.y - 40.0f}, 8);
  CHECK(f.get(vox).gainDb > -3.0f);
  CHECK(f.history() == h + 1);

  // The loop brace's end, dragged right: the loop grows (1/16 notes), one undo step. (Switching warps left the loop
  // off: on again first.)
  CHECK(v.setLoopEnabled(true));
  h = f.history();
  const doc::AudioClip& a = f.get(vox);
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), a.sample);
  REQUIRE(s != nullptr);
  const double fpt = doc::framesPerContentTick(s->rate, a.sourceBpm);
  const Ticks end0 = a.loop.end;
  const IRect ru = v.rulerRect();
  const float ex = v.xOfFrame(static_cast<double>(end0) * fpt);
  f.d.drag({ex, static_cast<float>(ru.y + ru.h / 2)}, {ex + 60.0f, static_cast<float>(ru.y + ru.h / 2)}, 6);
  CHECK(f.get(vox).loop.end > end0);
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

TEST_CASE("render: arrangement-audio (waveforms, fades, crossfade, gain, reverse, missing, MIDI between) matches the golden") {
  checkGolden("arrangement-audio");
}

TEST_CASE("render: clip-view-audio (the audio clip view on a Re-Pitch clip) matches the golden") { checkGolden("clip-view-audio"); }
