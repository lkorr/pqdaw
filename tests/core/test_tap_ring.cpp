// TapRing (pq/core/tap_ring.h, T4.6): the analysis tap the audio thread writes into.

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/tap_ring.h"

using pq::TapRing;
using pq::alloc_trap::countRtViolations;

TEST_CASE("tap ring: without a reader the writer does nothing") {
  TapRing tap(2, 1024, 256);
  std::vector<float> l(100, 1.0f), r(100, 2.0f);
  const float* ch[2] = {l.data(), r.data()};
  tap.write(ch, 2, 100);
  CHECK(tap.written() == 0);
  tap.addReader();
  tap.write(ch, 2, 100);
  CHECK(tap.written() == 100);
  tap.removeReader();
  tap.write(ch, 2, 100);
  CHECK(tap.written() == 100);
}

TEST_CASE("tap ring: frames come back in order across the wrap, channels planar, a mono source fills both") {
  TapRing tap(2, 64, 16);
  tap.addReader();
  std::vector<float> a(40), b(40);
  for (int i = 0; i < 40; ++i) {
    a[i] = static_cast<float>(i);
    b[i] = static_cast<float>(-i);
  }
  const float* ch[2] = {a.data(), b.data()};
  tap.write(ch, 2, 40);  // split into chunks of ≤ 16
  for (int i = 0; i < 40; ++i) {
    a[i] = static_cast<float>(40 + i);
    b[i] = static_cast<float>(-40 - i);
  }
  tap.write(ch, 2, 40);  // wraps (80 > 64)
  CHECK(tap.written() == 80);
  CHECK(tap.oldestReadable() == 80 - (64 - 16));
  std::vector<float> out(30);
  REQUIRE(tap.read(0, 50, 30, out.data()) == 30);
  for (int i = 0; i < 30; ++i) CHECK(out[i] == static_cast<float>(50 + i));
  REQUIRE(tap.read(1, 50, 30, out.data()) == 30);
  for (int i = 0; i < 30; ++i) CHECK(out[i] == static_cast<float>(-50 - i));
  // Overwritten frames and frames not written yet.
  CHECK(tap.read(0, 10, 10, out.data()) == 0);
  CHECK(tap.read(0, 75, 10, out.data()) == 5);
  CHECK(tap.read(0, 80, 10, out.data()) == 0);
  // Mono source into a stereo tap.
  const float* mono[1] = {a.data()};
  tap.write(mono, 1, 4);
  REQUIRE(tap.read(1, 80, 4, out.data()) == 4);
  CHECK(out[0] == 40.0f);
  CHECK(out[3] == 43.0f);
}

TEST_CASE("tap ring: the write is RT-safe (RtScope)") {
  TapRing tap(2, 4096, 1024);
  std::vector<float> l(3000, 0.5f), r(3000, -0.5f);
  const float* ch[2] = {l.data(), r.data()};
  CHECK(countRtViolations([&] { tap.write(ch, 2, 3000); }) == 0);  // no reader: one load
  tap.addReader();
  CHECK(countRtViolations([&] {
          for (int i = 0; i < 10; ++i) tap.write(ch, 2, 3000);
        }) == 0);
  CHECK(tap.written() == 30000);
}

TEST_CASE("tap ring: a concurrent reader never accepts a torn frame") {
  // The writer writes blocks whose every sample is the block's sequence number; a reader that accepts a window must
  // see consecutive blocks only (each value v followed by v or v + 1).
  constexpr uint32_t kBlock = 64;
  TapRing tap(1, 1024, kBlock);
  tap.addReader();
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::vector<float> blk(kBlock);
    for (uint32_t s = 0; s < 200000 && !stop.load(); ++s) {
      std::fill(blk.begin(), blk.end(), static_cast<float>(s));
      const float* ch[1] = {blk.data()};
      tap.write(ch, 1, kBlock);
    }
    stop = true;
  });
  std::vector<float> out(256);
  uint64_t accepted = 0;
  bool bad = false;
  while (!stop.load()) {
    const uint64_t from = tap.oldestReadable();
    const uint32_t got = tap.read(0, from, 256, out.data());
    if (got == 0) continue;
    ++accepted;
    for (uint32_t i = 1; i < got; ++i)
      if (!(out[i] == out[i - 1] || out[i] == out[i - 1] + 1.0f)) bad = true;
    // Frame f holds block f / kBlock.
    if (out[0] != static_cast<float>(from / kBlock)) bad = true;
  }
  writer.join();
  CHECK_FALSE(bad);
  CHECK(accepted > 0);
}
