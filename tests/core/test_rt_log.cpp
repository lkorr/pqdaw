// RtLog (T0.2-fix2): formatting, RT-clean push, exact drop accounting, SPSC under real threads, RtLogger lines.
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/rt_log.h"

using pq::RtLog;
using pq::RtLogger;
using pq::RtLogRecord;

namespace {
RtLogRecord drainOne(RtLog& log) {
  RtLogRecord out{};
  log.drain([&](const RtLogRecord& r) { out = r; });
  return out;
}
}  // namespace

TEST_CASE("rt log: records format with typed arguments") {
  RtLog log(16);
  log.log(7, "underrun at wake {} ({} frames late, {} ms, flag {})", uint64_t{42}, int32_t{-3}, 1.5, true);
  const RtLogRecord r = drainOne(log);
  CHECK(r.code == 7);
  CHECK(r.argc == 4);
  CHECK(r.tsc != 0);
  CHECK(pq::formatRtLogRecord(r) == "underrun at wake 42 (-3 frames late, 1.5 ms, flag true)");

  log.log(0, "braces {{}} and a missing {} {}", 1);
  CHECK(pq::formatRtLogRecord(drainOne(log)) == "braces {} and a missing 1 {?}");
  log.log(0, "no placeholders", 1, 2u);
  CHECK(pq::formatRtLogRecord(drainOne(log)) == "no placeholders 1 2");
  log.log(0, "float {}", 0.1f);
  CHECK(pq::formatRtLogRecord(drainOne(log)) == "float 0.100000001");
  static_assert(sizeof(RtLogRecord) <= 96, "RtLogRecord should stay small: it is copied through the ring");
}

TEST_CASE("rt log: push is RT-clean (allocation trap)") {
  RtLog log(1024);
  const uint64_t violations = pq::alloc_trap::countRtViolations([&] {
    for (int i = 0; i < 1000; ++i) log.log(1, "value {} {}", i, 0.5 * i);
    log.drain([](const RtLogRecord&) {});
  });
  CHECK(violations == 0);
  CHECK(log.pushed() == 1000);
  CHECK(log.dropped() == 0);
}

TEST_CASE("rt log: a full ring drops and counts exactly") {
  RtLog log(8);
  int accepted = 0;
  for (int i = 0; i < 20; ++i) accepted += log.log(0, "{}", i) ? 1 : 0;
  CHECK(accepted == 8);
  CHECK(log.pushed() == 20);
  CHECK(log.dropped() == 12);
  std::vector<uint64_t> seq;
  log.drain([&](const RtLogRecord& r) { seq.push_back(r.sequence); });
  CHECK(seq == std::vector<uint64_t>{0, 1, 2, 3, 4, 5, 6, 7});
  CHECK(log.log(0, "after"));
  CHECK(drainOne(log).sequence == 20);  // the gap 8..19 is the dropped records
}

TEST_CASE("rt log: RtLogger writes lines and reports drops once") {
  std::vector<std::string> lines;
  RtLog log(4);
  RtLogger logger([](void* user, const std::string& line) { static_cast<std::vector<std::string>*>(user)->push_back(line); },
                  &lines);
  logger.add(log, "audio");
  for (int i = 0; i < 6; ++i) log.log(0, "n={}", i);
  CHECK(logger.drainOnce() == 5);
  REQUIRE(lines.size() == 5);
  CHECK(lines[0] == "[audio #0] n=0");
  CHECK(lines[3] == "[audio #3] n=3");
  CHECK(lines[4] == "[audio] dropped 2 records (ring full; 2 in total)");
  CHECK(logger.drainOnce() == 0);  // nothing new, drops already reported
  log.log(0, "n={}", 6);
  CHECK(logger.drainOnce() == 1);
  CHECK(lines.back() == "[audio #6] n=6");
}

TEST_CASE("rt log: producer and consumer threads, 10^6 records, accounting adds up") {
  RtLog log(4096);
  constexpr uint64_t kCount = 1'000'000;
  std::atomic<bool> done{false};
  uint64_t received = 0, outOfOrder = 0, lastSeq = 0;
  bool first = true;
  std::thread consumer([&] {
    auto take = [&](const RtLogRecord& r) {
      if (!first && r.sequence <= lastSeq) ++outOfOrder;
      if (r.args[0].u != r.sequence) ++outOfOrder;  // payload matches the slot it was pushed into
      lastSeq = r.sequence;
      first = false;
      ++received;
    };
    while (!done.load(std::memory_order_acquire)) log.drain(take);
    log.drain(take);
  });
  for (uint64_t i = 0; i < kCount; ++i) log.log(0, "{}", i);
  done.store(true, std::memory_order_release);
  consumer.join();
  CHECK(outOfOrder == 0);
  CHECK(log.pushed() == kCount);
  CHECK(received + log.dropped() == kCount);
}
