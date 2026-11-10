// T6.1 (docs/plans/M6.md): the recording document model — monitor / inputs (arm and monitor are Params, an audio input
// is structural and lowers as the monitor gate), note input and the live input map in both note worlds, take lanes
// (audition, promote, comping that keeps every note and source frame at its absolute time), punch and record settings,
// warp markers and the time-stretch modes, and the file (schema 22: save → load deep-equal with every field).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "edit_helpers.h"
#include "pq/document/audio_clip.h"
#include "pq/document/audio_sequence.h"
#include "pq/document/devices.h"
#include "pq/document/input_map.h"
#include "pq/document/lower.h"
#include "pq/document/pitch_resolve.h"
#include "pq/document/serialize.h"

using namespace pq;
using namespace pq::doc;
using edit_test::commit;
using edit_test::kBar;

namespace {

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool updateGoldens() {
  const char* v = std::getenv("PQ_UPDATE_GOLDENS");
  return v != nullptr && *v != '\0' && *v != '0';
}

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// A MIDI track and an audio track, one 2-s stereo sample at 48 kHz.
struct Rec {
  DocumentStore store;
  TrackId midi, audio;
  SampleId sample;
  Rec() {
    IdSource& ids = store.ids();
    midi = edit(ops::createTrack(doc(), ids, TrackKind::Midi)).tracks.at(0);
    audio = edit(ops::createTrack(doc(), ids, TrackKind::Audio, midi)).tracks.at(0);
    SampleRef s;
    s.id = ids.sample();
    s.path = "samples/take.wav";
    s.frames = 96000;
    s.rate = 48000.0;
    s.channels = 2;
    s.displayName = "take";
    sample = s.id;
    REQUIRE(store.dispatch(AddSample{s}).ok());
  }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const Result<ops::Edit>& e) {
    commit(store, e);
    return *e;
  }
  const Track& track(TrackId id) const {
    const Track* t = findTrackPtr(doc(), id);
    REQUIRE(t);
    return *t;
  }
  // An Off clip of the sample at `start`, `seconds` long, reading from `frame`.
  AudioClip audioClip(Ticks start, double seconds, double frame) {
    AudioClip a;
    a.id = store.ids().clip();
    a.name = "take";
    a.start = start;
    a.sample = sample;
    a.seconds = seconds;
    a.contentStartFrame = frame;
    return syncedClip(*doc().timeline, a);
  }
  AudioTrackDesc lowered(TrackId t) const { return lowerAudioTrack(doc(), *findTrack(doc(), t)); }
};

ClipSlot slotOf(AudioClip a) {
  const ClipId id = a.id;
  return ClipSlot{id, nullptr, std::make_shared<const AudioClip>(std::move(a))};
}
ClipSlot slotOf(const ClipPtr& c) { return ClipSlot{c->id, c, nullptr}; }

// The frame the track plays at song time tau (the last lowered clip that covers it; nullopt: none).
std::optional<double> frameAt(const AudioTrackDesc& d, const TempoMap& tm, double tau) {
  std::optional<double> out;
  for (const AudioClipDesc& c : d.clips)
    if (auto f = sourceFrameAt(c, tm, tau)) out = f;
  return out;
}

}  // namespace

TEST_CASE("recording: arm and monitor are Params on MIDI and audio tracks; an audio input is structural and lowers the "
          "monitor gate (golden audio-track-input.pqp); without one nothing is lowered") {
  Rec r;
  // Live's exclusive arm now reaches audio tracks.
  commit(r.store, ops::armTrack(r.doc(), r.audio, false));
  CHECK(r.track(r.audio).mixer.arm);
  CHECK(r.store.lastChanges().paramOnly());
  commit(r.store, ops::armTrack(r.doc(), r.midi, false));
  CHECK(r.track(r.midi).mixer.arm);
  CHECK_FALSE(r.track(r.audio).mixer.arm);

  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.clipSlotOf = [](TrackId) { return 2u; };
  auto plain = lower(r.doc(), lo);
  REQUIRE_MESSAGE(plain.ok(), (plain.ok() ? std::string() : plain.error().message));
  auto indexOf = [&](const LoweredProject& lp) {
    for (uint32_t i = 0; i < lp.trackIds.size(); ++i)
      if (lp.trackIds[i] == r.audio) return i;
    FAIL("audio track not lowered");
    return 0u;
  };
  CHECK(patchText(*plain->patches[indexOf(*plain)]).find("track.in") == std::string::npos);
  CHECK(plain->audioInputs.empty());

  // An input: one structural step; the patch reads track.in through the gate.
  REQUIRE(r.store.dispatch(SetTrackAudioInput{r.audio, AudioInput{2, 2}}).ok());
  CHECK(r.store.lastChanges().structural());
  auto in = lower(r.doc(), lo, &*plain);
  REQUIRE(in.ok());
  const uint32_t gi = indexOf(*in);
  const std::string text = patchText(*in->patches[gi]);
  CHECK(text.find("track.in") != std::string::npos);
  REQUIRE(in->audioInputs.size() == 1);
  CHECK(in->audioInputs[0].track == r.audio);
  CHECK(in->audioInputs[0].input == AudioInput{2, 2});
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/document/fixtures/lowering/audio-track-input.pqp";
  if (updateGoldens()) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
    MESSAGE("rewrote " << path);
  }
  CHECK(readFile(path) == text);
  auto gate = [&](const Project& p) {
    for (const LoweredParam& lp : lowerParams(p, true, library()))
      if (lp.key == std::to_string(r.audio.value) + ":monitor") {
        CHECK(lp.ref == ParamRef::track(ParamKind::TrackMonitor, r.audio));
        return lp.value;
      }
    FAIL("no monitor param");
    return -1.0f;
  };
  CHECK(gate(r.doc()) == 0.0f);  // Auto, disarmed: the clips
  // The monitor switch and the arm are param writes: the IR stays (the patch object is shared).
  REQUIRE(r.store.dispatch(SetTrackMonitor{r.audio, MonitorMode::In}).ok());
  CHECK(r.store.lastChanges().paramOnly());
  CHECK(gate(r.doc()) == 1.0f);
  auto again = lower(r.doc(), lo, &*in);
  REQUIRE(again.ok());
  CHECK(again->patches[gi] == in->patches[gi]);
  REQUIRE(r.store.dispatch(SetTrackMonitor{r.audio, MonitorMode::Auto}).ok());
  REQUIRE(r.store.dispatch(SetTrackArm{r.audio, true}).ok());
  CHECK(r.store.lastChanges().paramOnly());
  CHECK(gate(r.doc()) == 1.0f);  // Auto follows the arm
  REQUIRE(r.store.dispatch(SetTrackMonitor{r.audio, MonitorMode::Off}).ok());
  CHECK(gate(r.doc()) == 0.0f);
  // A mono input on another channel: the same IR (the engine binds the channels), a new binding.
  REQUIRE(r.store.dispatch(SetTrackAudioInput{r.audio, AudioInput{5, 1}}).ok());
  auto mono = lower(r.doc(), lo, &*again);
  REQUIRE(mono.ok());
  CHECK(mono->audioInputs.at(0).input == AudioInput{5, 1});
  // Inputs are per kind.
  CHECK_FALSE(r.store.dispatch(SetTrackAudioInput{r.midi, AudioInput{0, 2}}).ok());
  CHECK_FALSE(r.store.dispatch(SetTrackAudioInput{r.audio, AudioInput{255, 2}}).ok());
  CHECK_FALSE(r.store.dispatch(SetTrackMidiInput{r.audio, MidiInput{MidiInputKind::None, "", 0}}).ok());
}

