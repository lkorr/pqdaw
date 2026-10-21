// T0.2 benchmarks: queue push+pop cost, pool/arena/epoch RT-side operations; T0.2-fix1: pinned
// cross-thread throughput and round-trip latency, 64 vs 128-byte index separation.
// Run from the release build: build/release/bench/pq_bench_core.exe [cross [runs]]  (runs >= 5, default 11)

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <immintrin.h>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
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

// ---- Cross-thread benchmarks (T0.2-fix1).
//
// Placement: every thread runs on its own physical core (never two SMT siblings), at raised priority
// (HIGH_PRIORITY_CLASS + THREAD_PRIORITY_HIGHEST). Physical core 0 is left alone (interrupts/DPCs land
// there most often): the consumer / ping side is on core 1, producers / pong on cores 2, 3, 4, 5.
// "unpinned" variants use plain std::threads at normal priority, as the T0.2 rows did.
//
// Layouts (review L3). Intel's L2 spatial prefetcher pairs 64-byte lines into 128-byte-aligned blocks.
// Each queue is constructed at a chosen offset in a 4096-aligned block so the pairing is controlled
// instead of depending on where the stack or heap put the object:
//   sep 64 @0    producer- and consumer-written index lines in the same 128-byte pair (the T0.2 layout's
//                worst case)
//   sep 64 @64   the same layout shifted by one line: the two index lines in different pairs
//   sep 128      HotLineSeparation = 128: different pairs wherever the object lands
// Variants are interleaved run by run (A B C, B C A, ...) so machine-load drift hits all of them.

constexpr uint32_t kCap = 1024;
constexpr uint64_t kChunk = 1u << 16;  // throughput: one sample per 65 536 popped items

// 64-byte command, the size class of a UI→audio command.
struct alignas(64) Cmd64 {
  uint64_t w[8] = {};
};

template <class T>
T makeItem(uint64_t i) noexcept {
  if constexpr (std::is_same_v<T, uint64_t>) {
    return i;
  } else {
    T c;
    for (uint64_t k = 0; k < 8; ++k) c.w[k] = i + k;
    return c;
  }
}
inline uint64_t seqOf(uint64_t v) noexcept { return v; }
inline uint64_t seqOf(const Cmd64& c) noexcept { return c.w[7] - 7 == c.w[0] ? c.w[0] : ~uint64_t{0}; }

// A Q constructed `offset` bytes into a fresh 4096-aligned, 8192-byte allocation. The size is the same for
// every Q so the heap hands out the same addresses (e.g. for the slot buffers allocated next) whatever the
// layout under test: only the queue's own layout differs between variants.
template <class Q>
class Placed {
public:
  static constexpr std::size_t kBlock = 8192;
  Placed(std::size_t offset, uint32_t capacity)
      : mem_(static_cast<std::byte*>(::operator new(kBlock, std::align_val_t{4096}))) {
    static_assert(sizeof(Q) + 128 <= kBlock);
    q_ = ::new (static_cast<void*>(mem_ + offset)) Q(capacity);
  }
  ~Placed() {
    q_->~Q();
    ::operator delete(mem_, kBlock, std::align_val_t{4096});
  }
  Placed(const Placed&) = delete;
  Placed& operator=(const Placed&) = delete;
  Q& operator*() noexcept { return *q_; }

private:
  std::byte* mem_;
  Q* q_ = nullptr;
};

template <class F>
std::thread spawn(bool pinned, uint64_t mask, F&& f) {
  if (pinned) return pq::bench::pinnedThread(mask, std::forward<F>(f));
  return std::thread(std::forward<F>(f));
}

