#include "pq/app/startup.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace pq::app {

namespace {
// The one process-wide startup stamp (an explicit app-level singleton, ARCHITECTURE §6).
std::atomic<int64_t> gMainEntryQpc{0};

#ifdef _WIN32
double qpcToMs(int64_t ticks) noexcept {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return 1000.0 * static_cast<double>(ticks) / static_cast<double>(f.QuadPart);
}
uint64_t fileTimeTicks(const FILETIME& t) noexcept {  // 100 ns units
  return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
#endif
}  // namespace

void markProcessStart() noexcept {
#ifdef _WIN32
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  gMainEntryQpc.store(now.QuadPart, std::memory_order_relaxed);
#endif
}

double msSinceMainEntry() noexcept {
#ifdef _WIN32
  const int64_t start = gMainEntryQpc.load(std::memory_order_relaxed);
  if (start == 0) return -1.0;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return qpcToMs(now.QuadPart - start);
#else
  return -1.0;
#endif
}

double msSinceProcessCreation() noexcept {
#ifdef _WIN32
  FILETIME created, exited, kernel, user, now;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
  GetSystemTimePreciseAsFileTime(&now);
  return static_cast<double>(fileTimeTicks(now) - fileTimeTicks(created)) * 1e-4;
#else
  return -1.0;
#endif
}

namespace {
struct Phase {
  const char* name;
  double ms;
  uint32_t thread;
};
// Explicit app-level singleton (ARCHITECTURE §6): the startup marks (UI and startup worker threads; never audio).
Phase gPhases[kMaxStartupPhases];
uint32_t gPhaseCount = 0;
std::vector<std::string> gNotes;
std::mutex gPhaseMutex;
}  // namespace

void addStartupNote(std::string note) {
  std::lock_guard lock(gPhaseMutex);
  gNotes.push_back(std::move(note));
}

void markStartupPhase(const char* name) noexcept {
  const double ms = msSinceProcessCreation();
#ifdef _WIN32
  const uint32_t tid = GetCurrentThreadId();
#else
  const uint32_t tid = 0;
#endif
  std::lock_guard lock(gPhaseMutex);
  if (gPhaseCount < kMaxStartupPhases) gPhases[gPhaseCount++] = {name, ms, tid};
}

std::string startupPhaseReport() {
  std::vector<Phase> ps;
  std::vector<std::string> notes;
  {
    std::lock_guard lock(gPhaseMutex);
    ps.assign(gPhases, gPhases + gPhaseCount);
    notes = gNotes;
  }
  const uint32_t n = static_cast<uint32_t>(ps.size());
  std::stable_sort(ps.begin(), ps.end(), [](const Phase& a, const Phase& b) { return a.ms < b.ms; });
  std::string out = "startup phases (ms after process creation; +ms since the previous mark):\n";
  double prev = 0.0;
  const uint32_t mainThread = n ? ps.front().thread : 0;
  for (const Phase& p : ps) {
    char line[160];
    std::snprintf(line, sizeof line, "  %8.1f  %+7.1f  %s%s\n", p.ms, p.ms - prev, p.name, p.thread == mainThread ? "" : "  [worker]");
    out += line;
    prev = p.ms;
  }
  for (const std::string& note : notes) out += "  " + note + "\n";
  return out;
}

bool startupLogEnabled() noexcept {
  static const bool on = [] {
    const char* v = std::getenv("PQ_STARTUP_LOG");
    return v && *v && !(v[0] == '0' && v[1] == 0);
  }();
  return on;
}

uint64_t processCycles() noexcept {
#ifdef _WIN32
  ULONG64 c = 0;
  return QueryProcessCycleTime(GetCurrentProcess(), &c) ? c : 0;
#else
  return 0;
#endif
}

std::vector<ThreadCycles> threadCycles() {
  std::vector<ThreadCycles> out;
#ifdef _WIN32
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;
  THREADENTRY32 te{};
  te.dwSize = sizeof te;
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
    if (!th) continue;
    ULONG64 c = 0;
    QueryThreadCycleTime(th, &c);
    ThreadCycles t{te.th32ThreadID, c, {}};
    PWSTR name = nullptr;
    if (SUCCEEDED(GetThreadDescription(th, &name)) && name) {
      t.description = name;
      LocalFree(name);
    }
    // The module the thread started in (ntdll's NtQueryInformationThread, class 9 = Win32 start address), so an
    // idle report can tell our threads from the GPU driver's.
    using QueryFn = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<QueryFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    void* start = nullptr;
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH];
    if (query && query(th, 9, &start, sizeof start, nullptr) == 0 && start &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(start), &mod) &&
        GetModuleFileNameW(mod, path, MAX_PATH)) {
      const wchar_t* leaf = path;
      for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') leaf = p + 1;
      t.module = leaf;
    }
    CloseHandle(th);
    out.push_back(std::move(t));
  }
  CloseHandle(snap);
#endif
  return out;
}

std::wstring processModuleName() {
#ifdef _WIN32
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  const wchar_t* leaf = path;
  for (const wchar_t* q = path; *q; ++q)
    if (*q == L'\\' || *q == L'/') leaf = q + 1;
  return leaf;
#else
  return {};
#endif
}

uint64_t currentThreadCycles() noexcept {
#ifdef _WIN32
  ULONG64 c = 0;
  return QueryThreadCycleTime(GetCurrentThread(), &c) ? c : 0;
#else
  return 0;
#endif
}

uint64_t tscNow() noexcept {
#ifdef _WIN32
  return __rdtsc();
#else
  return 0;
#endif
}

double processCpuMs() noexcept {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
  return static_cast<double>(fileTimeTicks(kernel) + fileTimeTicks(user)) * 1e-4;
#else
  return -1.0;
#endif
}

}  // namespace pq::app
