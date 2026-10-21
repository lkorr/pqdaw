#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <thread>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/null_device.h"

using namespace pq::engine;

namespace {

// Device-level callback that checks the environment the runner sets up and the frames/time it is given.
// Atomics because the threaded tests read them from the test thread.
struct Probe {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> wrongTime{0};
  std::atomic<uint64_t> notRt{0};
  std::atomic<uint64_t> noFtz{0};
  std::atomic<uint32_t> lastFrames{0};
  std::atomic<uint32_t> threadId{0};
  int64_t nextTime = 0;
  bool allocate = false;

  static void process(void* user, const AudioBlock& b) noexcept {
    auto& p = *static_cast<Probe*>(user);
    p.calls.fetch_add(1, std::memory_order_relaxed);
    p.frames.fetch_add(b.frames, std::memory_order_relaxed);
    p.lastFrames.store(b.frames, std::memory_order_relaxed);
    p.threadId.store(GetCurrentThreadIdForTest(), std::memory_order_relaxed);
    if (b.frameTime != p.nextTime) p.wrongTime.fetch_add(1, std::memory_order_relaxed);
    p.nextTime = b.frameTime + b.frames;
    if (!pq::inRtScope()) p.notRt.fetch_add(1, std::memory_order_relaxed);
    if (!pq::DenormalGuard::active()) p.noFtz.fetch_add(1, std::memory_order_relaxed);
    // out = 2·in (or a constant for output-only streams).
    for (uint32_t c = 0; c < b.numOut; ++c)
      for (uint32_t i = 0; i < b.frames; ++i) b.out[c][i] = c < b.numIn ? 2.0f * b.in[c][i] : 0.5f;
    if (p.allocate) {
      int* leak = new int(1);
      delete leak;
    }
  }

  static uint32_t GetCurrentThreadIdForTest() noexcept {
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  }
};

StreamConfig config(uint32_t bufferFrames, uint32_t in, uint32_t out, uint32_t rate = 48000) {
  StreamConfig c;
  c.bufferFrames = bufferFrames;
  c.inputChannels = in;
  c.outputChannels = out;
  c.sampleRate = rate;
  return c;
}

}  // namespace

TEST_CASE("null device: enumerate and open errors") {
  NullDevice dev;
  auto list = dev.enumerate();
  REQUIRE(list.ok());
  REQUIRE(list->size() == 1);
  CHECK((*list)[0].isDefaultOutput);
  CHECK((*list)[0].outputChannels == kMaxChannels);

  Probe probe;
  const AudioCallback cb{&Probe::process, &probe};
  CHECK(dev.open(config(0, 0, 2), cb).error().code == pq::ErrorCode::InvalidArgument);
  CHECK(dev.open(config(64, 0, kMaxChannels + 1), cb).error().code == pq::ErrorCode::InvalidArgument);
  CHECK(dev.open(config(64, 0, 2), AudioCallback{}).error().code == pq::ErrorCode::InvalidArgument);
  StreamConfig bad = config(64, 0, 2);
  bad.outputDevice = 5;
  CHECK(dev.open(bad, cb).error().code == pq::ErrorCode::NotFound);
  CHECK(dev.start().error().code == pq::ErrorCode::InvalidArgument);  // nothing open

  auto info = dev.open(config(64, 0, 2, 0), cb);
  REQUIRE(info.ok());
  CHECK(info->sampleRate == 48000);  // 0 = default rate
  CHECK(dev.open(config(64, 0, 2), cb).error().code == pq::ErrorCode::InvalidArgument);  // already open
  dev.close();
  CHECK_FALSE(dev.isOpen());
  CHECK(dev.open(config(64, 0, 2), cb).ok());  // reopen after close
}