// One throughput run: `producers` threads push perProducer uint64 items each, one consumer pops them all.
// Returns ns/item for the run; appends one ns/item sample per kChunk items to chunkNs.
template <class Queue>
double throughputRun(bool pinned, std::size_t offset, uint32_t producers, uint64_t perProducer,
                     std::vector<double>& chunkNs) {
  const auto& topo = pq::bench::cpuTopology();
  const double tpn = pq::bench::tscTicksPerNs();
  Placed<Queue> placed(offset, kCap);
  Queue& q = *placed;
  std::atomic<uint32_t> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> ts;
  for (uint32_t p = 0; p < producers; ++p)
    ts.push_back(spawn(pinned, topo.coreMask(2 + p), [&] {
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!go.load(std::memory_order_acquire)) _mm_pause();
      for (uint64_t i = 0; i < perProducer;)
        if (q.tryPush(i)) ++i;
    }));
  double nsPerItem = 0;
  const uint64_t total = perProducer * producers;
  ts.push_back(spawn(pinned, topo.coreMask(1), [&] {
    while (ready.load(std::memory_order_acquire) != producers) _mm_pause();
    uint64_t got = 0, sum = 0, v = 0;
    const uint64_t t0 = pq::bench::rdtsc();
    uint64_t last = t0;
    go.store(true, std::memory_order_release);
    while (got < total)
      if (q.tryPop(v)) {
        ++got;
        sum += v;
        if ((got & (kChunk - 1)) == 0) {
          const uint64_t now = pq::bench::rdtsc();
          chunkNs.push_back(static_cast<double>(now - last) / tpn / kChunk);
          last = now;
        }
      }
    nsPerItem = static_cast<double>(pq::bench::rdtsc() - t0) / tpn / static_cast<double>(total);
    doNotOptimize(sum);
  }));
  for (auto& t : ts) t.join();
  return nsPerItem;
}

// One round-trip run: ping pushes item i into `req`, pong pops it and pushes it back on `rep`, ping
// waits for it (what a UI→audio command plus its acknowledgement costs). Appends `trips` round-trip
// times (ns, rdtsc) after 10 000 untimed warm-up trips; returns the run's median.
template <class Ring, class T>
double roundTripRun(bool pinned, std::size_t offset, uint64_t pingMask, uint64_t pongMask, uint32_t trips,
                    std::vector<double>& outNs, uint64_t& errors) {
  const double tpn = pq::bench::tscTicksPerNs();
  Placed<Ring> reqP(offset, 64), repP(offset, 64);
  Ring& req = *reqP;
  Ring& rep = *repP;
  constexpr uint32_t kWarm = 10'000;
  const uint64_t total = uint64_t{kWarm} + trips;
  const std::size_t before = outNs.size();
  std::thread pong = spawn(pinned, pongMask, [&] {
    T v{};
    for (uint64_t n = 0; n < total;) {
      if (!req.tryPop(v)) continue;
      while (!rep.tryPush(v)) {}
      ++n;
    }
  });
  std::thread ping = spawn(pinned, pingMask, [&] {
    T r{};
    for (uint64_t i = 0; i < total; ++i) {
      const T v = makeItem<T>(i);
      const uint64_t t0 = pq::bench::rdtsc();
      while (!req.tryPush(v)) {}
      while (!rep.tryPop(r)) {}
      const uint64_t t1 = pq::bench::rdtsc();
      if (seqOf(r) != i) ++errors;
      if (i >= kWarm) outNs.push_back(static_cast<double>(t1 - t0) / tpn);
    }
  });
  ping.join();
  pong.join();
  return pq::bench::summarize(std::vector<double>(outNs.begin() + static_cast<std::ptrdiff_t>(before), outNs.end()))
      .medianNs;
}

// A benchmark variant: run(samples) performs one run, appends its fine-grained samples and returns the
// run's figure (ns/item for throughput, median ns for round trips).
struct Variant {
  std::string name;
  std::function<double(std::vector<double>&)> run;
  std::vector<double> perRun, samples;
};

const char* gFilter = "";  // "cross <runs> <substring>": only variant groups whose first name contains it

