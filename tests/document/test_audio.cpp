// T5.3: audio tracks and audio clips in the document — commands, edit operations (one undo step each, overwrite on
// drop, the crossfade drop rule), the warp laws (a tempo edit moves an Off clip's end and leaves a Re-Pitch clip's
// length in ticks), split / trim keeping every source frame at its absolute time (checked through the lowered
// descriptions), time edits, the file format (schema 7) and its strict reader.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "edit_helpers.h"
#include "pq/document/audio_clip.h"
#include "pq/document/audio_sequence.h"
#include "pq/document/serialize.h"

using namespace pq;
using namespace pq::doc;
using edit_test::commit;
using edit_test::kBar;

namespace {

std::string fixturePath(const std::string& rel) { return std::string(PQ_TEST_DATA_DIR) + "/document/fixtures/" + rel; }

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A project with an audio track "Audio" (and a MIDI track) and one sample of 2 s at 48 kHz (stereo).
struct AudioProject {
  DocumentStore store;
  TrackId audio, audio2, midi;
  SampleId sample;
  AudioProject() {
    IdSource& ids = store.ids();
    midi = commit1(ops::createTrack(doc(), ids, TrackKind::Midi)).tracks.at(0);
    audio = commit1(ops::createTrack(doc(), ids, TrackKind::Audio, midi)).tracks.at(0);
    audio2 = commit1(ops::createTrack(doc(), ids, TrackKind::Audio, audio)).tracks.at(0);
    SampleRef s;
    s.id = ids.sample();
    s.path = "C:/samples/loop.wav";
    s.contentHash = 0x0123456789abcdefull;
    s.size = 384044;
    s.frames = 96000;
    s.rate = 48000.0;
    s.channels = 2;
    s.displayName = "loop";
    sample = s.id;
    REQUIRE(store.dispatch(AddSample{s}).ok());
  }
  const Project& doc() const { return *store.current(); }
  ops::Edit commit1(const Result<ops::Edit>& e) {
    commit(store, e);
    return *e;
  }
  const Track& track(TrackId id) const {
    const Track* t = findTrackPtr(doc(), id);
    REQUIRE(t);
    return *t;
  }
  const AudioClip& clip(TrackId t, ClipId c) const {
    const Track& tr = track(t);
    const int32_t i = findClipIndex(tr, c);
    REQUIRE(i >= 0);
    REQUIRE(tr.clips[static_cast<size_t>(i)].audio);
    return *tr.clips[static_cast<size_t>(i)].audio;
  }
  ClipRef drop(TrackId t, Ticks start, SampleId s = {}) {
    const ops::NewAudioClip nc{t, start, s.valid() ? s : sample, {}};
    const ops::Edit e = commit1(ops::addAudioClips(doc(), store.ids(), {}, std::span<const ops::NewAudioClip>(&nc, 1)));
    return e.clips.at(0);
  }
  AudioTrackDesc lowered(TrackId t) const {
    const auto path = findTrack(doc(), t);
    REQUIRE(path);
    return lowerAudioTrack(doc(), *path);
  }
};

// The source frame every clip of `a` plays at song time tau (nullopt: none); `b` the same: every probe agrees.
void sameFrames(const AudioTrackDesc& a, const TempoMap& ta, const AudioTrackDesc& b, const TempoMap& tb, double from,
                double to, double tol = 1e-6) {
  int compared = 0;
  for (int i = 0; i <= 997; ++i) {
    const double tau = from + (to - from) * (static_cast<double>(i) + 0.37) / 998.0;
    std::optional<double> fa, fb;
    for (const AudioClipDesc& c : a.clips)
      if (auto f = sourceFrameAt(c, ta, tau)) fa = f;
    for (const AudioClipDesc& c : b.clips)
      if (auto f = sourceFrameAt(c, tb, tau)) fb = f;
    REQUIRE(fa.has_value() == fb.has_value());
    if (!fa) continue;
    CHECK(std::abs(*fa - *fb) <= tol);
    ++compared;
  }
  CHECK(compared > 100);
}

}  // namespace

