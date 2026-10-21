#include "pq/ui/atlas.h"

#include <algorithm>
#include <cmath>

namespace pq::ui {

namespace {
constexpr uint32_t kBlobX = kAtlasBaseWidth - kAtlasBlobSize;
constexpr size_t kMaxChanges = 64;
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

Image buildBaseAtlas() {
  Image img;
  img.width = kAtlasWidth;
  img.height = kAtlasHeight;
  // White at alpha 0 everywhere: glyph texels are white with coverage in alpha, and a bilinear tap that
  // strays into an empty texel then blends toward transparent white rather than toward black.
  img.rgba.resize(size_t(kAtlasWidth) * kAtlasHeight * 4);
  for (size_t i = 0; i < img.rgba.size(); i += 4) {
    img.rgba[i] = img.rgba[i + 1] = img.rgba[i + 2] = 255;
    img.rgba[i + 3] = 0;
  }
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) img.pixel(x, y)[3] = 255;
  // Glow sprite. r is measured at texel centres, so the outermost ring is already ~0.
  constexpr float n = static_cast<float>(kAtlasBlobSize);
  for (uint32_t y = 0; y < kAtlasBlobSize; ++y)
    for (uint32_t x = 0; x < kAtlasBlobSize; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / n * 2.0f - 1.0f;
      const float v = (static_cast<float>(y) + 0.5f) / n * 2.0f - 1.0f;
      const float r2 = u * u + v * v;
      const float a = r2 < 1.0f ? std::exp(-3.0f * r2) * (1.0f - r2) : 0.0f;
      img.pixel(kBlobX + x, y)[3] = static_cast<uint8_t>(std::lround(a * 255.0f));
    }
  return img;
}

Atlas::Atlas() : image_(buildBaseAtlas()) {
  // The first shelf runs to the right of the built-ins (column kAtlasBaseWidth is a gutter); row 0 is left
  // empty so no glyph touches the texture edge. Every reservation carries a 1-texel gutter on its right and
  // bottom, so shelf heights include one gutter row.
  shelves_.push_back({1, kAtlasBlobSize, kAtlasBaseWidth + 1});
  nextShelfY_ = 1 + kAtlasBlobSize;
}

bool Atlas::allocate(uint32_t w, uint32_t h, uint32_t& x, uint32_t& y) {
  const uint32_t needW = w + 1, needH = h + 1;
  if (needW > kAtlasWidth - 1 || needH > kAtlasHeight) return false;
  // Best fit among shelves tall enough but not wastefully so (a glyph run mixes heights: x-height letters,
  // ascenders, descenders), else a new shelf with a little slack for the next similar glyph.
  Shelf* best = nullptr;
  for (Shelf& s : shelves_) {
    if (s.height < needH || s.height > needH + needH / 2 + 2 || s.x + needW > kAtlasWidth) continue;
    if (!best || s.height < best->height) best = &s;
  }
  if (!best) {
    const uint32_t height = needH + needH / 4;
    if (nextShelfY_ + height > kAtlasHeight) {
      if (nextShelfY_ + needH > kAtlasHeight) return false;
      shelves_.push_back({nextShelfY_, kAtlasHeight - nextShelfY_, 1});
    } else {
      shelves_.push_back({nextShelfY_, height, 1});
    }
    nextShelfY_ += shelves_.back().height;
    best = &shelves_.back();
  }
  x = best->x;
  y = best->y;
  best->x += needW;
  return true;
}

void Atlas::markDirty(uint32_t y0, uint32_t y1) noexcept {
  if (y1 <= y0) return;
  ++version_;
  if (changes_.size() >= kMaxChanges) {
    // Fold the older half into one conservative entry.
    for (size_t i = 0; i < kMaxChanges / 2; ++i) {
      const Change& c = changes_[i];
      if (folded_.y1 <= folded_.y0) {
        folded_.y0 = c.y0;
        folded_.y1 = c.y1;
      } else {
        folded_.y0 = std::min(folded_.y0, c.y0);
        folded_.y1 = std::max(folded_.y1, c.y1);
      }
      folded_.version = c.version;
    }
    changes_.erase(changes_.begin(), changes_.begin() + kMaxChanges / 2);
  }
  changes_.push_back({version_, y0, y1});
}

Atlas::RowRange Atlas::dirtyRowsSince(uint64_t since) const noexcept {
  RowRange r;
  auto add = [&r](uint32_t y0, uint32_t y1) {
    if (y1 <= y0) return;
    if (r.y1 <= r.y0) {
      r.y0 = y0;
      r.y1 = y1;
    } else {
      r.y0 = std::min(r.y0, y0);
      r.y1 = std::max(r.y1, y1);
    }
  };
  if (since < folded_.version) add(folded_.y0, folded_.y1);
  for (const Change& c : changes_)
    if (c.version > since) add(c.y0, c.y1);
  return r;
}

}  // namespace pq::ui
