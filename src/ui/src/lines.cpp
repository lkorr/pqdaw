#include "pq/ui/lines.h"

#include <algorithm>
#include <cmath>

// Port notes (vs PQ Meter's smooth_line.h / spectrum.cpp):
//   - Vertices and indices are written through DrawList::primReserveSpan's raw pointers instead of
//     PrimWriteVtx/PrimWriteIdx (MSVC reloads member write pointers after every store; ~3× slower).
//   - smoothPolyline computes each vertex normal on the fly from its neighbours and carries it to the next
//     segment, instead of filling a scratch array first, and computes each segment's length once instead of
//     twice (PQ Meter's ext() lambda re-derives it per end). Divisions by a length become one reciprocal.
//     Geometry is the same up to float rounding.

namespace pq::ui {

namespace {

inline void writeQuadIdx(uint32_t* ix, uint32_t base, uint32_t i0, uint32_t i1, uint32_t i2, uint32_t i3) noexcept {
  ix[0] = base + i0;
  ix[1] = base + i1;
  ix[2] = base + i2;
  ix[3] = base + i0;
  ix[4] = base + i2;
  ix[5] = base + i3;
}

// The three quads of a ribbon segment: core, upper feather, lower feather. Vertices 0..3 are the core ring
// (a+, b+, b−, a−), 4..7 the transparent outer ring in the same order.
inline void writeRibbonIdx(uint32_t* ix, uint32_t base) noexcept {
  writeQuadIdx(ix, base, 0, 1, 2, 3);
  writeQuadIdx(ix + 6, base, 4, 5, 1, 0);
  writeQuadIdx(ix + 12, base, 3, 2, 6, 7);
}

// Unit normal (-dy, dx) of the direction b − a, or (0, 1) for a degenerate direction (PQ Meter's fallback
// direction (1, 0)).
inline Vec2 unitNormal(Vec2 a, Vec2 b) noexcept {
  float dx = b.x - a.x, dy = b.y - a.y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len > 1e-6f) {
    const float inv = 1.0f / len;
    dx *= inv;
    dy *= inv;
  } else {
    dx = 1.0f;
    dy = 0.0f;
  }
  return {-dy, dx};
}

inline Rgba clearOf(Rgba c) noexcept { return c & 0x00FFFFFFu; }

}  // namespace

void splinePath(std::span<const Vec2> in, std::vector<Vec2>& out, float maxSegPx) {
  out.clear();
  const size_t n = in.size();
  if (n < 2) {
    out.assign(in.begin(), in.end());
    return;
  }
  // Ceiling on the output size, enforced by scaling the per-segment target up when the path's total arc
  // length demands it: on a trace dense enough for the budget to bite, the extra chords are invisible.
  float arc = 0.0f;
  for (size_t i = 0; i + 1 < n; ++i) {
    const float dx = in[i + 1].x - in[i].x, dy = in[i + 1].y - in[i].y;
    arc += std::sqrt(dx * dx + dy * dy);
  }
  const float target = std::max(std::max(0.25f, maxSegPx), arc / kSplineMaxOutPoints);
  const float invTarget = 1.0f / target;
  out.reserve(n * 2);
  out.push_back(in[0]);
  for (size_t i = 0; i + 1 < n; ++i) {
    const Vec2 p1 = in[i];
    const Vec2 p2 = in[i + 1];
    const Vec2 p0 = in[i > 0 ? i - 1 : 0];
    const Vec2 p3 = in[i + 2 < n ? i + 2 : n - 1];
    const float dx = p2.x - p1.x, dy = p2.y - p1.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    // A segment already at or under the target adds nothing by being split.
    const int steps = std::clamp(static_cast<int>(std::ceil(len * invTarget)), 1, kSplineMaxStepsPerSegment);
    if (steps == 1) {
      out.push_back(p2);
      continue;
    }
    const float invSteps = 1.0f / static_cast<float>(steps);
    for (int s = 1; s <= steps; ++s) out.push_back(catmullRom(p0, p1, p2, p3, static_cast<float>(s) * invSteps));
  }
}

