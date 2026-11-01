// Vector faces: the world → device painter and static-layer tiles (TMM.4). See pq/ui/face_painter.h.

#include "pq/ui/face_painter.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "pq/ui/lines.h"
#include "pq/ui/raster.h"
#include "pq/ui/text.h"

namespace pq::ui {

namespace {

std::vector<Vec2>& scratchPoints() {
  thread_local std::vector<Vec2> pts;
  return pts;
}

// Text sizes a face may use (device px): every size to 16, then a few steps, so zooming creates few fonts.
uint32_t quantisePx(float px) noexcept {
  static constexpr uint32_t kSteps[] = {18, 20, 22, 24, 28, 32, 40, 48, 56, 64, 80, 96, 128, 160, 192, 256};
  const auto r = static_cast<uint32_t>(std::lround(px));
  if (r <= 16) return r;
  uint32_t best = kSteps[0];
  for (const uint32_t s : kSteps)
    if (static_cast<float>(s) <= px * 1.06f) best = s;
  return best;
}

}  // namespace

void FacePainter::disc(Vec2 c, float r, Rgba col) {
  ++count_;
  dl_.addCircleFilled(t_.map(c), t_.len(r), this->c(col));
}

void FacePainter::ring(Vec2 c, float r, float thickness, Rgba col) {
  ++count_;
  dl_.addCircleStroke(t_.map(c), t_.len(r), this->c(col), t_.len(thickness));
}

void FacePainter::arc(Vec2 c, float r, float a0, float a1, float thickness, Rgba col, LineCap cap) {
  ++count_;
  if (a0 == a1) return;
  dl_.addArcStroke(t_.map(c), t_.len(r), a0, a1, this->c(col), t_.len(thickness), cap);
}

void FacePainter::line(Vec2 a, Vec2 b, float thickness, Rgba col0) {
  ++count_;
  const Rgba col = c(col0);
  const Vec2 pts[2] = {t_.map(a), t_.map(b)};
  const float w = t_.len(thickness);
  // Below a pixel the ribbon keeps its coverage through alpha (as the draw list's thin strokes do).
  if (w < 1.0f) smoothPolyline(dl_, pts, 2, mulAlpha(col, std::max(w, 0.0f)), 1.0f);
  else smoothPolyline(dl_, pts, 2, col, w);
}

void FacePainter::polyline(std::span<const Vec2> pts, float thickness, Rgba col0) {
  ++count_;
  const Rgba col = c(col0);
  if (pts.size() < 2) return;
  std::vector<Vec2>& d = scratchPoints();
  d.resize(pts.size());
  for (size_t i = 0; i < pts.size(); ++i) d[i] = t_.map(pts[i]);
  const float w = t_.len(thickness);
  if (w < 1.0f) smoothPolyline(dl_, d.data(), static_cast<uint32_t>(d.size()), mulAlpha(col, std::max(w, 0.0f)), 1.0f);
  else smoothPolyline(dl_, d.data(), static_cast<uint32_t>(d.size()), col, w);
}

void FacePainter::dashed(std::span<const Vec2> pts, float thickness, float on, float off, Rgba col) {
  ++count_;
  if (pts.size() < 2 || on <= 0.0f) return;
  const float period = on + std::max(off, 0.0f);
  const float w = t_.len(thickness);
  const Rgba dc = w < 1.0f ? mulAlpha(c(col), std::max(w, 0.0f)) : c(col);
  const float tw = std::max(w, 1.0f);
  float phase = 0.0f;  // world units into the pattern
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    const Vec2 a = pts[i], b = pts[i + 1];
    const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) continue;
    float pos = 0.0f;
    while (pos < len) {
      float in = std::fmod(phase, period);
      if (period - in < 1e-4f * period) in = 0.0f;  // TMM.7: at a period's end in float: the next period (no zero runs)
      if (period > on && in < on && on - in < 1e-4f * period) in = on;  // ... at a dash's end: the gap
      const bool drawing = in < on;
      const float run = std::min(len - pos, drawing ? on - in : period - in);
      if (pos + run <= pos) break;  // below float resolution: the rest is shorter than a rounding step
      if (drawing && run > 0.0f) {
        const float u0 = pos / len, u1 = (pos + run) / len;
        const Vec2 seg[2] = {t_.map({a.x + dx * u0, a.y + dy * u0}), t_.map({a.x + dx * u1, a.y + dy * u1})};
        smoothPolyline(dl_, seg, 2, dc, tw);
      }
      pos += run;
      phase += run;
    }
  }
}

void FacePainter::fillUnder(std::span<const Vec2> curve, float baseY, Rgba top, Rgba bottom) {
  ++count_;
  if (curve.size() < 2) return;
  const auto n = static_cast<uint32_t>(curve.size());
  DrawList::PrimSpan s = dl_.primReserveSpan(6 * (n - 1), 2 * n);
  const Vec2 uv = dl_.whiteUv();
  const float by = t_.origin.y + baseY * t_.scale;
  for (uint32_t i = 0; i < n; ++i) {
    const Vec2 p = t_.map(curve[i]);
    s.v[2 * i] = {p, uv, c(top)};
    s.v[2 * i + 1] = {{p.x, by}, uv, c(bottom)};
  }
  for (uint32_t i = 0; i + 1 < n; ++i) {
    const uint32_t b = s.base + 2 * i;
    uint32_t* idx = s.i + 6 * i;
    idx[0] = b, idx[1] = b + 1, idx[2] = b + 2;
    idx[3] = b + 1, idx[4] = b + 3, idx[5] = b + 2;
  }
}

