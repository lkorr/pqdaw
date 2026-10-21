#include "pq/ui/text.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <unordered_map>

namespace pq::ui {

namespace {

// 26.6 fixed point → whole pixels, rounding half up (arithmetic shift floors negatives).
inline int32_t round26(int32_t v) noexcept { return (v + 32) >> 6; }
inline int32_t ceil26(int32_t v) noexcept { return (v + 63) >> 6; }

Error ftError(const char* what, FT_Error e) {
  return Error{ErrorCode::Device, std::string(what) + " failed (FreeType error " + std::to_string(e) + ")", {}};
}

struct Face {
  std::vector<uint8_t> bytes;  // FreeType and HarfBuzz read the font from here for the face's lifetime
  std::string name;
  hb_face_t* hb = nullptr;
};

struct Font {
  FaceId face = 0;
  uint32_t pixelSize = 0;
  FontOptions options;
  FT_Face ft = nullptr;
  hb_font_t* hb = nullptr;
  FT_Int32 loadFlags = 0;
  FT_Render_Mode renderMode = FT_RENDER_MODE_NORMAL;
  FontMetrics metrics;
};

struct GlyphSlot {
  int16_t left = 0, top = 0;  // bitmap offset from the glyph origin (top is up, FreeType convention)
  uint16_t w = 0, h = 0;      // 0 = no ink (space) or dropped (atlas full)
  Vec2 uv0{}, uv1{};
};

struct RunEntry {
  int32_t advance = 0;
  std::vector<GlyphQuad> glyphs;
};

struct RunKey {
  FontId font;
  std::string text;
};
struct RunView {
  FontId font;
  std::string_view text;
};
struct RunHash {
  using is_transparent = void;
  size_t operator()(const RunView& k) const noexcept {
    return std::hash<std::string_view>{}(k.text) ^ (size_t(k.font) * 0x9E3779B97F4A7C15ull);
  }
  size_t operator()(const RunKey& k) const noexcept { return (*this)(RunView{k.font, k.text}); }
};
struct RunEq {
  using is_transparent = void;
  static RunView v(const RunKey& k) noexcept { return {k.font, k.text}; }
  static RunView v(const RunView& k) noexcept { return k; }
  template <class A, class B>
  bool operator()(const A& a, const B& b) const noexcept {
    const RunView x = v(a), y = v(b);
    return x.font == y.font && x.text == y.text;
  }
};

const FontMetrics kNoMetrics{};

std::filesystem::path pathFromUtf8(std::string_view s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::string utf8FromPath(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

}  // namespace

struct TextSystem::Impl {
  FT_Library lib = nullptr;
  hb_buffer_t* buf = nullptr;
  std::shared_ptr<Atlas> atlas;
  std::vector<Face> faces;
  std::vector<Font> fonts;
  std::unordered_map<uint64_t, GlyphSlot> glyphs;  // (font << 32) | glyph id
  std::unordered_map<RunKey, RunEntry, RunHash, RunEq> runs;
  Stats stats;

  ~Impl() {
    for (Font& f : fonts) {
      if (f.hb) hb_font_destroy(f.hb);
      if (f.ft) FT_Done_Face(f.ft);
    }
    for (Face& f : faces)
      if (f.hb) hb_face_destroy(f.hb);
    if (buf) hb_buffer_destroy(buf);
    if (lib) FT_Done_FreeType(lib);
  }

  const GlyphSlot& glyph(FontId fid, uint32_t gid) {
    const uint64_t key = (uint64_t(fid) << 32) | gid;
    auto it = glyphs.find(key);
    if (it != glyphs.end()) return it->second;
    return glyphs.emplace(key, rasterise(fonts[fid], gid)).first->second;
  }

  GlyphSlot rasterise(const Font& f, uint32_t gid) {
    GlyphSlot g;
    if (FT_Load_Glyph(f.ft, gid, f.loadFlags) != 0) return g;
    FT_GlyphSlot slot = f.ft->glyph;
    if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, f.renderMode) != 0) return g;
    const FT_Bitmap& bm = slot->bitmap;
    if (bm.width == 0 || bm.rows == 0 || bm.pixel_mode != FT_PIXEL_MODE_GRAY) return g;
    uint32_t ax = 0, ay = 0;
    if (!atlas->allocate(bm.width, bm.rows, ax, ay)) {
      ++stats.atlasFull;
      return g;
    }
    for (uint32_t y = 0; y < bm.rows; ++y) {
      const uint8_t* src = bm.buffer + static_cast<ptrdiff_t>(y) * bm.pitch;
      uint8_t* dst = atlas->texel(ax, ay + y);
      for (uint32_t x = 0; x < bm.width; ++x) dst[x * 4 + 3] = src[x];  // rgb is already white
    }
    atlas->markDirty(ay, ay + bm.rows);
    ++stats.glyphsRasterised;
    g.left = static_cast<int16_t>(slot->bitmap_left);
    g.top = static_cast<int16_t>(slot->bitmap_top);
    g.w = static_cast<uint16_t>(bm.width);
    g.h = static_cast<uint16_t>(bm.rows);
    constexpr float iw = 1.0f / static_cast<float>(kAtlasWidth), ih = 1.0f / static_cast<float>(kAtlasHeight);
    g.uv0 = {static_cast<float>(ax) * iw, static_cast<float>(ay) * ih};
    g.uv1 = {static_cast<float>(ax + bm.width) * iw, static_cast<float>(ay + bm.rows) * ih};
    return g;
  }