TEST_CASE("recording: take lanes — audition is a Sequence change of what the track plays; removing the auditioned take "
          "plays the main lane again; samples and tempo edits reach take clips") {
  Rec r;
  // Main lane: one Off clip from frame 0. Take: one from frame 24000.
  const ClipSlot main = slotOf(r.audioClip(0, 1.5, 0.0));
  REQUIRE(r.store.dispatch(AddClip{r.audio, nullptr, main.audio}).ok());
  const AudioClip takeClip = r.audioClip(0, 1.5, 24000.0);
  const ClipSlot one[] = {slotOf(takeClip)};
  const ops::Edit added = r.edit(ops::addTakeLane(r.doc(), r.store.ids(), r.audio, one));
  CHECK(r.store.lastChanges().metaOnly());
  REQUIRE(r.track(r.audio).takes.size() == 1);
  const TakeLane& lane = *r.track(r.audio).takes[0];
  CHECK(lane.name == "Take 1");
  CHECK(lane.clips.at(0).id != takeClip.id);  // fresh ids
  CHECK(added.clips.at(0).clip == lane.clips.at(0).id);
  const LaneId take = lane.id;
  const TempoMap& tm = r.doc().timeline->tempoMap;
  CHECK(*frameAt(r.lowered(r.audio), tm, 0.5) == doctest::Approx(24000.0));  // 0.5 s in: frame 24000
  CHECK(r.lowered(r.audio).clips.size() == 1);

  commit(r.store, ops::setAuditionTake(r.doc(), r.audio, take));
  CHECK(r.store.lastChanges().sequenceOnly());
  CHECK(*frameAt(r.lowered(r.audio), r.doc().timeline->tempoMap, 0.5) == doctest::Approx(48000.0));
  // The sample a take plays cannot leave the table.
  CHECK_FALSE(r.store.dispatch(Batch{"x", {RemoveClip{{r.audio, main.id}}, RemoveSample{r.sample}}}).ok());
  // A tempo edit re-derives the take's Off clip too (it keeps its seconds).
  REQUIRE(r.store.dispatch(SetTempoMap{{TempoPoint{0, 60.0, TempoCurve::Step}}}).ok());
  const AudioClip& tc = *r.track(r.audio).takes[0]->clips[0].audio;
  CHECK(tc.length == offClipLength(r.doc().timeline->tempoMap, tc.start, tc.seconds));
  CHECK(tc.length == kPpq * 3 / 2);  // 1.5 s at 60 BPM
  // Removing the auditioned take is a Sequence change: the main lane plays again.
  REQUIRE(r.store.dispatch(RemoveTakeLane{r.audio, take}).ok());
  CHECK(r.store.lastChanges().has(kChangeSequence));
  CHECK_FALSE(r.track(r.audio).auditionTake.valid());
  CHECK(*frameAt(r.lowered(r.audio), r.doc().timeline->tempoMap, 0.5) == doctest::Approx(24000.0));
  REQUIRE(r.store.undo());
  CHECK(r.track(r.audio).auditionTake == take);

  // Notes: the audition take's notes are what resolveTrackPitches (and so the note sequence) gives.
  IdSource& ids = r.store.ids();
  ClipPtr mc = doctest_fixture::makeClip(ids, 0, kBar, {doctest_fixture::makeNote(ids, 0, 0)});
  REQUIRE(r.store.dispatch(AddClip{r.midi, mc}).ok());
  ClipPtr tcn = doctest_fixture::makeClip(ids, 0, kBar, {doctest_fixture::makeNote(ids, 0, 7), doctest_fixture::makeNote(ids, kPpq, 4)});
  const ClipSlot nt[] = {slotOf(tcn)};
  commit(r.store, ops::addTakeLane(r.doc(), ids, r.midi, nt, "Verse"));
  const LaneId mt = r.track(r.midi).takes.at(0)->id;
  CHECK(resolveTrackPitches(r.doc(), r.midi)->clips.at(0).notes.size() == 1);
  commit(r.store, ops::setAuditionTake(r.doc(), r.midi, mt));
  const auto heard = resolveTrackPitches(r.doc(), r.midi);
  REQUIRE(heard.ok());
  REQUIRE(heard->clips.size() == 1);
  CHECK(heard->clips[0].notes.size() == 2);
  // Kinds are checked: a note clip cannot go into an audio track's take lane.
  const auto wrongKind = ops::addTakeLane(r.doc(), ids, r.audio, nt);
  REQUIRE(wrongKind.ok());
  CHECK_FALSE(r.store.dispatch(wrongKind->command).ok());
}

