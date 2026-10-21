#pragma once

// Draw list: the one geometry format every widget and visualiser emits (SPEC §11).
//
// Vertex layout is ImGui's ImDrawVert {float2 pos, float2 uv, u32 rgba8} (20 bytes) with 32-bit indices, so
// PQ Meter's primitives (smooth_line.h, gradientPolyline, ...) port by replacing ImDrawList::PrimReserve /
// PrimWriteVtx / PrimWriteIdx / _VtxCurrentIdx with primReserve / primWriteVtx / primWriteIdx / vtxCurrentIdx.
//
// Every vertex samples a texture; untextured geometry points its uv at a white texel of the atlas
// (whiteUv()), so solid fills, gradients, glow sprites and (later) glyphs share one pipeline and one
// texture binding. Commands only split when blend mode, texture or clip rect changes.
//
// Antialiasing is geometric (PQ Meter house style, SPEC §11): filled shapes get a 1 px alpha ramp centred on
// the true edge (0.5 px inside, 0.5 px outside), which is a box-filter coverage estimate for straight
// edges. No MSAA, no SDF fragment work: fill cost is one vertex-colour * texel multiply per pixel.
//
// Not thread-safe; owned by the UI thread. Building does not allocate once capacity has grown to the
// frame's size (reset() keeps capacity).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include "pq/ui/color.h"

namespace pq::ui {

struct Vec2 {
  float x, y;
};

struct Rect {
  float x0, y0, x1, y1;
  float width() const noexcept { return x1 - x0; }
  float height() const noexcept { return y1 - y0; }
};

struct Vertex {
  Vec2 pos;
  Vec2 uv;
  Rgba col;
};
static_assert(sizeof(Vertex) == 20, "must match ImDrawVert");
static_assert(offsetof(Vertex, pos) == 0 && offsetof(Vertex, uv) == 8 && offsetof(Vertex, col) == 16);

enum class BlendMode : uint8_t {
  Alpha,     // dst = src*srcA + dst*(1-srcA)
  Additive,  // dst = src*srcA + dst            (glow sprites, heat dots; PQ Meter face.cpp blendAdditive)
};
inline constexpr uint32_t kBlendModeCount = 2;

// Index into the renderer's texture table. kAtlasTexture holds the white texel and the glow sprite
// (and, from T0.8, glyphs).
using TextureId = uint32_t;
inline constexpr TextureId kAtlasTexture = 0;

// Where the built-in sprites live in the atlas texture, in normalised uv.
struct AtlasUv {
  Vec2 white;  // samples to exactly (1,1,1,1) under linear filtering
  Rect blob;   // radial falloff sprite: rgb white, alpha 1 at the centre → 0 at the rect edge
};

// Clip rect in target pixels (integer scissor). x1/y1 exclusive.
struct ClipRect {
  int32_t x0, y0, x1, y1;
  bool operator==(const ClipRect&) const = default;
};

struct DrawCmd {
  uint32_t idxOffset;  // first index in indices()
  uint32_t idxCount;
  ClipRect clip;
  TextureId texture;
  BlendMode blend;
};

// Growable array of trivially-copyable elements that does not value-initialise on growth: the draw list
// writes every element it reserves, so zero-filling would be pure waste on the hot path.
template <class T>
class PodBuffer {
public:
  T* data() noexcept { return data_.get(); }
  const T* data() const noexcept { return data_.get(); }
  uint32_t size() const noexcept { return size_; }
  uint32_t capacity() const noexcept { return capacity_; }
  void clear() noexcept { size_ = 0; }
  // Grows size by n and returns a pointer to the n new (uninitialised) elements.
  T* grow(uint32_t n) {
    if (size_ + n > capacity_) reallocate(size_ + n);
    T* p = data_.get() + size_;
    size_ += n;
    return p;
  }
  void shrinkTo(uint32_t n) noexcept { size_ = n < size_ ? n : size_; }
  T& operator[](uint32_t i) noexcept { return data_[i]; }
  const T& operator[](uint32_t i) const noexcept { return data_[i]; }
  T& back() noexcept { return data_[size_ - 1]; }
  const T& back() const noexcept { return data_[size_ - 1]; }
  std::span<const T> span() const noexcept { return {data_.get(), size_}; }

private:
  void reallocate(uint32_t need) {
    uint32_t cap = capacity_ ? capacity_ : 256;
    while (cap < need) cap *= 2;
    std::unique_ptr<T[]> next(new T[cap]);  // default-init: no zeroing for trivial T
    if (size_) std::memcpy(next.get(), data_.get(), sizeof(T) * size_);
    data_ = std::move(next);
    capacity_ = cap;
  }
  std::unique_ptr<T[]> data_;
  uint32_t size_ = 0;
  uint32_t capacity_ = 0;
};

class DrawList {
public:
  explicit DrawList(const AtlasUv& atlas) noexcept;
  DrawList();  // uses the built-in atlas layout (pq/ui/atlas.h)

  // Starts a new frame for a target of the given pixel size. Keeps capacity. Clip = whole target,
  // blend = Alpha, texture = atlas.
  void reset(uint32_t targetWidth, uint32_t targetHeight) noexcept;
  uint32_t targetWidth() const noexcept { return targetW_; }
  uint32_t targetHeight() const noexcept { return targetH_; }

