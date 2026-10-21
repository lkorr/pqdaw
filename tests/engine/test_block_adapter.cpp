#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/engine/block_adapter.h"

using pq::engine::AudioBlock;
using pq::engine::BlockAdapter;

namespace {

// Engine stand-in: out[c] = in[c] + 1000·c, and checks the adapter's contract (fixed frame count, contiguous
// frameTime). Allocation-free, so it can run under RtScope.
struct CheckingEngine {
  uint32_t expectFrames = 0;
  uint64_t calls = 0;
  uint64_t wrongFrames = 0;
  uint64_t wrongTime = 0;
  int64_t nextTime = 0;

  static void process(void* user, const AudioBlock& b) noexcept {
    auto& e = *static_cast<CheckingEngine*>(user);
    ++e.calls;
    if (b.frames != e.expectFrames) ++e.wrongFrames;
    if (b.frameTime != e.nextTime) ++e.wrongTime;
    e.nextTime = b.frameTime + b.frames;
    for (uint32_t c = 0; c < b.numOut; ++c)
      for (uint32_t i = 0; i < b.frames; ++i) b.out[c][i] = b.in[c][i] + 1000.0f * static_cast<float>(c);
  }
};

constexpr uint32_t kCh = 2;

// Feeds `total` frames (input = frame index + 1, per channel offset 0.5·c) through the adapter in device
// callbacks of the given sizes (cycled) and returns the device output, channel-major.
std::vector<std::vector<float>> drive(BlockAdapter& adapter, const std::vector<uint32_t>& sizes, uint32_t total) {
  std::vector<std::vector<float>> in(kCh, std::vector<float>(total)), out(kCh, std::vector<float>(total, -1.0f));
  for (uint32_t c = 0; c < kCh; ++c)
    for (uint32_t t = 0; t < total; ++t) in[c][t] = static_cast<float>(t + 1) + 0.5f * static_cast<float>(c);
  uint32_t t = 0;
  for (size_t k = 0; t < total; ++k) {
    const uint32_t n = std::min(sizes[k % sizes.size()], total - t);
    const float* ip[kCh];
    float* op[kCh];
    for (uint32_t c = 0; c < kCh; ++c) {
      ip[c] = in[c].data() + t;
      op[c] = out[c].data() + t;
    }
    adapter.process(AudioBlock{ip, kCh, op, kCh, n, t});
    t += n;
  }
  return out;
}

float expected(uint32_t c, uint32_t t, uint32_t latency) {
  if (t < latency) return 0.0f;
  return static_cast<float>(t - latency + 1) + 0.5f * static_cast<float>(c) + 1000.0f * static_cast<float>(c);
}

}  // namespace

TEST_CASE("block adapter: engine always sees the fixed block; output = input delayed by latencyFrames()") {
  const uint32_t deviceSizes[] = {1, 2, 31, 32, 33, 37, 64, 100, 256, 480, 512};
  const uint32_t blockSizes[] = {1, 16, 32, 48, 64};
  for (uint32_t d : deviceSizes) {
    for (uint32_t b : blockSizes) {
      CAPTURE(d);
      CAPTURE(b);
      CheckingEngine eng;
      eng.expectFrames = b;
      BlockAdapter adapter;
      REQUIRE(adapter.prepare({&CheckingEngine::process, &eng}, kCh, kCh, b, d));
      CHECK(adapter.directMode() == (d % b == 0));
      const uint32_t latency = adapter.latencyFrames();
      CHECK(latency == (d % b == 0 ? 0u : b));

      const uint32_t total = 40 * d > 6000 ? 40 * d : 6000;
      const auto out = drive(adapter, {d}, total - total % d);  // whole device buffers only
      const uint32_t n = total - total % d;
      uint64_t bad = 0;
      for (uint32_t c = 0; c < kCh; ++c)
        for (uint32_t t = 0; t < n; ++t)
          if (out[c][t] != expected(c, t, latency)) ++bad;
      CHECK(bad == 0);
      CHECK(eng.calls == n / b);
      CHECK(eng.wrongFrames == 0);
      CHECK(eng.wrongTime == 0);
      CHECK(adapter.engineFrames() == static_cast<int64_t>(eng.calls) * b);
      CHECK(adapter.modeSwitches() == 0);
    }
  }
}

