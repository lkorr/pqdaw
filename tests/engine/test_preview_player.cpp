// T5.10: the browser's preview player (pq/engine/preview_player.h): the first nonzero sample in the block after play
// (head resident), exact frames at the device rate after the 2 ms declick, stop = a 2 ms fade then sleep (no work, the
// images freed), the end of the file, scrub (fade out, fade in at the new place), a 44.1 kHz file at 48 kHz, Sync
// (Re-Pitch to the project tempo), gain, a streamed file past its head; every process() under RtScope.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>

#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/preview_player.h"
#include "pq/media/sample_pool.h"
#include "pq/media/wav_file.h"

using namespace pq;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 48000.0;

fs::path tempDir() {
  const fs::path d = fs::temp_directory_path() / "pq_preview_player";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

// A stereo WAV whose frame i is (src(i), −src(i)), src(i) = 0.5·sin(0.01·i) + 0.1 (never 0 at frame 0).
float src(int64_t i) { return static_cast<float>(0.5 * std::sin(0.01 * static_cast<double>(i)) + 0.1); }
fs::path writeWav(const char* name, int64_t frames, uint32_t rate) {
  std::vector<std::vector<float>> ch(2, std::vector<float>(static_cast<size_t>(frames)));
  for (int64_t i = 0; i < frames; ++i) {
    ch[0][static_cast<size_t>(i)] = src(i);
    ch[1][static_cast<size_t>(i)] = -src(i);
  }
  const fs::path p = tempDir() / name;
  REQUIRE(media::writeWav(p, ch, rate).ok());
  return p;
}

media::PoolConfig poolConfig() {
  media::PoolConfig pc;
  pc.streamSlots = 8;
  pc.ringFloats = 2 * 65536;
  pc.decodedCacheDir = tempDir() / "decoded";  // never the user's %LOCALAPPDATA%
  return pc;
}

struct Out {
  std::vector<float> l, r;
};

// `blocks` device blocks of `n` frames, each under RtScope (the audio thread's rules).
Out run(engine::PreviewPlayer& p, int blocks, uint32_t n = 64) {
  Out o;
  std::vector<float> l(n), r(n);
  for (int b = 0; b < blocks; ++b) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    float* ch[2] = {l.data(), r.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      p.process(ch, 2, n);
    }
    o.l.insert(o.l.end(), l.begin(), l.end());
    o.r.insert(o.r.end(), r.begin(), r.end());
  }
  return o;
}

float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0;
  for (size_t i = from; i < std::min(to, v.size()); ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}

}  // namespace

TEST_CASE("preview player: the block after play carries the sample; exact frames after the declick; RtScope") {
  media::SamplePool pool(poolConfig());
  auto s = pool.load(writeWav("p48.wav", 48000, 48000));
  REQUIRE(s.ok());
  engine::PreviewConfig pc;
  pc.sampleRate = kSr;
  pc.streams = &pool.streams();
  engine::PreviewPlayer p(pc);
  // Idle: sleeps (no work).
  Out idle = run(p, 4);
  CHECK(peak(idle.l) == 0.0f);
  CHECK(p.stats().sleptBlocks == 4);
  CHECK(p.state() == engine::PreviewPlayer::State::Stopped);
  REQUIRE(p.play(*s, 0.0, {}).ok());
  CHECK(p.state() == engine::PreviewPlayer::State::Playing);
  Out o = run(p, 40);
  // The very next callback: nonzero from its second frame (frame 0 is the declick's 0).
  CHECK(std::fabs(o.l[1]) > 0.0f);
  // After the 2 ms (96-frame) declick the frames are the file's, exactly (rate 1: a straight copy).
  const auto declick = static_cast<size_t>(std::lround(0.002 * kSr));
  bool exact = true;
  for (size_t i = declick; i < o.l.size(); ++i) exact = exact && o.l[i] == src(static_cast<int64_t>(i)) && o.r[i] == -src(static_cast<int64_t>(i));
  CHECK(exact);
  CHECK(p.positionSeconds() == doctest::Approx(40.0 * 64.0 / kSr));
  CHECK(p.lengthSeconds() == doctest::Approx(1.0));
}

TEST_CASE("preview player: stop fades over 2 ms, then the player sleeps and its images are freed") {
  media::SamplePool pool(poolConfig());
  auto s = pool.load(writeWav("p48b.wav", 48000, 48000));
  REQUIRE(s.ok());
  engine::PreviewConfig pc;
  pc.sampleRate = kSr;
  pc.streams = &pool.streams();
  engine::PreviewPlayer p(pc);
  REQUIRE(p.play(*s, 0.0, {}).ok());
  (void)run(p, 10);
  p.stop();
  Out o = run(p, 6);
  CHECK(peak(o.l, 0, 32) > 0.0f);   // fading
  CHECK(peak(o.l, 96) == 0.0f);     // faded within 2 ms
  CHECK(p.state() == engine::PreviewPlayer::State::Stopped);
  CHECK(p.positionSeconds() < 0.0);
  const uint64_t slept = p.stats().sleptBlocks;
  (void)run(p, 8);
  CHECK(p.stats().sleptBlocks == slept + 8);  // asleep: one ring check per block
  // The last image stays held for a scrub; another play retires it once the audio thread has moved on.
  CHECK(p.collect() == 1);
  REQUIRE(p.play(*s, 0.5, {}).ok());
  (void)run(p, 2);
  CHECK(p.collect() == 1);
}