void smoothPolyline(DrawList& dl, const Vec2* pts, uint32_t n, Rgba col, float thickness, float feather) {
  if (n < 2 || thickness <= 0.0f) return;
  const Vec2 uv = dl.whiteUv();
  const Rgba clear = clearOf(col);
  const float hw = std::max(0.05f, thickness * 0.5f);
  const float fw = std::max(0.05f, feather);
  const float ow = hw + fw;

  const uint32_t segs = n - 1;
  const DrawList::PrimSpan sp = dl.primReserveSpan(segs * 18, segs * 8);
  Vertex* v = sp.v;
  uint32_t* ix = sp.i;
  uint32_t base = sp.base;

  // Per-vertex normals, averaged across the join (the chord of the two neighbours) so adjacent segments share
  // an edge direction and no seam shows through the fringe.
  Vec2 na = unitNormal(pts[0], pts[1]);
  for (uint32_t i = 0; i < segs; ++i) {
    const Vec2 a = pts[i], b = pts[i + 1];
    const Vec2 nb = unitNormal(a, pts[i + 2 < n ? i + 2 : n - 1]);
    // Miter length: extending the offset by 1/cos(theta/2) keeps the ribbon a constant width around a bend.
    // Clamped, because at a waveform's sharp peak the exact miter shoots off to infinity.
    float ea = 1.0f, eb = 1.0f;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float l = std::sqrt(dx * dx + dy * dy);
    if (l >= 1e-6f) {
      const float inv = 1.0f / l;
      const float sx = -dy * inv, sy = dx * inv;
      const float ca = std::fabs(na.x * sx + na.y * sy), cb = std::fabs(nb.x * sx + nb.y * sy);
      ea = std::clamp(ca > 1e-3f ? 1.0f / ca : 1.0f, 1.0f, kMiterClamp);
      eb = std::clamp(cb > 1e-3f ? 1.0f / cb : 1.0f, 1.0f, kMiterClamp);
    }
    const float iax = na.x * hw * ea, iay = na.y * hw * ea;
    const float ibx = nb.x * hw * eb, iby = nb.y * hw * eb;
    const float oax = na.x * ow * ea, oay = na.y * ow * ea;
    const float obx = nb.x * ow * eb, oby = nb.y * ow * eb;
    v[0] = {{a.x + iax, a.y + iay}, uv, col};
    v[1] = {{b.x + ibx, b.y + iby}, uv, col};
    v[2] = {{b.x - ibx, b.y - iby}, uv, col};
    v[3] = {{a.x - iax, a.y - iay}, uv, col};
    v[4] = {{a.x + oax, a.y + oay}, uv, clear};
    v[5] = {{b.x + obx, b.y + oby}, uv, clear};
    v[6] = {{b.x - obx, b.y - oby}, uv, clear};
    v[7] = {{a.x - oax, a.y - oay}, uv, clear};
    writeRibbonIdx(ix, base);
    v += 8;
    ix += 18;
    base += 8;
    na = nb;
  }
}

