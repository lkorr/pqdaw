// Thread placement, TSC and system-load helpers for pq_bench.h (T0.2-fix1). Kept out of the header so
// bench sources don't see <windows.h>.

#include "pq_bench.h"

#include <bit>
#include <chrono>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>  // NOMINMAX and WIN32_LEAN_AND_MEAN come from pq_target_defaults
#include <intrin.h>
#endif

namespace pq::bench {

#ifdef _WIN32

namespace {

CpuTopology queryTopology() {
  CpuTopology t;
  t.logicalProcessors = static_cast<int>(GetActiveProcessorCount(0));
  DWORD bytes = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
  if (bytes == 0) return t;
  std::vector<unsigned char> buf(bytes);
  auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes)) return t;
  for (DWORD off = 0; off < bytes;) {
    auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
    if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount >= 1 &&
        e->Processor.GroupMask[0].Group == 0) {
      uint64_t m = e->Processor.GroupMask[0].Mask;
      const uint64_t first = m & (~m + 1);
      m &= ~first;
      const uint64_t second = m & (~m + 1);
      t.physicalCores.push_back(first);
      t.smtSiblings.push_back(second);
    }
    off += e->Size;
  }
  // Order by logical processor number so core i is stable across runs.
  std::vector<std::size_t> idx(t.physicalCores.size());
  for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
    return std::countr_zero(t.physicalCores[a]) < std::countr_zero(t.physicalCores[b]);
  });
  CpuTopology sorted;
  sorted.logicalProcessors = t.logicalProcessors;
  for (std::size_t i : idx) {
    sorted.physicalCores.push_back(t.physicalCores[i]);
    sorted.smtSiblings.push_back(t.smtSiblings[i]);
  }
  return sorted;
}

uint64_t fileTime(const FILETIME& f) { return (uint64_t{f.dwHighDateTime} << 32) | f.dwLowDateTime; }

}  // namespace

const CpuTopology& cpuTopology() {
  static const CpuTopology t = queryTopology();
  return t;
}

bool pinCurrentThread(uint64_t mask) {
  if (mask == 0) return false;
  return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask)) != 0;
}

bool raisePriority() {
  static const bool processOk = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
  return processOk && SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
}

uint64_t rdtsc() noexcept { return __rdtsc(); }

void SystemLoad::restart() {
  FILETIME i, k, u;
  if (GetSystemTimes(&i, &k, &u)) {
    idle_ = fileTime(i);
    kernel_ = fileTime(k);
    user_ = fileTime(u);
  }
}

double SystemLoad::busyPercent() const {
  FILETIME i, k, u;
  if (!GetSystemTimes(&i, &k, &u)) return -1;
  const double idle = static_cast<double>(fileTime(i) - idle_);
  const double total = static_cast<double>((fileTime(k) - kernel_) + (fileTime(u) - user_));  // kernel includes idle
  return total > 0 ? 100.0 * (1.0 - idle / total) : -1;
}

#else

const CpuTopology& cpuTopology() {
  static const CpuTopology t;
  return t;
}
bool pinCurrentThread(uint64_t) { return false; }
bool raisePriority() { return false; }
uint64_t rdtsc() noexcept {
  return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}
void SystemLoad::restart() {}
double SystemLoad::busyPercent() const { return -1; }

#endif

double tscTicksPerNs() {
  static const double rate = [] {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const uint64_t c0 = rdtsc();
    while (clock::now() - t0 < std::chrono::milliseconds(50)) {}
    const uint64_t c1 = rdtsc();
    const double ns = std::chrono::duration<double, std::nano>(clock::now() - t0).count();
    return static_cast<double>(c1 - c0) / ns;
  }();
  return rate;
}

void printPlacement(const char* what, uint64_t mask) {
  const auto& t = cpuTopology();
  std::printf("PLACEMENT %-28s logical cpu %2d (%zu physical cores, %d logical)\n", what,
              mask ? std::countr_zero(mask) : -1, t.physicalCores.size(), t.logicalProcessors);
}

}  // namespace pq::bench
