#include "pq/ui/atlas.h"

#include <cmath>

namespace pq::ui {

namespace {
constexpr uint32_t kBlobX = kAtlasWidth - kAtlasBlobSize;
}

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
  img.rgba.assign(size_t(kAtlasWidth) * kAtlasHeight * 4, 0);
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) {
      uint8_t* p = img.pixel(x, y);
      p[0] = p[1] = p[2] = p[3] = 255;
    }
  // Glow sprite. r is measured at texel centres, so the outermost ring is already ~0 and clamp-to-edge
  // sampling past the quad edge cannot leak a hard border.
  constexpr float n = static_cast<float>(kAtlasBlobSize);
  for (uint32_t y = 0; y < kAtlasBlobSize; ++y)
    for (uint32_t x = 0; x < kAtlasBlobSize; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / n * 2.0f - 1.0f;
      const float v = (static_cast<float>(y) + 0.5f) / n * 2.0f - 1.0f;
      const float r2 = u * u + v * v;
      const float a = r2 < 1.0f ? std::exp(-3.0f * r2) * (1.0f - r2) : 0.0f;
      uint8_t* p = img.pixel(kBlobX + x, y);
      p[0] = p[1] = p[2] = 255;
      p[3] = static_cast<uint8_t>(std::lround(a * 255.0f));
    }
  return img;
}

}  // namespace pq::ui
