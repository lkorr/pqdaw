// T5.5: waveform overviews — every level equals a brute-force min / max (whole files and every published prefix),
// decoding a file gives the same overview, the disk cache round-trips and rejects bad files, the shared cache is
// trimmed least-recently-used first, sample windows, the analysis worker, and (perf) the 10-minute build / load times.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#include "pq/media/decoder.h"
#include "pq/media/hash.h"
#include "pq/media/wav_file.h"
#include "pq/media/waveform.h"

using namespace pq;
using namespace pq::media;
namespace fs = std::filesystem;

namespace {

using Planar = std::vector<std::vector<float>>;

Planar signal(uint32_t channels, uint64_t frames, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> noise(-0.05f, 0.05f);
  Planar p(channels, std::vector<float>(frames));
  for (uint32_t c = 0; c < channels; ++c)
    for (uint64_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(i);
      // A slow envelope over a sweep plus noise and the occasional full-scale spike (clipping at ±1).
      const double env = 0.5 + 0.45 * std::sin(t * 1e-4 + c);
      float v = static_cast<float>(env * std::sin(t * (0.001 + 1e-8 * t) + c)) + noise(rng);
      if (i % 9973 == 17) v = (i & 1) ? 1.2f : -1.3f;
      p[c][i] = v;
    }
  return p;
}

// Brute force: level L bucket k of channel c over frames [k · fpb, min((k + 1) · fpb, n)).
void checkLevels(const WaveformOverview& o, const Planar& p, uint64_t prefixFrames) {
  for (std::size_t L = 0; L < o.levelCount(); ++L) {
    const auto& lv = o.level(L);
    const uint64_t valid = o.validBuckets(L);
    CHECK(valid <= lv.buckets);
    for (uint32_t c = 0; c < o.channels(); ++c) {
      const int16_t* mm = o.minMax(L, c);
      for (uint64_t k = 0; k < valid; ++k) {
        const uint64_t a = k * lv.framesPerBucket, e = std::min<uint64_t>(a + lv.framesPerBucket, o.frames());
        REQUIRE(e <= prefixFrames);
        float lo = p[c][a], hi = p[c][a];
        for (uint64_t i = a; i < e; ++i) {
          lo = std::min(lo, p[c][i]);
          hi = std::max(hi, p[c][i]);
        }
        if (mm[2 * k] != quantiseMin(lo) || mm[2 * k + 1] != quantiseMax(hi)) {
          FAIL_CHECK("level " << L << " channel " << c << " bucket " << k << ": " << mm[2 * k] << "/" << mm[2 * k + 1]
                              << " vs " << quantiseMin(lo) << "/" << quantiseMax(hi));
          return;
        }
      }
    }
  }
}

fs::path tempDir(const char* name) {
  const fs::path d = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return d;
}

// A 16-bit PCM WAV (the common sample format), written directly.
void writePcm16(const fs::path& path, const Planar& p, uint32_t rate) {
  const uint32_t ch = static_cast<uint32_t>(p.size());
  const uint64_t frames = p[0].size();
  const uint32_t dataBytes = static_cast<uint32_t>(frames * ch * 2);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
  out.write("RIFF", 4);
  u32(36 + dataBytes);
  out.write("WAVEfmt ", 8);
  u32(16);
  u16(1);
  u16(static_cast<uint16_t>(ch));
  u32(rate);
  u32(rate * ch * 2);
  u16(static_cast<uint16_t>(ch * 2));
  u16(16);
  out.write("data", 4);
  u32(dataBytes);
  std::vector<int16_t> buf;
  constexpr uint64_t kChunk = 1 << 16;
  for (uint64_t at = 0; at < frames; at += kChunk) {
    const uint64_t n = std::min(kChunk, frames - at);
    buf.resize(n * ch);
    for (uint64_t i = 0; i < n; ++i)
      for (uint32_t c = 0; c < ch; ++c)
        buf[i * ch + c] = static_cast<int16_t>(std::clamp(std::lround(p[c][at + i] * 32767.0f), -32768l, 32767l));
    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size() * 2));
  }
}

}  // namespace

