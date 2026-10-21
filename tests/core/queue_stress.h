#pragma once

// Cross-thread stress drivers for SpscRing / MpscQueue with a 64-byte payload (T0.2-fix1, review L5).
//
// Every one of the eight words carries the item's sequence number (and producer and word index), and the
// consumer checks all eight on pop. A slot read before the producer finished writing it (index/payload
// reordering by the compiler or the CPU), or a slot overwritten while being read, shows up as words from
// two different items ("torn") or as an item from the previous lap (a sequence error). A single-word
// payload can't show either: one aligned 8-byte load is atomic on x86.
//
// Shared by tests/core/test_spsc_ring.cpp, test_mpsc_queue.cpp and the mutation-check executable
// (tests/core/queue_mutation_main.cpp), which compiles these same drivers against mutated queue headers.

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <immintrin.h>

namespace pq::test {

struct alignas(64) WidePayload {
  uint64_t w[8] = {};
};
static_assert(sizeof(WidePayload) == 64);

inline WidePayload makeWide(uint32_t producer, uint64_t seq) noexcept {
  WidePayload p;
  for (uint64_t k = 0; k < 8; ++k) p.w[k] = (uint64_t{producer} << 56) | (seq << 3) | k;
  return p;
}

// Decodes word 0 and checks the other seven agree with it. False = torn slot.
inline bool decodeWide(const WidePayload& p, uint32_t& producer, uint64_t& seq) noexcept {
  producer = static_cast<uint32_t>(p.w[0] >> 56);
  seq = (p.w[0] >> 3) & ((uint64_t{1} << 53) - 1);
  bool ok = (p.w[0] & 7) == 0;
  for (uint64_t k = 1; k < 8; ++k) ok &= p.w[k] == ((p.w[0] & ~uint64_t{7}) | k);
  return ok;
}

struct StressResult {
  uint64_t received = 0;
  uint64_t torn = 0;       // words of one slot disagree
  uint64_t seqErrors = 0;  // lost, duplicated, reordered, or stale (previous-lap) items
};

// Short busy-wait, then yield: tight enough to provoke races, polite enough for a shared machine.
struct Backoff {
  uint32_t spins = 0;
  void pause() noexcept {
    if (++spins < 64) _mm_pause();
    else {
      spins = 0;
      std::this_thread::yield();
    }
  }
  void reset() noexcept { spins = 0; }
};

// One producer pushes items 0..items-1; one consumer pops them. useFront: the consumer alternates
// tryPop and front()/popFront(); otherwise only tryPop.
template <class Ring>
StressResult spscWideStress(Ring& ring, uint64_t items, bool useFront) {
  std::atomic<bool> go{false};
  std::thread producer([&] {
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    Backoff b;
    for (uint64_t i = 0; i < items;) {
      if (ring.tryPush(makeWide(0, i))) {
        ++i;
        b.reset();
      } else {
        b.pause();
      }
    }
  });
  StressResult r;
  std::thread consumer([&] {
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    Backoff b;
    uint64_t expected = 0;
    while (r.received < items) {
      WidePayload v;
      bool got;
      if (useFront && (r.received & 1)) {
        const WidePayload* f = ring.front();
        got = f != nullptr;
        if (got) {
          v = *f;
          ring.popFront();
        }
      } else {
        got = ring.tryPop(v);
      }
      if (!got) {
        b.pause();
        continue;
      }
      b.reset();
      ++r.received;
      uint32_t p = 0;
      uint64_t seq = 0;
      if (!decodeWide(v, p, seq)) ++r.torn;
      else if (p != 0 || seq != expected) ++r.seqErrors;
      expected = seq + 1;
    }
  });
  go.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  return r;
}

// `producers` threads each push perProducer items; one consumer checks per-producer FIFO order.
template <class Queue>
StressResult mpscWideStress(Queue& q, uint32_t producers, uint64_t perProducer) {
  std::atomic<bool> go{false};
  std::vector<std::thread> ts;
  for (uint32_t p = 0; p < producers; ++p)
    ts.emplace_back([&, p] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      Backoff b;
      for (uint64_t i = 0; i < perProducer;) {
        if (q.tryPush(makeWide(p, i))) {
          ++i;
          b.reset();
        } else {
          b.pause();
        }
      }
    });
  StressResult r;
  std::vector<uint64_t> next(producers, 0);
  std::thread consumer([&] {
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    Backoff b;
    const uint64_t total = uint64_t{producers} * perProducer;
    while (r.received < total) {
      WidePayload v;
      if (!q.tryPop(v)) {
        b.pause();
        continue;
      }
      b.reset();
      ++r.received;
      uint32_t p = 0;
      uint64_t seq = 0;
      if (!decodeWide(v, p, seq)) ++r.torn;
      else if (p >= producers || seq != next[p]) ++r.seqErrors;
      else ++next[p];
    }
  });
  go.store(true, std::memory_order_release);
  for (auto& t : ts) t.join();
  consumer.join();
  for (uint32_t p = 0; p < producers; ++p)
    if (next[p] != perProducer) ++r.seqErrors;
  return r;
}

}  // namespace pq::test
