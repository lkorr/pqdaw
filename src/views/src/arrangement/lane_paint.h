#pragma once

// The lanes' paint pass, shared by lanes.cpp (grid, note clips, group summaries) and audio_paint.cpp (T5.6: audio
// clips with their waveforms, fades, crossfades and handles).

#include <cstdint>
#include <string_view>

#include "internal.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/tile_atlas.h"

namespace pq::views::arrangement {

// Everything one lanes paint needs, computed once.
struct LanePaint {
  ui::PaintContext& ctx;
  const ui::Theme& th;
  Model& m;
  ArrangementView& view;
  const doc::Project& p;
  ui::IRect r;      // canvas rect
  ui::IRect c;      // visible part being painted
  double k;         // device px per tick
  double origin;
  Ticks from, to;   // ticks meeting c (a pixel of slack each side)
  int32_t titleH;
  ui::FontId font = ui::kNoFont;
  int32_t baselineOff = 0;  // title strip top → name baseline

  float X(double tick) const noexcept { return static_cast<float>(static_cast<double>(r.x) + (tick - origin) * k); }
};

// Solid rects in one reservation.
void emitRects(ui::DrawList& dl, std::span<const Model::RectOut> rs);
// Emits the queued tile quads (one draw command per page), then the deferred rects and overlay geometry of their clips.
void flushTiles(LanePaint& lp);

// A clip's columns: its edges (cx0, cx1) and the part this paint draws (vx0, vx1); false when nothing is visible.
struct ClipCols {
  float cx0 = 0, cx1 = 0, vx0 = 0, vx1 = 0;
};
bool clipColumns(const LanePaint& lp, Ticks start, Ticks length, ClipCols& out) noexcept;
// Title strip in the clip colour with the name (sticky at the lanes' left edge), body fill; returns the title's bottom.
float paintClipChrome(LanePaint& lp, const ClipCols& cc, float top, float bottom, ui::Rgba colour, std::string_view name);
// Border (dark edges, or the selection outline) and the hover highlight (0 title, ±1 edge; kNoHover none). Deferred
// (drawn after the tile quads) when the clip's content is a queued tile.
inline constexpr int kNoHover = -2;
void paintClipBorder(LanePaint& lp, const ClipCols& cc, float top, float bottom, float titleB, bool selected, int hover,
                     bool deferred);

// T5.6 (audio_paint.cpp): an audio clip at geometry `g` (its own bounds, or a drag preview's). Returns true when its
// waveform was queued as a tile (its overlays then wait for flushTiles).
void paintAudioClip(LanePaint& lp, const ArrangementRow& row, const doc::AudioClipPtr& clip, const ClipGeom& g, float top,
                    float bottom, bool trackActive, bool selected, int hover);
// The crossfade X of an audio clip whose own columns are outside the painted part (the X reaches half its length into
// the previous clip: a partial repaint of that clip must draw it again).
void paintAudioCrossfade(LanePaint& lp, const doc::AudioClip& clip, float top, float bottom);
// Builds the missing waveform tiles of the visible audio clips within `texelBudget` (onFrame); false: more to build.
bool buildAudioTiles(LaneCanvas& canvas, ArrangementView& view, ui::TileAtlas& tiles, const ui::IRect& visible, size_t& texels,
                     bool& builtOne);

}  // namespace pq::views::arrangement
