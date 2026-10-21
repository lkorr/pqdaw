#pragma once

// Named test scenes for `pqdaw --screenshot <scene>` and golden-image tests. Scenes only use the draw list,
// so the same code renders headless (offscreen) and in a window.
//
// HiDPI (T0.7-fix3): scenes are laid out in logical px and drawn at SceneTextures::scale (pq/ui/ui_scale.h):
// the draw list's target is device px, positions are round(logical × scale), fonts are the scene's font roles
// resolved at the scale, and line widths / AA feathers / hairlines stay in device px. A scene at scale s on a
// W×H device target therefore shows the same logical layout as at scale 1 on (W/s)×(H/s). The test-pattern and
// lines scenes stretch an 800×480 design space over the target, so for them only hairlines, line widths and
// feathers distinguish a scale from a plain size change.

#include <memory>
#include <span>
#include <string_view>

#include "pq/core/result.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/renderer.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/ui_scale.h"
#include "pq/ui/widget.h"

namespace pq::ui {

// Logical scene size; the device size at scale s is round(800·s) × round(480·s).
inline constexpr uint32_t kDefaultSceneWidth = 800;
inline constexpr uint32_t kDefaultSceneHeight = 480;
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

std::span<const std::string_view> sceneNames() noexcept;
bool hasScene(std::string_view name) noexcept;
// The scene's logical size for --screenshot without --size and for its goldens: 800×480, except `widgets`
// (the T3.2 control gallery), which needs more room.
void sceneDefaultSize(std::string_view name, uint32_t& width, uint32_t& height) noexcept;

// Appends the scene to `dl`, which must already be reset to the target size in device px. The scene draws at
// `textures.scale`. `timeSeconds` animates the scene for the interactive window (0 = the golden frame).
// Returns false for unknown names.
bool buildScene(std::string_view name, DrawList& dl, const SceneTextures& textures, double timeSeconds);

class Widget;
class UiTree;
// The widget tree behind the `widgets-core` scene (T3.1 demo: rows/columns, nested splitters, a scroll view
// over virtualised content). The interactive window runs it live.
std::unique_ptr<Widget> makeWidgetsCoreDemo();

// The `widgets` scene (T3.2): every base control in normal / hover / pressed / focused / disabled, an
// interactive column, a meter strip, and an open context menu with a submenu, a popover and a tooltip.
// Builds the gallery as the tree's root; the tree must already have its viewport and resources (the menus
// measure their text when they open).
void buildWidgetsGallery(UiTree& tree);
// Whether `scene` is a live widget tree (the window runs it interactively), and its builder.
bool isWidgetScene(std::string_view scene) noexcept;
void buildWidgetScene(std::string_view scene, UiTree& tree);

}  // namespace pq::ui