TEST_CASE("waveform overview: every level equals a brute-force min / max (lengths, chunkings, channels)") {
  for (const uint64_t frames : {uint64_t{1}, uint64_t{255}, uint64_t{256}, uint64_t{257}, uint64_t{4096 * 4 + 3}, uint64_t{100000}}) {
    for (const uint32_t channels : {1u, 2u, 3u}) {
      const Planar p = signal(channels, frames, static_cast<uint32_t>(frames + channels));
      std::shared_ptr<WaveformOverview> o = WaveformOverview::create(channels, frames, 48000, 7);
      OverviewBuilder b(*o);
      std::mt19937 rng(static_cast<uint32_t>(frames));
      std::uniform_int_distribution<uint64_t> chunk(1, 3000);
      std::vector<const float*> ptrs(channels);
      for (uint64_t at = 0; at < frames;) {
        const uint64_t n = std::min(chunk(rng), frames - at);
        for (uint32_t c = 0; c < channels; ++c) ptrs[c] = p[c].data() + at;
        b.add(ptrs.data(), n);
        at += n;
      }
      b.finish();
      CAPTURE(frames);
      CAPTURE(channels);
      CHECK(o->complete());
      CHECK(o->level(o->levelCount() - 1).buckets == 1);
      checkLevels(*o, p, frames);
    }
  }
}

TEST_CASE("waveform overview: published prefixes are exact and fill in left to right") {
  const uint64_t frames = 3 * kOverviewPublishFrames + 12345;
  const Planar p = signal(2, frames, 3);
  std::shared_ptr<WaveformOverview> o = WaveformOverview::create(2, frames, 48000, 1);
  std::vector<uint64_t> seen;
  OverviewBuilder b(*o, [&] {
    seen.push_back(o->builtFrames());
    checkLevels(*o, p, o->builtFrames());
  });
  std::vector<const float*> ptrs(2);
  constexpr uint64_t kChunk = 70000;  // not a divisor of the publish interval
  for (uint64_t at = 0; at < frames; at += kChunk) {
    const uint64_t n = std::min(kChunk, frames - at);
    ptrs[0] = p[0].data() + at;
    ptrs[1] = p[1].data() + at;
    b.add(ptrs.data(), n);
    CHECK(o->builtFrames() <= b.position());
  }
  b.finish();
  REQUIRE(seen.size() == 4);  // three intermediate publishes + the final one
  CHECK(std::is_sorted(seen.begin(), seen.end()));
  CHECK(seen.back() == frames);
  checkLevels(*o, p, frames);
}

TEST_CASE("waveform overview: built from a decoded file equals the overview of its samples; cache round trip") {
  const fs::path dir = tempDir("pq_waveform_build");
  const uint64_t frames = 300000;
  const Planar p = signal(2, frames, 11);
  const fs::path wav = dir / "a.wav";
  REQUIRE(writeWav(wav, p, 44100).ok());
  auto d = Decoder::open(wav);
  REQUIRE(d.ok());
  int publishes = 0;
  auto o = buildOverview(**d, 99, [&](const OverviewPtr&) { ++publishes; });
  REQUIRE(o.ok());
  CHECK(publishes == 1);  // shorter than one publish interval: just the end
  CHECK((*o)->sampleRate() == 44100);
  CHECK((*o)->contentHash() == 99);
  checkLevels(**o, p, frames);

  // Save / load: identical data; a truncated file, a wrong hash and garbage are rejected.
  const fs::path f = overviewCacheFile(dir / "cache", 99, 1234);
  REQUIRE(saveOverview(**o, f).ok());
  auto back = loadOverview(f, 99);
  REQUIRE(back.ok());
  CHECK((*back)->complete());
  CHECK((*back)->frames() == frames);
  CHECK(std::equal((*back)->data().begin(), (*back)->data().end(), (*o)->data().begin(), (*o)->data().end()));
  CHECK_FALSE(loadOverview(f, 98).ok());
  {
    std::error_code ec;
    fs::resize_file(f, fs::file_size(f) - 2, ec);
    REQUIRE(!ec);
  }
  CHECK_FALSE(loadOverview(f, 99).ok());
  {
    std::ofstream g(f, std::ios::binary | std::ios::trunc);
    g << "not an overview";
  }
  CHECK_FALSE(loadOverview(f, 0).ok());
  fs::remove_all(dir);
}

