#pragma once

// Vector faces (TMM.4, docs/plans/MM.md "UI"): a face is resolution-independent vector code drawn in its own WORLD
// units (logical px at zoom 1) through a FacePainter, which maps world → device (device = origin + world · scale) over
// the draw list (it has no transform stack) and pq/ui/lines.h. The same calls are made at every zoom: zooming out
// removes nothing — a stroke thinner than a device pixel keeps its coverage by lowering its alpha (the draw list's
// sub-pixel rule), so far away a face reads as a faint, correct miniature, and the number of primitives is the same
// at every scale (primitives(), a test).
//
//   primitives  discs, rings, arcs (butt / round caps), polylines (feathered ribbons), dashed polylines, fills under a
//               curve (a triangle strip to a baseline), convex polygons, triangles, rounded rects, additive glow
//               sprites (BlendMode::Additive: light that adds up where voices overlap)
//   text        hinted bitmap glyphs at round(px · scale) (SPEC §11: no MSDF), sizes quantised so a zoom creates few
//               fonts; below 4 device px a run is drawn as a thin bar of its extent (legibility is gone, the layout is
//               not). Faces are label-free by design; this is for the few that need a glyph.
//   layers      a face's STATIC layers (value-independent, one colour each: frame, tracks) are rasterised into coverage
//               tiles (pq/ui/tile_atlas.h, pq/ui/raster.h) once per zoom by recordLayer() and drawn as one quad
//               (drawLayer(), stretched while a zoom settles); the live layer (values, voice dots) is drawn as geometry
//               every frame. A tile drawn in colour k equals the same geometry drawn directly in k (raster.h).
//
// UI thread only. Painting allocates nothing once the draw list and the dash scratch have grown.

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "pq/ui/color.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/tile_atlas.h"

namespace pq::ui {

class TextSystem;

// world → device: device = origin + world · scale (scale = device px per world unit: zoom × UI scale).
struct FaceTransform {
  Vec2 origin{0.0f, 0.0f};
  float scale = 1.0f;
  Vec2 map(Vec2 w) const noexcept { return {origin.x + w.x * scale, origin.y + w.y * scale}; }
  float len(float w) const noexcept { return w * scale; }
  Vec2 unmap(Vec2 d) const noexcept { return {(d.x - origin.x) / scale, (d.y - origin.y) / scale}; }
};

class FacePainter {
public:
  FacePainter(DrawList& dl, FaceTransform t) noexcept : dl_(dl), t_(t) {}

  const FaceTransform& transform() const noexcept { return t_; }
  DrawList& drawList() noexcept { return dl_; }
  // Calls made through this painter (one per primitive, whatever its tessellation): equal at every scale.
  uint32_t primitives() const noexcept { return count_; }

  // Every colour becomes `tint` with the colour's alpha multiplied in (a static layer painted in white drawn directly
  // in its layer colour: the same pixels as its coverage tile tinted, pq/ui/raster.h).
  void setTint(Rgba tint) noexcept {
    tint_ = tint;
    tinted_ = true;
  }
  // Text needs a text system and a face (face 0: the UI's regular face, as FontRole's default).
  void setText(TextSystem* text, uint32_t face = 0) noexcept {
    text_ = text;
    face_ = face;
  }

  // ---- primitives (positions, radii and thicknesses in world units; angles in radians, screen space: 0 = +x,
  // π/2 = down, increasing clockwise) ----
  void disc(Vec2 c, float r, Rgba col);
  void ring(Vec2 c, float r, float thickness, Rgba col);
  void arc(Vec2 c, float r, float a0, float a1, float thickness, Rgba col, LineCap cap = LineCap::Butt);
  void line(Vec2 a, Vec2 b, float thickness, Rgba col);
  void polyline(std::span<const Vec2> pts, float thickness, Rgba col);
  // Dashes of `on` world units every on + off along the polyline (the pattern continues across corners).
  void dashed(std::span<const Vec2> pts, float thickness, float on, float off, Rgba col);
  // The area between a curve (left to right) and the horizontal line y = baseY, colour `top` at the curve fading to
  // `bottom` at the baseline (one triangle strip).
  void fillUnder(std::span<const Vec2> curve, float baseY, Rgba top, Rgba bottom);
  void convex(std::span<const Vec2> pts, Rgba col);
  void triangle(Vec2 a, Vec2 b, Vec2 c, Rgba col);
  void roundedRect(const Rect& r, float radius, Rgba col);
  // An additive glow sprite of radius r (the atlas blob): call between beginGlow() / endGlow() to batch the blend.
  void glow(Vec2 c, float r, Rgba col);
  void beginGlow() noexcept { dl_.setBlend(BlendMode::Additive); }
  void endGlow() noexcept { dl_.setBlend(BlendMode::Alpha); }
  // A run of text of nominal size `px` (world units) centred on `centre` (its cap-height centre); see the file
  // comment for sizes and the bar below 4 device px. Without a text system: the bar.
  void text(Vec2 centre, std::string_view utf8, float px, Rgba col);
  // The device pixel size text() uses for nominal size `px` at this scale (0: drawn as a bar).
  uint32_t textPixels(float px) const noexcept;

private:
  DrawList& dl_;
  FaceTransform t_;
  TextSystem* text_ = nullptr;
  uint32_t face_ = 0;
  uint32_t count_ = 0;
  Rgba tint_ = 0;
  bool tinted_ = false;
  // A colour that draws nothing (alpha 0 after the tint): the primitive is counted but emits no vertices.
  bool invisible(Rgba col) const noexcept { return alphaOf(c(col)) == 0; }
  Rgba c(Rgba col) const noexcept {
    return tinted_ ? withAlpha(tint_, (alphaOf(tint_) * alphaOf(col) + 127u) / 255u) : col;
  }
};

// ---- static layers in coverage tiles ----------------------------------------------------------------------------
// One static layer rasterised at one scale: the tile covers the face's world rect (0, 0, w, h) plus `margin` device px
// on every side.
struct LayerTile {
  float scale = 0.0f;   // device px per world unit it was rasterised at (0: none)
  float margin = 0.0f;  // device px around the world rect
  TileAtlas::Tile tile{};
  bool failed = false;  // no room in the atlas at this scale: drawn directly
  bool empty = false;   // the layer drew nothing
};
// Rasterises `paint` (drawing in white through the painter it is given) for a face of world size w × h at `scale` into
// a new tile of `atlas`. `scratch` is a reusable draw list. False (out.failed set) when the atlas has no room or the
// tile would be too large.
bool recordLayer(TileAtlas& atlas, DrawList& scratch, float w, float h, float scale,
                 const std::function<void(FacePainter&)>& paint, LayerTile& out);
// Whether `t` holds a valid tile (at any scale).
bool layerReady(const TileAtlas& atlas, const LayerTile& t) noexcept;
// Draws a ready layer tile with the face's world origin at `origin` (device px, whole pixels for tile == direct) at
// `scale` (stretched when it differs from the tile's), in colour `col`.
void drawLayer(DrawList& dl, TileAtlas& atlas, const LayerTile& t, Vec2 origin, float scale, Rgba col);

}  // namespace pq::ui
