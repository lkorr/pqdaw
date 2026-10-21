#pragma once

// The piano roll's pitch axis (T3.11): which rows the note editor shows and where each one sits on screen.
//
// One row per degree of the clip's tuning (EDO mode: one row per EDO step; MOS / Scala: one row per scale degree),
// highest pitch at the top. A row is identified by its *absolute degree* (equave · size + degree within the equave,
// the row a Degree pitch identity lands on) and carries its pitch in cents from the tuning's reference, so notes of
// any identity can be placed:
//   - Degree notes sit on their row (rowOfDegree: O(1) when no fold hides rows, a binary search otherwise).
//   - Ratio / Cents notes sit at their true pitch (placeCents): the centre is interpolated linearly in cents between
//     the two neighbouring row centres, the block takes the nearest row's height, and the distance to that row's
//     pitch says whether the note is off the grid.
//
// Geometry is stored per row (top, centre, bottom in content pixels, y down from the top of the highest row), not
// derived from a uniform pitch, so other layouts slot in by building different extents:
//   - RowLayout::Uniform (this task): every row is rowPx tall and rows butt against each other (EDO mode, and
//     Ableton's chromatic look for any tuning). The centre is the middle of the row.
//   - T3.12 adds Proportional (rows at true cents with mos-roll.js's asymmetric half-height rule: a row's top and
//     bottom extents come from the distance to each neighbour, so the centre need not be the middle and rows may
//     leave gaps) and Equidistant scale layouts as further RowLayout values filled in build(); everything that reads
//     a RowModel (gutter, note area, hit tests, placement) already uses only top / centre / bottom and the cents.
//
// Folding (SPEC §9.4 "fold to used notes / fold to scale"): only the rows in use, or only the rows in the highlight
// scale, are listed (still in pitch order, packed one after the other).
//
// Building allocates (UI thread, once per tuning / zoom / fold change, never per frame). Every query is const,
// allocation-free and O(1) or O(log rows).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pq/tuning/tuning.h"