TEST_CASE("preview player: the end of the file stops it; scrub jumps with a fade") {
  media::SamplePool pool(poolConfig());
  auto s = pool.load(writeWav("short.wav", 4800, 48000));  // 0.1 s
  REQUIRE(s.ok());
  engine::PreviewConfig pc;
  pc.sampleRate = kSr;
  pc.streams = &pool.streams();
  engine::PreviewPlayer p(pc);
  REQUIRE(p.play(*s, 0.0, {}).ok());
  Out o = run(p, 100);  // 6400 frames
  CHECK(peak(o.l, 4900) == 0.0f);
  CHECK(p.state() == engine::PreviewPlayer::State::Stopped);
  // Scrub while playing.
  auto longer = pool.load(writeWav("long.wav", 96000, 48000));
  REQUIRE(longer.ok());
  REQUIRE(p.play(*longer, 0.0, {}).ok());
  (void)run(p, 10);
  REQUIRE(p.scrub(1.5));
  Out j = run(p, 10);
  CHECK(peak(j.l, 200) > 0.0f);
  const double at = p.positionSeconds();
  CHECK(at == doctest::Approx(1.5 + 640.0 / kSr).epsilon(0.001));
  // After the crossing fades, the frames are the file's from 1.5 s on.
  const auto base = static_cast<int64_t>(1.5 * kSr);
  bool exact = true;
  for (size_t i = 200; i < j.l.size(); ++i) exact = exact && j.l[i] == src(base + static_cast<int64_t>(i));
  CHECK(exact);
}

TEST_CASE("preview player: a 44.1 kHz file at 48 kHz (resampled), Sync re-pitches to the project tempo, gain") {
  media::SamplePool pool(poolConfig());
  auto s = pool.load(writeWav("p441.wav", 44100, 44100));
  REQUIRE(s.ok());
  engine::PreviewConfig pc;
  pc.sampleRate = kSr;
  pc.streams = &pool.streams();
  engine::PreviewPlayer p(pc);
  REQUIRE(p.play(*s, 0.0, {}).ok());
  Out o = run(p, 800);  // 51200 frames > 48000 (the file's 1 s at the device rate)
  CHECK(peak(o.l, 1000, 47000) > 0.3f);
  CHECK(peak(o.l, 48200) == 0.0f);
  // Sync: a 120 BPM file under a 60 BPM project plays at half speed (1 s of file in 2 s).
  engine::PreviewOptions sync;
  sync.sync = true;
  sync.fileBpm = 120.0;
  sync.projectBpm = 60.0;
  REQUIRE(p.play(*s, 0.0, sync).ok());
  (void)run(p, 750);  // 1 s
  CHECK(p.positionSeconds() == doctest::Approx(0.5).epsilon(0.01));
  CHECK(p.state() == engine::PreviewPlayer::State::Playing);
  // Gain: half the level (ramped over one block).
  REQUIRE(p.play(*s, 0.2, {}).ok());
  (void)run(p, 4);
  Out full = run(p, 4);
  p.setGain(0.5f);
  (void)run(p, 1);
  Out half = run(p, 4);
  CHECK(peak(half.l) == doctest::Approx(0.5 * peak(full.l)).epsilon(0.1));
}

TEST_CASE("preview player: a streamed file plays past its resident head (disk thread engaged)") {
  media::PoolConfig cfg = poolConfig();
  cfg.residentLimitBytes = 0;  // everything streams
  cfg.headFrames = 4096;
  media::SamplePool pool(cfg);
  pool.startDiskThread();
  auto s = pool.load(writeWav("streamed.wav", 96000, 48000));
  REQUIRE(s.ok());
  REQUIRE((*s)->view().streamed());
  engine::PreviewConfig pc;
  pc.sampleRate = kSr;
  pc.streams = &pool.streams();
  {
    engine::PreviewPlayer p(pc);
    pool.engage();
    REQUIRE(p.play(*s, 0.0, {}).ok());
    // Real-time-ish: give the disk thread time between blocks (a device callback every 1.3 ms).
    Out o;
    for (int b = 0; b < 200; ++b) {
      Out one = run(p, 1);
      o.l.insert(o.l.end(), one.l.begin(), one.l.end());
      if (b % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    pool.disengage();
    // Past the 4096-frame head the stream delivered the file's frames.
    size_t wrong = 0;
    for (size_t i = 5000; i < o.l.size(); ++i) wrong += o.l[i] != src(static_cast<int64_t>(i)) ? 1u : 0u;
    CHECK(wrong == 0);
    p.stop();
    (void)run(p, 4);
  }
  pool.stopDiskThread();
}
