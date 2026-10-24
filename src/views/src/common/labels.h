#pragma once

// Label collision (T3.x-fix-look, M3 review L18 / M18): one helper for every view that places text labels along an
// axis (the MOS chart's EDO and cents labels, the piano roll's harmonic numbers and key labels). A label is placed only
// if its extent, grown by a gap, does not touch one already placed; otherwise it is skipped (the caller draws nothing),
// so labels never overprint ("23\40 7\12 29\49" run together, "132L 11s"). Placement order is priority order: put the
// important labels first (the selected marker, the equave, the fundamental).
//
// Cheap by design: a label set holds a few dozen extents (labels are at least a text height apart once placed), so a
// linear scan per placement is O(placed) with no allocation once warm (the vector keeps its capacity across paints).
// Deterministic for a given input order, so partial and full repaints agree as long as every pass places the same
// labels in the same order (callers place over the whole widget, not just the painted part).

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace pq::views::paint {

class LabelSpace {
public:
  // `gap`: device px kept free between two labels.
  explicit LabelSpace(int32_t gap = 2) noexcept : gap_(gap) {}
  void clear() noexcept { taken_.clear(); }
  void setGap(int32_t gap) noexcept { gap_ = gap; }
  // True (and reserved) when [lo, hi) is free; false when it would touch a placed label.
  bool place(int32_t lo, int32_t hi) {
    if (hi < lo) std::swap(lo, hi);
    for (const auto& [a, b] : taken_)
      if (lo < b + gap_ && a < hi + gap_) return false;
    taken_.push_back({lo, hi});
    return true;
  }
  // Whether [lo, hi) is free, without reserving it.
  bool free(int32_t lo, int32_t hi) const noexcept {
    if (hi < lo) std::swap(lo, hi);
    for (const auto& [a, b] : taken_)
      if (lo < b + gap_ && a < hi + gap_) return false;
    return true;
  }
  size_t size() const noexcept { return taken_.size(); }

private:
  std::vector<std::pair<int32_t, int32_t>> taken_;
  int32_t gap_;
};

}  // namespace pq::views::paint
