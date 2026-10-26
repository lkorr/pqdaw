#pragma once

// A probe's per-block summary (T4.5, SPEC §7.3): the audio thread writes it after each block a probed value is computed
// in; any thread reads it. The cheap half of a probe — a value readout, a bool wire's LED, a modulated knob's live range
// (T4.7) — next to the full signal a TapRing carries (pq/core/tap_ring.h, the mini-scope).
//
// One writer thread. The fields are separate relaxed atomics: a reader may see `last` of one block and `hi` of the next,
// which no display can tell apart. `blocks` counts writes, so a reader can see whether anything arrived.
// Bools are written as 0 / 1 (the program's lane masks are decoded by the writer).

#include <atomic>
#include <cstdint>

#include "pq/core/cache_line.h"

namespace pq {

struct alignas(kHotLineSeparation) ProbeLevel {
  std::atomic<float> last{0.0f};  // the block's last frame
  std::atomic<float> lo{0.0f};    // minimum over the block (bool: 0 if it was low at any frame)
  std::atomic<float> hi{0.0f};    // maximum over the block (bool: 1 if it was high at any frame: the LED)
  std::atomic<uint64_t> blocks{0};

  // Writer (audio thread). RT-safe.
  void write(float lastValue, float low, float high) noexcept {
    last.store(lastValue, std::memory_order_relaxed);
    lo.store(low, std::memory_order_relaxed);
    hi.store(high, std::memory_order_relaxed);
    blocks.store(blocks.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }
};

}  // namespace pq
