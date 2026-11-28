// RT.2 (docs/plans/RT.md "Lowering and engine"): recording from a track through the playback link. Track A is an audio
// track hearing device input 1 (Monitor In) through `latency(480)` then `tanh` — a latency-bearing block upstream of
// its last device. Track B records a cable onto its Audio In, captured at B's graph latency compensation only, so its
// take is on the song timeline:
//   - from A's last device: frame f of the take == what the master plays of A at f + 480 (A alone at the master: B
//     monitors Off), sample-aligned, ≤ 1e-6;
//   - from A's mixer out (Post Mixer): follows A's volume (−6 dB: the take is A's post, the master's A, and 10^(−6/20) of
//     the device's take);
//   - from A's source (Pre FX): the input itself, bit for bit at offset 0 (A's devices do not reach it);
//   - from a rack chain (A: latency → a rack whose chain holds tanh): the chain's output, aligned;
//   - Resampling (B monitoring In): the master without B — the take is half of what the master plays (A + B's monitor);
//   - Monitor Off / In / Auto with a cable: B adds nothing / A again / A again while armed (and nothing unarmed).
// Plus: two audio tracks merged into one unit (a sidechain cable) each hear their own Ext. In (the one-input-per-unit
// limit lifted). Every engine block runs under RtScope (the capture path included).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/app/recording.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/audio_in.h"
#include "pq/document/racks.h"
#include "pq/media/wav_file.h"
#include "../support/blocks.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBlock = 64;
constexpr int64_t kLat = 480;

// Input channel c at engine frame n: distinct values, exact in float, small enough for tanh to stay far from flat.
float source(uint32_t c, int64_t n) {
  return (c == 0 ? 0.75f : -0.5f) * static_cast<float>((n % (1 << 16)) + 1) * (1.0f / static_cast<float>(1 << 16));
}

struct Rig {
  DocumentStore store{playback_fixture::unwiredDefaultProject()};
  std::unique_ptr<ProjectLink> link;
  ListenerId sub = 0;
  fs::path dir;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  std::vector<float> outL, outR;  // the master, every block run (index = engine frame)
  TrackId a, b;
  DeviceId lat, fx;

  Rig() {
    store.setDeviceLibrary(&playback_fixture::unitsLibrary());
    std::random_device rd;
    dir = fs::temp_directory_path() / ("pq_rt2_" + std::to_string(rd()));
  }
  ~Rig() {
    if (link) store.unsubscribe(sub);
    link.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void apply(const Result<ops::Edit>& e) { test_blocks::dispatchEdit(store, e); }
  void dispatch(const Command& c) { REQUIRE(store.dispatch(c).ok()); }
  TrackId audioTrack(AudioInput in, const char* name) {
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio, {}, name);
    REQUIRE(t.ok());
    dispatch(t->command);
    const TrackId id = t->tracks.at(0);
    if (!in.none()) dispatch(SetTrackAudioInput{id, in});
    playback_fixture::wireSeries(store);
    return id;
  }
  // A (input 1, Monitor In, latency(480) → tanh unless `rack`: latency → a rack whose one chain holds tanh) and B.
  void build(bool rack = false) {
    a = audioTrack(AudioInput{0, 1}, "A");
    dispatch(SetTrackMonitor{a, MonitorMode::In});
    lat = test_blocks::appendAfter(store, a, "atom:latency");
    apply(ops::setDeviceSetting(*store.current(), lat, "frames", static_cast<double>(kLat)));
    if (rack) {
      auto e = ops::addRack(*store.current(), store.ids(), a, false);
      apply(e);
      const DeviceId chain = e->devices.at(0);
      REQUIRE(findRacks(*store.current(), *findTrackPtr(*store.current(), a)).at(0).chains.at(0).id == chain);
      apply(ops::addToChain(*store.current(), store.ids(), chain, "atom:tanh", DeviceKind::AudioEffect));
      fx = chain;
    } else {
      fx = test_blocks::appendAfter(store, a, "atom:tanh");
    }
    b = audioTrack(AudioInput{}, "B");
    dispatch(SetTrackMonitor{b, MonitorMode::Off});
  }
  void from(const AudioFrom& f, TrackId t = {}) {
    apply(ops::setAudioFrom(*store.current(), store.ids(), t.valid() ? t : b, f, &playback_fixture::unitsLibrary()));
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
    link->flush();
  }
  void block() {
    std::vector<float> in0(kBlock), in1(kBlock);
    for (uint32_t f = 0; f < kBlock; ++f) {
      in0[f] = source(0, time + f);
      in1[f] = source(1, time + f);
    }
    const float* ins[2] = {in0.data(), in1.data()};
    float* outs[2] = {l.data(), r.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link->engine().process(engine::AudioBlock{ins, 2, outs, 2, kBlock, time});
    }
    outL.insert(outL.end(), l.begin(), l.end());
    outR.insert(outR.end(), r.begin(), r.end());
    time += kBlock;
  }
  void run(uint32_t blocks) {
    for (uint32_t k = 0; k < blocks; ++k) {
      block();
      if ((k & 15) == 15) (void)link->playhead();
      if ((k & 63) == 63) (void)link->recording().pump();
      if ((k & 255) == 255) link->collect();
    }
    (void)link->playhead();
  }
  // Records `blocks` blocks from song 0 on armed B; returns the take's channels and the engine frame recording began.
  std::vector<std::vector<float>> record(uint32_t blocks, int64_t& e0) {
    apply(ops::armTrack(*store.current(), b, true));
    link->flush();
    run(100);  // settled (the gains' smoothing, the monitor gate)
    link->record(0);
    e0 = time;
    run(blocks);
    link->stop();
    run(4);
    REQUIRE(link->recordCommits() == 1);
    const Track* t = findTrackPtr(*store.current(), b);
    REQUIRE(t != nullptr);
    REQUIRE(t->clips.size() == 1);
    REQUIRE(t->clips[0].audio != nullptr);
    const AudioClip& c = *t->clips[0].audio;
    CHECK(c.start == 0);
    CHECK(c.contentStartFrame == 0.0);
    const SampleRef* s = findSample(*store.current(), c.sample);
    REQUIRE(s != nullptr);
    auto w = media::readWav(fs::path(std::u8string(reinterpret_cast<const char8_t*>(s->path.data()), s->path.size())));
    REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
    REQUIRE(w.value().numChannels() == 2);
    return std::move(w.value().channels);
  }
};

