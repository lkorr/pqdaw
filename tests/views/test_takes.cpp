// T6.10: take lanes and comping in the arrangement, through real window dispatch (TestDriver) on the audio fixture
// (tests/views/fixtures/audio, 120 BPM: a bar = 2 s): take lanes under their track (fold / unfold; folded costs
// nothing), swipe = ops::compRegion (4 regions from 3 takes → the expected main lane, one undo step each, crossfades,
// every source frame where it belongs via the lowered sequence), click = swap a region in, Up / Down = the comp
// selection between takes, audition (a Sequence change only), delete, note-clip takes; the arrangement-takes /
// arrangement-comp goldens; (perf) 8 tracks x 8 takes x 50 clips painted within 2 ms.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/audio_clip.h"
#include "pq/document/audio_sequence.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../../src/views/src/arrangement/internal.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr Ticks kBar = kTicksPerWhole;
constexpr const char* kArr = "main/body/top/track-area/arrangement";
constexpr double kPxPerBar = 600.0;  // bars 0 … 1.5 in view: a quarter bar is 150 px
constexpr double kRate = 44100.0;    // beat.wav

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d;
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  doc::TrackId audio, keys;
  doc::SampleId beat;
  std::vector<doc::LaneId> takes;  // of `audio`
  explicit Fixture(uint32_t w = 1280, uint32_t h = 800) : d(w, h) {
    REQUIRE(ctx.openProject(views::test::fixtureDir("audio")).ok());
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (t.name == "Keys") keys = t.id;
    });
    for (const doc::SampleRef& s : *ctx.doc()->samples)
      if (s.displayName == "beat") beat = s.id;
    REQUIRE(keys.valid());
    REQUIRE(beat.valid());
    // An audio track after Keys with three one-bar takes of beat.wav from bar 0, take k reading from frame 10000·k.
    auto t = doc::ops::createTrack(*ctx.doc(), ctx.store().ids(), doc::TrackKind::Audio, keys, "Takes", doc::Colour{0x46A5E8});
    REQUIRE(t.ok());
    REQUIRE(ctx.store().dispatch(t->command).ok());
    audio = t->tracks.at(0);
    for (int k = 0; k < 3; ++k) takes.push_back(addAudioTake(audio, 0, 2.0, 10000.0 * k));
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

  doc::LaneId addAudioTake(doc::TrackId track, Ticks start, double seconds, double frame) {
    doc::AudioClip c;
    c.id = ctx.store().ids().clip();
    c.name = "beat";
    c.start = start;
    c.sample = beat;
    c.seconds = seconds;
    c.contentStartFrame = frame;
    c = doc::syncedClip(*ctx.doc()->timeline, c);
    const doc::ClipSlot slot{c.id, nullptr, std::make_shared<const doc::AudioClip>(std::move(c))};
    auto e = doc::ops::addTakeLane(*ctx.doc(), ctx.store().ids(), track, std::span<const doc::ClipSlot>(&slot, 1));
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    return this->track(track).takes.back()->id;
  }
  doc::LaneId addNoteTake(doc::TrackId track, std::vector<doc::Note> notes) {
    for (doc::Note& n : notes) n.id = ctx.store().ids().note();
    auto seq = doc::NoteSeq::fromNotes(std::move(notes));
    REQUIRE(seq.ok());
    doc::NoteClip c;
    c.id = ctx.store().ids().clip();
    c.name = "take";
    c.length = kBar;
    c.loop.end = kBar;
    c.notes = std::move(*seq);
    const doc::ClipSlot slot{c.id, std::make_shared<const doc::NoteClip>(std::move(c)), nullptr};
    auto e = doc::ops::addTakeLane(*ctx.doc(), ctx.store().ids(), track, std::span<const doc::ClipSlot>(&slot, 1));
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
    return this->track(track).takes.back()->id;
  }

  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Track& track(doc::TrackId id) const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), id);
    REQUIRE(t != nullptr);
    return *t;
  }
  int32_t takeRow(doc::LaneId lane) const {
    const auto rows = arr->takeRows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].lane == lane) return static_cast<int32_t>(i);
    return -1;
  }
  // A point in a take lane's body at tick t.
  Vec2 at(doc::LaneId lane, Ticks t) {
    const int32_t i = takeRow(lane);
    REQUIRE(i >= 0);
    const IRect r = arr->takeRect(static_cast<size_t>(i));
    return {arr->xOfTick(static_cast<double>(t)) + 0.5f, static_cast<float>(r.bottom() - 6)};
  }
  // A swipe over [t0, t1) in a take lane (a few px inside both ends: they snap back onto the grid).
  void swipe(doc::LaneId lane, Ticks t0, Ticks t1) {
    const Vec2 a = at(lane, t0), b = at(lane, t1);
    d.drag({a.x + 3.0f, a.y}, {b.x - 3.0f, b.y}, 6);
    d.step();
  }
  arrangement::TakeHeader& header(doc::LaneId lane) {
    const int32_t i = takeRow(lane);
    REQUIRE(i >= 0);
    REQUIRE(static_cast<size_t>(i) < arr->takeHeaderWidgets().size());
    d.tree().layoutNow();
    return *arr->takeHeaderWidgets()[static_cast<size_t>(i)];
  }
  static Vec2 centre(const IRect& r) { return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)}; }
};