TEST_CASE("audio: Ctrl+T makes an audio track (one undo step, empty chain, effects only); sends work as on MIDI tracks") {
  AudioProject f;
  const Track& t = f.track(f.audio);
  CHECK(t.kind == TrackKind::Audio);
  CHECK(t.name == "1 Audio");
  CHECK(t.devices.empty());
  CHECK(f.track(f.audio2).name == "2 Audio");
  // Inserted after the given track.
  CHECK(f.doc().tracks.at(1)->id == f.audio);
  // No instrument on an audio track; audio effects are fine.
  CHECK_FALSE(ops::addDevice(f.doc(), f.store.ids(), f.audio, "BasicSynth", DeviceKind::Instrument).ok());
  commit(f.store, ops::addDevice(f.doc(), f.store.ids(), f.audio, "utility", DeviceKind::AudioEffect));
  CHECK(f.track(f.audio).devices.size() == 1);
  // Mixer: sends to a return, solo, mute.
  const TrackId ret = f.commit1(ops::createTrack(f.doc(), f.store.ids(), TrackKind::Return)).tracks.at(0);
  REQUIRE(f.store.dispatch(SetSendLevel{f.audio, ret, -6.0f}).ok());
  commit(f.store, ops::soloTrack(f.doc(), f.audio, false));
  CHECK(f.track(f.audio).mixer.solo);
  // Note clips cannot go on an audio track, audio clips not on a MIDI track.
  NoteClip nc;
  nc.id = f.store.ids().clip();
  CHECK_FALSE(f.store.dispatch(AddClip{f.audio, std::make_shared<const NoteClip>(nc)}).ok());
  const ops::NewAudioClip onMidi{f.midi, 0, f.sample, {}};
  CHECK_FALSE(ops::addAudioClips(f.doc(), f.store.ids(), {}, std::span<const ops::NewAudioClip>(&onMidi, 1)).ok());
}

TEST_CASE("audio: dropping clips overwrites what lies under them; several files are one undo step") {
  AudioProject f;
  IdSource& ids = f.store.ids();
  const ClipRef a = f.drop(f.audio, 0);  // 2 s = one bar at 120 BPM
  CHECK(f.clip(f.audio, a.clip).length == kBar);
  CHECK(f.clip(f.audio, a.clip).name == "loop");
  CHECK(f.clip(f.audio, a.clip).seconds == doctest::Approx(2.0));
  // Two new files at once, one of them over the right half of `a`: a new sample entry and one undo step.
  SampleRef s2;
  s2.id = ids.sample();
  s2.path = "C:/samples/hit.wav";
  s2.frames = 24000;
  s2.rate = 48000.0;
  s2.channels = 1;
  s2.displayName = "hit";
  const ops::NewAudioClip drops[] = {{f.audio, kBar / 2, s2.id, {}}, {f.audio2, 0, f.sample, "second"}};
  const size_t before = f.store.history().cursor();
  commit(f.store, ops::addAudioClips(f.doc(), ids, std::span<const SampleRef>(&s2, 1), drops));
  CHECK(f.store.history().cursor() == before + 1);
  REQUIRE(findSample(f.doc(), s2.id));
  // The new clip overwrites only what lies under it: `a` is cut around it, its tail keeps playing its own audio.
  const Track& t = f.track(f.audio);
  REQUIRE(t.clips.size() == 3);
  CHECK(t.clips[0].id == a.clip);
  CHECK(t.clips[0].end() == kBar / 2);
  CHECK(t.clips[1].start() == kBar / 2);
  CHECK(t.clips[1].length() == kBar / 4);  // 0.5 s
  CHECK(t.clips[1].audio->sample == s2.id);
  CHECK(t.clips[2].start() == 3 * kBar / 4);
  CHECK(t.clips[2].end() == kBar);
  CHECK(t.clips[2].audio->contentStartFrame == 72000.0);
  CHECK(f.track(f.audio2).clips.at(0).name() == "second");
  // An unknown sample is refused.
  const ops::NewAudioClip bad{f.audio, 0, SampleId{999999}, {}};
  CHECK_FALSE(ops::addAudioClips(f.doc(), ids, {}, std::span<const ops::NewAudioClip>(&bad, 1)).ok());
}

