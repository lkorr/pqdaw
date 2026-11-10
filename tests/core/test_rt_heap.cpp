#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/rt_heap.h"

namespace {
struct alignas(64) Obj : pq::RtHeapObject {
  float v[40] = {};
};
}  // namespace

TEST_CASE("rt heap: alignment, zeroed arrays, tables, objects") {
  for (std::size_t align : {std::size_t{8}, std::size_t{16}, std::size_t{64}, std::size_t{128}}) {
    void* p = pq::rtHeapAllocate(100, align);
    CHECK(reinterpret_cast<uintptr_t>(p) % align == 0);
    pq::rtHeapFree(p, align);
  }
  pq::RtHeapArray<float> a(1000);
  REQUIRE(a.size() == 1000);
  CHECK(reinterpret_cast<uintptr_t>(a.data()) % 64 == 0);
  bool zero = true;
  for (std::size_t i = 0; i < a.size(); ++i) zero = zero && a.data()[i] == 0.0f;
  CHECK(zero);
  pq::RtHeapArray<float> b = std::move(a);
  CHECK(a.data() == nullptr);
  CHECK(b.size() == 1000);

  pq::RtTable<int> t{pq::rtHeap()};
  for (int i = 0; i < 1000; ++i) t.push_back(i);
  CHECK(t[999] == 999);
  pq::RtTable<int> u{pq::rtHeap()};
  u = t;  // same resource: a plain copy
  CHECK(u.size() == 1000);

  auto* o = new Obj();
  CHECK(reinterpret_cast<uintptr_t>(o) % 64 == 0);
  delete o;
}

TEST_CASE("rt heap: thread-safe") {
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int k = 0; k < 4; ++k)
    threads.emplace_back([&bad] {
      for (int i = 0; i < 2000; ++i) {
        pq::RtTable<uint64_t> v{pq::rtHeap()};
        v.assign(static_cast<std::size_t>(i % 37 + 1), static_cast<uint64_t>(i));
        for (uint64_t x : v) bad += x == static_cast<uint64_t>(i) ? 0 : 1;
      }
    });
  for (std::thread& t : threads) t.join();
  CHECK(bad.load() == 0);
}

TEST_CASE("rt heap: reading does not allocate; allocating and freeing are trapped (RtScope)") {
  pq::RtHeapArray<double> a(64);
  double sum = 0;
  CHECK(pq::alloc_trap::countRtViolations([&] {
          for (std::size_t i = 0; i < a.size(); ++i) sum += a.data()[i];
        }) == 0);
  CHECK(sum == 0.0);
  CHECK(pq::alloc_trap::countRtViolations([] { pq::RtHeapArray<float> tmp(16); }) == 2);
  CHECK(pq::alloc_trap::countRtViolations([] { pq::RtTable<int> v{pq::rtHeap()}; v.push_back(1); }) == 2);
}