namespace pq::views::roll {

// Row classification (bits of PitchRow::flags).
enum RowFlags : uint8_t {
  kRowEquaveStart = 1u << 0,  // degree 0 of an equave: the strong equave marker runs along its bottom edge
  kRowNatural = 1u << 1,      // a "white key": in the tuning's diatonic set (EDO), every degree of a MOS / scale
  kRowInScale = 1u << 2,      // in the highlight scale (scale highlighting, fold to scale)
  kRowNamed = 1u << 3,        // has a 12edo-style note name (C, C#, … on a 12edo-compatible EDO)
};

struct PitchRow {
  int64_t degree = 0;  // absolute degree: equave · size + degree within the equave
  double cents = 0.0;  // pitch from the tuning's reference (degree 0 of equave 0)
  int32_t top = 0;     // content px (device), y down; top ≤ centre < bottom
  int32_t centre = 0;
  int32_t bottom = 0;
  uint8_t flags = 0;
  int32_t height() const noexcept { return bottom - top; }
};

enum class RowLayout : uint8_t {
  Uniform,  // every row rowPx tall, packed (EDO mode)
};

enum class Fold : uint8_t {
  None,       // every degree in [lowCents, highCents] (and every used degree)
  UsedNotes,  // only the degrees in `used` (fold to used notes)
  Scale,      // only the degrees in the highlight scale within the range (fold to scale)
};

struct RowOptions {
  RowLayout layout = RowLayout::Uniform;
  int32_t rowPx = 12;  // device px per row (Uniform)
  Fold fold = Fold::None;
  // Pitch range the unfolded roll covers (it always also covers every degree in `used`).
  double lowCents = -6000.0;
  double highCents = 7600.0;
  // Absolute degrees the clip's notes use (Degree notes, and the nearest row of every Ratio / Cents note), sorted
  // ascending, unique. Fold::UsedNotes shows exactly these.
  std::span<const int64_t> used;
  // Highlight scale: one entry per degree within the equave (non-zero = in the scale). Empty: every degree.
  std::span<const uint8_t> scale;
};

// Where a note of a given pitch in cents is drawn.
struct CentsPlacement {
  int32_t top = 0, bottom = 0;  // content px (the nearest row's height, centred on the interpolated pitch)
  int32_t centre = 0;
  int64_t nearestDegree = 0;    // the row whose pitch is closest (the note's row for editing)
  double offsetCents = 0.0;     // pitch − that row's pitch
};

// Notes whose pitch is within this many cents of a row's pitch are "on" it (not marked off-grid). Far below any
// audible difference, far above float rounding of a degree's cents.
inline constexpr double kOnGridToleranceCents = 1e-3;

// Degrees per equave of a tuning (EDO n, MOS / scale size).
int32_t degreesPerEquave(const tuning::Tuning& t) noexcept;
// Cents of absolute degree `degree` from the reference (stretch included for MOS).
double degreeCents(const tuning::Tuning& t, int64_t degree) noexcept;
// Absolute degree of a Degree pitch identity (equave · size + degree).
int64_t absoluteDegree(const tuning::Tuning& t, int32_t degree, int32_t equave) noexcept;
// The default highlight scale (one entry per degree within the equave): for an EDO of the octave, the diatonic major
// scale on degree 0 built from the EDO's patent fifth (F C G D A E B; 12edo → C major, 31edo → 0 5 10 13 18 23 28);
// every degree when that chain does not give 7 distinct degrees, for other equaves, and for MOS / scale tunings
// (their degrees are the scale).
std::vector<uint8_t> defaultHighlightScale(const tuning::Tuning& t);
// n when the tuning is an EDO of the octave whose size is a multiple of 12 (it contains 12edo, so rows at multiples
// of n/12 get C-style names, Q2), else 0.
uint32_t twelveCompatibleEdo(const tuning::Tuning& t) noexcept;

class RowModel {
public:
  RowModel() = default;
  static RowModel build(const tuning::Tuning& tuning, const RowOptions& options);

  std::span<const PitchRow> rows() const noexcept { return rows_; }
  size_t size() const noexcept { return rows_.size(); }
  bool empty() const noexcept { return rows_.empty(); }
  const PitchRow& row(size_t i) const noexcept { return rows_[i]; }
  int32_t contentHeight() const noexcept { return rows_.empty() ? 0 : rows_.back().bottom; }
  int32_t degreesPerEquave() const noexcept { return size_; }
  // Row label ("C3", "7\31", …; empty when the row has none) and, on equave rows, the equave number ("3").
  const std::string& label(size_t i) const noexcept { return labels_[i]; }
  const std::string& equaveLabel(size_t i) const noexcept { return equaveLabels_[i]; }

  // Index of the row of absolute degree `degree`, or −1 if it is not shown (out of range or folded away).
  int64_t indexOfDegree(int64_t degree) const noexcept;
  const PitchRow* rowOfDegree(int64_t degree) const noexcept {
    const int64_t i = indexOfDegree(degree);
    return i < 0 ? nullptr : &rows_[static_cast<size_t>(i)];
  }
  // Placement of a pitch in cents (see the file comment). The model must not be empty.
  CentsPlacement placeCents(double cents) const noexcept;
  // The row under content y (clamped to the first / last row; in a gap the nearer row). The model must not be empty.
  size_t indexAtY(int32_t y) const noexcept;
  // Rows intersecting content [y0, y1): [first, last).
  std::pair<size_t, size_t> rowsIn(int32_t y0, int32_t y1) const noexcept;

private:
  std::vector<PitchRow> rows_;  // highest pitch first (ascending y)
  std::vector<std::string> labels_, equaveLabels_;
  int32_t size_ = 0;
  bool contiguous_ = false;  // rows_[i].degree == topDegree_ − i for every i
  int64_t topDegree_ = 0;
};

}  // namespace pq::views::roll
