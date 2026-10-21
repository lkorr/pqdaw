#include "pq/ui/text.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  uint16_t w = 0, h = 0;      // 0 = no ink (space)
  uint32_t page = 0;          // atlas page and its generation when rasterised (stale once the page is evicted)
  uint32_t gen = 0;
  Vec2 uv0{}, uv1{};
};

struct RunEntry {
  int32_t advance = 0;
  std::vector<GlyphQuad> glyphs;
  std::vector<CaretStop> carets;
  uint64_t epoch = 0;       // atlas eviction epoch the glyphs were checked against
  uint64_t builtFrame = 0;  // atlas frame of the build (an incomplete run is rebuilt on a later frame)
  uint32_t pageMask = 0;    // atlas pages the glyphs sit on
  uint32_t visibleBytes = 0;
  bool incomplete = false;  // a glyph was dropped (every atlas page in use this frame)
  bool truncated = false;
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

// Truncated runs live in the same cache under (font | kTruncatedKey, utf8 + 4 bytes of maxWidth). Font ids are
// small indices, so the two key spaces never meet; within truncated keys equal length implies the width bytes
// sit at the same position, so equal keys mean equal (string, width).
constexpr FontId kTruncatedKey = 0x80000000u;
constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026

const FontMetrics kNoMetrics{};

std::filesystem::path pathFromUtf8(std::string_view s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::string utf8FromPath(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

TextRun viewOf(const RunEntry& e) noexcept {
  TextRun r;
  r.advance = e.advance;
  r.glyphs = e.glyphs;
  r.carets = e.carets;
  r.truncated = e.truncated;
  r.visibleBytes = e.visibleBytes;
  r.pageMask = e.pageMask;
  return r;
}

// Writes the glyph quads (4 vertices, 6 indices each) with one draw-list span per stretch of glyphs on the same
// atlas page; a run on one page (pageMask with one bit, the normal case) is one span with no per-glyph check.
void emitGlyphs(DrawList& dl, const GlyphQuad* q, uint32_t n, uint32_t pageMask, Vec2 baseline, Rgba col) {
  if (n == 0) return;
  const TextureId prevTex = dl.texture();
  const float ox = std::floor(baseline.x + 0.5f), oy = std::floor(baseline.y + 0.5f);
  const GlyphQuad* const end = q + n;
  const bool onePage = (pageMask & (pageMask - 1)) == 0;
  while (q != end) {
    const uint32_t page = q->page;
    const GlyphQuad* stop = end;
    if (!onePage) {
      stop = q + 1;
      while (stop != end && stop->page == page) ++stop;
    }
    const uint32_t count = static_cast<uint32_t>(stop - q);
    dl.setTexture(atlasPageTexture(page));
    const DrawList::PrimSpan sp = dl.primReserveSpan(count * 6, count * 4);
    Vertex* v = sp.v;
    uint32_t* ix = sp.i;
    uint32_t b = sp.base;
    for (; q != stop; ++q) {
      const GlyphQuad& g = *q;
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
  }
  dl.setTexture(prevTex);
}

}  // namespace

int32_t TextRun::caretX(uint32_t byte) const noexcept {
  if (carets.empty()) return 0;
  // Last stop with stop.byte <= byte.
  auto it = std::upper_bound(carets.begin(), carets.end(), byte, [](uint32_t b, const CaretStop& s) { return b < s.byte; });
  return it == carets.begin() ? carets.front().x : (it - 1)->x;
}

uint32_t TextRun::hitTest(int32_t x) const noexcept {
  if (carets.empty()) return 0;
  // First stop at or right of x, then pick the nearer of it and its predecessor.
  auto it = std::lower_bound(carets.begin(), carets.end(), x, [](const CaretStop& s, int32_t v) { return s.x < v; });
  if (it == carets.begin()) return it->byte;
  if (it == carets.end()) return carets.back().byte;
  const CaretStop& prev = *(it - 1);
  return (x - prev.x <= it->x - x) ? prev.byte : it->byte;
}

struct TextSystem::Impl {
  FT_Library lib = nullptr;
  hb_buffer_t* buf = nullptr;
  std::shared_ptr<Atlas> atlas;
  std::vector<Face> faces;
  std::vector<Font> fonts;
  std::unordered_map<uint64_t, GlyphSlot> glyphs;  // (font << 32) | glyph id
  std::unordered_map<RunKey, RunEntry, RunHash, RunEq> runs;
  std::string keyScratch;  // truncated-run lookup key (reused: no allocation once grown)
  std::string textScratch;  // prefix + "…" while truncating
  Stats stats;
  bool reportedFull = false;

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

  // The glyph's atlas slot, rasterised (again, if its page was evicted) on demand; touches its page.
  // nullptr: the glyph was dropped because no atlas space could be freed this frame.
  const GlyphSlot* glyph(FontId fid, uint32_t gid) {
    const uint64_t key = (uint64_t(fid) << 32) | gid;
    auto it = glyphs.find(key);
    if (it != glyphs.end()) {
      GlyphSlot& g = it->second;
      if (g.w == 0) return &g;
      if (atlas->generation(g.page) == g.gen) {
        atlas->touch(g.page);
        return &g;
      }
      return rasterise(fonts[fid], gid, g) ? &g : nullptr;  // page evicted since: rasterise again
    }
    GlyphSlot g;
    if (!rasterise(fonts[fid], gid, g)) return nullptr;
    return &glyphs.emplace(key, g).first->second;
  }

  // Fills `g`. Returns false only when the atlas had no room (g unchanged); no-ink glyphs succeed with w = 0.
  bool rasterise(const Font& f, uint32_t gid, GlyphSlot& g) {
    GlyphSlot out;
    if (FT_Load_Glyph(f.ft, gid, f.loadFlags) != 0) return (g = out), true;
    FT_GlyphSlot slot = f.ft->glyph;
    if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, f.renderMode) != 0) return (g = out), true;
    const FT_Bitmap& bm = slot->bitmap;
    if (bm.width == 0 || bm.rows == 0 || bm.pixel_mode != FT_PIXEL_MODE_GRAY) return (g = out), true;
    Atlas::Slot a;
    if (!atlas->allocate(bm.width, bm.rows, a)) {
      ++stats.atlasFull;
      if (!reportedFull) {
        reportedFull = true;
        std::fprintf(stderr,
                     "pq::ui: glyph atlas full: all %u pages (1024x1024) are in use by this frame; dropping glyphs "
                     "(reported once, counted in TextSystem::Stats::atlasFull)\n",
                     atlas->pageCount());
      }
      return false;
    }
    for (uint32_t y = 0; y < bm.rows; ++y)
      std::memcpy(atlas->texel(a.page, a.x, a.y + y), bm.buffer + static_cast<ptrdiff_t>(y) * bm.pitch, bm.width);
    atlas->markDirty(a.page, a.x, a.y, a.x + bm.width, a.y + bm.rows);
    ++stats.glyphsRasterised;
    out.left = static_cast<int16_t>(slot->bitmap_left);
    out.top = static_cast<int16_t>(slot->bitmap_top);
    out.w = static_cast<uint16_t>(bm.width);
    out.h = static_cast<uint16_t>(bm.rows);
    out.page = a.page;
    out.gen = atlas->generation(a.page);
    constexpr float iw = 1.0f / static_cast<float>(kAtlasWidth), ih = 1.0f / static_cast<float>(kAtlasHeight);
    out.uv0 = {static_cast<float>(a.x) * iw, static_cast<float>(a.y) * ih};
    out.uv1 = {static_cast<float>(a.x + bm.width) * iw, static_cast<float>(a.y + bm.rows) * ih};
    g = out;
    return true;
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

  // (Re)builds `run` in place for `utf8` (vectors keep their capacity).
  void buildRun(RunEntry& run, FontId fid, std::string_view utf8) {
    const Font& f = fonts[fid];
    const uint32_t n = hbShape(f, utf8);
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, nullptr);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, nullptr);
    run.glyphs.clear();
    run.carets.clear();
    run.glyphs.reserve(n);
    run.carets.reserve(n + 1);
    run.pageMask = 0;
    run.incomplete = false;
    int32_t pen = 0;  // 26.6
    for (uint32_t i = 0; i < n; ++i) {
      if (i == 0 || info[i].cluster != info[i - 1].cluster) run.carets.push_back({info[i].cluster, round26(pen)});
      const GlyphSlot* g = glyph(fid, info[i].codepoint);
      if (!g) {
        run.incomplete = true;
      } else if (g->w != 0) {
        // Each glyph origin is snapped to a whole pixel (the bitmap was rendered at a whole-pixel origin).
        const int32_t ox = round26(pen + pos[i].x_offset);
        const int32_t oy = -round26(pos[i].y_offset);
        run.glyphs.push_back({static_cast<int16_t>(ox + g->left), static_cast<int16_t>(oy - g->top), g->w, g->h, g->page, g->uv0, g->uv1});
        run.pageMask |= 1u << g->page;
      }
      pen += pos[i].x_advance;
    }
    run.advance = round26(pen);
    run.carets.push_back({static_cast<uint32_t>(utf8.size()), run.advance});
    run.visibleBytes = static_cast<uint32_t>(utf8.size());
    run.truncated = false;
    run.epoch = atlas->evictions();
    run.builtFrame = atlas->frame();
  }

