#pragma once

// MOS regions of the generator axis (mos-explorer buildMOSRegions, SPEC §4.2 region chart) and the per-region linear
// form used by tuning-as-signal (SPEC §3.3, T2.6).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "pq/tuning/mos.h"

namespace pq::tuning {

// The region chart's colour palette (PALETTE), as "#rrggbb".
inline constexpr std::array<const char*, 30> kMosPalette = {
    "#6eb5ff", "#e94560", "#50c878", "#f5a623", "#bb86fc", "#ff6f91", "#00c9a7", "#ffc75f", "#845ec2", "#d65db1",
    "#ff9671", "#2ec4b6", "#e0aaff", "#c9184a", "#588157", "#a7c957", "#f4845f", "#277da1", "#f94144", "#43aa8b",
    "#577590", "#f8961e", "#90be6d", "#f9844a", "#4cc9f0", "#7209b7", "#3a0ca3", "#4361ee", "#4895ef", "#560bad",
};

// One region: generators in [lowCents, highCents] give the MOS `key`. Fractions are of the period.
struct MosRegion {
  MosKey key;                      // whole-scale counts (per-period counts × periods per equave)
  int32_t size = 0;
  MosKey perPeriodKey;
  int32_t perPeriodSize = 0;
  std::array<int32_t, 2> lowFrac{};
  std::array<int32_t, 2> highFrac{};
  double lowCents = 0.0;
  double highCents = 0.0;
  uint8_t collapsedColor = 0;      // kMosPalette indices (color and collapsedColor in the JS)
  uint8_t equalizedColor = 0;
  int32_t depth = 0;
  std::optional<MosKey> parent;    // of the whole-scale key
  bool isMirror = false;           // the period − g side

  RegionHint hint() const noexcept { return {perPeriodSize, lowCents, highCents}; }
};

struct MosRegionSet {
  std::vector<MosRegion> regions;                    // in the JS order (direct, mirror, direct, mirror, …)
  std::vector<std::pair<int32_t, uint8_t>> colorMap; // count → palette index, sorted by count (Object.entries order)
};

// buildMOSRegions(maxSize, maxDepth, period) with reps = periodRepetitions(viewLimit, period).
MosRegionSet buildMosRegions(int32_t maxSize, int32_t maxDepth, double period, int32_t periodsPerEquave);

// getEqualizedEnd: which end of the region has L = s.
enum class RegionEnd : uint8_t { Low, High };
RegionEnd equalizedEnd(const MosRegion& region, double period);

// findAdjacentRegion: the region (other than sel) within 0.5 cents of `cents`, preferring the nearest size and then
// the nearest centre. Returns an index into `regions`, or −1.
int32_t findAdjacentRegion(std::span<const MosRegion> regions, const MosRegion& sel, double cents, int32_t minSize,
                           int32_t maxSize);

// ---- Linear form per region (T2.6) ------------------------------------------------------------------------------
//
// For a fixed chain length n, the sorted order of the chain pitches i·g mod P changes only where i·g − j·g or i·g
// hits a multiple of P, i.e. at g/P = p/q with q < n: the Farey fractions of order n − 1. Between two neighbours the
// degree at sorted position j is chain element k_j wrapped m_j times, so
//     cents_j = k_j · g − m_j · P          (0 ≤ cents_j < P)
// and the whole scale is n fused multiply-adds. The table holds (k, m) for every interval, built off the audio thread;
// lookup is a binary search. Intervals are half-open [low, high): exactly at a boundary the interval above is used.
// (buildMosScale nudges a degenerate generator to one side instead; the pitch sets agree there except for the one
// chain note sitting on 0 vs P.) Rotation is applied with rotateLinearForm; MODMOS alterations are not linear in g
// (the JS rounds L and s to 0.01 cents) and are not represented here.

struct MosLinearSpan {
  const int32_t* k = nullptr;   // generator multiples, sorted-degree order (k[0] = 0)
  const int32_t* m = nullptr;   // period multiples for g reduced into [0, P)
  int32_t n = 0;
  int32_t periodShift = 0;      // floor(g / P): adds k·periodShift to every m
  double lowGenerator = 0.0;    // the interval in generator cents (for crossing detection)
  double highGenerator = 0.0;

  // Unrotated degree j (0 ≤ j < n) in cents. RT-safe.
  double cents(int32_t j, double g, double period) const noexcept {
    return static_cast<double>(k[j]) * g -
           (static_cast<double>(m[j]) + static_cast<double>(k[j]) * periodShift) * period;
  }
};

class MosLinearTable {
public:
  // n ≥ 1 notes per period. Holds about 0.3·n² intervals × n × 2 ints (n = 24: ~34 KB): build it off the audio
  // thread, once per chain length.
  explicit MosLinearTable(int32_t n);

  int32_t n() const noexcept { return n_; }
  // Number of constant-order intervals in [0, 1) of the period.
  int32_t intervalCount() const noexcept { return static_cast<int32_t>(bounds_.size()) - 1; }
  // Interval boundaries as fractions of the period (ascending, from 0 to 1).
  std::span<const double> boundaries() const noexcept { return bounds_; }

  // The interval containing generator g (any finite value; reduced into [0, P) with periodShift). RT-safe.
  MosLinearSpan lookup(double genCents, double period) const noexcept;

private:
  int32_t n_ = 0;
  std::vector<double> bounds_;  // intervalCount + 1 entries
  std::vector<int32_t> k_;      // intervalCount × n
  std::vector<int32_t> m_;      // intervalCount × n
};

// Coefficients of the scale rotated to `rotation` (0 ≤ rotation < n), written to kOut/mOut (n entries each), so that
// cents_j = kOut[j]·g − mOut[j]·P reproduces rotateMosScale(buildMosScale(g, n, P), rotation, P).pitches[j]. The
// periodShift is folded into mOut. RT-safe.
void rotateLinearForm(const MosLinearSpan& span, int32_t rotation, int32_t* kOut, int32_t* mOut) noexcept;

// Every degree's cents for g (unrotated). RT-safe.
void evaluateLinearForm(const MosLinearSpan& span, double genCents, double period, double* centsOut) noexcept;

}  // namespace pq::tuning
