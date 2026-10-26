// CPU coverage rasteriser (T4.2): see pq/ui/raster.h.
//
// Like the GPU, vertex positions are snapped to a fixed-point grid of 1/256 px (Vulkan's usual 8 sub-pixel bits) and
// the edge functions are evaluated exactly in integers: two triangles sharing an edge whose vertices differ only in
// float rounding (a ribbon's adjacent quads) then share it exactly, and the top-left rule gives each pixel centre on it
// to exactly one of them — no holes, no double coverage.

#include "pq/ui/raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pq::ui {

namespace {

constexpr int64_t kSub = 256;  // sub-pixel steps per pixel

// Floor / ceiling of a / b for any signs (b ≠ 0).
int64_t floorDiv(int64_t a, int64_t b) noexcept {
  const int64_t q = a / b, r = a % b;
  return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
int64_t ceilDiv(int64_t a, int64_t b) noexcept { return -floorDiv(-a, b); }

int64_t fixedOf(float v) noexcept { return static_cast<int64_t>(std::llround(static_cast<double>(v) * kSub)); }

// Top-left rule for an edge a → b of a triangle wound so that interior points have positive edge values (y down): a top
// edge is horizontal with the interior below it, a left edge goes up.
bool topLeft(int64_t ax, int64_t ay, int64_t bx, int64_t by) noexcept {
  const int64_t dx = bx - ax, dy = by - ay;
  return (dy == 0 && dx > 0) || dy < 0;
}

}  // namespace

void rasterizeCoverage(const DrawList& dl, uint8_t* dst, uint32_t width, uint32_t height, uint32_t rowPitch,
                       Vec2 origin) noexcept {
  const auto vtx = dl.vertices();
  const auto idx = dl.indices();
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    const Vertex* v[3] = {&vtx[idx[t]], &vtx[idx[t + 1]], &vtx[idx[t + 2]]};
    int64_t px[3], py[3];
    double a[3];
    bool finite = true;
    for (int k = 0; k < 3; ++k) {
      const float x = v[k]->pos.x - origin.x, y = v[k]->pos.y - origin.y;
      finite = finite && std::isfinite(x) && std::isfinite(y) && std::fabs(x) < 1e6f && std::fabs(y) < 1e6f;
      px[k] = fixedOf(x);
      py[k] = fixedOf(y);
      a[k] = static_cast<double>(alphaOf(v[k]->col)) * (1.0 / 255.0);
    }
    if (!finite || (a[0] <= 0.0 && a[1] <= 0.0 && a[2] <= 0.0)) continue;
    int64_t area = (px[1] - px[0]) * (py[2] - py[0]) - (py[1] - py[0]) * (px[2] - px[0]);
    if (area == 0) continue;
    if (area < 0) {
      std::swap(px[1], px[2]);
      std::swap(py[1], py[2]);
      std::swap(a[1], a[2]);
      area = -area;
    }
    const int64_t minX = std::min({px[0], px[1], px[2]}), maxX = std::max({px[0], px[1], px[2]});
    const int64_t minY = std::min({py[0], py[1], py[2]}), maxY = std::max({py[0], py[1], py[2]});
    // Pixels whose centre (x·256 + 128) can lie inside.
    const int64_t x0 = std::max<int64_t>(0, (minX - kSub / 2) / kSub - 1);
    const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(width) - 1, (maxX - kSub / 2) / kSub + 1);
    const int64_t y0 = std::max<int64_t>(0, (minY - kSub / 2) / kSub - 1);
    const int64_t y1 = std::min<int64_t>(static_cast<int64_t>(height) - 1, (maxY - kSub / 2) / kSub + 1);
    if (x0 > x1 || y0 > y1) continue;
    // Edge e is opposite vertex e: E_e(q) = cross(p[e+2] − p[e+1], q − p[e+1]) = A·qx + B·qy + C (positive inside).
    int64_t ea[3], eb[3], ec[3];
    bool tl[3];
    for (int e = 0; e < 3; ++e) {
      const int s = (e + 1) % 3, d = (e + 2) % 3;
      ea[e] = -(py[d] - py[s]);
      eb[e] = px[d] - px[s];
      ec[e] = -(ea[e] * px[s] + eb[e] * py[s]);
      tl[e] = topLeft(px[s], py[s], px[d], py[d]);
    }
    const double inv = 1.0 / static_cast<double>(area);
    const bool opaque = a[0] >= 1.0 && a[1] >= 1.0 && a[2] >= 1.0;
    for (int64_t y = y0; y <= y1; ++y) {
      const int64_t qy = y * kSub + kSub / 2;
      uint8_t* row = dst + static_cast<size_t>(y) * rowPitch;
      // The row's span: each edge function is linear in x, so "inside" is a half-line per edge (exact integer bounds);
      // only pixels in all three are visited (a long thin triangle costs its pixels, not its bounding box).
      int64_t lo = x0, hi = x1;
      for (int e = 0; e < 3 && lo <= hi; ++e) {
        const int64_t A = ea[e], K = eb[e] * qy + ec[e];
        // v(x) = A·(256x + 128) + K; inside: v > 0, or v ≥ 0 on a top-left edge.
        if (A == 0) {
          if (!(K > 0 || (K == 0 && tl[e]))) lo = hi + 1;
          continue;
        }
        const int64_t num = -K - A * (kSub / 2), den = A * kSub;
        if (A > 0) {
          lo = std::max(lo, tl[e] ? ceilDiv(num, den) : floorDiv(num, den) + 1);
        } else {
          hi = std::min(hi, tl[e] ? floorDiv(num, den) : ceilDiv(num, den) - 1);
        }
      }
      if (opaque) {  // every vertex fully opaque: full coverage over the span (crisp quads, cores of fills)
        if (lo <= hi) std::memset(row + lo, 255, static_cast<size_t>(hi - lo + 1));
        continue;
      }
      for (int64_t x = lo; x <= hi; ++x) {
        const int64_t qx = x * kSub + kSub / 2;
        int64_t w[3];
        for (int e = 0; e < 3; ++e) w[e] = ea[e] * qx + eb[e] * qy + ec[e];
        const double alpha = std::clamp((static_cast<double>(w[0]) * a[0] + static_cast<double>(w[1]) * a[1] +
                                         static_cast<double>(w[2]) * a[2]) * inv, 0.0, 1.0);
        if (alpha <= 0.0) continue;
        const double c = static_cast<double>(row[x]) * (1.0 / 255.0);
        const double out = alpha + c * (1.0 - alpha);
        row[x] = static_cast<uint8_t>(std::lround(std::min(out, 1.0) * 255.0));
      }
    }
  }
}

}  // namespace pq::ui
