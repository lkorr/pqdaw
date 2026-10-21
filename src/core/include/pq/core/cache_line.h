#pragma once

#include <cstddef>

namespace pq {

// Destructive-interference size on every x86-64 we target. Data written by different threads lives on
// different lines of this size (SPEC §13: no false sharing). Fixed rather than
// std::hardware_destructive_interference_size so struct layouts never depend on compiler flags.
inline constexpr std::size_t kCacheLineSize = 64;

}  // namespace pq
