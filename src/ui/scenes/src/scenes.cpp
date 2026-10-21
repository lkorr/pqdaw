#include "pq/ui/scenes.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include "pq/ui/gpu.h"
#include "pq/ui/offscreen.h"
#include "scenes_internal.h"

namespace pq::ui {

namespace {

constexpr std::string_view kNames[] = {"test-pattern", "text", "lines", "widgets-core", "primitives", "widgets", kPlayheadScene};

// The T0.7 acceptance scene: every primitive and pipeline the renderer offers, laid out in an 800×480 design
// space and scaled to the target. Colours are PQ Meter's signal-display tokens.
// HiDPI: the design space is stretched over the logical target (W/scale × H/scale) and then mapped to device
// px by the scale, which is the same as stretching it over the device target, so the scene does not read the
// scale. Hairlines (header divider, grid) and AA feathers are 1 device px at every scale.
void testPattern(DrawList& dl, const SceneTextures& tex, double time) {
  using namespace signal_theme;
  const float W = static_cast<float>(dl.targetWidth()), H = static_cast<float>(dl.targetHeight());
  const float sx = W / 800.0f, sy = H / 480.0f, s = std::min(sx, sy);
  // Design-space rect → target pixels. `snap` rounds to whole pixels for the non-AA rect primitive.
  auto R = [&](float x0, float y0, float x1, float y1) { return Rect{x0 * sx, y0 * sy, x1 * sx, y1 * sy}; };
  auto snap = [](Rect r) { return Rect{std::round(r.x0), std::round(r.y0), std::round(r.x1), std::round(r.y1)}; };
  auto P = [&](float x, float y) { return Vec2{x * sx, y * sy}; };

  // Header strip, a 1 device-px divider and grid (1 px lines at the grid token's α14).
  dl.addRectFilled(snap(R(0, 0, 800, 28)), kHeader);
  const float divider = std::round(28 * sy);
  dl.addRectFilled({0, divider, W, divider + 1}, withAlpha(kAccent, 90));
  for (int x = 40; x < 800; x += 40) {
    const float px = std::round(static_cast<float>(x) * sx);
    dl.addRectFilled({px, divider + 1, px + 1, H}, kGrid);
  }
  for (int y = 60; y < 480; y += 40) {
    const float py = std::round(static_cast<float>(y) * sy);
    dl.addRectFilled({0, py, W, py + 1}, kGrid);
  }
  // Header "traffic lights": tiny AA circles (rounded rects with radius = half size).
  for (int i = 0; i < 3; ++i) {
    const float cx = 16.0f + 16.0f * static_cast<float>(i);
    dl.addRoundedRectFilled(R(cx - 5, 9, cx + 5, 19), 5 * s, kBands[i]);
  }

  // Row 1: pixel-aligned solid swatches (band colours), then alpha-blending checks: translucent overlaps.
  for (int i = 0; i < 6; ++i) {
    const float x = 16.0f + 56.0f * static_cast<float>(i);
    dl.addRectFilled(snap(R(x, 44, x + 48, 92)), kBands[i]);
  }
  dl.addRectFilled(snap(R(368, 44, 448, 92)), withAlpha(kWaveformR, 255));
  dl.addRectFilled(snap(R(408, 52, 488, 100)), withAlpha(kAccent, 128));
  dl.addRectFilled(snap(R(448, 60, 528, 108)), withAlpha(kViolet, 64));
  dl.addRectFilled(snap(R(552, 44, 784, 92)), kPanel);
  for (int i = 0; i < 8; ++i) {  // white at α 32·(i+1): an 8-step alpha ramp over the panel
    const float x = 560.0f + 28.0f * static_cast<float>(i);
    dl.addRectFilled(snap(R(x, 52, x + 24, 84)), rgba(255, 255, 255, 32u * static_cast<uint32_t>(i + 1) - (i == 7 ? 1u : 0u)));
  }

  // Row 2: antialiased rounded rects, radius 0..pill, at fractional positions so the AA edges show.
  const float radii[6] = {0, 2, 4, 8, 16, 28};
  const Rgba fills[6] = {kAccent, kViolet, kWaveform, kWaveformR, kBands[1], kHeaderText};
  for (int i = 0; i < 6; ++i) {
    const float x = 16.25f + 128.0f * static_cast<float>(i);
    dl.addRoundedRectFilled(R(x, 116.5f, x + 112, 172.5f), radii[i] * s, fills[i]);
  }
  // Nested: a dark rounded rect inside a lighter one (border look, as widgets will use).
  // (drawn over the last swatch's right half so overlap AA is exercised too)
  dl.addRoundedRectFilled(R(700.25f, 124.5f, 760.25f, 164.5f), 12 * s, kPanel);

  // Row 3: linear gradients via vertex colours.
  dl.addRectFilledMultiColor(R(16, 188, 392, 236), kAccent, kViolet, kViolet, kAccent);            // horizontal
  dl.addRoundedRectGradientV(R(408, 188, 584, 236), 10 * s, kWaveform, withAlpha(kWaveform, 0));  // vertical fade
  dl.addRectFilledMultiColor(R(600, 188, 784, 236), kBands[0], kBands[1], kBands[2], kBands[4]);  // 4-corner

  // Row 4: textured quads. Magma ramp (linear), checker scaled 6× with nearest and linear sampling.
  dl.addImage(tex.magmaRamp, R(16, 252, 592, 300), {0.0f, 0.5f}, {1.0f, 0.5f});
  dl.addImage(tex.checker, snap(R(608, 252, 656, 300)), {0, 0}, {1, 1});
  dl.addImage(tex.checkerLinear, snap(R(672, 252, 720, 300)), {0, 0}, {1, 1});
  dl.addImage(tex.checker, snap(R(736, 252, 784, 300)), {0, 0}, {1, 1}, withAlpha(kViolet, 160));  // tinted

  // Row 5: a clipped panel of additive glow blobs ("light" mixing) and heat dots.
  const Rect panel = R(16, 316, 784, 464);
  dl.addRoundedRectFilled(panel, 6 * s, kPanel);
  dl.pushClip(panel);
  dl.setBlend(BlendMode::Additive);
  const float t = static_cast<float>(time);
  struct Blob {
    float x, y, r;
    Rgba col;
  };
  const Blob blobs[] = {
      {150, 395, 100, kAccent}, {245, 380, 95, kViolet},
      {325, 410, 85, kWaveform}, {400, 365, 75, kWaveformR},
      {40, 330, 70, withAlpha(kBands[1], 180)},  // partly outside the panel: clipped
      {770, 450, 90, withAlpha(kBands[5], 200)},  // ditto, bottom-right corner
  };
  for (int i = 0; i < static_cast<int>(std::size(blobs)); ++i) {
    const Blob& b = blobs[i];
    const float phase = static_cast<float>(i) * 1.7f;
    const Vec2 c = P(b.x + 18.0f * std::sin(t * 0.7f + phase), b.y + 10.0f * std::cos(t * 0.9f + phase));
    dl.addBlob(c, b.r * s, b.col);
  }
  // Heat dots: a row of small blobs in band colours, doubled up so additive saturation is visible.
  for (int i = 0; i < 12; ++i) {
    const Vec2 c = P(520.0f + 20.0f * static_cast<float>(i), 360.0f);
    dl.addBlob(c, 10 * s, kBands[i % 6]);
    if (i % 2 == 0) dl.addBlob(c, 6 * s, kBands[i % 6]);
  }
  dl.setBlend(BlendMode::Alpha);
  // Alpha-blended overlay inside the clip, drawn after the additive pass (blend switch back).
  dl.addRoundedRectFilled(R(520, 400, 760, 440), 8 * s, withAlpha(kBackground, 170));
  dl.addRoundedRectGradientV(R(532, 410, 748, 430), 4 * s, kAccent, kViolet);
  dl.popClip();
}

}  // namespace

void sceneDefaultSize(std::string_view name, uint32_t& width, uint32_t& height) noexcept {
  if (name == "widgets") {
    width = 1040;
    height = 720;
    return;
  }
  width = kDefaultSceneWidth;
  height = kDefaultSceneHeight;
}

bool isWidgetScene(std::string_view scene) noexcept { return scene == "widgets-core" || scene == "widgets"; }

void buildWidgetScene(std::string_view scene, UiTree& tree) {
  if (scene == "widgets") buildWidgetsGallery(tree);
  else if (scene == "widgets-core") tree.setRoot(makeWidgetsCoreDemo());
}

std::span<const std::string_view> sceneNames() noexcept { return kNames; }

bool hasScene(std::string_view name) noexcept { return std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames); }

