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
// lookup is a binary search. MODMOS alterations are not linear in g (the JS rounds L and s to 0.01 cents) and are not
// represented here.
//
// Boundaries (T2.6-fix2, M3 review H3). Away from a boundary the intervals are half-open [low, high). A generator
// within boundaryWindow(b) = 1e-6/q cents of boundary b = p/q (lowest terms) is degenerate for buildMOSScale: its
// chainSafeGenerator nudges it by 1e-7 cents, toward the region hint's midpoint when the hint has this chain length
// and contains the generator (±1e-6 cents), else toward the side whose chain has fewer distinct step sizes (no nudge
// on a tie). intervalAt / lookup reproduce that choice exactly, and report the nudge (MosLinearSpan::nudge), so the
// form gives the pitches buildMOSScale gives, boundaries included: e.g. 5L2s at exactly 720 cents with the 5L2s hint
// is the interval below 3\5 (degree 1 = 240 cents, the collapsed 5L2s), with the 2L5s hint the one above. The two
// sides differ in their degree-indexed pitches (whole steps for some degrees), not by one note.
//
// Mode transfer (T2.6-fix2, M3 review M7). Rotation r roots the scale at sorted degree r, which is chain element
// rootChain(t, r) = k[t][r]. When the generator moves into another interval of the same chain length, the
// explorer's transferModeRotation keeps that chain position: the rotation becomes rotationOf(t', rootChain). So a
// tuning is defined by its root's chain position (its rotation at its own generator), and the rotation in any
// interval follows from it (carryRotation). The tuning node's table (pq/dsp/tuning_table.h), the document's generator
// edits (SetTuningParam) and the piano roll's live morph all use this rule.

struct MosLinearSpan {
  const int32_t* k = nullptr;   // generator multiples, sorted-degree order (k[0] = 0)
  const int32_t* m = nullptr;   // period multiples for g reduced into [0, P)
  int32_t n = 0;
  int32_t periodShift = 0;      // floor(g / P) of the interval: adds k·periodShift to every m
  int32_t interval = 0;         // index into boundaries() of the interval's low end
  double lowGenerator = 0.0;    // the interval in generator cents (for crossing detection)
  double highGenerator = 0.0;
  double nudge = 0.0;           // buildMOSScale's degenerate-chain nudge of this generator (0, ±1e-7 cents)

  // Unrotated degree j (0 ≤ j < n) in cents at generator g (pass g + nudge for buildMOSScale's exact value).
  // RT-safe.
  double cents(int32_t j, double g, double period) const noexcept {
    return static_cast<double>(k[j]) * g -
           (static_cast<double>(m[j]) + static_cast<double>(k[j]) * periodShift) * period;
  }
};

class MosLinearTable {
public:
  // n ≥ 1 notes per period. Holds about 0.3·n² intervals × n × 3 ints (n = 24: ~50 KB): build it off the audio
  // thread, once per chain length.
  explicit MosLinearTable(int32_t n);

  int32_t n() const noexcept { return n_; }
  // Number of constant-order intervals in [0, 1) of the period.
  int32_t intervalCount() const noexcept { return static_cast<int32_t>(bounds_.size()) - 1; }
  // Interval boundaries as fractions of the period (ascending, from 0 to 1).
  std::span<const double> boundaries() const noexcept { return bounds_; }
  // Per boundary: where buildMOSScale without a region hint puts a generator inside its window (−1: the interval
  // below, +1: above, 0: no nudge, so the half-open rule), and the window in cents (1e-6 / denominator).
  std::span<const int8_t> boundarySides() const noexcept { return sides_; }
  std::span<const double> boundaryWindows() const noexcept { return windows_; }
  // Raw tables (intervalCount × n each, interval-major): k, m as in MosLinearSpan; inverse[t][c] = the sorted degree
  // of chain element c in interval t.
  std::span<const int32_t> kTable() const noexcept { return k_; }
  std::span<const int32_t> mTable() const noexcept { return m_; }
  std::span<const int32_t> inverseTable() const noexcept { return inv_; }

  // The interval holding generator g (any finite value) as buildMOSScale(g, n, P, hint) sees it (see "Boundaries").
  // RT-safe.
  MosLinearSpan lookup(double genCents, double period, const RegionHint* hint = nullptr) const noexcept;

  // Mode transfer (see above). RT-safe.
  int32_t rootChain(int32_t interval, int32_t rotation) const noexcept {
    return k_[static_cast<size_t>(interval) * static_cast<size_t>(n_) + static_cast<size_t>(rotation)];
  }
  int32_t rotationOf(int32_t interval, int32_t chain) const noexcept {
    return inv_[static_cast<size_t>(interval) * static_cast<size_t>(n_) + static_cast<size_t>(chain)];
  }
  // The rotation at generator toGen of the mode `rotation` defined at fromGen (transferModeRotation for an unchanged
  // chain length; 0 ≤ rotation < n). RT-safe.
  int32_t carryRotation(double fromGen, int32_t rotation, double toGen, double period,
                        const RegionHint* hint = nullptr) const noexcept;

private:
  int32_t n_ = 0;
  std::vector<double> bounds_;   // intervalCount + 1 entries
  std::vector<int8_t> sides_;    // intervalCount + 1
  std::vector<double> windows_;  // intervalCount + 1
  std::vector<int32_t> k_;       // intervalCount × n
  std::vector<int32_t> m_;       // intervalCount × n
  std::vector<int32_t> inv_;     // intervalCount × n
};

// The side (−1 below, +1 above, 0 none) buildMOSScale's chainSafeGenerator nudges a degenerate generator `genCents`
// at a boundary to, given the boundary's hint-less side: toward the hint's midpoint when the hint has n notes per
// period and contains genCents (±1e-6 cents), else `naturalSide`. RT-safe.
inline int32_t boundaryNudgeSide(double genCents, int32_t n, int8_t naturalSide, const RegionHint* hint) noexcept {
  if (hint != nullptr && hint->perPeriodSize == n && genCents >= hint->lowCents - 1e-6 &&
      genCents <= hint->highCents + 1e-6)
    return (hint->lowCents + hint->highCents) / 2.0 >= genCents ? 1 : -1;
  return naturalSide;
}

// chainSafeGenerator's nudge, in cents.
inline constexpr double kChainSafeNudge = 1e-7;

// Coefficients of the scale rotated to `rotation` (0 ≤ rotation < n), written to kOut/mOut (n entries each), so that
// cents_j = kOut[j]·g − mOut[j]·P reproduces rotateMosScale(buildMosScale(g, n, P), rotation, P).pitches[j]. The
// periodShift is folded into mOut. RT-safe.
void rotateLinearForm(const MosLinearSpan& span, int32_t rotation, int32_t* kOut, int32_t* mOut) noexcept;

// Every degree's cents for g (unrotated). RT-safe.
void evaluateLinearForm(const MosLinearSpan& span, double genCents, double period, double* centsOut) noexcept;

}  // namespace pq::tuning
