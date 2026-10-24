#pragma once

// The piano roll's pitch axis (T3.11, T3.12): which rows the note editor shows and where each one sits on screen.
//
// One row per degree of the clip's tuning (EDO mode: one row per EDO step; MOS / Scala: one row per scale degree),
// highest pitch at the top. A row is identified by its *absolute degree* (equave · size + degree within the equave,
// the row a Degree pitch identity lands on) and carries its pitch in cents from the tuning's reference, so notes of
// any identity can be placed:
//   - Degree notes sit on their row (rowOfDegree: O(1) when no fold hides rows, a binary search otherwise).
//   - Ratio / Cents notes sit at their true pitch (placeCents): the centre is interpolated linearly in cents between
//     the two neighbouring row centres, the block takes the nearest row's extents, and the distance to that row's
//     pitch says whether the note is off the grid (Q15: option A in every layout).
//
// Geometry is stored per row (top, centre, bottom in content pixels, y down from the top of the content), not derived
// from a uniform pitch. Three layouts (RowLayout):
//   - Uniform (T3.11): every row is rowPx tall and rows butt against each other (EDO mode). The centre is the
//     middle of the row.
//   - Proportional (T3.12, the default for MOS and Scala tunings, SPEC §4.1): rows sit at true cents (px per cent =
//     rowPx · size / equave, so an equave is as tall as in the other layouts) with mos-roll.js's layoutRows rule:
//     a nominal block of min(22, max(4, 0.62 · lane)) logical px, and each side of a row extends toward its nearest
//     neighbour at another pitch by at most half the nominal block and at most half the space to it less half a
//     1-px gap (the gap shrinks to a quarter of the space under 4 px). So blocks are asymmetric, rows leave gaps
//     (the interval), and as a MOS collapses its small steps close up while the large-step side of a row keeps its
//     bulk. Rows at the same pitch share their extents. A scale whose degrees are not in ascending pitch order (a
//     Scala file may do that) is laid out Equidistant instead (layout() reports it).
//   - Equidistant (T3.12): every degree gets an equal lane (mos-roll.js's even cents), a chromatic roll in the
//     scale's own steps.
// Rows are rounded to whole device px (each edge of mos-roll.js's float geometry to the nearest px).
//
// Step classes (scale layouts): each degree is classified by the size of the step above it (mos-roll.js getRows'
// stepCents), bucketed at 0.01 cents as mos-explorer's circle does: class 0 is the largest step (L), the last the
// smallest (s), MODMOS / Scala sizes in between. A row's z is its step's rank, so small-step notes are painted under
// large-step ones (SPEC §4.1).
//
// Live morph (T3.12): retune() moves the rows to another tuning with the same degrees per equave (a MOS generator
// change) in place: new cents, step classes and geometry, no allocation; the row set, labels and flags are kept.
//
// Folding (SPEC §9.4 "fold to used notes / fold to scale"): only the rows in use, or only the rows in the highlight
// scale, are listed (still in pitch order, packed one after the other in Uniform / Equidistant).
//
// Building allocates (UI thread, once per tuning / zoom / fold change). Every query and retune() are allocation-free,
// O(1) or O(log rows) (retune: O(rows)).

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
  int32_t top = 0;     // content px (device), y down; top ≤ centre ≤ bottom (Uniform: centre < bottom)
  int32_t centre = 0;
  int32_t bottom = 0;
  uint8_t flags = 0;
  uint8_t stepClass = 0;  // index into RowModel::stepClasses() (scale layouts; 0 otherwise)
  uint8_t z = 0;          // paint order: larger steps over smaller ones (scale layouts; 0 otherwise)
  int32_t height() const noexcept { return bottom - top; }
};

enum class RowLayout : uint8_t {
  Uniform,       // every row rowPx tall, packed (EDO mode)
  Proportional,  // rows at true cents, mos-roll.js's asymmetric blocks (MOS / Scala default)
  Equidistant,   // one equal lane per degree (MOS / Scala)
};

// mos-roll.js's block constants (logical px; RowOptions::pxScale converts them to device px).
inline constexpr double kRollNoteFill = 0.62;     // ROLL_NOTE_FILL: nominal block = this × the lane
inline constexpr double kRollMaxNoteH = 22.0;     // ROLL_MAX_NOTE_H
inline constexpr double kRollLegibleNoteH = 4.0;  // ROLL_LEGIBLE_NOTE_H: nominal floor
inline constexpr double kRollMinNoteGap = 1.0;    // ROLL_MIN_NOTE_GAP

