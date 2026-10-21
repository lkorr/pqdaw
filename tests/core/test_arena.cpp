#include <doctest/doctest.h>

#include <cstdint>

#include "pq/core/alloc_trap.h"
#include "pq/core/arena.h"

using pq::alloc_trap::countRtViolations;

namespace {
bool aligned(const void* p, std::size_t a) { return reinterpret_cast<uintptr_t>(p) % a == 0; }
}  // namespace

TEST_CASE("arena: 64-byte aligned bump allocation until full") {
  pq::Arena arena(1000);
  CHECK(arena.capacity() == 1024);  // rounded up to whole cache lines
  void* a = arena.allocate(1);
  void* b = arena.allocate(1);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK(aligned(a, 64));
  CHECK(aligned(b, 64));
  CHECK(static_cast<char*>(b) - static_cast<char*>(a) == 64);  // separate cache lines

  // Smaller alignment packs tighter.
  void* c = arena.allocate(4, 4);
  void* d = arena.allocate(4, 4);
  CHECK(static_cast<char*>(d) - static_cast<char*>(c) == 4);

  // Exhaustion returns nullptr and leaves the arena usable.
  CHECK(arena.allocate(2000) == nullptr);
  CHECK(arena.allocate(arena.remaining() + 1, 1) == nullptr);
  CHECK(arena.allocate(arena.remaining(), 1) != nullptr);
  CHECK(arena.remaining() == 0);
  CHECK(arena.allocate(1, 1) == nullptr);

  arena.reset();
  CHECK(arena.used() == 0);
  CHECK(arena.allocate(1) == a);
}

TEST_CASE("arena: typed arrays, mark and rewind") {
  pq::Arena arena(4096);
  float* f = arena.allocArray<float>(100);
  REQUIRE(f != nullptr);
  CHECK(aligned(f, 64));
  for (int i = 0; i < 100; ++i) f[i] = static_cast<float>(i);

  const auto m = arena.mark();
  int* scratch = arena.allocArray<int>(200);
  REQUIRE(scratch != nullptr);
  CHECK(arena.allocArray<double>(1u << 20) == nullptr);
  arena.rewind(m);
  CHECK(arena.allocArray<int>(1) == scratch);
  CHECK(f[99] == 99.0f);

  // Overflow of n * sizeof(T) is rejected rather than wrapped.
  CHECK(arena.allocArray<double>(SIZE_MAX / 4) == nullptr);
}

TEST_CASE("arena: allocate/allocArray/rewind/reset do not allocate (RtScope)") {
  pq::Arena arena(1 << 16);
  bool ok = true;
  CHECK(countRtViolations([&] {
          for (int round = 0; round < 100; ++round) {
            const auto m = arena.mark();
            ok &= arena.allocate(100) != nullptr;
            ok &= arena.allocArray<float>(256) != nullptr;
            arena.rewind(m);
          }
          arena.reset();
        }) == 0);
  CHECK(ok);
}
