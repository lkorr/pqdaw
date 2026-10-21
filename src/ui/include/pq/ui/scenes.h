#pragma once

// Named test scenes for `pqdaw --screenshot <scene>` and golden-image tests. Scenes only use the draw list,
// so the same code renders headless (offscreen) and in a window.

#include <memory>
#include <span>
#include <string_view>

#include "pq/core/result.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/renderer.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"

namespace pq::ui {

inline constexpr uint32_t kDefaultSceneWidth = 800;
inline constexpr uint32_t kDefaultSceneHeight = 480;
inline constexpr Rgba kSceneClearColor = signal_theme::kBackground;

// Fonts the scenes draw with (Inter, bundled under assets/fonts).
struct SceneFonts {
  FontId section = kNoFont;    // Regular 12
  FontId body = kNoFont;       // Regular 13
  FontId title = kNoFont;      // Regular 14
  FontId bodySemi = kNoFont;   // SemiBold 13
  FontId titleSemi = kNoFont;  // SemiBold 14
  FontId meter = kNoFont;      // SemiBold 24, tabular figures
  FontId bodyLight = kNoFont;  // Regular 13 with light (vertical-only) hinting, for comparison
};

// Textures and fonts the scenes sample (created once per renderer).
struct SceneTextures {
  TextureId magmaRamp = kAtlasTexture;  // 256×1 PQ Meter Magma colormap, linear filtering
  TextureId checker = kAtlasTexture;    // 8×8 two-tone checker, nearest filtering
  TextureId checkerLinear = kAtlasTexture;  // same pixels, linear filtering
  std::shared_ptr<TextSystem> text;     // null: scenes skip their glyphs (draw-list-only benchmarks)
  SceneFonts fonts;
};
// Creates the textures and a text system on the renderer's atlas, loading the fonts from assetsDir().
Result<SceneTextures> createSceneTextures(Renderer& renderer);

std::span<const std::string_view> sceneNames() noexcept;
bool hasScene(std::string_view name) noexcept;

// Appends the scene to `dl`, which must already be reset to the target size. `timeSeconds` animates the
// scene for the interactive window (0 = the golden frame). Returns false for unknown names.
bool buildScene(std::string_view name, DrawList& dl, const SceneTextures& textures, double timeSeconds);

}  // namespace pq::ui
