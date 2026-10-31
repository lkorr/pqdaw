// T5.2: the sample pool and RT-safe disk streaming. RtScope on claim / span / release (underrun, wrap) and on
// SampleCursor; resident vs streamed residency; offline (blocking) reads of streamed WAV and FLAC (decoded cache)
// bit-identical to a direct decode, both directions; prefetch on locate; memory budget and LRU demotion; the idle
// disk thread's wakeups; a two-thread stress of random claim/read/release cycles; and (perf) 64 concurrent streams
// of 10-minute files at buffer 64 paced by a NullDevice for 2 minutes with 0 underruns.

#include <doctest/doctest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "media_test_util.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/null_device.h"
#include "pq/media/decoder.h"
#include "pq/media/hash.h"
#include "pq/media/sample_pool.h"
#include "pq/media/stream.h"
#include "pq/media/waveform.h"

using namespace pq;
using namespace pq::media;
using namespace pq::media::test;
using Clock = std::chrono::steady_clock;

namespace {

struct FakeSample {
  SampleView v;
  const SampleView& view() const noexcept { return v; }
};

void commitRing(StreamPool& p, uint32_t slot) {
  REQUIRE(VirtualAlloc(p.ring(slot), p.ringFloats() * sizeof(float), MEM_COMMIT, PAGE_READWRITE) != nullptr);
}

// Writes frames [first, first + n) of a ramp (value = frame + 0.25 · channel) into slot `slot`'s ring and publishes
// `frontier`, as the disk thread would.
void fillRamp(StreamPool& p, uint32_t slot, uint32_t channels, int64_t first, int64_t n, int64_t frontier) {
  const StreamPool::Request r = p.request(slot);
  const int64_t cap = p.ringFrames(channels);
  float* ring = p.ring(slot);
  for (int64_t f = first; f < first + n; ++f)
    for (uint32_t c = 0; c < channels; ++c) ring[c * cap + f % cap] = static_cast<float>(f) + 0.25f * c;
  p.publishFill(slot, r.gen, frontier);
}

// Flattens a span's channel c into a vector (file order).
std::vector<float> flatten(const FrameSpan& s, uint32_t c) {
  std::vector<float> out;
  for (uint32_t i = 0; i < s.pieces; ++i) out.insert(out.end(), s.piece[i].ch[c], s.piece[i].ch[c] + s.piece[i].frames);
  return out;
}

// A 16-bit PCM WAV whose sample at (frame, channel) is pattern(frame, channel, seed).
int16_t pattern(int64_t f, uint32_t c, uint32_t seed) noexcept {
  return static_cast<int16_t>(static_cast<uint32_t>(f * 31 + c * 7919 + seed * 1009) & 0xFFFFu);
}
void writePatternWav(const fs::path& p, int64_t frames, uint32_t channels, uint32_t seed) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  const uint32_t data = static_cast<uint32_t>(frames * channels * 2);
  uint8_t hdr[44];
  auto put32 = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
  auto put16 = [&](int at, uint16_t v) { std::memcpy(hdr + at, &v, 2); };
  std::memcpy(hdr, "RIFF", 4);
  put32(4, 36 + data);
  std::memcpy(hdr + 8, "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);
  put16(22, static_cast<uint16_t>(channels));
  put32(24, 48000);
  put32(28, 48000 * channels * 2);
  put16(32, static_cast<uint16_t>(channels * 2));
  put16(34, 16);
  std::memcpy(hdr + 36, "data", 4);
  put32(40, data);
  DWORD w = 0;
  WriteFile(h, hdr, 44, &w, nullptr);
  std::vector<int16_t> buf(static_cast<std::size_t>(1 << 18) * channels);
  for (int64_t f0 = 0; f0 < frames; f0 += 1 << 18) {
    const int64_t n = std::min<int64_t>(1 << 18, frames - f0);
    for (int64_t f = 0; f < n; ++f)
      for (uint32_t c = 0; c < channels; ++c) buf[static_cast<std::size_t>(f * channels + c)] = pattern(f0 + f, c, seed);
    WriteFile(h, buf.data(), static_cast<DWORD>(n * channels * 2), &w, nullptr);
  }
  FlushFileBuffers(h);  // on disk before any test reads it (a lazy flush would compete with the streaming reads)
  CloseHandle(h);
}

fs::path tempDir(const char* name) {
  const fs::path d = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return d;
}

// Reads `total` frames through a cursor with blocking fills (an offline render) into planar vectors.
template <class S>
Planar renderThrough(SamplePool& pool, const S& sample, int64_t from, Direction dir, int64_t total, uint32_t block) {
  const uint32_t ch = sample.view().channels;
  Planar out(ch, std::vector<float>(static_cast<std::size_t>(total)));
  SampleCursor<S> cur;
  cur.start(pool.streams(), sample, from, dir);
  int64_t done = 0;
  while (done < total) {
    const auto n = static_cast<uint32_t>(std::min<int64_t>(block, total - done));
    if (cur.streaming()) REQUIRE(pool.waitFilled(cur.stream(), n));
    const FrameSpan s = cur.next(n);
    REQUIRE(s.missing == 0);
    for (uint32_t c = 0; c < ch; ++c) {
      const std::vector<float> v = flatten(s, c);
      // Reverse reads come in file order: place them below the previous ones.
      const int64_t at = dir == Direction::Forward ? done : total - done - n;
      std::copy(v.begin(), v.end(), out[c].begin() + at);
    }
    done += n;
  }
  return out;
}

double systemBusyPercent(FILETIME idle0, FILETIME kern0, FILETIME user0) {
  FILETIME idle1, kern1, user1;
  GetSystemTimes(&idle1, &kern1, &user1);
  auto u = [](FILETIME f) { return (uint64_t{f.dwHighDateTime} << 32) | f.dwLowDateTime; };
  const double idle = static_cast<double>(u(idle1) - u(idle0));
  const double total = static_cast<double>(u(kern1) - u(kern0) + u(user1) - u(user0));
  return total > 0 ? 100.0 * (1.0 - idle / total) : 0.0;
}

}  // namespace

