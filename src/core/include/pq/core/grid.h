#pragma once

// Editing grid: grid settings (Ableton Live 12 style), grid-line iteration, snapping, relative snap, quantize,
// and ruler labels (bars.beats.sixteenths). Shared by the arrangement, the piano roll and the edit operations.
//
// ---- The grid model -----------------------------------------------------------------------------------------
// A grid resolves, per time-signature segment, to a GridDivision: either a straight note value 1/d of a whole
// note (d = 1, 2, 4 … 64) or a cell of k bars (k = 1, 2, 4, 8 …), optionally divided into an n-tuplet.
//  - Note cells restart at every bar line (a 7/8 bar on a 1/4 grid: 0, ¼, ½, ¾, then the next bar), so bar
//    lines are always grid lines. When a straight note cell is at least a bar long it becomes a k-bar cell
//    (k = bit_floor(cell / bar)), e.g. fixed 1/1 in 3/4 = 1 bar.
//  - k-bar cells are counted from the first bar of the time-signature segment (bar 0 for the first one), so a
//    signature change always starts a new cell.
//  - Tuplet n puts n lines in the span of tupletBase(n) = bit_floor(n) straight cells (T0.5): a 7-tuplet at
//    1/16 is 7 lines per quarter, a triplet at 1 bar is 3 lines per 2 bars. Line j of a group is at
//    groupStart + round(j · span / n), computed from j directly (never accumulated). With PPQ 27720 every
//    3/5/7/9/11-tuplet of every note value 1/1 … 1/64 is an exact integer; only straight 1/64 (1732.5 ticks)
//    rounds, alternating 1732/1733 without drift (docs/QUESTIONS.md Q3). Groups that do not fit the rest of
//    the bar (or segment) are cut off by the next bar (segment) line.
//  - Adaptive: the finest division whose cell (including the tuplet) is ≥ adaptiveMinCellPx(level) pixels at
//    the current zoom. Candidates fine → coarse: 1/64 … 1/2 (a note cell longer than a beat must tile the bar
//    evenly, so 3/4 goes 1/4 → 1 bar, skipping 1/2), then 1, 2, 4, 8 … bars (up to kGridMaxBarsPerCell).
//  - Fixed: the chosen value regardless of zoom: 8, 4, 2 bars, 1/1 … 1/64 (GridSpec::fixedExp).
//  - Off: no lines, no snapping, quantize is the identity.
//
// ---- Line strength ------------------------------------------------------------------------------------------
// Bar (a bar line), Beat (on a beat of the time signature), Subdivision (a straight grid line, or the first line
// of a tuplet group), Tuplet (a line inside a tuplet group).
//
// ---- Snapping -----------------------------------------------------------------------------------------------
// snap(t): nearest grid line (ties go to the later line). Identity when snapping is off or the grid is Off.
// snapDelta(origin, rawDelta) — Ableton's relative snap while dragging. Rule implemented:
//     anchor = lineAtOrBefore(origin);  delta = nearestLine(anchor + rawDelta) − anchor
// so an object on the grid snaps to grid lines, and an object off the grid moves by whole grid steps keeping
// its offset from the line before it. A drag shorter than half a cell does not move it.
//
// ---- Quantize -----------------------------------------------------------------------------------------------
// quantize(start, end, amount%, mode) uses the grid lines (ignoring the snap toggle, so Ctrl+U works with snap
// off, like Live):
//     start' = start + round(amount · (nearestLine(start) − start))
//     StartOnly: end' = start' + (end − start)                  (the note moves, length kept; Live's default)
//     StartEnd:  qe = nearestLine(end), or the first line after nearestLine(start) if that would not be later;
//                end' = end + round(amount · (qe − end)), and always end' ≥ start' + 1.
//
// All queries are RT-safe (no allocation, bounded work: a binary search over the signature changes plus O(1)
// arithmetic per line). A Grid holds a pointer to its TimeSigMap, which must outlive it and its cursors.

#include <cstddef>
#include <cstdint>
#include <span>

#include "pq/core/time.h"

