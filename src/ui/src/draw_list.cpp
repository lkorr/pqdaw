#include "pq/ui/draw_list.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <numbers>

#include "pq/ui/atlas.h"

namespace pq::ui {

namespace {

constexpr uint32_t kMaxQuarterSegments = 64;  // cornerSegments' cap
constexpr float kPi = std::numbers::pi_v<float>;

// Quarter-circle unit vectors for every segment count q in [1, 64]: entry j of q is (cos, sin)(j·π/(2q)),
// j = 0..q, with the ends exactly (1, 0) and (0, 1). Built once (17 KB) so circles, arcs and rounded strokes
// pay no trig per shape; computed in double so every entry is correctly rounded.
struct QuarterTables {
  static constexpr uint32_t kTotal = kMaxQuarterSegments * (kMaxQuarterSegments + 3) / 2;  // Σ (q + 1)
  uint32_t offset[kMaxQuarterSegments + 1] = {};
  // Radius scale that gives the 4q-gon the circle's area: sqrt(2π / (N sin(2π/N))), N = 4q. An inscribed
  // polygon puts all of the chord sag inside the circle (a 10 px disc loses ~2.5 % of its coverage); scaled,
  // vertices bulge out by ≈ sag/3 and chord midpoints sit in by ≈ 2·sag/3, both still ≤ 0.2 px.
  float areaScale[kMaxQuarterSegments + 1] = {};
  Vec2 dirs[kTotal] = {};
  QuarterTables() noexcept {
    uint32_t o = 0;
    for (uint32_t q = 1; q <= kMaxQuarterSegments; ++q) {
      const double n = 4.0 * static_cast<double>(q);
      areaScale[q] = static_cast<float>(std::sqrt(2.0 * std::numbers::pi / (n * std::sin(2.0 * std::numbers::pi / n))));
      offset[q] = o;
      for (uint32_t j = 0; j <= q; ++j) {
        const double a = (std::numbers::pi * 0.5) * static_cast<double>(j) / static_cast<double>(q);
        dirs[o + j] = j == 0   ? Vec2{1, 0}
                      : j == q ? Vec2{0, 1}
                               : Vec2{static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
      }
      o += q + 1;
    }
  }
};

const QuarterTables& quarterTables() noexcept {
  static const QuarterTables t;
  return t;
}

const Vec2* quarterTable(uint32_t q) noexcept { return quarterTables().dirs + quarterTables().offset[q]; }

float circleAreaScale(uint32_t q) noexcept { return quarterTables().areaScale[q]; }

// Rotates a first-quadrant direction by m quarter turns (screen space: +x → +y → −x → −y).
inline Vec2 rotQuarter(Vec2 d, uint32_t m) noexcept {
  switch (m & 3u) {
    case 0: return d;
    case 1: return {-d.y, d.x};
    case 2: return {-d.x, -d.y};
    default: return {d.y, -d.x};
  }
}

// Walks the directions of a circle tessellated with 4q points, starting at point k0, without a division or a
// branch per point: the quadrant's rotation is a 2×2 matrix of 0/±1 applied with two multiply-adds.
class CircleWalk {
public:
  CircleWalk(const Vec2* quarter, uint32_t q, uint32_t k0) noexcept : quarter_(quarter), q_(q) {
    m_ = (k0 / q) & 3u;
    j_ = k0 % q;
    setQuadrant();
  }
  Vec2 dir() const noexcept {
    const Vec2 p = quarter_[j_];
    return {c_ * p.x - s_ * p.y, s_ * p.x + c_ * p.y};
  }
  void next() noexcept {
    if (++j_ == q_) {
      j_ = 0;
      m_ = (m_ + 1) & 3u;
      setQuadrant();
    }
  }

private:
  void setQuadrant() noexcept {
    static constexpr float kC[4] = {1, 0, -1, 0}, kS[4] = {0, 1, 0, -1};
    c_ = kC[m_];
    s_ = kS[m_];
  }
  const Vec2* quarter_;
  uint32_t q_, m_ = 0, j_ = 0;
  float c_ = 1, s_ = 0;
};

inline float snapPx(float v) noexcept { return std::floor(v + 0.5f); }

inline void quadIdx(uint32_t* ix, uint32_t a, uint32_t b, uint32_t c, uint32_t d) noexcept {
  ix[0] = a;
  ix[1] = b;
  ix[2] = c;
  ix[3] = a;
  ix[4] = c;
  ix[5] = d;
}

// The three quads (outer feather, core, inner feather) between ring cross-sections i and j, each 4 vertices
// ordered outer-feather, outer-core, inner-core, inner-feather.
inline void ringSegmentIdx(uint32_t* ix, uint32_t i, uint32_t j) noexcept {
  quadIdx(ix, i, j, j + 1, i + 1);
  quadIdx(ix + 6, i + 1, j + 1, j + 2, i + 2);
  quadIdx(ix + 12, i + 2, j + 2, j + 3, i + 3);
}

}  // namespace

DrawList::DrawList(const AtlasUv& atlas) noexcept : atlas_(atlas) { reset(0, 0); }
DrawList::DrawList() : DrawList(baseAtlasUv()) {}

void DrawList::reset(uint32_t targetWidth, uint32_t targetHeight) noexcept {
  vtx_.clear();
  idx_.clear();
  cmd_.clear();
  vtxWrite_ = vtx_.data();
  idxWrite_ = idx_.data();
  targetW_ = targetWidth;
  targetH_ = targetHeight;
  clipStack_[0] = {0, 0, static_cast<int32_t>(targetWidth), static_cast<int32_t>(targetHeight)};
  clipDepth_ = 0;
  clipOverflow_ = 0;
  blend_ = BlendMode::Alpha;
  texture_ = kAtlasTexture;
}

void DrawList::pushClip(const Rect& r) noexcept {
  assert(clipDepth_ < kMaxClipDepth && "clip stack overflow");
  if (clipDepth_ >= kMaxClipDepth) {  // keep push/pop balanced: the overflowing level reuses the top clip
    ++clipOverflow_;
    return;
  }
  const ClipRect& cur = clipStack_[clipDepth_];
  ClipRect c{static_cast<int32_t>(std::floor(r.x0)), static_cast<int32_t>(std::floor(r.y0)),
             static_cast<int32_t>(std::ceil(r.x1)), static_cast<int32_t>(std::ceil(r.y1))};
  c.x0 = std::max(c.x0, cur.x0);
  c.y0 = std::max(c.y0, cur.y0);
  c.x1 = std::max(c.x0, std::min(c.x1, cur.x1));
  c.y1 = std::max(c.y0, std::min(c.y1, cur.y1));
  clipStack_[++clipDepth_] = c;
}

void DrawList::popClip() noexcept {
  assert((clipDepth_ > 0 || clipOverflow_ > 0) && "popClip without pushClip");
  if (clipOverflow_ > 0) {
    --clipOverflow_;
  } else if (clipDepth_ > 0) {
    --clipDepth_;
  }
}

// Opens a command for the current state, or keeps appending to the last one when the state matches.
// An empty trailing command (state changed but nothing drawn) is replaced, and merges back into the
// command before it when the state flipped back, so A→B→A with nothing drawn under B stays one command.
// Texture compatibility: kAtlasTexture (shared sprites only) runs on any glyph atlas page, so a command on page
// p absorbs following fills, and a fills-only command adopts the page of the glyphs appended to it.
void DrawList::beginCmd() {
  const ClipRect clip = clipStack_[clipDepth_];
  const TextureId tex = texture_;
  auto same = [&](DrawCmd& c) {
    if (!(c.clip == clip) || c.blend != blend_) return false;
    if (c.texture == tex) return true;
    if (tex == kAtlasTexture && isAtlasPageTexture(c.texture)) return true;
    if (c.texture == kAtlasTexture && isAtlasPageTexture(tex)) {
      c.texture = tex;
      return true;
    }
    return false;
  };
  if (cmd_.size() != 0) {
    if (same(cmd_.back())) return;
    if (cmd_.back().idxCount == 0) {
      cmd_.shrinkTo(cmd_.size() - 1);
      if (cmd_.size() != 0 && same(cmd_.back())) return;
    }
  }
  *cmd_.grow(1) = DrawCmd{idx_.size(), 0, clip, tex, blend_};
}


void DrawList::primReserve(uint32_t idxCount, uint32_t vtxCount) {
  assert(vtxWrite_ == vtx_.data() + vtx_.size() && idxWrite_ == idx_.data() + idx_.size() &&
         "previous primReserve not fully written");
  beginCmd();
  cmd_.back().idxCount += idxCount;
  vtxWrite_ = vtx_.grow(vtxCount);
  idxWrite_ = idx_.grow(idxCount);
}

// Internal primitives write through local pointers rather than primWriteVtx/primWriteIdx (see draw_list.h).
DrawList::PrimSpan DrawList::primReserveSpan(uint32_t idxCount, uint32_t vtxCount) {
  primReserve(idxCount, vtxCount);
  const PrimSpan s{vtxWrite_, idxWrite_, vtxCurrentIdx()};
  vtxWrite_ += vtxCount;
  idxWrite_ += idxCount;
  return s;
}

void DrawList::addQuad(Vec2 p0, Vec2 p2, Vec2 uv0, Vec2 uv2, Rgba c0, Rgba c1, Rgba c2, Rgba c3) {
  const PrimSpan s = primReserveSpan(6, 4);
  s.v[0] = {{p0.x, p0.y}, {uv0.x, uv0.y}, c0};
  s.v[1] = {{p2.x, p0.y}, {uv2.x, uv0.y}, c1};
  s.v[2] = {{p2.x, p2.y}, {uv2.x, uv2.y}, c2};
  s.v[3] = {{p0.x, p2.y}, {uv0.x, uv2.y}, c3};
  const uint32_t b = s.base;
  s.i[0] = b;
  s.i[1] = b + 1;
  s.i[2] = b + 2;
  s.i[3] = b;
  s.i[4] = b + 2;
  s.i[5] = b + 3;
}

void DrawList::addRectFilled(const Rect& r, Rgba col) {
  if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
  addQuad({r.x0, r.y0}, {r.x1, r.y1}, atlas_.white, atlas_.white, col, col, col, col);
}

void DrawList::addRectFilledMultiColor(const Rect& r, Rgba tl, Rgba tr, Rgba br, Rgba bl) {
  if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
  // The bilinear surface is c(u,v) = tl + (tr−tl)u + (bl−tl)v + twist·uv with twist = tl−tr−bl+br. Two
  // Gouraud triangles miss the twist term by up to twist/4 and show a diagonal crease; with twist = 0 they
  // are exact, which covers every linear gradient.
  float c[4][4];  // [corner: tl, tr, bl, br][channel]
  const Rgba corners[4] = {tl, tr, bl, br};
  int twistMax = 0;
  for (uint32_t ch = 0; ch < 4; ++ch) {
    int v[4];
    for (uint32_t k = 0; k < 4; ++k) {
      v[k] = static_cast<int>((corners[k] >> (8 * ch)) & 0xFFu);
      c[k][ch] = static_cast<float>(v[k]);
    }
    twistMax = std::max(twistMax, std::abs(v[0] - v[1] - v[2] + v[3]));
  }
  if (twistMax == 0) {
    addQuad({r.x0, r.y0}, {r.x1, r.y1}, atlas_.white, atlas_.white, tl, tr, br, bl);
    return;
  }
  // N×N cells, each a fan of 4 triangles around an exact centre vertex: the fan's worst error per cell is
  // twist/(16N²), so N = ceil(sqrt(twist/16)) keeps it ≤ 1/255 (twist ≤ 510 → N ≤ 6).
  const uint32_t n =
      std::clamp(static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(twistMax) / 16.0f))), 1u, 8u);
  const uint32_t g = n + 1;  // grid points per row
  const PrimSpan s = primReserveSpan(12 * n * n, g * g + n * n);
  const Vec2 uv = atlas_.white;
  const float invN = 1.0f / static_cast<float>(n);
  const float w = r.width(), h = r.height();
  auto colourAt = [&c](float u, float v) noexcept {
    uint32_t out = 0;
    for (uint32_t ch = 0; ch < 4; ++ch) {
      const float top = c[0][ch] + (c[1][ch] - c[0][ch]) * u;
      const float bot = c[2][ch] + (c[3][ch] - c[2][ch]) * u;
      const float x = top + (bot - top) * v;
      out |= static_cast<uint32_t>(std::clamp(x + 0.5f, 0.0f, 255.0f)) << (8 * ch);
    }
    return out;
  };
  Vertex* vt = s.v;
  for (uint32_t y = 0; y <= n; ++y) {
    const float v = static_cast<float>(y) * invN;
    const float py = y == n ? r.y1 : r.y0 + h * v;
    for (uint32_t x = 0; x <= n; ++x) {
      const float u = static_cast<float>(x) * invN;
      *vt++ = {{x == n ? r.x1 : r.x0 + w * u, py}, uv, colourAt(u, v)};
    }
  }
  const uint32_t centre0 = s.base + g * g;
  uint32_t* ix = s.i;
  for (uint32_t y = 0; y < n; ++y) {
    const float v = (static_cast<float>(y) + 0.5f) * invN;
    for (uint32_t x = 0; x < n; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) * invN;
      *vt++ = {{r.x0 + w * u, r.y0 + h * v}, uv, colourAt(u, v)};
      const uint32_t a = s.base + y * g + x, b = a + 1, d = a + g, e = d + 1, m = centre0 + y * n + x;
      const uint32_t tri[12] = {a, b, m, b, e, m, e, d, m, d, a, m};
      std::memcpy(ix, tri, sizeof tri);
      ix += 12;
    }
  }
}

