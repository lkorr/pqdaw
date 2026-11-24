// OP.0 (docs/plans/OP.md): a sampling profiler for ANOTHER process of the same user (no admin rights, no ETW): which
// functions the busy threads of a running pqdaw are in. For the UI thread and the device's audio thread of the real
// app, where pq_bench_scenarios' in-process sampler does not reach.
//
//   pq_sample_process <pid> <seconds> <out file> [min thread CPU %, default 0.5]
//
// 1. Measures every thread's CPU over one second (GetThreadTimes) and keeps those at or above the threshold.
// 2. For <seconds>: suspends each kept thread ~1000 times a second (jittered), reads its instruction pointer, resumes.
// 3. Appends to <out file>, per thread, "# thread <id> <cpu %> <description>" and the counts per address in the format
//    tools/bench/symbolize_samples.py reads: "E <rva>" in the process's executable (resolve with its linker map: link
//    pqdaw with /MAP), "M <dll> <rva>", "J <address>" outside every module (JIT code).
// A waiting thread's samples are in ntdll / win32u (the wait's system call); kernel time spent inside a system call
// shows there too, so the tool also prints each thread's kernel and user CPU over the sampling.
// Only ever attach to a process you started.
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Module {
  uint64_t base = 0, size = 0;
  std::string name;
};

struct Thread {
  DWORD id = 0;
  HANDLE h = nullptr;
  double cpuPct = 0;
  uint64_t cpu0 = 0, kernel0 = 0;  // at the start of the sampling
  std::string description;
  std::vector<uint64_t> ips;
};

uint64_t cpu100ns(HANDLE h, uint64_t* kernel = nullptr) {
  FILETIME c, e, k, u;
  if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
  auto v = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  if (kernel != nullptr) *kernel = v(k);
  return v(k) + v(u);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: pq_sample_process <pid> <seconds> <out file> [min thread CPU %%]\n");
    return 2;
  }
  const DWORD pid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));
  const double seconds = std::strtod(argv[2], nullptr);
  const double minPct = argc > 4 ? std::strtod(argv[4], nullptr) : 0.5;
  HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (proc == nullptr) {
    std::fprintf(stderr, "cannot open process %lu\n", pid);
    return 1;
  }
  std::vector<Module> modules;
  {
    HMODULE mods[2048];
    DWORD need = 0;
    if (EnumProcessModules(proc, mods, sizeof mods, &need)) {
      for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 2048; ++i) {
        MODULEINFO mi{};
        char name[MAX_PATH] = {};
        if (!GetModuleInformation(proc, mods[i], &mi, sizeof mi)) continue;
        GetModuleBaseNameA(proc, mods[i], name, MAX_PATH);
        modules.push_back({reinterpret_cast<uint64_t>(mi.lpBaseOfDll), mi.SizeOfImage, name});
      }
    }
  }
  if (modules.empty()) {
    std::fprintf(stderr, "cannot list the modules of process %lu\n", pid);
    return 1;
  }
  const uint64_t exeBase = modules.front().base;  // the executable is listed first
  std::vector<Thread> threads;
  {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
      if (h == nullptr) continue;
      Thread t;
      t.id = te.th32ThreadID;
      t.h = h;
      PWSTR d = nullptr;
      if (SUCCEEDED(GetThreadDescription(h, &d)) && d != nullptr) {
        for (const wchar_t* p = d; *p != 0; ++p) t.description += static_cast<char>(*p < 128 ? *p : '?');
        LocalFree(d);
      }
      threads.push_back(std::move(t));
    }
    CloseHandle(snap);
  }
  std::vector<uint64_t> before;
  for (const Thread& t : threads) before.push_back(cpu100ns(t.h));
  Sleep(1000);
  std::vector<Thread> kept;
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].cpuPct = static_cast<double>(cpu100ns(threads[i].h) - before[i]) / 1e7 * 100.0;
    if (threads[i].cpuPct >= minPct) kept.push_back(std::move(threads[i]));
    else CloseHandle(threads[i].h);
  }
  std::printf("%zu of %zu threads at or above %.2f %% CPU\n", kept.size(), threads.size(), minPct);
  for (Thread& t : kept) {
    t.ips.reserve(static_cast<size_t>(seconds * 1200) + 64);
    t.cpu0 = cpu100ns(t.h, &t.kernel0);
  }
  LARGE_INTEGER f, now;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&now);
  const int64_t end = now.QuadPart + static_cast<int64_t>(seconds * static_cast<double>(f.QuadPart));
  uint32_t rng = 1;
  CONTEXT ctx;
  while (now.QuadPart < end && !kept.empty()) {
    rng = rng * 1664525u + 1013904223u;
    Sleep(0);
    const int64_t next = now.QuadPart + f.QuadPart / 2000 + static_cast<int64_t>((rng >> 8) % static_cast<uint32_t>(f.QuadPart / 1000));
    do {
      YieldProcessor();
      QueryPerformanceCounter(&now);
    } while (now.QuadPart < next);
    for (Thread& t : kept) {
      if (t.ips.size() == t.ips.capacity()) continue;
      if (SuspendThread(t.h) == static_cast<DWORD>(-1)) continue;
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(t.h, &ctx)) t.ips.push_back(ctx.Rip);
      ResumeThread(t.h);
    }
  }
  FILE* out = std::fopen(argv[3], "a");
  if (out == nullptr) return 1;
  for (Thread& t : kept) {
    std::map<std::string, uint64_t> counts;
    for (const uint64_t ip : t.ips) {
      char buf[400];
      const Module* m = nullptr;
      for (const Module& k : modules)
        if (ip >= k.base && ip < k.base + k.size) m = &k;
      if (m == nullptr) std::snprintf(buf, sizeof buf, "J %llx", static_cast<unsigned long long>(ip));
      else if (m->base == exeBase) std::snprintf(buf, sizeof buf, "E %llx", static_cast<unsigned long long>(ip - m->base));
      else std::snprintf(buf, sizeof buf, "M %s %llx", m->name.c_str(), static_cast<unsigned long long>(ip - m->base));
      ++counts[buf];
    }
    std::fprintf(out, "# thread %lu %.1f%% %s\t%zu\n", t.id, t.cpuPct, t.description.c_str(), t.ips.size());
    for (const auto& [k, n] : counts) std::fprintf(out, "%s\t%llu\n", k.c_str(), static_cast<unsigned long long>(n));
    uint64_t kernel1 = 0;
    const uint64_t cpu1 = cpu100ns(t.h, &kernel1);
    std::printf("thread %lu: %.2f %% CPU while sampled (kernel %.2f %%, user %.2f %%), %zu samples %s\n", t.id,
                static_cast<double>(cpu1 - t.cpu0) / (seconds * 1e7) * 100.0, static_cast<double>(kernel1 - t.kernel0) / (seconds * 1e7) * 100.0,
                static_cast<double>((cpu1 - t.cpu0) - (kernel1 - t.kernel0)) / (seconds * 1e7) * 100.0, t.ips.size(), t.description.c_str());
    CloseHandle(t.h);
  }
  std::fclose(out);
  CloseHandle(proc);
  return 0;
}