void smoothPolylineCols(DrawList& dl, const Vec2* pts, const Rgba* cols, uint32_t n, float thickness, float feather) {
  if (n < 2 || thickness <= 0.0f) return;
  const Vec2 uv = dl.whiteUv();
  const float hw = std::max(0.05f, thickness * 0.5f);
  const float fw = std::max(0.05f, feather);

  // ONE offset per vertex, shared by both segments that meet there: the miter direction (mean of the two
  // segment normals) scaled by 1/cos of the half-angle, clamped at 2×. Past a 2× miter (a turn sharper than
  // 120°) the miter is replaced by a BEVEL: each segment ends square on its own normal and one triangle (plus
  // its feather) fills the outside of the corner.
  static thread_local std::vector<Vec2> segN, off;
  static thread_local std::vector<int8_t> bevel;  // 0, or the outer side ±1
  segN.resize(n - 1);
  off.resize(n);
  bevel.assign(n, 0);
  for (uint32_t i = 0; i + 1 < n; ++i) segN[i] = unitNormal(pts[i], pts[i + 1]);
  uint32_t nBevel = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const Vec2 n0 = segN[i > 0 ? i - 1 : 0];
    const Vec2 n1 = segN[i < n - 1 ? i : n - 2];
    float mx = n0.x + n1.x, my = n0.y + n1.y;
    const float ml = std::sqrt(mx * mx + my * my);
    const float cosHalf = ml > 1e-4f ? (mx * n1.x + my * n1.y) / ml : 0.0f;
    if (i > 0 && i < n - 1 && cosHalf < 0.5f) {
      // Segment directions are the normals rotated back, d = (n.y, −n.x); a positive cross d0 × d1 turns
      // toward +n, so the gap opens on −n.
      const float cross = n0.y * (-n1.x) - (-n0.x) * n1.y;
      bevel[i] = cross > 0.0f ? -1 : 1;
      off[i] = n1;
      ++nBevel;
      continue;
    }
    const float inv = 1.0f / std::max(ml, 1e-4f);
    mx *= inv;
    my *= inv;
    const float ext = std::clamp(cosHalf > 1e-3f ? 1.0f / cosHalf : 2.0f, 1.0f, 2.0f);
    off[i] = {mx * ext, my * ext};
  }

  const uint32_t segs = n - 1;
  // Per segment 8 vertices / 18 indices; per bevel 5 vertices / 9 indices.
  const DrawList::PrimSpan sp = dl.primReserveSpan(segs * 18 + nBevel * 9, segs * 8 + nBevel * 5);
  Vertex* v = sp.v;
  uint32_t* ix = sp.i;
  uint32_t base = sp.base;
  for (uint32_t i = 0; i < segs; ++i) {
    const Vec2 a = pts[i], b = pts[i + 1];
    // A bevelled end sits square on this segment's own normal.
    const Vec2 na = bevel[i] ? segN[i] : off[i];
    const Vec2 nb = bevel[i + 1] ? segN[i] : off[i + 1];
    const Rgba ca = cols[i], cb = cols[i + 1];
    const Vec2 ia{na.x * hw, na.y * hw}, ib{nb.x * hw, nb.y * hw};
    const Vec2 oa{na.x * (hw + fw), na.y * (hw + fw)}, ob{nb.x * (hw + fw), nb.y * (hw + fw)};
    v[0] = {{a.x + ia.x, a.y + ia.y}, uv, ca};
    v[1] = {{b.x + ib.x, b.y + ib.y}, uv, cb};
    v[2] = {{b.x - ib.x, b.y - ib.y}, uv, cb};
    v[3] = {{a.x - ia.x, a.y - ia.y}, uv, ca};
    v[4] = {{a.x + oa.x, a.y + oa.y}, uv, clearOf(ca)};
    v[5] = {{b.x + ob.x, b.y + ob.y}, uv, clearOf(cb)};
    v[6] = {{b.x - ob.x, b.y - ob.y}, uv, clearOf(cb)};
    v[7] = {{a.x - oa.x, a.y - oa.y}, uv, clearOf(ca)};
    writeRibbonIdx(ix, base);
    v += 8;
    ix += 18;
    base += 8;
  }
  // The bevels: on the outside of each sharp corner, a core triangle from the vertex to the two square
  // segment ends, and a feather strip beyond it.
  for (uint32_t i = 1; i + 1 < n && nBevel > 0; ++i) {
    const int side = bevel[i];
    if (!side) continue;
    const Vec2 p = pts[i];
    const Vec2 n0 = segN[i - 1], n1 = segN[i];
    const float s = static_cast<float>(side);
    const Rgba c = cols[i], clear = clearOf(cols[i]);
    v[0] = {p, uv, c};
    v[1] = {{p.x + s * n0.x * hw, p.y + s * n0.y * hw}, uv, c};
    v[2] = {{p.x + s * n1.x * hw, p.y + s * n1.y * hw}, uv, c};
    v[3] = {{p.x + s * n0.x * (hw + fw), p.y + s * n0.y * (hw + fw)}, uv, clear};
    v[4] = {{p.x + s * n1.x * (hw + fw), p.y + s * n1.y * (hw + fw)}, uv, clear};
    constexpr uint32_t kIdx[9] = {0, 1, 2, 1, 3, 4, 1, 4, 2};
    for (uint32_t k = 0; k < 9; ++k) ix[k] = base + kIdx[k];
    v += 5;
    ix += 9;
    base += 5;
  }
}

