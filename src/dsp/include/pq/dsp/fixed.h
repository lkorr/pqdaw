#pragma once

// 32.32 fixed-point source positions (SM.15: one copy; the clip player, the audio sequence image, the Sampler's warp
// voice, its Complex voice and the granulator all read through these). A position is frames · 2^32 in an int64; a
// step is source frames per output frame in the same unit. All RT-safe.

#include <cmath>
#include <cstdint>

namespace pq::dsp {

inline constexpr int64_t kFixedOne = int64_t{1} << 32;  // one frame in 32.32
inline constexpr double k2m32 = 1.0 / 4294967296.0;     // 2^-32: a 32.32 value in frames
inline constexpr double kMaxReadRate = 64.0;            // source frames per output frame at most (beyond: clamped)

// v frames as 32.32 (round half away from zero). RT-safe.
inline int64_t toFixed(double v) noexcept { return static_cast<int64_t>(std::llround(v * 4294967296.0)); }
// A rate r ≥ 0 as a 32.32 step (= toFixed for r ≥ 0, without the CRT call: per-sample loops). RT-safe.
inline int64_t rateStep(double r) noexcept { return static_cast<int64_t>(r * 4294967296.0 + 0.5); }
// A read rate from a kernel's input: NaN and negatives are 0, at most kMaxReadRate. RT-safe.
inline double clampReadRate(double r) noexcept { return r > 0.0 ? (r < kMaxReadRate ? r : kMaxReadRate) : 0.0; }

// floor(a / b) and ceil(a / b), b > 0. RT-safe.
inline constexpr int64_t floorDiv(int64_t a, int64_t b) noexcept { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline constexpr int64_t ceilDiv(int64_t a, int64_t b) noexcept { return a > 0 ? (a - 1) / b + 1 : -(-a / b); }

// anchor + d · step in 32.32, wrapping arithmetic (exact whenever the true result fits; it does for any frame < 2^31).
inline constexpr int64_t fixedAt(int64_t anchor, int64_t d, int64_t step) noexcept {
  return static_cast<int64_t>(static_cast<uint64_t>(anchor) + static_cast<uint64_t>(d) * static_cast<uint64_t>(step));
}

}  // namespace pq::dsp
