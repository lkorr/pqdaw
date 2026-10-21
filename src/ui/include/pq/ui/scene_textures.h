#pragma once

// The fonts, theme and sample textures a UI tree or a test scene paints with (`SceneTextures`), and how to
// create them and re-resolve them at a new UI scale. Used by the window, TestDriver and the test scenes
// (pq/ui/scenes.h, in pq_ui_scenes, which is not part of the shipping toolkit).
//
// HiDPI (T0.7-fix3): fonts are roles in logical px resolved at SceneTextures::scale (pq/ui/ui_scale.h) to
// integer device-px fonts.

#include <memory>

#include "pq/core/result.h"
#include "pq/ui/atlas.h"
#include "pq/ui/renderer.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/ui_scale.h"
#include "pq/ui/widget.h"

namespace pq::ui {

inline constexpr Rgba kSceneClearColor = signal_theme::kBackground;

// Fonts the scenes draw with (Inter, bundled under assets/fonts), resolved at SceneTextures::scale: each id
// is the integer-px font round(role px × scale) of the role named in its comment.
struct SceneFonts {
  FontId section = kNoFont;    // Regular 12
  FontId body = kNoFont;       // Regular 13
  FontId title = kNoFont;      // Regular 14
  FontId bodySemi = kNoFont;   // SemiBold 13
  FontId titleSemi = kNoFont;  // SemiBold 14
  FontId meter = kNoFont;      // SemiBold 24, tabular figures
  FontId bodyLight = kNoFont;  // Regular 13 with light (vertical-only) hinting, for comparison
};
inline constexpr uint32_t kSceneFontRoleCount = 7;

// Textures and fonts the scenes sample (created once per renderer).
struct SceneTextures {
  TextureId magmaRamp = kAtlasTexture;  // 256×1 PQ Meter Magma colormap, linear filtering
  TextureId checker = kAtlasTexture;    // 8×8 two-tone checker, nearest filtering
  TextureId checkerLinear = kAtlasTexture;  // same pixels, linear filtering
  std::shared_ptr<TextSystem> text;     // null: scenes skip their glyphs (draw-list-only benchmarks)
  FaceId regular = 0, semiBold = 0;     // the loaded Inter faces (valid when `text` is set)
  UiScale scale{};                      // logical → device px for the scenes; `fonts` are resolved at it
  SceneFonts fonts;
  // The chrome theme (T3.2; assets/themes/ableton-dark.json) and its font roles resolved at `scale`.
  std::shared_ptr<const Theme> theme;
  ThemeFonts uiFonts;
};

// Creates the textures and a text system on the renderer's atlas, loading the fonts and the default theme
// from assetsDir() and resolving the font roles at `scale`.
Result<SceneTextures> createSceneTextures(Renderer& renderer, UiScale scale = {});
// Text only (no GPU): a text system on `atlas` with the fonts and theme loaded and resolved at `scale`; the
// texture ids stay kAtlasTexture. For CPU tests and benchmarks of the text paths.
Result<SceneTextures> createSceneText(std::shared_ptr<Atlas> atlas, UiScale scale = {});
// Sets `t.scale` and re-resolves `t.fonts` and `t.uiFonts` at it. Fonts at a device size not seen before are
// created now (FreeType face + printable-ASCII rasterisation, ≈1–2 ms each): call it when the scale changes
// (display-scale event, --scale), never from paint. Returning to a scale seen before is a lookup per role. On
// failure t is unchanged.
Status setSceneScale(SceneTextures& t, UiScale scale);
// Replaces the theme (hot reload) and resolves its font roles at t.scale. On failure t is unchanged.
Status setSceneTheme(SceneTextures& t, std::shared_ptr<const Theme> theme);
// What a widget tree paints with, pointing into `t` (valid while t lives and is not moved).
PaintResources paintResources(const SceneTextures& t) noexcept;

}  // namespace pq::ui