TEST_CASE("audio: a tempo edit moves the end of an Off clip and leaves a Re-Pitch clip's length in ticks") {
  AudioProject f;
  const ClipRef off = f.drop(f.audio, 0);
  const ClipRef rp = f.drop(f.audio2, 0);
  commit(f.store, ops::setAudioClipWarp(f.doc(), std::span<const ClipRef>(&rp, 1), AudioWarp::RePitch, 120.0));
  CHECK(f.clip(f.audio2, rp.clip).warp == AudioWarp::RePitch);
  CHECK(f.clip(f.audio2, rp.clip).length == kBar);
  ChangeSet cs;
  const Snapshot before = f.store.current();
  REQUIRE(f.store.dispatch(SetTempoMap{{TempoPoint{0, 60.0, TempoCurve::Step}}}).ok());
  const ChangeSet& c = f.store.lastChanges();
  CHECK(c.has(kChangeTimeline));
  CHECK(c.has(kChangeSequence));  // the Off clip's length changed
  CHECK_FALSE(c.structural());
  CHECK(f.clip(f.audio, off.clip).length == kBar / 2);   // 2 s at 60 BPM = 2 beats
  CHECK(f.clip(f.audio, off.clip).seconds == doctest::Approx(2.0));
  CHECK(f.clip(f.audio2, rp.clip).length == kBar);       // Re-Pitch: ticks
  // A ramp: the Off clip's end is where 2 s have passed.
  REQUIRE(f.store.dispatch(SetTempoMap{{TempoPoint{0, 90.0, TempoCurve::Linear}, TempoPoint{kBar, 180.0, TempoCurve::Step}}}).ok());
  const TempoMap& m = f.doc().timeline->tempoMap;
  CHECK(m.ticksToSeconds(clipEnd(f.clip(f.audio, off.clip))) == doctest::Approx(2.0).epsilon(1e-6));
  REQUIRE(f.store.undo());
  REQUIRE(f.store.undo());
  CHECK(f.store.current() == before);
}

