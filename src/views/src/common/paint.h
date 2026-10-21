#pragma once

// Drawing helpers shared by every view (private to pq_views). The same rules as the T3.2 controls: colours from
// the theme, 1-device-px borders, text through the theme's font roles with capitals centred vertically.

#include <algorithm>
#include <cmath>
#include <string_view>

#include "pq/ui/controls.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"

namespace pq::views::paint {

inline ui::IRect inset(const ui::IRect& r, int32_t d) noexcept {
  return {r.x + d, r.y + d, std::max(0, r.w - 2 * d), std::max(0, r.h - 2 * d)};
}

// Fill (rounded when radius ≥ 0.5 px) and a 1-device-px border inside `r`; a zero-alpha colour is skipped.
inline void box(ui::DrawList& dl, const ui::IRect& r, float radius, ui::Rgba fill, ui::Rgba border) {
  const ui::Rect f = r.toRect();
  if (ui::alphaOf(fill)) {
    if (radius < 0.5f) dl.addRectFilled(f, fill);
    else dl.addRoundedRectFilled(f, radius, fill);
  }
  if (ui::alphaOf(border)) {
    if (radius < 0.5f) dl.addRectStroke(f, border);
    else dl.addRoundedRectStroke(f, radius, border);
  }
}

// Baseline that centres capitals vertically in a box of height h (Inter's cap height is 0.727 em).
inline int32_t capCentredBaseline(int32_t y, int32_t h, uint32_t pixelSize) noexcept {
  const int32_t cap = static_cast<int32_t>(std::lround(0.727 * static_cast<double>(pixelSize)));
  return y + (h + cap + 1) / 2;
}

// One line of text in `box` between the paddings, truncated with "…", aligned, capitals centred. Returns the
// advance drawn.
inline int32_t text(const ui::PaintContext& ctx, ui::FontId font, const ui::IRect& box, std::string_view s, ui::Rgba col,
                    ui::TextAlign a, int32_t padL = 0, int32_t padR = 0) {
  ui::TextSystem* ts = ctx.res.text;
  if (!ts || font == ui::kNoFont || s.empty()) return 0;
  const int32_t avail = box.w - padL - padR;
  if (avail <= 0) return 0;
  const ui::TextRun run = ts->shapeTruncated(font, s, avail);
  int32_t x = box.x + padL;
  if (a == ui::TextAlign::Center) x += (avail - run.advance) / 2;
  else if (a == ui::TextAlign::Right) x = box.right() - padR - run.advance;
  const int32_t y = capCentredBaseline(box.y, box.h, ts->metrics(font).pixelSize);
  ui::TextSystem::drawRun(ctx.dl, run, {static_cast<float>(x), static_cast<float>(y)}, col);
  return run.advance;
}

inline ui::Rgba hoverOf(ui::Rgba c) noexcept { return ui::lerp(c, ui::rgba(255, 255, 255, ui::alphaOf(c)), 0.12f); }
inline ui::Rgba pressOf(ui::Rgba c) noexcept { return ui::lerp(c, ui::rgba(0, 0, 0, ui::alphaOf(c)), 0.14f); }

}  // namespace pq::views::paint
