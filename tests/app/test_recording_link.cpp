// T6.7: recording through the playback link (pq/app/project_link.h "Recording"): an armed audio track's input is
// monitored (Auto, stopped) and recorded to a WAV bit for bit, its take placed so every input frame plays on the song
// sample it was played against (record latency 0 / 256 / 1000 frames, with and without a PDC total of 480, checked on
// the clip and in a render); 4 loop passes → 4 take lanes (contiguous regions of one file, each the whole loop, with
// and without the punch = the loop: M6 review M6) + the last in the main lane; punch records exactly [in, out) and ends
// by itself; notes in both note worlds (Keys: the map's Degree; Hz: the frozen Cents, a note-on bend within 0.001 cent)
// on 31-EDO and a MOS, kept as played across a mode / tuning change before Stop (M4 / M5); overdub merges, no overdub
// makes takes; one undo step per session; RtScope over the recording blocks. Two same-named tracks on one input write
// two files (L11). Untitled projects' recordings (C1 / H1): a folder per untitled document, a take saved while
// recording, record / undo / Save As / redo, Save As into the unsaved folder refused, an opened project inside it.
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/input_map.h"
#include "pq/app/recording.h"
#include "pq/media/wav_file.h"
#include "pq/views/view_context.h"

#include <windows.h>  // GetCurrentProcessId (L13)

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

// Input channel c at engine frame n: distinct values, exact in float (the WAV is compared bit for bit).
float source(uint32_t c, int64_t n) {
  return (c == 0 ? 1.0f : -0.5f) * static_cast<float>((n % (1 << 20)) + 1) * (1.0f / static_cast<float>(1 << 20));
}

struct Rig {
  DocumentStore store{playback_fixture::unwiredDefaultProject()};
  std::unique_ptr<ProjectLink> link;
  ListenerId sub = 0;
  fs::path dir;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  bool silentInput = false;

  Rig() {
    std::random_device rd;
    dir = fs::temp_directory_path() / ("pq_t67_" + std::to_string(rd()));
  }
  ~Rig() {
    if (link) store.unsubscribe(sub);
    link.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void start() {
    LinkConfig c;
    c.compileThread = false;
    c.jit = false;
    c.sleepAfterMs = 0.0f;
    c.unsavedDir = (dir / "recorded").string();
    auto made = ProjectLink::create(c, store.current());
    REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
    link = std::move(made.value());
    link->setRecordStore(&store);
    sub = store.subscribe([this](const StoreEvent& e) { link->onDocumentChange(e); });
  }
  void dispatch(const Command& c) { REQUIRE(store.dispatch(c).ok()); }
  void apply(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  }
  TrackId audioTrack(AudioInput in) {
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio);
    REQUIRE(t.ok());
    dispatch(t->command);
    const TrackId id = t->tracks.at(0);
    if (!in.none()) dispatch(SetTrackAudioInput{id, in});
    playback_fixture::wireSeries(store);  // its source into its mixer (the fixture project is unwired)
    return id;
  }
  void arm(TrackId t) { apply(ops::armTrack(*store.current(), t, true)); }
  // One engine block with the input (source() at engine time, or silence), under RtScope.
  void block() {
    std::vector<float> in0(kBlock), in1(kBlock);
    for (uint32_t f = 0; f < kBlock; ++f) {
      in0[f] = silentInput ? 0.0f : source(0, time + f);
      in1[f] = silentInput ? 0.0f : source(1, time + f);
    }
    const float* ins[2] = {in0.data(), in1.data()};
    float* outs[2] = {l.data(), r.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link->engine().process(engine::AudioBlock{ins, 2, outs, 2, kBlock, time});
    }
    time += kBlock;
  }
  void run(uint32_t blocks, std::vector<float>* out = nullptr) {
    for (uint32_t b = 0; b < blocks; ++b) {
      block();
      if (out != nullptr) out->insert(out->end(), l.begin(), l.end());
      if ((b & 15) == 15) (void)link->playhead();  // the UI's poll: notes, punch-out, the end of a session
      if ((b & 63) == 63) (void)link->recording().pump();  // the rig runs far faster than real time: drain here too,
                                                           // or a loaded machine lets the 4-s rings overflow
      if ((b & 255) == 255) link->collect();
    }
    (void)link->playhead();
  }
  const Track& track(TrackId id) const {
    const Track* t = findTrackPtr(*store.current(), id);
    REQUIRE(t != nullptr);
    return *t;
  }
  const SampleRef& sampleOf(const AudioClip& c) const {
    const SampleRef* s = findSample(*store.current(), c.sample);
    REQUIRE(s != nullptr);
    return *s;
  }
  int64_t sampleAt(Ticks t) const { return store.current()->timeline->tempoMap.ticksToSamples(t, kRate); }
};

media::WavData readTake(const Rig& r, const AudioClip& c) {
  const SampleRef& s = r.sampleOf(c);
  CHECK(isAbsoluteSamplePath(s.path));  // an untitled project records into the unsaved folder
  auto w = media::readWav(fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.path.data()), s.path.size())));
  REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
  CHECK(w.value().sampleRate == 48000);
  CHECK(static_cast<int64_t>(w.value().frames()) == s.frames);
  return std::move(w.value());
}

double rms(const std::vector<float>& x, size_t from) {
  double s = 0.0;
  for (size_t i = from; i < x.size(); ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(std::max<size_t>(1, x.size() - from)));
}

}  // namespace