// The frame the track plays at song time tau (the last lowered clip covering it; nullopt: none).
std::optional<double> frameAt(const doc::Project& p, doc::TrackId id, double tau) {
  const std::optional<doc::TrackPath> path = doc::findTrack(p, id);
  REQUIRE(path.has_value());
  const doc::AudioTrackDesc d = doc::lowerAudioTrack(p, *path);
  std::optional<double> out;
  for (const doc::AudioClipDesc& c : d.clips)
    if (auto f = doc::sourceFrameAt(c, p.timeline->tempoMap, tau)) out = f;
  return out;
}

}  // namespace

TEST_CASE("takes: take lanes sit under their track with a header each; the fold triangle hides them (nothing painted)") {
  Fixture f;
  const auto rows = f.arr->rows();
  const auto it = std::find_if(rows.begin(), rows.end(), [&](const ArrangementRow& r) { return r.id == f.audio; });
  REQUIRE(it != rows.end());
  CHECK(it->takeCount == 3);
  REQUIRE(f.arr->takeRows().size() == 3);
  CHECK(f.arr->takeRows()[0].y == it->y + it->h);  // right under the track's own lane
  CHECK(it->takesH == f.arr->takeRows()[2].y + f.arr->takeRows()[2].h - (it->y + it->h));
  CHECK(it->lanesH >= it->takesH);
  CHECK(f.arr->takeHeaderWidgets().size() == 3);
  REQUIRE(f.d.enableGpu().ok());  // painting needs the theme and fonts
  f.arr->lanes().markDirty();
  f.d.step();
  CHECK(f.arr->stats().takeRowsPainted == 3);
  CHECK(f.arr->stats().takeClipsPainted == 3);
  // Fold through the header's triangle: no take rows, no headers, nothing painted; view state only (no undo step).
  const size_t h0 = f.history();
  TrackHeader* th = f.arr->header(f.audio);
  REQUIRE(th != nullptr);
  REQUIRE(th->foldButton().visible());
  f.d.click(Fixture::centre(th->foldButton().rect()));
  f.d.step();
  CHECK(f.arr->takesFolded(f.audio));
  CHECK(f.arr->takeRows().empty());
  CHECK(f.arr->takeHeaderWidgets().empty());
  f.arr->lanes().markDirty();
  f.d.step();
  CHECK(f.arr->stats().takeRowsPainted == 0);
  CHECK(f.arr->stats().takeClipsPainted == 0);
  CHECK(f.history() == h0);
  f.d.click(Fixture::centre(f.arr->header(f.audio)->foldButton().rect()));
  f.d.step();
  CHECK(f.arr->takeRows().size() == 3);
  // A track without takes shows no triangle.
  CHECK_FALSE(f.arr->header(f.keys)->foldButton().visible());
}