  // --- state ------------------------------------------------------------------------------------
  void setBlend(BlendMode b) noexcept { blend_ = b; }
  BlendMode blend() const noexcept { return blend_; }
  void setTexture(TextureId t) noexcept { texture_ = t; }
  TextureId texture() const noexcept { return texture_; }
  // Intersects with the current clip (rounded outwards to whole pixels). Stack depth ≤ kMaxClipDepth.
  void pushClip(const Rect& r) noexcept;
  void popClip() noexcept;
  ClipRect clip() const noexcept { return clipStack_[clipDepth_]; }
  static constexpr uint32_t kMaxClipDepth = 32;

  // --- low-level writer (the PQ Meter port target) -----------------------------------------------
  // Reserves idxCount indices and vtxCount vertices in the current command; the caller must then write
  // exactly that many with primWriteVtx/primWriteIdx. Indices are absolute (base on vtxCurrentIdx()).
  void primReserve(uint32_t idxCount, uint32_t vtxCount);
  void primWriteVtx(Vec2 pos, Vec2 uv, Rgba col) noexcept {
    vtxWrite_->pos = pos;
    vtxWrite_->uv = uv;
    vtxWrite_->col = col;
    ++vtxWrite_;
  }
  void primWriteIdx(uint32_t i) noexcept { *idxWrite_++ = i; }
  uint32_t vtxCurrentIdx() const noexcept { return static_cast<uint32_t>(vtxWrite_ - vtx_.data()); }
  Vec2 whiteUv() const noexcept { return atlas_.white; }
  const AtlasUv& atlas() const noexcept { return atlas_; }

  // --- primitives ---------------------------------------------------------------------------------
  // Axis-aligned, no AA: intended for pixel-aligned rects (crisp edges). 4 vertices.
  void addRectFilled(const Rect& r, Rgba col);
  // Bilinear 4-corner gradient (linear gradients are the special cases tl=bl,tr=br or tl=tr,bl=br).
  void addRectFilledMultiColor(const Rect& r, Rgba tl, Rgba tr, Rgba br, Rgba bl);
  // Antialiased rounded rect (radius 0 = antialiased plain rect, for fractional coordinates).
  void addRoundedRectFilled(const Rect& r, float radius, Rgba col);
  // Same, with a vertical linear gradient from top to bottom colour.
  void addRoundedRectGradientV(const Rect& r, float radius, Rgba top, Rgba bottom);
  // Radial glow sprite (atlas blob) centred at c. Draw with BlendMode::Additive for light-like mixing.
  void addBlob(Vec2 c, float radius, Rgba col);
  // Textured quad. Switches to `tex` for this quad and restores the previous texture afterwards.
  void addImage(TextureId tex, const Rect& r, Vec2 uv0, Vec2 uv1, Rgba tint = rgba(255, 255, 255));

  // Segments per quarter circle used for a corner of the given radius (max chord error ≈ 0.2 px).
  static uint32_t cornerSegments(float radius) noexcept;

  // --- output -------------------------------------------------------------------------------------
  std::span<const Vertex> vertices() const noexcept { return {vtx_.data(), vtxCurrentIdx()}; }
  std::span<const uint32_t> indices() const noexcept { return idx_.span(); }
  std::span<const DrawCmd> commands() const noexcept { return cmd_.span(); }
  bool empty() const noexcept { return idx_.size() == 0; }

  // Same as primReserve, but hands back raw write pointers and the base vertex index instead of going through
  // primWriteVtx/primWriteIdx. Hot primitives (lines.h, text.h) write through these locals: MSVC does no
  // type-based alias analysis, so every store through the member write pointers forces a reload of the
  // pointer itself and the writes serialise through store-to-load forwarding (measured ~3x slower).
  struct PrimSpan {
    Vertex* v;
    uint32_t* i;
    uint32_t base;  // index of v[0]
  };
  PrimSpan primReserveSpan(uint32_t idxCount, uint32_t vtxCount);

private:
  void beginCmd();
  // Axis-aligned quad p0 (top-left) → p2 (bottom-right); colours clockwise from top-left.
  void addQuad(Vec2 p0, Vec2 p2, Vec2 uv0, Vec2 uv2, Rgba c0, Rgba c1, Rgba c2, Rgba c3);
  void addConvexAA(const Vec2* pts, const Rgba* cols, uint32_t n);
  void addRoundedRect(const Rect& r, float radius, Rgba top, Rgba bottom);

  AtlasUv atlas_;
  PodBuffer<Vertex> vtx_;
  PodBuffer<uint32_t> idx_;
  PodBuffer<DrawCmd> cmd_;
  PodBuffer<Vec2> scratchPts_;   // polygon outline scratch (reused across calls)
  PodBuffer<Vec2> scratchNrm_;
  PodBuffer<Rgba> scratchCol_;
  Vertex* vtxWrite_ = nullptr;
  uint32_t* idxWrite_ = nullptr;
  uint32_t targetW_ = 0, targetH_ = 0;
  ClipRect clipStack_[kMaxClipDepth + 1] = {};
  uint32_t clipDepth_ = 0;
  uint32_t clipOverflow_ = 0;
  BlendMode blend_ = BlendMode::Alpha;
  TextureId texture_ = kAtlasTexture;
  // Memo of the last corner radius's tessellation (see addRoundedRect).
  float arcRadius_ = -1.0f;
  uint32_t arcSegments_ = 0;
  float arcCos_ = 1.0f, arcSin_ = 0.0f;
};

}  // namespace pq::ui
