#include "pq/views/view_scenes.h"

#include <algorithm>
#include <string>

#include "pq/ui/draw_list.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mos_wheel/mos_chart.h"

namespace pq::views {

namespace {

struct SceneEntry {
  std::string_view name;
  uint32_t width, height;  // default logical size
  // Extra setup after the shell is built (nullptr: the shell as loaded).
  Status (*setup)(Shell& shell);
};

ArrangementView* arrangementOf(Shell& shell) {
  ui::Widget* w = shell.slot(ShellSlot::TrackArea).find("arrangement");
  return dynamic_cast<ArrangementView*>(w);
}

// T3.8: the arrangement at its default zoom, the start marker at bar 5 (tests/views/fixtures/arrangement).
Status setupArrangementBasic(Shell& shell) {
  if (!arrangementOf(shell)) return Error{ErrorCode::NotFound, "arrangement view not installed", {}};
  shell.context().setStartMarker(4 * kTicksPerWhole);
  return Ok{};
}

// T3.8: zoomed out to 14 logical px per 4/4 bar (adaptive grid and previews at a coarse level).
Status setupArrangementZoomedOut(Shell& shell) {
  ArrangementView* a = arrangementOf(shell);
  if (!a) return Error{ErrorCode::NotFound, "arrangement view not installed", {}};
  a->setTimeView(0.0, 14.0 / static_cast<double>(kTicksPerWhole));
  return Ok{};
}

// T3.13: the MOS region chart alone, filling the window: opened in the shell's overlay layer (not dismissable), so
// the scene is the chart widget exactly as a popover would host it.
MosChart* openMosChart(Shell& shell, const MosChartConfig& config) {
  ui::OverlayLayer& layer = shell.overlay();
  ui::UiTree* tree = layer.tree();
  if (!tree) return nullptr;
  auto chart = std::make_unique<MosChart>("mos-chart", config);
  const float sc = tree->scale();
  chart->setSize(ui::Length::fixed(static_cast<float>(tree->width()) / sc), ui::Length::fixed(static_cast<float>(tree->height()) / sc));
  ui::OverlayOptions o;
  o.placement = ui::Placement::AtPoint;
  o.dismissOnOutsideClick = false;
  o.takeFocus = false;
  auto& w = static_cast<MosChart&>(layer.open(std::move(chart), std::move(o)));
  tree->layoutNow();
  return &w;
}

// T3.13: 5L 2s selected at 7\12 (700 ¢), EDO markers up to 31, the whole octave.
Status setupMosChartDefault(Shell& shell) {
  MosChartConfig cfg;
  cfg.maxEdo = 31;
  MosChart* c = openMosChart(shell, cfg);
  if (!c) return Error{ErrorCode::NotFound, "no overlay layer for the MOS chart", {}};
  if (!c->select({5, 2}, 700.0)) return Error{ErrorCode::NotFound, "5L 2s not in the chart", {}};
  return Ok{};
}

// T3.13: zoomed to 655–745 ¢ around 5L 2s (generator 18\31), EDO markers and temperament ranges legible, the pointer
// resting on 7L 2s (size 9) so the hover card shows.
Status setupMosChartZoomed(Shell& shell) {
  MosChartConfig cfg;
  cfg.maxEdo = 31;
  MosChart* c = openMosChart(shell, cfg);
  if (!c) return Error{ErrorCode::NotFound, "no overlay layer for the MOS chart", {}};
  c->setView(655.0, 745.0);
  if (!c->select({5, 2}, 18.0 * 1200.0 / 31.0)) return Error{ErrorCode::NotFound, "5L 2s not in the chart", {}};
  ui::UiTree& tree = *shell.overlay().tree();
  tree.layoutNow();
  const ui::IRect row = c->rowRect(9);
  if (row.empty()) return Error{ErrorCode::NotFound, "row 9 not visible", {}};
  ui::PointerEvent e;
  e.type = ui::PointerType::Move;
  e.pos = {c->xOfCents(676.0), static_cast<float>(row.y + row.h / 2)};
  tree.dispatchPointer(e);
  return Ok{};
}

// One entry per scene. T3.8+ append theirs (arrangement-basic, pianoroll-31edo, mos-chart-default, …).
constexpr SceneEntry kScenes[] = {
    {"shell", 1280, 800, nullptr},
    {"arrangement-basic", 1280, 800, setupArrangementBasic},
    {"arrangement-zoomed-out", 1280, 800, setupArrangementZoomedOut},
    {"mos-chart-default", 800, 600, setupMosChartDefault},
    {"mos-chart-zoomed", 800, 600, setupMosChartZoomed},
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
  // Settle before the screenshot: run the frames and timers that change what is drawn (progressive work such as the
  // arrangement's note previews, a zoom settling), as the window would within its first frames.
  double now = 0.0;
  tree.update(now);
  ui::DrawList dl;
  for (int i = 0; i < 256 && (tree.needsFrame() || tree.nextWakeTime() <= now + 1.0); ++i) {
    dl.reset(dw, dh);
    textures->text->beginFrame();
    tree.paint(dl, res, now);
    now = std::max(now + 1.0 / 60.0, std::min(tree.nextWakeTime(), now + 1.0));
    tree.update(now);
  }
  dl.reset(dw, dh);
  textures->text->beginFrame();
  tree.paint(dl, res, now);
  if (Status s = (*target)->draw(**renderer, dl, ui::kSceneClearColor); !s) return std::move(s).error();
  auto img = (*target)->read();
  if (img && (*gpu)->validationErrorCount() != 0)
    return Error{ErrorCode::Device, std::to_string((*gpu)->validationErrorCount()) + " Vulkan validation error(s)", {}};
  return img;  // the shell and the tree are destroyed before the context they subscribe to (declaration order)
}

}  // namespace pq::views