namespace pq {

enum class GridMode : uint8_t { Adaptive, Fixed };

inline constexpr int kGridLevelMin = -3;   // Narrowest
inline constexpr int kGridLevelMax = 3;    // Widest
inline constexpr int kGridFixedExpMin = -3;  // 8 bars
inline constexpr int kGridFixedExpMax = 6;   // 1/64
inline constexpr int kGridMaxTuplet = 32;
inline constexpr int64_t kGridMaxBarsPerCell = int64_t{1} << 16;
inline constexpr double kGridAdaptiveBasePx = 24.0;  // minimum cell width at level 0 (Medium), logical px

// The user's grid settings (what the right-click grid menu and Ctrl+1…5 edit).
struct GridSpec {
  GridMode mode = GridMode::Adaptive;
  int8_t level = 0;     // adaptive density: −3 Narrowest … 0 Medium … +3 Widest
  int8_t fixedExp = 4;  // fixed value: e ≥ 0 → 1/2^e note (0 = 1/1 … 6 = 1/64); e < 0 → 2^−e bars (−3 = 8 bars)
  uint8_t tuplet = 1;   // 1 straight, 3 triplet, 5, 7, 11 (the menu values; any 1…kGridMaxTuplet works)
  bool snap = true;     // Ctrl+4
  bool off = false;     // grid "Off": no lines, no snap (the remembered settings stay for when it is turned on)
  friend bool operator==(const GridSpec&, const GridSpec&) = default;

  // Keyboard commands. They edit the active mode's value and leave `off` alone, except toggledMode.
  GridSpec narrowed() const noexcept;       // Ctrl+1: finer (adaptive level − 1 / fixed value halved)
  GridSpec widened() const noexcept;        // Ctrl+2: coarser
  GridSpec tripletToggled() const noexcept; // Ctrl+3: triplet ↔ straight (any other tuplet → triplet)
  GridSpec snapToggled() const noexcept;    // Ctrl+4
  GridSpec modeToggled() const noexcept;    // Ctrl+5: adaptive ↔ fixed (turns an Off grid on)
};

// Minimum adaptive cell width in logical pixels: kGridAdaptiveBasePx · 2^level (3, 6, 12, 24, 48, 96, 192 px).
double adaptiveMinCellPx(int level) noexcept;     // RT-safe
const char* adaptiveLevelName(int level) noexcept;  // "Narrowest" … "Widest" (static strings)
const char* fixedValueName(int fixedExp) noexcept;  // "8 Bars" … "1/64" (static strings)

// A resolved grid cell. Exactly one of bars / denominator is > 0; both are 0 for an Off grid.
struct GridDivision {
  int32_t bars = 0;         // cells of `bars` bars
  int32_t denominator = 0;  // straight note value 1/denominator
  int32_t tuplet = 1;
  friend bool operator==(const GridDivision&, const GridDivision&) = default;
};

// "1/16", "1/8T", "1/16 7:4", "1 Bar", "4 Bars 3:2", "Off". Returns the length written (not NUL-terminated);
// 0 if `out` is too small. 24 chars always suffice.
std::size_t formatGridDivision(const GridDivision& d, std::span<char> out) noexcept;  // RT-safe

enum class GridStrength : uint8_t { Bar, Beat, Subdivision, Tuplet };

struct GridLine {
  Ticks tick = 0;
  int64_t bar = 0;       // 0-based bar index containing the line
  Ticks tickInBar = 0;   // 0 on bar lines
  TimeSignature sig;     // signature of that bar
  GridStrength strength = GridStrength::Bar;
};

// A ruler position as displayed: bar (1-based; bars before the start count −1, −2 …, there is no bar 0),
// beat (1-based), sixteenth within the beat (1-based; one sixteenth = kTicksPerWhole / 16 ticks), and the ticks
// left over inside that sixteenth (tuplet and 1/32 positions).
struct RulerPosition {
  int64_t bar = 1;
  int32_t beat = 1;
  int32_t sixteenth = 1;
  Ticks remainder = 0;
  friend bool operator==(const RulerPosition&, const RulerPosition&) = default;
};
RulerPosition rulerPosition(const TimeSigMap& sigs, Ticks t) noexcept;  // RT-safe
RulerPosition rulerPosition(const GridLine& line) noexcept;             // RT-safe; no map lookup
// "17" on a bar line, "17.3" on a beat, "17.3.2" otherwise (the remainder is not shown). Returns the length
// written (not NUL-terminated); 0 if `out` is too small. kRulerLabelMaxChars always suffice.
inline constexpr std::size_t kRulerLabelMaxChars = 48;
std::size_t formatRulerPosition(const RulerPosition& p, std::span<char> out) noexcept;  // RT-safe

struct TickRange {
  Ticks start = 0;
  Ticks end = 0;
  friend bool operator==(const TickRange&, const TickRange&) = default;
};

namespace detail {
// Line layout of one time-signature segment (internal; shared by Grid and GridLineCursor).
struct GridLayout {
  std::size_t seg = 0;
  int64_t segBar = 0;
  Ticks segTick = 0;
  Ticks segEndTick = 0;  // INT64_MAX for the last segment
  Ticks barLen = 0;
  Ticks beatLen = 0;
  TimeSignature sig;
  int64_t groupBars = 1;  // lines restart every groupBars bars
  Ticks groupLen = 0;     // groupBars · barLen
  int64_t num = 1;        // line spacing = num / den ticks (reduced)
  int64_t den = 1;
  int64_t tuplet = 1;     // lines per tuplet group
};
}  // namespace detail

class GridLineCursor;

// A grid at a zoom level, bound to a time-signature map. Cheap to copy (~40 bytes); build one per frame/gesture.
class Grid {
public:
  // pxPerTick: horizontal zoom (only used by adaptive grids; ≤ 0 or non-finite → the coarsest cell).
  Grid(const GridSpec& spec, const TimeSigMap& sigs, double pxPerTick) noexcept;  // RT-safe
  // An adaptive straight grid with an explicit minimum cell width, e.g. for ruler label density.
  static Grid adaptive(double minCellPx, const TimeSigMap& sigs, double pxPerTick, int tuplet = 1) noexcept;