// max |take[f] − scale · master[e0 + f + lag]| over the take's frames, per channel.
double worstVsMaster(const Rig& r, const std::vector<std::vector<float>>& take, int64_t e0, int64_t lag, double scale = 1.0) {
  double worst = 0.0;
  REQUIRE(!take[0].empty());
  for (size_t f = 0; f < take[0].size(); ++f) {
    const size_t k = static_cast<size_t>(e0 + lag) + f;
    if (k >= r.outL.size()) break;
    worst = std::max(worst, std::fabs(take[0][f] - scale * r.outL[k]));
    worst = std::max(worst, std::fabs(take[1][f] - scale * r.outR[k]));
  }
  return worst;
}

double rms(const std::vector<float>& x) {
  double s = 0.0;
  for (float v : x) s += static_cast<double>(v) * v;
  return std::sqrt(s / static_cast<double>(std::max<size_t>(1, x.size())));
}

}  // namespace

TEST_CASE("RT.2 recording from a track: A's last device (latency upstream), its mixer out (follows its volume), its "
          "source (ignores its devices), a rack chain — each take aligned to the song") {
  double deviceRms = 0.0;
  {  // A's last device: the master is A alone, 480 late; the take is it on the timeline.
    Rig r;
    r.build();
    r.from(AudioFrom::output(Wire{.from = r.fx}));
    r.start();
    int64_t e0 = 0;
    const auto take = r.record(1500, e0);
    CHECK(r.link->outputLatency() == static_cast<uint32_t>(kLat));
    CHECK(static_cast<int64_t>(take[0].size()) == 1500 * kBlock - kLat);  // the last 480 played were not captured yet
    CHECK(rms(take[0]) > 0.05);
    CHECK(worstVsMaster(r, take, e0, kLat) <= 1e-6);
    // Sample-aligned: one frame off is far from it.
    CHECK(worstVsMaster(r, take, e0, kLat + 1) > 1e-5);
    deviceRms = rms(take[0]);
  }
  {  // A's mixer out (Post Mixer) at −6 dB: its post, what the master plays of A.
    Rig r;
    r.build();
    r.dispatch(SetTrackVolume{r.a, -6.0f});
    r.from(AudioFrom::output(Wire{.mixerOut = r.a}));
    r.start();
    int64_t e0 = 0;
    const auto take = r.record(1500, e0);
    CHECK(worstVsMaster(r, take, e0, kLat) <= 1e-6);
    CHECK(rms(take[0]) / deviceRms == doctest::Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(1e-3));
  }
  {  // A's source (Pre FX): the input as A hears it, before its latency and tanh: bit for bit, no offset.
    Rig r;
    r.build();
    r.from(AudioFrom::output(Wire{.noteSource = r.a}));
    r.start();
    int64_t e0 = 0;
    const auto take = r.record(800, e0);
    REQUIRE(static_cast<int64_t>(take[0].size()) == 800 * kBlock);
    for (size_t f = 0; f < take[0].size(); ++f) {
      if (take[0][f] != source(0, e0 + static_cast<int64_t>(f)) || take[1][f] != take[0][f]) FAIL("frame " << f << " differs");
    }
  }
  {  // A rack chain (latency → rack{tanh}): the chain's output, aligned (the rack is its only chain: the master's A).
    Rig r;
    r.build(true);
    r.from(AudioFrom::output(Wire{.from = r.fx}));
    r.start();
    int64_t e0 = 0;
    const auto take = r.record(1500, e0);
    CHECK(rms(take[0]) > 0.05);
    CHECK(worstVsMaster(r, take, e0, kLat) <= 1e-6);
  }
}

