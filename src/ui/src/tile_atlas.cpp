#include "pq/ui/tile_atlas.h"

#include <algorithm>
#include <cstring>

namespace pq::ui {

TileAtlas::TileAtlas(uint32_t maxPages, uint32_t pageSize)
    : maxPages_(std::clamp<uint32_t>(maxPages, 1, kMaxTilePages)), pageSize_(std::max<uint32_t>(pageSize, 16)) {
  pages_.resize(maxPages_);
}

void TileAtlas::clearPage(uint32_t page) {
  Page& p = pages_[page];
  p.shelves.clear();
  p.openShelf.clear();
  p.nextShelfY = 0;
  ++p.generation;
  // Texels queued for the page's old tiles would land under its new ones: drop them.
  std::erase_if(uploads_, [page](const Upload& u) { return u.page == page; });
}

bool TileAtlas::allocateIn(uint32_t page, uint32_t needW, uint32_t needH, uint32_t& x, uint32_t& y) {
  Page& p = pages_[page];
  // Shelves come in heights rounded up to 4 texels (fewer distinct shelves, ≤ 3 rows of waste per tile); a new
  // tile goes on the newest shelf of its height, else on a new shelf.
  const auto it = p.openShelf.find(needH);
  if (it != p.openShelf.end()) {
    Shelf& s = p.shelves[it->second];
    if (s.x + needW <= pageSize_) {
      x = s.x;
      y = s.y;
      s.x += needW;
      return true;
    }
  }
  if (p.nextShelfY + needH > pageSize_) return false;
  p.shelves.push_back({p.nextShelfY, needH, needW});
  p.openShelf[needH] = static_cast<uint32_t>(p.shelves.size() - 1);
  x = 0;
  y = p.nextShelfY;
  p.nextShelfY += needH;
  return true;
}

bool TileAtlas::allocate(uint32_t w, uint32_t h, Tile& out) {
  if (w == 0 || h == 0 || w > maxTileSize() || h > maxTileSize()) {
    ++stats_.failures;
    return false;
  }
  const uint32_t needW = w + 2;
  const uint32_t needH = std::min(pageSize_, (h + 2 + 3) & ~3u);
  uint32_t x = 0, y = 0;
  auto take = [&](uint32_t page) {
    out = {page, x + 1, y + 1, w, h, pages_[page].generation};
    pages_[page].lastUsed = frame_;
    fillPage_ = page;
    ++stats_.allocations;
    return true;
  };
  // The page being filled, then any open page with room, then a new page.
  if (pageCount_ > 0 && allocateIn(fillPage_, needW, needH, x, y)) return take(fillPage_);
  for (uint32_t p = 0; p < pageCount_; ++p)
    if (p != fillPage_ && allocateIn(p, needW, needH, x, y)) return take(p);
  if (pageCount_ < maxPages_) {
    const uint32_t p = pageCount_++;
    if (allocateIn(p, needW, needH, x, y)) return take(p);
  }
  // Evict the least recently used page not used in this frame.
  uint32_t victim = UINT32_MAX;
  for (uint32_t p = 0; p < pageCount_; ++p)
    if (pages_[p].lastUsed < frame_ && (victim == UINT32_MAX || pages_[p].lastUsed < pages_[victim].lastUsed)) victim = p;
  if (victim == UINT32_MAX) {
    ++stats_.failures;
    return false;
  }
  clearPage(victim);
  ++stats_.evictions;
  if (allocateIn(victim, needW, needH, x, y)) return take(victim);
  ++stats_.failures;
  return false;
}

uint8_t* TileAtlas::write(const Tile& t) {
  if (!valid(t)) return nullptr;
  const uint32_t w = t.w + 2, h = t.h + 2;
  const size_t at = (bytes_.size() + 3) & ~size_t{3};  // copy offsets stay 4-byte aligned
  bytes_.resize(at + size_t(w) * h);  // zero-filled: the border and the background are transparent
  uploads_.push_back({t.page, t.x - 1, t.y - 1, w, h, at});
  stats_.texelsWritten += size_t(w) * h;
  touch(t.page);
  return bytes_.data() + at;
}

void TileAtlas::clearUploads() noexcept {
  uploads_.clear();
  bytes_.clear();  // keeps its capacity: one frame's worth of written tiles
}

}  // namespace pq::ui