TEST_CASE("recording: compRegion keeps every source frame and every note at its absolute time; one undo step; the take "
          "is unchanged; the edges are crossfaded and the pieces carry their take") {
  Rec r;
  REQUIRE(r.store.dispatch(SetTempoMap{{TempoPoint{0, 131.0, TempoCurve::Linear}, TempoPoint{8 * kBar, 89.0, TempoCurve::Step}}}).ok());
  // Main lane: frame 0 from bar 0 for 1.9 s; take: frame 30000 + a little, from a quarter in, 1.7 s.
  const AudioClip m = r.audioClip(0, 1.9, 0.0);
  REQUIRE(r.store.dispatch(AddClip{r.audio, nullptr, std::make_shared<const AudioClip>(m)}).ok());
  const ClipSlot takeClips[] = {slotOf(r.audioClip(kPpq / 3, 1.7, 30000.25))};
  commit(r.store, ops::addTakeLane(r.doc(), r.store.ids(), r.audio, takeClips));
  const LaneId take = r.track(r.audio).takes.at(0)->id;
  const TempoMap& tm = r.doc().timeline->tempoMap;
  const AudioTrackDesc mainBefore = r.lowered(r.audio);
  REQUIRE(r.store.dispatch(SetAuditionTake{r.audio, take}).ok());
  const AudioTrackDesc takeHeard = r.lowered(r.audio);
  REQUIRE(r.store.dispatch(SetAuditionTake{r.audio, {}}).ok());
  const TakeLanePtr laneBefore = r.track(r.audio).takes[0];

  const Ticks t0 = kPpq + 17, t1 = 3 * kPpq;  // off the grid on the left
  const ops::Edit e = r.edit(ops::compRegion(r.doc(), r.store.ids(), r.audio, take, t0, t1));
  CHECK(r.store.lastChanges().sequenceOnly());
  CHECK(r.track(r.audio).takes[0] == laneBefore);  // non-destructive: the very same take
  const Track& t = r.track(r.audio);
  REQUIRE(t.clips.size() == 3);  // main | comp | main
  REQUIRE(e.clips.size() == 1);
  CHECK(t.clips[1].id == e.clips[0].clip);
  CHECK(t.clips[1].take == take);
  CHECK_FALSE(t.clips[0].take.valid());
  CHECK(t.clips[1].start() == t0);
  CHECK(t.clips[1].end() == t1);
  CHECK(t.clips[1].audio->crossfadeIn == ops::kCompCrossfade);
  CHECK(t.clips[2].audio->crossfadeIn == ops::kCompCrossfade);

  const AudioTrackDesc after = r.lowered(r.audio);
  const double s0 = tm.ticksToSeconds(t0), s1 = tm.ticksToSeconds(t1), end = tm.ticksToSeconds(m.start + m.length);
  int compared = 0;
  for (int i = 0; i < 2000; ++i) {
    const double tau = end * (static_cast<double>(i) + 0.31) / 2000.0;
    const std::optional<double> want = (tau >= s0 && tau < s1) ? frameAt(takeHeard, tm, tau) : frameAt(mainBefore, tm, tau);
    const std::optional<double> got = frameAt(after, tm, tau);
    REQUIRE(want.has_value() == got.has_value());
    if (!want) continue;
    CHECK(std::abs(*want - *got) <= 1e-6);
    ++compared;
  }
  CHECK(compared > 1500);

  // Notes: what plays in [t0, t1) is the take's, the rest the main lane's, each at its absolute time.
  IdSource& ids = r.store.ids();
  std::vector<Note> mn, tn;
  for (int i = 0; i < 16; ++i) mn.push_back(doctest_fixture::makeNote(ids, i * kPpq / 4, i % 7, kPpq / 8));
  for (int i = 0; i < 16; ++i) tn.push_back(doctest_fixture::makeNote(ids, i * kPpq / 4 + kPpq / 16, 10 + i % 5, kPpq / 8));
  ClipPtr mc = doctest_fixture::makeClip(ids, 0, kBar, mn);
  REQUIRE(r.store.dispatch(AddClip{r.midi, mc}).ok());
  ClipPtr tc = doctest_fixture::makeClip(ids, 0, kBar, tn);
  const ClipSlot nt[] = {slotOf(tc)};
  commit(r.store, ops::addTakeLane(r.doc(), ids, r.midi, nt));
  const LaneId mtake = r.track(r.midi).takes.at(0)->id;
  const std::vector<edit_test::Occ> mainNotes = edit_test::played(r.track(r.midi));
  REQUIRE(r.store.dispatch(SetAuditionTake{r.midi, mtake}).ok());
  Track takeOnly = r.track(r.midi);
  takeOnly.clips = r.track(r.midi).takes[0]->clips;
  const std::vector<edit_test::Occ> takeNotes = edit_test::played(takeOnly);
  REQUIRE(r.store.dispatch(SetAuditionTake{r.midi, {}}).ok());
  const Ticks n0 = kPpq, n1 = 3 * kPpq;
  commit(r.store, ops::compRegion(r.doc(), ids, r.midi, mtake, n0, n1));
  std::vector<edit_test::Occ> want;
  for (const edit_test::Occ& o : mainNotes)
    if (o.start < n0 || o.start >= n1) want.push_back(o);
  for (const edit_test::Occ& o : takeNotes)
    if (o.start >= n0 && o.start < n1) want.push_back(o);
  CHECK(edit_test::played(r.track(r.midi)) == edit_test::sorted(want));
  // Every clip and note id of the project (both lanes, both kinds) is unique.
  std::vector<uint32_t> all;
  forEachTrack(r.doc(), [&](const Track& tr, const Track*) {
    auto see = [&](const std::vector<ClipSlot>& v) {
      for (const ClipSlot& s : v) {
        all.push_back(s.id.value);
        if (s.clip)
          for (const Note& n : s.clip->notes) all.push_back(n.id.value);
      }
    };
    see(tr.clips);
    for (const TakeLanePtr& l : tr.takes) see(l->clips);
  });
  std::sort(all.begin(), all.end());
  CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
}

