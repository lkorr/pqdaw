// Orbiting main ports (TX.orbit-ports): the outline geometry and the shared placement rule. See
// pq/views/circuit/orbit.h.

#include "pq/views/circuit/orbit.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pq::views::circuit {

using graph::Point2;

namespace {
constexpr float kTau = 2.0f * std::numbers::pi_v<float>;
constexpr float kStep = kTau / 180.0f;  // 2°
}  // namespace

float wrapAngle(float a) noexcept {
  if (!std::isfinite(a)) return 0.0f;
  a = std::fmod(a, kTau);
  return a < 0.0f ? a + kTau : a;
}

float angleDelta(float a, float b) noexcept {
  float d = std::fmod(b - a, kTau);
  if (d > std::numbers::pi_v<float>) d -= kTau;
  else if (d < -std::numbers::pi_v<float>) d += kTau;
  return d;
}

Point2 outlinePoint(const Outline& o, float angle, Point2* normal) noexcept {
  const float dx = std::cos(angle), dy = std::sin(angle);
  if (o.round) {
    if (normal != nullptr) *normal = {dx, dy};
    return {o.cx + o.r * dx, o.cy + o.r * dy};
  }
  const float hw = 0.5f * (o.x1 - o.x0), hh = 0.5f * (o.y1 - o.y0);
  const float cx = 0.5f * (o.x0 + o.x1), cy = 0.5f * (o.y0 + o.y1);
  const float cr = std::clamp(o.corner, 0.0f, std::min(hw, hh));
  constexpr float kInf = std::numeric_limits<float>::infinity();
  const float tx = std::fabs(dx) > 1e-7f ? hw / std::fabs(dx) : kInf;
  const float ty = std::fabs(dy) > 1e-7f ? hh / std::fabs(dy) : kInf;
  const float t = std::min(tx, ty);
  const float qx = t * dx, qy = t * dy;
  // On a straight part of the edge: the rect's point.
  if (std::fabs(qx) <= hw - cr + 1e-5f || std::fabs(qy) <= hh - cr + 1e-5f) {
    if (normal != nullptr) *normal = tx < ty ? Point2{dx < 0.0f ? -1.0f : 1.0f, 0.0f} : Point2{0.0f, dy < 0.0f ? -1.0f : 1.0f};
    return {cx + qx, cy + qy};
  }
  // In a corner: where the ray leaves the corner's circle (the far root).
  const float kx = (dx < 0.0f ? -1.0f : 1.0f) * (hw - cr), ky = (dy < 0.0f ? -1.0f : 1.0f) * (hh - cr);
  const float b = dx * kx + dy * ky;
  const float disc = std::max(0.0f, b * b - (kx * kx + ky * ky) + cr * cr);
  const float tc = b + std::sqrt(disc);
  const float px = tc * dx, py = tc * dy;
  if (normal != nullptr) {
    const float nx = px - kx, ny = py - ky, l = std::sqrt(nx * nx + ny * ny);
    *normal = l > 1e-6f ? Point2{nx / l, ny / l} : Point2{dx, dy};
  }
  return {cx + px, cy + py};
}

float outlineDistance(const Outline& o, Point2 p) noexcept {
  if (o.round) return std::hypot(p.x - o.cx, p.y - o.cy) - o.r;
  const float hw = 0.5f * (o.x1 - o.x0), hh = 0.5f * (o.y1 - o.y0);
  const float cr = std::clamp(o.corner, 0.0f, std::min(hw, hh));
  const float qx = std::fabs(p.x - 0.5f * (o.x0 + o.x1)) - (hw - cr), qy = std::fabs(p.y - 0.5f * (o.y0 + o.y1)) - (hh - cr);
  return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - cr;
}

Point2 orbitPoint(const Outline& o, float angle, float scale) noexcept {
  Point2 n;
  const Point2 p = outlinePoint(o, angle, &n);
  const float out = (kOrbitPortR + kOrbitGap) * scale;
  return {p.x + n.x * out, p.y + n.y * out};
}

float orbitAngle(const Outline& o, std::span<const Point2> targets, float fallback) noexcept {
  const Point2 c = o.centre();
  float sx = 0.0f, sy = 0.0f;
  uint32_t n = 0;
  for (const Point2& t : targets) {
    const float dx = t.x - c.x, dy = t.y - c.y, l = std::sqrt(dx * dx + dy * dy);
    if (!(l > 1e-3f)) continue;
    sx += dx / l, sy += dy / l;
    ++n;
  }
  if (n == 0 || std::sqrt(sx * sx + sy * sy) < 0.05f * static_cast<float>(n)) return fallback;
  return wrapAngle(std::atan2(sy, sx));
}

void solveOrbits(const Outline& o, float scale, const bool on[2], const bool pinned[2], const std::span<const Point2> targets[2],
                 std::span<const Point2> fixed, float angles[2]) noexcept {
  const float pr = kOrbitPortR * scale;
  const float toFixed = pr + (kOrbitPinR + kOrbitSpacing) * scale, toPort = 2.0f * pr + kOrbitSpacing * scale;
  Point2 placed[2];
  uint32_t nPlaced = 0;
  auto clearAt = [&](float a) {
    const Point2 p = orbitPoint(o, a, scale);
    for (const Point2& q : fixed)
      if (std::hypot(p.x - q.x, p.y - q.y) < toFixed) return false;
    for (uint32_t i = 0; i < nPlaced; ++i)
      if (std::hypot(p.x - placed[i].x, p.y - placed[i].y) < toPort) return false;
    return true;
  };
  for (int pass = 0; pass < 2; ++pass)
    for (int k = 0; k < 2; ++k) {
      if (!on[k] || pinned[k] != (pass == 0)) continue;
      float a = pinned[k] ? wrapAngle(angles[k]) : orbitAngle(o, targets[k], k == 0 ? kOrbitDefaultIn : kOrbitDefaultOut);
      if (!pinned[k] && !clearAt(a)) {
        // Step along the outline, both ways, nearest first, to the first clear place (bounded; none: where it was).
        for (uint32_t j = 1; j <= kOrbitSearchSteps; ++j) {
          const float d = kStep * static_cast<float>(j);
          if (clearAt(a + d)) {
            a = wrapAngle(a + d);
            break;
          }
          if (clearAt(a - d)) {
            a = wrapAngle(a - d);
            break;
          }
        }
      }
      angles[k] = a;
      placed[nPlaced++] = orbitPoint(o, a, scale);
    }
}

}  // namespace pq::views::circuit
