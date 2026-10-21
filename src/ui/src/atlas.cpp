#include "pq/ui/atlas.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pq::ui {

namespace {
constexpr uint32_t kBlobX = kAtlasBaseWidth - kAtlasBlobSize;

const CoverageImage& baseImage() {
  static const CoverageImage img = buildBaseAtlas();
  return img;
}
}  // namespace

AtlasUv baseAtlasUv() noexcept {
  constexpr float iw = 1.0f / static_cast<float>(kAtlasWidth);
  constexpr float ih = 1.0f / static_cast<float>(kAtlasHeight);
  AtlasUv uv{};
  // Corner shared by texels (1,1)..(2,2) of the 4×4 white block: bilinear filtering only ever reads white.
  uv.white = {2.0f * iw, 2.0f * ih};
  uv.blob = {static_cast<float>(kBlobX) * iw, 0.0f, static_cast<float>(kBlobX + kAtlasBlobSize) * iw,
             static_cast<float>(kAtlasBlobSize) * ih};
  return uv;
}

CoverageImage buildBaseAtlas() {
  CoverageImage img;
  img.width = kAtlasWidth;
  img.height = kAtlasHeight;
  // Coverage 0 everywhere: sampled as (1, 1, 1, coverage), so a bilinear tap that strays into an empty texel
  // blends toward transparent white rather than toward black.
  img.texels.assign(kAtlasPageBytes, 0);
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) *img.at(x, y) = 255;
  // Glow sprite. r is measured at texel centres, so the outermost ring is already ~0.
  constexpr float n = static_cast<float>(kAtlasBlobSize);
  for (uint32_t y = 0; y < kAtlasBlobSize; ++y)
    for (uint32_t x = 0; x < kAtlasBlobSize; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / n * 2.0f - 1.0f;
      const float v = (static_cast<float>(y) + 0.5f) / n * 2.0f - 1.0f;
      const float r2 = u * u + v * v;
      const float a = r2 < 1.0f ? std::exp(-3.0f * r2) * (1.0f - r2) : 0.0f;
      *img.at(kBlobX + x, y) = static_cast<uint8_t>(std::lround(a * 255.0f));
    }
  return img;
}

Atlas::Atlas(uint32_t maxPages) : maxPages_(std::clamp<uint32_t>(maxPages, 1, kMaxAtlasPages)) {
  // Page 0 always exists: it backs kAtlasTexture (the shared sprites) even before any glyph is drawn.
  openPage(pages_[0]);
  pageCount_ = 1;
}

void Atlas::openPage(Page& p) {
  if (p.image.texels.empty()) {
    p.image = baseImage();
  } else {
    std::memcpy(p.image.texels.data(), baseImage().texels.data(), kAtlasPageBytes);
  }
  // The first shelf runs to the right of the built-ins (column kAtlasBaseWidth is a gutter); row 0 is left
  // empty so no glyph touches the texture edge. Every reservation carries a 1-texel gutter on its right and
  // bottom, so shelf heights include one gutter row.
  p.shelves.clear();
  p.shelves.push_back({1, kAtlasBlobSize, kAtlasBaseWidth + 1});
  p.nextShelfY = 1 + kAtlasBlobSize;
  p.lastUsed = frame_;
  p.dirty = {0, 0, kAtlasWidth, kAtlasHeight};  // the whole page goes up once (new or cleared)
  p.rects.clear();
  p.collapsed = true;
  ++version_;
}

bool Atlas::allocateIn(Page& p, uint32_t needW, uint32_t needH, uint32_t& x, uint32_t& y) {
  // Best fit among shelves tall enough but not wastefully so (a glyph run mixes heights: x-height letters,
  // ascenders, descenders), else a new shelf with a little slack for the next similar glyph.
  Shelf* best = nullptr;
  for (Shelf& s : p.shelves) {
    if (s.height < needH || s.height > needH + needH / 2 + 2 || s.x + needW > kAtlasWidth) continue;
    if (!best || s.height < best->height) best = &s;
  }
  if (!best) {
    const uint32_t height = needH + needH / 4;
    if (p.nextShelfY + height > kAtlasHeight) {
      if (p.nextShelfY + needH > kAtlasHeight) return false;
      p.shelves.push_back({p.nextShelfY, kAtlasHeight - p.nextShelfY, 1});
    } else {
      p.shelves.push_back({p.nextShelfY, height, 1});
    }
    p.nextShelfY += p.shelves.back().height;
    best = &p.shelves.back();
  }
  x = best->x;
  y = best->y;
  best->x += needW;
  return true;
}

bool Atlas::allocate(uint32_t w, uint32_t h, Slot& out) {
  const uint32_t needW = w + 1, needH = h + 1;
  if (needW > kAtlasWidth - 1 || needH > kAtlasHeight - 1) return false;
  // Placement order keeps the LRU meaningful: pages this frame already uses (free gaps cost no protection),
  // then the fill page (the newest opened / evicted). Gaps in other pages are left alone: filling one would
  // mark an idle page as used and could leave nothing evictable later in the same frame.
  for (uint32_t i = 0; i < pageCount_; ++i)
    if (pages_[i].lastUsed == frame_ && allocateIn(pages_[i], needW, needH, out.x, out.y)) {
      out.page = i;
      return true;
    }
  if (pages_[fillPage_].lastUsed != frame_ && allocateIn(pages_[fillPage_], needW, needH, out.x, out.y)) {
    out.page = fillPage_;
    pages_[fillPage_].lastUsed = frame_;
    return true;
  }
  uint32_t target = UINT32_MAX;
  if (pageCount_ < maxPages_) {
    target = pageCount_++;
    openPage(pages_[target]);
  } else {
    // Least recently used page that the current frame has not touched.
    uint64_t oldest = frame_;
    for (uint32_t i = 0; i < pageCount_; ++i)
      if (pages_[i].lastUsed < oldest) {
        oldest = pages_[i].lastUsed;
        target = i;
      }
    if (target == UINT32_MAX) return false;
    Page& p = pages_[target];
    ++p.generation;
    p.evictedAt = ++evictions_;
    openPage(p);
  }
  fillPage_ = target;
  if (!allocateIn(pages_[target], needW, needH, out.x, out.y)) return false;
  out.page = target;
  return true;
}

void Atlas::markDirty(uint32_t page, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) noexcept {
  if (x1 <= x0 || y1 <= y0) return;
  Page& p = pages_[page];
  if (!p.collapsed) {
    // The next glyph on the same shelf (same top, starting at the previous one's gutter) extends that rect.
    DirtyRect* last = p.rects.empty() ? nullptr : &p.rects.back();
    if (last && last->y0 == y0 && x0 <= last->x1 + 1 && x0 >= last->x0) {
      last->x1 = std::max(last->x1, x1);
      last->y1 = std::max(last->y1, y1);
    } else if (p.rects.size() < kMaxDirtyRects) {
      p.rects.push_back({x0, y0, x1, y1});
    } else {
      p.rects.clear();
      p.collapsed = true;
    }
  }
  DirtyRect& d = p.dirty;
  if (d.empty()) {
    d = {x0, y0, x1, y1};
  } else {
    d.x0 = std::min(d.x0, x0);
    d.y0 = std::min(d.y0, y0);
    d.x1 = std::max(d.x1, x1);
    d.y1 = std::max(d.y1, y1);
  }
  ++version_;
}

Atlas::DirtyRect Atlas::takeDirty(uint32_t page) noexcept {
  Page& p = pages_[page];
  const DirtyRect d = p.dirty;
  p.dirty = {};
  p.rects.clear();
  p.collapsed = false;
  return d;
}

}  // namespace pq::ui