TEST_CASE("T6.7 recording: an audio input, monitored and recorded bit for bit, lands on its song samples after the "
          "record latency (0 / 256 / 1000, PDC 0 / 480)") {
  for (const uint32_t lat : {0u, 256u, 1000u}) {
    for (const bool pdc : {false, true}) {
      CAPTURE(lat);
      CAPTURE(pdc);
      Rig r;
      const TrackId t = r.audioTrack(AudioInput{0, 1});  // mono: device input 1
      if (pdc) {  // another track 10 ms early: everything else (the recorded track, the output) is 480 late
        const TrackId other = r.audioTrack(AudioInput{});
        r.dispatch(SetTrackDelay{other, -10.0f});
      }
      r.arm(t);
      r.start();
      r.link->flush();
      r.link->engine().recorder().setDeviceLatency(lat / 2, lat - lat / 2);
      // Stopped and armed, Monitor Auto: the input is heard.
      std::vector<float> mon;
      r.run(200, &mon);
      CHECK(r.link->outputLatency() == (pdc ? 480u : 0u));
      CHECK(rms(mon, mon.size() - 4800) > 0.005);  // the input ramps to ≈ 0.012 by then
      // Record 4 s from 0 (no count-in), then Stop.
      r.link->record(0);
      const int64_t e0 = r.time;  // the Record command applies at this block's first frame
      r.run(3000);
      r.link->stop();
      r.run(4);
      REQUIRE(r.link->recordCommits() == 1);
      const Track& tr = r.track(t);
      REQUIRE(tr.takes.size() == 1);
      REQUIRE(tr.clips.size() == 1);
      REQUIRE(tr.clips[0].audio != nullptr);
      const AudioClip& c = *tr.clips[0].audio;
      CHECK(c.warp == AudioWarp::Off);
      CHECK(tr.clips[0].take == tr.takes[0]->id);
      REQUIRE(tr.takes[0]->clips.size() == 1);
      CHECK(tr.takes[0]->clips[0].audio->contentStartFrame == c.contentStartFrame);
      // The file: what was played, bit for bit: frame f was played against song sample f and captured `offset` later
      // (the input at engine time e0 + offset + f); the frames captured before were played before the take began, and
      // the last `offset` played before Stop were not captured yet.
      const int64_t off = int64_t{lat} + (pdc ? 480 : 0);
      const media::WavData w = readTake(r, c);
      REQUIRE(w.numChannels() == 1);
      REQUIRE(static_cast<int64_t>(w.frames()) == 192000 - off);
      for (int64_t f = 0; f < 192000 - off; ++f)
        if (w.channels[0][f] != source(0, e0 + off + f)) FAIL("file frame " << f << " differs");
      CHECK(c.start == 0);
      CHECK(r.sampleAt(c.start) == 0);
      CHECK(c.contentStartFrame == 0.0);
      CHECK(std::llround(c.seconds * kRate) == 192000 - off);
      // A render: the track plays its take with the input muted (Monitor Off): output frame k (PDC total late) is
      // file frame k, times the track's constant gain.
      r.dispatch(SetTrackMonitor{t, MonitorMode::Off});
      r.silentInput = true;
      REQUIRE(r.link->waitForSamples());  // the new file is loaded into the pool (a load plays silence until then)
      r.run(100);
      r.link->play(0);
      std::vector<float> out;
      r.run(2000, &out);
      const size_t lag = pdc ? 480 : 0;
      const size_t ref = 50000;
      const double g = out[ref + lag] / w.channels[0][ref];
      CHECK(std::fabs(g) > 0.3);
      double worst = 0.0;
      for (size_t k = 2000; k + lag < out.size() && k < w.frames() && k < 120000; ++k)
        worst = std::max(worst, std::fabs(out[k + lag] - g * w.channels[0][k]));
      CHECK(worst < 1e-6);
      r.link->stop();
      r.run(2);
    }
  }
}

TEST_CASE("T6.7b recording: the manual record offset (Preferences, ms) moves the take by exactly its frames") {
  // Device latency 256; offsets +2.5 ms = +120 frames, -1 ms = -48 frames, 0. The file is the same in every case; only
  // the take's placement in it moves by the offset.
  for (const double ms : {0.0, 2.5, -1.0}) {
    CAPTURE(ms);
    Rig r;
    const TrackId t = r.audioTrack(AudioInput{0, 1});
    r.arm(t);
    r.start();
    r.link->flush();
    r.link->engine().recorder().setDeviceLatency(128, 128);
    r.link->setRecordOffsetMs(ms);
    r.run(10);
    r.link->record(0);
    const int64_t e0 = r.time;
    r.run(1500);
    r.link->stop();
    r.run(4);
    REQUIRE(r.link->recordCommits() == 1);
    const Track& tr = r.track(t);
    REQUIRE(tr.clips.size() == 1);
    const AudioClip& c = *tr.clips[0].audio;
    const int64_t off = 256 + std::llround(ms * 48.0);
    CHECK(r.link->engine().recorder().latencyOffset(0) == off);
    CHECK(c.start == 0);
    CHECK(c.contentStartFrame == 0.0);
    CHECK(std::llround(c.seconds * kRate) == 96000 - off);
    const media::WavData w = readTake(r, c);
    REQUIRE(static_cast<int64_t>(w.frames()) == 96000 - off);
    CHECK(w.channels[0][0] == source(0, e0 + off));  // song sample 0 plays the frame captured `off` later
  }
  // A sample-rate change keeps the offset in ms (its frames follow the rate).
  Rig r;
  r.start();
  r.link->setRecordOffsetMs(2.5);
  REQUIRE(r.link->setSampleRate(96000.0).ok());
  CHECK(r.link->engine().recorder().latencyOffset(0) == 240);
}

