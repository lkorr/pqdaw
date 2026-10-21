#include <doctest/doctest.h>

#include <cfloat>
#include <cstring>
#include <thread>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"

namespace {
// volatile: forces the multiply to happen at run time under the current MXCSR.
float mul(float a, float b) {
  volatile float x = a;
  volatile float y = b;
  return x * y;
}
}  // namespace

TEST_CASE("denormal guard: flushes inside, restores outside") {
  const float tiny = FLT_MIN;  // smallest normal
  REQUIRE_FALSE(pq::DenormalGuard::active());
  // Without the guard, FLT_MIN * 0.5 is a denormal (non-zero).
  CHECK(mul(tiny, 0.5f) != 0.0f);
  {
    pq::DenormalGuard guard;
    CHECK(pq::DenormalGuard::active());
    CHECK(mul(tiny, 0.5f) == 0.0f);  // FTZ: denormal results flush to zero
    volatile float denormal = 0;
    {
      // The smallest positive denormal, built from its bit pattern (no FP arithmetic involved).
      unsigned bits = 1;
      float d;
      static_assert(sizeof d == sizeof bits);
      std::memcpy(&d, &bits, sizeof d);
      denormal = d;
    }
    CHECK(mul(denormal, 1.0f) == 0.0f);  // DAZ: denormal inputs read as zero
    {
      pq::DenormalGuard nested;
      CHECK(pq::DenormalGuard::active());
    }
    CHECK(pq::DenormalGuard::active());  // the nested guard restored the outer state, not the default
  }
  CHECK_FALSE(pq::DenormalGuard::active());
  CHECK(mul(tiny, 0.5f) != 0.0f);
}

TEST_CASE("denormal guard: per-thread, RT-safe") {
  bool otherActive = true;
  {
    pq::DenormalGuard guard;
    std::thread t([&] { otherActive = pq::DenormalGuard::active(); });
    t.join();
  }
  CHECK_FALSE(otherActive);
  CHECK(pq::alloc_trap::countRtViolations([] { pq::DenormalGuard g; }) == 0);
}