void smoothBand(DrawList& dl, const Vec2* up, const Vec2* dn, uint32_t n, Rgba col, float feather) {
  if (n < 2) return;
  const Vec2 uv = dl.whiteUv();
  const Rgba clear = clearOf(col);
  const float f = std::max(0.05f, feather);
  const uint32_t segs = n - 1;
  // Body, plus a feather strip above and below. Column quads rather than a stroked outline: the two edges
  // meet and cross wherever the signal goes quiet, and an outline through that self-intersects.
  const DrawList::PrimSpan sp = dl.primReserveSpan(segs * 18, segs * 8);
  Vertex* v = sp.v;
  uint32_t* ix = sp.i;
  uint32_t base = sp.base;
  for (uint32_t i = 0; i < segs; ++i) {
    const float x0 = up[i].x, x1 = up[i + 1].x;
    const float t0 = up[i].y, t1 = up[i + 1].y;
    const float b0 = dn[i].y, b1 = dn[i + 1].y;
    v[0] = {{x0, t0}, uv, col};
    v[1] = {{x1, t1}, uv, col};
    v[2] = {{x1, b1}, uv, col};
    v[3] = {{x0, b0}, uv, col};
    v[4] = {{x0, t0 - f}, uv, clear};
    v[5] = {{x1, t1 - f}, uv, clear};
    v[6] = {{x1, b1 + f}, uv, clear};
    v[7] = {{x0, b0 + f}, uv, clear};
    writeRibbonIdx(ix, base);
    v += 8;
    ix += 18;
    base += 8;
  }
}

void smoothCurve(DrawList& dl, std::span<const Vec2> pts, Rgba col, float thickness, float maxSegPx, float feather) {
  if (pts.size() < 2) return;
  static thread_local std::vector<Vec2> sp;
  splinePath(pts, sp, maxSegPx);
  smoothPolyline(dl, sp.data(), static_cast<uint32_t>(sp.size()), col, thickness, feather);
}

void gradientPolyline(DrawList& dl, const Vec2* pts, const Rgba* cols, uint32_t n, float thickness) {
  if (n < 2 || thickness <= 0.0f) return;
  const uint32_t segs = n - 1;
  const DrawList::PrimSpan sp = dl.primReserveSpan(segs * 6, segs * 4);
  const Vec2 uv = dl.whiteUv();
  const float hw = thickness * 0.5f;
  Vertex* v = sp.v;
  uint32_t* ix = sp.i;
  uint32_t base = sp.base;
  for (uint32_t i = 0; i < segs; ++i) {
    const Vec2 a = pts[i], b = pts[i + 1];
    const Vec2 nn = unitNormal(a, b);
    const float nx = nn.x * hw, ny = nn.y * hw;
    v[0] = {{a.x + nx, a.y + ny}, uv, cols[i]};
    v[1] = {{b.x + nx, b.y + ny}, uv, cols[i + 1]};
    v[2] = {{b.x - nx, b.y - ny}, uv, cols[i + 1]};
    v[3] = {{a.x - nx, a.y - ny}, uv, cols[i]};
    writeQuadIdx(ix, base, 0, 1, 2, 3);
    v += 4;
    ix += 6;
    base += 4;
  }
}