  // True when a cached run must be rebuilt: one of its pages was evicted after it was built, or it lost a
  // glyph to a full atlas in an earlier frame. Otherwise its pages are touched for this frame.
  bool stale(RunEntry& e) {
    Atlas& a = *atlas;
    if (e.incomplete | (e.epoch != a.evictions())) [[unlikely]]
      return staleSlow(e);
    a.touchMask(e.pageMask);
    return false;
  }
  __declspec(noinline) bool staleSlow(RunEntry& e) {
    if (e.incomplete && e.builtFrame != atlas->frame()) return true;
    for (uint32_t m = e.pageMask; m != 0; m &= m - 1)
      if (atlas->evictedAt(static_cast<uint32_t>(std::countr_zero(m))) > e.epoch) return true;
    e.epoch = atlas->evictions();
    atlas->touchMask(e.pageMask);
    return false;
  }

  // Builds the truncated form of `utf8` (which does not fit in maxWidth) into `out`.
  // Pixel advance of `utf8` as buildRun would compute it, without rasterising anything.
  int32_t widthOf(FontId fid, std::string_view utf8) {
    const uint32_t n = hbShape(fonts[fid], utf8);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, nullptr);
    int32_t pen = 0;
    for (uint32_t i = 0; i < n; ++i) pen += pos[i].x_advance;
    return round26(pen);
  }

  // Builds the truncated form of `utf8` (whose run `full` is wider than maxWidth) into `out`: the longest
  // cluster prefix (trailing spaces dropped) whose prefix + "…" fits.
  void buildTruncated(RunEntry& out, FontId fid, std::string_view utf8, int32_t maxWidth, const RunEntry& full) {
    const int32_t ellipsis = shapeEntry(fid, kEllipsis).advance;
    const ptrdiff_t clusters = static_cast<ptrdiff_t>(full.carets.size()) - 1;  // the stops before the end stop
    auto cutAt = [&](ptrdiff_t c) {
      uint32_t cut = full.carets[static_cast<size_t>(c)].byte;
      while (cut > 0 && utf8[cut - 1] == ' ') --cut;  // "Hello …" → "Hello…"
      return cut;
    };
    auto fits = [&](uint32_t cut) {
      textScratch.assign(utf8.substr(0, cut));
      textScratch.append(kEllipsis);
      return widthOf(fid, textScratch) <= maxWidth;
    };
    // Estimate from the full run (stop c's x is the width of the prefix before it), then correct by shaping
    // the candidates: kerning against the ellipsis can move the answer by a cluster either way.
    ptrdiff_t c = clusters - 1;
    while (c >= 0 && full.carets[static_cast<size_t>(c)].x + ellipsis > maxWidth) --c;
    uint32_t best = UINT32_MAX;
    if (c >= 0 && fits(cutAt(c))) {
      best = cutAt(c);
      for (; c + 1 < clusters; ++c) {
        const uint32_t next = cutAt(c + 1);
        if (next != best && !fits(next)) break;
        best = next;
      }
    } else {
      while (--c >= 0)
        if (fits(cutAt(c))) {
          best = cutAt(c);
          break;
        }
    }
    if (best != UINT32_MAX) {
      textScratch.assign(utf8.substr(0, best));
      textScratch.append(kEllipsis);
      buildRun(out, fid, textScratch);
      out.truncated = true;
      out.visibleBytes = best;
      return;
    }
    // Not even "…" fits.
    out.glyphs.clear();
    out.carets.clear();
    out.carets.push_back({0, 0});
    out.advance = 0;
    out.pageMask = 0;
    out.incomplete = false;
    out.truncated = true;
    out.visibleBytes = 0;
    out.epoch = atlas->evictions();
    out.builtFrame = atlas->frame();
  }

  // The hit path stays small (lookup, two compares, one store); rebuilds and misses are out of line so
  // their stack frames and inlined shaping code do not tax every cached draw (measured: +12 ns/label).
  RunEntry& shapeEntry(FontId fid, std::string_view utf8) {
    ++stats.shapeCalls;
    auto it = runs.find(RunView{fid, utf8});
    if (it == runs.end()) [[unlikely]]
      return insertRun(fid, utf8);
    ++stats.cacheHits;
    RunEntry& e = it->second;
    if (stale(e)) [[unlikely]]
      rebuildRun(e, fid, utf8);
    return e;
  }

  __declspec(noinline) void rebuildRun(RunEntry& e, FontId fid, std::string_view utf8) {
    ++stats.runsRebuilt;
    buildRun(e, fid, utf8);
  }

  __declspec(noinline) RunEntry& insertRun(FontId fid, std::string_view utf8) {
    // The cache only grows here; beginFrame() trims it, so runs handed out earlier this frame stay valid
    // (unordered_map nodes never move).
    auto [pos, inserted] = runs.emplace(RunKey{fid, std::string(utf8)}, RunEntry{});
    (void)inserted;
    buildRun(pos->second, fid, utf8);
    return pos->second;
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

void TextSystem::beginFrame() {
  if (impl_->runs.size() > kMaxCachedRuns) {
    impl_->runs.clear();
    ++impl_->stats.cacheFlushes;
  }
  impl_->atlas->beginFrame();
}

TextRun TextSystem::shape(FontId font, std::string_view utf8) {
  if (font >= impl_->fonts.size() || utf8.empty()) return {};
  return viewOf(impl_->shapeEntry(font, utf8));
}

TextRun TextSystem::shapeTruncated(FontId font, std::string_view utf8, int32_t maxWidth) {
  if (font >= impl_->fonts.size()) return {};
  if (utf8.empty()) {
    TextRun r;
    r.truncated = maxWidth < 0;
    return r;
  }
  Impl& im = *impl_;
  RunEntry& full = im.shapeEntry(font, utf8);
  if (full.advance <= maxWidth) return viewOf(full);
  // Key: utf8 + the width's 4 bytes, under the truncated-key font id.
  std::string& key = im.keyScratch;
  key.assign(utf8);
  char wb[4];
  std::memcpy(wb, &maxWidth, 4);
  key.append(wb, 4);
  ++im.stats.shapeCalls;
  auto it = im.runs.find(RunView{font | kTruncatedKey, key});
  if (it != im.runs.end()) {
    ++im.stats.cacheHits;
    if (im.stale(it->second)) {
      ++im.stats.runsRebuilt;
      im.buildTruncated(it->second, font, utf8, maxWidth, full);
    }
    return viewOf(it->second);
  }
  auto [pos, inserted] = im.runs.emplace(RunKey{font | kTruncatedKey, key}, RunEntry{});
  (void)inserted;
  im.buildTruncated(pos->second, font, utf8, maxWidth, full);
  return viewOf(pos->second);
}

int32_t TextSystem::measure(FontId font, std::string_view utf8) {
  if (font >= impl_->fonts.size() || utf8.empty()) return 0;
  return impl_->shapeEntry(font, utf8).advance;
}

int32_t TextSystem::draw(DrawList& dl, FontId font, Vec2 baseline, std::string_view utf8, Rgba col) {
  if (font >= impl_->fonts.size() || utf8.empty()) return 0;
  const RunEntry& e = impl_->shapeEntry(font, utf8);
  emitGlyphs(dl, e.glyphs.data(), static_cast<uint32_t>(e.glyphs.size()), e.pageMask, baseline, col);
  return e.advance;
}

int32_t TextSystem::drawTruncated(DrawList& dl, FontId font, Vec2 baseline, std::string_view utf8, int32_t maxWidth, Rgba col) {
  const TextRun run = shapeTruncated(font, utf8, maxWidth);
  drawRun(dl, run, baseline, col);
  return run.advance;
}

void TextSystem::drawRun(DrawList& dl, const TextRun& run, Vec2 baseline, Rgba col) {
  emitGlyphs(dl, run.glyphs.data(), static_cast<uint32_t>(run.glyphs.size()), run.pageMask, baseline, col);
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
