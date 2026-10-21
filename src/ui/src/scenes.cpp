#include "pq/ui/scenes.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "scenes_internal.h"

namespace pq::ui {

namespace {

constexpr std::string_view kNames[] = {"test-pattern", "text", "lines", "widgets-core", "primitives"};

Image magmaRampImage() {
  using signal_theme::kMagma;
  Image img;
  img.width = 256;
  img.height = 1;
  img.rgba.resize(256 * 4);
  for (uint32_t i = 0; i < 256; ++i) {
    const float t = static_cast<float>(i) / 255.0f;
    size_t s = 0;
    while (s + 2 < std::size(kMagma) && t > kMagma[s + 1].t) ++s;
    const auto& a = kMagma[s];
    const auto& b = kMagma[s + 1];
    const float u = std::clamp((t - a.t) / (b.t - a.t), 0.0f, 1.0f);
    uint8_t* p = img.pixel(i, 0);
    p[0] = static_cast<uint8_t>(std::lround((a.r + (b.r - a.r) * u) * 255.0f));
    p[1] = static_cast<uint8_t>(std::lround((a.g + (b.g - a.g) * u) * 255.0f));
    p[2] = static_cast<uint8_t>(std::lround((a.b + (b.b - a.b) * u) * 255.0f));
    p[3] = 255;
  }
  return img;
}

Image checkerImage() {
  Image img;
  img.width = img.height = 8;
  img.rgba.resize(8 * 8 * 4);
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 8; ++x) {
      const Rgba c = ((x ^ y) & 1) ? signal_theme::kAccent : signal_theme::kHeader;
      uint8_t* p = img.pixel(x, y);
      p[0] = static_cast<uint8_t>(redOf(c));
      p[1] = static_cast<uint8_t>(greenOf(c));
      p[2] = static_cast<uint8_t>(blueOf(c));
      p[3] = 255;
    }
  return img;
}

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

Result<SceneTextures> createSceneTextures(Renderer& renderer, UiScale scale) {
  SceneTextures t;
  auto magma = renderer.createTexture(magmaRampImage(), TextureFilter::Linear);
  if (!magma) return std::move(magma).error();
  t.magmaRamp = *magma;
  const Image checker = checkerImage();
  auto nearest = renderer.createTexture(checker, TextureFilter::Nearest);
  if (!nearest) return std::move(nearest).error();
  t.checker = *nearest;
  auto linear = renderer.createTexture(checker, TextureFilter::Linear);
  if (!linear) return std::move(linear).error();
  t.checkerLinear = *linear;

  auto text = createSceneText(renderer.atlas(), scale);
  if (!text) return std::move(text).error();
  t.text = std::move(text->text);
  t.regular = text->regular;
  t.semiBold = text->semiBold;
  t.scale = text->scale;
  t.fonts = text->fonts;
  return t;
}

Result<SceneTextures> createSceneText(std::shared_ptr<Atlas> atlas, UiScale scale) {
  SceneTextures t;
  auto text = TextSystem::create(std::move(atlas));
  if (!text) return std::move(text).error();
  t.text = std::move(*text);
  const std::string fonts = assetsDir() + "/fonts/";
  auto regular = t.text->loadFace(fonts + "Inter-Regular.ttf");
  if (!regular) return std::move(regular).error();
  auto semi = t.text->loadFace(fonts + "Inter-SemiBold.ttf");
  if (!semi) return std::move(semi).error();
  t.regular = *regular;
  t.semiBold = *semi;
  if (Status s = setSceneScale(t, scale); !s) return std::move(s).error();
  return t;
}

Status setSceneScale(SceneTextures& t, UiScale scale) {
  if (!t.text) {
    t.scale = scale;
    return Ok{};
  }
  // The scene's font roles (logical px), in a fixed order so atlas packing is deterministic.
  struct Want {
    FontId SceneFonts::*id;
    FontRole role;
  };
  const Want wants[kSceneFontRoleCount] = {
      {&SceneFonts::section, {t.regular, text_size::kSection, {}}},
      {&SceneFonts::body, {t.regular, text_size::kBody, {}}},
      {&SceneFonts::title, {t.regular, text_size::kTitle, {}}},
      {&SceneFonts::bodySemi, {t.semiBold, text_size::kBody, {}}},
      {&SceneFonts::titleSemi, {t.semiBold, text_size::kTitle, {}}},
      {&SceneFonts::meter, {t.semiBold, text_size::kMeter, {TextHinting::Full, true}}},
      {&SceneFonts::bodyLight, {t.regular, text_size::kBody, {TextHinting::Light, false}}},
  };
  SceneFonts resolved;
  for (const Want& w : wants) {
    auto f = t.text->font(w.role, scale);
    if (!f) return std::move(f).error();
    resolved.*w.id = *f;
  }
  t.fonts = resolved;
  t.scale = scale;
  return Ok{};
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
  return false;
}

}  // namespace pq::ui
