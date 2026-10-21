#pragma once

// Glyph atlas (T0.8, T0.8-fix1): up to kMaxAtlasPages pages of 1024² 8-bit coverage. Each page also carries
// the shared sprites (a white block for untextured geometry and a radial glow sprite) at the same texels, so
// geometry drawn with kAtlasTexture is valid on any page and text and shapes stay in one draw command (no
// texture switch between a widget's fill and its label), the same trade ImGui makes with its font atlas.
//
// GPU side a page is an R8_UNORM image viewed with the component swizzle (1, 1, 1, R): the shader samples
// white × coverage exactly as it did with the old RGBA page, at a quarter of the memory and upload bandwidth.
//
// The page size is fixed, so the uv of the built-in sprites is a constant (baseAtlasUv()) and DrawLists built
// before any glyph exists stay valid.
//
// Eviction is per page, least recently used by frame stamp: allocate() tries the pages the current frame
// already uses, then the fill page (the newest), then opens a new page, then clears the page whose last use is
// oldest — but never a page used in the current frame (touch()), so glyphs referenced by runs handed out this
// frame stay put until the frame ends. When every page was used this frame, allocate() fails (the text system
// counts and reports it). Clearing a page bumps its generation; glyph and run caches compare generations /
// eviction epochs to notice.
//
// Change tracking: per page, the rects written since the renderer last took them (glyphs placed side by side on
// a shelf merge into one rect; past kMaxDirtyRects the page falls back to their bounding rect), so a frame
// uploads about the new glyphs' own texels rather than whole rows.
//
// UI thread only.

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

#include "pq/ui/draw_list.h"

namespace pq::ui {

inline constexpr uint32_t kAtlasWidth = 1024;
inline constexpr uint32_t kAtlasHeight = 1024;
inline constexpr uint32_t kAtlasBlobSize = 64;  // blob sprite occupies x ∈ [64, 128), y ∈ [0, 64)
inline constexpr uint32_t kAtlasBaseWidth = 128;  // the built-ins occupy x ∈ [0, 128), y ∈ [0, 64)
inline constexpr size_t kAtlasPageBytes = size_t(kAtlasWidth) * kAtlasHeight;  // R8: one byte per texel

// uv of the built-in sprites for the layout produced by buildBaseAtlas().
AtlasUv baseAtlasUv() noexcept;

// 8-bit coverage image (rows top to bottom).
struct CoverageImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> texels;  // width*height

  uint8_t* at(uint32_t x, uint32_t y) noexcept { return texels.data() + size_t(y) * width + x; }
  const uint8_t* at(uint32_t x, uint32_t y) const noexcept { return texels.data() + size_t(y) * width + x; }
};

// Coverage of an empty page: 0 everywhere except the white block (texels [0,4)×[0,4) = 255) and the blob
// (alpha falls off as exp(-3r²)·(1-r²), r = 1 at the sprite edge, zero beyond, so it fades smoothly to exactly
// 0 at its rim).
CoverageImage buildBaseAtlas();

class Atlas {
public:
  // `maxPages` (1..kMaxAtlasPages) bounds the GPU memory (1 MiB per page); tests use 1 to force eviction.
  explicit Atlas(uint32_t maxPages = kMaxAtlasPages);

  struct Slot {
    uint32_t page = 0, x = 0, y = 0;
  };
  // Reserves a w×h texel rectangle (with a 1-texel transparent gutter right and below, so linear filtering at
  // the rect's edge never reads a neighbour) on any page, opening or evicting a page if needed (see above).
  // The page is touched. Returns false when nothing can be freed this frame or the rect is larger than a page.
  bool allocate(uint32_t w, uint32_t h, Slot& out);

  // Frame boundary (TextSystem::beginFrame): pages touched before it become evictable.
  void beginFrame() noexcept { ++frame_; }
  uint64_t frame() const noexcept { return frame_; }
  // Marks a page as used by the current frame (it is not evicted before the next beginFrame).
  void touch(uint32_t page) noexcept { pages_[page].lastUsed = frame_; }
  void touchMask(uint32_t pageMask) noexcept {
    for (uint32_t m = pageMask; m != 0; m &= m - 1) pages_[static_cast<uint32_t>(std::countr_zero(m))].lastUsed = frame_;
  }

  // Writable texel for filling an allocated rect; call markDirty for the rect written.
  uint8_t* texel(uint32_t page, uint32_t x, uint32_t y) noexcept { return pages_[page].image.at(x, y); }
  const CoverageImage& pageImage(uint32_t page) const noexcept { return pages_[page].image; }
  void markDirty(uint32_t page, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) noexcept;

  struct DirtyRect {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) × [y0, y1); empty when x0 == x1
    bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
  };
  static constexpr uint32_t kMaxDirtyRects = 64;
  // Bounding rect of everything changed on the page since the last takeDirty.
  DirtyRect dirty(uint32_t page) const noexcept { return pages_[page].dirty; }
  // The changed rects (disjoint; just the bounding rect after a page open/evict or more than kMaxDirtyRects).
  std::span<const DirtyRect> dirtyRects(uint32_t page) const noexcept {
    const Page& p = pages_[page];
    if (p.dirty.empty()) return {};
    return p.collapsed ? std::span<const DirtyRect>(&p.dirty, 1) : std::span<const DirtyRect>(p.rects);
  }
  // Returns the bounding rect and clears the page's change tracking (the renderer, once per frame).
  DirtyRect takeDirty(uint32_t page) noexcept;
  // Incremented by every change (markDirty, page open/evict): a cheap "anything to upload?" check.
  uint64_t version() const noexcept { return version_; }

  uint32_t pageCount() const noexcept { return pageCount_; }
  uint32_t maxPages() const noexcept { return maxPages_; }
  // Bumped each time the page is cleared by eviction.
  uint32_t generation(uint32_t page) const noexcept { return pages_[page].generation; }
  // Number of evictions so far (the eviction epoch), and the epoch at which `page` was last evicted (0: never).
  uint64_t evictions() const noexcept { return evictions_; }
  uint64_t evictedAt(uint32_t page) const noexcept { return pages_[page].evictedAt; }

  uint32_t shelfCount(uint32_t page = 0) const noexcept { return static_cast<uint32_t>(pages_[page].shelves.size()); }
  uint32_t usedHeight(uint32_t page = 0) const noexcept { return pages_[page].nextShelfY; }
  // CPU bytes of the page images (each page is also one kAtlasPageBytes R8 image on the GPU).
  size_t memoryBytes() const noexcept { return size_t(pageCount_) * kAtlasPageBytes; }

private:
  struct Shelf {
    uint32_t y, height, x;  // x: next free column
  };
  struct Page {
    CoverageImage image;
    std::vector<Shelf> shelves;
    uint32_t nextShelfY = 0;
    uint64_t lastUsed = 0;
    uint64_t evictedAt = 0;
    uint32_t generation = 0;
    DirtyRect dirty;              // bounding rect
    std::vector<DirtyRect> rects;  // individual rects unless collapsed
    bool collapsed = false;
  };
  void openPage(Page& p);  // resets to the base layout, all dirty
  bool allocateIn(Page& p, uint32_t needW, uint32_t needH, uint32_t& x, uint32_t& y);

  std::array<Page, kMaxAtlasPages> pages_{};
  uint32_t pageCount_ = 0;
  uint32_t fillPage_ = 0;  // newest opened or evicted page: new glyphs go here once the frame's pages are full
  uint32_t maxPages_ = kMaxAtlasPages;
  uint64_t frame_ = 1;
  uint64_t version_ = 0;
  uint64_t evictions_ = 0;
};

}  // namespace pq::ui
