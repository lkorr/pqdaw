#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "pq/core/aligned_buffer.h"

namespace pq {

// Fixed-capacity object pool: storage for `capacity` objects of T, allocated once in the constructor.
// create()/destroy() are O(1) and RT-safe: a LIFO stack of free slot indices (the most recently freed,
// cache-warm slot is reused first). Single-threaded: one owning thread (typically the audio thread)
// creates and destroys; hand objects to other threads through a queue, not by sharing the pool.
template <class T>
class FixedPool {
public:
  // Allocates: construct off the audio thread.
  explicit FixedPool(uint32_t capacity) : slots_(capacity), freeList_(capacity) {
    for (uint32_t i = 0; i < capacity; ++i) freeList_[i] = capacity - 1 - i;  // slot 0 on top
    freeCount_ = capacity;
  }

  // Destroys any objects still alive.
  ~FixedPool() {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      // Mark free slots, then destroy every slot not marked.
      AlignedBuffer<uint8_t> isFree(slots_.size());
      for (uint32_t i = 0; i < freeCount_; ++i) isFree[freeList_[i]] = 1;
      for (std::size_t i = 0; i < slots_.size(); ++i)
        if (!isFree[i]) std::launder(reinterpret_cast<T*>(&slots_[i]))->~T();
    }
  }

  FixedPool(const FixedPool&) = delete;
  FixedPool& operator=(const FixedPool&) = delete;

  // Constructs a T in a free slot; returns nullptr if the pool is exhausted. RT-safe provided T's
  // constructor is (it must be noexcept).
  template <class... Args>
  T* create(Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible_v<T, Args...>, "FixedPool: T's constructor must be noexcept");
    if (freeCount_ == 0) return nullptr;
    const uint32_t idx = freeList_[--freeCount_];
    return ::new (static_cast<void*>(&slots_[idx])) T(std::forward<Args>(args)...);
  }

  // Destroys *p and returns its slot. p must come from this pool's create(). RT-safe.
  void destroy(T* p) noexcept {
    assert(owns(p));
    assert(freeCount_ < slots_.size());
    p->~T();
    freeList_[freeCount_++] = indexOf(p);
  }

  bool owns(const T* p) const noexcept {
    const auto* b = reinterpret_cast<const std::byte*>(slots_.data());
    const auto* q = reinterpret_cast<const std::byte*>(p);
    return q >= b && q < b + slots_.size() * sizeof(Slot) &&
           static_cast<std::size_t>(q - b) % sizeof(Slot) == 0;
  }

  uint32_t indexOf(const T* p) const noexcept {
    return static_cast<uint32_t>(reinterpret_cast<const Slot*>(p) - slots_.data());
  }

  uint32_t capacity() const noexcept { return static_cast<uint32_t>(slots_.size()); }
  uint32_t available() const noexcept { return freeCount_; }
  uint32_t inUse() const noexcept { return capacity() - freeCount_; }

private:
  struct Slot {
    alignas(T) std::byte bytes[sizeof(T)];
  };
  static_assert(sizeof(Slot) == sizeof(T));

  AlignedBuffer<Slot> slots_;
  AlignedBuffer<uint32_t> freeList_;
  uint32_t freeCount_ = 0;
};

}  // namespace pq