TEST_CASE("audio: split and trim keep the frame that plays at every absolute time (Off, Re-Pitch, reverse, transpose)") {
  for (const bool rePitch : {false, true})
    for (const bool reverse : {false, true}) {
      CAPTURE(rePitch);
      CAPTURE(reverse);
      AudioProject f;
      REQUIRE(f.store.dispatch(SetTempoMap{{TempoPoint{0, 137.5, TempoCurve::Linear}, TempoPoint{4 * kBar, 97.0, TempoCurve::Step}}}).ok());
      const ClipRef r = f.drop(f.audio, kBar / 3);
      const std::span<const ClipRef> one(&r, 1);
      commit(f.store, ops::setAudioClipTranspose(f.doc(), one, AudioTranspose{TransposeUnit::Cents, 700.0, 0, {1}}));
      commit(f.store, ops::setAudioClipDetune(f.doc(), one, 13.0f));
      if (rePitch) {
        commit(f.store, ops::setAudioClipWarp(f.doc(), one, AudioWarp::RePitch, 93.0));
        commit(f.store, ops::resizeClips(f.doc(), f.store.ids(), one, ops::ClipEdge::End, 2 * kBar));
        commit(f.store, ops::setAudioClipLoop(f.doc(), r, ClipLoop{true, kPpq / 3, 3 * kPpq}));
      } else {
        // Start marker mid-file (a left trim), so the content start is fractional.
        commit(f.store, ops::resizeClips(f.doc(), f.store.ids(), one, ops::ClipEdge::Start, kPpq / 7));
      }
      if (reverse) commit(f.store, ops::setAudioClipsReverse(f.doc(), one, true));
      const AudioClip& c0 = f.clip(f.audio, r.clip);
      const TempoMap& tm = f.doc().timeline->tempoMap;
      const AudioTrackDesc before = f.lowered(f.audio);
      const double t0 = tm.ticksToSeconds(c0.start), t1 = tm.ticksToSeconds(clipEnd(c0));
      if (!rePitch) CHECK(before.clips.at(0).rateRatio == doctest::Approx(std::exp2(713.0 / 1200.0)));

      // Split at two points (one off the grid).
      const Ticks cuts[] = {c0.start + c0.length / 3 + 17, c0.start + (2 * c0.length) / 3};
      commit(f.store, ops::splitClips(f.doc(), f.store.ids(), one, cuts));
      REQUIRE(f.track(f.audio).clips.size() == 3);
      sameFrames(before, tm, f.lowered(f.audio), f.doc().timeline->tempoMap, t0, t1);

      // Trim both outer edges inward: what remains plays the same frames.
      const ClipRef first{f.audio, f.track(f.audio).clips.front().id};
      const ClipRef last{f.audio, f.track(f.audio).clips.back().id};
      commit(f.store, ops::resizeClips(f.doc(), f.store.ids(), std::span<const ClipRef>(&first, 1), ops::ClipEdge::Start, kPpq / 5));
      commit(f.store, ops::resizeClips(f.doc(), f.store.ids(), std::span<const ClipRef>(&last, 1), ops::ClipEdge::End, -kPpq / 4));
      const double u0 = tm.ticksToSeconds(f.track(f.audio).clips.front().start());
      const double u1 = tm.ticksToSeconds(f.track(f.audio).clips.back().end());
      sameFrames(before, tm, f.lowered(f.audio), f.doc().timeline->tempoMap, u0, u1 - 1e-9);
    }
}

TEST_CASE("audio: Off at rate 1 is exact on frame-aligned edits") {
  AudioProject f;  // 120 BPM: a 16th = 6000 frames at 48 kHz
  const ClipRef r = f.drop(f.audio, kBar);
  const Ticks sixteenth = kTicksPerWhole / 16;
  const Ticks cut = kBar + 3 * sixteenth;
  commit(f.store, ops::splitClips(f.doc(), f.store.ids(), std::span<const ClipRef>(&r, 1), std::span<const Ticks>(&cut, 1)));
  const Track& t = f.track(f.audio);
  REQUIRE(t.clips.size() == 2);
  CHECK(t.clips[1].audio->contentStartFrame == 18000.0);
  CHECK(t.clips[0].audio->seconds == 0.375);
  CHECK(t.clips[1].audio->seconds == 1.625);
  CHECK(t.clips[1].end() == 2 * kBar);
}

