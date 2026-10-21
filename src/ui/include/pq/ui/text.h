#pragma once

// Text: FreeType (hinted, integer pixel sizes) + HarfBuzz shaping, glyph bitmaps in the shared atlas page
// (SPEC §11: crisp hinted bitmap glyphs, never MSDF or unhinted vector text).
//
//   - A face is a font file in memory (loadFace). A font is a face at an integer pixel size with options
//     (font()); each font has its own FT_Face + hb_font so sizes never fight over FT_Activate_Size.
//   - Glyphs are rasterised on first use (grayscale, hinted) into the Atlas (white, coverage in alpha) and
//     stay there; printable ASCII is rasterised when a font is created so a warm UI never touches FreeType.
//   - HarfBuzz shapes with the font's linear (unhinted) advances; each glyph origin and the baseline are then
//     snapped to whole pixels, so every texel lands on exactly one pixel (no resampling blur) and no glyph is
//     more than half a pixel from its ideal position (rounding hinted advances one by one accumulates error
//     into visibly uneven spacing at 12-14 px).
//   - Shaped runs are cached per (font, string): a cached draw is one hash lookup plus 4 vertices / 6 indices
//     per visible glyph. Shaping is the expensive part (≈2 µs for a 12-character label with Inter's kerning,
//     see bench/RESULTS.md), so only new strings pay it.
//
// UI thread only. Not RT-safe (allocates on cache misses, calls FreeType/HarfBuzz).

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/ui/atlas.h"
#include "pq/ui/color.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/ui_scale.h"

namespace pq::ui {

// Theme text sizes in logical pixels (SPEC §11 / PQ Meter settings_theme.h: section 12, body 13, title 14)
// plus a large size for meter readouts. At a UiScale they render as round(size × scale) px fonts (FontRole).
namespace text_size {
inline constexpr uint32_t kSection = 12;
inline constexpr uint32_t kBody = 13;
inline constexpr uint32_t kTitle = 14;
inline constexpr uint32_t kMeter = 24;
}  // namespace text_size

using FaceId = uint32_t;
using FontId = uint32_t;
inline constexpr FontId kNoFont = UINT32_MAX;

enum class TextHinting : uint8_t {
  Light,  // FT_LOAD_TARGET_LIGHT: vertical-only hinting, glyph shapes and widths kept (DirectWrite-like)
  Full,   // FT_LOAD_TARGET_NORMAL: the font's own TrueType instructions (Inter ships ttfautohint hints)
};

struct FontOptions {
  TextHinting hinting = TextHinting::Full;
  bool tabularFigures = false;  // OpenType 'tnum': equal-width digits, for readouts that update in place
  bool operator==(const FontOptions&) const = default;
};

// A font role: a face at a logical pixel size (a theme size) with options. TextSystem::font(role, scale)
// resolves it to the hinted integer-pixel font of size scale.fontPx(logicalPx) (HiDPI: a 13 px body role is
// a 20 px font at 150 %; glyphs are rasterised at that size, never resampled).
struct FontRole {
  FaceId face = 0;
  uint32_t logicalPx = 13;
  FontOptions options{};
};

struct FontMetrics {
  int32_t ascent = 0;      // px above the baseline (rounded up)
  int32_t descent = 0;     // px below the baseline (positive, rounded up)
  int32_t lineHeight = 0;  // baseline-to-baseline px
  uint32_t pixelSize = 0;
};

// One visible glyph of a shaped run, relative to the run's (pixel-snapped) baseline origin.
struct GlyphQuad {
  int16_t x0, y0;  // top-left in px
  uint16_t w, h;   // bitmap size in px
  Vec2 uv0, uv1;
};

// A shaped run. `glyphs` holds only glyphs with ink (spaces have none). Valid until the next shape()/draw*
// call on the same TextSystem (a cache flush may free it).
struct TextRun {
  int32_t advance = 0;  // px from the origin to the pen position after the last glyph
  std::span<const GlyphQuad> glyphs;
  // Per-cluster pen positions are not kept; advances of individual glyphs are available via shapeGlyphs().
};

// Shaping output of one glyph, before rasterisation (for tests and layout code that needs per-glyph data).
struct ShapedGlyph {
  uint32_t glyphId;
  uint32_t cluster;  // byte offset into the UTF-8 string
  int32_t xAdvance;  // 26.6 fixed point, as HarfBuzz reports it
  int32_t xOffset, yOffset;
};

class TextSystem {
public:
  struct Stats {
    uint64_t shapeCalls = 0;      // shape()/draw() lookups
    uint64_t cacheHits = 0;       // ... served from the run cache
    uint64_t glyphsRasterised = 0;
    uint64_t atlasFull = 0;       // glyphs dropped because the atlas page was full
    uint64_t cacheFlushes = 0;
    uint32_t cachedRuns = 0;
  };
  static constexpr uint32_t kMaxCachedRuns = 16384;  // the run cache is flushed when it would exceed this

  // `atlas` is normally Renderer::atlas(); glyphs packed into it reach the GPU on the renderer's next upload.
  static Result<std::unique_ptr<TextSystem>> create(std::shared_ptr<Atlas> atlas);
  ~TextSystem();
  TextSystem(const TextSystem&) = delete;
  TextSystem& operator=(const TextSystem&) = delete;

  Result<FaceId> loadFace(const std::string& path);
  Result<FaceId> loadFaceFromMemory(std::vector<uint8_t> bytes, std::string name);

  // The font for (face, pixel size, options), created on first request. pixelSize in 4..256.
  Result<FontId> font(FaceId face, uint32_t pixelSize, const FontOptions& options = {});
  // The font for `role` at `scale` (created on first request, like font(); later requests for the same
  // device size are a lookup, so switching back to a scale seen before costs no rasterisation).
  Result<FontId> font(const FontRole& role, UiScale scale) {
    return font(role.face, scale.fontPx(role.logicalPx), role.options);
  }
  const FontMetrics& metrics(FontId font) const noexcept;

  // Shaped, rasterised run for `utf8` in `font` (cached). Invalid UTF-8 shapes as U+FFFD.
  TextRun shape(FontId font, std::string_view utf8);
  int32_t measure(FontId font, std::string_view utf8) { return shape(font, utf8).advance; }

  // Draws `utf8` with its baseline origin at `baseline` (rounded to whole pixels). Returns the advance in px.
  int32_t draw(DrawList& dl, FontId font, Vec2 baseline, std::string_view utf8, Rgba col);
  // Draws a run returned by shape() (same origin rule).
  static void drawRun(DrawList& dl, const TextRun& run, Vec2 baseline, Rgba col);

  // Uncached HarfBuzz output (no rasterisation): what shape() builds its runs from.
  std::vector<ShapedGlyph> shapeGlyphs(FontId font, std::string_view utf8);

  const Stats& stats() const noexcept;
  void clearRunCache() noexcept;
  const std::shared_ptr<Atlas>& atlas() const noexcept;

private:
  TextSystem();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Directory holding the bundled assets (fonts/…): $PQ_ASSETS_DIR if set, else `assets/` next to the
// executable, else the source tree's assets/ directory recorded at build time (dev builds and tests).
std::string assetsDir();

}  // namespace pq::ui
