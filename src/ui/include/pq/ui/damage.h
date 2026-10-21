#pragma once

// Damage tracking for redraw-on-change (SPEC §11: render only on change or animation; idle UI ≈ 0% CPU/GPU).
//
// Anything that changes what is on screen reports the area it touched; the frame loop draws only when
// something is damaged or an animation is running, and otherwise blocks in the OS event wait. The union
// rect is kept for future partial-redraw use (scissored re-render into a preserved image); today a damaged
// frame is redrawn whole, because swapchain images do not keep their previous contents.

#include <algorithm>

#include "pq/ui/draw_list.h"

namespace pq::ui {

class Damage {
public:
  void add(const Rect& r) noexcept {
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
    if (!any_) {
      bounds_ = r;
      any_ = true;
      return;
    }
    bounds_.x0 = std::min(bounds_.x0, r.x0);
    bounds_.y0 = std::min(bounds_.y0, r.y0);
    bounds_.x1 = std::max(bounds_.x1, r.x1);
    bounds_.y1 = std::max(bounds_.y1, r.y1);
  }
  // Whole-target damage (resize, expose, theme change).
  void addAll() noexcept {
    any_ = true;
    full_ = true;
  }
  // Continuous animation (meters, scopes): every frame is damaged until switched off.
  void setAnimating(bool on) noexcept { animating_ = on; }
  bool animating() const noexcept { return animating_; }

  bool needsFrame() const noexcept { return any_ || animating_; }
  bool full() const noexcept { return full_ || animating_; }
  // Union of reported rects (meaningless when full()).
  Rect bounds() const noexcept { return bounds_; }

  // Call after a frame was drawn.
  void clear() noexcept {
    any_ = false;
    full_ = false;
    bounds_ = {};
  }

private:
  Rect bounds_{};
  bool any_ = false;
  bool full_ = false;
  bool animating_ = false;
};

}  // namespace pq::ui