TEST_CASE("takes: four swipes from three takes comp the main lane (one undo step each, crossfades); every source frame "
          "plays where its take has it (lowered sequence)") {
  Fixture f;
  const Ticks q = kBar / 4;
  const int pick[] = {0, 1, 2, 0};
  for (int i = 0; i < 4; ++i) {
    const size_t h = f.history();
    f.swipe(f.takes[static_cast<size_t>(pick[i])], i * q, (i + 1) * q);
    CHECK(f.history() == h + 1);
    REQUIRE(f.arr->compSelection().has_value());
    CHECK(f.arr->compSelection()->lane == f.takes[static_cast<size_t>(pick[i])]);
    CHECK(f.arr->compSelection()->range == TickRange{i * q, (i + 1) * q});
  }
  const doc::Track& t = f.track(f.audio);
  REQUIRE(t.clips.size() == 4);
  for (int i = 0; i < 4; ++i) {
    const doc::ClipSlot& c = t.clips[static_cast<size_t>(i)];
    CHECK(c.start() == i * q);
    CHECK(c.end() == (i + 1) * q);
    CHECK(c.take == f.takes[static_cast<size_t>(pick[i])]);
    REQUIRE(c.audio);
    CHECK(c.audio->crossfadeIn == (i == 0 ? 0 : doc::ops::kCompCrossfade));  // each inner boundary crossfaded
  }
  // The lowered sequence: outside the crossfades each moment plays its region's take at its own frame (sample-continuous
  // inside a region: frame = 10000·k + 44100·tau).
  const doc::Project& p = *f.ctx.doc();
  const double xf = p.timeline->tempoMap.ticksToSeconds(doc::ops::kCompCrossfade);
  int compared = 0;
  for (int i = 0; i < 4000; ++i) {
    const double tau = 2.0 * (static_cast<double>(i) + 0.37) / 4000.0;
    const int region = static_cast<int>(tau / 0.5);
    if (std::abs(tau - 0.5 * std::round(tau / 0.5)) < xf && region > 0 && std::round(tau / 0.5) < 4.0) continue;
    const std::optional<double> got = frameAt(p, f.audio, tau);
    REQUIRE(got.has_value());
    CHECK(*got == doctest::Approx(10000.0 * pick[region] + kRate * tau).epsilon(1e-9));
    ++compared;
  }
  CHECK(compared > 3800);
  // The takes are untouched (non-destructive) and undo goes back one comp at a time.
  for (size_t k = 0; k < 3; ++k) CHECK(f.track(f.audio).takes[k]->clips.size() == 1);
  REQUIRE(f.ctx.undo());
  CHECK(f.track(f.audio).clips.size() == 3);
  CHECK(f.track(f.audio).clips.back().end() == 3 * q);
}

TEST_CASE("takes: a click swaps the region under it in; Up / Down move the comp selection between takes (one undo "
          "step each); Esc drops it") {
  Fixture f;
  const Ticks q = kBar / 4;
  f.swipe(f.takes[0], q, 3 * q);  // main lane: take 0 over [q, 3q)
  REQUIRE(f.track(f.audio).clips.size() == 1);
  // A click on take 2 inside that region: the whole main-lane clip there comes from take 2 now.
  size_t h = f.history();
  f.d.click(f.at(f.takes[2], 2 * q));
  f.d.step();
  CHECK(f.history() == h + 1);
  REQUIRE(f.track(f.audio).clips.size() == 1);
  CHECK(f.track(f.audio).clips[0].take == f.takes[2]);
  CHECK(f.track(f.audio).clips[0].start() == q);
  CHECK(f.track(f.audio).clips[0].end() == 3 * q);
  REQUIRE(f.arr->compSelection().has_value());
  CHECK(f.arr->compSelection()->lane == f.takes[2]);
  // A click in a gap of the main lane: the take's clip over that gap.
  h = f.history();
  f.d.click(f.at(f.takes[1], 7 * q / 2));
  f.d.step();
  CHECK(f.history() == h + 1);
  REQUIRE(f.track(f.audio).clips.size() == 2);
  CHECK(f.track(f.audio).clips[1].take == f.takes[1]);
  CHECK(f.track(f.audio).clips[1].start() == 3 * q);
  CHECK(f.track(f.audio).clips[1].end() == kBar);
  // Up / Down: the selection's range from the take above / below.
  h = f.history();
  CHECK(f.d.press("Up") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.arr->compSelection()->lane == f.takes[0]);
  CHECK(f.track(f.audio).clips[1].take == f.takes[0]);
  CHECK(f.track(f.audio).clips[1].start() == 3 * q);
  (void)f.d.press("Up");  // no take above: nothing
  CHECK(f.history() == h + 1);
  (void)f.d.press("Down");
  (void)f.d.press("Down");
  CHECK(f.history() == h + 3);
  CHECK(f.arr->compSelection()->lane == f.takes[2]);
  CHECK(f.track(f.audio).clips[1].take == f.takes[2]);
  // Esc drops the comp selection; Up then steps the track selection again (no document change).
  CHECK(f.d.press("Escape") == KeyResult::Action);
  CHECK_FALSE(f.arr->compSelection().has_value());
  (void)f.d.press("Up");
  CHECK(f.history() == h + 3);
}

