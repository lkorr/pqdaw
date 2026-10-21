/* CPU baseline query (pq/core/cpu_baseline.h). C, compiled WITHOUT /arch:AVX2 and without /GL (pq_cpu_baseline
   in src/core/CMakeLists.txt): it runs from the startup check before anything else, possibly on a CPU that
   has no AVX at all. Only intrinsics, Win32 calls and file-local helpers. */

#include "pq/core/cpu_baseline.h"

#include <intrin.h>
#include <windows.h>

static bool bit(int reg, int n) { return (((unsigned)reg >> n) & 1u) != 0; }

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* Case-insensitive comparison of [s, s + len) with the NUL-terminated lower-case `name`. */
static bool tokenIs(const char* s, size_t len, const char* name) {
  size_t i = 0;
  for (; i < len && name[i] != '\0'; ++i)
    if (lower(s[i]) != name[i]) return false;
  return i == len && name[i] == '\0';
}

enum { kFeatureCount = 8 };
static const char* const kNames[kFeatureCount] = {"avx", "avx2", "fma", "bmi1", "bmi2", "lzcnt", "f16c", "movbe"};
static const char* const kLabels[kFeatureCount] = {"AVX", "AVX2", "FMA", "BMI1", "BMI2", "LZCNT", "F16C", "MOVBE"};

/* The flags in kNames order. */
static bool* flagAt(PqCpuBaseline* b, int i) {
  switch (i) {
    case 0: return &b->avx;
    case 1: return &b->avx2;
    case 2: return &b->fma;
    case 3: return &b->bmi1;
    case 4: return &b->bmi2;
    case 5: return &b->lzcnt;
    case 6: return &b->f16c;
    default: return &b->movbe;
  }
}

void pq_apply_cpu_disable_list(PqCpuBaseline* b, const char* list) {
  if (b == NULL || list == NULL) return;
  const char* p = list;
  while (*p != '\0') {
    while (*p == ',' || *p == ' ') ++p;
    const char* start = p;
    while (*p != '\0' && *p != ',' && *p != ' ') ++p;
    const size_t len = (size_t)(p - start);
    if (len == 0) continue;
    const bool all = tokenIs(start, len, "all");
    for (int i = 0; i < kFeatureCount; ++i)
      if (all || tokenIs(start, len, kNames[i])) *flagAt(b, i) = false;
  }
}

static void put(char* out, size_t n, size_t* w, const char* s) {
  for (; *s != '\0'; ++s)
    if (*w + 1 < n) out[(*w)++] = *s;
}

int pq_missing_cpu_baseline(const PqCpuBaseline* b, char* out, size_t n) {
  PqCpuBaseline copy = *b;
  int missing = 0;
  size_t w = 0;
  for (int i = 0; i < kFeatureCount; ++i) {
    if (*flagAt(&copy, i)) continue;
    if (missing++ > 0) put(out, n, &w, ", ");
    put(out, n, &w, kLabels[i]);
  }
  if (n > 0) out[w] = '\0';
  return missing;
}

PqCpuBaseline pq_query_cpu_baseline(void) {
  PqCpuBaseline b = {false, false, false, false, false, false, false, false};
  int r[4] = {0, 0, 0, 0};
  __cpuid(r, 0);
  const int maxLeaf = r[0];
  if (maxLeaf >= 1) {
    __cpuid(r, 1);
    const int ecx1 = r[2];
    /* AVX state must be enabled by the OS: XCR0 bits 1 (SSE) and 2 (AVX). XGETBV needs OSXSAVE. */
    const bool osAvx = bit(ecx1, 27) && (_xgetbv(0) & 0x6) == 0x6;
    b.avx = osAvx && bit(ecx1, 28);
    b.fma = b.avx && bit(ecx1, 12);
    b.f16c = b.avx && bit(ecx1, 29);
    b.movbe = bit(ecx1, 22);
    if (maxLeaf >= 7) {
      __cpuidex(r, 7, 0);
      b.avx2 = b.avx && bit(r[1], 5);
      b.bmi1 = bit(r[1], 3);
      b.bmi2 = bit(r[1], 8);
    }
  }
  __cpuid(r, (int)0x80000000u);
  if ((unsigned)r[0] >= 0x80000001u) {
    __cpuid(r, (int)0x80000001u);
    b.lzcnt = bit(r[2], 5); /* ABM / LZCNT */
  }

  char disable[256];
  const DWORD len = GetEnvironmentVariableA(PQ_CPU_DISABLE_ENV_VAR, disable, (DWORD)sizeof(disable));
  if (len > 0 && len < sizeof(disable)) pq_apply_cpu_disable_list(&b, disable);
  return b;
}