TEST_CASE("RT.2 Resampling records the master without the recording track (B monitoring In doubles A at the master)") {
  Rig r;
  r.build();
  r.from(AudioFrom::resampling());
  r.dispatch(SetTrackMonitor{r.b, MonitorMode::In});
  r.start();
  int64_t e0 = 0;
  const auto take = r.record(1500, e0);
  CHECK(r.link->outputLatency() == static_cast<uint32_t>(kLat));
  CHECK(rms(take[0]) > 0.05);
  // master = A + B's monitor of the take's signal = 2 · the take, aligned.
  CHECK(worstVsMaster(r, take, e0, kLat, 0.5) <= 1e-6);
}

TEST_CASE("RT.2 monitor Off / In / Auto with a cable source: B adds nothing, A again, A again while armed only") {
  // The reference: B Off for the whole run (the master is A alone).
  Rig ref;
  ref.build();
  ref.from(AudioFrom::output(Wire{.from = ref.fx}));
  ref.start();
  ref.run(4 * 400);
  Rig r;
  r.build();
  r.from(AudioFrom::output(Wire{.from = r.fx}));
  r.start();
  const MonitorMode modes[4] = {MonitorMode::Off, MonitorMode::In, MonitorMode::Auto, MonitorMode::Auto};
  const double want[4] = {1.0, 2.0, 1.0, 2.0};  // Auto: unarmed, then armed (stopped)
  for (int k = 0; k < 4; ++k) {
    r.dispatch(SetTrackMonitor{r.b, modes[k]});
    if (k == 3) r.apply(ops::armTrack(*r.store.current(), r.b, true));
    r.link->flush();
    const size_t from = r.outL.size();
    r.run(400);
    double worst = 0.0;
    for (size_t i = from + 4000; i < r.outL.size(); ++i)  // past the gate's smoothing
      worst = std::max(worst, std::fabs(r.outL[i] - want[k] * ref.outL[i]));
    CAPTURE(k);
    CHECK(rms(std::vector<float>(ref.outL.begin() + static_cast<std::ptrdiff_t>(from), ref.outL.begin() + static_cast<std::ptrdiff_t>(from + 25600))) > 0.05);
    CHECK(worst <= 1e-6);
  }
}

TEST_CASE("RT.2 two audio tracks merged into one unit each hear their own Ext. In (no one-input-per-unit limit)") {
  Rig r;
  const TrackId x = r.audioTrack(AudioInput{0, 1}, "X");  // input 1
  const TrackId y = r.audioTrack(AudioInput{1, 1}, "Y");  // input 2
  r.dispatch(SetTrackMonitor{x, MonitorMode::In});
  r.dispatch(SetTrackMonitor{y, MonitorMode::In});
  const DeviceId add = test_blocks::appendAfter(r.store, y, "atom:add");
  // X's own signal into Y's add.b: a sidechain (merges X and Y into one unit). Y = in2 + in1, X = in1.
  Wire w;
  w.noteSource = x;
  w.to = add;
  w.param = "b";
  r.apply(ops::connect(*r.store.current(), r.store.ids(), w));
  r.start();
  REQUIRE(r.link->engine().program() != nullptr);
  CHECK(r.link->engine().program()->numUnits() == 2);  // {X, Y} and the master
  r.run(300);
  double worst = 0.0;
  for (size_t i = 4000; i < r.outL.size(); ++i) {
    const double want = 2.0 * source(0, static_cast<int64_t>(i)) + source(1, static_cast<int64_t>(i));
    worst = std::max(worst, std::fabs(r.outL[i] - want));
  }
  CHECK(worst <= 1e-6);
}