TEST_CASE("takes: Esc cancels a swipe; a swipe narrower than a grid cell comps nothing") {
  Fixture f;
  const Ticks q = kBar / 4;
  const size_t h = f.history();
  const Vec2 a = f.at(f.takes[1], q), b = f.at(f.takes[1], 2 * q);
  f.d.move(a);
  f.d.down(a);
  f.d.move({a.x + 20.0f, a.y});
  f.d.move(b);
  CHECK(f.arr->comper().dragging());
  CHECK(f.d.press("Escape") == KeyResult::Widget);
  CHECK_FALSE(f.arr->comper().dragging());
  f.d.up(b);
  f.d.step();
  CHECK(f.history() == h);
  CHECK(f.track(f.audio).clips.empty());
}

TEST_CASE("takes: audition plays a take instead of the main lane (a Sequence change only), lit in its header; delete "
          "removes the lane (one undo step)") {
  Fixture f;
  arrangement::TakeHeader& h1 = f.header(f.takes[1]);
  const size_t h = f.history();
  f.d.click(Fixture::centre(h1.auditionRect()));
  f.d.step();
  CHECK(f.track(f.audio).auditionTake == f.takes[1]);
  CHECK(f.ctx.store().lastChanges().sequenceOnly());  // the link re-sequences the track; nothing compiles
  CHECK_FALSE(f.ctx.store().lastChanges().structural());
  CHECK(f.history() == h + 1);
  CHECK(f.header(f.takes[1]).auditioned());
  CHECK_FALSE(f.header(f.takes[0]).auditioned());
  f.d.click(Fixture::centre(f.header(f.takes[1]).auditionRect()));  // again: the main lane plays
  f.d.step();
  CHECK_FALSE(f.track(f.audio).auditionTake.valid());
  CHECK_FALSE(f.header(f.takes[1]).auditioned());
  // Delete take 2 (its header's ×; done at the next frame, not from inside the header's own handler).
  const size_t h2 = f.history();
  f.d.click(Fixture::centre(f.header(f.takes[2]).deleteRect()));
  f.d.step();
  f.d.step();
  CHECK(f.history() == h2 + 1);
  REQUIRE(f.track(f.audio).takes.size() == 2);
  CHECK(f.arr->takeRows().size() == 2);
  CHECK(f.arr->takeHeaderWidgets().size() == 2);
  REQUIRE(f.ctx.undo());
  f.d.step();
  CHECK(f.track(f.audio).takes.size() == 3);
  CHECK(f.arr->takeHeaderWidgets().size() == 3);
}