float DrawList::snapThickness(float t) noexcept { return std::max(1.0f, snapPx(t)); }

void DrawList::addHLine(float x0, float x1, float y, Rgba col, float thickness) {
  const float t = snapThickness(thickness);
  float a = snapPx(x0), b = snapPx(x1);
  if (b < a) std::swap(a, b);
  if (b <= a) return;
  const float top = std::floor(y - 0.5f * t + 0.5f);
  addQuad({a, top}, {b, top + t}, atlas_.white, atlas_.white, col, col, col, col);
}

void DrawList::addVLine(float x, float y0, float y1, Rgba col, float thickness) {
  const float t = snapThickness(thickness);
  float a = snapPx(y0), b = snapPx(y1);
  if (b < a) std::swap(a, b);
  if (b <= a) return;
  const float left = std::floor(x - 0.5f * t + 0.5f);
  addQuad({left, a}, {left + t, b}, atlas_.white, atlas_.white, col, col, col, col);
}

void DrawList::addRectStroke(const Rect& r0, Rgba col, float thickness) {
  const Rect r{snapPx(r0.x0), snapPx(r0.y0), snapPx(r0.x1), snapPx(r0.y1)};
  if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
  const float t = snapThickness(thickness);
  if (2.0f * t >= std::min(r.width(), r.height())) {
    addRectFilled(r, col);
    return;
  }
  // Outer corners 0..3 then inner corners 4..7, clockwise from top-left; side k is the trapezoid between
  // outer k → k+1 and inner k+1 → k. The shared diagonal edges at the corners are rasterised exactly once.
  const PrimSpan s = primReserveSpan(24, 8);
  const Vec2 uv = atlas_.white;
  const float xs[2][4] = {{r.x0, r.x1, r.x1, r.x0}, {r.x0 + t, r.x1 - t, r.x1 - t, r.x0 + t}};
  const float ys[2][4] = {{r.y0, r.y0, r.y1, r.y1}, {r.y0 + t, r.y0 + t, r.y1 - t, r.y1 - t}};
  for (uint32_t ring = 0; ring < 2; ++ring)
    for (uint32_t k = 0; k < 4; ++k) s.v[ring * 4 + k] = {{xs[ring][k], ys[ring][k]}, uv, col};
  for (uint32_t k = 0; k < 4; ++k) {
    const uint32_t k1 = (k + 1) & 3u;
    quadIdx(s.i + 6 * k, s.base + k, s.base + k1, s.base + 4 + k1, s.base + 4 + k);
  }
}

