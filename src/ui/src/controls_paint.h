#pragma once

// Drawing helpers shared by the controls and overlays (private to pq_ui).

#include <algorithm>
#include <cmath>
#include <string_view>

#include "pq/ui/controls.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"

namespace pq::ui::detail {

inline Rect toRect(const IRect& r) noexcept { return r.toRect(); }

inline IRect inset(const IRect& r, int32_t d) noexcept {
  return {r.x + d, r.y + d, std::max(0, r.w - 2 * d), std::max(0, r.h - 2 * d)};
}

// Rounded (or square, below half a pixel) fill and 1-device-px border inside `r`.
inline void box(DrawList& dl, const IRect& r, float radius, Rgba fill, Rgba border) {
  const Rect f = r.toRect();
  if (alphaOf(fill)) {
    if (radius < 0.5f) dl.addRectFilled(f, fill);
    else dl.addRoundedRectFilled(f, radius, fill);
  }
  if (alphaOf(border)) {
    if (radius < 0.5f) dl.addRectStroke(f, border);
    else dl.addRoundedRectStroke(f, radius, border);
  }
}

// Baseline that centres capital letters vertically in a box of height h (Inter's cap height is 0.727 em).
inline int32_t capCentredBaseline(int32_t y, int32_t h, uint32_t pixelSize) noexcept {
  const int32_t cap = static_cast<int32_t>(std::lround(0.727 * static_cast<double>(pixelSize)));
  return y + (h + cap + 1) / 2;
}

// One line of text inside `box`, truncated with "…" to the space between the paddings, aligned, with capitals
// centred vertically. Returns the drawn advance.
inline int32_t drawText(const PaintContext& ctx, FontId font, const IRect& box, std::string_view s, Rgba col, TextAlign a,
                        int32_t padL, int32_t padR) {
  TextSystem* ts = ctx.res.text;
  if (!ts || font == kNoFont || s.empty()) return 0;
  const int32_t avail = box.w - padL - padR;
  if (avail <= 0) return 0;
  const TextRun run = ts->shapeTruncated(font, s, avail);
  int32_t x = box.x + padL;
  if (a == TextAlign::Center) x += (avail - run.advance) / 2;
  else if (a == TextAlign::Right) x = box.right() - padR - run.advance;
  const int32_t y = capCentredBaseline(box.y, box.h, ts->metrics(font).pixelSize);
  TextSystem::drawRun(ctx.dl, run, {static_cast<float>(x), static_cast<float>(y)}, col);
  return run.advance;
}

inline const ThemeFonts* fontsOf(const PaintContext& ctx) noexcept { return ctx.res.uiFonts; }

// Hover / press variants of a solid accent colour (toggles that are on).
inline Rgba hoverOf(Rgba c) noexcept { return lerp(c, rgba(255, 255, 255, alphaOf(c)), 0.12f); }
inline Rgba pressOf(Rgba c) noexcept { return lerp(c, rgba(0, 0, 0, alphaOf(c)), 0.14f); }

}  // namespace pq::ui::detail
