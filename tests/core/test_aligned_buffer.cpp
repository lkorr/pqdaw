#include <doctest/doctest.h>

#include <cstdint>
#include <numeric>
#include <utility>

#include "pq/core/alloc_trap.h"
#include "pq/core/aligned_buffer.h"

namespace {
struct alignas(128) Wide {
  float v[32];
};
int gCount = 0;
struct Counted {
  Counted() { ++gCount; }
  ~Counted() { --gCount; }
};
}  // namespace

TEST_CASE("aligned buffer: alignment, zero-init, access") {
  pq::AlignedBuffer<float> b(1000);
  CHECK(b.size() == 1000);
  CHECK(reinterpret_cast<uintptr_t>(b.data()) % 64 == 0);
  CHECK(std::accumulate(b.begin(), b.end(), 0.0f) == 0.0f);
  for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<float>(i);
  CHECK(b[999] == 999.0f);
  CHECK(b.span().size() == 1000);

  pq::AlignedBuffer<Wide> w(3);
  CHECK(reinterpret_cast<uintptr_t>(w.data()) % 128 == 0);
  CHECK(pq::AlignedBuffer<Wide>::kAlignment == 128);

  pq::AlignedBuffer<int> empty;
  CHECK(empty.empty());
  CHECK(empty.data() == nullptr);
}

TEST_CASE("aligned buffer: move transfers ownership; destructors run once") {
  gCount = 0;
  {
    pq::AlignedBuffer<Counted> a(5);
    CHECK(gCount == 5);
    pq::AlignedBuffer<Counted> b(std::move(a));
    CHECK(a.empty());
    CHECK(b.size() == 5);
    pq::AlignedBuffer<Counted> c(2);
    CHECK(gCount == 7);
    c = std::move(b);
    CHECK(gCount == 5);
  }
  CHECK(gCount == 0);
}

TEST_CASE("aligned buffer: element access does not allocate; construction does (RtScope)") {
  pq::AlignedBuffer<double> b(64);
  double sum = 0;
  CHECK(pq::alloc_trap::countRtViolations([&] {
          for (auto& x : b) x = 1.0;
          for (std::size_t i = 0; i < b.size(); ++i) sum += b[i];
        }) == 0);
  CHECK(sum == 64.0);
  // Constructing one on the RT path is exactly what the trap must catch.
  CHECK(pq::alloc_trap::countRtViolations([] { pq::AlignedBuffer<float> tmp(16); }) == 2);
}
