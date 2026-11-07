// The boundaries of a chain circuit painted on a canvas (T5.x-circuit-chain; since T5.x-one-circuit the project patch
// view draws them inside opened racks): pq/views/circuit/chain_paint.h.

#include "pq/views/circuit/chain_paint.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../common/paint.h"
#include "pq/ui/theme.h"

namespace pq::views::circuit {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

bool overlaps(const Rect& r, const ui::IRect& clip) noexcept {
  return r.x1 >= static_cast<float>(clip.x) && r.x0 <= static_cast<float>(clip.right()) && r.y1 >= static_cast<float>(clip.y) &&
         r.y0 <= static_cast<float>(clip.bottom());
}

ui::IRect toIRect(const Rect& r) noexcept {
  const auto x0 = static_cast<int32_t>(std::floor(r.x0)), y0 = static_cast<int32_t>(std::floor(r.y0));
  return {x0, y0, static_cast<int32_t>(std::ceil(r.x1)) - x0, static_cast<int32_t>(std::ceil(r.y1)) - y0};
}

Rect worldRect(const CircuitCanvas& c, float x0, float y0, float x1, float y1) noexcept {
  const Vec2 a = c.toDevice({x0, y0}), b = c.toDevice({x1, y1});
  return {std::round(a.x), std::round(a.y), std::round(b.x), std::round(b.y)};
}

// The kind's accent: instruments the value colour, audio effects the signal colour, racks the solo blue, chains the
// symbol outline.
Rgba accentOf(const ui::Theme& th, const ChainBox& b) noexcept {
  switch (b.kind) {
    case ChainBoxKind::Device: return b.instrument ? th.accent.primary : th.circuit.wireAudio;
    case ChainBoxKind::Rack: return th.accent.solo;
    case ChainBoxKind::Chain: return th.circuit.symbolStroke;
  }
  return th.circuit.symbolStroke;
}

}  // namespace