TEST_CASE("T6.7 recording: 4 loop passes make 4 take lanes (contiguous regions of one file), the last also in main") {
  for (const bool punch : {false, true}) {  // M6 review M6: the punch = the loop region keeps every pass whole too
  CAPTURE(punch);
  Rig r;
  const TrackId t = r.audioTrack(AudioInput{1, 1});  // device input 2
  r.arm(t);
  if (punch) r.dispatch(SetPunch{PunchRegion{0, 4 * kPpq, true}});
  r.start();
  r.link->flush();
  r.link->engine().recorder().setDeviceLatency(128, 128);
  r.link->setLoop(views::TransportLoop{0, 4 * kPpq, true});  // 1 bar = 96 000 samples at 120 BPM
  r.run(10);
  r.link->record(0);
  const int64_t e0 = r.time;
  r.run(6000);  // exactly 4 × 96 000 frames
  r.link->stop();
  r.run(4);
  REQUIRE(r.link->recordCommits() == 1);
  const Track& tr = r.track(t);
  REQUIRE(tr.takes.size() == 4);
  REQUIRE(tr.clips.size() == 1);
  const AudioClip& main = *tr.clips[0].audio;
  CHECK(tr.clips[0].take == tr.takes[3]->id);
  double cf = 0.0;
  for (size_t k = 0; k < 4; ++k) {
    CAPTURE(k);
    CHECK(tr.takes[k]->name == "Take " + std::to_string(k + 1));
    REQUIRE(tr.takes[k]->clips.size() == 1);
    const AudioClip& c = *tr.takes[k]->clips[0].audio;
    CHECK(c.sample == main.sample);  // one continuous file for the session
    CHECK(c.start == 0);             // every pass covers the loop from its start
    CHECK(c.contentStartFrame == cf);  // contiguous: each take begins where the one before ended
    const int64_t len = std::llround(c.seconds * kRate);
    // Every pass whole; the last ends `offset` early (what was played then is captured after Stop).
    CHECK(len == (k < 3 ? 96000 : 96000 - 256));
    cf += static_cast<double>(len);
  }
  CHECK(main.contentStartFrame == tr.takes[3]->clips[0].audio->contentStartFrame);
  CHECK(main.seconds == tr.takes[3]->clips[0].audio->seconds);
  CHECK(r.link->engine().recorder().droppedFrames() == 0);
  const media::WavData w = readTake(r, main);
  REQUIRE(w.frames() == 4 * 96000 - 256);
  for (int64_t f = 0; f < 4 * 96000 - 256; ++f)
    if (w.channels[0][f] != source(1, e0 + 256 + f)) FAIL("file frame " << f << " differs");
  // One undo step takes the whole session back.
  REQUIRE(r.store.undo());
  CHECK(r.track(t).takes.empty());
  CHECK(r.track(t).clips.empty());
  }
}

TEST_CASE("T6.7 recording: punch records exactly [in, out) of what was played, and ends by itself") {
  Rig r;
  const TrackId t = r.audioTrack(AudioInput{0, 2});  // stereo: inputs 1-2
  r.arm(t);
  r.dispatch(SetPunch{PunchRegion{kPpq, 3 * kPpq, true}});  // song samples [24 000, 72 000)
  r.start();
  r.link->flush();
  r.link->engine().recorder().setDeviceLatency(100, 156);
  r.run(10);
  r.link->record(0);
  const int64_t e0 = r.time;
  r.run(1700);  // to song 108 800: past the punch-out (the session ends there, playback goes on)
  CHECK(r.link->playhead().playing);
  REQUIRE(r.link->recordCommits() == 1);
  CHECK_FALSE(r.link->playhead().recording);
  const Track& tr = r.track(t);
  REQUIRE(tr.clips.size() == 1);
  const AudioClip& c = *tr.clips[0].audio;
  CHECK(c.start == kPpq);
  CHECK(r.sampleAt(c.start) == 24000);
  CHECK(c.contentStartFrame == 0.0);
  CHECK(std::llround(c.seconds * kRate) == 48000);
  const media::WavData w = readTake(r, c);
  REQUIRE(w.numChannels() == 2);
  REQUIRE(w.frames() == 48000);  // exactly the punch region
  // Frame 0 was captured at song 24 000 + 256: what was played against song 24 000.
  for (int64_t f = 0; f < 48000; ++f)
    if (w.channels[0][f] != source(0, e0 + 24256 + f) || w.channels[1][f] != source(1, e0 + 24256 + f))
      FAIL("file frame " << f << " differs");
  r.link->stop();
  r.run(2);
}