void DrawList::addRoundedRectStroke(const Rect& r0, float radius, Rgba col, float thickness) {
  const Rect r{snapPx(r0.x0), snapPx(r0.y0), snapPx(r0.x1), snapPx(r0.y1)};
  const float w = r.width(), h = r.height();
  if (w <= 0.0f || h <= 0.0f) return;
  const float t = snapThickness(thickness);
  radius = std::clamp(radius, 0.0f, 0.5f * std::min(w, h));
  if (2.0f * t >= std::min(w, h)) {
    addRoundedRectFilled(r, radius, col);
    return;
  }
  if (radius < 0.5f) {
    addRectStroke(r, col, t);
    return;
  }
  // Outer and inner contours share the tessellation directions d; a contour point is corner centre + d·radius.
  // Vertices per direction: outer edge ± 0.5 (feather, core), inner edge ± 0.5 (core, feather). On the
  // straight sides d is the axis normal, so integer edges give a ramp-free side: the core lands on the edge
  // pixels' centres and the feather on their neighbours' centres.
  const float ri = std::max(radius - t, 0.5f);  // an inner corner sharper than 0.5 px has no visible effect
  const Rect in{r.x0 + t, r.y0 + t, r.x1 - t, r.y1 - t};
  const Vec2 co[4] = {{r.x0 + radius, r.y0 + radius}, {r.x1 - radius, r.y0 + radius},
                      {r.x1 - radius, r.y1 - radius}, {r.x0 + radius, r.y1 - radius}};
  const Vec2 ci[4] = {{in.x0 + ri, in.y0 + ri}, {in.x1 - ri, in.y0 + ri}, {in.x1 - ri, in.y1 - ri}, {in.x0 + ri, in.y1 - ri}};
  const uint32_t q = cornerSegments(radius);
  const Vec2* quarter = quarterTable(q);
  const uint32_t pts = 4 * (q + 1);
  const PrimSpan s = primReserveSpan(18 * pts, 4 * pts);
  const Vec2 uv = atlas_.white;
  const Rgba clear = withAlpha(col, 0);
  const float rad[4] = {radius + 0.5f, radius - 0.5f, ri + 0.5f, ri - 0.5f};
  Vertex* v = s.v;
  for (uint32_t k = 0; k < 4; ++k) {
    // The top-left corner runs from −x to −y (third quadrant); rotation as a 0/±1 matrix, no branch per point.
    const Vec2 rc = rotQuarter({1, 0}, k + 2);
    for (uint32_t j = 0; j <= q; ++j) {
      const Vec2 d{rc.x * quarter[j].x - rc.y * quarter[j].y, rc.y * quarter[j].x + rc.x * quarter[j].y};
      v[0] = {{co[k].x + d.x * rad[0], co[k].y + d.y * rad[0]}, uv, clear};
      v[1] = {{co[k].x + d.x * rad[1], co[k].y + d.y * rad[1]}, uv, col};
      v[2] = {{ci[k].x + d.x * rad[2], ci[k].y + d.y * rad[2]}, uv, col};
      v[3] = {{ci[k].x + d.x * rad[3], ci[k].y + d.y * rad[3]}, uv, clear};
      v += 4;
    }
  }
  for (uint32_t p = 0; p < pts; ++p) ringSegmentIdx(s.i + 18 * p, s.base + 4 * p, s.base + 4 * (p + 1 == pts ? 0 : p + 1));
}

