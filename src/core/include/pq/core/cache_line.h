#pragma once

#include <cstddef>

namespace pq {

// Destructive-interference size on every x86-64 we target. Data written by different threads lives on
// different lines of this size (SPEC §13: no false sharing). Fixed rather than
// std::hardware_destructive_interference_size so struct layouts never depend on compiler flags.
inline constexpr std::size_t kCacheLineSize = 64;

// Distance between the hot index lines that two threads write in the cross-thread queues (SpscRing,
// MpscQueue). Intel's L2 spatial prefetcher fetches the 128-byte-aligned pair of a missed line, so two
// written lines in the same pair can still ping-pong; 128 keeps them in different pairs (folly and
// crossbeam use 128 for the same reason). Chosen by measurement with pinned threads (T0.2-fix1,
// bench/RESULTS.md): the queues take it as a defaulted template argument so both layouts stay benchable.
inline constexpr std::size_t kHotLineSeparation = 128;

}  // namespace pq