namespace {

enum class World { Edo31, Mos };

TuningDef worldDef(World w) {
  TuningDef d;
  if (w == World::Edo31) {
    d.system = tuning::Edo::make(31).value();
  } else {
    tuning::MosParams m;
    m.n = 7;
    m.period = 1200.0;
    m.generator = 705.0;
    d.system = m;
  }
  return d;
}

// A MIDI track in tuning `w`, note input `mode`, armed (Monitor Auto: it listens to every port).
TrackId midiTrack(Rig& r, World w, NoteInputMode mode) {
  IdSource& ids = r.store.ids();
  const TrackId track = ids.track();
  const TuningId tuning = ids.tuning();
  Batch b{"midi", {}};
  b.commands.push_back(AddTuning{TuningSlot{tuning, "t", Colour{0x2266AA}, worldDef(w)}});
  b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Keys"), {}, kAppend});
  b.commands.push_back(SetTrackTuning{track, TuningAssignment::mainTuning(tuning)});
  b.commands.push_back(SetNoteInput{track, NoteInput{mode, InputKeyMap::Tuning}});
  (void)playback_fixture::instrument(b, ids, track, ids.device());
  r.dispatch(b);
  playback_fixture::wireSeries(r.store);
  r.arm(track);
  return track;
}

engine::RawMidi midi(uint8_t status, uint8_t d1, uint8_t d2) {
  engine::RawMidi m;
  m.port = 0;
  m.status = status;
  m.d1 = d1;
  m.d2 = d2;
  return m;  // no stamp: it plays at the start of the block that drains it
}

}  // namespace

TEST_CASE("T6.7 recording: notes in Keys and Hz on 31-EDO and a MOS store the map's identities (Hz within 0.001 cent)") {
  for (const World w : {World::Edo31, World::Mos})
    for (const NoteInputMode mode : {NoteInputMode::Keys, NoteInputMode::Hz}) {
      CAPTURE(static_cast<int>(w));
      CAPTURE(static_cast<int>(mode));
      Rig r;
      const TrackId t = midiTrack(r, w, mode);
      r.start();
      r.link->setLivePorts({"Port"}, {2.0f});
      r.link->flush();
      r.silentInput = true;
      r.run(10);
      engine::LivePorts& ports = *r.link->livePorts();
      r.link->record(0);
      const uint8_t k1 = 78, k2 = 55;
      for (uint32_t b = 0; b < 800; ++b) {
        if (b == 9) REQUIRE(ports.push(0, midi(0xE0, 0, 0x50)));  // bend +50 cents (of a 2-semitone range)
        if (b == 10) REQUIRE(ports.push(0, midi(0x90, k1, 100)));
        if (b == 400) REQUIRE(ports.push(0, midi(0x80, k1, 40)));
        if (b == 401) REQUIRE(ports.push(0, midi(0xE0, 0, 0x40)));  // bend back to centre
        if (b == 500) REQUIRE(ports.push(0, midi(0x90, k2, 90)));  // held until Stop: ends with the pass
        r.run(1);
      }
      r.link->stop();
      r.run(4);
      REQUIRE(r.link->recordCommits() == 1);
      const Project& p = *r.store.current();
      const Track& tr = r.track(t);
      REQUIRE(tr.takes.size() == 1);
      REQUIRE(tr.clips.size() == 1);
      const NoteClip& c = *tr.clips[0].clip;
      CHECK(c.start == 0);
      const std::vector<Note> notes(c.notes.begin(), c.notes.end());
      REQUIRE(notes.size() == 2);
      auto map = buildInputPitchMap(p, t);
      REQUIRE(map.ok());
      const TempoMap& tm = p.timeline->tempoMap;
      auto tickOf = [&](int64_t block) { return static_cast<Ticks>(std::llround(tm.samplesToTicks(block * kBlock, kRate))); };
      CHECK(notes[0].start == tickOf(10));
      CHECK(notes[0].start + notes[0].length == tickOf(400));
      CHECK(notes[0].velocity == doctest::Approx(100.0f));
      CHECK(notes[1].start == tickOf(500));
      CHECK(notes[1].start + notes[1].length == c.length);  // held at Stop: to the end of the pass
      const InputPitch& p1 = (*map)->at(k1);
      const InputPitch& p2 = (*map)->at(k2);
      REQUIRE(p1.mapped);
      REQUIRE(p2.mapped);
      if (mode == NoteInputMode::Keys) {
        CHECK(notes[0].pitch.kind() == tuning::PitchKind::Degree);
        CHECK(notes[0].pitch == p1.id);  // the bend moved the voice, the identity is the key's degree
        CHECK(notes[1].pitch == p2.id);
      } else {
        REQUIRE(notes[0].pitch.kind() == tuning::PitchKind::Cents);
        const TuningDef& def = worldDef(w);
        const double want = 1200.0 * std::log2(p1.hz * std::exp2(50.0 / 1200.0) / def.referenceHz);
        CHECK(std::fabs(notes[0].pitch.cents() - want) < 0.001);  // the note-on bend frozen in
        CHECK(std::fabs(notes[1].pitch.cents() - 1200.0 * std::log2(p2.hz / def.referenceHz)) < 0.001);
        CHECK(notes[1].pitch == p2.id);
      }
    }
}

TEST_CASE("T6.7 recording: overdub merges notes into the main lane's clip; without it every session adds a take") {
  for (const bool overdub : {true, false}) {
    CAPTURE(overdub);
    Rig r;
    const TrackId t = midiTrack(r, World::Edo31, NoteInputMode::Keys);
    RecordSettings rs;
    rs.overdub = overdub;
    r.dispatch(SetRecordSettings{rs});
    r.start();
    r.link->setLivePorts({"Port"}, {2.0f});
    r.link->flush();
    r.silentInput = true;
    engine::LivePorts& ports = *r.link->livePorts();
    for (int pass = 0; pass < 2; ++pass) {
      r.run(10);
      r.link->record(0);
      for (uint32_t b = 0; b < 600; ++b) {
        if (b == 100u + 200u * static_cast<uint32_t>(pass)) REQUIRE(ports.push(0, midi(0x90, static_cast<uint8_t>(60 + pass), 100)));
        if (b == 150u + 200u * static_cast<uint32_t>(pass)) REQUIRE(ports.push(0, midi(0x80, static_cast<uint8_t>(60 + pass), 64)));
        r.run(1);
      }
      r.link->stop();
      r.run(4);
      REQUIRE(r.link->recordCommits() == static_cast<uint64_t>(pass + 1));
    }
    const Track& tr = r.track(t);
    REQUIRE(tr.clips.size() == 1);
    const std::vector<Note> notes(tr.clips[0].clip->notes.begin(), tr.clips[0].clip->notes.end());
    if (overdub) {
      CHECK(tr.takes.empty());
      CHECK(notes.size() == 2);  // both sessions' notes in the one clip
    } else {
      CHECK(tr.takes.size() == 2);
      CHECK(notes.size() == 1);  // the second session's clip replaced the first in the main lane
      CHECK(tr.clips[0].take == tr.takes[1]->id);
    }
  }
}

