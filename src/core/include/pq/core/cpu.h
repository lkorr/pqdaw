#pragma once

#include <string>

namespace pq {

// Host CPU capabilities, queried once at startup (not RT-relevant).
// The prebuilt binaries require AVX2 + FMA (the x86-64-v3 baseline, checked at startup before any AVX2 code
// runs: pq/core/cpu_baseline.h); the JIT may use AVX-512 when present.
struct CpuFeatures {
  bool sse41 = false;
  bool avx2 = false;
  bool fma = false;
  bool avx512f = false;
  bool avx512bw = false;
  bool avx512vl = false;
  bool avx512dq = false;
  std::string brand;
};

const CpuFeatures& cpuFeatures();

}  // namespace pq