  // Runs HarfBuzz on `utf8`; the output stays in `buf`.
  uint32_t hbShape(const Font& f, std::string_view utf8) {
    hb_buffer_clear_contents(buf);
    hb_buffer_add_utf8(buf, utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));
    hb_buffer_guess_segment_properties(buf);
    hb_feature_t feat[1];
    unsigned nFeat = 0;
    if (f.options.tabularFigures) feat[nFeat++] = {HB_TAG('t', 'n', 'u', 'm'), 1, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
    hb_shape(f.hb, buf, nFeat ? feat : nullptr, nFeat);
    return hb_buffer_get_length(buf);
  }

  RunEntry buildRun(FontId fid, std::string_view utf8) {
    const Font& f = fonts[fid];
    const uint32_t n = hbShape(f, utf8);
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, nullptr);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, nullptr);
    RunEntry run;
    run.glyphs.reserve(n);
    int32_t pen = 0;  // 26.6
    for (uint32_t i = 0; i < n; ++i) {
      const GlyphSlot& g = glyph(fid, info[i].codepoint);
      if (g.w != 0) {
        // Each glyph origin is snapped to a whole pixel (the bitmap was rendered at a whole-pixel origin).
        const int32_t ox = round26(pen + pos[i].x_offset);
        const int32_t oy = -round26(pos[i].y_offset);
        run.glyphs.push_back({static_cast<int16_t>(ox + g.left), static_cast<int16_t>(oy - g.top), g.w, g.h, g.uv0, g.uv1});
      }
      pen += pos[i].x_advance;
    }
    run.advance = round26(pen);
    return run;
  }
};

TextSystem::TextSystem() : impl_(std::make_unique<Impl>()) {}
TextSystem::~TextSystem() = default;

Result<std::unique_ptr<TextSystem>> TextSystem::create(std::shared_ptr<Atlas> atlas) {
  if (!atlas) return Error{ErrorCode::InvalidArgument, "TextSystem: no atlas", {}};
  std::unique_ptr<TextSystem> t(new TextSystem());
  if (FT_Error e = FT_Init_FreeType(&t->impl_->lib); e != 0) return ftError("FT_Init_FreeType", e);
  t->impl_->buf = hb_buffer_create();
  if (!hb_buffer_allocation_successful(t->impl_->buf)) return Error{ErrorCode::Device, "hb_buffer_create failed", {}};
  t->impl_->atlas = std::move(atlas);
  return t;
}

Result<FaceId> TextSystem::loadFace(const std::string& path) {
  std::ifstream in(pathFromUtf8(path), std::ios::binary);
  if (!in) return Error{ErrorCode::Io, "cannot open font '" + path + "'", {}};
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.empty()) return Error{ErrorCode::Io, "empty font file '" + path + "'", {}};
  return loadFaceFromMemory(std::move(bytes), path);
}

