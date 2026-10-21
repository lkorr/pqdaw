/* Startup CPU baseline check (pq/core/cpu_baseline.h), linked into every pq_add_executable target as the
   `pq_cpu_check` object. C, compiled WITHOUT /arch:AVX2 and without /GL. Only Win32 calls and file-local
   helpers. */

#include <windows.h>

#include "pq/core/cpu_baseline.h"

static size_t length(const char* s) {
  size_t n = 0;
  while (s[n] != '\0') ++n;
  return n;
}

static void append(char* buf, size_t cap, size_t* w, const char* s) {
  for (; *s != '\0'; ++s)
    if (*w + 1 < cap) buf[(*w)++] = *s;
  buf[*w] = '\0';
}

/* True when nobody would see stderr: the process is the only one on its console (a console created for it,
   i.e. started from Explorer, which closes as soon as the process exits), or it has no console (GUI
   subsystem) and no redirected stderr. */
static bool wantsMessageBox(HANDLE err) {
  DWORD pids[2];
  const DWORD onConsole = GetConsoleProcessList(pids, 2);
  if (onConsole == 1) return true;
  return onConsole == 0 && (err == NULL || err == INVALID_HANDLE_VALUE);
}

static void __cdecl checkCpuBaseline(void) {
  const PqCpuBaseline b = pq_query_cpu_baseline();
  char missing[128];
  if (pq_missing_cpu_baseline(&b, missing, sizeof(missing)) == 0) return;

  char exe[MAX_PATH] = "pqdaw";
  const DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    size_t base = 0;
    for (size_t i = 0; exe[i] != '\0'; ++i)
      if (exe[i] == '\\' || exe[i] == '/') base = i + 1;
    size_t i = 0;
    for (; exe[base + i] != '\0'; ++i) exe[i] = exe[base + i];
    exe[i] = '\0';
  }

  char msg[768];
  size_t w = 0;
  msg[0] = '\0';
  append(msg, sizeof(msg), &w, exe);
  append(msg, sizeof(msg), &w,
         ": this CPU is not supported. pqdaw is built for x86-64 CPUs with AVX2 and FMA (x86-64-v3: Intel "
         "Haswell / 4th-generation Core or later, AMD Excavator / Zen or later). Missing: ");
  append(msg, sizeof(msg), &w, missing);
  append(msg, sizeof(msg), &w, ".\n");

  const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err != NULL && err != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(err, msg, (DWORD)length(msg), &written, NULL);
  }
  if (wantsMessageBox(err)) MessageBoxA(NULL, msg, "Unsupported CPU", MB_OK | MB_ICONERROR);
  ExitProcess(1);
}

/* The CRT calls the .CRT$XCA..XCZ function pointers in section-name order; C++ dynamic initialisers are in
   .CRT$XCU, so .CRT$XCB runs before every one of them. A C static initialiser is always a constant, so the
   pointer is in the image (no initialiser of its own). The /include keeps it from being discarded. */
typedef void(__cdecl* PqCrtInit)(void);
#pragma section(".CRT$XCB", long, read)
__declspec(allocate(".CRT$XCB")) const PqCrtInit pq_cpu_baseline_check = checkCpuBaseline;
#pragma comment(linker, "/include:pq_cpu_baseline_check")