void DrawList::addConvexPolyFilled(std::span<const Vec2> pts, Rgba col) {
  const uint32_t n = static_cast<uint32_t>(pts.size());
  if (n < 3) return;
  float area2 = 0.0f;  // shoelace; positive = clockwise on screen (y down)
  for (uint32_t i = 0; i < n; ++i) {
    const Vec2 a = pts[i], b = pts[i + 1 == n ? 0 : i + 1];
    area2 += a.x * b.y - b.x * a.y;
  }
  if (area2 == 0.0f) return;
  addConvexAA(pts.data(), &col, 0, n, area2 > 0.0f ? 1.0f : -1.0f);
}

void DrawList::addTriangleFilled(Vec2 a, Vec2 b, Vec2 c, Rgba col) {
  const float area2 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (area2 == 0.0f) return;
  const Vec2 pts[3] = {a, b, c};
  addConvexAA(pts, &col, 0, 3, area2 > 0.0f ? 1.0f : -1.0f);
}

void DrawList::addCircleFilled(Vec2 c, float radius, Rgba col) {
  if (radius <= 0.0f) return;
  const uint32_t q = cornerSegments(radius);
  const Vec2* quarter = quarterTable(q);
  const uint32_t n = 4 * q;
  const PrimSpan s = primReserveSpan(9 * n, 2 * n + 1);
  const Vec2 uv = atlas_.white;
  const Rgba clear = withAlpha(col, 0);
  const float ks = circleAreaScale(q);
  const float rc = std::max(radius - 0.5f, 0.0f) * ks, rf = (radius + 0.5f) * ks;
  Vertex* v = s.v;
  *v++ = {c, uv, col};
  CircleWalk walk(quarter, q, 0);
  for (uint32_t k = 0; k < n; ++k, walk.next()) {
    const Vec2 d = walk.dir();
    v[0] = {{c.x + d.x * rc, c.y + d.y * rc}, uv, col};
    v[1] = {{c.x + d.x * rf, c.y + d.y * rf}, uv, clear};
    v += 2;
  }
  // Centre fan (no sliver triangles) + one feather quad per edge.
  uint32_t* ix = s.i;
  const uint32_t b = s.base;
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t i0 = b + 1 + 2 * k, i1 = b + 1 + 2 * (k + 1 == n ? 0 : k + 1);
    ix[0] = b;
    ix[1] = i0;
    ix[2] = i1;
    quadIdx(ix + 3, i0, i1, i1 + 1, i0 + 1);
    ix += 9;
  }
}

