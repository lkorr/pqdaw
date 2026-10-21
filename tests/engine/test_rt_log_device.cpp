// RtLog from the device callback path (T0.2-fix2 acceptance): 10^6 records pushed from NullDevice callbacks into a
// 4096-record ring drained at 100 Hz lose none; overfilling reports the drops exactly.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "pq/core/alloc_trap.h"
#include "pq/core/rt_log.h"
#include "pq/engine/null_device.h"

using namespace pq::engine;
using pq::RtLog;
using pq::RtLogger;
using pq::RtLogRecord;

namespace {

// Pushes `perCallback` records per device callback (until `limit` in total), from inside the runner's RtScope.
struct LoggingEngine {
  RtLog* log = nullptr;
  uint32_t perCallback = 0;
  uint64_t limit = 0;
  std::atomic<uint64_t> pushed{0};
  std::atomic<uint64_t> failed{0};
  std::atomic<uint64_t> notRt{0};

  static void process(void* user, const AudioBlock& b) noexcept {  // RT-safe
    auto& e = *static_cast<LoggingEngine*>(user);
    for (uint32_t c = 0; c < b.numOut; ++c)
      for (uint32_t i = 0; i < b.frames; ++i) b.out[c][i] = 0.0f;
    if (!pq::inRtScope()) e.notRt.fetch_add(1, std::memory_order_relaxed);
    uint64_t n = e.pushed.load(std::memory_order_relaxed);
    for (uint32_t k = 0; k < e.perCallback && n < e.limit; ++k, ++n)
      if (!e.log->log(1, "block at {} record {}", b.frameTime, n)) e.failed.fetch_add(1, std::memory_order_relaxed);
    e.pushed.store(n, std::memory_order_relaxed);
  }
};

struct CountingSink {
  uint64_t records = 0;
  uint64_t dropLines = 0;
  static void line(void* user, const std::string& l) {
    auto& s = *static_cast<CountingSink*>(user);
    if (l.find("dropped") != std::string::npos)
      ++s.dropLines;
    else
      ++s.records;
  }
};

}  // namespace

TEST_CASE("rt log device: 10^6 records from NullDevice callbacks, 4096 ring drained every 10 ms of audio, none lost") {
  // Manual device clock: 48 kHz, 48-frame callbacks = 1 ms each, so 10 callbacks are 10 ms; 400 records per
  // callback = 4000 per drain interval (97.7 % of the ring) and 2500 callbacks for 10^6 records.
  RtLog log(4096);
  CountingSink sink;
  RtLogger logger(&CountingSink::line, &sink);
  logger.add(log, "audio");
  LoggingEngine engine;
  engine.log = &log;
  engine.perCallback = 400;
  engine.limit = 1'000'000;
  NullDevice dev;
  StreamConfig c;
  c.bufferFrames = 48;
  c.outputChannels = 2;
  c.sampleRate = 48000;
  REQUIRE(dev.open(c, {&LoggingEngine::process, &engine}).ok());
  const uint64_t violationsBefore = pq::alloc_trap::violationCount();
  for (uint32_t cb = 1; engine.pushed.load() < engine.limit; ++cb) {
    dev.processBlock();
    if (cb % 10 == 0) logger.drainOnce();  // 100 Hz in device time
  }
  logger.drainOnce();
  CHECK(pq::alloc_trap::violationCount() == violationsBefore);  // pushes ran inside the runner's RtScope
  CHECK(engine.notRt.load() == 0);
  CHECK(engine.pushed.load() == 1'000'000);
  CHECK(engine.failed.load() == 0);
  CHECK(log.dropped() == 0);
  CHECK(sink.records == 1'000'000);
  CHECK(sink.dropLines == 0);

  // Overfill: one interval of 5000 records into the empty 4096 ring: exactly 904 dropped, reported once.
  engine.perCallback = 500;
  engine.limit += 5000;
  for (int cb = 0; cb < 10; ++cb) dev.processBlock();
  logger.drainOnce();
  CHECK(engine.failed.load() == 904);
  CHECK(log.dropped() == 904);
  CHECK(sink.records == 1'000'000 + 4096);
  CHECK(sink.dropLines == 1);
}

TEST_CASE("rt log device: threaded NullDevice and RtLogger thread at 100 Hz, 10^6 records, none lost") {
  // Real threads: the NullDevice timer thread runs 1000 callbacks/s (48 frames at 48 kHz) pushing 200 records each
  // (200 000/s, 2000 per 10 ms drain: half the ring, so a drain may be up to ~10 ms late without loss); the
  // logger drains every 10 ms on its own thread. ~5 s.
  RtLog log(4096);
  CountingSink sink;
  RtLogger logger(&CountingSink::line, &sink, 10);
  logger.add(log, "audio");
  LoggingEngine engine;
  engine.log = &log;
  engine.perCallback = 200;
  engine.limit = 1'000'000;
  NullDevice dev;
  StreamConfig c;
  c.bufferFrames = 48;
  c.outputChannels = 2;
  c.sampleRate = 48000;
  REQUIRE(dev.open(c, {&LoggingEngine::process, &engine}).ok());
  const uint64_t violationsBefore = pq::alloc_trap::violationCount();
  logger.start();
  REQUIRE(dev.start().ok());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (engine.pushed.load() < engine.limit && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(dev.stop().ok());
  logger.stop();
  dev.close();
  CHECK(pq::alloc_trap::violationCount() == violationsBefore);
  CHECK(engine.pushed.load() == 1'000'000);
  CHECK(log.dropped() == 0);
  CHECK(engine.failed.load() == 0);
  CHECK(sink.records == 1'000'000);
  CHECK(logger.drains() >= 400);  // ~100 Hz over ~5 s
}