TEST_CASE("recording: warp markers — adding pins the frame playing now (nothing changes), moving one keeps every other "
          "marker's song time exactly, removing drops it; warp modes keep placement and the start's frame") {
  Rec r;
  const AudioClip a0 = r.audioClip(kBar, 1.9, 1000.0);
  REQUIRE(r.store.dispatch(AddClip{r.audio, nullptr, std::make_shared<const AudioClip>(a0)}).ok());
  const ClipRef c{r.audio, a0.id};
  const std::span<const ClipRef> one(&c, 1);
  auto clip = [&]() -> const AudioClip& { return *r.track(r.audio).clips.at(0).audio; };
  CHECK_FALSE(ops::addWarpMarker(r.doc(), c, kPpq).ok());  // warp Off: no markers

  // Off → Beats → Complex Pro → Off: placement and the frame at the start stay; between warped modes only the mode.
  const Ticks start = clip().start, end = clipEnd(clip());
  commit(r.store, ops::setWarpMode(r.doc(), one, AudioWarp::Beats));
  CHECK(clip().warp == AudioWarp::Beats);
  CHECK(clip().start == start);
  CHECK(clip().length == end - start);
  const TempoMap& tm = r.doc().timeline->tempoMap;
  const double tauStart = tm.ticksToSeconds(start) + 1e-9;
  CHECK(*frameAt(r.lowered(r.audio), tm, tauStart) == doctest::Approx(1000.0).epsilon(1e-3));
  const AudioClip beats = clip();
  commit(r.store, ops::setWarpMode(r.doc(), one, AudioWarp::ComplexPro));
  CHECK(clip().contentStart == beats.contentStart);
  CHECK(clip().length == beats.length);
  CHECK(r.lowered(r.audio).clips.at(0).pitchRatio == 1.0);
  commit(r.store, ops::setAudioClipTranspose(r.doc(), one, AudioTranspose{TransposeUnit::Cents, 700.0, 0, {1}}));
  CHECK(r.lowered(r.audio).clips.at(0).pitchRatio == doctest::Approx(std::exp2(700.0 / 1200.0)));  // time-stretch transposes
  CHECK(r.lowered(r.audio).clips.at(0).rateRatio == 1.0);

  // Markers.
  const AudioTrackDesc before = r.lowered(r.audio);
  // Inside the clip (1.9 s at 120 BPM = 3.8 quarters).
  commit(r.store, ops::addWarpMarker(r.doc(), c, clip().contentStart + kPpq / 2));
  commit(r.store, ops::addWarpMarker(r.doc(), c, clip().contentStart + 3 * kPpq / 2));
  commit(r.store, ops::addWarpMarker(r.doc(), c, clip().contentStart + 3 * kPpq));
  REQUIRE(clip().markers.size() == 3);
  const AudioTrackDesc pinned = r.lowered(r.audio);
  for (int i = 0; i < 500; ++i) {  // adding markers changes nothing that plays
    const double tau = tm.ticksToSeconds(start) + (tm.ticksToSeconds(end) - tm.ticksToSeconds(start)) * (i + 0.5) / 500.0;
    CHECK(*frameAt(pinned, tm, tau) == doctest::Approx(*frameAt(before, tm, tau)).epsilon(1e-9));
  }
  // Song time of a marker: where its content tick plays (no loop: start + content − contentStart).
  auto songOf = [&](const WarpMarker& w) { return clip().start + (w.content - clip().contentStart); };
  const WarpMarker m0 = clip().markers[0], m2 = clip().markers[2];
  commit(r.store, ops::moveWarpMarker(r.doc(), c, 1, clip().markers[1].content + kPpq / 4));
  CHECK(clip().markers[0] == m0);
  CHECK(clip().markers[2] == m2);
  CHECK(songOf(clip().markers[0]) == songOf(m0));
  CHECK(songOf(clip().markers[2]) == songOf(m2));
  const AudioTrackDesc moved = r.lowered(r.audio);
  // Exactly at those song times the pinned frames play.
  const std::optional<double> f0 = frameAt(moved, tm, tm.ticksToSeconds(songOf(m0)));
  const std::optional<double> f2 = frameAt(moved, tm, tm.ticksToSeconds(songOf(m2)));
  REQUIRE(f0);
  REQUIRE(f2);
  CHECK(*f0 == doctest::Approx(m0.sourceFrame).epsilon(1e-12));
  CHECK(*f2 == doctest::Approx(m2.sourceFrame).epsilon(1e-12));
  // A move clamps between the neighbours.
  commit(r.store, ops::moveWarpMarker(r.doc(), c, 1, m2.content + 100));
  CHECK(clip().markers[1].content == m2.content - 1);
  commit(r.store, ops::removeWarpMarker(r.doc(), c, 1));
  REQUIRE(clip().markers.size() == 2);
  CHECK(clip().markers[0] == m0);
  CHECK(clip().markers[1] == m2);
  // Warped → Off keeps the start marker's frame through the markers.
  commit(r.store, ops::setWarpMode(r.doc(), one, AudioWarp::Off));
  CHECK(clip().contentStartFrame == doctest::Approx(warpFrameAt(beats, 48000.0, static_cast<double>(beats.contentStart))));
  CHECK(clip().markers.size() == 2);  // kept for the next warped life
  CHECK_FALSE(ops::moveWarpMarker(r.doc(), c, 0, 0).ok());
}