// The plan's throughput check (perf: wall-clock, disk-bound; run with -Perf): 16 armed mono inputs at 48 kHz, engine
// blocks of 64, 10 minutes of audio paced at 4× real time (the disk thread must keep up with 4× the real rate), the
// writer on the sample pool's disk thread as in the app → no dropped frame, every file whole.
TEST_CASE("T6.7 recording: 16 mono inputs for 10 minutes at 4x real time drop nothing" * doctest::test_suite("perf")) {
  Rig r;
  std::vector<TrackId> tracks;
  for (uint16_t c = 0; c < 16; ++c) tracks.push_back(r.audioTrack(AudioInput{c, 1}));
  for (TrackId t : tracks) r.arm(t);
  r.start();
  r.link->flush();
  // 16 inputs: the rig's two source channels repeated.
  std::vector<std::vector<float>> in(16, std::vector<float>(kBlock));
  const float* ins[16];
  for (uint32_t c = 0; c < 16; ++c) ins[c] = in[c].data();
  float* outs[2] = {r.l.data(), r.r.data()};
  r.link->record(0);
  const uint64_t blocks = 10ull * 60 * 48000 / kBlock;
  const auto t0 = std::chrono::steady_clock::now();
  for (uint64_t b = 0; b < blocks; ++b) {
    for (uint32_t c = 0; c < 16; ++c)
      for (uint32_t f = 0; f < kBlock; ++f) in[c][f] = source(c & 1u, r.time + f);
    r.link->engine().process(engine::AudioBlock{ins, 16, outs, 2, kBlock, r.time});
    r.time += kBlock;
    if ((b & 63) == 63) {
      (void)r.link->playhead();
      // 4× real time: block b ends at (b + 1) · 64 / 48000 / 4 s.
      const auto due = t0 + std::chrono::duration<double>(static_cast<double>(b + 1) * kBlock / 48000.0 / 4.0);
      std::this_thread::sleep_until(std::chrono::time_point_cast<std::chrono::steady_clock::duration>(due));
    }
  }
  CHECK(r.link->engine().recorder().droppedFrames() == 0);
  r.link->stop();
  r.run(4);
  REQUIRE(r.link->recordCommits() == 1);
  for (TrackId t : tracks) {
    const Track& tr = r.track(t);
    REQUIRE(tr.clips.size() == 1);
    const SampleRef& s = r.sampleOf(*tr.clips[0].audio);
    CHECK(s.frames == static_cast<int64_t>(blocks * kBlock));
  }
  MESSAGE("16 inputs x 10 min at 4x real time: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()
                                                  << " s, dropped " << r.link->engine().recorder().droppedFrames());
}

TEST_CASE("T6.7b recording: removeUnsavedFolder deletes only a folder strictly inside its root") {
  std::random_device rd;
  const fs::path base = fs::temp_directory_path() / ("pq_t67b_guard_" + std::to_string(rd()));
  const fs::path root = base / "pqdaw" / "Unsaved";
  const fs::path session = root / "123-456";
  fs::create_directories(session / "samples" / "recorded");
  { std::ofstream(session / "samples" / "recorded" / "a.wav") << "x"; }
  fs::create_directories(root / "999-1");  // another run's folder
  { std::ofstream(root / "999-1" / "b.wav") << "y"; }
  { std::ofstream(base / "keep.txt") << "z"; }
  std::string why;
  CHECK(app::removeUnsavedFolder(fs::path("relative/x"), root, &why) == 0);  // relative
  CHECK(app::removeUnsavedFolder(root, root, &why) == 0);                    // the root itself
  CHECK(app::removeUnsavedFolder(root / "123-456" / ".." / "..", root, &why) == 0);  // out of it
  CHECK(app::removeUnsavedFolder(base, root, &why) == 0);                    // above it
  CHECK(app::removeUnsavedFolder(fs::path("C:/x/y"), fs::path("C:/x"), &why) == 0);  // a root next to the drive's
  CHECK(fs::exists(session / "samples" / "recorded" / "a.wav"));
  CHECK(app::removeUnsavedFolder(session, root, &why) == 4);  // the folder, samples, recorded, a.wav
  CHECK(why.empty());
  CHECK_FALSE(fs::exists(session));
  CHECK(fs::exists(root / "999-1" / "b.wav"));
  CHECK(fs::exists(base / "keep.txt"));
  CHECK(app::removeUnsavedFolder(session, root, &why) == 0);  // gone: nothing to do
  std::error_code ec;
  fs::remove_all(base, ec);
}