void runInterleaved(std::vector<Variant>& vs, int runs, std::size_t samplesPerRun, const char* sampleWhat,
                    const char* unit, unsigned spinningThreads) {
  if (vs.empty() || vs.front().name.find(gFilter) == std::string::npos) return;
  for (auto& v : vs) {
    v.perRun.reserve(static_cast<std::size_t>(runs));
    v.samples.reserve(static_cast<std::size_t>(runs) * samplesPerRun);
  }
  pq::bench::SystemLoad load;
  for (int r = 0; r < runs; ++r)
    for (std::size_t k = 0; k < vs.size(); ++k) {
      Variant& v = vs[(k + static_cast<std::size_t>(r)) % vs.size()];
      v.perRun.push_back(v.run(v.samples));
    }
  const double busy = load.busyPercent();
  char line[160];
  for (auto& v : vs) {
    std::snprintf(line, sizeof line, "%s [per run]", v.name.c_str());
    pq::bench::reportStats(line, pq::bench::summarize(v.perRun), unit);
    std::snprintf(line, sizeof line, "%s [per %s]", v.name.c_str(), sampleWhat);
    pq::bench::reportStats(line, pq::bench::summarize(v.samples), unit);
  }
  std::printf("LOAD   system busy %.1f%% over the block (all 16 logical cpus; includes this benchmark's %u spinning "
              "threads = %.1f%%)\n",
              busy, spinningThreads, 100.0 * spinningThreads / pq::bench::cpuTopology().logicalProcessors);
}

template <template <class, std::size_t> class Q>
std::vector<Variant> throughputLayouts(const char* what, uint32_t producers, uint64_t items) {
  const uint64_t per = items / producers;
  std::vector<Variant> vs;
  vs.push_back({std::string(what) + ", sep 64 @0", [=](auto& s) {
                  return throughputRun<Q<uint64_t, 64>>(true, 0, producers, per, s);
                }, {}, {}});
  vs.push_back({std::string(what) + ", sep 64 @64", [=](auto& s) {
                  return throughputRun<Q<uint64_t, 64>>(true, 64, producers, per, s);
                }, {}, {}});
  vs.push_back({std::string(what) + ", sep 128", [=](auto& s) {
                  return throughputRun<Q<uint64_t, 128>>(true, 0, producers, per, s);
                }, {}, {}});
  return vs;
}

template <class T>
std::vector<Variant> roundTripLayouts(const char* what, uint32_t trips, uint64_t& errors) {
  const auto& topo = pq::bench::cpuTopology();
  const uint64_t a = topo.coreMask(1), b = topo.coreMask(2);
  uint64_t* e = &errors;
  std::vector<Variant> vs;
  vs.push_back({std::string(what) + ", sep 64 @0", [=](auto& s) {
                  return roundTripRun<pq::SpscRing<T, 64>, T>(true, 0, a, b, trips, s, *e);
                }, {}, {}});
  vs.push_back({std::string(what) + ", sep 64 @64", [=](auto& s) {
                  return roundTripRun<pq::SpscRing<T, 64>, T>(true, 64, a, b, trips, s, *e);
                }, {}, {}});
  vs.push_back({std::string(what) + ", sep 128", [=](auto& s) {
                  return roundTripRun<pq::SpscRing<T, 128>, T>(true, 0, a, b, trips, s, *e);
                }, {}, {}});
  return vs;
}

