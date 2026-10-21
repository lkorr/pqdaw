#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

#include "pq/core/aligned_buffer.h"
#include "pq/core/cache_line.h"

namespace pq {

// Bump allocator over one fixed block (ARCHITECTURE §3: all Program state lives in one arena).
// Every allocation starts on a 64-byte boundary by default, so separately allocated objects never share
// a cache line and SIMD loads are aligned. The block is allocated (and zeroed, which also pre-faults its
// pages) in the constructor; allocate() never touches the heap and returns nullptr when out of space.
// Nothing is freed individually: reset() or rewind() reclaims in bulk and runs no destructors, so only
// put trivially destructible data (or data whose destructors you run yourself) in an arena.
// Single-threaded.
class Arena {
public:
  static constexpr std::size_t kDefaultAlign = kCacheLineSize;

  // Allocates: construct off the audio thread.
  explicit Arena(std::size_t capacityBytes)
      : block_((capacityBytes + kCacheLineSize - 1) / kCacheLineSize * kCacheLineSize) {}

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) noexcept = default;
  Arena& operator=(Arena&&) noexcept = default;

  // Returns `bytes` of uninitialised storage aligned to `align` (a power of two <= 64), or nullptr if
  // the arena is full. RT-safe.
  void* allocate(std::size_t bytes, std::size_t align = kDefaultAlign) noexcept {
    assert(std::has_single_bit(align) && align <= kCacheLineSize);
    const std::size_t start = (used_ + align - 1) & ~(align - 1);
    if (start > block_.size() || bytes > block_.size() - start) return nullptr;
    used_ = start + bytes;
    return block_.data() + start;
  }

  // Default-initialised array of n T (cache-line aligned); nullptr if full. RT-safe.
  template <class T>
  T* allocArray(std::size_t n) noexcept {
    static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
    static_assert(std::is_nothrow_default_constructible_v<T>);
    static_assert(alignof(T) <= kCacheLineSize);
    if (n > (block_.size() / sizeof(T))) return nullptr;
    void* p = allocate(n * sizeof(T), kDefaultAlign);
    if (!p) return nullptr;
    T* first = static_cast<T*>(p);
    std::uninitialized_default_construct_n(first, n);
    return first;
  }

  // Bulk reclamation. RT-safe.
  struct Marker {
    std::size_t used;
  };
  Marker mark() const noexcept { return {used_}; }
  void rewind(Marker m) noexcept {
    assert(m.used <= used_);
    used_ = m.used;
  }
  void reset() noexcept { used_ = 0; }

  std::size_t used() const noexcept { return used_; }
  std::size_t capacity() const noexcept { return block_.size(); }
  std::size_t remaining() const noexcept { return block_.size() - used_; }

private:
  struct alignas(kCacheLineSize) Line {
    std::byte bytes[kCacheLineSize];
  };
  // Byte view over a cache-line aligned block.
  struct Block {
    explicit Block(std::size_t bytes) : lines(bytes / kCacheLineSize) {}
    std::byte* data() noexcept { return lines.empty() ? nullptr : lines.data()->bytes; }
    std::size_t size() const noexcept { return lines.size() * kCacheLineSize; }
    AlignedBuffer<Line> lines;
  };

  Block block_;
  std::size_t used_ = 0;
};

}  // namespace pq