void gradientBand(DrawList& dl, const Vec2* top, const Rgba* topCol, float yBottom, float bottomScale, uint32_t n) {
  if (n < 2) return;
  const uint32_t segs = n - 1;
  const DrawList::PrimSpan sp = dl.primReserveSpan(segs * 6, segs * 4);
  const Vec2 uv = dl.whiteUv();
  // withAlpha in spectrum.cpp SCALES the alpha the colour already has (= mulAlpha): the per-column floor
  // fade is in that alpha, and the bottom edge stays a fraction of it.
  const float bs = std::clamp(bottomScale, 0.0f, 1.0f);
  Vertex* v = sp.v;
  uint32_t* ix = sp.i;
  uint32_t base = sp.base;
  Rgba bottomPrev = mulAlpha(topCol[0], bs);
  for (uint32_t i = 0; i < segs; ++i) {
    const Rgba bottomNext = mulAlpha(topCol[i + 1], bs);
    v[0] = {top[i], uv, topCol[i]};
    v[1] = {top[i + 1], uv, topCol[i + 1]};
    v[2] = {{top[i + 1].x, yBottom}, uv, bottomNext};
    v[3] = {{top[i].x, yBottom}, uv, bottomPrev};
    writeQuadIdx(ix, base, 0, 1, 2, 3);
    bottomPrev = bottomNext;
    v += 4;
    ix += 6;
    base += 4;
  }
}

void splineSmooth(const float* src, float* dst, uint32_t n, int knotStep) {
  const int ni = static_cast<int>(n);
  if (knotStep < 2 || ni < 2 * knotStep) {
    std::copy(src, src + n, dst);
    return;
  }
  // Knots sit at uniform parameter positions k·knotStep; the last one or two land past the final column and
  // re-sample it, which keeps the parameterisation uniform without a special-cased short segment.
  static thread_local std::vector<float> knots;
  const int nk = (ni - 1) / knotStep + 2;
  knots.resize(static_cast<size_t>(nk));
  for (int k = 0; k < nk; ++k) {
    const int c = std::min(k * knotStep, ni - 1);
    const int a = std::max(0, c - knotStep / 2);
    const int b = std::min(ni - 1, c + knotStep / 2);
    float m = src[a];
    for (int i = a + 1; i <= b; ++i) m = std::max(m, src[i]);
    knots[static_cast<size_t>(k)] = m;
  }
  const float invStep = 1.0f / static_cast<float>(knotStep);
  for (int s = 0; s < ni; ++s) {
    const int i1 = std::min(s / knotStep, nk - 2);
    const float u = static_cast<float>(s - i1 * knotStep) * invStep;
    const float c0 = knots[static_cast<size_t>(std::max(0, i1 - 1))];
    const float c1 = knots[static_cast<size_t>(i1)];
    const float c2 = knots[static_cast<size_t>(i1 + 1)];
    const float c3 = knots[static_cast<size_t>(std::min(nk - 1, i1 + 2))];
    dst[s] = 0.5f * ((2.0f * c1) + (-c0 + c2) * u + (2.0f * c0 - 5.0f * c1 + 4.0f * c2 - c3) * u * u +
                     (-c0 + 3.0f * c1 - 3.0f * c2 + c3) * u * u * u);
  }
}

// --- layered looks -----------------------------------------------------------------------------------------

void scopeTrace(DrawList& dl, std::span<const Vec2> pts, Rgba color, float lineWidth, float glow, BlendMode glowBlend) {
  if (pts.size() < 2) return;
  const uint32_t n = static_cast<uint32_t>(pts.size());
  // Glow underlay: wide, very faint passes under the core make a hairline trace read as luminous rather than
  // as a thin scratch. Opacity falls off faster than linearly with width so the outer pass is a haze.
  if (glow > 0.001f) {
    const BlendMode prev = dl.blend();
    dl.setBlend(glowBlend);
    const float base = std::max(1.0f, lineWidth);
    for (int i = 2; i >= 1; --i) {  // PQ Meter's i = 3 pass has alpha 0
      const float t = static_cast<float>(i) / 3.0f;
      const float thick = base + t * 7.0f * glow;
      const float a = 0.10f * glow * (1.0f - t) * (1.0f - t * 0.5f);
      smoothPolyline(dl, pts.data(), n, smoothAlphaMul(color, a), thick, thick * 0.5f);
    }
    dl.setBlend(prev);
  }
  smoothPolyline(dl, pts.data(), n, color, std::max(0.6f, lineWidth));
}

