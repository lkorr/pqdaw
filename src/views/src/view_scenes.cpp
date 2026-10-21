#include "pq/views/view_scenes.h"

#include <algorithm>
#include <string>

#include "pq/ui/draw_list.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"
#include "pq/views/fake_transport.h"

namespace pq::views {

namespace {

struct SceneEntry {
  std::string_view name;
  uint32_t width, height;  // default logical size
  // Extra setup after the shell is built (nullptr: the shell as loaded).
  Status (*setup)(Shell& shell);
};

// One entry per scene. T3.8+ append theirs (arrangement-basic, pianoroll-31edo, mos-chart-default, …).
constexpr SceneEntry kScenes[] = {
    {"shell", 1280, 800, nullptr},
};

std::string_view kNames[std::size(kScenes)] = {};

const SceneEntry* findScene(std::string_view name) noexcept {
  for (const SceneEntry& e : kScenes)
    if (e.name == name) return &e;
  return nullptr;
}

}  // namespace

std::span<const std::string_view> viewSceneNames() noexcept {
  for (size_t i = 0; i < std::size(kScenes); ++i) kNames[i] = kScenes[i].name;
  return kNames;
}

bool isViewScene(std::string_view scene) noexcept { return findScene(scene) != nullptr; }

void viewSceneDefaultSize(std::string_view scene, uint32_t& width, uint32_t& height) noexcept {
  const SceneEntry* e = findScene(scene);
  width = e ? e->width : 1280;
  height = e ? e->height : 800;
}

Result<std::unique_ptr<Shell>> buildViewScene(std::string_view scene, ui::UiTree& tree, ViewContext& ctx) {
  const SceneEntry* e = findScene(scene);
  if (!e) return Error{ErrorCode::NotFound, "unknown view scene '" + std::string(scene) + "'", {}};
  auto shell = Shell::create(tree, ctx);
  if (!shell) return std::move(shell).error();
  if (e->setup)
    if (Status s = e->setup(**shell); !s) return std::move(s).error();
  tree.layoutNow();
  return shell;
}

Result<ui::Image> renderViewScene(std::string_view scene, std::string_view projectDir, uint32_t width, uint32_t height,
                                  ui::UiScale scale, const ui::GpuOptions& options) {
  if (!isViewScene(scene)) return Error{ErrorCode::NotFound, "unknown view scene '" + std::string(scene) + "'", {}};
  if (width == 0 || height == 0) return Error{ErrorCode::InvalidArgument, "renderViewScene: zero size", {}};
  FakeTransport transport;
  ViewContext ctx(transport);
  if (!projectDir.empty())
    if (Status s = ctx.openProject(projectDir); !s) return std::move(s).error();
  ctx.setStatus({});

  auto gpu = ui::GpuContext::create(options);
  if (!gpu) return std::move(gpu).error();
  auto renderer = ui::Renderer::create(**gpu, ui::OffscreenTarget::kFormat, 1);
  if (!renderer) return std::move(renderer).error();
  const auto dw = static_cast<uint32_t>(std::max(1, scale.px(static_cast<float>(width))));
  const auto dh = static_cast<uint32_t>(std::max(1, scale.px(static_cast<float>(height))));
  auto target = ui::OffscreenTarget::create(**gpu, dw, dh);
  if (!target) return std::move(target).error();
  auto textures = ui::createSceneTextures(**renderer, scale);
  if (!textures) return std::move(textures).error();
  const ui::PaintResources res = ui::paintResources(*textures);

  ui::UiTree tree;
  tree.setViewport(dw, dh, scale);
  tree.setResources(&res);
  auto shell = buildViewScene(scene, tree, ctx);
  if (!shell) return std::move(shell).error();
  tree.update(0.0);
  ui::DrawList dl;
  dl.reset(dw, dh);
  textures->text->beginFrame();
  tree.paint(dl, res, 0.0);
  if (Status s = (*target)->draw(**renderer, dl, ui::kSceneClearColor); !s) return std::move(s).error();
  auto img = (*target)->read();
  if (img && (*gpu)->validationErrorCount() != 0)
    return Error{ErrorCode::Device, std::to_string((*gpu)->validationErrorCount()) + " Vulkan validation error(s)", {}};
  return img;  // the shell and the tree are destroyed before the context they subscribe to (declaration order)
}

}  // namespace pq::views
