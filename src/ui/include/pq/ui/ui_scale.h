#pragma once

// UiScale (T0.7-fix3, SPEC §11): the logical → device pixel factor of the UI, i.e. the window's display
// scale (Windows per-monitor DPI / 96: 1.0 at 100 %, 1.25, 1.5, 2.0 ...). It is the one conversion every
// UI layer uses:
//
//   - Layout (UiTree, widgets, scenes) is specified in logical px. Positions and sizes become device px as
//     round(logical × factor), each edge snapped on its own, so adjacent logical rects stay adjacent and
//     every fill edge lands on a device pixel boundary.
//   - Text: a font role of N logical px renders with the hinted integer-pixel font round(N × factor)
//     (fontPx). Glyph bitmaps are never resampled: a 13 px role is a 20 px font at 150 %, not a 13 px font
//     scaled up (no MSDF, no blur).
//   - Line widths, AA feathers and hairlines stay in device px (the draw list and pq/ui/lines.h take device
//     px and never read the scale): a 1 px grid line is one device pixel at any scale.
//
// Source of truth: the app reads SDL_GetWindowDisplayScale at startup and on
// SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED and pushes the new value to UiTree::setViewport and
// setSceneScale (pq/ui/scenes.h); everything else reads it from there (UiTree::uiScale(), PaintContext::scale,
// SceneTextures::scale). Nothing caches a device-px value derived from an old scale across a change.

#include <cmath>
#include <cstdint>

#include "pq/ui/draw_list.h"

namespace pq::ui {

struct UiScale {
  static constexpr float kMin = 0.5f;
  static constexpr float kMax = 4.0f;

  float factor = 1.0f;

  constexpr UiScale() noexcept = default;
  // Non-finite or non-positive values mean "unknown" (1.0); the rest is clamped to [kMin, kMax].
  // Implicit, so call sites that pass a plain display-scale float keep working.
  UiScale(float f) noexcept : factor(sanitize(f)) {}  // NOLINT(google-explicit-constructor)

  static float sanitize(float f) noexcept {
    if (!(f > 0.0f) || !std::isfinite(f)) return 1.0f;
    return f < kMin ? kMin : (f > kMax ? kMax : f);
  }

  // Logical px → snapped device px.
  int32_t px(float logical) const noexcept { return static_cast<int32_t>(std::lround(logical * factor)); }
  float pxf(float logical) const noexcept { return static_cast<float>(px(logical)); }
  // Logical px → unsnapped device px (antialiased geometry that may sit between pixels: radii, blob centres).
  float exact(float logical) const noexcept { return logical * factor; }
  // A logical rect with each edge snapped to device px.
  Rect rect(float x0, float y0, float x1, float y1) const noexcept { return {pxf(x0), pxf(y0), pxf(x1), pxf(y1)}; }
  Vec2 point(float x, float y) const noexcept { return {pxf(x), pxf(y)}; }
  // Integer pixel size of a font role of `logicalPx` (at least 1).
  uint32_t fontPx(uint32_t logicalPx) const noexcept {
    const long v = std::lround(static_cast<float>(logicalPx) * factor);
    return v < 1 ? 1u : static_cast<uint32_t>(v);
  }
  // Device px (e.g. a window's pixel size) → logical px, unsnapped.
  float toLogical(float devicePx) const noexcept { return devicePx / factor; }

  bool operator==(const UiScale&) const = default;
};

}  // namespace pq::ui
