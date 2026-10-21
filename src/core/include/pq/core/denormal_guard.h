#pragma once

#include <immintrin.h>

namespace pq {

// RAII: sets FTZ (flush-to-zero) and DAZ (denormals-are-zero) in MXCSR for the current thread and
// restores the previous MXCSR on destruction (SPEC §13: denormals flushed on audio threads).
// Open one at the top of every audio callback / worker loop that runs DSP. Each MXCSR access costs a
// few cycles: once per block, not per sample. Affects SSE/AVX code only (we generate no x87 code on x64).
class DenormalGuard {
public:
  static constexpr unsigned kFtz = 0x8000;  // MXCSR bit 15
  static constexpr unsigned kDaz = 0x0040;  // MXCSR bit 6

  DenormalGuard() noexcept : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | kFtz | kDaz); }  // RT-safe
  ~DenormalGuard() noexcept { _mm_setcsr(saved_); }                                       // RT-safe

  DenormalGuard(const DenormalGuard&) = delete;
  DenormalGuard& operator=(const DenormalGuard&) = delete;

  // True if FTZ and DAZ are both set on the calling thread right now. RT-safe.
  static bool active() noexcept { return (_mm_getcsr() & (kFtz | kDaz)) == (kFtz | kDaz); }

private:
  unsigned saved_;
};

}  // namespace pq