TEST_CASE("stream: claim / span / release are RT-safe; underrun reads silence; the ring wraps") {
  StreamPool pool(4, 2 * 1000);  // stereo rings of 1000 frames
  const uint32_t slot = 0;
  StreamId id;
  {
    RtScope rt;
    id = pool.claim(7, 100, Direction::Forward, 2, 1u << 20);
  }
  REQUIRE(id.valid());
  commitRing(pool, id.slot);
  CHECK(pool.freeSlots() == 3);
  FrameSpan s;
  {
    RtScope rt;
    s = pool.span(id, 64);  // nothing filled yet: silence, one underrun
  }
  CHECK(s.missing == 64);
  CHECK(pool.underruns() == 1);
  CHECK(flatten(s, 0) == std::vector<float>(64, 0.0f));
  // The disk side fills [164, 1100): the ring index wraps at 1000.
  fillRamp(pool, id.slot, 2, 164, 936, 1100);
  {
    RtScope rt;
    s = pool.span(id, 900);  // [164, 1064): crosses frame 1000 → two pieces
  }
  CHECK(s.missing == 0);
  CHECK(s.pieces == 2);
  const std::vector<float> l = flatten(s, 0), r = flatten(s, 1);
  bool ok = true;
  for (int i = 0; i < 900; ++i) ok = ok && l[i] == 164.0f + i && r[i] == 164.25f + i;
  CHECK(ok);
  CHECK(pool.consumedPosition(id.slot, id.gen) == 164);  // the frames just returned stay protected
  {
    RtScope rt;
    s = pool.span(id, 100);  // [1064, 1164): 36 delivered, 64 late
  }
  CHECK(s.missing == 64);
  CHECK(flatten(s, 0)[35] == 1099.0f);
  CHECK(flatten(s, 0)[36] == 0.0f);
  CHECK(pool.underruns() == 2);
  {
    RtScope rt;
    pool.release(id);
  }
  CHECK(pool.freeSlots() == 4);
  // A new claim of the same slot does not see the previous generation's fill.
  StreamId again;
  {
    RtScope rt;
    again = pool.claim(7, 164, Direction::Forward, 2, 1u << 20);
    s = pool.span(again, 16);
  }
  CHECK(again.slot == slot);
  CHECK(again.gen != id.gen);
  CHECK(s.missing == 16);
  // Past the file's end: silence, not an underrun.
  StreamId tail;
  const uint64_t before = pool.underruns();
  {
    RtScope rt;
    tail = pool.claim(8, 990, Direction::Forward, 1, 1000);
  }
  commitRing(pool, tail.slot);
  fillRamp(pool, tail.slot, 1, 990, 10, 1000);
  {
    RtScope rt;
    s = pool.span(tail, 32);
    pool.release(tail);
    pool.release(again);
  }
  CHECK(s.missing == 0);
  CHECK(pool.underruns() == before);
  // Every slot taken: claim fails without blocking.
  StreamId all[5];
  {
    RtScope rt;
    for (StreamId& a : all) a = pool.claim(1, 0, Direction::Forward, 1, 10);
  }
  CHECK_FALSE(all[4].valid());
  {
    RtScope rt;
    for (int i = 0; i < 4; ++i) pool.release(all[i]);
  }
  CHECK(pool.freeSlots() == 4);
}

