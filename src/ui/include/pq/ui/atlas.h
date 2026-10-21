#pragma once

// The atlas texture (TextureId kAtlasTexture): a white block for untextured geometry, a radial glow sprite,
// and (T0.8) glyph bitmaps packed into the rest of the page. Glyphs share the page with the white texel so
// text and shapes stay in one draw command (no texture switch between a widget's fill and its label), the
// same trade ImGui makes with its font atlas.
//
// The page size is fixed, so the uv of the built-in sprites is a constant (baseAtlasUv()) and DrawLists built
// before any glyph exists stay valid. CPU side, the page is an RGBA8 Image whose texels are white with the
// coverage in alpha; the Renderer re-uploads the rows that changed (see Renderer::upload).
//
// UI thread only.

#include <cstdint>
#include <vector>

#include "pq/ui/draw_list.h"
#include "pq/ui/image.h"

namespace pq::ui {

inline constexpr uint32_t kAtlasWidth = 1024;
inline constexpr uint32_t kAtlasHeight = 1024;
inline constexpr uint32_t kAtlasBlobSize = 64;  // blob sprite occupies x ∈ [64, 128), y ∈ [0, 64)
inline constexpr uint32_t kAtlasBaseWidth = 128;  // the built-ins occupy x ∈ [0, 128), y ∈ [0, 64)

// uv of the built-in sprites for the layout produced by buildBaseAtlas().
AtlasUv baseAtlasUv() noexcept;

// RGBA8 pixels of the base atlas page. Texels default to white at alpha 0. White block: texels [0,4)×[0,4).
// Blob: rgb white, alpha falls off as exp(-3r²)·(1-r²) (r = 1 at the sprite edge, zero beyond), so it fades
// smoothly to exactly 0 at its rim.
Image buildBaseAtlas();

// The atlas page plus a shelf packer for the free area and per-row change tracking.
class Atlas {
public:
  Atlas();

  const Image& image() const noexcept { return image_; }

  // Reserves a w×h texel rectangle (a 1-texel transparent gutter is kept around it, so linear filtering at
  // the rect's edge never reads a neighbour). Returns false when the page is full.
  bool allocate(uint32_t w, uint32_t h, uint32_t& x, uint32_t& y);
  // Writable texel (RGBA8) for filling an allocated rect; call markDirty for the rows written.
  uint8_t* texel(uint32_t x, uint32_t y) noexcept { return image_.pixel(x, y); }
  void markDirty(uint32_t y0, uint32_t y1) noexcept;

  // Incremented by every markDirty. A consumer remembers the version it last uploaded.
  uint64_t version() const noexcept { return version_; }
  struct RowRange {
    uint32_t y0 = 0, y1 = 0;  // [y0, y1); empty when y0 == y1
  };
  // Union of the rows changed after `since` (a version previously returned by version()).
  RowRange dirtyRowsSince(uint64_t since) const noexcept;

  uint32_t shelfCount() const noexcept { return static_cast<uint32_t>(shelves_.size()); }
  uint32_t usedHeight() const noexcept { return nextShelfY_; }

private:
  struct Shelf {
    uint32_t y, height, x;  // x: next free column
  };
  struct Change {
    uint64_t version;
    uint32_t y0, y1;
  };
  Image image_;
  std::vector<Shelf> shelves_;
  uint32_t nextShelfY_ = 0;
  uint64_t version_ = 0;
  // Recent changes; older ones are folded into `folded_` (a conservative union) to bound the list.
  std::vector<Change> changes_;
  Change folded_{0, 0, 0};
};

}  // namespace pq::ui
