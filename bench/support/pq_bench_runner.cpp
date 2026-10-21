// The benchmark runner of pq_bench.h (T1.11): option parsing, filtering, row collection, CSV output.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>

#include "pq_bench.h"

#ifdef _WIN32
#include <intrin.h>
#endif

namespace pq::bench {

namespace {

// The rows of the benchmark running on this thread (report()/reportStats() append here).
struct Active {
  std::vector<Row>* rows = nullptr;
  const std::string* benchmark = nullptr;
};
thread_local Active tActive;

std::string csvField(const std::string& s) {
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string q = "\"";
  for (char c : s) {
    if (c == '"') q += '"';
    q += c;
  }
  return q + '"';
}

void usage(const std::string& suite) {
  std::printf(
      "usage: %s [options]\n"
      "  --filter REGEX   run the benchmarks whose name matches (ECMAScript regex, searched; default all)\n"
      "  --list           print the benchmark names and exit\n"
      "  --repeats N      timed rounds per measurement (default 15; median and p99 are over these)\n"
      "  --warmup N       untimed rounds first (default 3)\n"
      "  --min-time MS    minimum length of one round in ms (default 2)\n"
      "  --csv FILE       write the rows as CSV (tools/bench/append_results.py turns it into RESULTS.md rows)\n"
      "  --no-pin         do not pin the main thread to physical core 1 / raise its priority\n",
      suite.c_str());
}

bool parseInt(const char* s, int& v, int lo, int hi) {
  char* end = nullptr;
  const long x = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || x < lo || x > hi) return false;
  v = static_cast<int>(x);
  return true;
}

}  // namespace

namespace detail {
void recordRow(const char* name, const Stats& s, const char* unit) {
  if (tActive.rows == nullptr) return;
  Row r;
  r.benchmark = *tActive.benchmark;
  r.name = name;
  r.unit = unit;
  r.stats = s;
  tActive.rows->push_back(std::move(r));
}
}  // namespace detail

std::string cpuBrand() {
#ifdef _WIN32
  int regs[4] = {};
  __cpuid(regs, static_cast<int>(0x80000000));
  if (static_cast<unsigned>(regs[0]) < 0x80000004u) return "unknown";
  char brand[49] = {};
  for (int i = 0; i < 3; ++i) {
    __cpuid(regs, static_cast<int>(0x80000002u + static_cast<unsigned>(i)));
    std::memcpy(brand + 16 * i, regs, 16);
  }
  std::string b(brand);
  const size_t a = b.find_first_not_of(' ');
  return a == std::string::npos ? "unknown" : b.substr(a);
#else
  return "unknown";
#endif
}

const char* buildType() {
#ifdef NDEBUG
  return "release";
#else
  return "dev";
#endif
}

bool writeCsv(const std::string& path, const std::string& suite, const std::vector<Row>& rows) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  const std::string cpu = csvField(cpuBrand());
  f << "suite,benchmark,name,unit,median,min,p99,max,samples,load_percent,cpu,build\n";
  char num[256];
  for (const Row& r : rows) {
    std::snprintf(num, sizeof num, "%.6g,%.6g,%.6g,%.6g,%zu,%.1f", r.stats.medianNs, r.stats.minNs, r.stats.p99Ns,
                  r.stats.maxNs, r.stats.samples, r.loadPercent);
    f << csvField(suite) << ',' << csvField(r.benchmark) << ',' << csvField(r.name) << ',' << csvField(r.unit) << ','
      << num << ',' << cpu << ',' << buildType() << '\n';
  }
  return static_cast<bool>(f);
}

int Runner::run(const RunOptions& o) {
  std::regex re;
  if (!o.filter.empty()) re = std::regex(o.filter, std::regex::ECMAScript);
  auto selected = [&](const std::string& n) { return o.filter.empty() || std::regex_search(n, re); };
  if (o.list) {
    for (const Bench& b : benches_)
      if (selected(b.name)) std::printf("%s\n", b.name.c_str());
    return 0;
  }
  if (std::none_of(benches_.begin(), benches_.end(), [&](const Bench& b) { return selected(b.name); })) {
    std::fprintf(stderr, "%s: no benchmark matches '%s' (--list shows the names)\n", suite_.c_str(), o.filter.c_str());
    return 1;
  }
  if (o.pin) {
    pinCurrentThread(cpuTopology().coreMask(1));
    raisePriority();
  }
  std::printf("SUITE %s: %s, %s build, %d repeats, %d warmup, rounds >= %.1f ms%s\n", suite_.c_str(), cpuBrand().c_str(),
              buildType(), o.repeats, o.warmup, o.minRoundNs / 1e6, o.pin ? ", main thread pinned to core 1" : "");
  for (const Bench& b : benches_) {
    if (!selected(b.name)) continue;
    const size_t first = rows_.size();
    tActive = {&rows_, &b.name};
    SystemLoad load;
    Context ctx(b.name, o);
    b.fn(ctx);
    const double busy = load.busyPercent();
    tActive = {};
    for (size_t i = first; i < rows_.size(); ++i) rows_[i].loadPercent = busy;
  }
  if (!o.csvPath.empty()) {
    if (!writeCsv(o.csvPath, suite_, rows_)) {
      std::fprintf(stderr, "%s: cannot write %s\n", suite_.c_str(), o.csvPath.c_str());
      return 1;
    }
    std::printf("CSV %s: %zu rows\n", o.csvPath.c_str(), rows_.size());
  }
  return 0;
}

int Runner::main(int argc, char** argv) {
  RunOptions o;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const bool hasValue = i + 1 < argc;
    int ms = 0;
    if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
      usage(suite_);
      return 0;
    } else if (std::strcmp(a, "--list") == 0) {
      o.list = true;
    } else if (std::strcmp(a, "--no-pin") == 0) {
      o.pin = false;
    } else if (std::strcmp(a, "--filter") == 0 && hasValue) {
      o.filter = argv[++i];
      try {
        (void)std::regex(o.filter, std::regex::ECMAScript);
      } catch (const std::regex_error&) {
        std::fprintf(stderr, "%s: bad --filter regex '%s'\n", suite_.c_str(), o.filter.c_str());
        return 2;
      }
    } else if (std::strcmp(a, "--csv") == 0 && hasValue) {
      o.csvPath = argv[++i];
    } else if (std::strcmp(a, "--repeats") == 0 && hasValue && parseInt(argv[i + 1], o.repeats, 1, 100000)) {
      ++i;
    } else if (std::strcmp(a, "--warmup") == 0 && hasValue && parseInt(argv[i + 1], o.warmup, 0, 100000)) {
      ++i;
    } else if (std::strcmp(a, "--min-time") == 0 && hasValue && parseInt(argv[i + 1], ms, 0, 600000)) {
      o.minRoundNs = ms * 1e6;
      ++i;
    } else {
      std::fprintf(stderr, "%s: bad option '%s'\n", suite_.c_str(), a);
      usage(suite_);
      return 2;
    }
  }
  return run(o);
}

}  // namespace pq::bench
