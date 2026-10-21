#include <doctest/doctest.h>

#include <type_traits>
#include <unordered_set>

#include "pq/core/stable_id.h"

namespace {
struct ATag;
struct BTag;
using AId = pq::StableId<ATag>;
using BId = pq::StableId<BTag>;
}  // namespace

TEST_CASE("stable id: default is invalid, allocator counts up from 1 and never repeats") {
  CHECK_FALSE(AId{}.valid());
  pq::StableIdAllocator<ATag> alloc;
  const AId a = alloc.next(), b = alloc.next(), c = alloc.next();
  CHECK(a.value == 1);
  CHECK(b.value == 2);
  CHECK(c.value == 3);
  CHECK(a < b);
  CHECK(a != c);
  CHECK(alloc.peek() == 4);
  alloc.reserveThrough(AId{10});
  CHECK(alloc.next().value == 11);
  alloc.reserveThrough(AId{5});  // never moves backwards
  CHECK(alloc.next().value == 12);
}

TEST_CASE("stable id: hashable, trivially copyable, ids of different tags are different types") {
  static_assert(std::is_trivially_copyable_v<AId>);
  static_assert(sizeof(AId) == 4);
  static_assert(!std::is_same_v<AId, BId>);
  static_assert(!std::is_convertible_v<AId, BId>);
  std::unordered_set<AId> set{AId{1}, AId{2}, AId{1}};
  CHECK(set.size() == 2);
}
