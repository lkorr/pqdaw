#pragma once

// The CPU baseline the prebuilt binaries are compiled for, and the startup check that enforces it.
//
// Everything pqdaw ships is built /arch:AVX2, which lets MSVC emit AVX, AVX2, FMA3, BMI1/BMI2, LZCNT, F16C and
// MOVBE anywhere (the x86-64-v3 feature set; LZCNT/TZCNT even decode silently as BSR/BSF on CPUs without
// them, giving wrong results rather than a crash). On a CPU without them the process would die with an
// illegal instruction, or worse, so executables check first (review M0 L9):
//
//   pq_add_executable links the `pq_cpu_check` object, which registers a CRT initialiser in section
//   .CRT$XCB. The CRT runs .CRT$XC* in name order and every C++ dynamic initialiser (static constructors,
//   ours and LLVM's) is in .CRT$XCU, so the check runs before any of them and before main(). If a baseline
//   feature is missing it prints which to stderr (and shows a message box when the process owns its console
//   or has none, i.e. was launched from Explorer) and exits with code 1.
//
// Guarantee: the check and the query below are C (src/core/baseline/*.c), compiled without /arch:AVX2 and
// without /GL (the C release flags have no /GL), so they share no inline/COMDAT code with /arch:AVX2 C++
// and link-time code generation cannot re-target them. They call only intrinsics and Win32 functions, which
// are plain x86-64. Code that runs before .CRT$XCB is the CRT's own startup (prebuilt, plain x86-64) and C
// initialisers (.CRT$XI*), of which pqdaw has none. Not covered: TLS callbacks and DLL entry points of
// loaded DLLs (pqdaw links its dependencies statically and loads no DLL with AVX2 code before main).
// Verified by disassembly: no VEX/BMI/LZCNT/MOVBE instruction in either object (T0.1-fix1 progress entry).
//
// A C interface on purpose: the functions are reachable from the C startup check.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The x86-64-v3 features /arch:AVX2 code may use. `avx` (and everything that needs AVX state) includes the
// OS check: XCR0 has the SSE and AVX state enabled.
typedef struct PqCpuBaseline {
  bool avx;
  bool avx2;
  bool fma;
  bool bmi1;
  bool bmi2;
  bool lzcnt;
  bool f16c;
  bool movbe;
} PqCpuBaseline;

// Test hook: the environment variable that forces baseline features off, as a comma-separated list of
// feature names ("avx2", "avx2,fma"; "all" for every one; case-insensitive). Read by pq_query_cpu_baseline(),
// so it also reaches pq::cpuFeatures() and therefore the startup check, the JIT target choice and the rest.
#define PQ_CPU_DISABLE_ENV_VAR "PQ_TEST_CPU_DISABLE"

// CPUID/XGETBV query with the PQ_TEST_CPU_DISABLE override applied. Safe on any x86-64 CPU.
PqCpuBaseline pq_query_cpu_baseline(void);
// Forces off the features named in `list` (the PQ_TEST_CPU_DISABLE syntax; unknown names are ignored, NULL or
// empty changes nothing).
void pq_apply_cpu_disable_list(PqCpuBaseline* b, const char* list);
// Writes the names of the missing features ("AVX2, FMA") into `out` (always NUL-terminated when n > 0,
// truncated if it does not fit) and returns how many are missing (0: the CPU meets the baseline).
int pq_missing_cpu_baseline(const PqCpuBaseline* b, char* out, size_t n);

#ifdef __cplusplus
}  // extern "C"
#endif
