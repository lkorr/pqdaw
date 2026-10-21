#include "pq/engine/callback_stats.h"

#include <array>

#include <windows.h>

namespace pq::engine {
namespace {

double calibrateTsc() {
  LARGE_INTEGER freq{}, q0{}, q1{};
  QueryPerformanceFrequency(&freq);
  // Pair each QPC read with the TSC read right after it; 20 ms keeps the error near 1e-5 (QPC ticks at 10 MHz).
  QueryPerformanceCounter(&q0);
  const uint64_t t0 = readTsc();
  Sleep(20);
  QueryPerformanceCounter(&q1);
  const uint64_t t1 = readTsc();
  const double seconds = static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(freq.QuadPart);
  return static_cast<double>(t1 - t0) / seconds;
}

}  // namespace

double tscFrequencyHz() {
  static const double hz = calibrateTsc();
  return hz;
}

bool tscIsInvariant() {
  std::array<int, 4> r{};
  __cpuid(r.data(), 0x80000000);
  if (static_cast<unsigned>(r[0]) < 0x80000007u) return false;
  __cpuid(r.data(), 0x80000007);
  return (r[3] & (1 << 8)) != 0;
}

void CallbackStats::reset(uint32_t bufferFrames, uint32_t sampleRate) {
  ticksPerUs_ = tscFrequencyHz() * 1e-6;
  reliable_ = tscIsInvariant();
  periodUs_ = sampleRate ? 1e6 * bufferFrames / sampleRate : 0.0;
  periodTicks_ = sampleRate ? static_cast<uint64_t>(periodUs_ * ticksPerUs_) : ~0ull;
  usPerFrame_ = sampleRate ? 1e6 / sampleRate : 0.0;
  for (auto* a : {&callbacks_, &frames_, &inputOverflows_, &outputUnderflows_, &deadlineMisses_, &totalTicks_,
                  &maxTicks_, &maxGapTicks_, &firstStartAtomic_, &lastStartAtomic_})
    a->store(0, std::memory_order_relaxed);
  for (auto* a : {&wakes_, &waitTimeouts_, &restarts_, &queueCount_}) a->store(0, std::memory_order_relaxed);
  queueSum_.store(0, std::memory_order_relaxed);
  maxLateness_.store(0, std::memory_order_relaxed);
  minQueue_.store(-1, std::memory_order_relaxed);
  maxQueue_.store(-1, std::memory_order_relaxed);
}

CallbackStats::Snapshot CallbackStats::snapshot() const {
  Snapshot s;
  s.callbacks = callbacks_.load(std::memory_order_relaxed);
  s.frames = frames_.load(std::memory_order_relaxed);
  s.inputOverflows = inputOverflows_.load(std::memory_order_relaxed);
  s.outputUnderflows = outputUnderflows_.load(std::memory_order_relaxed);
  s.deadlineMisses = deadlineMisses_.load(std::memory_order_relaxed);
  s.periodUs = periodUs_;
  const double total = static_cast<double>(totalTicks_.load(std::memory_order_relaxed)) / ticksPerUs_;
  s.meanDurationUs = s.callbacks ? total / static_cast<double>(s.callbacks) : 0.0;
  s.maxDurationUs = static_cast<double>(maxTicks_.load(std::memory_order_relaxed)) / ticksPerUs_;
  s.maxIntervalUs = static_cast<double>(maxGapTicks_.load(std::memory_order_relaxed)) / ticksPerUs_;
  s.activeSeconds = static_cast<double>(lastStartAtomic_.load(std::memory_order_relaxed) -
                                        firstStartAtomic_.load(std::memory_order_relaxed)) /
                    (ticksPerUs_ * 1e6);
  s.timingReliable = reliable_;
  s.wakes = wakes_.load(std::memory_order_relaxed);
  s.waitTimeouts = waitTimeouts_.load(std::memory_order_relaxed);
  s.maxLatenessFrames = maxLateness_.load(std::memory_order_relaxed);
  s.maxLatenessUs = static_cast<double>(s.maxLatenessFrames) * usPerFrame_;
  s.minQueueFrames = minQueue_.load(std::memory_order_relaxed);
  s.maxQueueFrames = maxQueue_.load(std::memory_order_relaxed);
  s.restarts = restarts_.load(std::memory_order_relaxed);
  const uint64_t qn = queueCount_.load(std::memory_order_relaxed);
  if (qn) s.meanQueueFrames = static_cast<double>(queueSum_.load(std::memory_order_relaxed)) / static_cast<double>(qn);
  return s;
}

}  // namespace pq::engine