Result<FaceId> TextSystem::loadFaceFromMemory(std::vector<uint8_t> bytes, std::string name) {
  // Validate by opening it once.
  FT_Face probe = nullptr;
  if (FT_Error e = FT_New_Memory_Face(impl_->lib, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &probe); e != 0)
    return ftError(("FT_New_Memory_Face(" + name + ")").c_str(), e);
  const bool scalable = FT_IS_SCALABLE(probe);
  FT_Done_Face(probe);
  if (!scalable) return Error{ErrorCode::Unsupported, "font '" + name + "' is not scalable", {}};
  hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(bytes.data()), static_cast<unsigned>(bytes.size()),
                                   HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  hb_face_t* hbFace = hb_face_create(blob, 0);
  hb_blob_destroy(blob);  // the face keeps its own reference
  impl_->faces.push_back({std::move(bytes), std::move(name), hbFace});
  return static_cast<FaceId>(impl_->faces.size() - 1);
}

Result<FontId> TextSystem::font(FaceId face, uint32_t pixelSize, const FontOptions& options) {
  if (face >= impl_->faces.size()) return Error{ErrorCode::InvalidArgument, "TextSystem::font: bad face", {}};
  if (pixelSize < 4 || pixelSize > 256) return Error{ErrorCode::InvalidArgument, "TextSystem::font: size out of range", {}};
  for (size_t i = 0; i < impl_->fonts.size(); ++i) {
    const Font& f = impl_->fonts[i];
    if (f.face == face && f.pixelSize == pixelSize && f.options == options) return static_cast<FontId>(i);
  }
  const Face& fc = impl_->faces[face];
  Font f;
  f.face = face;
  f.pixelSize = pixelSize;
  f.options = options;
  if (FT_Error e = FT_New_Memory_Face(impl_->lib, fc.bytes.data(), static_cast<FT_Long>(fc.bytes.size()), 0, &f.ft); e != 0)
    return ftError("FT_New_Memory_Face", e);
  if (FT_Error e = FT_Set_Pixel_Sizes(f.ft, 0, pixelSize); e != 0) {
    FT_Done_Face(f.ft);
    return ftError("FT_Set_Pixel_Sizes", e);
  }
  const bool light = options.hinting == TextHinting::Light;
  f.loadFlags = FT_LOAD_NO_BITMAP | (light ? FT_LOAD_TARGET_LIGHT : FT_LOAD_TARGET_NORMAL);
  f.renderMode = light ? FT_RENDER_MODE_LIGHT : FT_RENDER_MODE_NORMAL;
  // Shaping runs on HarfBuzz's own OpenType tables at 26.6 scale, i.e. with the font's linear (unhinted)
  // advances; glyph origins are then rounded to whole pixels one by one. Per-glyph hinted advances (hb-ft with
  // hinting load flags) round every advance, and the errors pile up into visibly uneven letter and word
  // spacing at 12-14 px ("foxjumps", "t he" in SemiBold 13); with linear advances no glyph is ever more than
  // half a pixel from its ideal position. It is also cheaper: hb-ft's hinted advances load and hint glyphs.
  f.hb = hb_font_create(fc.hb);
  hb_font_set_scale(f.hb, static_cast<int>(pixelSize) * 64, static_cast<int>(pixelSize) * 64);
  const FT_Size_Metrics& m = f.ft->size->metrics;
  f.metrics.ascent = ceil26(static_cast<int32_t>(m.ascender));
  f.metrics.descent = ceil26(-static_cast<int32_t>(m.descender));
  f.metrics.lineHeight = ceil26(static_cast<int32_t>(m.height));
  f.metrics.pixelSize = pixelSize;
  impl_->fonts.push_back(f);
  const FontId id = static_cast<FontId>(impl_->fonts.size() - 1);
  // Impl's destructor releases the FT_Face and the hb_font.
  // Warm the printable ASCII range so ordinary UI text never rasterises mid-frame.
  for (uint32_t c = 0x20; c < 0x7F; ++c)
    if (const FT_UInt gid = FT_Get_Char_Index(f.ft, c); gid != 0) impl_->glyph(id, gid);
  return id;
}

const FontMetrics& TextSystem::metrics(FontId font) const noexcept {
  return font < impl_->fonts.size() ? impl_->fonts[font].metrics : kNoMetrics;
}