TEST_CASE("audio: the crossfade drop rule, and every audio edit is one undo step of Sequence class") {
  AudioProject f;
  const ClipRef a = f.drop(f.audio, 0);
  const ClipRef b = f.drop(f.audio, kBar);  // adjacent: a ends at kBar
  // Not adjacent → refused.
  const ClipRef c = f.drop(f.audio2, kBar / 2);
  CHECK_FALSE(ops::setAudioClipCrossfade(f.doc(), c, kPpq).ok());
  commit(f.store, ops::setAudioClipCrossfade(f.doc(), b, kPpq / 2));
  CHECK(f.clip(f.audio, b.clip).crossfadeIn == kPpq / 2);
  CHECK(f.store.lastChanges().sequenceOnly());
  // Clamped to both clips.
  commit(f.store, ops::setAudioClipCrossfade(f.doc(), b, 100 * kBar));
  CHECK(f.clip(f.audio, b.clip).crossfadeIn == kBar);
  commit(f.store, ops::setAudioClipCrossfade(f.doc(), b, kPpq / 2));
  // Moving both together keeps it; moving the left one away drops it in the same step (undo restores it).
  const ClipRef both[] = {a, b};
  commit(f.store, ops::moveClipsBy(f.doc(), f.store.ids(), both, kBar));
  CHECK(f.clip(f.audio, b.clip).crossfadeIn == kPpq / 2);
  const Snapshot adjacent = f.store.current();
  commit(f.store, ops::moveClipsBy(f.doc(), f.store.ids(), std::span<const ClipRef>(&a, 1), -kPpq));
  CHECK(f.clip(f.audio, b.clip).crossfadeIn == 0);
  REQUIRE(f.store.undo());
  CHECK(f.store.current() == adjacent);
  CHECK(f.clip(f.audio, b.clip).crossfadeIn == kPpq / 2);
  // Deleting the left clip drops it too.
  commit(f.store, ops::deleteClips(f.doc(), std::span<const ClipRef>(&a, 1)));
  CHECK(f.clip(f.audio, b.clip).crossfadeIn == 0);
  REQUIRE(f.store.undo());

  // Each audio setting: one step, Sequence only (no compile), clamped.
  const std::span<const ClipRef> one(&b, 1);
  commit(f.store, ops::setAudioClipGain(f.doc(), one, 40.0f));
  CHECK(f.clip(f.audio, b.clip).gainDb == kMaxClipGainDb);
  CHECK(f.store.lastChanges().sequenceOnly());
  commit(f.store, ops::setAudioClipTranspose(f.doc(), one, AudioTranspose{TransposeUnit::Steps, 0.0, 7, {1}}));
  CHECK(f.clip(f.audio, b.clip).transpose.steps == 7);
  CHECK(f.store.lastChanges().sequenceOnly());
  CHECK_FALSE(ops::setAudioClipTranspose(f.doc(), one, AudioTranspose{TransposeUnit::Ratio, 0.0, 0, tuning::Rational(100, 1)}).ok());
  commit(f.store, ops::setAudioClipTranspose(f.doc(), one, AudioTranspose{TransposeUnit::Ratio, 0.0, 0, tuning::Rational(3, 2)}));
  commit(f.store, ops::setAudioClipDetune(f.doc(), one, -99.0f));
  CHECK(f.clip(f.audio, b.clip).detuneCents == -50.0f);
  commit(f.store, ops::setAudioClipsReverse(f.doc(), one, true));
  commit(f.store, ops::setAudioClipFades(f.doc(), b, AudioFade{kPpq, 0.5f}, AudioFade{10 * kBar, -2.0f}));
  CHECK(f.clip(f.audio, b.clip).fadeIn == AudioFade{kPpq, 0.5f});
  CHECK(f.clip(f.audio, b.clip).fadeOut == AudioFade{kBar - kPpq, -1.0f});
  CHECK(f.store.lastChanges().sequenceOnly());
  CHECK_FALSE(ops::setAudioClipLoop(f.doc(), b, ClipLoop{true, 0, kPpq}).ok());  // Off: no loop
  commit(f.store, ops::setAudioClipWarp(f.doc(), one, AudioWarp::RePitch, 100.0));
  CHECK(f.store.lastChanges().sequenceOnly());
  commit(f.store, ops::setAudioClipLoop(f.doc(), b, ClipLoop{true, 0, kPpq}));
  // Rename, colour, deactivate (generic edits).
  REQUIRE(f.store.dispatch(RenameClip{b, "renamed"}).ok());
  CHECK(f.store.lastChanges().metaOnly());
  REQUIRE(f.store.dispatch(SetClipColour{b, Colour{0x112233}}).ok());
  REQUIRE(f.store.dispatch(SetClipActive{b, false}).ok());
  CHECK(f.clip(f.audio, b.clip).name == "renamed");
  CHECK(f.clip(f.audio, b.clip).colour == Colour{0x112233});
  CHECK_FALSE(f.clip(f.audio, b.clip).active);
  CHECK_FALSE(f.lowered(f.audio).clips.at(1).playable);
}