TEST_CASE("stream: reverse spans and the SampleCursor over head + stream, both directions (RtScope)") {
  StreamPool pool(2, 2 * 512);
  // A 3000-frame stereo "file": resident head of 1000 frames (ramp), the rest streamed.
  std::vector<float> head(2 * (1000 + 2 * kSampleGuardFrames), 0.0f);
  FakeSample fs;
  fs.v.channels = 2;
  fs.v.frames = 3000;
  fs.v.residentFrames = 1000;
  fs.v.sampleRate = 48000;
  fs.v.streamSource = 5;
  for (uint32_t c = 0; c < 2; ++c) {
    float* d = head.data() + c * (1000 + 2 * kSampleGuardFrames) + kSampleGuardFrames;
    for (int f = 0; f < 1000; ++f) d[f] = static_cast<float>(f) + 0.25f * c;
    fs.v.data[c] = d;
  }
  // Forward from 900: 100 frames from the head, then the stream from 1000.
  SampleCursor<FakeSample> cur;
  {
    RtScope rt;
    cur.start(pool, fs, 900, Direction::Forward);
  }
  REQUIRE(cur.streaming());
  commitRing(pool, cur.stream().slot);
  fillRamp(pool, cur.stream().slot, 2, 1000, 400, 1400);
  FrameSpan s;
  {
    RtScope rt;
    s = cur.next(256);  // [900, 1156)
  }
  CHECK(s.missing == 0);
  std::vector<float> l = flatten(s, 0);
  bool ok = l.size() == 256;
  for (int i = 0; i < 256 && ok; ++i) ok = l[static_cast<std::size_t>(i)] == 900.0f + i;
  CHECK(ok);
  {
    RtScope rt;
    cur.stop();
  }
  // Reverse from 1200 (exclusive) down: the stream serves [1000, 1200), then the head.
  {
    RtScope rt;
    cur.start(pool, fs, 1200, Direction::Reverse);
  }
  REQUIRE(cur.streaming());
  commitRing(pool, cur.stream().slot);
  {
    const StreamPool::Request r = pool.request(cur.stream().slot);
    CHECK(r.dir == Direction::Reverse);
    CHECK(r.start == 1200);
  }
  fillRamp(pool, cur.stream().slot, 2, 1000, 200, 1000);  // reverse frontier: the lowest filled frame
  {
    RtScope rt;
    s = cur.next(300);  // [900, 1200) in file order
  }
  CHECK(s.missing == 0);
  l = flatten(s, 0);
  ok = l.size() == 300;
  for (int i = 0; i < 300 && ok; ++i) ok = l[static_cast<std::size_t>(i)] == 900.0f + i;
  CHECK(ok);
  CHECK(cur.position() == 900);
  {
    RtScope rt;
    s = cur.next(1000);  // [-100, 900): 100 frames of silence below 0, then the head
  }
  CHECK(s.missing == 0);
  l = flatten(s, 0);
  CHECK(l[0] == 0.0f);
  CHECK(l[99] == 0.0f);
  CHECK(l[100] == 0.0f);  // frame 0
  CHECK(l[999] == 899.0f);
  {
    RtScope rt;
    cur.stop();
  }
  CHECK(pool.freeSlots() == 2);
}

TEST_CASE("pool: small files are fully resident (no stream), one entry per file") {
  SamplePool pool;
  auto a = pool.load(fixture("wav_pcm24.wav"));
  REQUIRE(a.ok());
  const SampleView& v = (*a)->view();
  CHECK_FALSE(v.streamed());
  CHECK(v.frames == 6000);
  CHECK(v.residentFrames == 6000);
  CHECK(v.streamSource == 0);
  CHECK(v.data[0][-1] == 0.0f);  // guard frames
  CHECK(v.data[0][6000] == 0.0f);
  const Planar ref = referencePcm24();
  CHECK(std::equal(ref[0].begin(), ref[0].end(), v.data[0]));
  CHECK(std::equal(ref[1].begin(), ref[1].end(), v.data[1]));
  auto b = pool.load(fixture("wav_pcm24.wav"));
  REQUIRE(b.ok());
  CHECK(a->get() == b->get());
  // Reading a resident sample never touches a stream slot.
  SampleCursor<PooledSample> cur;
  {
    RtScope rt;
    cur.start(pool.streams(), **a, 0, Direction::Forward);
    const FrameSpan s = cur.next(4096);
    CHECK(s.missing == 0);
    CHECK(s.pieces == 1);
  }
  CHECK_FALSE(cur.streaming());
  CHECK(pool.streams().freeSlots() == pool.streams().slotCount());
  CHECK(pool.stats().samples == 1);
  CHECK(pool.stats().residentBytes == (6000 + 2 * kSampleGuardFrames) * 2 * sizeof(float));
  a = Error{};
  b = Error{};
  CHECK(pool.stats().samples == 0);
  CHECK(pool.stats().residentBytes == 0);
}

