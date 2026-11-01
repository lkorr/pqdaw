// TMM.2: voice taps (pq/core/voice_tap.h) — interest, stride sampling independent of the call split, records and
// their lane masks, lossy reads, the reader's voices / staleness / history, RT safety, and delivery to a 60 Hz reader
// at 48 kHz / 64-frame blocks (≥ 95 % of the blocks arrive).

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/voice_tap.h"

using pq::VoiceTap;
using pq::VoiceTapConfig;
using pq::VoiceTapReader;
using pq::VoiceTapRecord;

namespace {

VoiceTapConfig config(uint32_t groups, uint32_t depth, uint32_t stride) {
  VoiceTapConfig c;
  c.groups = groups;
  c.depth = depth;
  c.stride = stride;
  return c;
}

}  // namespace

TEST_CASE("voice tap: nothing is written without a reader; records sample every stride frames, whatever the split") {
  VoiceTap a(config(2, 16, 8)), b(config(2, 16, 8));
  std::vector<float> x(512 * 8);
  for (size_t i = 0; i < x.size(); ++i) x[i] = 0.001f * static_cast<float>(i);
  a.write(1, 8, x.data(), 8, 64, false, nullptr, nullptr);
  CHECK_FALSE(a.wanted());
  CHECK(a.written(1) == 0);
  a.addReader();
  b.addReader();
  a.write(1, 8, x.data(), 8, 512, false, nullptr, nullptr);  // one call
  std::mt19937 rng(3);
  for (uint32_t f = 0; f < 512;) {                          // random splits
    const uint32_t n = std::min<uint32_t>(512 - f, 1 + rng() % 30);
    b.write(1, 8, x.data() + f * 8, 8, n, false, nullptr, nullptr);
    f += n;
  }
  REQUIRE(a.written(1) == 8);  // 512 frames / (8 samples · stride 8)
  REQUIRE(b.written(1) == 8);
  for (uint64_t i = 0; i < 8; ++i) {
    VoiceTapRecord ra, rb;
    REQUIRE(a.read(1, i, ra));
    REQUIRE(b.read(1, i, rb));
    CHECK(ra.serial == i + 1);
    CHECK(ra.group == 1);
    CHECK(ra.stride == 8);
    CHECK(ra.laneMask == 0xFFu);  // no image host set it
    bool same = true;
    for (uint32_t s = 0; s < 8; ++s)
      for (uint32_t l = 0; l < 8; ++l) {
        same = same && ra.sample[s][l] == x[(i * 64 + s * 8) * 8 + l];
        same = same && ra.sample[s][l] == rb.sample[s][l];
      }
    CHECK(same);
  }
  CHECK(a.written(0) == 0);
  a.write(7, 8, x.data(), 8, 64, false, nullptr, nullptr);  // a group the tap does not have: ignored
}