TEST_CASE("recording: T6.8 warp commands — slide keeps the content tick, Warp From Here, Warp as N-Bar Loop, auto-warp; "
          "each one step") {
  Rec r;
  const AudioClip a0 = r.audioClip(kBar, 1.9, 1000.0);
  REQUIRE(r.store.dispatch(AddClip{r.audio, nullptr, std::make_shared<const AudioClip>(a0)}).ok());
  const ClipRef c{r.audio, a0.id};
  const std::span<const ClipRef> one(&c, 1);
  auto clip = [&]() -> const AudioClip& { return *r.track(r.audio).clips.at(0).audio; };
  const size_t h0 = r.store.history().cursor();
  CHECK_FALSE(ops::warpFromHere(r.doc(), c, kPpq, 100.0).ok());  // Off: no markers

  // Auto-warp: Off → Beats at 100 BPM, the length re-derived so the clip plays to the file's end (2 s of source at 100
  // BPM = 3.333 quarters).
  commit(r.store, ops::autoWarpClips(r.doc(), one, AudioWarp::Beats, 100.0));
  CHECK(r.store.history().cursor() == h0 + 1);
  CHECK(clip().warp == AudioWarp::Beats);
  CHECK(clip().sourceBpm == 100.0);
  const double fpt = framesPerContentTick(48000.0, 100.0);
  CHECK(static_cast<double>(clip().contentStart + clip().length) * fpt == doctest::Approx(96000.0).epsilon(1e-4));
  CHECK(warpFrameAt(clip(), 48000.0, static_cast<double>(clip().contentStart)) == doctest::Approx(1000.0).epsilon(1e-3));
  const AudioClip warped = clip();
  CHECK(ops::autoWarpClips(r.doc(), one, AudioWarp::Tones, 90.0)->empty());  // a warped clip is left alone

  // Slide: the marker keeps its content tick, its frame moves, clamped strictly between its neighbours.
  commit(r.store, ops::addWarpMarker(r.doc(), c, clip().contentStart + kPpq / 2));
  commit(r.store, ops::addWarpMarker(r.doc(), c, clip().contentStart + kPpq));
  commit(r.store, ops::addWarpMarker(r.doc(), c, clip().contentStart + 2 * kPpq));
  const std::vector<WarpMarker> m = clip().markers;
  size_t h = r.store.history().cursor();
  commit(r.store, ops::slideWarpMarker(r.doc(), c, 1, m[1].sourceFrame + 500.0));
  CHECK(r.store.history().cursor() == h + 1);
  CHECK(clip().markers[1].content == m[1].content);
  CHECK(clip().markers[1].sourceFrame == m[1].sourceFrame + 500.0);
  CHECK(clip().markers[0] == m[0]);
  CHECK(clip().markers[2] == m[2]);
  commit(r.store, ops::slideWarpMarker(r.doc(), c, 1, m[2].sourceFrame + 1e6));
  CHECK(clip().markers[1].sourceFrame < m[2].sourceFrame);
  CHECK(clip().markers[1].sourceFrame > m[2].sourceFrame - 1e-6);
  commit(r.store, ops::slideWarpMarker(r.doc(), c, 0, -5000.0));
  CHECK(clip().markers[0].sourceFrame == 0.0);
  CHECK_FALSE(ops::slideWarpMarker(r.doc(), c, 7, 10.0).ok());

  // Warp From Here at the second marker, 132 BPM: earlier markers kept, later ones dropped, one beat later at 132.
  const std::vector<WarpMarker> m1 = clip().markers;
  h = r.store.history().cursor();
  commit(r.store, ops::warpFromHere(r.doc(), c, m1[1].content, 132.0));
  CHECK(r.store.history().cursor() == h + 1);
  REQUIRE(clip().markers.size() == 3);
  CHECK(clip().markers[0] == m1[0]);
  CHECK(clip().markers[1] == m1[1]);
  CHECK(clip().markers[2].content == m1[1].content + kPpq);
  CHECK(clip().markers[2].sourceFrame == doctest::Approx(m1[1].sourceFrame + 48000.0 * 60.0 / 132.0));
  // Past the last marker the slope is the last segment's: 132 BPM on.
  const double f4 = warpFrameAt(clip(), 48000.0, static_cast<double>(m1[1].content + 4 * kPpq));
  CHECK(f4 == doctest::Approx(m1[1].sourceFrame + 4.0 * 48000.0 * 60.0 / 132.0));
  // At a point before every marker: a lone marker, and the source BPM becomes the tempo.
  const double atStart = warpFrameAt(clip(), 48000.0, static_cast<double>(clip().contentStart));
  commit(r.store, ops::warpFromHere(r.doc(), c, clip().contentStart, 96.0));
  REQUIRE(clip().markers.size() == 1);
  CHECK(clip().sourceBpm == 96.0);
  CHECK(clip().markers[0].sourceFrame == atStart);  // where the start played

  // Warp as a 1-bar loop: the region the clip plays becomes exactly one bar, looped; the region's start frame stays.
  const double a = warpFrameAt(clip(), 48000.0, static_cast<double>(clip().contentStart));
  const double b = warpFrameAt(clip(), 48000.0, static_cast<double>(clip().contentStart + clip().length));
  h = r.store.history().cursor();
  commit(r.store, ops::warpAsLoop(r.doc(), c, 1));
  CHECK(r.store.history().cursor() == h + 1);
  CHECK(clip().loop.enabled);
  CHECK(clip().loop.end - clip().loop.start == kTicksPerWhole);
  CHECK(warpFrameAt(clip(), 48000.0, static_cast<double>(clip().loop.start)) == doctest::Approx(a));
  CHECK(warpFrameAt(clip(), 48000.0, static_cast<double>(clip().loop.end)) == doctest::Approx(b));
  CHECK(clip().length == kTicksPerWhole);
  CHECK_FALSE(ops::warpAsLoop(r.doc(), c, 1000).ok());  // 1000 bars in a few seconds: far above kMaxBpm

  // An Off clip warps as a loop in the given mode, its window spanning the bars.
  commit(r.store, ops::setWarpMode(r.doc(), one, AudioWarp::Off));
  const double w0 = clip().contentStartFrame, w1 = w0 + clip().seconds * 48000.0;
  commit(r.store, ops::warpAsLoop(r.doc(), c, 1, AudioWarp::Tones));
  CHECK(clip().warp == AudioWarp::Tones);
  CHECK(warpFrameAt(clip(), 48000.0, static_cast<double>(clip().loop.start)) == doctest::Approx(w0));
  CHECK(warpFrameAt(clip(), 48000.0, static_cast<double>(clip().loop.end)) == doctest::Approx(w1));
  (void)warped;
}

