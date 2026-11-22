// Grain taps (SM.6): pq/core/grain_tap.h.
#include "pq/core/grain_tap.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

#if !defined(_M_X64) && !defined(__x86_64__)
#error "GrainTap's seqlock relies on x86-64 store ordering (as VoiceTap: pq/core/tap_ring.h)"
#endif

namespace pq {

GrainTap::GrainTap(const GrainTapConfig& c)
    : groups_(std::max(c.groups, 1u)), depth_(std::bit_ceil(std::max(c.depth, 2u))), mask_(depth_ - 1),
      heads_(new GroupHead[groups_]), ring_(static_cast<size_t>(groups_) * depth_) {}

GrainTapRecord* GrainTap::begin(uint32_t group) noexcept {  // RT-safe
  if (!wanted() || group >= groups_) return nullptr;
  const uint64_t w = heads_[group].written.load(std::memory_order_relaxed);
  return &ring_[static_cast<size_t>(group) * depth_ + (static_cast<uint32_t>(w) & mask_)];
}

void GrainTap::commit(uint32_t group, uint32_t count) noexcept {  // RT-safe
  if (group >= groups_) return;
  GroupHead& h = heads_[group];
  const uint64_t w = h.written.load(std::memory_order_relaxed);
  GrainTapRecord& r = ring_[static_cast<size_t>(group) * depth_ + (static_cast<uint32_t>(w) & mask_)];
  r.serial = w + 1;
  r.group = group;
  r.count = std::min(count, kGrainTapGrains);
  // Publish after the data (the reader's seqlock check); x86-64 (TSO) orders the stores.
  h.written.store(w + 1, std::memory_order_seq_cst);
}

bool GrainTap::read(uint32_t group, uint64_t index, GrainTapRecord& out) const noexcept {
  if (group >= groups_) return false;
  const uint64_t w0 = written(group);
  if (index >= w0 || index + depth_ <= w0) return false;
  const GrainTapRecord& r = ring_[static_cast<size_t>(group) * depth_ + (static_cast<uint32_t>(index) & mask_)];
  const uint32_t count = std::min(r.count, kGrainTapGrains);
  out.serial = r.serial;
  out.group = r.group;
  out.count = count;
  std::memcpy(out.grain, r.grain, sizeof(GrainTapGrain) * count);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  // A write in progress after w1 fills the slot of record w1, which is the slot of record w1 − depth.
  return index + depth_ > written(group) && out.serial == index + 1;
}

bool GrainTap::latest(uint32_t group, GrainTapRecord& out) const noexcept {
  if (group >= groups_) return false;
  const uint64_t w = written(group);
  return w > 0 && read(group, w - 1, out);
}

GrainTapReader::GrainTapReader(std::shared_ptr<GrainTap> tap, double staleAfter)
    : tap_(std::move(tap)), staleAfter_(staleAfter) {
  last_.resize(tap_->groups());
  serial_.assign(tap_->groups(), 0);
  seen_.assign(tap_->groups(), -std::numeric_limits<double>::infinity());
  tap_->addReader();
}

GrainTapReader::~GrainTapReader() { tap_->removeReader(); }

uint32_t GrainTapReader::poll(double now) {
  uint32_t got = 0;
  for (uint32_t g = 0; g < tap_->groups(); ++g) {
    if (tap_->written(g) == serial_[g]) continue;
    if (!tap_->latest(g, last_[g])) continue;
    serial_[g] = last_[g].serial;
    seen_[g] = now;
    ++got;
  }
  for (uint32_t g = 0; g < tap_->groups(); ++g)
    if (now - seen_[g] > staleAfter_) last_[g].count = 0;
  return got;
}

const GrainTapRecord* GrainTapReader::group(uint32_t g) const noexcept { return g < last_.size() ? &last_[g] : nullptr; }

}  // namespace pq
