#pragma once

// Live's colour chooser (private to pq_views): the theme's clip palette (14 × 5), the current colour outlined; a click
// picks. Shared by the track headers (T3.10) and the Tunings panel (T3.19). Open it in a Popover sized with
// colourGridSize().

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

#include "pq/ui/controls.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"

namespace pq::views::paint {

inline constexpr float kColourSwatch = 16.0f;  // logical px
inline constexpr float kColourSwatchGap = 2.0f;

// Logical size of the grid (without the popover's padding).
inline float colourGridWidth() noexcept { return ui::kClipPaletteColumns * (kColourSwatch + kColourSwatchGap) - kColourSwatchGap; }
inline float colourGridHeight() noexcept { return ui::kClipPaletteRows * (kColourSwatch + kColourSwatchGap) - kColourSwatchGap; }

// 0xRRGGBB of a palette colour (alpha dropped).
inline uint32_t rgbOf(ui::Rgba c) noexcept {
  return (static_cast<uint32_t>(ui::redOf(c)) << 16) | (static_cast<uint32_t>(ui::greenOf(c)) << 8) | ui::blueOf(c);
}

class ColourGrid : public ui::Control {
public:
  ColourGrid(uint32_t current, std::function<void(uint32_t)> pick, std::string infoTitle = "Track Colour",
             std::string infoBody = "Click a colour for the selected tracks.")
      : Control("colours"), current_(current), pick_(std::move(pick)) {
    setSize(ui::Length::fixed(colourGridWidth()), ui::Length::fixed(colourGridHeight()));
    setInfo(std::move(infoTitle), std::move(infoBody));
  }
  ui::IRect cell(uint32_t i) const noexcept {
    const ui::UiScale s = tree() ? tree()->uiScale() : ui::UiScale{};
    const ui::IRect r = rect();
    const float col = static_cast<float>(i % ui::kClipPaletteColumns), row = static_cast<float>(i / ui::kClipPaletteColumns);
    const float step = kColourSwatch + kColourSwatchGap;
    const int32_t x0 = r.x + s.px(col * step), y0 = r.y + s.px(row * step);
    return {x0, y0, r.x + s.px(col * step + kColourSwatch) - x0, r.y + s.px(row * step + kColourSwatch) - y0};
  }
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    for (uint32_t i = 0; i < th->clipPalette.size() && i < ui::kClipPaletteSize; ++i) {
      const ui::IRect c = cell(i);
      const ui::Rgba col = th->clipPalette[i];
      ctx.dl.addRectFilled(c.toRect(), col);
      const bool cur = (col & 0x00FFFFFFu) == (ui::hex(current_) & 0x00FFFFFFu);
      if (cur || i == hovered_) ctx.dl.addRectStroke(c.toRect(), cur ? th->clip.selectedBorder : th->mixer.boxBorder);
    }
  }
  // T4.16a: the pointer hand over a colour (the hovered cell, kept by onPointer).
  ui::Cursor cursorAt(ui::Vec2 pos) const override { return hovered_ != UINT32_MAX ? ui::Cursor::Pointer : Control::cursorAt(pos); }
  bool onPointer(const ui::PointerEvent& e) override {
    const ui::PaintResources* res = tree() ? tree()->resources() : nullptr;
    const uint32_t n = res && res->theme ? static_cast<uint32_t>(std::min<size_t>(res->theme->clipPalette.size(), ui::kClipPaletteSize)) : 0;
    uint32_t at = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i)
      if (cell(i).contains(e.pos)) at = i;
    if (e.type == ui::PointerType::Move && at != hovered_) {
      hovered_ = at;
      markDirty();
    }
    if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left && at != UINT32_MAX) {
      if (pick_) pick_(rgbOf(res->theme->clipPalette[at]));
      return true;
    }
    return e.type == ui::PointerType::Up || e.type == ui::PointerType::Down;
  }

private:
  uint32_t current_;
  std::function<void(uint32_t)> pick_;
  uint32_t hovered_ = UINT32_MAX;
};

}  // namespace pq::views::paint