TEST_CASE("recording: the warp law through markers is piecewise linear and invertible") {
  const std::vector<WarpMarker> m = {{1000.0, 100}, {5000.0, 300}, {5500.0, 1300}};
  const double fpt = 2.0;
  CHECK(warpFrameAt(std::span<const WarpMarker>{}, fpt, 50.0) == 100.0);
  CHECK(warpFrameAt(std::span<const WarpMarker>(m.data(), 1), fpt, 150.0) == 1100.0);  // a lone marker: slope fpt
  CHECK(warpFrameAt(m, fpt, 100.0) == 1000.0);
  CHECK(warpFrameAt(m, fpt, 200.0) == 3000.0);
  CHECK(warpFrameAt(m, fpt, 800.0) == 5250.0);
  CHECK(warpFrameAt(m, fpt, 0.0) == -1000.0);    // before the first: the first segment's slope (20 / tick)
  CHECK(warpFrameAt(m, fpt, 2300.0) == 6000.0);  // after the last: the last segment's (0.5 / tick)
  for (double c : {-30.0, 100.0, 222.5, 300.0, 999.0, 4000.0})
    CHECK(warpContentAt(m, fpt, warpFrameAt(m, fpt, c)) == doctest::Approx(c));
}

TEST_CASE("recording: the input map — Keys gives degrees exactly as a drawn note resolves, Hz freezes Cents; .kbm, "
          "12-TET nearest, bends, the computer keyboard's degrees") {
  Rec r;
  IdSource& ids = r.store.ids();
  auto withTuning = [&](TuningDef def) {
    const TuningId id = ids.tuning();
    REQUIRE(r.store.dispatch(AddTuning{TuningSlot{id, "t", Colour{}, std::move(def)}}).ok());
    REQUIRE(r.store.dispatch(SetTrackTuning{r.midi, TuningAssignment::mainTuning(id)}).ok());
    return id;
  };
  auto mapOf = [&](NoteInput in) {
    auto m = buildInputPitchMap(r.doc(), r.midi, in);
    REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
    return *m;
  };

  // 12-EDO with A4 = 440: key 69 → 440 Hz.
  TuningDef e12;
  e12.referenceHz = 440.0 / std::exp2(9.0 / 12.0);
  const TuningId t12 = withTuning(e12);
  const InputPitchMapPtr k12 = mapOf({});
  CHECK(k12->at(69).mapped);
  CHECK(k12->at(69).id == tuning::PitchId::fromDegree(9, 0));
  CHECK(std::abs(k12->at(69).hz / 440.0 - 1.0) <= 1e-6);
  CHECK(k12->at(69).follow == PitchFollow::Degree);
  CHECK(k12->at(69).signal == t12);
  CHECK(k12->at(48).id == tuning::PitchId::fromDegree(0, -1));
  // Hz mode: the same pitch as an exact Cents identity (cents above the reference) that follows the reference.
  const InputPitchMapPtr h12 = mapOf({NoteInputMode::Hz, InputKeyMap::Tuning});
  REQUIRE(h12->at(69).id.kind() == tuning::PitchKind::Cents);
  CHECK(std::abs(h12->at(69).id.cents() - 900.0) <= 0.001);
  CHECK(std::abs(h12->at(69).hz / 440.0 - 1.0) <= 1e-9);
  // A bend at note-on is frozen in Hz mode, dropped in Keys mode.
  CHECK(std::abs(h12->atNoteOn(69, 50.0).id.cents() - 950.0) <= 0.001);
  CHECK(k12->atNoteOn(69, 50.0).id == k12->at(69).id);

  // 31-EDO: consecutive degrees from key 60.
  TuningDef e31;
  e31.system = tuning::Edo::make(31).value();
  withTuning(e31);
  const InputPitchMapPtr k31 = mapOf({});
  CHECK(k31->at(91).id == tuning::PitchId::fromDegree(0, 1));
  CHECK(k31->at(59).id == tuning::PitchId::fromDegree(30, -1));
  CHECK(k31->degreesPerEquave() == 31);
  // Hz mode: degree 0 equave 1 is exactly 1200 ¢; 12-TET nearest: key 72 (an octave up) is degree 31.
  CHECK(std::abs(mapOf({NoteInputMode::Hz, InputKeyMap::Tuning})->at(91).id.cents() - 1200.0) <= 0.001);
  const InputPitchMapPtr n31 = mapOf({NoteInputMode::Keys, InputKeyMap::TwelveTetNearest});
  CHECK(n31->at(72).id == tuning::PitchId::fromDegree(0, 1));
  CHECK(n31->at(64).id == tuning::PitchId::fromDegree(10, 0));  // 400 ¢ → 10 × 38.71 = 387.1 ¢ is nearest
  // 12-TET Hz: the key's 12-TET pitch whatever the tuning.
  const InputPitchMapPtr hz12 = mapOf({NoteInputMode::Hz, InputKeyMap::TwelveTetHz});
  CHECK(std::abs(hz12->at(69).hz / 440.0 - 1.0) <= 1e-9);
  CHECK(std::abs(hz12->at(81).hz / 880.0 - 1.0) <= 1e-9);
  // The computer keyboard's degrees go through the same path.
  CHECK(k31->pitchForDegree(31).id == k31->at(91).id);
  CHECK(k31->pitchForDegree(-1).id == k31->at(59).id);
  CHECK(k31->pitchForDegree(62).hz == doctest::Approx(k31->at(91).hz * 2.0));

  // MOS 5L2s: a Degree identity whose resolution (Hz, follow, signal, degree, noteDegree) is resolveTrackPitches'.
  TuningDef mos;
  mos.system = tuning::MosParams{};
  const TuningId tm = withTuning(mos);
  const InputPitchMapPtr km = mapOf({});
  const InputPitch& k = km->at(64);
  CHECK(k.id == tuning::PitchId::fromDegree(4, 0));
  Note drawn = doctest_fixture::makeNote(ids, 0, 4);
  REQUIRE(r.store.dispatch(AddClip{r.midi, doctest_fixture::makeClip(ids, 0, kBar, {drawn})}).ok());
  const auto pitches = resolveTrackPitches(r.doc(), r.midi);
  REQUIRE(pitches.ok());
  const NotePitch& np = pitches->clips.at(0).notes.at(0);
  REQUIRE(np.hz);
  CHECK(k.hz == *np.hz);
  CHECK(k.follow == np.follow);
  CHECK(k.follow == PitchFollow::Degree);
  CHECK(k.signal == np.signal);
  CHECK(k.signal == tm);
  CHECK(k.degree == np.degree);
  CHECK(k.equave == np.equave);
  CHECK(k.noteDegree == static_cast<float>(*np.keyDegree));

  // Scala with its .kbm (fixture: map size 4, middle 60, entries 0 1 x 3): the .kbm wins; 'x' is unmapped.
  withTuning(doctest_fixture::fixtureScaleDef());
  const InputPitchMapPtr ks = mapOf({});
  CHECK(ks->at(60).id == tuning::PitchId::fromDegree(0, 0));
  CHECK(ks->at(61).id == tuning::PitchId::fromDegree(1, 0));
  CHECK_FALSE(ks->at(62).mapped);
  CHECK(ks->at(63).id == tuning::PitchId::fromDegree(3, 0));
  CHECK(ks->at(62).follow == PitchFollow::Static);
  // "Consecutive" ignores the .kbm.
  CHECK(mapOf({NoteInputMode::Keys, InputKeyMap::Consecutive})->at(62).mapped);

  // The track's own setting; a build is cheap (CPU: one map per tuning / setting edit, off the audio thread).
  REQUIRE(r.store.dispatch(SetNoteInput{r.midi, NoteInput{NoteInputMode::Hz, InputKeyMap::Tuning}}).ok());
  auto own = buildInputPitchMap(r.doc(), r.midi);
  REQUIRE(own.ok());
  CHECK((*own)->input().mode == NoteInputMode::Hz);
  CHECK_FALSE(buildInputPitchMap(r.doc(), r.audio).ok());
  withTuning(mos);
  const auto t0 = std::chrono::steady_clock::now();
  constexpr int kBuilds = 50;
  for (int i = 0; i < kBuilds; ++i) REQUIRE(buildInputPitchMap(r.doc(), r.midi, NoteInput{}).ok());
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kBuilds;
  MESSAGE("input map build (MOS, Keys): " << us << " us");
  CHECK(us < 5000.0);  // loose: a regression guard, not a benchmark (the target is <= 50 us optimised)
}