TEST_CASE("waveform overview: loadOrBuild builds once, then reads the cache; the shared cache is trimmed LRU") {
  const fs::path dir = tempDir("pq_waveform_cache");
  const fs::path cache = dir / "waveforms";
  std::vector<fs::path> wavs;
  for (int i = 0; i < 3; ++i) {
    wavs.push_back(dir / ("s" + std::to_string(i) + ".wav"));
    REQUIRE(writeWav(wavs.back(), signal(1, 50000 + 1000 * static_cast<uint64_t>(i), static_cast<uint32_t>(i)), 48000).ok());
  }
  OverviewRequest r;
  r.path = wavs[0];
  r.cacheDir = cache;
  auto a = loadOrBuildOverview(r);
  REQUIRE(a.ok());
  const uint64_t hash = *contentHash(wavs[0]);
  const fs::path f0 = overviewCacheFile(cache, hash, fs::file_size(wavs[0]), sourceMtimeOf(wavs[0]));
  REQUIRE(fs::exists(f0));
  CHECK((*a)->contentHash() == hash);
  // A second request reads the file: same data, and a marker written into the cache file's header is what comes back.
  auto b = loadOrBuildOverview(r);
  REQUIRE(b.ok());
  CHECK(std::equal((*a)->data().begin(), (*a)->data().end(), (*b)->data().begin(), (*b)->data().end()));
  {
    std::fstream g(f0, std::ios::binary | std::ios::in | std::ios::out);
    g.seekp(64);  // the first level-0 min of the data
    const int16_t marker = -12345;
    g.write(reinterpret_cast<const char*>(&marker), 2);
  }
  auto c = loadOrBuildOverview(r);
  REQUIRE(c.ok());
  CHECK((*c)->minMax(0, 0)[0] == -12345);

  // LRU: three files, a limit that holds two: the least recently written goes.
  const uint64_t one = fs::file_size(f0);
  r.cacheLimitBytes = 2 * one + one / 2;
  for (int i = 1; i < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // distinct write times
    r.path = wavs[static_cast<size_t>(i)];
    REQUIRE(loadOrBuildOverview(r).ok());
  }
  CHECK_FALSE(fs::exists(f0));  // oldest written, trimmed when the third was saved
  int left = 0;
  for (const auto& e : fs::directory_iterator(cache)) left += e.path().extension() == ".pqwf" ? 1 : 0;
  CHECK(left == 2);
  fs::remove_all(dir);
}

TEST_CASE("waveform overview: sample windows read what a decode reads, clamped to the file") {
  const fs::path dir = tempDir("pq_waveform_window");
  const Planar p = signal(2, 20000, 5);
  const fs::path wav = dir / "w.wav";
  REQUIRE(writeWav(wav, p, 48000).ok());
  auto w = readSampleWindow(wav, 1000, 3000);
  REQUIRE(w.ok());
  CHECK((*w)->frame0 == 1000);
  CHECK((*w)->frames == 3000);
  for (uint32_t c = 0; c < 2; ++c)
    for (int64_t i = 0; i < 3000; i += 97) CHECK((*w)->channel(c)[i] == p[c][static_cast<size_t>(1000 + i)]);
  auto e = readSampleWindow(wav, 19000, 5000);
  REQUIRE(e.ok());
  CHECK((*e)->frames == 1000);
  auto neg = readSampleWindow(wav, -50, 100);
  REQUIRE(neg.ok());
  CHECK((*neg)->frame0 == 0);
  CHECK((*neg)->frames == 50);
  fs::remove_all(dir);
}

TEST_CASE("analysis worker: runs posted jobs in order on its thread; waitIdle") {
  AnalysisWorker w;
  std::vector<int> order;
  std::atomic<std::thread::id> where{};
  for (int i = 0; i < 20; ++i)
    w.post([&, i] {
      order.push_back(i);
      where = std::this_thread::get_id();
    });
  w.waitIdle();
  CHECK(w.idle());
  REQUIRE(order.size() == 20);
  CHECK(std::is_sorted(order.begin(), order.end()));
  CHECK(where.load() != std::this_thread::get_id());
}

TEST_CASE("waveform overview (perf): a 10-minute 48 kHz stereo WAV builds in <= 250 ms and loads from the cache in <= 5 ms" *
          doctest::test_suite("perf")) {
  const fs::path dir = tempDir("pq_waveform_perf");
  const uint64_t frames = 10ull * 60 * 48000;
  const fs::path wav = dir / "ten_minutes.wav";
  writePcm16(wav, signal(2, frames, 1), 48000);
  // Warm the OS cache (the criterion says warm cache), then the median of 5 builds.
  { std::ifstream in(wav, std::ios::binary); std::vector<char> b(1 << 20); while (in.read(b.data(), static_cast<std::streamsize>(b.size()))) {} }
  std::vector<double> build, load;
  OverviewPtr last;
  for (int i = 0; i < 5; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    auto d = Decoder::open(wav);
    REQUIRE(d.ok());
    auto o = buildOverview(**d, 1);
    REQUIRE(o.ok());
    build.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    last = *o;
  }
  const fs::path f = overviewCacheFile(dir / "cache", 1, fs::file_size(wav));
  REQUIRE(saveOverview(*last, f).ok());
  for (int i = 0; i < 9; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    auto o = loadOverview(f, 1);
    REQUIRE(o.ok());
    load.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(build.begin(), build.end());
  std::sort(load.begin(), load.end());
  MESSAGE("10-min stereo overview: build median " << build[2] << " ms, cache load median " << load[4] << " ms ("
                                                    << last->bytes() / 1024 << " KiB)");
  CHECK(build[2] <= 250.0);
  CHECK(load[4] <= 5.0);
  fs::remove_all(dir);
}
