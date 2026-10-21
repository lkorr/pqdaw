#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/mpsc_queue.h"
#include "queue_stress.h"

using pq::alloc_trap::countRtViolations;

TEST_CASE("mpsc: FIFO, full and empty (single thread)") {
  pq::MpscQueue<int> q(4);
  CHECK(q.capacity() == 4);
  int v = -1;
  CHECK_FALSE(q.tryPop(v));
  for (int i = 0; i < 4; ++i) CHECK(q.tryPush(i));
  CHECK_FALSE(q.tryPush(99));
  for (int i = 0; i < 4; ++i) {
    REQUIRE(q.tryPop(v));
    CHECK(v == i);
  }
  CHECK_FALSE(q.tryPop(v));
  // Wrap-around over many laps.
  int next = 0, expect = 0;
  for (int round = 0; round < 1000; ++round) {
    const int n = 1 + round % 4;
    for (int i = 0; i < n; ++i) REQUIRE(q.tryPush(next++));
    for (int i = 0; i < n; ++i) {
      REQUIRE(q.tryPop(v));
      REQUIRE(v == expect++);
    }
  }
}

TEST_CASE("mpsc: move-only payloads") {
  pq::MpscQueue<std::unique_ptr<int>> q(2);
  REQUIRE(q.tryPush(std::make_unique<int>(4)));
  std::unique_ptr<int> out;
  REQUIRE(q.tryPop(out));
  CHECK(*out == 4);
}

TEST_CASE("mpsc: tryPush/tryPop do not allocate (RtScope)") {
  pq::MpscQueue<uint64_t> q(16);
  uint64_t v = 0;
  bool ok = true;
  CHECK(countRtViolations([&] {
          for (uint64_t i = 0; i < 64; ++i) {
            ok &= q.tryPush(i);
            ok &= q.tryPop(v);
            ok &= v == i;
          }
          ok &= !q.tryPop(v);
        }) == 0);
  CHECK(ok);
}

TEST_CASE("mpsc stress: 4 producers x 2.5*10^6 items, no loss, duplication or per-producer reordering") {
  constexpr uint32_t kProducers = 4;
  constexpr uint64_t kPerProducer = 2'500'000;  // 10^7 items in total
  struct Item {
    uint32_t producer = 0;
    uint64_t seq = 0;
  };
  pq::MpscQueue<Item> q(1024);
  std::atomic<bool> go{false};

  std::vector<std::thread> producers;
  for (uint32_t p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (uint64_t i = 0; i < kPerProducer;) {
        if (q.tryPush(Item{p, i})) ++i;
        else std::this_thread::yield();
      }
    });
  }

  std::vector<uint64_t> nextSeq(kProducers, 0);
  uint64_t received = 0;
  uint64_t errors = 0;
  uint64_t violations = 0;
  std::thread consumer([&] {
    pq::alloc_trap::ExpectViolations expect;
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    while (received < kProducers * kPerProducer) {
      Item it;
      bool got;
      {
        pq::RtScope rt;
        got = q.tryPop(it);
      }
      if (!got) {
        std::this_thread::yield();
        continue;
      }
      ++received;
      if (it.producer >= kProducers || it.seq != nextSeq[it.producer]) ++errors;
      else ++nextSeq[it.producer];
    }
    violations = expect.count();
  });

  go.store(true, std::memory_order_release);
  for (auto& t : producers) t.join();
  consumer.join();

  CHECK(errors == 0);
  CHECK(received == kProducers * kPerProducer);
  for (uint32_t p = 0; p < kProducers; ++p) CHECK(nextSeq[p] == kPerProducer);
  CHECK(violations == 0);
  Item extra;
  CHECK_FALSE(q.tryPop(extra));
}

// T0.2-fix1 (review L5): 64-byte payloads (producer, sequence and word index in every word) through a
// small queue, checked on pop for torn slots and per-producer order.
TEST_CASE("mpsc stress: 64-byte payloads, 4 producers x 10^6 items, no torn or stale slots") {
  pq::MpscQueue<pq::test::WidePayload> q(16);
  const auto res = pq::test::mpscWideStress(q, 4, 1'000'000);
  CHECK(res.received == 4'000'000);
  CHECK(res.torn == 0);
  CHECK(res.seqErrors == 0);
}

TEST_CASE("mpsc stress: 64-byte payloads with the 64-byte index layout (benchmark comparison variant)") {
  pq::MpscQueue<pq::test::WidePayload, 64> q(16);
  const auto res = pq::test::mpscWideStress(q, 2, 500'000);
  CHECK(res.received == 1'000'000);
  CHECK(res.torn == 0);
  CHECK(res.seqErrors == 0);
}

TEST_CASE("mpsc: hot index lines are kHotLineSeparation apart") {
  static_assert(alignof(pq::MpscQueue<uint64_t>) == pq::kHotLineSeparation);
  static_assert(sizeof(pq::MpscQueue<uint64_t>) >= 3 * pq::kHotLineSeparation);
  static_assert(alignof(pq::MpscQueue<uint64_t, 64>) == 64);
  pq::MpscQueue<uint64_t> q(2);
  CHECK(reinterpret_cast<uintptr_t>(&q) % pq::kHotLineSeparation == 0);
}