TEST_CASE("null device: manual mode delivers exact buffer sizes, frame times and data") {
  for (uint32_t bufferFrames : {1u, 37u, 64u, 480u}) {
    CAPTURE(bufferFrames);
    NullDevice dev;
    Probe probe;
    auto info = dev.open(config(bufferFrames, 1, 2), {&Probe::process, &probe});
    REQUIRE(info.ok());
    CHECK(info->bufferFrames == bufferFrames);
    CHECK(info->inputChannels == 1);
    CHECK(info->outputChannels == 2);

    for (uint32_t k = 0; k < 10; ++k) {
      for (uint32_t i = 0; i < bufferFrames; ++i) dev.input(0)[i] = static_cast<float>(k * bufferFrames + i);
      dev.processBlock();
      CHECK(probe.lastFrames.load() == bufferFrames);
      bool ok = true;
      for (uint32_t i = 0; i < bufferFrames; ++i) {
        ok = ok && dev.output(0)[i] == 2.0f * static_cast<float>(k * bufferFrames + i);
        ok = ok && dev.output(1)[i] == 0.5f;
      }
      CHECK(ok);
    }
    CHECK(probe.calls.load() == 10);
    CHECK(probe.frames.load() == 10ull * bufferFrames);
    CHECK(probe.wrongTime.load() == 0);
    CHECK(probe.notRt.load() == 0);
    CHECK(probe.noFtz.load() == 0);
    // The runner restores the caller's MXCSR afterwards.
    CHECK_FALSE(pq::DenormalGuard::active());

    const auto st = dev.stats().snapshot();
    CHECK(st.callbacks == 10);
    CHECK(st.frames == 10ull * bufferFrames);
    CHECK(st.xruns() == 0);
    CHECK(st.periodUs == doctest::Approx(1e6 * bufferFrames / 48000.0));
    CHECK(st.meanDurationUs > 0.0);
    CHECK(st.maxDurationUs >= st.meanDurationUs);
  }
}

TEST_CASE("null device: shorter blocks are delivered as asked, longer ones clamped to the buffer") {
  NullDevice dev;
  Probe probe;
  REQUIRE(dev.open(config(37, 0, 2), {&Probe::process, &probe}).ok());
  dev.processBlock(5);
  CHECK(probe.lastFrames.load() == 5);
  dev.processBlock(1000);
  CHECK(probe.lastFrames.load() == 37);
  CHECK(probe.wrongTime.load() == 0);
  CHECK(dev.stats().snapshot().frames == 42);
}

TEST_CASE("null device: backend xrun flags are counted") {
  NullDevice dev;
  Probe probe;
  REQUIRE(dev.open(config(64, 0, 2), {&Probe::process, &probe}).ok());
  dev.processBlock(0, true, false);
  dev.processBlock(0, false, true);
  dev.processBlock(0, true, true);
  dev.processBlock();
  const auto st = dev.stats().snapshot();
  CHECK(st.inputOverflows == 2);
  CHECK(st.outputUnderflows == 2);
  CHECK(st.xruns() == 4);
  CHECK(st.callbacks == 4);
}

TEST_CASE("null device: a callback slower than the buffer period counts as a deadline miss") {
  struct Slow {
    static void process(void*, const AudioBlock& b) noexcept {
      const uint64_t until = readTsc() + static_cast<uint64_t>(tscFrequencyHz() * 0.003);  // 3 ms > 1 ms period
      while (readTsc() < until) {
      }
      for (uint32_t c = 0; c < b.numOut; ++c)
        for (uint32_t i = 0; i < b.frames; ++i) b.out[c][i] = 0.0f;
    }
  };
  (void)tscFrequencyHz();  // calibrate outside the callback
  NullDevice dev;
  REQUIRE(dev.open(config(48, 0, 1), {&Slow::process, nullptr}).ok());  // 1 ms at 48 kHz
  dev.processBlock();
  const auto st = dev.stats().snapshot();
  CHECK(st.deadlineMisses == 1);
  CHECK(st.maxDurationUs > 2500.0);
  CHECK(st.maxDurationUs < 100000.0);
}

TEST_CASE("null device: the callback path runs under RtScope (allocation trap)") {
  NullDevice dev;
  Probe probe;
  REQUIRE(dev.open(config(37, 2, 2), {&Probe::process, &probe}).ok());
  // Clean callback: no violations across the whole device → runner → engine path.
  CHECK(pq::alloc_trap::countRtViolations([&] {
          for (int k = 0; k < 20; ++k) dev.processBlock();
        }) == 0);
  // An allocating callback is caught, which proves the scope really covers the engine call. No outer RtScope
  // here: the runner opens its own.
  probe.allocate = true;
  pq::alloc_trap::ExpectViolations expect;
  dev.processBlock();
  CHECK(expect.count() == 2);  // the new and the delete
  probe.allocate = false;
}

