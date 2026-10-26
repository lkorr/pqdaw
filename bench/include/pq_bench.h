#pragma once

// Benchmark helpers shared by all bench executables.
// Usage:
//   auto s = pq::bench::measure([&] { kernel(buf, n); }, n);   // n = ops (samples, items…) per call
//   pq::bench::report("fastExp2/avx2", s, "ns/sample");
// Prints one machine-readable line: BENCH <name> <median> <min> <unit>. Copy results into bench/RESULTS.md (release only).
//
// Runner (T1.11): named benchmarks, a regex filter, warmup/repeats/median/p99, CSV output, and
// tools/bench/append_results.py to turn the CSV into RESULTS.md rows (date, commit, machine). Usage and how to
// migrate a bench: tools/README.md "Benchmarks".
//   int main(int argc, char** argv) {
//     pq::bench::Runner r("pq_bench_dsp");
//     r.add("atom/add", [&](pq::bench::Context& c) {
//       c.measure([&] { kernel(args); }, 64, "ns/sample", "L1");     // row "atom/add L1"
//     });
//     return r.main(argc, argv);   // --filter REGEX --list --repeats N --warmup N --min-time MS --csv FILE --no-pin
//   }
// A benchmark runs only when the filter matches its name; everything it reports — through Context::measure /
// record, and through report()/reportStats() while it runs (so existing code migrates by wrapping it in add()) —
// becomes a row: printed as a BENCH line and written to the CSV with the CPU, build type, preset and system load.
// Median and p99 are over the timed repeats (nearest rank: with < 100 repeats p99 is the maximum).
//
// Per-block sample mode (T1.11-fix1, Q75 B), for benches whose call is one audio block (process(), ≥ ~1 µs):
//   c.measureBlocks([&] { host.process(block); }, 64, "ns/sample");   // row "<name> [per block]"
// times every call on its own (rdtsc) after a warm-up, so median / p99 / max describe single blocks (the tail a
// device callback sees), not round averages; `--blocks N` samples (default 2000).
//
// Higher-is-better units ("x realtime", "…/s": higherIsBetter()): report the low tail in the p99 field (the speed 99 %
// of rounds reached), min = the worst value, max = the best; summarizeHigherIsBetter() does that for raw samples.
// The CSV's `better` column says which kind a row is, so tools/bench/append_results.py labels it right.
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
#include <functional>
#include <string>
#include <string_view>
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

// The same for higher-is-better samples (speeds): p99Ns holds the 1st percentile (nearest rank from the bottom), the
// tail that matters for a speed; min / median / max as usual.
inline Stats summarizeHigherIsBetter(std::vector<double> v) {
  Stats s = summarize(v);
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  const std::size_t rank1 = (n + 99) / 100;  // ceil(0.01 n), 1-based
  s.p99Ns = v[std::min(n, std::max<std::size_t>(rank1, 1)) - 1];
  return s;
}