TextRun TextSystem::shape(FontId font, std::string_view utf8) {
  if (font >= impl_->fonts.size() || utf8.empty()) return {};
  ++impl_->stats.shapeCalls;
  auto it = impl_->runs.find(RunView{font, utf8});
  if (it != impl_->runs.end()) {
    ++impl_->stats.cacheHits;
    return {it->second.advance, it->second.glyphs};
  }
  if (impl_->runs.size() >= kMaxCachedRuns) {
    // Wholesale flush rather than LRU bookkeeping on every hit: a UI's working set of labels is far below the
    // cap, so this only triggers under churn (e.g. a readout printing a new number every frame), where the
    // runs being dropped are dead anyway.
    impl_->runs.clear();
    ++impl_->stats.cacheFlushes;
  }
  RunEntry run = impl_->buildRun(font, utf8);
  auto [pos, inserted] = impl_->runs.emplace(RunKey{font, std::string(utf8)}, std::move(run));
  (void)inserted;
  return {pos->second.advance, pos->second.glyphs};
}

int32_t TextSystem::draw(DrawList& dl, FontId font, Vec2 baseline, std::string_view utf8, Rgba col) {
  const TextRun run = shape(font, utf8);
  drawRun(dl, run, baseline, col);
  return run.advance;
}

void TextSystem::drawRun(DrawList& dl, const TextRun& run, Vec2 baseline, Rgba col) {
  const uint32_t n = static_cast<uint32_t>(run.glyphs.size());
  if (n == 0) return;
  const TextureId prevTex = dl.texture();
  if (prevTex != kAtlasTexture) dl.setTexture(kAtlasTexture);
  const float ox = std::floor(baseline.x + 0.5f), oy = std::floor(baseline.y + 0.5f);
  const DrawList::PrimSpan sp = dl.primReserveSpan(n * 6, n * 4);
  Vertex* v = sp.v;
  uint32_t* ix = sp.i;
  uint32_t b = sp.base;
  for (const GlyphQuad& g : run.glyphs) {
    const float x0 = ox + static_cast<float>(g.x0), y0 = oy + static_cast<float>(g.y0);
    const float x1 = x0 + static_cast<float>(g.w), y1 = y0 + static_cast<float>(g.h);
    v[0] = {{x0, y0}, g.uv0, col};
    v[1] = {{x1, y0}, {g.uv1.x, g.uv0.y}, col};
    v[2] = {{x1, y1}, g.uv1, col};
    v[3] = {{x0, y1}, {g.uv0.x, g.uv1.y}, col};
    ix[0] = b;
    ix[1] = b + 1;
    ix[2] = b + 2;
    ix[3] = b;
    ix[4] = b + 2;
    ix[5] = b + 3;
    v += 4;
    ix += 6;
    b += 4;
  }
  if (prevTex != kAtlasTexture) dl.setTexture(prevTex);
}

std::vector<ShapedGlyph> TextSystem::shapeGlyphs(FontId font, std::string_view utf8) {
  std::vector<ShapedGlyph> out;
  if (font >= impl_->fonts.size()) return out;
  const uint32_t n = impl_->hbShape(impl_->fonts[font], utf8);
  const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(impl_->buf, nullptr);
  const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(impl_->buf, nullptr);
  out.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
    out.push_back({info[i].codepoint, info[i].cluster, pos[i].x_advance, pos[i].x_offset, pos[i].y_offset});
  return out;
}

const TextSystem::Stats& TextSystem::stats() const noexcept {
  impl_->stats.cachedRuns = static_cast<uint32_t>(impl_->runs.size());
  return impl_->stats;
}

void TextSystem::clearRunCache() noexcept { impl_->runs.clear(); }

const std::shared_ptr<Atlas>& TextSystem::atlas() const noexcept { return impl_->atlas; }

std::string assetsDir() {
  namespace fs = std::filesystem;
  if (const char* env = std::getenv("PQ_ASSETS_DIR"); env && *env) return env;
  if (const char* base = SDL_GetBasePath(); base) {
    const fs::path p = pathFromUtf8(base) / "assets";
    std::error_code ec;
    if (fs::is_directory(p, ec)) return utf8FromPath(p);
  }
#ifdef PQ_SOURCE_ASSETS_DIR
  return PQ_SOURCE_ASSETS_DIR;
#else
  return "assets";
#endif
}

}  // namespace pq::ui
