#pragma once

#include <atomic>
#include <cstdint>

#include <intrin.h>

#include "pq/core/cache_line.h"

namespace pq::engine {

// Timestamp counter read on the audio thread: one `rdtsc` instruction, never a syscall. (QueryPerformanceCounter
// is a user-mode TSC read on invariant-TSC machines too, but that is an OS implementation detail; rdtsc is
// guaranteed.) Ticks are converted to time off the audio thread with tscFrequencyHz().
inline uint64_t readTsc() noexcept { return __rdtsc(); }  // RT-safe

// TSC ticks per second, calibrated once against QueryPerformanceCounter over ~20 ms on first call (blocks the
// caller for that long: call it off the audio thread, e.g. when opening a stream). Thread-safe.
double tscFrequencyHz();

// True if the CPU reports an invariant TSC (CPUID 0x80000007 EDX bit 8): constant rate across P-/C-states and
// synchronised across cores, so audio-thread timestamps are comparable even if the thread migrates.
bool tscIsInvariant();

// Per-stream callback statistics. One writer (the audio thread, via record()), any number of readers
// (snapshot()). Each counter is a relaxed atomic written with load+store, not a locked read-modify-write,
// because there is a single writer; a snapshot may mix values from adjacent callbacks, never torn values.
class CallbackStats {
public:
  struct Snapshot {
    uint64_t callbacks = 0;
    uint64_t frames = 0;
    uint64_t inputOverflows = 0;    // callbacks the backend flagged with an input overflow
    uint64_t outputUnderflows = 0;  // callbacks the backend flagged with an output underflow
    uint64_t deadlineMisses = 0;    // callbacks whose own duration exceeded one buffer period
    double periodUs = 0;            // buffer period = bufferFrames / sampleRate
    double meanDurationUs = 0;      // time spent inside the callback (the engine's work + adapter)
    double maxDurationUs = 0;
    double maxIntervalUs = 0;       // longest gap between the starts of consecutive callbacks
    double activeSeconds = 0;       // from the start of the first callback to the start of the last one
    bool timingReliable = false;    // false if the TSC is not invariant
    uint64_t xruns() const noexcept { return inputOverflows + outputUnderflows; }
  };

  // Off the audio thread, before the stream starts. Zeroes every counter and sets the deadline used for
  // deadlineMisses. Calibrates the TSC on first use (~20 ms).
  void reset(uint32_t bufferFrames, uint32_t sampleRate);

  // Records one callback that started at startTsc and ended at endTsc. RT-safe.
  void record(uint64_t startTsc, uint64_t endTsc, uint32_t frames, bool inputOverflow,
              bool outputUnderflow) noexcept {  // RT-safe
    const uint64_t prevStart = lastStartAtomic_.load(std::memory_order_relaxed);
    if (prevStart == 0) firstStartAtomic_.store(startTsc, std::memory_order_relaxed);
    lastStartAtomic_.store(startTsc, std::memory_order_relaxed);
    bump(callbacks_);
    frames_.store(frames_.load(std::memory_order_relaxed) + frames, std::memory_order_relaxed);
    if (inputOverflow) bump(inputOverflows_);
    if (outputUnderflow) bump(outputUnderflows_);
    const uint64_t dur = endTsc - startTsc;
    totalTicks_.store(totalTicks_.load(std::memory_order_relaxed) + dur, std::memory_order_relaxed);
    if (dur > maxTicks_.load(std::memory_order_relaxed)) maxTicks_.store(dur, std::memory_order_relaxed);
    if (dur > periodTicks_) bump(deadlineMisses_);
    if (prevStart != 0) {
      const uint64_t gap = startTsc - prevStart;
      if (gap > maxGapTicks_.load(std::memory_order_relaxed)) maxGapTicks_.store(gap, std::memory_order_relaxed);
    }
  }

  // Any thread. Converts ticks to microseconds.
  Snapshot snapshot() const;

private:
  static void bump(std::atomic<uint64_t>& a) noexcept {
    a.store(a.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }

  // Writer-only state.
  uint64_t periodTicks_ = ~0ull;
  double periodUs_ = 0;
  double ticksPerUs_ = 1;
  bool reliable_ = false;
  // Shared counters on their own cache line, away from whatever owns this object.
  alignas(kCacheLineSize) std::atomic<uint64_t> callbacks_{0};
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> inputOverflows_{0};
  std::atomic<uint64_t> outputUnderflows_{0};
  std::atomic<uint64_t> deadlineMisses_{0};
  std::atomic<uint64_t> totalTicks_{0};
  std::atomic<uint64_t> maxTicks_{0};
  std::atomic<uint64_t> maxGapTicks_{0};
  std::atomic<uint64_t> firstStartAtomic_{0};
  std::atomic<uint64_t> lastStartAtomic_{0};
};

}  // namespace pq::engine
