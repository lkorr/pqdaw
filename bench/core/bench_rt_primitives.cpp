// T0.2 benchmarks: queue push+pop cost, pool/arena/epoch RT-side operations.
// Run from the release build: build/release/bench/pq_bench_core.exe

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pq/core/arena.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/core/fixed_pool.h"
#include "pq/core/mpsc_queue.h"
#include "pq/core/rt_log.h"
#include "pq/core/spsc_ring.h"
#include "pq_bench.h"

namespace {

using pq::bench::doNotOptimize;
using pq::bench::measure;
using pq::bench::report;

constexpr int kBatch = 64;

// Two threads: `producers` threads push `items` in total, one thread pops. Returns ns per item
// (median and min over `repeats` runs). Measures throughput, including cache-line transfer.
template <class Queue>
pq::bench::Stats crossThread(uint32_t capacity, uint32_t producers, uint64_t items, int repeats = 7) {
  std::vector<double> perItem;
  for (int r = 0; r < repeats; ++r) {
    Queue q(capacity);
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    const uint64_t per = items / producers;
    for (uint32_t p = 0; p < producers; ++p)
      ts.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {}
        for (uint64_t i = 0; i < per;)
          if (q.tryPush(i)) ++i;
      });
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    uint64_t got = 0, sum = 0, v = 0;
    while (got < per * producers)
      if (q.tryPop(v)) {
        ++got;
        sum += v;
      }
    const auto t1 = std::chrono::steady_clock::now();
    for (auto& t : ts) t.join();
    doNotOptimize(sum);
    perItem.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(got));
  }
  std::sort(perItem.begin(), perItem.end());
  return {perItem[perItem.size() / 2], perItem.front()};
}

}  // namespace

int main() {
  // --- Single thread: push+pop pair, both sides warm in L1. The cost of the operations themselves.
  {
    pq::SpscRing<uint64_t> r(1024);
    uint64_t v = 0;
    report("spsc push+pop (1 thread, batch 64)", measure([&] {
             for (int i = 0; i < kBatch; ++i) r.tryPush(static_cast<uint64_t>(i));
             for (int i = 0; i < kBatch; ++i) r.tryPop(v);
             doNotOptimize(v);
           }, kBatch), "ns/push+pop");
    report("spsc push+pop (1 thread, alternating)", measure([&] {
             r.tryPush(v);
             r.tryPop(v);
             doNotOptimize(v);
           }, 1), "ns/push+pop");
  }
  // RtLog (T0.2-fix2): push of a 2-argument record (format pointer, code, 2 typed args, rdtsc) and the
  // consumer's drain without formatting; formatting cost is the logger thread's.
  {
    pq::RtLog log(4096);
    uint64_t seen = 0;
    report("rtlog push 2 args + drain (1 thread, batch 64)", measure([&] {
             for (int i = 0; i < kBatch; ++i) log.log(3, "wake {} late {}", static_cast<uint64_t>(i), 0.25 * i);
             log.drain([&](const pq::RtLogRecord& r) { seen += r.sequence; });
             doNotOptimize(seen);
           }, kBatch), "ns/record");
    pq::RtLogRecord r;
    r.format = "wake {} late {}";
    r.argc = 2;
    r.kinds[0] = pq::RtLogRecord::Kind::UInt;
    r.kinds[1] = pq::RtLogRecord::Kind::Float;
    r.args[0].u = 123456;
    r.args[1].d = 0.25;
    std::string text;
    report("rtlog format one record (logger thread)", measure([&] {
             text = pq::formatRtLogRecord(r);
             doNotOptimize(text[0]);
           }, 1), "ns/record");
  }
  {
    pq::MpscQueue<uint64_t> q(1024);
    uint64_t v = 0;
    report("mpsc push+pop (1 thread, batch 64)", measure([&] {
             for (int i = 0; i < kBatch; ++i) q.tryPush(static_cast<uint64_t>(i));
             for (int i = 0; i < kBatch; ++i) q.tryPop(v);
             doNotOptimize(v);
           }, kBatch), "ns/push+pop");
  }
  {
    // Baseline for scale: what the lock-free queues replace.
    std::mutex m;
    std::deque<uint64_t> d;
    uint64_t v = 0;
    report("mutex+deque push+pop (1 thread, batch 64)", measure([&] {
             for (int i = 0; i < kBatch; ++i) {
               std::lock_guard l(m);
               d.push_back(static_cast<uint64_t>(i));
             }
             for (int i = 0; i < kBatch; ++i) {
               std::lock_guard l(m);
               v = d.front();
               d.pop_front();
             }
             doNotOptimize(v);
           }, kBatch), "ns/push+pop");
  }

  // --- Cross thread: throughput with producer and consumer on different cores.
  constexpr uint64_t kItems = 10'000'000;
  report("spsc 1P/1C throughput (cap 1024)", crossThread<pq::SpscRing<uint64_t>>(1024, 1, kItems), "ns/item");
  report("mpsc 1P/1C throughput (cap 1024)", crossThread<pq::MpscQueue<uint64_t>>(1024, 1, kItems), "ns/item");
  report("mpsc 4P/1C throughput (cap 1024)", crossThread<pq::MpscQueue<uint64_t>>(1024, 4, kItems), "ns/item");

  // --- Pool, arena, epoch: RT-side operations.
  {
    struct Voice {
      float state[16];
    };
    pq::FixedPool<Voice> pool(256);
    Voice* vs[kBatch];
    report("fixed pool create+destroy (batch 64)", measure([&] {
             for (int i = 0; i < kBatch; ++i) vs[i] = pool.create();
             for (int i = kBatch - 1; i >= 0; --i) pool.destroy(vs[i]);
             doNotOptimize(vs[0]);
           }, kBatch), "ns/create+destroy");
  }
  {
    pq::Arena arena(1 << 20);
    report("arena allocate 64B-aligned (batch 64, reset)", measure([&] {
             void* p = nullptr;
             for (int i = 0; i < kBatch; ++i) p = arena.allocate(48);
             doNotOptimize(p);
             arena.reset();
           }, kBatch), "ns/alloc");
  }
  {
    pq::EpochReclaimer rec(1);
    const auto r = rec.registerReader();
    report("epoch quiescent (reader side)", measure([&] {
             for (int i = 0; i < kBatch; ++i) rec.quiescent(*r);
           }, kBatch), "ns/call");
  }
  return 0;
}
