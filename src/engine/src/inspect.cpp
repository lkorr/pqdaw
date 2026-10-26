// Cost meters (T4.5): pq/engine/inspect.h.

#include "pq/engine/inspect.h"

#include <algorithm>

#include "pq/engine/callback_stats.h"

namespace pq::engine {

CostMeter::CostMeter(uint32_t maxRegions)
    : maxRegions_(std::max<uint32_t>(maxRegions, 1)), cycles_(new std::atomic<uint64_t>[std::max<uint32_t>(maxRegions, 1)]) {
  for (uint32_t i = 0; i < maxRegions_; ++i) cycles_[i].store(0, std::memory_order_relaxed);
}

uint32_t CostMeter::attach(std::span<const uint32_t> units) {
  std::lock_guard lock(mutex_);
  const uint32_t key = nextKey_++;
  units_.assign(units.begin(), units.end());
  unitsKey_ = key;
  active_.store(key, std::memory_order_relaxed);
  return key;
}

void CostMeter::add(const uint64_t* regionCycles, uint32_t n, uint64_t totalCycles, uint32_t frames) noexcept {  // RT-safe
  n = std::min(n, maxRegions_);
  for (uint32_t i = 0; i < n; ++i)
    if (regionCycles[i] != 0) cycles_[i].store(cycles_[i].load(std::memory_order_relaxed) + regionCycles[i], std::memory_order_relaxed);
  total_.store(total_.load(std::memory_order_relaxed) + totalCycles, std::memory_order_relaxed);
  frames_.store(frames_.load(std::memory_order_relaxed) + frames, std::memory_order_release);
}

bool CostMeter::read(Reading& out) {
  std::lock_guard lock(mutex_);
  const uint32_t key = unitsKey_;
  const uint64_t frames = frames_.load(std::memory_order_acquire);
  const uint64_t total = total_.load(std::memory_order_relaxed);
  const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(units_.size()), maxRegions_);
  if (key != baseKey_ || baseCycles_.size() != n) {
    // A new image: take the baseline (the previous image may still have added its last block after attach()).
    baseKey_ = key;
    baseTotal_ = total;
    baseFrames_ = frames;
    baseCycles_.resize(n);
    for (uint32_t i = 0; i < n; ++i) baseCycles_[i] = cycles_[i].load(std::memory_order_relaxed);
    return false;
  }
  if (frames <= baseFrames_) return false;
  const double df = static_cast<double>(frames - baseFrames_);
  const double nsPerCycle = 1e9 / tscFrequencyHz();
  out.key = key;
  out.frames = frames - baseFrames_;
  out.totalNsPerFrame = static_cast<double>(total - baseTotal_) * nsPerCycle / df;
  out.regionNsPerFrame.assign(units_.size(), 0.0);
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t c = cycles_[i].load(std::memory_order_relaxed);
    out.regionNsPerFrame[i] = static_cast<double>(c - baseCycles_[i]) * nsPerCycle / df;
    baseCycles_[i] = c;
  }
  out.units = units_;
  baseTotal_ = total;
  baseFrames_ = frames;
  return true;
}

}  // namespace pq::engine