void DrawList::addCircleStroke(Vec2 c, float radius, Rgba col, float thickness) {
  if (radius <= 0.0f || thickness <= 0.0f) return;
  addRing(c, radius, thickness, 0.0f, 2.0f * kPi, true, LineCap::Butt, col);
}

void DrawList::addArcStroke(Vec2 c, float radius, float a0, float a1, Rgba col, float thickness, LineCap cap) {
  if (radius <= 0.0f || thickness <= 0.0f || !(a0 != a1) || !std::isfinite(a0) || !std::isfinite(a1)) return;
  if (a1 < a0) std::swap(a0, a1);
  const bool closed = a1 - a0 >= 2.0f * kPi;
  addRing(c, radius, thickness, a0, closed ? a0 + 2.0f * kPi : a1, closed, cap, col);
}

void DrawList::addRing(Vec2 c, float r, float thickness, float a0, float a1, bool closed, LineCap cap, Rgba col) {
  // Cross-section radii: outer feather, outer core, inner core, inner feather. Below 1 px the core collapses
  // onto r and alpha drops to 2t/(1+t), which keeps the profile's integral (the coverage) equal to t.
  const float h = 0.5f * thickness;
  float rad[4];
  Rgba core = col;
  rad[0] = r + h + 0.5f;
  rad[3] = std::max(r - h - 0.5f, 0.0f);
  if (thickness >= 1.0f) {
    rad[1] = r + h - 0.5f;
    rad[2] = std::max(r - h + 0.5f, 0.0f);
  } else {
    rad[1] = rad[2] = r;
    core = mulAlpha(col, 2.0f * thickness / (1.0f + thickness));
  }
  const Rgba clear = withAlpha(col, 0);
  const Rgba cols[4] = {clear, core, core, clear};
  const Vec2 uv = atlas_.white;
  const uint32_t q = cornerSegments(r + h);
  const Vec2* quarter = quarterTable(q);
  const uint32_t n4 = 4 * q;
  const float ks = circleAreaScale(q);
  for (float& x : rad) x *= ks;

  if (closed) {
    const PrimSpan s = primReserveSpan(18 * n4, 4 * n4);
    Vertex* v = s.v;
    CircleWalk walk(quarter, q, 0);
    for (uint32_t k = 0; k < n4; ++k, walk.next()) {
      const Vec2 d = walk.dir();
      for (uint32_t e = 0; e < 4; ++e) v[e] = {{c.x + d.x * rad[e], c.y + d.y * rad[e]}, uv, cols[e]};
      v += 4;
    }
    for (uint32_t k = 0; k < n4; ++k) ringSegmentIdx(s.i + 18 * k, s.base + 4 * k, s.base + 4 * (k + 1 == n4 ? 0 : k + 1));
    return;
  }

  // Open arc: exact end directions, interior directions from the table at the multiples of the step strictly
  // inside (a0, a1), so no chord spans more than one step.
  const float step = (0.5f * kPi) / static_cast<float>(q);
  const float invStep = 1.0f / step;
  constexpr float kEps = 1e-3f;  // in steps: an interior point this close to an end would be a degenerate chord
  const int64_t kFirst = static_cast<int64_t>(std::floor(a0 * invStep + kEps)) + 1;
  const int64_t kLast = static_cast<int64_t>(std::ceil(a1 * invStep - kEps)) - 1;
  const uint32_t interior = kLast >= kFirst ? static_cast<uint32_t>(kLast - kFirst + 1) : 0u;
  const uint32_t pts = interior + 2;
  const Vec2 d0{std::cos(a0), std::sin(a0)}, d1{std::cos(a1), std::sin(a1)};
  // Caps: butt = 4 transparent vertices + 3 quads; round = a half-disc fan of 2·qc segments.
  const bool round = cap == LineCap::Round;
  const uint32_t qc = round ? cornerSegments(h + 0.5f) : 0;
  const uint32_t capVtx = round ? 1 + 2 * (2 * qc - 1) : 4;
  const uint32_t capIdx = round ? 9 * (2 * qc) : 18;
  const PrimSpan s = primReserveSpan(18 * (pts - 1) + 2 * capIdx, 4 * pts + 2 * capVtx);
  Vertex* v = s.v;
  // Butt caps pull the end cross-section 0.5 px into the arc and fade out 0.5 px beyond the end (a box filter
  // centred on the end), limited to a quarter of the arc length so tiny arcs keep their shape.
  const float buttShift = round ? 0.0f : std::min(0.5f, 0.25f * r * (a1 - a0));
  const Vec2 t0{-d0.y, d0.x}, t1{-d1.y, d1.x};  // tangents towards increasing angle
  auto section = [&](Vec2 d, Vec2 shift) noexcept {
    for (uint32_t e = 0; e < 4; ++e) v[e] = {{c.x + d.x * rad[e] + shift.x, c.y + d.y * rad[e] + shift.y}, uv, cols[e]};
    v += 4;
  };
  section(d0, {t0.x * buttShift, t0.y * buttShift});
  if (interior) {
    const int64_t n4i = static_cast<int64_t>(n4);
    CircleWalk walk(quarter, q, static_cast<uint32_t>(((kFirst % n4i) + n4i) % n4i));
    for (uint32_t k = 0; k < interior; ++k, walk.next()) {
      const Vec2 d = walk.dir();
      for (uint32_t e = 0; e < 4; ++e) v[e] = {{c.x + d.x * rad[e], c.y + d.y * rad[e]}, uv, cols[e]};
      v += 4;
    }
  }
  section(d1, {-t1.x * buttShift, -t1.y * buttShift});
  uint32_t* ix = s.i;
  for (uint32_t p = 0; p + 1 < pts; ++p) {
    ringSegmentIdx(ix, s.base + 4 * p, s.base + 4 * (p + 1));
    ix += 18;
  }

  auto nextIdx = [&]() noexcept { return s.base + static_cast<uint32_t>(v - s.v); };
  // `out` is the unit tangent pointing away from the arc at that end.
  auto writeCap = [&](uint32_t endBase, Vec2 d, Vec2 out) noexcept {
    if (!round) {
      const uint32_t b = nextIdx();
      const Vec2 o{out.x * (2.0f * buttShift), out.y * (2.0f * buttShift)};
      for (uint32_t e = 0; e < 4; ++e) {
        const Vertex& src = s.v[endBase - s.base + e];
        *v++ = {{src.pos.x + o.x, src.pos.y + o.y}, uv, clear};
      }
      ringSegmentIdx(ix, endBase, b);
      ix += 18;
      return;
    }
    // Round: a half-disc around the stroke's mid-line point, sweeping from +d (outer side) through `out` to
    // −d. Its first and last core/feather vertices are the arc end's own, so there is no seam.
    const float rmid = 0.5f * (rad[1] + rad[2]);
    const Vec2 m{c.x + d.x * rmid, c.y + d.y * rmid};
    const float rcore = 0.5f * (rad[1] - rad[2]), rfea = 0.5f * (rad[0] - rad[3]);
    const Vec2* qt = quarterTable(qc);
    const uint32_t centre = nextIdx();
    *v++ = {m, uv, core};
    const uint32_t segs = 2 * qc;
    for (uint32_t j = 1; j < segs; ++j) {
      // φ = j·π/(2qc): (cos φ, sin φ) from the quarter table, the second quarter by a quarter turn.
      const Vec2 cs = j <= qc ? qt[j] : Vec2{-qt[j - qc].y, qt[j - qc].x};
      const Vec2 dir{d.x * cs.x + out.x * cs.y, d.y * cs.x + out.y * cs.y};
      *v++ = {{m.x + dir.x * rcore, m.y + dir.y * rcore}, uv, core};
      *v++ = {{m.x + dir.x * rfea, m.y + dir.y * rfea}, uv, clear};
    }
    auto coreIdx = [&](uint32_t j) noexcept {
      return j == 0 ? endBase + 1 : (j == segs ? endBase + 2 : centre + 1 + 2 * (j - 1));
    };
    for (uint32_t j = 0; j < segs; ++j) {
      const uint32_t c0 = coreIdx(j), c1 = coreIdx(j + 1);
      const uint32_t f0 = j == 0 ? endBase : c0 + 1;
      const uint32_t f1 = j + 1 == segs ? endBase + 3 : c1 + 1;
      ix[0] = centre;
      ix[1] = c0;
      ix[2] = c1;
      quadIdx(ix + 3, c0, c1, f1, f0);
      ix += 9;
    }
  };
  writeCap(s.base, d0, {-t0.x, -t0.y});
  writeCap(s.base + 4 * (pts - 1), d1, t1);
  assert(v == s.v + 4 * pts + 2 * capVtx);
  assert(ix == s.i + 18 * (pts - 1) + 2 * capIdx);
}