void paintChainBoxes(ui::PaintContext& ctx, const CircuitCanvas& canvas, const ChainCircuit& c, float dx, float dy) {
  if (ctx.res.theme == nullptr) return;
  const ui::Theme& th = *ctx.res.theme;
  ui::DrawList& dl = ctx.dl;
  const ui::IRect clip = ui::intersect(ctx.clip, canvas.rect());
  const float s = canvas.deviceScale(), us = ctx.scale.factor;
  const float line = std::max(1.0f, std::round(us));
  const float bar = std::max(2.0f, std::round(3.0f * us));
  const bool names = ctx.res.uiFonts != nullptr && ctx.res.text != nullptr;
  const ui::FontId font = names ? ctx.res.uiFonts->small : ui::kNoFont;
  const float px = names ? static_cast<float>(ctx.res.text->metrics(font).pixelSize) : 12.0f;
  const float strip = chain_metrics::kTitle * s;
  // Titles that do not fit their strip (zoomed out) go in pills at the boxes' top-left corners, outer boxes first;
  // a pill that would overlap one already placed is left out.
  std::vector<Rect> pills;
  auto pill = [&](const Rect& r, const std::string& text, Rgba col) {
    if (!names || r.x1 - r.x0 < 28.0f * us || r.y1 - r.y0 < px + 4.0f) return;
    const float pad = std::round(3.0f * us), h = std::round(px + 4.0f * us);
    const int32_t avail = static_cast<int32_t>(r.x1 - r.x0 - 2.0f * pad - 2.0f);
    const ui::TextRun run = ctx.res.text->shapeTruncated(font, text, avail);
    if (run.advance <= 0) return;
    const Rect box{r.x0 + 1.0f, r.y0 + 1.0f, r.x0 + 1.0f + static_cast<float>(run.advance) + 2.0f * pad, r.y0 + 1.0f + h};
    for (const Rect& q : pills)
      if (!(box.x1 <= q.x0 || q.x1 <= box.x0 || box.y1 <= q.y0 || q.y1 <= box.y0)) return;
    pills.push_back(box);
    dl.addRoundedRectFilled(box, 2.0f * us, ui::withAlpha(th.surface.sunken, 230));
    ui::TextSystem::drawRun(dl, run, {box.x0 + pad, static_cast<float>(paint::capCentredBaseline(static_cast<int32_t>(box.y0), static_cast<int32_t>(h), static_cast<uint32_t>(px)))}, col);
  };

  // The track's band (track mode): its colour, its name.
  if (!c.rack.valid() && c.track.valid()) {
    const Rect r = worldRect(canvas, c.x0 + dx, c.y0 + dy, c.x1 + dx, c.y1 + dy);
    if (overlaps(r, clip)) {
      const Rgba col = ui::hex(c.trackColour);
      dl.addRectFilled(r, ui::lerp(th.circuit.background, col, 0.06f));
      dl.addRectFilled({r.x0, r.y0, r.x0 + bar, r.y1}, col);
      const float h = std::max(std::min(24.0f * s, r.y1 - r.y0), 0.0f);
      if (names && h >= px + 2.0f) {
        paint::text(ctx, font, toIRect({r.x0 + bar + 6.0f, r.y0, std::min(r.x1, r.x0 + 600.0f * us), r.y0 + h}), c.trackName,
                    th.circuit.symbolText, ui::TextAlign::Left);
      } else if (names) {  // zoomed out: the name just above the band (its strip is too thin to hold it)
        const float lh = std::round(px + 4.0f * us);
        paint::text(ctx, font, toIRect({r.x0, r.y0 - lh - 2.0f * us, std::min(r.x1, r.x0 + 600.0f * us), r.y0 - 2.0f * us}), c.trackName,
                    th.circuit.symbolText, ui::TextAlign::Left);
      }
    }
  }
  for (const ChainBox& b : c.boxes) {
    const Rect r = worldRect(canvas, b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy);
    if (!overlaps(r, clip) || r.x1 - r.x0 < 2.0f || r.y1 - r.y0 < 2.0f) continue;
    const Rgba accent = accentOf(th, b);
    const float hs = std::min(strip, r.y1 - r.y0);
    switch (b.kind) {
      case ChainBoxKind::Device:
        dl.addRectFilled(r, ui::lerp(th.circuit.background, th.circuit.subcircuitFill, 0.45f));
        dl.addRectFilled({r.x0, r.y0, r.x1, r.y0 + hs}, ui::lerp(th.circuit.background, accent, 0.22f));
        dl.addRectStroke(r, ui::mulAlpha(accent, 0.9f), line);
        dl.addRectFilled({r.x0, r.y0, r.x0 + bar, r.y1}, accent);
        break;
      case ChainBoxKind::Rack:
        dl.addRectFilled(r, ui::lerp(th.circuit.background, accent, 0.05f));
        dl.addRectFilled({r.x0, r.y0, r.x1, r.y0 + hs}, ui::lerp(th.circuit.background, accent, 0.18f));
        dl.addRectStroke(r, ui::mulAlpha(accent, 0.9f), line);
        dl.addRectFilled({r.x0, r.y0, r.x0 + bar, r.y1}, accent);
        break;
      case ChainBoxKind::Chain:
        dl.addRectFilled(r, ui::mulAlpha(th.circuit.background, 0.6f));
        dl.addRectStroke(r, ui::mulAlpha(accent, 0.45f), line);
        break;
    }
    if (!names) continue;
    const Rgba textCol = b.kind == ChainBoxKind::Chain ? th.circuit.pinText : th.circuit.symbolText;
    if (hs >= px + 2.0f) {  // the title in its strip, the kind after it
      const float x0 = r.x0 + (b.kind == ChainBoxKind::Chain ? 6.0f * us : bar + 6.0f * us);
      const ui::IRect box = toIRect({x0, r.y0, r.x1 - 4.0f * us, r.y0 + hs});
      const int32_t adv = paint::text(ctx, font, box, b.title, textCol, ui::TextAlign::Left);
      if (b.kind != ChainBoxKind::Chain && adv > 0 && adv + 12 < box.w) {
        ui::IRect rest = box;
        rest.x += adv + static_cast<int32_t>(std::round(10.0f * us));
        rest.w -= adv + static_cast<int32_t>(std::round(10.0f * us));
        paint::text(ctx, font, rest, b.kindLabel, th.circuit.pinText, ui::TextAlign::Left);
      }
    } else if (b.kind != ChainBoxKind::Chain) {
      pill(r, b.title, textCol);
    }
  }
}

void paintChainVeils(ui::PaintContext& ctx, const CircuitCanvas& canvas, const ChainCircuit& c, float dx, float dy) {
  if (ctx.res.theme == nullptr) return;
  const ui::Theme& th = *ctx.res.theme;
  const ui::IRect clip = ui::intersect(ctx.clip, canvas.rect());
  const float s = canvas.deviceScale();
  const bool names = ctx.res.uiFonts != nullptr && ctx.res.text != nullptr;
  for (const ChainBox& b : c.boxes) {
    if (b.active && !b.missing) continue;
    const Rect r = worldRect(canvas, b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy);
    if (!overlaps(r, clip)) continue;
    const float hs = std::min(chain_metrics::kTitle * s, r.y1 - r.y0);
    ctx.dl.addRectFilled({r.x0, r.y0 + hs, r.x1, r.y1}, ui::mulAlpha(th.circuit.background, 0.6f));
    if (names && hs >= static_cast<float>(ctx.res.text->metrics(ctx.res.uiFonts->small).pixelSize) + 2.0f)
      paint::text(ctx, ctx.res.uiFonts->small, toIRect({r.x0, r.y0, r.x1 - 6.0f, r.y0 + hs}), b.missing ? "missing" : "off",
                  th.circuit.pinText, ui::TextAlign::Right);
  }
}

}  // namespace pq::views::circuit