TEST_CASE("audio: generic clip edits — move to another audio track, Alt-copy, duplicate, delete; never onto MIDI") {
  AudioProject f;
  IdSource& ids = f.store.ids();
  const ClipRef a = f.drop(f.audio, 0);
  const ops::ClipMove mv{a, f.audio2, kBar};
  commit(f.store, ops::moveClips(f.doc(), ids, std::span<const ops::ClipMove>(&mv, 1)));
  CHECK(f.track(f.audio).clips.empty());
  REQUIRE(f.track(f.audio2).clips.size() == 1);
  CHECK(f.track(f.audio2).clips[0].start() == kBar);
  const ClipRef moved{f.audio2, a.clip};
  const ops::ClipMove toMidi{moved, f.midi, 0};
  CHECK_FALSE(ops::moveClips(f.doc(), ids, std::span<const ops::ClipMove>(&toMidi, 1)).ok());
  ops::MoveClipsOptions copy;
  copy.copy = true;
  const ops::ClipMove cp{moved, f.audio, 0};
  const ops::Edit e = f.commit1(ops::moveClips(f.doc(), ids, std::span<const ops::ClipMove>(&cp, 1), copy));
  REQUIRE(f.track(f.audio).clips.size() == 1);
  CHECK(f.track(f.audio).clips[0].id != a.clip);
  CHECK(f.track(f.audio).clips[0].audio->sample == f.sample);
  commit(f.store, ops::duplicateClips(f.doc(), ids, std::span<const ClipRef>(&e.clips.at(0), 1)));
  CHECK(f.track(f.audio).clips.size() == 2);
  CHECK(f.track(f.audio).clips[1].start() == kBar);
  commit(f.store, ops::deleteClips(f.doc(), std::span<const ClipRef>(&moved, 1)));
  CHECK(f.track(f.audio2).clips.empty());
  // Consolidating audio clips is not supported yet (it renders).
  CHECK_FALSE(ops::consolidateClips(f.doc(), ids, std::span<const ClipRef>(&e.clips.at(0), 1)).ok());
  // Unused samples stay in the table; a used one cannot be removed.
  CHECK_FALSE(f.store.dispatch(RemoveSample{f.sample}).ok());
}

TEST_CASE("audio: time edits move and cut audio clips like note clips") {
  AudioProject f;
  IdSource& ids = f.store.ids();
  const ClipRef a = f.drop(f.audio, 0);
  const AudioTrackDesc before = f.lowered(f.audio);
  const TempoMap tm = f.doc().timeline->tempoMap;
  // Insert a bar at half a bar: the clip is cut, its second half moves a bar later.
  commit(f.store, ops::insertTime(f.doc(), ids, kBar / 2, kBar));
  const Track& t = f.track(f.audio);
  REQUIRE(t.clips.size() == 2);
  CHECK(t.clips[0].end() == kBar / 2);
  CHECK(t.clips[1].start() == kBar + kBar / 2);
  CHECK(t.clips[1].end() == 2 * kBar);
  CHECK(t.clips[1].audio->contentStartFrame == 48000.0);
  // Delete it again: the halves join up and play as before.
  commit(f.store, ops::deleteTime(f.doc(), ids, kBar / 2, kBar + kBar / 2));
  sameFrames(before, tm, f.lowered(f.audio), f.doc().timeline->tempoMap, 0.0, 2.0 - 1e-9);
  // Duplicate time copies the slice.
  commit(f.store, ops::duplicateTime(f.doc(), ids, 0, kBar / 4));
  CHECK(f.track(f.audio).clips.size() == 4);
  (void)a;
}