uint32_t DrawList::cornerSegments(float radius) noexcept {
  constexpr float kMaxError = 0.2f;  // px of chord sag; below what the 1 px AA ramp can show
  if (radius <= kMaxError) return 1;
  const float theta = 2.0f * std::acos(1.0f - kMaxError / radius);
  const float n = std::ceil((std::numbers::pi_v<float> * 0.5f) / theta);
  return static_cast<uint32_t>(std::clamp(n, 1.0f, 64.0f));
}

void DrawList::addRoundedRectFilled(const Rect& r, float radius, Rgba col) { addRoundedRect(r, radius, col, col); }

void DrawList::addRoundedRectGradientV(const Rect& r, float radius, Rgba top, Rgba bottom) {
  addRoundedRect(r, radius, top, bottom);
}

void DrawList::addRoundedRect(const Rect& r, float radius, Rgba top, Rgba bottom) {
  const float w = r.width(), h = r.height();
  if (w <= 0.0f || h <= 0.0f) return;
  radius = std::clamp(radius, 0.0f, 0.5f * std::min(w, h));
  // UI code draws runs of shapes with the same radius: memoise the segment count and rotation step so the
  // acos/cos/sin are paid once per distinct radius, not per shape.
  if (radius != arcRadius_) {
    arcRadius_ = radius;
    arcSegments_ = radius > 0.0f ? cornerSegments(radius) : 0;
    const float step = arcSegments_ ? (std::numbers::pi_v<float> * 0.5f) / static_cast<float>(arcSegments_) : 0.0f;
    arcCos_ = std::cos(step);
    arcSin_ = std::sin(step);
  }
  const uint32_t n = arcSegments_;
  const float cs = arcCos_, sn = arcSin_;
  const uint32_t maxPts = 4 * (n + 1);
  scratchPts_.clear();
  scratchCol_.clear();
  Vec2* pts = scratchPts_.grow(maxPts);
  Rgba* cols = scratchCol_.grow(maxPts);

  // Clockwise in screen space (y down), starting at the left end of the top-left arc. Each corner's arc
  // starts on one axis direction and ends exactly on the next, so straight sides stay axis-aligned.
  const Vec2 centres[4] = {{r.x0 + radius, r.y0 + radius}, {r.x1 - radius, r.y0 + radius},
                           {r.x1 - radius, r.y1 - radius}, {r.x0 + radius, r.y1 - radius}};
  const Vec2 axes[5] = {{-1, 0}, {0, -1}, {1, 0}, {0, 1}, {-1, 0}};
  const bool gradient = top != bottom;
  const float invH = 1.0f / h;
  uint32_t count = 0;
  for (uint32_t k = 0; k < 4; ++k) {
    Vec2 d = axes[k];
    for (uint32_t i = 0; i <= n; ++i) {
      if (i == n && n) d = axes[k + 1];
      const Vec2 p{centres[k].x + d.x * radius, centres[k].y + d.y * radius};
      // Drop coincident points (pill shapes have zero-length straight sides): they would give a
      // zero-length edge with no defined normal.
      if (count == 0 || std::fabs(p.x - pts[count - 1].x) + std::fabs(p.y - pts[count - 1].y) > 1e-3f) {
        pts[count] = p;
        cols[count] = gradient ? lerp(top, bottom, (p.y - r.y0) * invH) : top;
        ++count;
      }
      const float nx = d.x * cs - d.y * sn;
      d.y = d.x * sn + d.y * cs;
      d.x = nx;
    }
  }
  if (count > 1 && std::fabs(pts[0].x - pts[count - 1].x) + std::fabs(pts[0].y - pts[count - 1].y) <= 1e-3f) --count;
  addConvexAA(pts, cols, 1, count);
}

