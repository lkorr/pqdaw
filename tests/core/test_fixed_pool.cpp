#include <doctest/doctest.h>

#include <cstdint>
#include <set>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/fixed_pool.h"

using pq::alloc_trap::countRtViolations;

namespace {
int gLive = 0;
struct Tracked {
  explicit Tracked(int v) noexcept : value(v) { ++gLive; }
  ~Tracked() { --gLive; }
  int value;
  double pad[3]{};
};
}  // namespace

TEST_CASE("fixed pool: create until exhausted, destroy, reuse") {
  pq::FixedPool<Tracked> pool(8);
  CHECK(pool.capacity() == 8);
  CHECK(pool.available() == 8);

  std::vector<Tracked*> objs;
  std::set<Tracked*> distinct;
  for (int i = 0; i < 8; ++i) {
    Tracked* t = pool.create(i);
    REQUIRE(t != nullptr);
    CHECK(t->value == i);
    CHECK(pool.owns(t));
    CHECK(reinterpret_cast<uintptr_t>(t) % alignof(Tracked) == 0);
    objs.push_back(t);
    distinct.insert(t);
  }
  CHECK(distinct.size() == 8);
  CHECK(pool.create(100) == nullptr);
  CHECK(pool.inUse() == 8);
  CHECK(gLive == 8);

  Tracked* freed = objs[3];
  pool.destroy(freed);
  CHECK(gLive == 7);
  CHECK(pool.available() == 1);
  Tracked* again = pool.create(42);
  CHECK(again == freed);  // LIFO: the most recently freed slot comes back first
  CHECK(again->value == 42);

  Tracked outside(0);
  CHECK_FALSE(pool.owns(&outside));
}

TEST_CASE("fixed pool: destructor destroys live objects only") {
  gLive = 0;
  {
    pq::FixedPool<Tracked> pool(4);
    Tracked* a = pool.create(1);
    pool.create(2);
    pool.destroy(a);
    CHECK(gLive == 1);
  }
  CHECK(gLive == 0);
}

TEST_CASE("fixed pool: create/destroy do not allocate (RtScope)") {
  pq::FixedPool<Tracked> pool(32);
  Tracked* objs[32] = {};
  bool ok = true;
  CHECK(countRtViolations([&] {
          for (int round = 0; round < 10; ++round) {
            for (int i = 0; i < 32; ++i) ok &= (objs[i] = pool.create(i)) != nullptr;
            ok &= pool.create(0) == nullptr;
            for (int i = 0; i < 32; ++i) pool.destroy(objs[i]);
          }
        }) == 0);
  CHECK(ok);
  CHECK(pool.available() == 32);
}
