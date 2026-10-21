#pragma once

// The built-in atlas texture (TextureId kAtlasTexture): a white block for untextured geometry and a radial
// glow sprite. T0.8 extends this with glyphs; the layout here only fixes where the two built-ins live.

#include <cstdint>

#include "pq/ui/draw_list.h"
#include "pq/ui/image.h"

namespace pq::ui {

inline constexpr uint32_t kAtlasWidth = 128;
inline constexpr uint32_t kAtlasHeight = 64;
inline constexpr uint32_t kAtlasBlobSize = 64;  // blob sprite occupies x ∈ [64, 128), y ∈ [0, 64)

// uv of the built-in sprites for the layout produced by buildBaseAtlas().
AtlasUv baseAtlasUv() noexcept;

// RGBA8 pixels of the base atlas. White block: texels [0,4)×[0,4). Blob: rgb white, alpha falls off as
// exp(-3r²)·(1-r²) (r = 1 at the sprite edge, zero beyond), so it fades smoothly to exactly 0 at its rim.
Image buildBaseAtlas();

}  // namespace pq::ui
