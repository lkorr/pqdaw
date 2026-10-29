#pragma once

// The pointer-drag rules every editor shares (M3 review M17, first step; private to pq_views): the click slop (how far
// the pointer may wander before a press becomes a drag), a press → drag tracker and the rubber band of a range drag.
// The arrangement editor, its automation lanes, the piano roll and the circuit canvas use these instead of their own
// copies. Auto-scroll and the selection model stay per view for now (the review's split of ViewContext into services is
// later work).

#include <algorithm>
#include <cmath>

#include "pq/core/grid.h"
#include "pq/core/time.h"
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

// ---- keyboard nudges (T3.x-fix-look, T4.16b) -------------------------------------------------------------------

// Alt+Left / Right (Live: nudge without the grid): a 1/128 note.
inline constexpr Ticks kFineNudge = kTicksPerWhole / 128;
// Shift / Alt arrows with the grid off step by a sixteenth.
inline constexpr Ticks kGridOffStep = kTicksPerWhole / 16;

// `from` moved by `cells` grid cells: to the next / previous grid line each step (from a point off the grid the
// first step reaches the nearest line on that side), or by kGridOffStep with the grid off.
inline Ticks stepByCells(const Grid& g, Ticks from, int32_t cells) noexcept {
  Ticks t = from;
  for (int32_t i = 0; i < (cells < 0 ? -cells : cells); ++i) {
    if (!g.enabled()) t += (cells > 0 ? 1 : -1) * kGridOffStep;
    else t = cells > 0 ? g.lineAfter(t) : g.lineAtOrBefore(t - 1);
  }
  return t;
}

// The modifiers held after a key event: a modifier key's own bit follows its press / release (platforms differ in
// whether the event's state already includes it). Views use this to re-evaluate a hover cursor when e.g. Alt is
// pressed with the pointer still (T4.16b).
inline ui::Mods modsAfterKey(const ui::KeyEvent& e) noexcept {
  ui::Mods m = static_cast<ui::Mods>(e.mods & ui::mod::kChordMask);
  ui::Mods bit = 0;
  if (e.key == ui::key::kLShift || e.key == ui::key::kRShift) bit = ui::mod::kShift;
  else if (e.key == ui::key::kLCtrl || e.key == ui::key::kRCtrl) bit = ui::mod::kCtrl;
  else if (e.key == ui::key::kLAlt || e.key == ui::key::kRAlt) bit = ui::mod::kAlt;
  if (bit) m = static_cast<ui::Mods>(e.down ? (m | bit) : (m & ~bit));
  return m;
}

}  // namespace pq::views::drag