TEST_CASE("recording: save -> load is deep-equal with every new field set, byte-stable, and the reader refuses what the "
          "commands refuse") {
  Rec r;
  IdSource& ids = r.store.ids();
  // Every T6.1 field away from its default.
  commit(r.store, ops::armTrack(r.doc(), r.midi, false));
  REQUIRE(r.store.dispatch(SetTrackMonitor{r.audio, MonitorMode::Off}).ok());
  REQUIRE(r.store.dispatch(SetTrackMonitor{r.midi, MonitorMode::In}).ok());
  REQUIRE(r.store.dispatch(SetTrackAudioInput{r.audio, AudioInput{3, 1}}).ok());
  REQUIRE(r.store.dispatch(SetTrackMidiInput{r.midi, MidiInput{MidiInputKind::Port, "Keystation 49", 2}}).ok());
  REQUIRE(r.store.dispatch(SetTrackDelay{r.audio, -7.25f}).ok());
  REQUIRE(r.store.dispatch(SetNoteInput{r.midi, NoteInput{NoteInputMode::Hz, InputKeyMap::TwelveTetNearest}}).ok());
  REQUIRE(r.store.dispatch(SetPunch{PunchRegion{kBar, 3 * kBar, true}}).ok());
  REQUIRE(r.store.dispatch(SetRecordSettings{RecordSettings{4, true, -9.5f, true, kPpq / 4, true}}).ok());  // T6.6: cue
  AudioClip a = r.audioClip(0, 1.9, 123.5);
  REQUIRE(r.store.dispatch(AddClip{r.audio, nullptr, std::make_shared<const AudioClip>(a)}).ok());
  const ClipRef ac{r.audio, a.id};
  commit(r.store, ops::setWarpMode(r.doc(), std::span<const ClipRef>(&ac, 1), AudioWarp::Texture));
  commit(r.store, ops::addWarpMarker(r.doc(), ac, kPpq));
  commit(r.store, ops::addWarpMarker(r.doc(), ac, 2 * kPpq + 3));
  commit(r.store, ops::setAudioClipWarpParams(r.doc(), std::span<const ClipRef>(&ac, 1),
                                              WarpParams{kPpq / 8, BeatsLoop::BackForward, 42.5f, 71.0f, 33.0f, 80.0f, 64.0f}));
  const ClipSlot at[] = {slotOf(r.audioClip(0, 1.0, 9000.0)), slotOf(r.audioClip(kBar, 0.5, 100.0))};
  commit(r.store, ops::addTakeLane(r.doc(), ids, r.audio, at, "Take A"));
  const LaneId ta = r.track(r.audio).takes.at(0)->id;
  commit(r.store, ops::compRegion(r.doc(), ids, r.audio, ta, kPpq / 2, kPpq));
  const ClipSlot nt[] = {slotOf(doctest_fixture::makeClip(ids, 0, kBar, {doctest_fixture::makeNote(ids, 0, 3)}))};
  commit(r.store, ops::addTakeLane(r.doc(), ids, r.midi, nt));
  commit(r.store, ops::setAuditionTake(r.doc(), r.midi, r.track(r.midi).takes.at(0)->id));

  const Project& p = r.doc();
  CHECK(p.punch != PunchRegion{});
  CHECK(p.record != RecordSettings{});
  const ClipSlot* comped = nullptr;
  for (const ClipSlot& s : r.track(r.audio).clips)
    if (s.take == ta) comped = &s;
  REQUIRE(comped != nullptr);
  CHECK(comped->audio->warp == AudioWarp::Off);  // a piece of the take's Off clip
  const LaneId auditioned = r.track(r.midi).auditionTake;
  REQUIRE(auditioned.valid());
  auto text = serializeProject(p);
  REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
  CHECK(text->find("\"schemaVersion\": 22") != std::string::npos);
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == p);
  const Track& la = *findTrackPtr(*back->project, r.audio);
  CHECK(la.mixer.audioIn == AudioInput{3, 1});
  CHECK(la.takes.at(0)->clips.size() == 2);
  bool texture = false;
  for (const ClipSlot& s : la.clips) texture = texture || (s.audio->warp == AudioWarp::Texture && s.audio->markers.size() == 2);
  CHECK(texture);
  auto again = serializeProject(*back->project);
  REQUIRE(again.ok());
  CHECK(*again == *text);

  // The reader refuses what the commands refuse (every message names the path).
  auto refused = [&](const std::string& from, const std::string& to) {
    std::string bad = *text;
    const size_t at = bad.find(from);
    REQUIRE(at != std::string::npos);
    bad.replace(at, from.size(), to);
    auto res = parseProject(bad);
    CHECK_FALSE(res.ok());
    if (!res.ok()) MESSAGE(res.error().message);
  };
  refused("\"monitor\": \"off\"", "\"monitor\": \"sometimes\"");
  refused("\"channels\": 1", "\"channels\": 3");
  refused("\"channel\": 2", "\"channel\": 17");
  refused("\"countInBars\": 4", "\"countInBars\": 3");
  refused("\"auditionTake\": " + std::to_string(auditioned.value), "\"auditionTake\": 4242");
  refused("\"beatsLoop\": \"backForward\"", "\"beatsLoop\": \"sideways\"");
  refused("\"punch\": {\"in\": " + std::to_string(kBar), "\"punch\": {\"in\": " + std::to_string(4 * kBar));
}

