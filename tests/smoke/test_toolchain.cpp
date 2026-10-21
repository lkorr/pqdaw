#include <doctest/doctest.h>

#include "pq/core/cpu.h"

TEST_CASE("toolchain: C++20, MSVC, static release CRT") {
  CHECK(__cplusplus >= 202002L);
#if defined(_DLL) || defined(_DEBUG)
  FAIL("expected /MT (static release CRT) to match the prebuilt LLVM");
#endif
}

TEST_CASE("cpu: host supports the AVX2 baseline") {
  const auto& f = pq::cpuFeatures();
  MESSAGE("CPU: " << f.brand << " avx2=" << f.avx2 << " fma=" << f.fma << " avx512f=" << f.avx512f);
  CHECK(f.sse41);
  CHECK(f.avx2);
  CHECK(f.fma);
}