TEST_CASE("M6 review L13: findStaleUnsaved lists only ended sessions' folders with takes, and deletes nothing") {
  std::random_device rd;
  const fs::path root = fs::temp_directory_path() / ("pq_l13_stale_" + std::to_string(rd())) / "Unsaved";
  auto take = [](const fs::path& f) {
    fs::create_directories(f.parent_path());
    std::ofstream(f, std::ios::binary) << "RIFFxxxx";
  };
  take(root / "111-1000" / "1" / "Audio 1.wav");                  // ended (C1 layout)
  take(root / "111-1000" / "2" / "Audio 2.WAV");
  take(root / "222-900" / "samples" / "recorded" / "old.wav");    // ended (the T6.7 layout), older
  fs::create_directories(root / "333-1100" / "1");                // ended, but no takes: not offered
  take(root / "444-1200" / "1" / "live.wav");                     // still running
  take(root / "555-1300" / "1" / "own.wav");                      // this run
  take(root / "notasession" / "x.wav");                           // not a session folder
  take(root / "666-1400x" / "x.wav");
  const auto alive = [](uint32_t pid, int64_t) { return pid == 444; };
  const std::vector<app::StaleUnsavedSession> stale = app::findStaleUnsaved(root, "555-1300", alive);
  REQUIRE(stale.size() == 2);
  CHECK(stale[0].dir.filename() == "222-900");  // oldest first
  CHECK(stale[0].started == 900);
  CHECK(stale[1].dir.filename() == "111-1000");
  CHECK(stale[1].wavs.size() == 2);
  CHECK(stale[1].bytes == 16);
  CHECK(fs::exists(root / "333-1100"));  // nothing was touched
  CHECK(fs::exists(root / "111-1000" / "1" / "Audio 1.wav"));
  // The real check: this process is alive, a session named after it is not offered.
  CHECK(app::processAlive(GetCurrentProcessId(), static_cast<int64_t>(std::time(nullptr))));
  CHECK_FALSE(app::processAlive(GetCurrentProcessId(), 1000));  // a pid reused by another process
  std::error_code ec;
  fs::remove_all(root.parent_path(), ec);
}

namespace {

fs::path u8p(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }

size_t wavsIn(const fs::path& dir) {
  size_t n = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) n += e.path().extension() == ".wav" ? 1 : 0;
  return n;
}

// A ViewContext on the rig's link (the app's wiring: commits go to the context's store; its untitled folders are the
// link's). The rig's own store holds the starting document.
struct Attached {
  Rig& r;
  views::ViewContext ctx;
  explicit Attached(Rig& rig) : r(rig), ctx(*rig.link) {
    ctx.store().reset(r.store.current());
    r.store.unsubscribe(r.sub);
    r.link->attach(ctx);
  }
  ~Attached() {
    r.link->detach();
    r.sub = r.store.subscribe([this](const StoreEvent& e) { r.link->onDocumentChange(e); });
  }
  const SampleRef& sampleOf(TrackId t) const {
    const Track* tr = findTrackPtr(*ctx.doc(), t);
    REQUIRE(tr != nullptr);
    REQUIRE_FALSE(tr->clips.empty());
    REQUIRE(tr->clips[0].audio != nullptr);
    const SampleRef* s = findSample(*ctx.doc(), tr->clips[0].audio->sample);
    REQUIRE(s != nullptr);
    return *s;
  }
};

}  // namespace

TEST_CASE("T6.7b recording: discarding an untitled project deletes its own folder only; each untitled document gets a fresh one") {
  Rig r;
  const TrackId t = r.audioTrack(AudioInput{0, 1});
  r.arm(t);
  r.start();
  r.link->flush();
  fs::create_directories(r.dir);
  { std::ofstream(r.dir / "neighbour.txt") << "keep"; }
  // A finished session (into the first untitled document's folder), then New on it through a ViewContext: it goes.
  r.run(10);
  r.link->record(0);
  r.run(200);
  r.link->stop();
  r.run(4);
  REQUIRE(r.link->recordCommits() == 1);
  const fs::path first = u8p(r.link->unsavedRecordDir());
  REQUIRE(wavsIn(first) == 1);
  {
    Attached a(r);
    REQUIRE(a.ctx.projectDir().empty());
    CHECK(a.ctx.unsavedRecordingDir() == r.link->unsavedRecordDir());
    a.ctx.newProject();
    CHECK_FALSE(fs::exists(first));
    CHECK(fs::exists(r.dir / "neighbour.txt"));
    // The new untitled document has a folder of its own (M6 review C1).
    const fs::path second = u8p(a.ctx.unsavedRecordingDir());
    CHECK(second != first);
    CHECK(a.ctx.unsavedRecordingDir() == r.link->unsavedRecordDir());
    fs::create_directories(second);
    { std::ofstream(second / "x.wav") << "x"; }
    // Its first save moves every file of its folder into the project (referenced or not); the empty folder goes.
    REQUIRE(a.ctx.saveProjectAs((r.dir / "Song").string()).ok());
    CHECK(fs::exists(r.dir / "Song" / "samples" / "recorded" / "x.wav"));
    CHECK_FALSE(fs::exists(second));
    // A titled project's New discards nothing; the next untitled document gets a third folder.
    a.ctx.newProject();
    const fs::path third = u8p(a.ctx.unsavedRecordingDir());
    CHECK(third != second);
    fs::create_directories(third);
    { std::ofstream(third / "y.wav") << "y"; }
    // Untitled again: quitting (nothing to save) discards its folder, and only that.
    CHECK(a.ctx.confirmQuit([] {}));
    CHECK_FALSE(fs::exists(third));
    CHECK(fs::exists(r.dir / "Song" / "samples" / "recorded" / "x.wav"));
  }
}

