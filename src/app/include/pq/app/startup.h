#pragma once

// Startup timing (T3.7; SPEC §11: first frame < 300 ms after launch). main() calls markProcessStart() first thing;
// the shell window logs the first presented frame against it (and against the OS process creation time, which also
// counts loading the executable and the CRT start-up before main).

#include <cstdint>
#include <string>
#include <vector>

namespace pq::app {

void markProcessStart() noexcept;
// Milliseconds since markProcessStart() (QueryPerformanceCounter); < 0 if it was never called.
double msSinceMainEntry() noexcept;
// Milliseconds since the OS created this process (GetProcessTimes); < 0 where unavailable.
double msSinceProcessCreation() noexcept;

// Startup phases (T0.7-fix6): markStartupPhase records "phase ended at t ms after process creation" (any thread;
// `name` must outlive the process, e.g. a string literal; at most kMaxStartupPhases, later marks are dropped).
// startupPhaseReport() is the table of every mark with the time since the previous mark on any thread, printed at
// the first frame when startupLogEnabled() (environment PQ_STARTUP_LOG set and not "0").
inline constexpr uint32_t kMaxStartupPhases = 48;
void markStartupPhase(const char* name) noexcept;
// A detail line printed under the table (e.g. how the Vulkan device time splits).
void addStartupNote(std::string note);
std::string startupPhaseReport();
bool startupLogEnabled() noexcept;
// CPU time (user + kernel) this process has used so far, in milliseconds (GetProcessTimes); < 0 where unavailable.
double processCpuMs() noexcept;
// Precise process CPU: cycles all threads ran (QueryProcessCycleTime, counted in TSC ticks) and the TSC itself, so a
// caller converts cycles to seconds with the TSC rate measured over the same interval (GetProcessTimes only
// advances in 15.6 ms scheduler ticks, too coarse for a 0.1 % budget over 10 s). 0 where unavailable.
uint64_t processCycles() noexcept;
// Per-thread cycles of this process (thread id, cycles), for attributing idle CPU. Empty where unavailable.
struct ThreadCycles {
  uint32_t threadId;
  uint64_t cycles;
  std::wstring description;  // SetThreadDescription name, if any
  std::wstring module;       // file name of the module its start address is in ("pqdaw.exe", a GPU driver dll, ...)
};
std::vector<ThreadCycles> threadCycles();
// File name of this process's executable ("pqdaw.exe").
std::wstring processModuleName();
uint64_t tscNow() noexcept;
// Cycles the calling thread has run (QueryThreadCycleTime).
uint64_t currentThreadCycles() noexcept;

}  // namespace pq::app
