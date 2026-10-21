#pragma once

// CPU time of the calling thread, for per-frame cost checks that must not depend on machine load (T0.x-flake).
// QueryThreadCycleTime counts only while the thread runs (time blocked or preempted is excluded), in units of the
// invariant TSC on current Windows; tscTicksPerUs() converts. Calibrating the TSC against steady_clock compares
// two wall clocks, so a busy machine does not change the result. (thread_cpu.cpp: keeps <windows.h> out of the
// test sources.)

#include <cstdint>

namespace pq::test {

uint64_t threadCycles() noexcept;
double tscTicksPerUs();
// Microseconds of CPU this thread has run.
inline double threadCpuUs() { return static_cast<double>(threadCycles()) / tscTicksPerUs(); }

}  // namespace pq::test
