#pragma once

// Named test scenes for `pqdaw --screenshot <scene>` and golden-image tests. Scenes only use the draw list,
// so the same code renders headless (offscreen) and in a window. They live in pq_ui_scenes (src/ui/scenes),
// linked by the app and the tests but not part of the pq_ui toolkit (T0.1-fix1, review M0 L10).
//
// HiDPI (T0.7-fix3): scenes are laid out in logical px and drawn at SceneTextures::scale (pq/ui/ui_scale.h):
// the draw list's target is device px, positions are round(logical × scale), fonts are the scene's font roles
// resolved at the scale, and line widths / AA feathers / hairlines stay in device px. A scene at scale s on a
// W×H device target therefore shows the same logical layout as at scale 1 on (W/s)×(H/s). The test-pattern and
// lines scenes stretch an 800×480 design space over the target, so for them only hairlines, line widths and
// feathers distinguish a scale from a plain size change.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "pq/core/result.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/ui_scale.h"

namespace pq::ui {

// Logical scene size; the device size at scale s is round(800·s) × round(480·s).
inline constexpr uint32_t kDefaultSceneWidth = 800;
inline constexpr uint32_t kDefaultSceneHeight = 480;

std::span<const std::string_view> sceneNames() noexcept;
bool hasScene(std::string_view name) noexcept;
// The scene's logical size for --screenshot without --size and for its goldens: 800×480, except `widgets`
// (the T3.2 control gallery), which needs more room.
void sceneDefaultSize(std::string_view name, uint32_t& width, uint32_t& height) noexcept;

// One-call headless screenshot: creates a GPU context, renderer and target, builds `scene` at width×height
// logical px and UI scale `scale` (device size round(width·scale) × round(height·scale), fonts at
// round(role·scale) px) and returns the pixels. Fails for unknown scene names, a zero size or if no Vulkan 1.3
// device exists.
Result<Image> renderSceneImage(std::string_view scene, uint32_t width, uint32_t height, const GpuOptions& options = {},
                               UiScale scale = {});

// Appends the scene to `dl`, which must already be reset to the target size in device px. The scene draws at
// `textures.scale`. `timeSeconds` animates the scene for the interactive window (0 = the golden frame).
// Returns false for unknown names.
bool buildScene(std::string_view name, DrawList& dl, const SceneTextures& textures, double timeSeconds);

// --- layered scenes (T0.7-fix1 partial redraw) ----------------------------------------------------------
// `lines-playhead`: the `lines` scene, static (drawn at time 0), under a 2 device-px playhead that sweeps the
// target's width every kPlayheadPeriodSeconds. buildScene() draws both; the window draws the static part once
// into a retained layer and only the playhead per frame, damaging the playhead's old and new columns
// (pq/ui/renderer.h FrameDesc), which is the partial-redraw acceptance case.
inline constexpr std::string_view kPlayheadScene = "lines-playhead";
inline constexpr double kPlayheadPeriodSeconds = 4.0;
inline constexpr int32_t kPlayheadWidth = 2;  // device px
// The playhead's pixels at time t on a w×h target: [x, x + 2) × [0, h) with x a whole pixel.
ClipRect playheadRect(uint32_t w, uint32_t h, double t) noexcept;
// True for scenes with a static layer + animated overlay split.
bool sceneIsLayered(std::string_view name) noexcept;
// The static layer / the overlay at time t of a layered scene (appended to `dl`, already reset). False for
// other scenes.
bool buildSceneStatic(std::string_view name, DrawList& dl, const SceneTextures& textures);
bool buildSceneOverlay(std::string_view name, DrawList& dl, const SceneTextures& textures, double timeSeconds);

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