TEST_CASE("audio: Steps transposes resolve in the clip's tuning and follow its definition") {
  AudioProject f;
  const ClipRef r = f.drop(f.audio, 0);
  commit(f.store, ops::setAudioClipTranspose(f.doc(), std::span<const ClipRef>(&r, 1),
                                             AudioTranspose{TransposeUnit::Steps, 0.0, 7, {1}}));
  CHECK(f.lowered(f.audio).clips.at(0).rateRatio == doctest::Approx(std::exp2(700.0 / 1200.0)));
  TuningDef d;
  d.system = *tuning::Edo::make(31);
  REQUIRE(f.store.dispatch(SetClipTuning{r, TuningAssignment::privateTuning(d)}).ok());
  CHECK(f.store.lastChanges().sequenceOnly());
  CHECK(f.lowered(f.audio).clips.at(0).rateRatio == doctest::Approx(std::exp2(7.0 / 31.0)));
}

// ---- File format (schema 7) ------------------------------------------------------------------------------------

namespace {

// Every audio field set at least once to a non-default value.
struct RichAudio : AudioProject {
  RichAudio() {
    IdSource& ids = store.ids();
    REQUIRE(store.dispatch(SetTempoMap{{TempoPoint{0, 120.0, TempoCurve::Linear}, TempoPoint{2 * kBar, 90.0, TempoCurve::Step}}}).ok());
    SampleRef s2;
    s2.id = ids.sample();
    s2.path = "samples/imported/Kick (2).flac";
    s2.contentHash = 0xfedcba9876543210ull;
    s2.size = 1234567;
    s2.frames = 44100;
    s2.rate = 44100.0;
    s2.channels = 1;
    s2.displayName = "Kick \"808\" — ünïcode";
    REQUIRE(store.dispatch(AddSample{s2}).ok());
    const ClipRef a = drop(audio, 0);
    const ClipRef b = drop(audio, clipEnd(clip(audio, a.clip)), s2.id);
    const ClipRef c = drop(audio2, kBar);
    const std::span<const ClipRef> one(&a, 1), two(&b, 1), three(&c, 1);
    commit(store, ops::setAudioClipGain(doc(), one, -3.5f));
    commit(store, ops::setAudioClipTranspose(doc(), one, AudioTranspose{TransposeUnit::Cents, -1250.25, 0, {1}}));
    commit(store, ops::setAudioClipDetune(doc(), one, 12.5f));
    commit(store, ops::setAudioClipFades(doc(), a, AudioFade{kPpq / 3, 0.25f}, AudioFade{kPpq / 2, -0.75f}));
    commit(store, ops::resizeClips(doc(), store.ids(), one, ops::ClipEdge::Start, kPpq / 7));
    commit(store, ops::setAudioClipTranspose(doc(), two, AudioTranspose{TransposeUnit::Ratio, 0.0, 0, tuning::Rational(5, 4)}));
    commit(store, ops::setAudioClipsReverse(doc(), two, true));
    commit(store, ops::setAudioClipCrossfade(doc(), b, kPpq / 4));
    commit(store, ops::setAudioClipWarp(doc(), three, AudioWarp::RePitch, 87.5));
    commit(store, ops::setAudioClipLoop(doc(), c, ClipLoop{true, kPpq, 3 * kPpq}));
    commit(store, ops::setAudioClipTranspose(doc(), three, AudioTranspose{TransposeUnit::Steps, 0.0, -5, {1}}));
    TuningDef d;
    d.system = *tuning::Edo::make(19);
    REQUIRE(store.dispatch(SetClipTuning{c, TuningAssignment::privateTuning(d)}).ok());
    REQUIRE(store.dispatch(SetClipActive{c, false}).ok());
    REQUIRE(store.dispatch(RenameClip{c, "re-pitched"}).ok());
  }
};

}  // namespace

