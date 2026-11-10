// M6 review (recording) through the playback link (the T6.7 test rig, copied from tests/app/test_recording_link.cpp):
// (1) loop recording with the punch on, punch region = loop region (1 bar = 96 000 samples, record latency 256, 4
// passes): does each take keep what was PLAYED in the whole loop? Control: the same with the punch off.
// (2) two armed audio tracks with the same name on the same input: distinct files?
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <algorithm>
#include <chrono>
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

static void loopPunch(bool punch) {
  Rig r;
  const TrackId t = r.audioTrack(AudioInput{0, 1});
  r.arm(t);
  if (punch) r.dispatch(SetPunch{PunchRegion{0, 4 * kPpq, true}});
  r.start();
  r.link->flush();
  r.link->engine().recorder().setDeviceLatency(128, 128);
  r.link->setLoop(views::TransportLoop{0, 4 * kPpq, true});
  r.run(10);
  r.link->record(0);
  const int64_t e0 = r.time;
  r.run(6000);  // 4 x 96 000 frames
  r.link->stop();
  r.run(4);
  REQUIRE(r.link->recordCommits() == 1);
  const Track& tr = r.track(t);
  MESSAGE("punch " << (punch ? "ON (= loop)" : "off") << ": " << tr.takes.size() << " takes");
  for (size_t k = 0; k < tr.takes.size(); ++k) {
    const AudioClip& c = *tr.takes[k]->clips[0].audio;
    const int64_t len = std::llround(c.seconds * kRate);
    const media::WavData w = readTake(r, c);
    // Which engine frame does take sample 0 hold? (the source is a counter of engine time)
    const float v = w.channels[0][static_cast<size_t>(c.contentStartFrame)];
    int64_t eng = -1;
    for (int64_t n = e0; n < e0 + 400000; ++n)
      if (source(0, n) == v) { eng = n; break; }
    MESSAGE("  take " << k + 1 << ": start sample " << r.sampleAt(c.start) << ", length " << len << " (loop 96000, missing "
            << 96000 - len << "), content from engine frame e0+" << eng - e0 << " = played at loop-relative "
            << (eng - e0 - 256) - static_cast<int64_t>(k) * 96000 << ", file frames " << w.frames());
  }
}

TEST_CASE("probe: loop record, punch off") { loopPunch(false); }
TEST_CASE("probe: loop record, punch = loop") { loopPunch(true); }

// Two armed audio tracks with the same name on the same input (a renamed / duplicated track, or "dry + amp" of one
// mic) record in the same second: record() picks each WAV path by exists() BEFORE any file is created.
TEST_CASE("probe: two same-named tracks on one input") {
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
  const int64_t e0 = r.time;
  r.run(1500);  // 96 000 frames
  r.link->stop();
  r.run(4);
  REQUIRE(r.link->recordCommits() == 1);
  const AudioClip& ca = *r.track(a).clips.at(0).audio;
  const AudioClip& cb = *r.track(b).clips.at(0).audio;
  const std::string pa = r.sampleOf(ca).path, pb = r.sampleOf(cb).path;
  MESSAGE("track A file: " << pa);
  MESSAGE("track B file: " << pb);
  MESSAGE((pa == pb ? "SAME FILE for both tracks" : "distinct files"));
  const media::WavData w = readTake(r, ca);
  int64_t wrong = 0;
  for (int64_t f = 0; f < static_cast<int64_t>(w.frames()) && f < 96000; ++f) wrong += w.channels[0][f] != source(0, e0 + f) ? 1 : 0;
  MESSAGE("file frames " << w.frames() << " (expected 96000), frames differing from the input: " << wrong);
  CHECK(pa != pb);
  CHECK(wrong == 0);
}
