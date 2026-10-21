#include "pq/ui/draw_list.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

#include "pq/ui/atlas.h"

namespace pq::ui {

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
void DrawList::beginCmd() {
  const ClipRect clip = clipStack_[clipDepth_];
  auto same = [&](const DrawCmd& c) { return c.clip == clip && c.texture == texture_ && c.blend == blend_; };
  if (cmd_.size() != 0) {
    if (same(cmd_.back())) return;
    if (cmd_.back().idxCount == 0) {
      cmd_.shrinkTo(cmd_.size() - 1);
      if (cmd_.size() != 0 && same(cmd_.back())) return;
    }
  }
  *cmd_.grow(1) = DrawCmd{idx_.size(), 0, clip, texture_, blend_};
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
  addQuad({r.x0, r.y0}, {r.x1, r.y1}, atlas_.white, atlas_.white, tl, tr, br, bl);
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
  addConvexAA(pts, cols, count);
}

// Antialiased convex polygon fill, clockwise in screen space (ImGui's AddConvexPolyFilled AA scheme):
// an inner ring 0.5 px inside the edge carrying the colour, an outer ring 0.5 px outside at alpha 0, a
// triangle fan over the inner ring and one fringe quad per edge. 2n vertices, 3(n-2) + 6n indices.
void DrawList::addConvexAA(const Vec2* pts, const Rgba* cols, uint32_t n) {
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
    en[i] = {dy, -dx};
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
    v[0] = {{pts[i].x - dx, pts[i].y - dy}, uv, cols[i]};
    v[1] = {{pts[i].x + dx, pts[i].y + dy}, uv, withAlpha(cols[i], 0)};
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
