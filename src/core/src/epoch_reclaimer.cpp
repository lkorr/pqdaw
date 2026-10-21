#include "pq/core/epoch_reclaimer.h"

#include <algorithm>

namespace pq {

EpochReclaimer::EpochReclaimer(uint32_t maxReaders) : slots_(maxReaders) {}

EpochReclaimer::~EpochReclaimer() {
  for (const Retired& r : retired_) r.deleter(r.ptr);
}

std::optional<EpochReclaimer::ReaderId> EpochReclaimer::registerReader() {
  std::lock_guard lock(mutex_);
  for (uint32_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i].registered) {
      slots_[i].registered = true;
      slots_[i].observed.store(epoch_.load(std::memory_order_acquire), std::memory_order_seq_cst);
      return ReaderId{i};
    }
  }
  return std::nullopt;
}

void EpochReclaimer::unregisterReader(ReaderId id) {
  std::lock_guard lock(mutex_);
  slots_[id.index].observed.store(kOffline, std::memory_order_release);
  slots_[id.index].registered = false;
}

void EpochReclaimer::retire(void* p, Deleter deleter) {
  if (!p) return;
  // Pairs with the fence in goOnline(): a reader coming online after collect() saw it offline must
  // observe the caller's unpublishing store, which precedes this fence.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  // Epoch e = the value after the increment: a reader that loads >= e has synchronised with this
  // increment, so it can no longer reach p.
  const uint64_t e = epoch_.fetch_add(1, std::memory_order_seq_cst) + 1;
  std::lock_guard lock(mutex_);
  retired_.push_back({p, deleter, e});
}

std::size_t EpochReclaimer::collect() {
  // Take the batch first, then read the reader slots: every retire() in the batch happens-before the
  // slot reads (through the mutex), so a reader's announced epoch is compared against an increment it
  // could have seen.
  std::vector<Retired> batch;
  {
    std::lock_guard lock(mutex_);
    batch.swap(retired_);
  }
  if (batch.empty()) return 0;

  uint64_t safe = kOffline;  // no online reader: everything is safe
  for (uint32_t i = 0; i < slots_.size(); ++i)
    safe = std::min(safe, slots_[i].observed.load(std::memory_order_seq_cst));

  const auto keepEnd = std::partition(batch.begin(), batch.end(), [&](const Retired& r) { return r.epoch > safe; });
  const auto freed = static_cast<std::size_t>(batch.end() - keepEnd);
  for (auto it = keepEnd; it != batch.end(); ++it) it->deleter(it->ptr);

  if (keepEnd != batch.begin()) {
    std::lock_guard lock(mutex_);
    retired_.insert(retired_.end(), batch.begin(), keepEnd);
  }
  return freed;
}

std::size_t EpochReclaimer::pending() const {
  std::lock_guard lock(mutex_);
  return retired_.size();
}

}  // namespace pq