bool buildScene(std::string_view name, DrawList& dl, const SceneTextures& textures, double timeSeconds) {
  if (name == "test-pattern") {
    testPattern(dl, textures, timeSeconds);
    return true;
  }
  if (name == "text") {
    detail::textScene(dl, textures, timeSeconds);
    return true;
  }
  if (name == "lines") {
    detail::linesScene(dl, textures, timeSeconds);
    return true;
  }
  if (name == "widgets-core") {
    detail::widgetsScene(dl, textures, timeSeconds);
    return true;
  }
  if (name == "primitives") {
    detail::primitivesScene(dl, textures, timeSeconds);
    return true;
  }
  if (name == "widgets") {
    detail::controlsScene(dl, textures, timeSeconds);
    return true;
  }
  if (sceneIsLayered(name)) {
    buildSceneStatic(name, dl, textures);
    buildSceneOverlay(name, dl, textures, timeSeconds);
    return true;
  }
  return false;
}

ClipRect playheadRect(uint32_t w, uint32_t h, double t) noexcept {
  const double phase = t / kPlayheadPeriodSeconds - std::floor(t / kPlayheadPeriodSeconds);
  const int32_t span = std::max(0, static_cast<int32_t>(w) - kPlayheadWidth);
  const int32_t x = std::min(span, static_cast<int32_t>(phase * static_cast<double>(span + 1)));
  return {x, 0, x + kPlayheadWidth, static_cast<int32_t>(h)};
}

