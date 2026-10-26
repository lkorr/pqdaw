#pragma once

// The pointer-drag rules every editor shares (M3 review M17, first step; private to pq_views): the click slop (how far
// the pointer may wander before a press becomes a drag), a press → drag tracker and the rubber band of a range drag.
// The arrangement editor, its automation lanes, the piano roll and the circuit canvas use these instead of their own
// copies. Auto-scroll and the selection model stay per view for now (the review's split of ViewContext into services is
// later work).

#include <algorithm>
#include <cmath>

#include "pq/ui/input.h"
#include "pq/ui/widget.h"

namespace pq::views::drag {

// Logical px the pointer may move between press and release and still be a click (Live's feel).
inline constexpr float kClickSlop = 3.0f;

// Has the pointer moved past the click slop (device px, at UI scale `scale`)?
inline bool beyondSlop(ui::Vec2 a, ui::Vec2 b, float scale) noexcept {
  return std::fabs(a.x - b.x) > kClickSlop * scale || std::fabs(a.y - b.y) > kClickSlop * scale;
}

// A press that may become a drag.
struct Tracker {
  bool active = false;
  bool moved = false;  // past the slop since the press (stays true)
  ui::PointerButton button = ui::PointerButton::None;
  ui::Mods mods = ui::mod::kNone;
  ui::Vec2 press{0, 0};
  ui::Vec2 last{0, 0};

  void begin(const ui::PointerEvent& e) noexcept {
    active = true;
    moved = false;
    button = e.button;
    mods = e.mods;
    press = last = e.pos;
  }
  // Returns true when this move started the drag (the first move past the slop).
  bool move(ui::Vec2 p, float scale) noexcept {
    last = p;
    if (moved || !beyondSlop(p, press, scale)) return false;
    moved = true;
    return true;
  }
  void end() noexcept {
    active = false;
    moved = false;
    button = ui::PointerButton::None;
  }
};

// The rectangle between the press and the current point (device px, normalised).
inline ui::IRect rubberBand(ui::Vec2 a, ui::Vec2 b) noexcept {
  const float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y), x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
  return {static_cast<int32_t>(std::floor(x0)), static_cast<int32_t>(std::floor(y0)),
          static_cast<int32_t>(std::ceil(x1) - std::floor(x0)), static_cast<int32_t>(std::ceil(y1) - std::floor(y0))};
}

}  // namespace pq::views::drag
