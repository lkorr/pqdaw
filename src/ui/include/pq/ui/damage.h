#pragma once

// Damage tracking for redraw-on-change and partial redraw (SPEC §11: render only on change or animation; idle
// UI ≈ 0% CPU/GPU; T0.7-fix1: redraw only the damaged pixels).
//
// Anything that changes what is on screen reports the area it touched; the frame loop draws only when
// something is damaged or an animation is running, and otherwise blocks in the OS event wait. Reported rects
// are rounded outwards to whole pixels and kept as a small set of *disjoint* rects (overlapping or exactly
// tiling rects merge into their bounding box; past kMaxRects the pair whose merge adds the least area is
// merged). The frame then clears and redraws only those rects of the persistent colour target
// (pq/ui/renderer.h FrameDesc), each pixel exactly once, so the result equals a full redraw.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

#include "pq/ui/draw_list.h"

namespace pq::ui {

class Damage {
public:
  static constexpr uint32_t kMaxRects = 8;

  // Rounded outwards to whole pixels (floor of the low edges, ceil of the high edges).
  void add(const Rect& r) noexcept {
    if (!(r.x1 > r.x0) || !(r.y1 > r.y0)) return;
    addPixels(ClipRect{static_cast<int32_t>(std::floor(r.x0)), static_cast<int32_t>(std::floor(r.y0)),
                 static_cast<int32_t>(std::ceil(r.x1)), static_cast<int32_t>(std::ceil(r.y1))});
  }
  // Whole-pixel rect, as is.
  void addPixels(ClipRect r) noexcept {
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
    any_ = true;
    if (full_) return;
    // Absorb every rect r overlaps (or tiles with exactly); the grown r may now touch others: repeat.
    for (uint32_t i = 0; i < count_;) {
      if (mergeable(rects_[i], r)) {
        r = unite(rects_[i], r);
        rects_[i] = rects_[--count_];
        i = 0;
        continue;
      }
      ++i;
    }
    while (count_ == kMaxRects) {
      // Full: fold r into the rect whose bounding box with it grows the damaged area least, and absorb again.
      uint32_t best = 0;
      int64_t bestGrowth = INT64_MAX;
      for (uint32_t i = 0; i < count_; ++i) {
        const int64_t g = area(unite(rects_[i], r)) - area(rects_[i]) - area(r);
        if (g < bestGrowth) {
          bestGrowth = g;
          best = i;
        }
      }
      r = unite(rects_[best], r);
      rects_[best] = rects_[--count_];
      for (uint32_t i = 0; i < count_;) {
        if (mergeable(rects_[i], r)) {
          r = unite(rects_[i], r);
          rects_[i] = rects_[--count_];
          i = 0;
          continue;
        }
        ++i;
      }
    }
    rects_[count_++] = r;
  }
  // Whole-target damage (resize, expose, theme change).
  void addAll() noexcept {
    any_ = true;
    full_ = true;
    count_ = 0;
  }
  // Continuous whole-target animation (test pattern blobs): every frame is fully damaged until switched off.
  // Animations that touch a known area (a playhead, a meter) report their rects with add() instead.
  void setAnimating(bool on) noexcept { animating_ = on; }
  bool animating() const noexcept { return animating_; }

  bool needsFrame() const noexcept { return any_ || animating_; }
  bool full() const noexcept { return full_ || animating_; }
  // The disjoint damage rects in whole pixels (not clamped to any target; meaningless when full()).
  std::span<const ClipRect> rects() const noexcept { return {rects_, count_}; }
  // Pixels covered by rects().
  int64_t area() const noexcept {
    int64_t a = 0;
    for (uint32_t i = 0; i < count_; ++i) a += area(rects_[i]);
    return a;
  }
  // Bounding box of rects() (meaningless when full()).
  Rect bounds() const noexcept {
    if (count_ == 0) return {};
    ClipRect b = rects_[0];
    for (uint32_t i = 1; i < count_; ++i) b = unite(b, rects_[i]);
    return {static_cast<float>(b.x0), static_cast<float>(b.y0), static_cast<float>(b.x1), static_cast<float>(b.y1)};
  }

  // Call after a frame was drawn.
  void clear() noexcept {
    any_ = false;
    full_ = false;
    count_ = 0;
  }

  static int64_t area(const ClipRect& r) noexcept { return int64_t(r.x1 - r.x0) * int64_t(r.y1 - r.y0); }
  static ClipRect unite(const ClipRect& a, const ClipRect& b) noexcept {
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
  }

private:
  // Overlapping, or edge-adjacent with a common side span (their union is exactly a rect).
  static bool mergeable(const ClipRect& a, const ClipRect& b) noexcept {
    if (a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1) return true;
    return area(unite(a, b)) == area(a) + area(b);
  }

  ClipRect rects_[kMaxRects] = {};
  uint32_t count_ = 0;
  bool any_ = false;
  bool full_ = false;
  bool animating_ = false;
};

}  // namespace pq::ui
