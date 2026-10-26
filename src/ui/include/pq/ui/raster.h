#pragma once

// CPU coverage rasteriser for draw-list geometry (T4.2): renders the triangles of a DrawList into an 8-bit coverage
// image the way the GPU draws them — every pixel sampled at its centre, the vertex alpha (alpha of Vertex::col)
// interpolated barycentrically, the top-left fill rule on shared edges, and coverage accumulated like alpha blending of
// one colour (c ← a + c·(1 − a)). A coverage tile drawn with colour k (pq/ui/tile_atlas.h) then equals the same
// geometry drawn directly in colour k: 1 − Π(1 − aᵢ) either way. Only untextured geometry (fills, strokes, feathered
// ribbons from pq/ui/lines.h); glyph quads are not supported (texture lookups are ignored).
//
// The circuit canvas rasterises each schematic symbol once per zoom level into tiles this way (one quad per symbol per
// frame afterwards). UI thread; allocation-free.

#include <cstdint>

#include "pq/ui/draw_list.h"

namespace pq::ui {

// Adds the triangles of `dl` into `dst` (width × height bytes, rows rowPitch apart), pixel (x, y) sampling the draw
// list's point (origin.x + x + 0.5, origin.y + y + 0.5). Triangles outside the image are skipped; dst is not cleared.
void rasterizeCoverage(const DrawList& dl, uint8_t* dst, uint32_t width, uint32_t height, uint32_t rowPitch,
                       Vec2 origin) noexcept;

}  // namespace pq::ui