TEST_CASE("pool: streamed WAV and FLAC read through the disk thread equal a direct decode (offline, both directions)") {
  const fs::path cacheDir = tempDir("pq_media_stream_cache");
  PoolConfig cfg;
  cfg.residentLimitBytes = 0;  // everything streams
  cfg.headFrames = 1000;
  cfg.streamSlots = 8;
  cfg.ringFloats = 2 * 1536;  // small rings: many refills and wraps
  cfg.decodedCacheDir = cacheDir;
  SamplePool pool(cfg);
  pool.startDiskThread();
  for (const char* name : {"wav_pcm16.wav", "aiff_pcm24.aiff", "flac_24.flac", "mp3_vbr.mp3"}) {
    CAPTURE(std::string(name));
    auto s = pool.load(fixture(name));
    REQUIRE(s.ok());
    const SampleView& v = (*s)->view();
    CHECK(v.streamed());
    CHECK(v.residentFrames == 1000);
    auto d = Decoder::open(fixture(name));
    REQUIRE(d.ok());
    const Planar ref = decodeAll(**d);
    const int64_t n = static_cast<int64_t>(ref[0].size());
    const Planar fwd = renderThrough(pool, **s, 0, Direction::Forward, n, 333);
    CHECK(fwd == ref);
    {  // the same render from a fully resident load of the file (default pool) is bit-identical
      SamplePool residentPool;
      auto r = residentPool.load(fixture(name));
      REQUIRE(r.ok());
      REQUIRE_FALSE((*r)->view().streamed());
      CHECK(renderThrough(residentPool, **r, 0, Direction::Forward, n, 333) == fwd);
    }
    const Planar rev = renderThrough(pool, **s, n, Direction::Reverse, n, 257);
    CHECK(rev == ref);
    // From the middle.
    const Planar mid = renderThrough(pool, **s, 2500, Direction::Forward, 1000, 64);
    CHECK(std::equal(mid[0].begin(), mid[0].end(), ref[0].begin() + 2500));
  }
  // Compressed files went through the decoded cache; PCM files did not.
  int cached = 0;
  for (const auto& e : fs::directory_iterator(cacheDir)) cached += e.path().extension() == ".f32";
  CHECK(cached == 2);
  // A second pool reuses the cache file (no re-decode: same content).
  {
    SamplePool again(cfg);
    again.startDiskThread();
    auto s = again.load(fixture("flac_24.flac"));
    REQUIRE(s.ok());
    const Planar fwd = renderThrough(again, **s, 0, Direction::Forward, 6000, 512);
    CHECK(fwd == referencePcm24());
  }
  CHECK(pool.stats().underruns == 0);
  pool.stopDiskThread();
  std::error_code ec;
  fs::remove_all(cacheDir, ec);
}

