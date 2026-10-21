#pragma once

// Minimal benchmark helper shared by all bench executables until T1.11 replaces or extends it.
// Usage:
//   auto s = pq::bench::measure([&] { kernel(buf, n); }, n);   // n = ops (samples, items…) per call
//   pq::bench::report("fastExp2/avx2", s, "ns/sample");
// Prints one machine-readable line: BENCH <name> <median> <min> <unit>. Copy results into bench/RESULTS.md (release only).
//
// Cross-thread benchmarks (T0.2-fix1): pin each thread to its own physical core and raise its priority so
// the result doesn't depend on where the scheduler puts the threads or whether two land on SMT siblings,
// collect one sample per run (or per chunk/round trip), and report min/median/p99/max:
//   pq::bench::raisePriority();                                   // process + current thread
//   const auto& topo = pq::bench::cpuTopology();                  // one mask per physical core
//   pq::bench::pinCurrentThread(topo.coreMask(1));
//   std::thread t = pq::bench::pinnedThread(topo.coreMask(2), [&] { ... });
//   pq::bench::reportStats("spsc round trip", pq::bench::summarize(samples), "ns");
//   pq::bench::SystemLoad load; ...; load.busyPercent();          // how busy the rest of the machine was

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

namespace pq::bench {

struct Stats {
  double medianNs = 0;  // per op
  double minNs = 0;     // per op
  double p99Ns = 0;     // per op; nearest rank, so with fewer than 100 samples it is the maximum
  double maxNs = 0;     // per op
  std::size_t samples = 0;
};

// min / median / p99 / max of `v` (any unit; the field names say ns because most callers use ns).
inline Stats summarize(std::vector<double> v) {
  Stats s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  s.minNs = v.front();
  s.medianNs = v[n / 2];
  const std::size_t rank99 = (99 * n + 99) / 100;  // ceil(0.99 n), 1-based nearest rank
  s.p99Ns = v[std::min(n, std::max<std::size_t>(rank99, 1)) - 1];
  s.maxNs = v.back();
  s.samples = n;
  return s;
}

// Defeat dead-code elimination of a computed value.
template <class T>
inline void doNotOptimize(const T& v) {
  const volatile char* p = reinterpret_cast<const volatile char*>(&v);
  (void)*p;
}

// Calls f() repeatedly: `warmup` untimed rounds, then `repeats` timed rounds, each round running f()
// enough times to last at least `minRoundNs`. Returns ns per op (opsPerCall ops per call of f).
template <class F>
Stats measure(F&& f, uint64_t opsPerCall, int repeats = 15, int warmup = 3, double minRoundNs = 2e6) {
  using clock = std::chrono::steady_clock;
  auto timeCalls = [&](uint64_t calls) {
    const auto t0 = clock::now();
    for (uint64_t i = 0; i < calls; ++i) f();
    return std::chrono::duration<double, std::nano>(clock::now() - t0).count();
  };
  uint64_t calls = 1;
  while (timeCalls(calls) < minRoundNs && calls < (1ull << 40)) calls *= 2;
  for (int i = 0; i < warmup; ++i) timeCalls(calls);
  std::vector<double> perOp;
  perOp.reserve(static_cast<size_t>(repeats));
  for (int i = 0; i < repeats; ++i)
    perOp.push_back(timeCalls(calls) / static_cast<double>(calls * opsPerCall));
  return summarize(std::move(perOp));
}

inline void report(const char* name, const Stats& s, const char* unit = "ns/op") {
  std::printf("BENCH %-40s median %10.4f  min %10.4f  %s\n", name, s.medianNs, s.minNs, unit);
}

// Like report(), with p99, max and the sample count.
inline void reportStats(const char* name, const Stats& s, const char* unit = "ns/op") {
  std::printf("BENCH %-52s median %9.3f  min %9.3f  p99 %9.3f  max %9.3f  (n=%zu)  %s\n", name, s.medianNs,
              s.minNs, s.p99Ns, s.maxNs, s.samples, unit);
}

// ---- Thread placement (Windows; elsewhere the queries return empty / false and pinning is a no-op).

struct CpuTopology {
  // One affinity mask per physical core: the lowest-numbered logical processor of that core, so two
  // masks never name SMT siblings. Processor group 0 only (this machine has a single group).
  std::vector<uint64_t> physicalCores;
  // The second logical processor of each physical core (0 when the core has no SMT sibling).
  std::vector<uint64_t> smtSiblings;
  int logicalProcessors = 0;

  // Mask of physical core i (wrapping), or 0 if the topology is unknown.
  uint64_t coreMask(std::size_t i) const noexcept {
    return physicalCores.empty() ? 0 : physicalCores[i % physicalCores.size()];
  }
};

// Queried once (GetLogicalProcessorInformationEx, RelationProcessorCore).
const CpuTopology& cpuTopology();

// Restricts the calling thread to `mask` (SetThreadAffinityMask). False if the mask is 0 or the call fails.
bool pinCurrentThread(uint64_t mask);

// HIGH_PRIORITY_CLASS for the process (once) and THREAD_PRIORITY_HIGHEST for the calling thread, so
// normal-priority work (other builds) rarely preempts a pinned benchmark thread. False on failure.
bool raisePriority();

// A std::thread that pins itself to `mask` and raises its priority before running f().
template <class F>
std::thread pinnedThread(uint64_t mask, F&& f) {
  return std::thread([mask, fn = std::forward<F>(f)]() mutable {
    pinCurrentThread(mask);
    raisePriority();
    fn();
  });
}

// Time-stamp counter (invariant TSC on every CPU we target) and its rate, calibrated once against
// steady_clock over ~50 ms. Cheap per-event timing for round-trip latencies.
uint64_t rdtsc() noexcept;
double tscTicksPerNs();

// System-wide CPU utilisation between construction (or restart()) and busyPercent(), from
// GetSystemTimes: how much of the machine other processes (and this one) kept busy during a run.
class SystemLoad {
public:
  SystemLoad() { restart(); }
  void restart();
  double busyPercent() const;  // 0..100 over all logical processors; -1 if unavailable

private:
  uint64_t idle_ = 0, kernel_ = 0, user_ = 0;
};

// One line describing the placement and load, for the RESULTS.md "method" note.
void printPlacement(const char* what, uint64_t mask);

}  // namespace pq::bench