TEST_CASE("block adapter: a direct-mode stream that delivers an odd buffer falls back to FIFO once") {
  CheckingEngine eng;
  eng.expectFrames = 32;
  BlockAdapter adapter;
  REQUIRE(adapter.prepare({&CheckingEngine::process, &eng}, kCh, kCh, 32, 64));
  REQUIRE(adapter.directMode());
  // 64, 64, then 37s: the first 128 frames are direct (no latency), then FIFO with 32 frames of latency.
  const uint32_t total = 128 + 37 * 40;
  const auto out = drive(adapter, {64, 64, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
                                   37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
                                   37, 37},
                         total);
  CHECK_FALSE(adapter.directMode());
  CHECK(adapter.modeSwitches() == 1);
  CHECK(adapter.latencyFrames() == 32);
  uint64_t bad = 0;
  for (uint32_t c = 0; c < kCh; ++c) {
    for (uint32_t t = 0; t < 128; ++t)
      if (out[c][t] != expected(c, t, 0)) ++bad;
    for (uint32_t t = 128; t < 160; ++t)  // one FIFO block of silence at the switch
      if (out[c][t] != 0.0f) ++bad;
    // After the gap the FIFO plays the input from the switch point on, 32 frames late.
    for (uint32_t t = 160; t < total; ++t)
      if (out[c][t] != expected(c, t, 32)) ++bad;
  }
  CHECK(bad == 0);
  CHECK(eng.wrongFrames == 0);
  CHECK(eng.wrongTime == 0);
}

TEST_CASE("block adapter: mixed device buffer sizes in FIFO mode keep exact latency") {
  CheckingEngine eng;
  eng.expectFrames = 32;
  BlockAdapter adapter;
  REQUIRE(adapter.prepare({&CheckingEngine::process, &eng}, kCh, kCh, 32, 37));
  REQUIRE_FALSE(adapter.directMode());
  const uint32_t total = 5000;
  const auto out = drive(adapter, {37, 1, 480, 5, 64, 31, 33, 2}, total);
  uint64_t bad = 0;
  for (uint32_t c = 0; c < kCh; ++c)
    for (uint32_t t = 0; t < total; ++t)
      if (out[c][t] != expected(c, t, 32)) ++bad;
  CHECK(bad == 0);
  CHECK(eng.calls == total / 32);
  CHECK(eng.wrongTime == 0);
}

TEST_CASE("block adapter: output-only and input-only streams") {
  struct Gen {
    uint64_t calls = 0;
    static void process(void* user, const AudioBlock& b) noexcept {
      auto& g = *static_cast<Gen*>(user);
      ++g.calls;
      for (uint32_t c = 0; c < b.numOut; ++c)
        for (uint32_t i = 0; i < b.frames; ++i) b.out[c][i] = static_cast<float>(b.frameTime + i);
    }
  } gen;
  BlockAdapter adapter;
  REQUIRE(adapter.prepare({&Gen::process, &gen}, 0, 1, 64, 37));
  std::vector<float> out(37 * 10);
  for (uint32_t k = 0; k < 10; ++k) {
    float* op[1] = {out.data() + 37 * k};
    adapter.process(AudioBlock{nullptr, 0, op, 1, 37, 37 * k});
  }
  for (uint32_t t = 0; t < out.size(); ++t) CHECK(out[t] == (t < 64 ? 0.0f : static_cast<float>(t - 64)));
  CHECK(gen.calls == 370 / 64);

  BlockAdapter inOnly;
  REQUIRE(inOnly.prepare({&Gen::process, &gen}, 2, 0, 32, 96));
  std::vector<float> in(96, 1.0f);
  const float* ip[2] = {in.data(), in.data()};
  inOnly.process(AudioBlock{ip, 2, nullptr, 0, 96, 0});
  CHECK(inOnly.engineFrames() == 96);
}

TEST_CASE("block adapter: process() does not allocate (direct and FIFO)") {
  CheckingEngine eng;
  std::vector<float> in(kCh * 480, 0.25f), out(kCh * 480);
  const float* ip[kCh] = {in.data(), in.data() + 480};
  float* op[kCh] = {out.data(), out.data() + 480};
  for (uint32_t d : {480u, 37u, 1u}) {
    CAPTURE(d);
    eng.expectFrames = 32;
    BlockAdapter adapter;
    REQUIRE(adapter.prepare({&CheckingEngine::process, &eng}, kCh, kCh, 32, d));
    CHECK(pq::alloc_trap::countRtViolations([&] {
            for (int k = 0; k < 50; ++k) adapter.process(AudioBlock{ip, kCh, op, kCh, d, 0});
          }) == 0);
  }
}

TEST_CASE("block adapter: prepare() rejects invalid arguments") {
  CheckingEngine eng;
  BlockAdapter a;
  CHECK_FALSE(a.prepare({nullptr, &eng}, 2, 2, 32, 256));
  CHECK_FALSE(a.prepare({&CheckingEngine::process, &eng}, 2, 2, 0, 256));
  CHECK_FALSE(a.prepare({&CheckingEngine::process, &eng}, 2, 2, BlockAdapter::kMaxBlockFrames + 1, 256));
  CHECK_FALSE(a.prepare({&CheckingEngine::process, &eng}, pq::engine::kMaxChannels + 1, 2, 32, 256));
  CHECK_FALSE(a.prepare({&CheckingEngine::process, &eng}, 2, 2, 32, 0));
  CHECK(a.prepare({&CheckingEngine::process, &eng}, 2, 2, BlockAdapter::kMaxBlockFrames, 256));
}