// M5 review H2: two A-law WAVs (decoded through the .f32 cache, 1 byte per frame, so an edit keeps the size, like
// a CBR MP3 re-export) that differ only outside the content hash's three windows. They used to share one cache file,
// so the second played the first one's audio. A third load of the first file reuses its own cache file.
TEST_CASE("pool: two compressed files with one sampled hash and size never share a decoded cache file") {
  const fs::path dir = tempDir("pq_media_cache_identity");
  auto alaw = [](const std::vector<uint8_t>& frames) {
    std::vector<uint8_t> h;
    auto le16 = [&](uint16_t v) { h.push_back(uint8_t(v)), h.push_back(uint8_t(v >> 8)); };
    auto le32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) h.push_back(uint8_t(v >> (8 * i))); };
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    le32(36 + static_cast<uint32_t>(frames.size()));
    h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    le32(16), le16(6), le16(1), le32(48000), le32(48000), le16(1), le16(8);  // A-law, mono
    h.insert(h.end(), {'d', 'a', 't', 'a'});
    le32(static_cast<uint32_t>(frames.size()));
    h.insert(h.end(), frames.begin(), frames.end());
    return h;
  };
  std::mt19937 rng(1);
  std::vector<uint8_t> fa(1u << 20);
  for (auto& x : fa) x = static_cast<uint8_t>(rng());
  std::vector<uint8_t> fb = fa;
  for (std::size_t i = 100000; i < 120000; ++i) fb[i] ^= 0x55;  // outside the start, middle and end windows
  const fs::path a = dir / "take1.wav", b = dir / "take2.wav";
  writeBytes(a, alaw(fa));
  writeBytes(b, alaw(fb));
  fs::last_write_time(b, fs::last_write_time(a) + std::chrono::seconds(5));  // a later export
  REQUIRE(*contentHash(a) == *contentHash(b));  // the collision this test is about
  PoolConfig cfg;
  cfg.residentLimitBytes = 0;  // stream: decoded into the cache
  cfg.headFrames = 1u << 17;  // the head covers the edited frames
  cfg.streamSlots = 4;
  cfg.ringFloats = 2 * 4096;
  cfg.decodedCacheDir = dir / "cache";
  auto headMatches = [](const SampleView& v, const fs::path& file) {
    auto d = Decoder::open(file);
    REQUIRE(d.ok());
    const Planar ref = decodeAll(**d);
    REQUIRE(v.residentFrames == 1 << 17);
    std::size_t wrong = 0;
    for (std::size_t f = 0; f < static_cast<std::size_t>(v.residentFrames); ++f) wrong += v.data[0][f] != ref[0][f];
    return wrong;
  };
  {
    SamplePool pool(cfg);
    auto sa = pool.load(a);
    auto sb = pool.load(b);
    REQUIRE(sa.ok());
    REQUIRE(sb.ok());
    REQUIRE((*sb)->view().streamed());
    CHECK(headMatches((*sa)->view(), a) == 0);
    CHECK(headMatches((*sb)->view(), b) == 0);
  }
  int files = 0;
  for (const auto& e : fs::directory_iterator(dir / "cache")) files += e.path().extension() == ".f32";
  CHECK(files == 2);
  {  // a new session: each file reuses its own cache file
    SamplePool pool(cfg);
    auto sb = pool.load(b);
    REQUIRE(sb.ok());
    CHECK(headMatches((*sb)->view(), b) == 0);
    auto sa = pool.load(a);
    REQUIRE(sa.ok());
    CHECK(headMatches((*sa)->view(), a) == 0);
  }
  files = 0;
  for (const auto& e : fs::directory_iterator(dir / "cache")) files += e.path().extension() == ".f32";
  CHECK(files == 2);
  // The waveform overview cache has the same key: one file each.
  for (const fs::path& f : {a, b, a}) {
    OverviewRequest r;
    r.path = f;
    r.cacheDir = dir / "pqwf";
    REQUIRE(loadOrBuildOverview(r).ok());
  }
  files = 0;
  for (const auto& e : fs::directory_iterator(dir / "pqwf")) files += e.path().extension() == ".pqwf";
  CHECK(files == 2);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("pool: a locate prefetches; Play adopts the stream with data ready (≤ 50 ms, indicative)") {
  const fs::path dir = tempDir("pq_media_prefetch");
  const fs::path file = dir / "long.wav";
  writePatternWav(file, 48000 * 60, 2, 3);  // 1 min stereo 16-bit, 11 MB
  PoolConfig cfg;
  cfg.residentLimitBytes = 1u << 20;
  cfg.streamSlots = 16;
  SamplePool pool(cfg);
  pool.startDiskThread();
  auto s = pool.load(file);
  REQUIRE(s.ok());
  REQUIRE((*s)->view().streamed());
  std::mt19937 rng(11);
  std::vector<double> ms;
  for (int i = 0; i < 10; ++i) {
    const int64_t at = 200000 + rng() % (48000 * 50);
    const uint32_t freeBefore = pool.streams().freeSlots();
    const auto t0 = Clock::now();
    REQUIRE(pool.prefetch(**s, at, Direction::Forward));
    CHECK(pool.streams().freeSlots() == freeBefore - 1);
    SampleCursor<PooledSample> cur;
    cur.start(pool.streams(), **s, at, Direction::Forward);  // adopts the prefetched stream
    CHECK(pool.streams().freeSlots() == freeBefore - 1);
    REQUIRE(cur.streaming());
    while (pool.streams().buffered(cur.stream()) < 4096 && Clock::now() - t0 < std::chrono::seconds(2))
      std::this_thread::yield();
    ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    const FrameSpan sp = cur.next(4096);
    CHECK(sp.missing == 0);
    const std::vector<float> l = flatten(sp, 0);
    bool ok = true;
    for (int f = 0; f < 4096; ++f) ok = ok && l[static_cast<std::size_t>(f)] == pattern(at + f, 0, 3) / 32768.0f;
    CHECK(ok);
  }
  std::sort(ms.begin(), ms.end());
  MESSAGE("locate → 4096 frames ready: median " << ms[ms.size() / 2] << " ms, max " << ms.back() << " ms");
  CHECK(ms.back() <= 50.0);
  // A cleared prefetch frees its slot.
  REQUIRE(pool.prefetch(**s, 1234567, Direction::Forward));
  pool.clearPrefetches();
  CHECK(pool.streams().freeSlots() == pool.streams().slotCount());
  pool.stopDiskThread();
  s = Error{};
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("pool: ring and head memory stay within the documented budget; LRU demotion to streamed") {
  const fs::path dir = tempDir("pq_media_budget");
  EpochReclaimer reclaimer(1);
  auto reader = reclaimer.registerReader();
  REQUIRE(reader);
  PoolConfig cfg;
  cfg.headFrames = 4096;
  cfg.streamSlots = 32;
  cfg.residentLimitBytes = 4u << 20;
  cfg.budgetBytes = 3u << 20;  // room for one 2.2 MB resident file at a time
  cfg.reclaimer = &reclaimer;
  cfg.decodedCacheDir = dir / "cache";
  SamplePool pool(cfg);
  pool.startDiskThread();
  std::vector<SampleRef> refs;
  for (uint32_t i = 0; i < 3; ++i) {
    const fs::path f = dir / ("f" + std::to_string(i) + ".wav");
    writePatternWav(f, 48000 * 6, 2, i);  // 6 s stereo: 2.2 MB as float
    auto s = pool.load(f);
    REQUIRE(s.ok());
    refs.push_back(*s);
  }
  const SamplePool::Stats st = pool.stats();
  CHECK(st.ringBytesReserved == uint64_t{32} * cfg.ringFloats * sizeof(float));
  CHECK(st.ringBytesCommitted <= st.ringBytesReserved);
  CHECK(st.demotions == 2);  // the two older files were demoted as the newer ones came in
  CHECK(st.streamedSamples == 2);
  CHECK(st.headBytes == 2 * (4096 + 2 * kSampleGuardFrames) * 2 * sizeof(float));
  CHECK(st.residentBytes + st.headBytes <= cfg.budgetBytes);
  CHECK(refs[0]->view().streamed());
  CHECK(refs[1]->view().streamed());
  CHECK_FALSE(refs[2]->view().streamed());
  // The replaced residencies are retired through the reclaimer (freed once the reader passes a quiescent state).
  CHECK(reclaimer.pending() == 2);
  reclaimer.quiescent(*reader);
  CHECK(reclaimer.collect() == 2);
  // A demoted sample still reads exactly.
  const Planar fwd = renderThrough(pool, *refs[0], 0, Direction::Forward, 48000 * 6, 4096);
  bool ok = true;
  for (int64_t f = 0; f < 48000 * 6 && ok; f += 997) ok = fwd[1][static_cast<std::size_t>(f)] == pattern(f, 1, 0) / 32768.0f;
  CHECK(ok);
  pool.stopDiskThread();
  refs.clear();
  reclaimer.unregisterReader(*reader);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// T5.x-perf-audio (Q151 item 1): decimated copies of a resident sample: built on demand, level by level (each frame m
// of level L the decimator's output at 2^L·m, computed from level L − 1 with its guards), counted in the budget,
// replaced through the reclaimer when more levels are asked for, retired by a demotion, released with the sample.
TEST_CASE("pool: mips of a resident sample (levels, guards, budget, reclaimer, demotion)") {
  const fs::path dir = tempDir("pq_media_mips");
  EpochReclaimer reclaimer(1);
  auto reader = reclaimer.registerReader();
  REQUIRE(reader);
  PoolConfig cfg;
  cfg.headFrames = 4096;
  cfg.streamSlots = 8;
  cfg.residentLimitBytes = 8u << 20;
  cfg.reclaimer = &reclaimer;
  cfg.decodedCacheDir = dir / "cache";
  SamplePool pool(cfg);
  const fs::path f = dir / "m.wav";
  writePatternWav(f, 48001, 2, 3);  // odd length: level frames round up
  auto s = pool.load(f);
  REQUIRE(s.ok());
  const PooledSample& smp = **s;
  CHECK(smp.mips() == nullptr);
  const uint64_t base = pool.stats().residentBytes;
  REQUIRE(pool.buildMips(smp, 1));
  const SampleMips* m1 = smp.mips();
  REQUIRE(m1 != nullptr);
  CHECK(m1->levels == 1);
  CHECK(m1->level[0].frames == 24001);
  CHECK(m1->level[0].channels == 2);
  CHECK(m1->level[0].sampleRate == 24000);
  const uint64_t one = (24001 + 2 * kSampleGuardFrames) * 2 * sizeof(float);
  CHECK(pool.stats().mipBytes == one);
  CHECK(pool.stats().residentBytes == base + one);
  // Level 1 is the decimator over the sample (zero outside it), guards included.
  const SampleView& v = smp.view();
  for (uint32_t c = 0; c < 2; ++c) {
    std::vector<float> want(24001 + 2 * kSampleGuardFrames);
    dsp::decimate2(v.data[c], 0, v.frames, want.data(), -kSampleGuardFrames, static_cast<int64_t>(want.size()));
    bool same = true;
    for (int64_t k = -kSampleGuardFrames; k < 24001 + kSampleGuardFrames; ++k)
      same = same && m1->level[0].data[c][k] == want[static_cast<size_t>(k + kSampleGuardFrames)];
    CHECK(same);
  }
  CHECK(pool.buildMips(smp, 1));  // already there: nothing rebuilt
  CHECK(smp.mips() == m1);
  CHECK(pool.stats().mipBuilds == 1);
  // More levels: a new set replaces the old one, which waits for the reader's quiescent state.
  REQUIRE(pool.buildMips(smp, 5));  // clamped to kMaxMipLevels
  const SampleMips* m3 = smp.mips();
  REQUIRE(m3 != nullptr);
  CHECK(m3->levels == dsp::kMaxMipLevels);
  CHECK(m3->level[1].frames == 12001);
  CHECK(m3->level[2].frames == 6001);
  CHECK(m3->level[0].data[0][100] == m1->level[0].data[0][100]);
  for (uint32_t c = 0; c < 2; ++c) {  // level 3 from level 2, guards included
    std::vector<float> want(6001);
    dsp::decimate2(m3->level[1].data[c], -kSampleGuardFrames, 12001 + kSampleGuardFrames, want.data(), 0, 6001);
    CHECK(std::equal(want.begin(), want.end(), m3->level[2].data[c]));
  }
  CHECK(reclaimer.pending() == 1);
  reclaimer.quiescent(*reader);
  CHECK(reclaimer.collect() == 1);
  const uint64_t all = one + (12001 + 6001 + 4 * kSampleGuardFrames) * 2 * sizeof(float);
  CHECK(pool.stats().mipBytes == all);
  CHECK(pool.stats().residentBytes == base + all);
  // No room in the budget: none built.
  {
    PoolConfig tight = cfg;
    tight.budgetBytes = base + one / 2;
    SamplePool small(tight);
    auto t = small.load(f);
    REQUIRE(t.ok());
    CHECK_FALSE(small.buildMips(**t, 1));
    CHECK((*t)->mips() == nullptr);
    CHECK(small.stats().mipBytes == 0);
  }
  // Released with the sample.
  s = Error{};
  CHECK(pool.stats().mipBytes == 0);
  CHECK(pool.stats().residentBytes == 0);
  reclaimer.unregisterReader(*reader);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("pool: a demotion retires a sample's mips; a streamed sample gets none") {
  const fs::path dir = tempDir("pq_media_mips_demote");
  EpochReclaimer reclaimer(1);
  auto reader = reclaimer.registerReader();
  REQUIRE(reader);
  PoolConfig cfg;
  cfg.headFrames = 4096;
  cfg.streamSlots = 8;
  cfg.residentLimitBytes = 4u << 20;
  cfg.budgetBytes = 4u << 20;  // room for one 2.2 MB file and its level-1 mip, not for two files
  cfg.reclaimer = &reclaimer;
  cfg.decodedCacheDir = dir / "cache";
  SamplePool pool(cfg);
  const fs::path a = dir / "a.wav", b = dir / "b.wav";
  writePatternWav(a, 48000 * 6, 2, 1);
  writePatternWav(b, 48000 * 6, 2, 2);
  auto sa = pool.load(a);
  REQUIRE(sa.ok());
  REQUIRE(pool.buildMips(**sa, 1));
  CHECK(pool.stats().mipBytes > 0);
  auto sb = pool.load(b);  // demotes a (LRU), mips and all
  REQUIRE(sb.ok());
  CHECK((*sa)->view().streamed());
  CHECK((*sa)->mips() == nullptr);
  CHECK(pool.stats().mipBytes == 0);
  CHECK(pool.stats().residentBytes + pool.stats().headBytes <= cfg.budgetBytes);
  CHECK(reclaimer.pending() == 2);  // the residency and the mips
  reclaimer.quiescent(*reader);
  CHECK(reclaimer.collect() == 2);
  CHECK_FALSE(pool.buildMips(**sa, 1));  // streamed: no mips
  CHECK((*sa)->mips() == nullptr);
  sa = Error{};
  sb = Error{};
  reclaimer.unregisterReader(*reader);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("pool: with no active stream the disk thread makes 0 wakeups in 10 s" * doctest::test_suite("slow")) {
  SamplePool pool;
  pool.startDiskThread();
  std::this_thread::sleep_for(std::chrono::seconds(10));
  CHECK(pool.stats().diskWakeups == 0);
  // Engaged, it polls; disengaged again it goes back to blocking.
  pool.engage();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  pool.disengage();
  const uint64_t polled = pool.stats().diskWakeups;
  CHECK(polled >= 20);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const uint64_t settled = pool.stats().diskWakeups;
  std::this_thread::sleep_for(std::chrono::seconds(1));
  CHECK(pool.stats().diskWakeups == settled);
  pool.stopDiskThread();
}

namespace {

// Two threads: this one claims, reads and releases streams at random (positions, directions, lengths), the disk
// thread fills them. Every frame a span delivers (not silent) must equal the direct decode.
void stress(int cycles, int waitEvery) {
  const fs::path tmp = tempDir("pq_media_stress");
  PoolConfig cfg;
  cfg.residentLimitBytes = 0;
  cfg.headFrames = 512;
  cfg.streamSlots = 8;
  cfg.ringFloats = 2 * 2048;
  cfg.decodedCacheDir = tmp;
  SamplePool pool(cfg);
  pool.startDiskThread();
  pool.engage();
  struct File {
    SampleRef s;
    Planar ref;
  };
  std::vector<File> files;
  for (const char* name : {"wav_pcm24.wav", "flac_16.flac"}) {
    auto s = pool.load(fixture(name));
    REQUIRE(s.ok());
    auto d = Decoder::open(fixture(name));
    REQUIRE(d.ok());
    files.push_back({*s, decodeAll(**d)});
  }
  std::mt19937_64 rng(77);
  uint64_t checked = 0, wrong = 0, silent = 0;
  for (int i = 0; i < cycles; ++i) {
    const File& f = files[rng() % files.size()];
    const int64_t n = f.s->view().frames;
    const Direction dir = (rng() & 1) ? Direction::Reverse : Direction::Forward;
    int64_t pos = static_cast<int64_t>(rng() % static_cast<uint64_t>(n + 1));
    SampleCursor<PooledSample> cur;
    {
      RtScope rt;
      cur.start(pool.streams(), *f.s, pos, dir);
    }
    const int spans = 1 + static_cast<int>(rng() % 4);
    const bool wait = (i % waitEvery) == 0;  // some cycles wait for data, so most spans check real frames
    for (int k = 0; k < spans; ++k) {
      const auto len = static_cast<uint32_t>(1 + rng() % 700);
      if (wait && cur.streaming()) (void)pool.waitFilled(cur.stream(), len, std::chrono::milliseconds(200));
      FrameSpan s;
      {
        RtScope rt;
        s = cur.next(len);
      }
      const int64_t a = dir == Direction::Forward ? pos : pos - len;
      for (uint32_t c = 0; c < 2; ++c) {
        int64_t frame = a;
        for (uint32_t p = 0; p < s.pieces; ++p)
          for (uint32_t j = 0; j < s.piece[p].frames; ++j, ++frame) {
            const float got = s.piece[p].ch[c][j];
            const float want = frame >= 0 && frame < n ? f.ref[c][static_cast<std::size_t>(frame)] : 0.0f;
            if (got == want) ++checked;
            else if (got == 0.0f && s.missing > 0) ++silent;  // a late frame reads as silence
            else ++wrong;
          }
      }
      pos = dir == Direction::Forward ? pos + len : pos - len;
    }
    {
      RtScope rt;
      cur.stop();
    }
  }
  MESSAGE(cycles << " cycles: " << checked << " samples checked, " << silent << " late (silent), " << wrong << " wrong");
  CHECK(wrong == 0);
  CHECK(checked > static_cast<uint64_t>(cycles));
  CHECK(pool.streams().freeSlots() == cfg.streamSlots);
  pool.disengage();
  pool.stopDiskThread();
  files.clear();
  std::error_code ec;
  fs::remove_all(tmp, ec);
}

}  // namespace

TEST_CASE("stream: two-thread stress, 20k random claim / read / release cycles against a direct decode") { stress(20000, 4); }

TEST_CASE("stream: two-thread stress, 10^6 random claim / read / release cycles" * doctest::test_suite("slow")) {
  stress(1000000, 64);
}

TEST_CASE("stream: 64 stereo streams of 10-minute files at buffer 64, 2 min realtime: 0 underruns" *
          doctest::test_suite("perf")) {
  constexpr uint32_t kStreams = 64;
  constexpr int64_t kFrames = 48000LL * 600;
  const fs::path dir = tempDir("pq_media_stream64");
  for (uint32_t i = 0; i < kStreams; ++i) writePatternWav(dir / ("s" + std::to_string(i) + ".wav"), kFrames, 2, i);
  PoolConfig cfg;  // defaults: 256 slots, 2^17-frame heads, 32 MiB resident limit (these are 220 MiB as float)
  SamplePool pool(cfg);
  pool.startDiskThread();
  std::vector<SampleRef> samples;
  for (uint32_t i = 0; i < kStreams; ++i) {
    auto s = pool.load(dir / ("s" + std::to_string(i) + ".wav"));
    REQUIRE(s.ok());
    REQUIRE((*s)->view().streamed());
    samples.push_back(*s);
  }
  struct Player {
    SamplePool* pool;
    std::vector<SampleRef>* samples;
    SampleCursor<PooledSample> cur[kStreams];
    std::atomic<uint64_t> missing{0};
    std::atomic<uint64_t> wrong{0};
    std::atomic<uint64_t> blocks{0};
    std::atomic<uint64_t> firstMiss{UINT64_MAX};
    std::atomic<uint64_t> lastMiss{0};
    bool started = false;
    static void process(void* user, const engine::AudioBlock& b) noexcept {
      auto& p = *static_cast<Player*>(user);
      if (!p.started) {
        // Each stream starts at its own offset (and so its own position in its file).
        for (uint32_t i = 0; i < kStreams; ++i)
          p.cur[i].start(p.pool->streams(), *(*p.samples)[i], int64_t{i} * 4800, Direction::Forward);
        p.started = true;
      }
      uint64_t miss = 0, bad = 0;
      for (uint32_t i = 0; i < kStreams; ++i) {
        const int64_t at = p.cur[i].position();
        const FrameSpan s = p.cur[i].next(b.frames);
        miss += s.missing;
        // Spot-check one frame per block (the first) against the file's pattern.
        if (s.missing == 0 && s.piece[0].ch[1][0] != pattern(at, 1, i) / 32768.0f) ++bad;
      }
      for (uint32_t c = 0; c < b.numOut; ++c) std::fill(b.out[c], b.out[c] + b.frames, 0.0f);
      const uint64_t block = p.blocks.fetch_add(1, std::memory_order_relaxed);
      if (miss) {
        p.missing.fetch_add(miss, std::memory_order_relaxed);
        if (p.firstMiss.load(std::memory_order_relaxed) == UINT64_MAX) p.firstMiss.store(block, std::memory_order_relaxed);
        p.lastMiss.store(block, std::memory_order_relaxed);
      }
      if (bad) p.wrong.fetch_add(bad, std::memory_order_relaxed);
    }
  };
  auto player = std::make_unique<Player>();
  player->pool = &pool;
  player->samples = &samples;
  engine::NullDevice dev;
  engine::StreamConfig sc;
  sc.bufferFrames = 64;
  sc.outputChannels = 2;
  sc.sampleRate = 48000;
  REQUIRE(dev.open(sc, {&Player::process, player.get()}).ok());
  // Stopped, the insert marker at each stream's start: the link prefetches, then Play adopts the streams.
  for (uint32_t i = 0; i < kStreams; ++i) (void)pool.prefetch(*samples[i], int64_t{i} * 4800, Direction::Forward);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  FILETIME idle0, kern0, user0;
  GetSystemTimes(&idle0, &kern0, &user0);
  pool.engage();
  REQUIRE(dev.start().ok());
  std::this_thread::sleep_for(std::chrono::seconds(120));
  REQUIRE(dev.stop().ok());
  pool.disengage();
  dev.close();
  const double load = systemBusyPercent(idle0, kern0, user0);
  const SamplePool::Stats st = pool.stats();
  MESSAGE(player->blocks.load() << " blocks of 64 frames, " << st.diskReads << " reads, "
                                << st.diskBytesRead / (1 << 20) << " MiB read, rings committed "
                                << st.ringBytesCommitted / (1 << 20) << " MiB, heads " << st.headBytes / (1 << 20)
                                << " MiB; system load " << load << " %; late blocks "
                                << (player->missing.load() ? std::to_string(player->firstMiss.load()) + ".." +
                                                                 std::to_string(player->lastMiss.load())
                                                           : std::string("none")));
  CHECK(player->blocks.load() > 80000);
  CHECK(player->missing.load() == 0);
  CHECK(st.underruns == 0);
  CHECK(player->wrong.load() == 0);
  for (auto& c : player->cur) c.stop();
  pool.stopDiskThread();
  samples.clear();
  std::error_code ec;
  fs::remove_all(dir, ec);
}
