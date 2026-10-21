#include "pq/core/cpu.h"

#include <array>
#include <cstring>
#include <intrin.h>

namespace pq {
namespace {

CpuFeatures detect() {
  CpuFeatures f;
  std::array<int, 4> r{};

  __cpuid(r.data(), 0);
  const int maxLeaf = r[0];

  if (maxLeaf >= 1) {
    __cpuid(r.data(), 1);
    f.sse41 = (r[2] & (1 << 19)) != 0;
    f.fma = (r[2] & (1 << 12)) != 0;
    const bool osxsave = (r[2] & (1 << 27)) != 0;
    // AVX state must be enabled by the OS (XCR0 bits 1-2), AVX-512 additionally needs bits 5-7.
    const unsigned long long xcr0 = osxsave ? _xgetbv(0) : 0;
    const bool osAvx = (xcr0 & 0x6) == 0x6;
    const bool osAvx512 = (xcr0 & 0xE6) == 0xE6;

    if (maxLeaf >= 7) {
      __cpuidex(r.data(), 7, 0);
      f.avx2 = osAvx && (r[1] & (1 << 5)) != 0;
      f.avx512f = osAvx512 && (r[1] & (1 << 16)) != 0;
      f.avx512dq = osAvx512 && (r[1] & (1 << 17)) != 0;
      f.avx512bw = osAvx512 && (r[1] & (1 << 30)) != 0;
      f.avx512vl = osAvx512 && (r[1] & (1 << 31)) != 0;
    }
    f.fma = f.fma && osAvx;
  }

  __cpuid(r.data(), 0x80000000);
  if (static_cast<unsigned>(r[0]) >= 0x80000004u) {
    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
      __cpuid(r.data(), 0x80000002 + i);
      std::memcpy(brand + i * 16, r.data(), 16);
    }
    f.brand = brand;
    const auto first = f.brand.find_first_not_of(' ');
    f.brand = first == std::string::npos ? std::string{} : f.brand.substr(first);
  }
  return f;
}

}  // namespace

const CpuFeatures& cpuFeatures() {
  static const CpuFeatures features = detect();
  return features;
}

}  // namespace pq