TEST_CASE("recording: time edits move take lanes with their track (the audition kept) and the punch region with them") {
  Rec r;
  IdSource& ids = r.store.ids();
  ClipPtr main = doctest_fixture::makeClip(ids, kBar, kBar, {doctest_fixture::makeNote(ids, 0, 0)});
  REQUIRE(r.store.dispatch(AddClip{r.midi, main}).ok());
  const ClipSlot nt[] = {slotOf(doctest_fixture::makeClip(ids, kBar, 2 * kBar, {doctest_fixture::makeNote(ids, kPpq, 2)}))};
  commit(r.store, ops::addTakeLane(r.doc(), ids, r.midi, nt));
  const ClipSlot at[] = {slotOf(r.audioClip(2 * kBar, 1.0, 500.0))};
  commit(r.store, ops::addTakeLane(r.doc(), ids, r.audio, at));
  const LaneId mt = r.track(r.midi).takes.at(0)->id;
  REQUIRE(r.store.dispatch(SetAuditionTake{r.midi, mt}).ok());
  REQUIRE(r.store.dispatch(SetPunch{PunchRegion{2 * kBar, 4 * kBar, true}}).ok());
  const std::vector<edit_test::Occ> heard = [&] {
    Track v = r.track(r.midi);
    v.clips = playedClips(v);
    return edit_test::played(v);
  }();

  commit(r.store, ops::insertTime(r.doc(), ids, kBar / 2, kBar));
  const Track& m = r.track(r.midi);
  REQUIRE(m.takes.size() == 1);
  CHECK(m.takes[0]->id == mt);
  CHECK(m.auditionTake == mt);  // still auditioned
  CHECK(m.takes[0]->clips.at(0).start() == 2 * kBar);
  CHECK(m.clips.at(0).start() == 2 * kBar);
  CHECK(r.track(r.audio).takes.at(0)->clips.at(0).start() == 3 * kBar);
  CHECK(r.doc().punch == PunchRegion{3 * kBar, 5 * kBar, true});
  Track v = m;
  v.clips = playedClips(m);
  std::vector<edit_test::Occ> shifted = heard;
  for (edit_test::Occ& o : shifted) o.start += kBar;
  CHECK(edit_test::played(v) == shifted);
  // Deleting across the take splits it as it would a clip of the main lane.
  commit(r.store, ops::deleteTime(r.doc(), ids, 3 * kBar, 3 * kBar + kBar / 2));
  CHECK(r.track(r.midi).takes.at(0)->clips.size() == 2);
}
