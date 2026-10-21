#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/cache_line.h"

namespace pq {

// Quiescent-state-based reclamation (QSBR) for objects the audio thread reads through atomically
// published pointers (SPEC §13: reclamation happens off the audio thread).
//
// Protocol:
//   writer (non-RT):  old = published.exchange(next);  reclaimer.retire(old);
//   reader (RT):      once per block, after it has dropped every pointer it loaded earlier:
//                     reclaimer.quiescent(id);
//   any non-RT thread, periodically: reclaimer.collect();   // runs deleters for safe objects
// An object retired at epoch e is freed once every online reader has announced a quiescent state after
// e. Readers that are not running (device stopped) call goOffline() so they do not hold reclamation up,
// and goOnline() before touching shared pointers again.
//
// RT side (quiescent/goOffline/goOnline): wait-free, no allocation, no locks; quiescent() is one acquire
// load of the global epoch and one release store to the reader's own cache line. Retire/collect use a
// mutex and a vector: non-RT only. Up to `maxReaders` readers (one audio thread in v1, a few later).
class EpochReclaimer {
public:
  using Deleter = void (*)(void*) noexcept;
  struct ReaderId {
    uint32_t index;
  };

  // Allocates: construct off the audio thread.
  explicit EpochReclaimer(uint32_t maxReaders = 1);
  // Frees everything still retired. All readers must have stopped reading.
  ~EpochReclaimer();

  EpochReclaimer(const EpochReclaimer&) = delete;
  EpochReclaimer& operator=(const EpochReclaimer&) = delete;

  // Non-RT. Registers a reader, online at the current epoch; nullopt if all slots are taken. Call it
  // before the reader thread first loads a shared pointer (e.g. before starting the audio stream).
  std::optional<ReaderId> registerReader();
  // Non-RT. Takes the reader offline and frees its slot.
  void unregisterReader(ReaderId id);

  // Reader thread: announces that it holds no references obtained before this call. RT-safe.
  void quiescent(ReaderId id) noexcept {
    slots_[id.index].observed.store(epoch_.load(std::memory_order_acquire), std::memory_order_release);
  }
  // Reader thread: stops participating until goOnline(); holds no references from here on. RT-safe.
  void goOffline(ReaderId id) noexcept {
    slots_[id.index].observed.store(kOffline, std::memory_order_release);
  }
  // Reader thread: resumes participating; call before loading any shared pointer. RT-safe.
  void goOnline(ReaderId id) noexcept {
    slots_[id.index].observed.store(epoch_.load(std::memory_order_acquire), std::memory_order_seq_cst);
    // Orders the store above before the reader's next pointer loads against collect()'s slot reads.
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  // Non-RT, any thread. p must already be unreachable for readers that pass a quiescent state from now
  // on (i.e. unpublished). deleter(p) runs later on a thread calling collect() (or in the destructor).
  void retire(void* p, Deleter deleter);
  template <class T>
  void retire(T* p) {
    retire(static_cast<void*>(p), [](void* q) noexcept { delete static_cast<T*>(q); });
  }

  // Non-RT, any thread. Frees every retired object that all online readers have passed; returns how many.
  std::size_t collect();

  // Non-RT. Number of retired objects not yet freed.
  std::size_t pending() const;
  // Current global epoch (diagnostics, tests).
  uint64_t epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }

private:
  static constexpr uint64_t kOffline = ~uint64_t{0};

  struct alignas(kCacheLineSize) Slot {
    std::atomic<uint64_t> observed{kOffline};  // last epoch the reader announced, or kOffline
    bool registered = false;                   // guarded by mutex_
  };
  struct Retired {
    void* ptr;
    Deleter deleter;
    uint64_t epoch;
  };

  alignas(kCacheLineSize) std::atomic<uint64_t> epoch_{1};
  AlignedBuffer<Slot> slots_;
  mutable std::mutex mutex_;  // guards retired_ and Slot::registered (non-RT only)
  std::vector<Retired> retired_;
};

}  // namespace pq
