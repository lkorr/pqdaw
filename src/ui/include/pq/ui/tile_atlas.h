#pragma once

// Cached raster tiles (T3.8-fix1): 8-bit coverage images that a widget rasterises once on the CPU (off its paint
// path) and then draws as one textured quad per frame, instead of re-emitting the geometry they show. The
// arrangement's clip note previews are the first user: at far zoom a clip's 50 notes are one quad, not 50 rects.
//
// Pages are R8_UNORM images of kTilePageSize² texels, sampled like the glyph atlas through the (1, 1, 1, R)
// swizzle, so a tile drawn with vertex colour c gives c with alpha × coverage: one mask serves every colour (a
// muted clip's grey, a selected clip) and colour changes never re-rasterise. The renderer owns one TileAtlas
// (Renderer::tiles()) and reserves the texture ids tilePageTexture(0 … kMaxTilePages − 1) for its pages; it creates
// a page's GPU image when the page first opens and, in every upload(), stages the tiles written since its last
// upload (clearUploads()), then calls beginFrame().
//
// Memory is bounded: at most maxPages() pages (default kMaxTilePages = 4, i.e. 4 × 2048² R8 = 16 MiB of GPU memory),
// and no CPU copy of the pages — a tile's texels are queued once and dropped once the renderer has staged them (with
// a renderer: what one frame wrote; without one, the queue still never holds more than the live tiles, ≤ the pages'
// size, because evicting a page drops its queued tiles). Eviction is per page, least recently used by frame stamp:
// allocate() fills the
// newest page, then any page with room, then opens a page, then clears the page whose last use (touch, allocate) is
// oldest — never one used in the current frame. A cleared page's generation goes up, so every Tile handed out on it
// before reads as !valid() and its owner rebuilds it (texels of a cleared page are garbage until rewritten; every
// tile writes its whole allocation including a transparent one-texel border, so sampling a valid tile never reads
// anything else, also under linear filtering of a stretched draw).
//
// UI thread only. Without a renderer (CPU-only tests and benchmarks) the atlas works the same; call beginFrame()
// (and clearUploads(), standing in for the upload) once per frame where the renderer would. TestDriver::step, which
// paints frames without rendering them, calls beginFrame() only, so its tiles still reach the GPU at render().

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "pq/ui/draw_list.h"

namespace pq::ui {

inline constexpr uint32_t kTilePageSize = 2048;

class TileAtlas {
public:
  explicit TileAtlas(uint32_t maxPages = kMaxTilePages, uint32_t pageSize = kTilePageSize);

  // A tile's content rect [x, x + w) × [y, y + h) on `page` (texels); a one-texel transparent border surrounds it.
  struct Tile {
    uint32_t page = UINT32_MAX;
    uint32_t x = 0, y = 0, w = 0, h = 0;
    uint32_t generation = 0;
  };
  // Largest content size a tile may have.
  uint32_t maxTileSize() const noexcept { return pageSize_ - 2; }
  // Reserves a w×h tile (+ border). Returns false when it is larger than maxTileSize() or every page is full and was
  // used in the current frame. The tile's page is touched.
  bool allocate(uint32_t w, uint32_t h, Tile& out);
  // Whether `t` still holds what was written to it (its page was not cleared since).
  bool valid(const Tile& t) const noexcept {
    return t.page < pageCount_ && pages_[t.page].generation == t.generation;
  }
  // The (w + 2) × (h + 2) texels of t's allocation including the border, zeroed, rows rowPitch(t) bytes apart; the
  // content starts at (1, 1). They are queued for the next upload: write them before calling write() again (the
  // queue may move) and before the frame ends. Returns nullptr for an invalid tile.
  uint8_t* write(const Tile& t);
  static uint32_t rowPitch(const Tile& t) noexcept { return t.w + 2; }
  // Marks `page` as used by the current frame (not evicted before the next beginFrame).
  void touch(uint32_t page) noexcept {
    if (page < pageCount_) pages_[page].lastUsed = frame_;
  }
  static constexpr TextureId texture(uint32_t page) noexcept { return tilePageTexture(page); }
  // uv of a texel position on a page.
  float uvScale() const noexcept { return 1.0f / static_cast<float>(pageSize_); }

  // --- renderer side ---
  struct Upload {
    uint32_t page, x, y, w, h;  // texels written (allocation incl. border)
    size_t offset;              // into uploadBytes(), rows of w bytes
  };
  std::span<const Upload> uploads() const noexcept { return uploads_; }
  std::span<const uint8_t> uploadBytes() const noexcept { return bytes_; }
  // The renderer has staged the queued uploads: drop them (keeps the capacity).
  void clearUploads() noexcept;
  // Frame boundary: pages touched before it become evictable.
  void beginFrame() noexcept { ++frame_; }
  uint64_t frame() const noexcept { return frame_; }

  uint32_t pageSize() const noexcept { return pageSize_; }
  uint32_t pageCount() const noexcept { return pageCount_; }
  uint32_t maxPages() const noexcept { return maxPages_; }
  uint32_t generation(uint32_t page) const noexcept { return pages_[page].generation; }
  // GPU memory the opened pages take (R8: one byte per texel), and the bound: maxPages() pages.
  size_t gpuBytes() const noexcept { return size_t(pageCount_) * pageSize_ * pageSize_; }
  size_t maxGpuBytes() const noexcept { return size_t(maxPages_) * pageSize_ * pageSize_; }
  struct Stats {
    uint64_t allocations = 0;
    uint64_t evictions = 0;      // pages cleared
    uint64_t failures = 0;       // allocate() returned false
    uint64_t texelsWritten = 0;  // through write()
  };
  const Stats& stats() const noexcept { return stats_; }

private:
  struct Shelf {
    uint32_t y, height, x;  // x: next free column
  };
  struct Page {
    std::vector<Shelf> shelves;
    std::unordered_map<uint32_t, uint32_t> openShelf;  // shelf height → index of the newest shelf of that height
    uint32_t nextShelfY = 0;
    uint64_t lastUsed = 0;
    uint32_t generation = 0;
  };
  bool allocateIn(uint32_t page, uint32_t needW, uint32_t needH, uint32_t& x, uint32_t& y);
  void clearPage(uint32_t page);

  std::vector<Page> pages_;
  uint32_t pageCount_ = 0;
  uint32_t fillPage_ = 0;
  uint32_t maxPages_;
  uint32_t pageSize_;
  uint64_t frame_ = 1;
  std::vector<Upload> uploads_;
  std::vector<uint8_t> bytes_;
  Stats stats_;
};

}  // namespace pq::ui
