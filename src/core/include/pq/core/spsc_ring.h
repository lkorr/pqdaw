#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "pq/core/aligned_buffer.h"
#include "pq/core/cache_line.h"

namespace pq {

// Bounded lock-free single-producer/single-consumer ring (ARCHITECTURE §3, SPEC §13).
//
// - Capacity is a power of two; indices are free-running uint32 counters masked into the buffer, so
//   "full" is head - tail == capacity and every slot is usable.
// - Only acquire/release ordering. The producer-owned and consumer-owned indices sit on separate cache
//   lines, and each side keeps a private cached copy of the other side's index, so in the common case a
//   push or pop touches no cache line the other thread writes.
// - Exactly one thread may push and exactly one (possibly different) thread may pop at any time.
// - T must be default-constructible (slots are pre-built) and nothrow move-assignable (copy push needs
//   nothrow copy assignment). Popped slots
//   keep their moved-from value until overwritten; prefer trivially copyable T on RT paths.
// - HotLineSeparation: alignment of the producer-written and consumer-written index blocks (and of the
//   read-only block after them). The default (kHotLineSeparation, 128) keeps them out of each other's
//   adjacent-line prefetch pair; 64 exists for the benchmark comparison (bench/core/bench_rt_primitives.cpp).
template <class T, std::size_t HotLineSeparation = kHotLineSeparation>
class SpscRing {
  static_assert(HotLineSeparation >= kCacheLineSize && std::has_single_bit(HotLineSeparation),
                "SpscRing: HotLineSeparation must be a power of two >= the cache line size");
  static_assert(std::is_default_constructible_v<T>, "SpscRing<T>: T must be default-constructible");
  static_assert(std::is_nothrow_move_assignable_v<T>, "SpscRing<T>: T move assignment must be noexcept (RT-safe)");

public:
  // Allocates: construct off the audio thread. capacityPow2 must be a power of two in [2, 2^31].
  explicit SpscRing(uint32_t capacityPow2)
      : capacity_(capacityPow2), mask_(capacityPow2 - 1), slots_(capacityPow2) {
    assert(capacityPow2 >= 2 && capacityPow2 <= (1u << 31) && std::has_single_bit(capacityPow2));
  }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer side. Returns false (and leaves v untouched) if the ring is full. RT-safe.
  bool tryPush(const T& v) noexcept
    requires std::is_nothrow_copy_assignable_v<T>
  {
    return emplace(v);
  }
  bool tryPush(T&& v) noexcept { return emplace(std::move(v)); }

  // Consumer side. Returns false if the ring is empty. RT-safe.
  bool tryPop(T& out) noexcept {
    const uint32_t tail = c_.tail.load(std::memory_order_relaxed);
    if (tail == c_.cachedHead) {
      c_.cachedHead = p_.head.load(std::memory_order_acquire);
      if (tail == c_.cachedHead) return false;
    }
    out = std::move(slots_[tail & mask_]);
    c_.tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer side: pointer to the front element without removing it, or nullptr if empty. RT-safe.
  T* front() noexcept {
    const uint32_t tail = c_.tail.load(std::memory_order_relaxed);
    if (tail == c_.cachedHead) {
      c_.cachedHead = p_.head.load(std::memory_order_acquire);
      if (tail == c_.cachedHead) return nullptr;
    }
    return &slots_[tail & mask_];
  }

  // Consumer side: removes the front element (only after front() returned non-null). RT-safe.
  void popFront() noexcept {
    const uint32_t tail = c_.tail.load(std::memory_order_relaxed);
    assert(tail != p_.head.load(std::memory_order_acquire));
    c_.tail.store(tail + 1, std::memory_order_release);
  }

  // Approximate fill level; exact when called from either side while the other is idle. RT-safe.
  uint32_t sizeApprox() const noexcept {
    return p_.head.load(std::memory_order_acquire) - c_.tail.load(std::memory_order_acquire);
  }
  bool emptyApprox() const noexcept { return sizeApprox() == 0; }
  uint32_t capacity() const noexcept { return capacity_; }

private:
  template <class U>
  bool emplace(U&& v) noexcept {
    const uint32_t head = p_.head.load(std::memory_order_relaxed);
    if (head - p_.cachedTail == capacity_) {
      p_.cachedTail = c_.tail.load(std::memory_order_acquire);
      if (head - p_.cachedTail == capacity_) return false;
    }
    slots_[head & mask_] = std::forward<U>(v);
    p_.head.store(head + 1, std::memory_order_release);
    return true;
  }

  struct alignas(HotLineSeparation) ProducerSide {
    std::atomic<uint32_t> head{0};  // next slot to write; published to the consumer
    uint32_t cachedTail = 0;        // producer's last view of c_.tail
  };
  struct alignas(HotLineSeparation) ConsumerSide {
    std::atomic<uint32_t> tail{0};  // next slot to read; published to the producer
    uint32_t cachedHead = 0;        // consumer's last view of p_.head
  };

  ProducerSide p_;
  ConsumerSide c_;
  // Read-only after construction (shared, never written: no false sharing with the lines above).
  alignas(HotLineSeparation) const uint32_t capacity_;
  const uint32_t mask_;
  AlignedBuffer<T> slots_;
};

}  // namespace pq
