#pragma once

// Minimal benchmark helper shared by all bench executables until T1.11 replaces or extends it.
// Usage:
//   auto s = pq::bench::measure([&] { kernel(buf, n); }, n);   // n = ops (samples, items…) per call
//   pq::bench::report("fastExp2/avx2", s, "ns/sample");
// Prints one machine-readable line: BENCH <name> <median> <min> <unit>. Copy results into bench/RESULTS.md (release only).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace pq::bench {

struct Stats {
  double medianNs = 0;  // per op
  double minNs = 0;     // per op
};

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
  std::sort(perOp.begin(), perOp.end());
  return {perOp[perOp.size() / 2], perOp.front()};
}

inline void report(const char* name, const Stats& s, const char* unit = "ns/op") {
  std::printf("BENCH %-40s median %10.4f  min %10.4f  %s\n", name, s.medianNs, s.minNs, unit);
}

}  // namespace pq::bench
