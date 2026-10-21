#pragma once

// Stable identifiers: a typed 32-bit id that survives edits, as opposed to a dense index that changes when items are
// removed. Used for graph nodes (hot-swap state transfer keyed by NodeId, SPEC §7.5) and later for tracks, clips and
// devices in the document model. The tag type makes ids of different things incomparable at compile time.
//
// Value 0 is reserved as "invalid", so a default-constructed id is never mistaken for a real one.

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace pq {

template <class Tag>
struct StableId {
  uint32_t value = 0;

  constexpr bool valid() const noexcept { return value != 0; }  // RT-safe
  friend constexpr auto operator<=>(StableId, StableId) noexcept = default;
};

// Hands out increasing ids, never reusing one. Not thread-safe (each owner allocates its own ids).
template <class Tag>
class StableIdAllocator {
public:
  StableIdAllocator() noexcept = default;

  // The next id. 2^32 − 1 ids per allocator: a patch that adds a node every microsecond takes 70 minutes of
  // continuous adds to run out, and ids are never reused, so exhaustion is a programming error.
  StableId<Tag> next() noexcept {
    assert(next_ != 0 && "StableIdAllocator exhausted");
    return StableId<Tag>{next_++};
  }
  // The id that next() would return (for copying an allocator's position into a rebuilt container).
  uint32_t peek() const noexcept { return next_; }
  // Never hand out ids ≤ `id` (after importing items with existing ids).
  void reserveThrough(StableId<Tag> id) noexcept {
    if (id.value >= next_) next_ = id.value + 1;
  }

  friend bool operator==(const StableIdAllocator&, const StableIdAllocator&) noexcept = default;

private:
  uint32_t next_ = 1;
};

}  // namespace pq

template <class Tag>
struct std::hash<pq::StableId<Tag>> {
  size_t operator()(pq::StableId<Tag> id) const noexcept { return std::hash<uint32_t>{}(id.value); }
};
