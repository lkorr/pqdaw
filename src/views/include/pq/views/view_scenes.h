#pragma once

// Named view scenes for `pqdaw --screenshot <scene> <out.png> --project <dir>` and the views goldens (T3.7). A view
// scene is the real shell (pq/views/shell/shell.h) over a project, optionally set up further (a view zoomed, a clip
// opened): the same code the window runs, rendered headless. Later view tasks add their scenes to the table in
// view_scenes.cpp (one entry each).

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "pq/core/result.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/ui_scale.h"
#include "pq/ui/widget.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::views {

std::span<const std::string_view> viewSceneNames() noexcept;
bool isViewScene(std::string_view scene) noexcept;
// Logical size used when --size is not given (and by the goldens).
void viewSceneDefaultSize(std::string_view scene, uint32_t& width, uint32_t& height) noexcept;

// T4.2: scenes over a patch file (`circuit`, `circuit-svf`, `circuit-svf-core`): the circuit viewer on that file.
bool isPatchScene(std::string_view scene) noexcept;

// Builds `scene` into `tree` over `ctx` (the project already loaded): the shell plus the scene's setup. The tree must
// have its viewport and resources. `patchFile`: the .pqp of a patch scene.
Result<std::unique_ptr<Shell>> buildViewScene(std::string_view scene, ui::UiTree& tree, ViewContext& ctx,
                                              std::string_view patchFile = {});

// Headless render: a fresh ViewContext (FakeTransport, stopped) with `projectDir` loaded (empty: a new default
// project), the scene built at width×height logical px and UI scale `scale`, painted once and read back. The status
// line is cleared after loading so goldens do not depend on its wording. For a patch scene `projectDir` is the .pqp
// file to show (no project is loaded).
Result<ui::Image> renderViewScene(std::string_view scene, std::string_view projectDir, uint32_t width, uint32_t height,
                                  ui::UiScale scale = {}, const ui::GpuOptions& options = {});

}  // namespace pq::views
