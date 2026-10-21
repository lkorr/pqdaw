#include "thread_cpu.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>

#include <chrono>

namespace pq::test {

uint64_t threadCycles() noexcept {
  ULONG64 c = 0;
  QueryThreadCycleTime(GetCurrentThread(), &c);
  return c;
}

double tscTicksPerUs() {
  static const double perUs = [] {
    using Clock = std::chrono::steady_clock;
    // A (clock, TSC) pair read back to back; retried if the thread was preempted between the reads.
    auto sample = [](Clock::time_point& t) {
      for (;;) {
        const uint64_t a = __rdtsc();
        t = Clock::now();
        const uint64_t b = __rdtsc();
        if (b - a < 20000) return a + (b - a) / 2;
      }
    };
    Clock::time_point t0, t1;
    const uint64_t c0 = sample(t0);
    while (Clock::now() - t0 < std::chrono::milliseconds(20)) {
    }
    const uint64_t c1 = sample(t1);
    return static_cast<double>(c1 - c0) / std::chrono::duration<double, std::micro>(t1 - t0).count();
  }();
  return perUs;
}

}  // namespace pq::test
