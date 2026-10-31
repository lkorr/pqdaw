// Cost meters (T4.5): pq/engine/inspect.h.

#include "pq/engine/inspect.h"

#include <intrin.h>

#include <algorithm>
#include <vector>

#include "pq/engine/callback_stats.h"

namespace pq::engine {

CostMeter::CostMeter(uint32_t maxRegions)
    : maxRegions_(std::max<uint32_t>(maxRegions, 1)),
      cycles_(new std::atomic<uint64_t>[std::max<uint32_t>(maxRegions, 1)]),
      timings_(new std::atomic<uint64_t>[std::max<uint32_t>(maxRegions, 1)]) {
  for (uint32_t i = 0; i < maxRegions_; ++i) {
    cycles_[i].store(0, std::memory_order_relaxed);
    timings_[i].store(0, std::memory_order_relaxed);
  }
  (void)timerCost();  // calibrated here, off the audio thread
}

const CostMeter::TimerCost& CostMeter::timerCost() {
  static const TimerCost cost = [] {
    TimerCost c;
    // What an empty timed region reads: back-to-back TSC reads (the median of many).
    std::vector<uint64_t> d(4001);
    for (uint64_t& x : d) {
      const uint64_t a = __rdtsc();
      const uint64_t b = __rdtsc();
      x = b - a;
    }
    std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
    c.innerCycles = static_cast<double>(d[d.size() / 2]);
    // What one timing adds to the block around it: two reads and the counter update, as the generated code does them
    // (the best of a few trials of 2000).
    volatile uint64_t acc[2] = {0, 0};
    double best = 1e30;
    for (int trial = 0; trial < 7; ++trial) {
      constexpr int kN = 2000;
      const uint64_t t0 = __rdtsc();
      for (int i = 0; i < kN; ++i) {
        const uint64_t a = __rdtsc();
        const uint64_t b = __rdtsc();
        acc[0] = acc[0] + (b - a);
        acc[1] = acc[1] + 1;
      }
      best = std::min(best, static_cast<double>(__rdtsc() - t0) / kN);
    }
    c.outerCycles = best;
    return c;
  }();
  return cost;
}

uint32_t CostMeter::attach(std::span<const uint32_t> units) {
  std::lock_guard lock(mutex_);
  const uint32_t key = nextKey_++;
  // Keys below the running one can never run again as the metered image (activate only moves forward).
  const uint32_t running = active_.load(std::memory_order_acquire);
  std::erase_if(attached_, [&](const Attached& a) { return a.key < running; });
  attached_.push_back(Attached{key, nextTag_, std::vector<uint32_t>(units.begin(), units.end())});
  return key;
}

void CostMeter::add(const uint64_t* regionCycles, uint32_t n, uint64_t totalCycles, uint32_t frames) noexcept {  // RT-safe
  n = std::min(n, maxRegions_);
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t c = regionCycles[2 * static_cast<size_t>(i)], k = regionCycles[2 * static_cast<size_t>(i) + 1];
    if (k == 0) continue;
    cycles_[i].store(cycles_[i].load(std::memory_order_relaxed) + c, std::memory_order_relaxed);
    timings_[i].store(timings_[i].load(std::memory_order_relaxed) + k, std::memory_order_relaxed);
  }
  total_.store(total_.load(std::memory_order_relaxed) + totalCycles, std::memory_order_relaxed);
  frames_.store(frames_.load(std::memory_order_relaxed) + frames, std::memory_order_release);
}

bool CostMeter::read(Reading& out) {
  std::lock_guard lock(mutex_);
  const uint32_t key = active_.load(std::memory_order_acquire);  // the image that runs (L5: not the last bound)
  if (key != unitsKey_) {
    const auto it = std::find_if(attached_.begin(), attached_.end(), [&](const Attached& a) { return a.key == key; });
    if (it == attached_.end()) return false;  // nothing has run yet
    units_ = it->units;
    unitsKey_ = key;
    unitsTag_ = it->tag;
    std::erase_if(attached_, [&](const Attached& a) { return a.key < key; });
  }
  const uint64_t frames = frames_.load(std::memory_order_acquire);
  const uint64_t total = total_.load(std::memory_order_relaxed);
  const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(units_.size()), maxRegions_);
  if (key != baseKey_ || baseCycles_.size() != n) {
    // A new image: take the baseline (the previous image may still have added its last block after attach()).
    baseKey_ = key;
    baseTotal_ = total;
    baseFrames_ = frames;
    baseCycles_.resize(n);
    baseTimings_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      baseCycles_[i] = cycles_[i].load(std::memory_order_relaxed);
      baseTimings_[i] = timings_[i].load(std::memory_order_relaxed);
    }
    return false;
  }
  if (frames <= baseFrames_) return false;
  const double df = static_cast<double>(frames - baseFrames_);
  const double nsPerCycle = 1e9 / tscFrequencyHz();
  const TimerCost& tc = timerCost();
  out.key = key;
  out.tag = unitsTag_;
  out.frames = frames - baseFrames_;
  out.regionNsPerFrame.assign(units_.size(), 0.0);
  double timings = 0.0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t c = cycles_[i].load(std::memory_order_relaxed), k = timings_[i].load(std::memory_order_relaxed);
    const double dk = static_cast<double>(k - baseTimings_[i]);
    timings += dk;
    const double dc = static_cast<double>(c - baseCycles_[i]) - dk * tc.innerCycles;
    out.regionNsPerFrame[i] = std::max(0.0, dc) * nsPerCycle / df;
    baseCycles_[i] = c;
    baseTimings_[i] = k;
  }
  const double dt = static_cast<double>(total - baseTotal_) - timings * tc.outerCycles;
  out.totalNsPerFrame = std::max(0.0, dt) * nsPerCycle / df;
  out.units = units_;
  baseTotal_ = total;
  baseFrames_ = frames;
  return true;
}

}  // namespace pq::engine
