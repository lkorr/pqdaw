#include "pq/ui/scene_textures.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include "pq/ui/image.h"

namespace pq::ui {

namespace {

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

}  // namespace

Result<SceneTextures> createSceneTextures(Renderer& renderer, UiScale scale) {
  auto text = createSceneText(renderer.atlas(), scale);
  if (!text) return std::move(text).error();
  return createSceneTextures(renderer, std::move(*text));
}

Result<SceneTextures> createSceneTextures(Renderer& renderer, SceneTextures text) {
  SceneTextures t = std::move(text);
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
  t.tiles = renderer.tiles();
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
  auto theme = loadTheme(defaultThemePath());
  if (!theme) return std::move(theme).error();
  t.theme = std::make_shared<const Theme>(std::move(*theme));
  if (Status s = setSceneScale(t, scale); !s) return std::move(s).error();
  return t;
}

Status setSceneTheme(SceneTextures& t, std::shared_ptr<const Theme> theme) {
  if (!theme) return Error{ErrorCode::InvalidArgument, "setSceneTheme: null theme", {}};
  if (t.text) {
    auto f = resolveThemeFonts(*t.text, t.regular, t.semiBold, *theme, t.scale);
    if (!f) return std::move(f).error();
    t.uiFonts = *f;
  }
  t.theme = std::move(theme);
  return Ok{};
}

PaintResources paintResources(const SceneTextures& t) noexcept {
  PaintResources r;
  r.text = t.text.get();
  r.fonts = &t.fonts;
  r.theme = t.theme.get();
  r.uiFonts = &t.uiFonts;
  r.tiles = t.tiles.get();
  return r;
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
  ThemeFonts ui = t.uiFonts;
  if (t.theme) {
    auto f = resolveThemeFonts(*t.text, t.regular, t.semiBold, *t.theme, scale);
    if (!f) return std::move(f).error();
    ui = *f;
  }
  t.fonts = resolved;
  t.uiFonts = ui;
  t.scale = scale;
  return Ok{};
}

}  // namespace pq::ui