// One step size of a scale (scale layouts).
struct StepClass {
  double cents = 0.0;  // the step size, bucketed at 0.01 cents
};

enum class Fold : uint8_t {
  None,       // every degree in [lowCents, highCents] (and every used degree)
  UsedNotes,  // only the degrees in `used` (fold to used notes)
  Scale,      // only the degrees in the highlight scale within the range (fold to scale)
};

struct RowOptions {
  // Uniform for an EDO whatever is asked; MOS / Scale tunings take Proportional or Equidistant (Uniform asked for a
  // scale means Equidistant).
  RowLayout layout = RowLayout::Uniform;
  int32_t rowPx = 12;  // device px per row (Uniform); the average row (lane) height of the scale layouts
  // Scale layouts: px per cent (0 = rowPx · size / equave). Tests set mos-roll.js's exact value.
  double pxPerCent = 0.0;
  // Device px per logical px, for the block constants (the UI scale).
  double pxScale = 1.0;
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
  int32_t top = 0, bottom = 0;  // content px (the nearest row's extents around the interpolated pitch)
  int32_t centre = 0;
  int64_t nearestDegree = 0;    // the row whose pitch is closest (the note's row for editing)
  double offsetCents = 0.0;     // pitch − that row's pitch
  uint8_t z = 0;                // the nearest row's z
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
  int32_t contentHeight() const noexcept { return contentHeight_; }
  int32_t degreesPerEquave() const noexcept { return size_; }
  // The layout the rows were built with (Uniform for EDOs; Equidistant for an unordered scale asked Proportional).
  RowLayout layout() const noexcept { return layout_; }
  bool scaleLayout() const noexcept { return layout_ != RowLayout::Uniform; }
  // Average row height in device px (the lane: rowPx, or px per cent × equave / size), rounded, ≥ 1.
  int32_t laneHeight() const noexcept { return laneHeight_; }
  double pxPerCent() const noexcept { return ppc_; }
  // Step classes of the scale, largest first (one class when every step is equal; empty in Uniform).
  std::span<const StepClass> stepClasses() const noexcept { return classes_; }
  // L / s of the scale (largest / smallest step; 20 when s < 0.01 cents, as mos-explorer), 1 without classes.
  double hardness() const noexcept;
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
  // Pitch in cents at content y: interpolated linearly between the row centres around y (extrapolated past the
  // ends). The model must not be empty.
  double centsAtY(double y) const noexcept;
  // Scale layouts: row i's centre y before rounding (tests compare it with mos-roll.js); Uniform: the centre.
  double exactCentreY(size_t i) const noexcept { return y_.empty() ? rows_[i].centre : y_[i]; }
  // Live morph: the same rows for `tuning`, which must have the same degrees per equave as the one built with.
  // Recomputes cents, step classes and geometry in place (allocation-free once built).
  void retune(const tuning::Tuning& tuning) noexcept;
  // The row under content y (clamped to the first / last row; in a gap the nearer row). The model must not be empty.
  size_t indexAtY(int32_t y) const noexcept;
  // Rows intersecting content [y0, y1): [first, last).
  std::pair<size_t, size_t> rowsIn(int32_t y0, int32_t y1) const noexcept;

private:
  void classify(const tuning::Tuning& t) noexcept;
  void layoutRows() noexcept;

  std::vector<PitchRow> rows_;  // highest pitch first (ascending y)
  std::vector<std::string> labels_, equaveLabels_;
  std::vector<StepClass> classes_;
  std::vector<double> steps_;  // scale layouts: per degree within the equave, the step above it (bucketed)
  std::vector<uint8_t> degClass_;  // scale layouts: per degree within the equave, its step class
  std::vector<double> y_;      // scale layouts: the exact (unrounded) centre y of each row
  int32_t size_ = 0;
  bool contiguous_ = false;  // rows_[i].degree == topDegree_ − i for every i
  int64_t topDegree_ = 0;
  RowLayout layout_ = RowLayout::Uniform;
  double ppc_ = 0.0;           // px per cent
  double lane_ = 0.0;          // lane height, device px (fractional)
  double pxScale_ = 1.0;
  double originCents_ = 0.0;   // Proportional: the pitch at content y = pad_
  double pad_ = 0.0;           // scale layouts: content y of originCents_ / of the top row's lane centre
  int32_t laneHeight_ = 0;
  int32_t contentHeight_ = 0;
};

}  // namespace pq::views::roll
