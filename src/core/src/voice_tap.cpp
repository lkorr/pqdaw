#include "pq/core/voice_tap.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>

namespace pq {

namespace {

inline float wrapUnit(float x) noexcept {  // RT-safe: frac(x) in [0, 1) (NaN stays NaN)
  const float f = x - std::floor(x);
  return f < 1.0f ? f : 0.0f;
}

}  // namespace

VoiceTap::VoiceTap(const VoiceTapConfig& c)
    : groups_(std::max(c.groups, 1u)), depth_(std::bit_ceil(std::max(c.depth, 2u))), mask_(depth_ - 1),
      stride_(std::max(c.stride, 1u)), heads_(new GroupHead[groups_]),
      ring_(static_cast<size_t>(groups_) * depth_) {
  w_.laneMask.reset(new std::atomic<uint32_t>[groups_]);
  w_.pending.reset(new Pending[groups_]);
  for (uint32_t g = 0; g < groups_; ++g) {
    w_.laneMask[g].store(0xFFu, std::memory_order_relaxed);  // until an image host says otherwise
    w_.pending[g] = Pending{};
  }
}

void VoiceTap::publish(uint32_t group, uint32_t lanes, const float* position, const float* increment) noexcept {
  GroupHead& h = heads_[group];
  const uint64_t w = h.written.load(std::memory_order_relaxed);
  VoiceTapRecord& r = ring_[static_cast<size_t>(group) * depth_ + (static_cast<uint32_t>(w) & mask_)];
  const Pending& p = w_.pending[group];
  r.serial = w + 1;
  r.group = group;
  r.lanes = lanes;
  r.stride = stride_;
  r.laneMask = lanes == 1 ? 1u : w_.laneMask[group].load(std::memory_order_relaxed);
  std::memcpy(r.sample, p.sample, sizeof(r.sample));
  for (uint32_t l = 0; l < kVoiceTapLanes; ++l) {
    r.position[l] = position != nullptr && l < lanes ? position[l] : 0.0f;
    r.increment[l] = increment != nullptr && l < lanes ? increment[l] : 0.0f;
  }
  h.written.store(w + 1, std::memory_order_seq_cst);  // publish after the data (the reader's seqlock check)
}

void VoiceTap::write(uint32_t group, uint32_t lanes, const float* x, uint32_t xStride, uint32_t frames, bool wrap,
                     const float* position, const float* increment) noexcept {  // RT-safe
  if (!wanted() || group >= groups_ || frames == 0) return;
  Pending& p = w_.pending[group];
  const uint32_t nl = lanes == 1 ? 1u : kVoiceTapLanes;
  uint32_t f = p.cursor;
  while (f < frames) {
    const float* s = x + static_cast<size_t>(f) * xStride;
    float* d = p.sample[p.count];
    for (uint32_t l = 0; l < nl; ++l) d[l] = wrap ? wrapUnit(s[l]) : s[l];
    for (uint32_t l = nl; l < kVoiceTapLanes; ++l) d[l] = 0.0f;
    if (++p.count == kVoiceTapSamples) {
      publish(group, nl, position, increment);
      p.count = 0;
    }
    f += stride_;
  }
  p.cursor = f - frames;
}

bool VoiceTap::read(uint32_t group, uint64_t index, VoiceTapRecord& out) const noexcept {
  if (group >= groups_) return false;
  const uint64_t w0 = written(group);
  if (index >= w0 || index + depth_ <= w0) return false;
  std::memcpy(&out, &ring_[static_cast<size_t>(group) * depth_ + (static_cast<uint32_t>(index) & mask_)], sizeof(out));
  std::atomic_thread_fence(std::memory_order_seq_cst);
  // A write in progress after w1 fills the slot of record w1, which is the slot of record w1 − depth.
  return index + depth_ > written(group);
}

// ---- reader -------------------------------------------------------------------------------------------------------

VoiceTapReader::VoiceTapReader(std::shared_ptr<VoiceTap> tap, uint32_t history, double staleAfter)
    : tap_(std::move(tap)), history_(std::max(history, kVoiceTapSamples)), staleAfter_(staleAfter) {
  assert(tap_ != nullptr);
  const uint32_t groups = tap_->groups();
  const uint32_t slots = groups * kVoiceTapLanes;
  voices_.assign(slots, Voice{});
  hist_.assign(static_cast<size_t>(slots) * history_, 0.0f);
  histHead_.assign(slots, 0);
  histCount_.assign(slots, 0);
  next_.assign(groups, 0);
  seen_.assign(groups, -std::numeric_limits<double>::infinity());
  mask_.assign(groups, 0);
  tap_->addReader();
  // Start at what is written now: a new reader does not replay what was sampled before it looked.
  for (uint32_t g = 0; g < groups; ++g) next_[g] = tap_->written(g);
}

VoiceTapReader::~VoiceTapReader() { tap_->removeReader(); }

void VoiceTapReader::take(const VoiceTapRecord& r, double now) {
  const uint32_t g = r.group;
  seen_[g] = now;
  mask_[g] = r.laneMask;
  const uint32_t lanes = r.lanes == 1 ? 1u : kVoiceTapLanes;
  for (uint32_t l = 0; l < lanes; ++l) {
    const uint32_t slot = g * kVoiceTapLanes + l;
    Voice& v = voices_[slot];
    v.value = r.sample[kVoiceTapSamples - 1][l];
    v.position = r.position[l];
    v.increment = r.increment[l];
    v.serial = r.serial;
    if (((r.laneMask >> l) & 1u) == 0) continue;  // a free lane: its values are not a voice's
    float* h = hist_.data() + static_cast<size_t>(slot) * history_;
    for (uint32_t k = 0; k < kVoiceTapSamples; ++k) {
      h[histHead_[slot]] = r.sample[k][l];
      histHead_[slot] = histHead_[slot] + 1 == history_ ? 0 : histHead_[slot] + 1;
    }
    histCount_[slot] = std::min(histCount_[slot] + kVoiceTapSamples, history_);
  }
}

uint32_t VoiceTapReader::poll(double now) {
  uint32_t n = 0;
  VoiceTapRecord r;
  for (uint32_t g = 0; g < tap_->groups(); ++g) {
    const uint64_t w = tap_->written(g);
    const uint64_t depth = tap_->depth();
    // Records ≤ w − depth are overwritten, and the one at w − depth + 1 … may be while we copy: start where safe.
    const uint64_t oldest = w + 1 > depth ? w + 1 - depth : 0;
    if (next_[g] < oldest) {
      lost_ += oldest - next_[g];
      next_[g] = oldest;
    }
    for (; next_[g] < w; ++next_[g]) {
      if (!tap_->read(g, next_[g], r)) {
        ++lost_;
        continue;
      }
      take(r, now);
      ++n;
    }
  }
  received_ += n;
  for (uint32_t g = 0; g < tap_->groups(); ++g) {
    const bool fresh = now - seen_[g] <= staleAfter_;
    for (uint32_t l = 0; l < kVoiceTapLanes; ++l) {
      const uint32_t slot = g * kVoiceTapLanes + l;
      const bool active = fresh && ((mask_[g] >> l) & 1u) != 0;
      if (!active && voices_[slot].active) histCount_[slot] = 0;  // a voice ended: its glow goes with it
      voices_[slot].active = active;
    }
  }
  return n;
}

uint32_t VoiceTapReader::history(uint32_t slot, std::span<float> out) const noexcept {
  if (slot >= voices_.size()) return 0;
  const uint32_t n = std::min<uint32_t>(histCount_[slot], static_cast<uint32_t>(out.size()));
  const float* h = hist_.data() + static_cast<size_t>(slot) * history_;
  // The newest n values, oldest first: they end at histHead_ − 1.
  uint32_t i = (histHead_[slot] + history_ - n) % history_;
  for (uint32_t k = 0; k < n; ++k) {
    out[k] = h[i];
    i = i + 1 == history_ ? 0 : i + 1;
  }
  return n;
}

}  // namespace pq
