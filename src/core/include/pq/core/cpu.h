#pragma once

#include <string>

namespace pq {

// Host CPU capabilities, queried once at startup (not RT-relevant).
// The prebuilt binaries require AVX2; the JIT may use AVX-512 when present.
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
