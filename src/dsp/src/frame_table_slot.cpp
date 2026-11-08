#include "pq/dsp/frame_table_slot.h"

#include <cassert>
#include <utility>

namespace pq::dsp {

FrameTableSlot::FrameTableSlot(std::shared_ptr<const FrameTable> initial, const VoiceTapConfig& tap)
    : tap_(std::make_shared<VoiceTap>(tap)) {
  assert(initial != nullptr);
  auto* e = new Entry;
  e->view = initial->view();
  e->generation = 1;
  e->table = std::move(initial);
  entry_.store(e, std::memory_order_release);
}

FrameTableSlot::~FrameTableSlot() {
  // No image reads the slot any more (they hold it alive): every entry still on the chain is ours.
  const Entry* e = entry_.load(std::memory_order_acquire);
  while (e != nullptr) {
    const Entry* older = e->prev.load(std::memory_order_relaxed);
    deleter_(const_cast<Entry*>(e));
    e = older;
  }
}

void FrameTableSlot::publish(std::shared_ptr<const FrameTable> table, EpochReclaimer& reclaimer) {
  assert(table != nullptr);
  std::lock_guard lock(writeMutex_);
  const auto now = std::chrono::steady_clock::now();
  auto* cur = const_cast<Entry*>(entry_.load(std::memory_order_relaxed));
  auto* e = new Entry;
  e->view = table->view();
  e->prev.store(cur, std::memory_order_relaxed);
  e->generation = cur->generation + 1;
  e->table = std::move(table);
  cur->superseded = now;
  entry_.store(e, std::memory_order_release);
  // AU.fold (header "Publication"): keep the newest and the one it replaced, then every entry superseded less than
  // kFrameTableKeepSeconds ago, at most kFrameTableMaxEntries in all; the first one past that is unlinked from its
  // newer neighbour and retired with everything older (the chain ends at the previous cut: those are ours alone).
  const auto keep = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(kFrameTableKeepSeconds));
  Entry* newer = cur;
  auto* x = const_cast<Entry*>(cur->prev.load(std::memory_order_relaxed));
  for (uint32_t kept = 2; x != nullptr; ++kept) {
    if (kept >= kFrameTableMaxEntries || now - x->superseded >= keep) {
      newer->prev.store(nullptr, std::memory_order_release);  // unreachable for every kernel that searches from now on
      while (x != nullptr) {
        auto* older = const_cast<Entry*>(x->prev.load(std::memory_order_relaxed));
        reclaimer.retire(x, deleter_);
        x = older;
      }
      break;
    }
    newer = x;
    x = const_cast<Entry*>(x->prev.load(std::memory_order_relaxed));
  }
}

std::shared_ptr<const FrameTable> FrameTableSlot::table() const {
  std::lock_guard lock(writeMutex_);
  return entry_.load(std::memory_order_relaxed)->table;
}

}  // namespace pq::dsp
