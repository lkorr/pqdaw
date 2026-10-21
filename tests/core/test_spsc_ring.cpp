#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "pq/core/alloc_trap.h"
#include "pq/core/spsc_ring.h"
#include "queue_stress.h"

using pq::alloc_trap::countRtViolations;

TEST_CASE("spsc: FIFO, full and empty") {
  pq::SpscRing<int> r(4);
  CHECK(r.capacity() == 4);
  int v = -1;
  CHECK_FALSE(r.tryPop(v));
  CHECK(r.front() == nullptr);
  for (int i = 0; i < 4; ++i) CHECK(r.tryPush(i));
  CHECK_FALSE(r.tryPush(99));  // full: all capacity slots usable, the fifth fails
  CHECK(r.sizeApprox() == 4);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(r.tryPop(v));
    CHECK(v == i);
  }
  CHECK_FALSE(r.tryPop(v));
  CHECK(r.emptyApprox());
}

TEST_CASE("spsc: wraps around many times") {
  pq::SpscRing<uint32_t> r(8);
  uint32_t next = 0;
  uint32_t expect = 0;
  for (int round = 0; round < 1000; ++round) {
    const int n = 1 + round % 8;
    for (int i = 0; i < n; ++i) REQUIRE(r.tryPush(next++));
    for (int i = 0; i < n; ++i) {
      uint32_t v = 0;
      REQUIRE(r.tryPop(v));
      REQUIRE(v == expect++);
    }
  }
}

TEST_CASE("spsc: front/popFront peek without copying") {
  pq::SpscRing<int> r(2);
  REQUIRE(r.tryPush(10));
  REQUIRE(r.tryPush(11));
  REQUIRE(r.front() != nullptr);
  CHECK(*r.front() == 10);
  r.popFront();
  CHECK(*r.front() == 11);
  r.popFront();
  CHECK(r.front() == nullptr);
}

TEST_CASE("spsc: move-only payloads") {
  pq::SpscRing<std::unique_ptr<int>> r(2);
  REQUIRE(r.tryPush(std::make_unique<int>(3)));
  std::unique_ptr<int> out;
  REQUIRE(r.tryPop(out));
  CHECK(*out == 3);
}

TEST_CASE("spsc: tryPush/tryPop/front/popFront do not allocate (RtScope)") {
  pq::SpscRing<uint64_t> r(16);
  uint64_t v = 0;
  bool ok = true;
  CHECK(countRtViolations([&] {
          for (uint64_t i = 0; i < 64; ++i) {
            ok &= r.tryPush(i);
            ok &= r.tryPop(v);
            ok &= r.tryPush(i);
            ok &= r.front() != nullptr;
            r.popFront();
          }
          ok &= !r.tryPop(v);
          ok &= r.sizeApprox() == 0;
        }) == 0);
  CHECK(ok);
}

TEST_CASE("spsc stress: 10^7 items across two threads, no loss, duplication or reordering") {
  constexpr uint64_t kItems = 10'000'000;
  pq::SpscRing<uint64_t> r(1024);  // small ring: forces many full/empty transitions
  std::atomic<bool> go{false};

  std::thread producer([&] {
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    for (uint64_t i = 0; i < kItems;) {
      if (r.tryPush(i)) ++i;
      else std::this_thread::yield();
    }
  });

  uint64_t expected = 0;
  uint64_t errors = 0;
  uint64_t violations = 0;
  std::thread consumer([&] {
    pq::alloc_trap::ExpectViolations expect;
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    while (expected < kItems) {
      uint64_t v = 0;
      bool got;
      {
        pq::RtScope rt;  // the consumer side is the audio thread in practice
        got = r.tryPop(v);
      }
      if (!got) {
        std::this_thread::yield();
        continue;
      }
      if (v != expected) ++errors;  // a lost, duplicated or reordered item breaks the sequence
      expected = v + 1;
    }
    violations = expect.count();
  });

  go.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  CHECK(errors == 0);
  CHECK(expected == kItems);
  CHECK(violations == 0);
  uint64_t extra = 0;
  CHECK_FALSE(r.tryPop(extra));
}

// T0.2-fix1 (review L5): 64-byte payload with the sequence number in every word, checked on pop. Catches
// torn slots and payload/index reordering that a one-word payload can't show. Small rings force a
// full/empty transition every few items. The mutation check (tests/core/queue_mutation_main.cpp) runs
// these drivers against a tryPop whose acquire is weakened to relaxed.
TEST_CASE("spsc stress: 64-byte payloads, 4*10^6 items, tryPop, no torn or stale slots") {
  pq::SpscRing<pq::test::WidePayload> r(16);
  const auto res = pq::test::spscWideStress(r, 4'000'000, false);
  CHECK(res.received == 4'000'000);
  CHECK(res.torn == 0);
  CHECK(res.seqErrors == 0);
}

TEST_CASE("spsc stress: 64-byte payloads, 4*10^6 items, tryPop alternating with front/popFront") {
  pq::SpscRing<pq::test::WidePayload> r(16);
  const auto res = pq::test::spscWideStress(r, 4'000'000, true);
  CHECK(res.received == 4'000'000);
  CHECK(res.torn == 0);
  CHECK(res.seqErrors == 0);
}

TEST_CASE("spsc stress: 64-byte payloads with the 64-byte index layout (benchmark comparison variant)") {
  pq::SpscRing<pq::test::WidePayload, 64> r(16);
  const auto res = pq::test::spscWideStress(r, 1'000'000, true);
  CHECK(res.received == 1'000'000);
  CHECK(res.torn == 0);
  CHECK(res.seqErrors == 0);
}

TEST_CASE("spsc: hot index blocks are kHotLineSeparation apart") {
  static_assert(alignof(pq::SpscRing<uint64_t>) == pq::kHotLineSeparation);
  static_assert(sizeof(pq::SpscRing<uint64_t>) >= 3 * pq::kHotLineSeparation);
  static_assert(alignof(pq::SpscRing<uint64_t, 64>) == 64);
  pq::SpscRing<uint64_t> r(2);
  CHECK(reinterpret_cast<uintptr_t>(&r) % pq::kHotLineSeparation == 0);
}