void crossThreadBenchmarks(int runs) {
  const auto& topo = pq::bench::cpuTopology();
  if (topo.physicalCores.size() < 6) {
    std::printf("SKIP   cross-thread benchmarks need 6 physical cores (found %zu)\n", topo.physicalCores.size());
    return;
  }
  const bool prio = pq::bench::raisePriority();
  std::printf("METHOD pinned: consumer/ping physical core 1 (cpu %d), producer/pong core 2 (cpu %d), extra producers "
              "cores 3-5 (cpus %d %d %d); priority raised: %s; TSC %.3f ticks/ns; %d runs per variant, interleaved\n",
              std::countr_zero(topo.coreMask(1)), std::countr_zero(topo.coreMask(2)),
              std::countr_zero(topo.coreMask(3)), std::countr_zero(topo.coreMask(4)),
              std::countr_zero(topo.coreMask(5)), prio ? "yes" : "NO", pq::bench::tscTicksPerNs(), runs);

  constexpr uint64_t kItems = 10'000'000;
  constexpr uint32_t kTrips = 200'000;
  {
    auto vs = throughputLayouts<pq::SpscRing>("spsc 1P/1C throughput", 1, kItems);
    runInterleaved(vs, runs, kItems / kChunk + 1, "64k items", "ns/item", 2);
  }
  {
    auto vs = throughputLayouts<pq::MpscQueue>("mpsc 1P/1C throughput", 1, kItems);
    runInterleaved(vs, runs, kItems / kChunk + 1, "64k items", "ns/item", 2);
  }
  {
    auto vs = throughputLayouts<pq::MpscQueue>("mpsc 4P/1C throughput", 4, kItems / 2);
    runInterleaved(vs, runs, kItems / kChunk + 1, "64k items", "ns/item", 5);
  }
  uint64_t errors = 0;
  {
    auto vs = roundTripLayouts<uint64_t>("spsc round trip u64", kTrips, errors);
    runInterleaved(vs, runs, kTrips, "trip", "ns/round trip", 2);
  }
  {
    auto vs = roundTripLayouts<Cmd64>("spsc round trip 64B cmd", kTrips, errors);
    runInterleaved(vs, runs, kTrips, "trip", "ns/round trip", 2);
  }
  // Placement comparison with the shipping layout: why pinning matters for these numbers.
  {
    const uint64_t a = topo.coreMask(1), sib = topo.smtSiblings[1];
    uint64_t* e = &errors;
    std::vector<Variant> vs;
    if (sib != 0)
      vs.push_back({"spsc round trip u64, SMT siblings (same core)", [=](auto& s) {
                      return roundTripRun<pq::SpscRing<uint64_t>, uint64_t>(true, 0, a, sib, kTrips, s, *e);
                    }, {}, {}});
    vs.push_back({"spsc round trip u64, unpinned", [=](auto& s) {
                    return roundTripRun<pq::SpscRing<uint64_t>, uint64_t>(false, 0, 0, 0, kTrips, s, *e);
                  }, {}, {}});
    runInterleaved(vs, runs, kTrips, "trip", "ns/round trip", 2);
  }
  {
    std::vector<Variant> vs;
    vs.push_back({"spsc 1P/1C throughput, unpinned", [=](auto& s) {
                    return throughputRun<pq::SpscRing<uint64_t>>(false, 0, 1, kItems, s);
                  }, {}, {}});
    runInterleaved(vs, runs, kItems / kChunk + 1, "64k items", "ns/item", 2);
  }
  if (errors) std::printf("ERROR  %llu round-trip payload mismatches\n", static_cast<unsigned long long>(errors));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "cross") == 0) {
    if (argc > 3) gFilter = argv[3];
    crossThreadBenchmarks(argc > 2 ? std::max(5, std::atoi(argv[2])) : 11);
    return 0;
  }
  // Single-thread rows: main thread pinned to physical core 1 at raised priority.
  pq::bench::pinCurrentThread(pq::bench::cpuTopology().coreMask(1));
  pq::bench::raisePriority();
  // --- Single thread: push+pop pair, both sides warm in L1. The cost of the operations themselves.
  {
    pq::SpscRing<uint64_t> r(1024);
    uint64_t v = 0;
    pq::bench::reportStats("spsc push+pop (1 thread, batch 64)", measure([&] {
             for (int i = 0; i < kBatch; ++i) r.tryPush(static_cast<uint64_t>(i));
             for (int i = 0; i < kBatch; ++i) r.tryPop(v);
             doNotOptimize(v);
           }, kBatch), "ns/push+pop");
    pq::bench::reportStats("spsc push+pop (1 thread, alternating)", measure([&] {
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
    pq::bench::reportStats("mpsc push+pop (1 thread, batch 64)", measure([&] {
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

  // --- Cross thread (T0.2-fix1): pinned throughput, round-trip latency, 64 vs 128-byte separation.
  // "pq_bench_core cross" runs only this part.
  crossThreadBenchmarks(11);

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
    pq::bench::reportStats("epoch quiescent (reader side)", measure([&] {
             for (int i = 0; i < kBatch; ++i) rec.quiescent(*r);
           }, kBatch), "ns/call");
  }
  return 0;
}