void scopeEnvelope(DrawList& dl, const Vec2* up, const Vec2* dn, uint32_t n, Rgba color, float lineWidth, float glow,
                   BlendMode glowBlend) {
  if (n < 2) return;
  if (glow > 0.001f) {
    const BlendMode prev = dl.blend();
    dl.setBlend(glowBlend);
    const float base = std::max(1.0f, lineWidth);
    for (int i = 2; i >= 1; --i) {
      const float t = static_cast<float>(i) / 3.0f;
      const float thick = base + t * 7.0f * glow;
      const Rgba gc = smoothAlphaMul(color, 0.10f * glow * (1.0f - t) * (1.0f - t * 0.5f));
      smoothPolyline(dl, up, n, gc, thick, thick * 0.5f);
      smoothPolyline(dl, dn, n, gc, thick, thick * 0.5f);
    }
    dl.setBlend(prev);
  }
  // Fill first, then stroke the two edges: the stroke gives the boundary the same soft edge the curve has.
  smoothBand(dl, up, dn, n, color);
  const float ew = std::max(0.6f, lineWidth * 0.8f);
  smoothPolyline(dl, up, n, color, ew);
  smoothPolyline(dl, dn, n, color, ew);
}

void spectrumCurve(DrawList& dl, const Vec2* poly, const Rgba* cols, const float* levelT, uint32_t n, float yBottom,
                   const SpectrumCurveStyle& st) {
  if (n < 2) return;
  static thread_local std::vector<Rgba> tmp;
  tmp.resize(n);
  Rgba* t = tmp.data();

  // 1. Area under the curve: a vertical gradient reads as depth and keeps the grid visible near the floor.
  if (st.fillAlpha > 0.001f) {
    if (st.gradientFill) {
      const float fa = st.fillAlpha * 0.75f;
      for (uint32_t s = 0; s < n; ++s) t[s] = mulAlpha(cols[s], fa);
    } else {
      // Flat: PQ Meter fills with the fixed curve colour (0.43, 0.85, 1.0) at fillAlpha.
      const Rgba flat = setAlpha(rgba(110, 217, 255), st.fillAlpha);
      for (uint32_t s = 0; s < n; ++s) t[s] = flat;
    }
    gradientBand(dl, poly, t, yBottom, st.gradientFill ? 0.0f : 1.0f, n);
  }

  // 2. Bloom: three stacked passes of decreasing width and rising alpha approximate a blur; each vertex's
  //    alpha is scaled by its level cubed so only the loud parts glow.
  if (st.glow && st.glowAmount > 0.01f) {
    struct Layer {
      float mul, alpha;
    };
    constexpr Layer kLayers[3] = {{7.0f, 0.070f}, {4.0f, 0.110f}, {2.2f, 0.170f}};
    const BlendMode prev = dl.blend();
    dl.setBlend(st.glowBlend);
    for (const Layer& L : kLayers) {
      for (uint32_t s = 0; s < n; ++s) {
        const float k = levelT[s] * levelT[s] * levelT[s];
        t[s] = mulAlpha(cols[s], L.alpha * st.glowAmount * (0.15f + 0.85f * k));
      }
      gradientPolyline(dl, poly, t, n, std::max(1.0f, st.lineWidth * L.mul));
    }
    dl.setBlend(prev);
  }

  // 3. The curve: a soft body at the requested width, then a thin brighter core on top (what the eye tracks).
  gradientPolyline(dl, poly, cols, n, std::max(1.0f, st.lineWidth * 1.9f));
  for (uint32_t s = 0; s < n; ++s) t[s] = brighten(cols[s], 0.30f + 0.45f * levelT[s]);
  gradientPolyline(dl, poly, t, n, std::max(0.8f, st.lineWidth * 0.85f));
}

}  // namespace pq::ui