TEST_CASE("takes: note-clip takes comp the same way (swipe, click, Up / Down): every note at its absolute time") {
  Fixture f;
  auto notes = [](int pitchBase) {
    std::vector<doc::Note> v;
    for (int i = 0; i < 16; ++i) {
      doc::Note n;
      n.start = i * kBar / 16;
      n.length = kBar / 32;
      n.pitch = tuning::PitchId::fromDegree(pitchBase + i % 5, 0);
      n.velocity = 100.0f;
      v.push_back(n);
    }
    return v;
  };
  const doc::LaneId a = f.addNoteTake(f.keys, notes(0)), b = f.addNoteTake(f.keys, notes(10));
  f.d.tree().layoutNow();
  f.d.step();
  // Keys' main lane: one clip "chords" over bar 0. Swipe take b over [1/4, 1/2) bar.
  const Ticks q = kBar / 4;
  const size_t h = f.history();
  f.swipe(b, q, 2 * q);
  CHECK(f.history() == h + 1);
  // What plays: the main lane's notes outside [q, 2q), take b's inside, each at its absolute tick.
  struct Occ {
    Ticks start;
    tuning::PitchId pitch;
    bool operator==(const Occ&) const = default;
  };
  auto played = [](const doc::Track& t) {
    std::vector<Occ> out;
    for (const doc::ClipSlot& s : t.clips)
      for (const doc::Note& n : s.clip->notes) {
        const Ticks at = n.start - s.clip->contentStart + s.clip->start;
        if (at >= s.clip->start && at < s.clip->start + s.clip->length) out.push_back({at, n.pitch});
      }
    std::sort(out.begin(), out.end(), [](const Occ& x, const Occ& y) { return x.start < y.start; });
    return out;
  };
  std::vector<Occ> want;
  // Expected from the takes themselves: take b inside the region; the original main lane elsewhere (undo to read it).
  REQUIRE(f.ctx.undo());
  for (const Occ& o : played(f.track(f.keys)))
    if (o.start < q || o.start >= 2 * q) want.push_back(o);
  REQUIRE(f.ctx.redo());
  for (const doc::Note& n : f.track(f.keys).takeLane(b)->clips[0].clip->notes)
    if (n.start >= q && n.start < 2 * q) want.push_back({n.start, n.pitch});
  std::sort(want.begin(), want.end(), [](const Occ& x, const Occ& y) { return x.start < y.start; });
  CHECK(played(f.track(f.keys)) == want);
  bool fromB = false;
  for (const doc::ClipSlot& s : f.track(f.keys).clips) fromB = fromB || (s.take == b && s.start() == q && s.end() == 2 * q);
  CHECK(fromB);
  // Up: the same range from take a.
  CHECK(f.d.press("Up") == KeyResult::Action);
  CHECK(f.history() == h + 2);
  CHECK(f.arr->compSelection()->lane == a);
  bool fromA = false;
  for (const doc::ClipSlot& s : f.track(f.keys).clips) fromA = fromA || (s.take == a && s.start() == q && s.end() == 2 * q);
  CHECK(fromA);
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

TEST_CASE("render: arrangement-takes (take lanes under an audio and a MIDI track, an auditioned take) matches the golden") {
  checkGolden("arrangement-takes");
}

TEST_CASE("render: arrangement-comp (4 comped regions from 3 takes, a note comp, the comp selection) matches the golden") {
  checkGolden("arrangement-comp");
}

TEST_CASE("takes perf: 8 tracks x 8 takes x 50 clips paint within 2 ms" * doctest::test_suite("perf")) {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 1440};
  doc::DocumentStore& st = ctx.store();
  doc::TrackId prev{};
  for (int t = 0; t < 8; ++t) {
    auto tr = doc::ops::createTrack(*ctx.doc(), st.ids(), doc::TrackKind::Midi, prev, "T" + std::to_string(t), doc::Colour{0x46A5E8});
    REQUIRE(tr.ok());
    REQUIRE(st.dispatch(tr->command).ok());
    prev = tr->tracks.at(0);
    for (int k = 0; k < 8; ++k) {
      std::vector<doc::ClipSlot> clips;
      for (int c = 0; c < 50; ++c) {
        std::vector<doc::Note> notes;
        for (int i = 0; i < 8; ++i) {
          doc::Note n;
          n.id = st.ids().note();
          n.start = i * kBar / 16;
          n.length = kBar / 32;
          n.pitch = tuning::PitchId::fromDegree((i * 3 + k + c) % 12, 0);
          n.velocity = 100.0f;
          notes.push_back(n);
        }
        auto seq = doc::NoteSeq::fromNotes(std::move(notes));
        REQUIRE(seq.ok());
        doc::NoteClip nc;
        nc.id = st.ids().clip();
        nc.name = "take";
        nc.start = c * kBar;
        nc.length = kBar / 2;
        nc.loop.end = kBar / 2;
        nc.notes = std::move(*seq);
        clips.push_back({nc.id, std::make_shared<const doc::NoteClip>(std::move(nc)), nullptr});
      }
      auto e = doc::ops::addTakeLane(*ctx.doc(), st.ids(), prev, clips);
      REQUIRE(e.ok());
      REQUIRE(st.dispatch(e->command).ok());
    }
  }
  auto s = Shell::create(d.tree(), ctx);
  REQUIRE(s.ok());
  REQUIRE(d.enableGpu().ok());
  auto* arr = dynamic_cast<ArrangementView*>(d.find(kArr));
  REQUIRE(arr != nullptr);
  CHECK(arr->takeRows().size() == 64);
  arr->zoomToSelection();  // the whole song: every clip of every visible lane in view
  d.tree().layoutNow();
  for (int i = 0; i < 200 && d.tree().needsFrame(); ++i) d.step();  // previews and tiles built
  std::vector<double> ms;
  for (int i = 0; i < 40; ++i) {
    arr->lanes().markDirty();
    d.step();
    ms.push_back(arr->stats().lanesPaintMs);
  }
  std::sort(ms.begin(), ms.end());
  MESSAGE("8 x 8 x 50 takes: lanes paint median " << ms[ms.size() / 2] << " ms, max " << ms.back() << " ms; take lanes "
                                                   << arr->stats().takeRowsPainted << ", take clips " << arr->stats().takeClipsPainted);
  CHECK(arr->stats().takeClipsPainted >= 50 * 8);
  CHECK(ms[ms.size() / 2] <= 2.0);
  d.tree().setRoot(std::make_unique<Widget>());
}
