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
  // No image reads the slot any more (they hold it alive): the current entry and the one it replaced are ours.
  const Entry* e = entry_.load(std::memory_order_acquire);
  if (e == nullptr) return;
  if (e->prev != nullptr) deleter_(const_cast<Entry*>(e->prev));
  deleter_(const_cast<Entry*>(e));
}

void FrameTableSlot::publish(std::shared_ptr<const FrameTable> table, EpochReclaimer& reclaimer) {
  assert(table != nullptr);
  std::lock_guard lock(writeMutex_);
  const Entry* cur = entry_.load(std::memory_order_relaxed);
  auto* e = new Entry;
  e->view = table->view();
  e->prev = cur;
  e->generation = cur->generation + 1;
  e->table = std::move(table);
  entry_.store(e, std::memory_order_release);
  // cur->prev is unreachable for every kernel that loads the slot from now on (see the header).
  if (cur->prev != nullptr) reclaimer.retire(const_cast<Entry*>(cur->prev), deleter_);
}

std::shared_ptr<const FrameTable> FrameTableSlot::table() const {
  std::lock_guard lock(writeMutex_);
  return entry_.load(std::memory_order_relaxed)->table;
}

}  // namespace pq::dsp
