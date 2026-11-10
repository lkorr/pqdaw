#pragma once

#include <cstddef>
#include <memory_resource>
#include <type_traits>
#include <utility>
#include <vector>

#include "pq/core/cache_line.h"

namespace pq {

// PF.4b: the heap for what the audio thread reads every block but other threads build (an image's tables, buffers and
// object, a unit's packets). One private heap whose allocations are serialised, so where a block lands no longer
// depends on which thread asked or how many asked at once. The CRT heap gives contending threads regions of their own:
// with PF.4's parallel compiles, 64 units' images were spread over six regions laid out alike, and audio/64 rate 1
// cost +1.4 µs per block (bench/RESULTS.md, PF.4b).
// Thread-safe; not RT-safe: allocate and free off the audio thread. An allocation or free inside an RtScope is
// reported to the allocation trap like operator new / delete.
std::pmr::memory_resource* rtHeap() noexcept;
void* rtHeapAllocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t));
void rtHeapFree(void* p, std::size_t align = alignof(std::max_align_t)) noexcept;

// A table on the RT heap (construct it with rtHeap()).
template <class T>
using RtTable = std::pmr::vector<T>;

// A fixed array of n zeroed T on the RT heap, cache-line aligned (AlignedBuffer's counterpart). Element access is
// RT-safe.
template <class T>
class RtHeapArray {
public:
  RtHeapArray() noexcept = default;
  explicit RtHeapArray(std::size_t n) : size_(n) {
    static_assert(std::is_trivially_default_constructible_v<T> && std::is_trivially_destructible_v<T>);
    if (n == 0) return;
    data_ = static_cast<T*>(rtHeapAllocate(n * sizeof(T), kCacheLineSize));
    for (std::size_t i = 0; i < n; ++i) data_[i] = T{};
  }
  ~RtHeapArray() { rtHeapFree(data_, kCacheLineSize); }
  RtHeapArray(RtHeapArray&& o) noexcept : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}
  RtHeapArray& operator=(RtHeapArray&& o) noexcept {
    if (this != &o) {
      rtHeapFree(data_, kCacheLineSize);
      data_ = std::exchange(o.data_, nullptr);
      size_ = std::exchange(o.size_, 0);
    }
    return *this;
  }
  RtHeapArray(const RtHeapArray&) = delete;
  RtHeapArray& operator=(const RtHeapArray&) = delete;

  T* data() noexcept { return data_; }
  const T* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }

private:
  T* data_ = nullptr;
  std::size_t size_ = 0;
};

// Gives a class's objects RT-heap storage (cache-line aligned): `struct X : RtHeapObject { … }`.
struct RtHeapObject {
  static void* operator new(std::size_t n) { return rtHeapAllocate(n, kCacheLineSize); }
  static void operator delete(void* p) noexcept { rtHeapFree(p, kCacheLineSize); }
};

}  // namespace pq