// Units where a larger number is better: "x realtime" style speeds and rates per second.
inline bool higherIsBetter(std::string_view unit) noexcept {
  return unit.find("realtime") != std::string_view::npos || (unit.size() >= 2 && unit.substr(unit.size() - 2) == "/s");
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

namespace detail {
// Adds a row to the Runner benchmark running on this thread (a no-op outside a Runner).
void recordRow(const char* name, const Stats& s, const char* unit);
}  // namespace detail

inline void report(const char* name, const Stats& s, const char* unit = "ns/op") {
  std::printf("BENCH %-40s median %10.4f  min %10.4f  %s\n", name, s.medianNs, s.minNs, unit);
  detail::recordRow(name, s, unit);
}

// Like report(), with p99, max and the sample count.
inline void reportStats(const char* name, const Stats& s, const char* unit = "ns/op") {
  std::printf("BENCH %-52s median %9.3f  min %9.3f  p99 %9.3f  max %9.3f  (n=%zu)  %s\n", name, s.medianNs,
              s.minNs, s.p99Ns, s.maxNs, s.samples, unit);
  detail::recordRow(name, s, unit);
}

// ---- Runner (T1.11) ---------------------------------------------------------------------------------------------

uint64_t rdtsc() noexcept;  // declared again below with the TSC helpers
double tscTicksPerNs();

struct RunOptions {
  std::string filter;        // ECMAScript regex searched in benchmark names; empty = all
  int repeats = 15;          // timed rounds per Context::measure
  int warmup = 3;            // untimed rounds first
  double minRoundNs = 2e6;   // each round repeats the call until it lasts this long
  uint32_t blocks = 2000;    // Context::measureBlocks: timed single calls
  std::string csvPath;       // empty: no CSV
  bool list = false;         // print the names and exit
  bool pin = true;           // pin the main thread to physical core 1 and raise its priority
};

// One row of results.
struct Row {
  std::string benchmark;     // the add() name
  std::string name;          // the reported name (benchmark name + suffix, or report()'s label)
  std::string unit;
  Stats stats;
  double loadPercent = -1;   // system busy % while its benchmark ran (all logical CPUs; -1 unknown)
};

class Context {
public:
  Context(const std::string& name, const RunOptions& o) : name_(name), opt_(o) {}
  const std::string& name() const noexcept { return name_; }
  const RunOptions& options() const noexcept { return opt_; }

  // measure() with the runner's warmup / repeats / round length; reported as `name suffix`.
  template <class F>
  Stats measure(F&& f, uint64_t opsPerCall, const char* unit, std::string_view suffix = {}) {
    const Stats s = pq::bench::measure(std::forward<F>(f), opsPerCall, opt_.repeats, opt_.warmup, opt_.minRoundNs);
    record(s, unit, suffix);
    return s;
  }
  // Per-block sample mode: `warmup` × 20 untimed calls, then options().blocks calls timed one by one (rdtsc), each
  // reported per op (opsPerCall ops per call): median / p99 / max over single calls. For calls of ≥ ~1 µs (an audio
  // block); the timer costs ~20 ns per call. Reported as `name [per block]` (or `name suffix [per block]`).
  template <class F>
  Stats measureBlocks(F&& f, uint64_t opsPerCall, const char* unit, std::string_view suffix = {}) {
    for (int i = 0; i < opt_.warmup * 20; ++i) f();
    std::vector<double> perOp;
    perOp.reserve(opt_.blocks);
    const double nsPerTick = 1.0 / tscTicksPerNs();
    for (uint32_t i = 0; i < opt_.blocks; ++i) {
      const uint64_t t0 = rdtsc();
      f();
      const uint64_t t1 = rdtsc();
      perOp.push_back(static_cast<double>(t1 - t0) * nsPerTick / static_cast<double>(opsPerCall));
    }
    const Stats s = summarize(std::move(perOp));
    record(s, unit, suffix.empty() ? std::string_view("[per block]") : suffix);
    return s;
  }
  // A result measured some other way (cross-thread samples, derived figures).
  void record(const Stats& s, const char* unit, std::string_view suffix = {}) {
    const std::string label = suffix.empty() ? name_ : name_ + " " + std::string(suffix);
    reportStats(label.c_str(), s, unit);
  }

private:
  const std::string& name_;
  const RunOptions& opt_;
};

class Runner {
public:
  explicit Runner(std::string suite) : suite_(std::move(suite)) {}
  // Benchmarks run in the order added. '/' groups names (`atom/add`, `spsc/round trip`).
  void add(std::string name, std::function<void(Context&)> fn) { benches_.push_back({std::move(name), std::move(fn)}); }
  // Parses the options (usage on --help), runs the matching benchmarks, writes the CSV. Exit code: 0; 1 when the
  // filter matched nothing or the CSV could not be written; 2 for bad options.
  int main(int argc, char** argv);
  int run(const RunOptions& options);
  const std::vector<Row>& rows() const noexcept { return rows_; }
  const std::string& suite() const noexcept { return suite_; }

private:
  struct Bench {
    std::string name;
    std::function<void(Context&)> fn;
  };
  std::string suite_;
  std::vector<Bench> benches_;
  std::vector<Row> rows_;
};

// The CSV the runner writes: a header, then one line per row: suite, benchmark, name, unit, median, min, p99, max,
// samples, load_percent, cpu, build, preset, better (fields quoted when they contain a comma or a quote). `better` is
// "higher" for higherIsBetter() units (p99 then holds the low tail), else "lower".
bool writeCsv(const std::string& path, const std::string& suite, const std::vector<Row>& rows);
// The CPU brand string (cpuid) and "release" / "dev" (whether NDEBUG was set for the bench support library).
std::string cpuBrand();
const char* buildType();
// The CMake preset the bench support library was compiled with: "ship" (/GL + /LTCG), "release" (no LTCG) or "dev",
// from its compile flags (bench/CMakeLists.txt, T1.11-fix1). RESULTS.md baselines come from ship.
const char* presetName();

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