TEST_CASE("T6.7b recording: a session still running when its untitled project is discarded ends without a commit") {
  Rig r;
  const TrackId t = r.audioTrack(AudioInput{0, 1});
  r.arm(t);
  r.start();
  r.link->flush();
  r.run(10);
  r.link->record(0);
  r.run(300);
  REQUIRE(r.link->recording().active());
  const fs::path folder = u8p(r.link->unsavedRecordDir());
  CHECK(r.link->discardUnsavedRecordings(r.link->unsavedRecordDir()) >= 2);  // the folder and the session's WAV
  CHECK_FALSE(r.link->recording().active());
  CHECK_FALSE(fs::exists(folder));
  r.run(10);
  CHECK(r.link->recordCommits() == 0);
  CHECK(r.track(t).takes.empty());
}

TEST_CASE("T6.7b recording: a first save that could neither move nor copy a recording pins the folder; no discard deletes it") {
  Rig r;
  const TrackId t = r.audioTrack(AudioInput{0, 1});
  r.arm(t);
  r.start();
  Attached a(r);
  r.link->flush();
  r.run(10);
  r.link->record(0);
  r.run(200);
  r.link->stop();
  r.run(4);
  REQUIRE(r.link->recordCommits() == 1);
  const std::string refPath = a.sampleOf(t).path;
  const fs::path wav = u8p(refPath);
  const std::string folder = r.link->unsavedRecordDir();
  REQUIRE(fs::exists(wav));
  // A file where the save's samples/recorded folder would go: neither the move nor the copy can succeed.
  const fs::path song = r.dir / "Song";
  fs::create_directories(song / "samples");
  { std::ofstream(song / "samples" / "recorded") << "in the way"; }
  REQUIRE(a.ctx.saveProjectAs(song.string()).ok());
  CHECK(a.sampleOf(t).path == refPath);  // still the unsaved file
  CHECK(a.ctx.unsavedFolderInUse(folder, false));  // the saved project pins it
  CHECK(r.link->discardUnsavedRecordings(folder) == 0);  // the link's last line of defence too
  a.ctx.newProject();
  a.ctx.newProject();
  CHECK(a.ctx.confirmQuit([] {}));
  CHECK(fs::exists(wav));
}

// M6 review C1 path A (probe_unsaved_delete A): the WAV's path was fixed in the untitled folder at Record; the project
// is saved while it records, so the take is committed into a titled project.
TEST_CASE("M6 review C1: a take still recording when the untitled project is saved goes into the project; New + quit keep it") {
  Rig r;
  const TrackId t = r.audioTrack(AudioInput{0, 1});
  r.arm(t);
  r.start();
  Attached a(r);
  r.link->flush();
  r.run(10);
  r.link->record(0);
  r.run(300);
  const fs::path song = r.dir / "SongA";
  REQUIRE(a.ctx.saveProjectAs(song.string()).ok());  // its file is being written: it stays until the commit
  r.run(300);
  r.link->stop();
  r.run(4);
  REQUIRE(r.link->recordCommits() == 1);
  const SampleRef ref = a.sampleOf(t);  // a copy: the snapshot goes with New
  CHECK_FALSE(isAbsoluteSamplePath(ref.path));  // in the project, project-relative
  const fs::path wav = song / u8p(ref.path);
  REQUIRE(fs::exists(wav));
  auto w = media::readWav(wav);
  REQUIRE(w.ok());
  CHECK(static_cast<int64_t>(w.value().frames()) == ref.frames);
  REQUIRE(a.ctx.saveProject().ok());
  a.ctx.newProject();
  CHECK(a.ctx.confirmQuit([] {}));
  CHECK(fs::exists(wav));
  // The saved project plays it from the project folder.
  REQUIRE(a.ctx.openProject(song.string()).ok());
  CHECK(a.sampleOf(t).path == ref.path);
}

// M6 review C1 path B (probe_unsaved_delete B): the take's snapshot was not current at the first save.
TEST_CASE("M6 review C1: record, undo, Save As, redo, Save: the redone take is in the project; New + quit keep it") {
  Rig r;
  const TrackId t = r.audioTrack(AudioInput{0, 1});
  r.arm(t);
  r.start();
  Attached a(r);
  r.link->flush();
  r.run(10);
  r.link->record(0);
  r.run(300);
  r.link->stop();
  r.run(4);
  REQUIRE(r.link->recordCommits() == 1);
  const fs::path unsaved = u8p(a.sampleOf(t).path);
  REQUIRE(a.ctx.undo());
  const fs::path song = r.dir / "SongB";
  REQUIRE(a.ctx.saveProjectAs(song.string()).ok());
  REQUIRE(a.ctx.redo());
  const SampleRef ref = a.sampleOf(t);
  CHECK_FALSE(isAbsoluteSamplePath(ref.path));  // the redo snapshot was remapped too
  const fs::path wav = song / u8p(ref.path);
  CHECK(fs::exists(wav));
  CHECK_FALSE(fs::exists(unsaved));
  REQUIRE(a.ctx.saveProject().ok());
  a.ctx.newProject();
  CHECK(a.ctx.confirmQuit([] {}));
  CHECK(fs::exists(wav));
}

