#pragma once

// Text: FreeType (hinted, integer pixel sizes) + HarfBuzz shaping, glyph bitmaps in the glyph atlas
// (SPEC §11: crisp hinted bitmap glyphs, never MSDF or unhinted vector text).
//
//   - A face is a font file in memory (loadFace). A font is a face at an integer pixel size with options
//     (font()); each font has its own FT_Face + hb_font so sizes never fight over FT_Activate_Size.
//   - Glyphs are rasterised on first use (grayscale, hinted) into the Atlas (8-bit coverage pages, see
//     pq/ui/atlas.h); printable ASCII is rasterised when a font is created so a warm UI never touches
//     FreeType. When every page is full the least recently used page not touched this frame is cleared and
//     glyphs on it are re-rasterised on their next use. If every page was used this frame the glyph is dropped
//     (Stats::atlasFull; reported once on stderr) and its run is rebuilt on a later frame.
//   - Frames: call beginFrame() once per frame before any shape/draw. Runs returned by shape() stay valid until
//     the next beginFrame() (or clearRunCache()): the cache is only flushed there, and the atlas never evicts a
//     page the current frame has used.
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
  uint32_t page;   // atlas page (drawn with atlasPageTexture(page))
  Vec2 uv0, uv1;
};

// A caret stop: the pen x (px, pixel-snapped like the glyph origins) at the start of the cluster that begins
// at UTF-8 byte offset `byte`. A run's stops are in logical order and end with {byte count, advance}.
struct CaretStop {
  uint32_t byte;
  int32_t x;
};

// A shaped run. `glyphs` holds only glyphs with ink (spaces have none). Valid until the next
// TextSystem::beginFrame() / clearRunCache() (M8: later shape() calls in the same frame never invalidate it).
struct TextRun {
  int32_t advance = 0;  // px from the origin to the pen position after the last glyph
  std::span<const GlyphQuad> glyphs;
  // One stop per cluster (a cluster is the unit HarfBuzz will not split: a character with its combining marks,
  // a ligature) plus the end stop. Left-to-right text (all UI scripts in scope). A caret can sit at any stop.
  std::span<const CaretStop> carets;
  // shapeTruncated(): true when the text did not fit and was cut, with "…" appended. visibleBytes is the
  // length of the source prefix shown (the whole string when not truncated); the ellipsis's stop has
  // byte == visibleBytes and the end stop byte == visibleBytes + 3.
  bool truncated = false;
  uint32_t visibleBytes = 0;
  uint32_t pageMask = 0;  // bit p set: some glyph sits on atlas page p (one bit: drawn as one span)

  // x of the caret at byte offset `byte` (the last stop at or before it; past the end → advance).
  int32_t caretX(uint32_t byte) const noexcept;
  // Byte offset of the caret stop nearest to x (ties go to the earlier stop): a click → caret position.
  uint32_t hitTest(int32_t x) const noexcept;
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
    uint64_t atlasFull = 0;       // glyphs dropped because every atlas page was in use this frame
    uint64_t runsRebuilt = 0;     // cached runs rebuilt after their atlas page was evicted (or a glyph dropped)
    uint64_t cacheFlushes = 0;
    uint32_t cachedRuns = 0;
  };
  // beginFrame() flushes the run cache when it holds more than this (wholesale: a UI's working set of labels
  // is far below it, so it only triggers under churn, e.g. a readout printing a new number every frame).
  static constexpr uint32_t kMaxCachedRuns = 16384;

  // `atlas` is normally Renderer::atlas(); glyphs packed into it reach the GPU with the renderer's next frame.
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

  // Frame boundary: flushes the run cache if it is over kMaxCachedRuns and lets the atlas evict pages that
  // earlier frames used. Runs returned before this call are invalid after it.
  void beginFrame();

  // Shaped, rasterised run for `utf8` in `font` (cached). Invalid UTF-8 shapes as U+FFFD.
  TextRun shape(FontId font, std::string_view utf8);
  int32_t measure(FontId font, std::string_view utf8);  // shape(font, utf8).advance
  // Like shape(), but when the run is wider than maxWidth px it is cut at a cluster boundary and "…" (U+2026)
  // is appended so that the result fits (spaces before the ellipsis are dropped). If not even "…" fits, the
  // run is empty (advance 0, truncated). Cached per (font, string, maxWidth).
  TextRun shapeTruncated(FontId font, std::string_view utf8, int32_t maxWidth);

  // Draws `utf8` with its baseline origin at `baseline` (rounded to whole pixels). Returns the advance in px.
  int32_t draw(DrawList& dl, FontId font, Vec2 baseline, std::string_view utf8, Rgba col);
  // draw() of shapeTruncated(): never wider than maxWidth px.
  int32_t drawTruncated(DrawList& dl, FontId font, Vec2 baseline, std::string_view utf8, int32_t maxWidth, Rgba col);
  // Draws a run returned by shape() (same origin rule). Restores the draw list's texture binding.
  static void drawRun(DrawList& dl, const TextRun& run, Vec2 baseline, Rgba col);

  // Uncached HarfBuzz output (no rasterisation): what shape() builds its runs from.
  std::vector<ShapedGlyph> shapeGlyphs(FontId font, std::string_view utf8);

  const Stats& stats() const noexcept;
  // Drops every cached run now (invalidates all TextRuns handed out): call it at a frame boundary only.
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