TEST_CASE("null device + block adapter: odd device buffers reach the engine as fixed blocks, allocation-free") {
  struct Engine {
    uint64_t calls = 0;
    uint64_t wrongFrames = 0;
    static void process(void* user, const AudioBlock& b) noexcept {
      auto& e = *static_cast<Engine*>(user);
      ++e.calls;
      if (b.frames != 32) ++e.wrongFrames;
      for (uint32_t c = 0; c < b.numOut; ++c)
        for (uint32_t i = 0; i < b.frames; ++i) b.out[c][i] = b.in[c][i];
    }
  };
  for (uint32_t bufferFrames : {1u, 37u, 480u}) {
    CAPTURE(bufferFrames);
    Engine engine;
    BlockAdapter adapter;
    NullDevice dev;
    auto info = dev.open(config(bufferFrames, 2, 2), adapter.deviceCallback());
    REQUIRE(info.ok());
    REQUIRE(adapter.prepare({&Engine::process, &engine}, 2, 2, 32, info->bufferFrames));
    CHECK(adapter.directMode() == (bufferFrames == 480));
    const uint32_t callbacks = 9600 / bufferFrames;
    uint64_t bad = 0;
    const uint32_t latency = adapter.latencyFrames();
    CHECK(pq::alloc_trap::countRtViolations([&] {
            for (uint32_t k = 0; k < callbacks; ++k) {
              for (uint32_t c = 0; c < 2; ++c)
                for (uint32_t i = 0; i < bufferFrames; ++i)
                  dev.input(c)[i] = static_cast<float>(k * bufferFrames + i + 1);
              dev.processBlock();
              for (uint32_t c = 0; c < 2; ++c)
                for (uint32_t i = 0; i < bufferFrames; ++i) {
                  const uint32_t t = k * bufferFrames + i;
                  const float want = t < latency ? 0.0f : static_cast<float>(t - latency + 1);
                  if (dev.output(c)[i] != want) ++bad;
                }
            }
          }) == 0);
    CHECK(bad == 0);
    CHECK(engine.calls == callbacks * bufferFrames / 32);
    CHECK(engine.wrongFrames == 0);
  }
}

TEST_CASE("null device: threaded mode keeps the buffer cadence") {
  // Generous tolerance: the machine may be shared and the timer is not realtime. Absolute deadlines mean the
  // count tracks elapsed time even when single wake-ups are late.
  for (uint32_t bufferFrames : {64u, 480u}) {
    CAPTURE(bufferFrames);
    NullDevice dev;
    Probe probe;
    REQUIRE(dev.open(config(bufferFrames, 0, 2), {&Probe::process, &probe}).ok());
    const uint64_t violationsBefore = pq::alloc_trap::violationCount();
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(dev.start().ok());
    CHECK(dev.isRunning());
    CHECK(dev.start().error().code == pq::ErrorCode::InvalidArgument);  // already running
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    REQUIRE(dev.stop().ok());
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK_FALSE(dev.isRunning());

    const double period = bufferFrames / 48000.0;
    const double expected = elapsed / period;
    const auto calls = static_cast<double>(probe.calls.load());
    CAPTURE(expected);
    CAPTURE(calls);
    CHECK(calls >= expected * 0.8 - 3);
    CHECK(calls <= expected * 1.05 + 3);  // never ahead of real time (plus the first, immediate callback)
    CHECK(probe.frames.load() == probe.calls.load() * bufferFrames);
    CHECK(probe.wrongTime.load() == 0);
    CHECK(probe.notRt.load() == 0);
    CHECK(probe.noFtz.load() == 0);
    CHECK(probe.threadId.load() != Probe::GetCurrentThreadIdForTest());
    CHECK(pq::alloc_trap::violationCount() == violationsBefore);
    CHECK(dev.stats().snapshot().callbacks == probe.calls.load());

    // Restart works and continues the stream's frame time.
    const uint64_t before = probe.calls.load();
    REQUIRE(dev.start().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(dev.stop().ok());
    CHECK(probe.calls.load() > before);
    CHECK(probe.wrongTime.load() == 0);
  }
}