TEST_CASE("audio: fixtures/audio — save → load is deep-equal with every field, byte-stable") {
  RichAudio f;
  Result<std::string> text = serializeProject(f.doc(), f.store.view());
  REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
  CHECK(text->rfind("{\n  \"schemaVersion\": 9,", 0) == 0);
  const std::string path = fixturePath("audio/project.pqd");
  if (std::getenv("PQ_UPDATE_FIXTURES")) {
    std::ofstream(path, std::ios::binary) << *text;
    MESSAGE("rewrote " << path);
  }
  CHECK(readFile(path) == *text);
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == f.doc());
  Result<LoadedProject> file = loadProject(fixturePath("audio"));
  REQUIRE_MESSAGE(file.ok(), (file.ok() ? std::string() : file.error().message));
  CHECK(*file->project == f.doc());
  // An Off clip's length in the file is re-derived from the tempo map.
  std::string edited = *text;
  const std::string key = "\"length\": " + std::to_string(f.track(f.audio).clips[0].length());
  const size_t at = edited.find(key);
  REQUIRE(at != std::string::npos);
  edited.replace(at, key.size(), "\"length\": 5");
  Result<LoadedProject> rederived = parseProject(edited);
  REQUIRE(rederived.ok());
  CHECK(*rederived->project == f.doc());
}

TEST_CASE("audio: strict reader errors name the path") {
  RichAudio f;
  const std::string text = *serializeProject(f.doc(), f.store.view());
  auto broken = [&](const std::string& from, const std::string& to) {
    std::string t = text;
    const size_t at = t.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, from);
    t.replace(at, from.size(), to);
    return parseProject(t);
  };
  const std::string sample = "\"sample\": " + std::to_string(f.sample.value);
  Result<LoadedProject> r = broken(sample, "\"sample\": 999");
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().message.find("$.tracks[1].clips[0].sample: unknown sample id 999") == 0);
  CHECK_FALSE(broken("\"warp\": \"off\"", "\"warp\": \"beats\"").ok());
  CHECK_FALSE(broken("\"kind\": \"audio\", \"id\"", "\"kind\": \"midi\", \"id\"").ok());
  CHECK_FALSE(broken("\"transpose\": {\"cents\"", "\"transpose\": {\"steps\": 1, \"cents\"").ok());
  CHECK_FALSE(broken("\"seconds\": ", "\"seconds\": -").ok());
  CHECK_FALSE(broken("\"frames\": 96000", "\"frames\": 0").ok());
  CHECK_FALSE(broken("\"hash\": \"0123456789abcdef\"", "\"hash\": \"xyz\"").ok());
  // An audio clip on a MIDI track, a note clip's key on an audio clip.
  CHECK_FALSE(broken("\"kind\": \"audio\",\n      \"name\": \"1 Audio\"", "\"kind\": \"midi\",\n      \"name\": \"1 Audio\"").ok());
  CHECK_FALSE(broken("\"crossfadeIn\": 0,", "\"crossfadeIn\": 0, \"notes\": [],").ok());
}

TEST_CASE("audio: 10k audio clips round-trip") {
  AudioProject f;
  IdSource& ids = f.store.ids();
  Batch b{"many", {}};
  for (int i = 0; i < 10000; ++i) {
    AudioClip c;
    c.id = ids.clip();
    c.name = "c";
    c.start = static_cast<Ticks>(i) * kBar;
    c.sample = f.sample;
    c.seconds = 1.5;
    c.contentStartFrame = 0.25 * i;
    c.gainDb = -0.5f * static_cast<float>(i % 7);
    b.commands.push_back(AddClip{i % 2 ? f.audio : f.audio2, nullptr, std::make_shared<const AudioClip>(c)});
  }
  REQUIRE(f.store.dispatch(b).ok());
  Result<std::string> text = serializeProject(f.doc());
  REQUIRE(text.ok());
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE(back.ok());
  CHECK(*back->project == f.doc());
}