bool sceneIsLayered(std::string_view name) noexcept { return name == kPlayheadScene; }

bool buildSceneStatic(std::string_view name, DrawList& dl, const SceneTextures& textures) {
  if (!sceneIsLayered(name)) return false;
  detail::linesScene(dl, textures, 0.0);
  return true;
}

bool buildSceneOverlay(std::string_view name, DrawList& dl, const SceneTextures& /*textures*/, double timeSeconds) {
  if (!sceneIsLayered(name)) return false;
  const ClipRect r = playheadRect(dl.targetWidth(), dl.targetHeight(), timeSeconds);
  dl.addRectFilled({static_cast<float>(r.x0), static_cast<float>(r.y0), static_cast<float>(r.x1), static_cast<float>(r.y1)},
                   rgba(235, 240, 255, 225));
  return true;
}

Result<Image> renderSceneImage(std::string_view scene, uint32_t width, uint32_t height, const GpuOptions& options,
                               UiScale scale) {
  if (!hasScene(scene)) return Error{ErrorCode::NotFound, "unknown scene '" + std::string(scene) + "'", {}};
  auto gpu = GpuContext::create(options);
  if (!gpu) return std::move(gpu).error();
  auto renderer = Renderer::create(**gpu, OffscreenTarget::kFormat, 1);
  if (!renderer) return std::move(renderer).error();
  if (width == 0 || height == 0) return Error{ErrorCode::InvalidArgument, "renderSceneImage: zero size", {}};
  // Logical size → device px (at least 1 × 1).
  const uint32_t dw = static_cast<uint32_t>(std::max(1, scale.px(static_cast<float>(width))));
  const uint32_t dh = static_cast<uint32_t>(std::max(1, scale.px(static_cast<float>(height))));
  auto target = OffscreenTarget::create(**gpu, dw, dh);
  if (!target) return std::move(target).error();
  auto textures = createSceneTextures(**renderer, scale);
  if (!textures) return std::move(textures).error();
  DrawList dl;
  dl.reset(dw, dh);
  buildScene(scene, dl, *textures, 0.0);
  if (Status s = (*target)->draw(**renderer, dl, kSceneClearColor); !s) return std::move(s).error();
  auto img = (*target)->read();
  if (img && (*gpu)->validationErrorCount() != 0)
    return Error{ErrorCode::Device, std::to_string((*gpu)->validationErrorCount()) + " Vulkan validation error(s)", {}};
  return img;
}

}  // namespace pq::ui