  bool enabled() const noexcept { return kind_ != Kind::Off; }        // has lines
  bool snapping() const noexcept { return enabled() && snap_; }       // snap() / snapDelta() act

  GridDivision divisionFor(TimeSignature sig) const noexcept;  // RT-safe
  GridDivision divisionAt(Ticks t) const noexcept;             // RT-safe

  // Raw line queries (ignore the snap toggle). On an Off grid every tick counts as a line.
  Ticks lineAtOrBefore(Ticks t) const noexcept;  // RT-safe
  Ticks lineAtOrAfter(Ticks t) const noexcept;   // RT-safe
  Ticks lineAfter(Ticks t) const noexcept;       // RT-safe; first line > t
  Ticks nearestLine(Ticks t) const noexcept;     // RT-safe; ties → later line
  TickRange cellAt(Ticks t) const noexcept;      // RT-safe; [lineAtOrBefore(t), lineAfter(t))

  Ticks snap(Ticks t) const noexcept;                        // RT-safe; nearestLine if snapping, else t
  Ticks snapDelta(Ticks origin, Ticks rawDelta) const noexcept;  // RT-safe; see the header comment

  // Lines with from ≤ tick < to, in order. Lazy; allocates nothing.
  GridLineCursor lines(Ticks from, Ticks to) const noexcept;  // RT-safe

private:
  friend class GridLineCursor;
  enum class Kind : uint8_t { Off, Adaptive, Fixed };
  struct Bracket {
    Ticks before;  // last line ≤ t
    Ticks after;   // first line ≥ t
  };
  Grid() = default;
  detail::GridLayout layoutFor(std::size_t seg) const noexcept;
  Bracket bracket(Ticks t) const noexcept;

  const TimeSigMap* sigs_ = nullptr;
  double pxPerTick_ = 0.0;
  double minCellPx_ = kGridAdaptiveBasePx;
  Kind kind_ = Kind::Off;
  int8_t fixedExp_ = 4;
  uint8_t tuplet_ = 1;
  bool snap_ = false;
};

// Quantize a note/clip [start, end) to the grid (see the header comment). amountPercent is clamped to 0…100.
enum class QuantizeMode : uint8_t { StartOnly, StartEnd };
TickRange quantize(const Grid& grid, Ticks start, Ticks end, int amountPercent, QuantizeMode mode) noexcept;  // RT-safe

// Allocation-free lazy iterator over grid lines. Either call next() in a loop or use range-for.
class GridLineCursor {
public:
  bool next(GridLine& out) noexcept;  // RT-safe; false when done

  struct Sentinel {};
  class Iterator {
  public:
    const GridLine& operator*() const noexcept { return line_; }
    const GridLine* operator->() const noexcept { return &line_; }
    Iterator& operator++() noexcept { valid_ = cursor_->next(line_); return *this; }
    bool operator==(Sentinel) const noexcept { return !valid_; }
  private:
    friend class GridLineCursor;
    GridLineCursor* cursor_ = nullptr;
    GridLine line_{};
    bool valid_ = false;
  };
  Iterator begin() noexcept { Iterator it; it.cursor_ = this; it.valid_ = next(it.line_); return it; }
  Sentinel end() const noexcept { return {}; }

private:
  friend class Grid;
  GridLineCursor() = default;
  void advanceGroup() noexcept;

  Grid grid_;
  detail::GridLayout L_;
  int64_t groupBar_ = 0;
  Ticks groupStart_ = 0;
  Ticks groupEnd_ = 0;
  int64_t j_ = 0;
  Ticks to_ = 0;
  bool done_ = true;
};

}  // namespace pq