// Antialiased convex polygon fill, clockwise in screen space (ImGui's AddConvexPolyFilled AA scheme):
// an inner ring 0.5 px inside the edge carrying the colour, an outer ring 0.5 px outside at alpha 0, a
// triangle fan over the inner ring and one fringe quad per edge. 2n vertices, 3(n-2) + 6n indices.
void DrawList::addConvexAA(const Vec2* pts, const Rgba* cols, uint32_t colStride, uint32_t n, float orient) {
  if (n < 3) return;
  constexpr float kHalfFeather = 0.5f;
  scratchNrm_.clear();
  Vec2* en = scratchNrm_.grow(n);  // outward normal of edge i → i+1
  for (uint32_t i = 0; i < n; ++i) {
    const Vec2 a = pts[i], b = pts[i + 1 == n ? 0 : i + 1];
    float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    if (len2 > 0.0f) {
      const float inv = 1.0f / std::sqrt(len2);
      dx *= inv;
      dy *= inv;
    }
    en[i] = {dy * orient, -dx * orient};
  }

  const PrimSpan s = primReserveSpan(3 * (n - 2) + 6 * n, 2 * n);
  const uint32_t base = s.base;
  const Vec2 uv = atlas_.white;
  Vertex* v = s.v;
  Vec2 n0 = en[n - 1];
  for (uint32_t i = 0; i < n; ++i) {
    const Vec2 n1 = en[i];
    float dx = 0.5f * (n0.x + n1.x), dy = 0.5f * (n0.y + n1.y);
    n0 = n1;
    // Miter: scale the averaged normal so both adjacent edges are offset by exactly kHalfFeather; clamp
    // the scale at 2 (90°) so sharp corners don't spike.
    const float len2 = dx * dx + dy * dy;
    const float k = kHalfFeather / std::max(len2, 0.5f);
    dx *= k;
    dy *= k;
    const Rgba col = cols[i * colStride];
    v[0] = {{pts[i].x - dx, pts[i].y - dy}, uv, col};
    v[1] = {{pts[i].x + dx, pts[i].y + dy}, uv, withAlpha(col, 0)};
    v += 2;
  }
  uint32_t* ix = s.i;
  for (uint32_t i = 2; i < n; ++i) {
    ix[0] = base;
    ix[1] = base + 2 * (i - 1);
    ix[2] = base + 2 * i;
    ix += 3;
  }
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t j = i + 1 == n ? 0 : i + 1;
    const uint32_t ii = base + 2 * i, oi = ii + 1, ij = base + 2 * j, oj = ij + 1;
    ix[0] = ij;
    ix[1] = ii;
    ix[2] = oi;
    ix[3] = oi;
    ix[4] = oj;
    ix[5] = ij;
    ix += 6;
  }
}

void DrawList::addBlob(Vec2 c, float radius, Rgba col) {
  if (radius <= 0.0f) return;
  const Rect& uv = atlas_.blob;
  addQuad({c.x - radius, c.y - radius}, {c.x + radius, c.y + radius}, {uv.x0, uv.y0}, {uv.x1, uv.y1}, col, col, col, col);
}

void DrawList::addImage(TextureId tex, const Rect& r, Vec2 uv0, Vec2 uv1, Rgba tint) {
  if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
  const TextureId prev = texture_;
  texture_ = tex;
  addQuad({r.x0, r.y0}, {r.x1, r.y1}, uv0, uv1, tint, tint, tint, tint);
  texture_ = prev;
}

}  // namespace pq::ui