// M6 review H1 (probe_unsaved_delete C): a project inside the unsaved folders would be deleted with them.
TEST_CASE("M6 review H1: Save As inside the unsaved root is refused; a project opened from inside an untitled folder keeps it") {
  Rig r;
  r.start();
  Attached a(r);
  const fs::path root = u8p(r.link->unsavedRoot());
  CHECK_FALSE(a.ctx.saveProjectAs((root / "SongC").string()).ok());
  CHECK_FALSE(a.ctx.saveProjectAs(root.string()).ok());
  CHECK_FALSE(a.ctx.saveProjectAs((u8p(a.ctx.unsavedRecordingDir()) / "Deep" / "SongC").string()).ok());
  std::string upper = (root / "SongD").string();
  for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  CHECK_FALSE(a.ctx.saveProjectAs(upper).ok());  // Windows paths ignore case
  CHECK_FALSE(fs::exists(root / "SongC"));
  CHECK(a.ctx.projectDir().empty());
  // A project copied by hand into the untitled document's folder and opened from there: the Open discards the
  // untitled document, but its folder now holds the open project: kept.
  REQUIRE(a.ctx.saveProjectAs((r.dir / "Elsewhere").string()).ok());
  a.ctx.newProject();
  const fs::path folder = u8p(a.ctx.unsavedRecordingDir());
  fs::create_directories(folder);
  fs::copy(r.dir / "Elsewhere", folder / "Inside", fs::copy_options::recursive);
  REQUIRE(a.ctx.openProject((folder / "Inside").string()).ok());
  CHECK(fs::exists(folder / "Inside"));
  CHECK(a.ctx.unsavedFolderInUse(folder.string(), true));
}

// M6 review L11 (probe_record_link 2): same name, same input, same second.
TEST_CASE("M6 review L11: two armed tracks with the same name on one input record two files") {
  Rig r;
  const TrackId a = r.audioTrack(AudioInput{0, 1});
  const TrackId b = r.audioTrack(AudioInput{0, 1});
  r.dispatch(RenameTrack{a, "Vox"});
  r.dispatch(RenameTrack{b, "Vox"});
  r.arm(a);
  r.arm(b);
  r.start();
  r.link->flush();
  r.run(10);
  r.link->record(0);
  r.run(1500);
  r.link->stop();
  r.run(4);
  REQUIRE(r.link->recordCommits() == 1);
  const AudioClip& ca = *r.track(a).clips.at(0).audio;
  const AudioClip& cb = *r.track(b).clips.at(0).audio;
  const SampleRef& sa = r.sampleOf(ca);
  const SampleRef& sb = r.sampleOf(cb);
  CHECK(sa.path != sb.path);
  const media::WavData wa = readTake(r, ca);
  const media::WavData wb = readTake(r, cb);
  CHECK(wa.frames() == 96000);
  CHECK(wb.frames() == 96000);
  CHECK(wa.channels[0] == wb.channels[0]);
}

// M6 review M4 / M5 at the record commit: a note keeps the identity it was played with, whatever the track's note
// input mode or its tuning is when the session ends.
TEST_CASE("M6 review M4 / M5: recorded notes keep their played identity across a mode and a tuning change before Stop") {
  for (const NoteInputMode mode : {NoteInputMode::Hz, NoteInputMode::Keys}) {
    CAPTURE(static_cast<int>(mode));
    Rig r;
    const TrackId t = midiTrack(r, World::Edo31, mode);
    r.start();
    r.link->setLivePorts({"Port"}, {2.0f});
    r.link->flush();
    r.silentInput = true;
    r.run(10);
    auto before = buildInputPitchMap(*r.store.current(), t);
    REQUIRE(before.ok());
    const uint8_t k1 = 78;
    const InputPitch played = (*before)->at(k1);
    REQUIRE(played.mapped);
    engine::LivePorts& ports = *r.link->livePorts();
    r.link->record(0);
    REQUIRE(ports.push(0, midi(0xE0, 0, 0x50)));  // +50 cents
    r.run(2);
    REQUIRE(ports.push(0, midi(0x90, k1, 100)));
    r.run(100);
    REQUIRE(ports.push(0, midi(0x80, k1, 40)));
    REQUIRE(ports.push(0, midi(0xE0, 0, 0x40)));
    r.run(20);
    // Before Stop: the other note world, and another tuning (12-EDO).
    r.dispatch(SetNoteInput{t, NoteInput{mode == NoteInputMode::Hz ? NoteInputMode::Keys : NoteInputMode::Hz, InputKeyMap::Tuning}});
    TuningId tid;
    for (const auto& s : *r.store.current()->tunings)
      if (s->name == "t") tid = s->id;
    r.dispatch(SetTuningDef{tid, TuningDef{}});
    r.run(20);
    r.link->stop();
    r.run(4);
    REQUIRE(r.link->recordCommits() == 1);
    const Track& tr = r.track(t);
    REQUIRE(tr.clips.size() == 1);
    const std::vector<Note> notes(tr.clips[0].clip->notes.begin(), tr.clips[0].clip->notes.end());
    REQUIRE(notes.size() == 1);
    if (mode == NoteInputMode::Keys) {
      CHECK(notes[0].pitch == played.id);  // the key's degree, the bend not frozen in
    } else {
      REQUIRE(notes[0].pitch.kind() == tuning::PitchKind::Cents);
      CHECK(notes[0].pitch.cents() == played.id.cents() + 50.0);  // exactly what the note-on played
      const double want = 1200.0 * std::log2(played.hz * std::exp2(50.0 / 1200.0) / worldDef(World::Edo31).referenceHz);
      CHECK(std::fabs(notes[0].pitch.cents() - want) < 0.001);
    }
  }
}