void FacePainter::convex(std::span<const Vec2> pts, Rgba col) {
  ++count_;
  if (pts.size() < 3) return;
  std::vector<Vec2>& d = scratchPoints();
  d.resize(pts.size());
  for (size_t i = 0; i < pts.size(); ++i) d[i] = t_.map(pts[i]);
  dl_.addConvexPolyFilled(d, c(col));
}

void FacePainter::triangle(Vec2 a, Vec2 b, Vec2 cc, Rgba col) {
  ++count_;
  dl_.addTriangleFilled(t_.map(a), t_.map(b), t_.map(cc), this->c(col));
}

void FacePainter::roundedRect(const Rect& r, float radius, Rgba col) {
  ++count_;
  const Vec2 a = t_.map({r.x0, r.y0}), b = t_.map({r.x1, r.y1});
  dl_.addRoundedRectFilled({a.x, a.y, b.x, b.y}, t_.len(radius), c(col));
}

void FacePainter::glow(Vec2 c, float r, Rgba col) {
  ++count_;
  dl_.addBlob(t_.map(c), t_.len(r), this->c(col));
}

uint32_t FacePainter::textPixels(float px) const noexcept {
  const float d = px * t_.scale;
  if (d < 3.5f) return 0;
  return std::clamp<uint32_t>(quantisePx(d), 4, 256);
}

void FacePainter::text(Vec2 centre, std::string_view utf8, float px, Rgba col0) {
  ++count_;
  const Rgba col = c(col0);
  if (utf8.empty()) return;
  const uint32_t dpx = textPixels(px);
  const Vec2 at = t_.map(centre);
  if (dpx != 0 && text_ != nullptr) {
    if (auto f = text_->font(face_, dpx)) {
      const TextRun run = text_->shape(*f, utf8);
      const float cap = std::round(0.727f * static_cast<float>(dpx));
      TextSystem::drawRun(dl_, run, {std::round(at.x - static_cast<float>(run.advance) * 0.5f), std::round(at.y + cap * 0.5f)}, col);
      return;
    }
  }
  // Too small to read (or no text system): a bar of the run's extent (≈ 0.55 em per character, 0.35 em tall).
  const float w = t_.len(px) * 0.55f * static_cast<float>(utf8.size()), h = std::max(t_.len(px) * 0.35f, 0.5f);
  dl_.addRoundedRectFilled({at.x - w * 0.5f, at.y - h * 0.5f, at.x + w * 0.5f, at.y + h * 0.5f}, 0.0f,
                           mulAlpha(col, std::min(1.0f, 0.6f * h + 0.2f)));
}

// ---- static layers ----------------------------------------------------------------------------------------------

bool recordLayer(TileAtlas& atlas, DrawList& scratch, float w, float h, float scale,
                 const std::function<void(FacePainter&)>& paint, LayerTile& out) {
  out = LayerTile{};
  out.scale = scale;
  out.margin = std::ceil(kSmoothFeather + 2.0f);
  const auto tw = static_cast<uint32_t>(std::ceil(w * scale + 2.0f * out.margin));
  const auto th = static_cast<uint32_t>(std::ceil(h * scale + 2.0f * out.margin));
  if (tw == 0 || th == 0 || tw > atlas.maxTileSize() || th > atlas.maxTileSize()) {
    out.failed = true;
    return false;
  }
  scratch.reset(tw, th);
  FacePainter p(scratch, {{out.margin, out.margin}, scale});
  paint(p);
  if (scratch.empty()) {
    out.empty = true;
    return true;
  }
  if (!atlas.allocate(tw, th, out.tile)) {
    out.failed = true;
    return false;
  }
  uint8_t* dst = atlas.write(out.tile);
  if (dst == nullptr) {
    out.failed = true;
    return false;
  }
  const uint32_t pitch = TileAtlas::rowPitch(out.tile);
  rasterizeCoverage(scratch, dst + pitch + 1, tw, th, pitch, {0.0f, 0.0f});
  return true;
}

bool layerReady(const TileAtlas& atlas, const LayerTile& t) noexcept {
  return t.scale > 0.0f && !t.failed && (t.empty || atlas.valid(t.tile));
}

void drawLayer(DrawList& dl, TileAtlas& atlas, const LayerTile& t, Vec2 origin, float scale, Rgba col) {
  if (t.empty) return;
  const float k = scale / t.scale, inv = atlas.uvScale();
  atlas.touch(t.tile.page);
  const float x = origin.x - t.margin * k, y = origin.y - t.margin * k;
  dl.addImage(TileAtlas::texture(t.tile.page), {x, y, x + static_cast<float>(t.tile.w) * k, y + static_cast<float>(t.tile.h) * k},
              {static_cast<float>(t.tile.x) * inv, static_cast<float>(t.tile.y) * inv},
              {static_cast<float>(t.tile.x + t.tile.w) * inv, static_cast<float>(t.tile.y + t.tile.h) * inv}, col);
}

}  // namespace pq::ui
