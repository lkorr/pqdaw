#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "pq/core/aligned_buffer.h"
#include "pq/core/cache_line.h"

namespace pq {

// Bounded lock-free multi-producer/single-consumer queue (Vyukov's bounded queue with a per-cell
// sequence number, specialised for one consumer so the pop side needs no CAS).
//
// - Producers claim a slot with one CAS on the shared enqueue position, write the value, then publish
//   it by storing the cell's sequence (release). A failed CAS retries with the fresh position, so
//   tryPush is lock-free (some producer always progresses) but not wait-free. Producers are expected to
//   be non-RT threads; the consumer is typically the audio thread and its tryPop is wait-free.
// - FIFO per producer. Across producers, order is the order of slot claims.
// - A producer that is pre-empted between claiming and publishing a slot makes tryPop report "empty"
//   until it resumes (later items are not skipped). The consumer never blocks on it.
// - Positions are uint64, so wrap-around is not a practical concern.
template <class T>
class MpscQueue {
  static_assert(std::is_default_constructible_v<T>, "MpscQueue<T>: T must be default-constructible");
  static_assert(std::is_nothrow_move_assignable_v<T>, "MpscQueue<T>: T move assignment must be noexcept (RT-safe)");

public:
  // Allocates: construct off the audio thread. capacityPow2 must be a power of two >= 2.
  explicit MpscQueue(uint32_t capacityPow2) : mask_(capacityPow2 - 1), cells_(capacityPow2) {
    assert(capacityPow2 >= 2 && std::has_single_bit(capacityPow2));
    for (uint32_t i = 0; i < capacityPow2; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }

  MpscQueue(const MpscQueue&) = delete;
  MpscQueue& operator=(const MpscQueue&) = delete;

  // Any number of producer threads. Returns false if the queue is full. No allocation, no locks; the CAS
  // retry is lock-free but not wait-free, so the audio thread should send its own traffic on an SpscRing.
  bool tryPush(const T& v) noexcept
    requires std::is_nothrow_copy_assignable_v<T>
  {
    return emplace(v);
  }
  bool tryPush(T&& v) noexcept { return emplace(std::move(v)); }

  // Single consumer thread. Returns false if empty (or the next item is not yet published). RT-safe.
  bool tryPop(T& out) noexcept {
    const uint64_t pos = dequeuePos_;
    Cell& cell = cells_[pos & mask_];
    if (cell.seq.load(std::memory_order_acquire) != pos + 1) return false;
    out = std::move(cell.value);
    // Free the cell for the producer one lap ahead.
    cell.seq.store(pos + mask_ + 1, std::memory_order_release);
    dequeuePos_ = pos + 1;
    return true;
  }

  uint32_t capacity() const noexcept { return static_cast<uint32_t>(mask_ + 1); }

private:
  template <class U>
  bool emplace(U&& v) noexcept {
    uint64_t pos = enqueuePos_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& cell = cells_[pos & mask_];
      const uint64_t seq = cell.seq.load(std::memory_order_acquire);
      const int64_t diff = static_cast<int64_t>(seq - pos);
      if (diff == 0) {
        if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          cell.value = std::forward<U>(v);
          cell.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
        // CAS failure reloaded pos; retry.
      } else if (diff < 0) {
        return false;  // the cell still holds an item from the previous lap: full
      } else {
        pos = enqueuePos_.load(std::memory_order_relaxed);  // another producer took this slot
      }
    }
  }

  struct Cell {
    std::atomic<uint64_t> seq{0};
    T value{};
  };

  alignas(kCacheLineSize) std::atomic<uint64_t> enqueuePos_{0};  // shared by producers
  alignas(kCacheLineSize) uint64_t dequeuePos_ = 0;              // consumer-owned
  alignas(kCacheLineSize) const uint64_t mask_;
  AlignedBuffer<Cell> cells_;
};

}  // namespace pq