TEST_CASE("voice tap: lane masks, wrapping, position / increment, mono records, a pattern input") {
  VoiceTap t(config(1, 4, 1));
  t.addReader();
  t.setLaneMask(0, 0x105u);  // only the low 8 bits are lanes
  std::vector<float> x(8 * 8);
  for (size_t i = 0; i < x.size(); ++i) x[i] = -1.25f + 0.5f * static_cast<float>(i);
  const float pos[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f}, inc[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  t.write(0, 8, x.data(), 8, 8, true, pos, inc);
  VoiceTapRecord r;
  REQUIRE(t.read(0, 0, r));
  CHECK(r.laneMask == 0x05u);
  bool ok = true;
  for (uint32_t s = 0; s < 8; ++s)
    for (uint32_t l = 0; l < 8; ++l) {
      const float v = x[s * 8 + l];
      ok = ok && r.sample[s][l] == v - std::floor(v) && r.sample[s][l] >= 0.0f && r.sample[s][l] < 1.0f;
    }
  for (uint32_t l = 0; l < 8; ++l) ok = ok && r.position[l] == pos[l] && r.increment[l] == inc[l];
  CHECK(ok);
  // Mono: lane 0 only, mask 1; a pattern (stride 0) repeats its value.
  const float pat[8] = {0.75f, 9, 9, 9, 9, 9, 9, 9};
  t.write(0, 1, pat, 0, 8, false, pos, inc);
  REQUIRE(t.read(0, 1, r));
  CHECK(r.lanes == 1);
  CHECK(r.laneMask == 1u);
  for (uint32_t s = 0; s < 8; ++s) {
    CHECK(r.sample[s][0] == 0.75f);
    CHECK(r.sample[s][1] == 0.0f);
  }
  CHECK(r.position[0] == 0.1f);
  CHECK(r.position[1] == 0.0f);
}

TEST_CASE("voice tap: a reader that falls behind loses the overwritten records and says so") {
  VoiceTap t(config(1, 8, 1));
  auto shared = std::shared_ptr<VoiceTap>(&t, [](VoiceTap*) {});
  VoiceTapReader rd(shared, 16);
  CHECK(t.wanted());
  std::vector<float> x(8 * 8, 0.5f);
  for (uint32_t i = 0; i < 20; ++i) t.write(0, 8, x.data(), 8, 8, false, nullptr, nullptr);
  CHECK(rd.poll(0.0) == 7);  // depth 8: the newest 7 are safe to read
  CHECK(rd.lost() == 13);
  CHECK(rd.received() == 7);
  VoiceTapRecord r;
  CHECK_FALSE(t.read(0, 0, r));    // overwritten
  CHECK_FALSE(t.read(0, 20, r));   // not written yet
  CHECK(t.read(0, 19, r));
}

TEST_CASE("voice tap reader: voices from the lane masks, staleness, history newest last") {
  auto tap = std::make_shared<VoiceTap>(config(2, 32, 2));
  {
    VoiceTapReader rd(tap, 20, 0.1);
    CHECK(rd.voiceSlots() == 16);
    tap->setLaneMask(1, 0b0000'0110u);
    std::vector<float> x(16 * 8);
    for (uint32_t f = 0; f < 16; ++f)
      for (uint32_t l = 0; l < 8; ++l) x[f * 8 + l] = 0.01f * static_cast<float>(f) + 0.1f * static_cast<float>(l);
    const float pos[8] = {0, 0.5f, 0.25f, 0, 0, 0, 0, 0}, inc[8] = {0, 0.01f, 0.02f, 0, 0, 0, 0, 0};
    for (uint32_t k = 0; k < 3; ++k) tap->write(1, 8, x.data(), 8, 16, true, pos, inc);
    CHECK(rd.poll(1.0) == 3);
    for (uint32_t s = 0; s < 16; ++s) CHECK(rd.voice(s).active == (s == 9 || s == 10));
    CHECK(rd.voice(9).value == x[14 * 8 + 1]);  // the last sample: frame 14 (stride 2)
    CHECK(rd.voice(9).position == 0.5f);
    CHECK(rd.voice(10).increment == 0.02f);
    CHECK(rd.voice(9).serial == 3);
    std::vector<float> h(40);
    const uint32_t n = rd.history(9, h);
    CHECK(n == 20);  // 3 records · 8 samples, capped at 20
    bool ordered = true;
    for (uint32_t k = 0; k < n; ++k) ordered = ordered && h[k] == x[(((24 - n + k) % 8) * 2) * 8 + 1];
    CHECK(ordered);
    CHECK(rd.history(8, h) == 0);  // a free lane keeps no history
    CHECK(rd.poll(1.05) == 0);
    CHECK(rd.voice(9).active);       // within staleAfter
    CHECK(rd.poll(1.2) == 0);
    CHECK_FALSE(rd.voice(9).active);  // the group stopped writing: its voices ended
    CHECK(rd.history(9, h) == 0);
    CHECK(tap->wanted());
  }
  CHECK_FALSE(tap->wanted());  // the reader left
}

TEST_CASE("voice tap: write is allocation-free") {
  REQUIRE(pq::alloc_trap::installed());
  VoiceTap t(config(8, 64, 8));
  t.addReader();
  std::vector<float> x(1024 * 8, 0.25f);
  const float pos[8] = {}, inc[8] = {};
  const uint64_t v = pq::alloc_trap::countRtViolations([&] {
    for (uint32_t g = 0; g < 8; ++g) {
      t.setLaneMask(g, 0x0Fu);
      for (const uint32_t n : {1u, 7u, 64u, 1024u}) t.write(g, 8, x.data(), 8, n, true, pos, inc);
      t.write(g, 1, x.data(), 0, 64, false, nullptr, nullptr);
    }
  });
  CHECK(v == 0);
  CHECK(t.written(3) > 0);
}

TEST_CASE("voice tap: at 48 kHz / 64 frames, a 60 Hz reader receives ≥ 95 % of the blocks, intact") {
  // The writer is paced like an audio callback (one 64-frame block per 1.333 ms, 8 groups written per block); the
  // reader polls every 16.7 ms. Records carry their serial in every sample, so a torn copy would show.
  auto tap = std::make_shared<VoiceTap>(config(8, 64, 8));
  VoiceTapReader rd(tap, 128);
  std::atomic<bool> done{false};
  constexpr uint32_t kBlocks = 1500;  // 2 s
  std::thread writer([&] {
    std::vector<float> x(64 * 8);
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t b = 0; b < kBlocks; ++b) {
      std::this_thread::sleep_until(t0 + std::chrono::microseconds(1333) * b);
      for (uint32_t g = 0; g < 8; ++g) {
        std::fill(x.begin(), x.end(), static_cast<float>(b + 1) * 1e-4f);  // 1-based serial, < 1
        tap->setLaneMask(g, 0xFFu);
        tap->write(g, 8, x.data(), 8, 64, false, nullptr, nullptr);
      }
    }
    done.store(true);
  });
  const auto t0 = std::chrono::steady_clock::now();
  bool intact = true;
  std::vector<float> h(128);
  while (!done.load()) {
    std::this_thread::sleep_for(std::chrono::microseconds(16667));
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    rd.poll(now);
    for (uint32_t s = 0; s < rd.voiceSlots(); ++s) {
      const uint32_t n = rd.history(s, h);
      for (uint32_t k = 0; k + 8 <= n; k += 8)  // each record's 8 samples are one block's
        for (uint32_t j = 1; j < 8; ++j) intact = intact && h[k + j] == h[k];
    }
  }
  writer.join();
  rd.poll(1e9);
  const double share = static_cast<double>(rd.received()) / (kBlocks * 8.0);
  MESSAGE("voice tap delivery at 48 kHz / 64 frames, 60 Hz polls: " << rd.received() << " of " << kBlocks * 8
                                                                      << " records (" << share * 100.0 << " %), lost "
                                                                      << rd.lost());
  CHECK(share >= 0.95);
  CHECK(intact);
}
