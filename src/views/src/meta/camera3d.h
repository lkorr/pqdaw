#pragma once

// The 3D faces' camera (VZ.7; private to views), ported from PQ Meter (modules/waterfall.cpp and stereo_spectrogram.cpp:
// viewRotation, the autofit / free-camera scale, the perspective divide, the orbit / pan / roll drags, the release snap
// and the eased snap). Box coordinates are three axes in [−1, 1] whose meaning is the face's (the Waterfall: frequency,
// height, time; the Stereo Spectrogram: time, frequency, pan); R = Rz(roll) · Rx(pitch) · Ry(yaw) acts on them in that
// order, so at yaw = pitch = roll = 0 the first axis is screen x (right) and the second screen y (up), the third depth.
//
//   fit       autofit divides each screen row by its L1 norm (the box always fits; it re-stretches as it turns); the
//             free camera holds one scale (0.62, both faces alike) so the box turns rigidly
//   persp     0 … 1: a depth divide 1 / max(0.35, 1 − 0.28 · persp · z) (near faces grow); 0 is orthographic
//   drags     Left: orbit (yaw / pitch, 0.4 °/px), Shift: pan, Ctrl: roll; the wheel zooms (0.3 … 4); releasing near
//             a canonical view (within 5° flat, or pitch past 84°) eases into it (50 ms); a double-click snaps to the
//             nearest named view (or the 3D view from one); the face's chips snap to each named view.

#include <algorithm>
#include <cmath>

#include "pq/ui/draw_list.h"

namespace pq::views::cam3d {

struct Pose {
  float yaw = 45.0f, pitch = 15.0f, roll = 0.0f, zoom = 1.0f, panX = 0.0f, panY = 0.0f;
  bool operator==(const Pose&) const = default;
};

// The four named views of a face, in chip order (the last is the 3D view).
struct NamedView {
  const char* chip;
  float yaw, pitch;
};

inline void rotation(float yawDeg, float pitchDeg, float rollDeg, float R[9]) noexcept {
  constexpr float k = 3.14159265358979323846f / 180.0f;
  const float cy = std::cos(yawDeg * k), sy = std::sin(yawDeg * k);
  const float cp = std::cos(pitchDeg * k), sp = std::sin(pitchDeg * k);
  const float cr = std::cos(rollDeg * k), sr = std::sin(rollDeg * k);
  R[0] = cr * cy - sr * sp * sy;
  R[1] = -sr * cp;
  R[2] = cr * sy + sr * sp * cy;
  R[3] = sr * cy + cr * sp * sy;
  R[4] = cr * cp;
  R[5] = sr * sy - cr * sp * cy;
  R[6] = -cp * sy;
  R[7] = sp;
  R[8] = cp * cy;
}

inline constexpr float kFreeCamScale = 0.62f;

// One frame's projection into a plot rect (any units: world or device px).
struct Projector {
  float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  float sx = 1.0f, sy = 1.0f, cx = 0.0f, cy = 0.0f, perspK = 0.0f;
  Projector() = default;
  Projector(const Pose& p, bool freeCam, float persp, const ui::Rect& plot) noexcept {
    rotation(p.yaw, p.pitch, p.roll, R);
    const float fitX = freeCam ? kFreeCamScale : 1.0f / std::max(1e-6f, std::fabs(R[0]) + std::fabs(R[1]) + std::fabs(R[2]));
    const float fitY = freeCam ? kFreeCamScale : 1.0f / std::max(1e-6f, std::fabs(R[3]) + std::fabs(R[4]) + std::fabs(R[5]));
    const float hw = 0.5f * (plot.x1 - plot.x0), hh = 0.5f * (plot.y1 - plot.y0);
    sx = fitX * p.zoom * hw;
    sy = fitY * p.zoom * hh;
    cx = 0.5f * (plot.x0 + plot.x1) + p.panX * hw;
    cy = 0.5f * (plot.y0 + plot.y1) - p.panY * hh;
    perspK = persp * 0.28f;
  }
  ui::Vec2 operator()(float a, float b, float c) const noexcept {
    float ps = 1.0f;
    if (perspK > 0.0f) ps = 1.0f / std::max(0.35f, 1.0f - perspK * (R[6] * a + R[7] * b + R[8] * c));
    return {cx + (R[0] * a + R[1] * b + R[2] * c) * ps * sx, cy - (R[3] * a + R[4] * b + R[5] * c) * ps * sy};
  }
  // + toward the viewer.
  float depth(float a, float b, float c) const noexcept { return R[6] * a + R[7] * b + R[8] * c; }
};

// An angle in degrees into [-180, 180) (yaw and roll turn all the way round).
inline float wrap180(float a) noexcept { return a - 360.0f * std::floor((a + 180.0f) / 360.0f); }

// The camera's motion: drags, the wheel, snaps (eased). Times in seconds.
class Orbit {
public:
  enum class Drag { None, Orbit, Pan, Roll };
  Pose pose;
  bool snapping() const noexcept { return snap_; }
  const Pose& target() const noexcept { return target_; }  // where a snap eases to
  bool dragging() const noexcept { return drag_ != Drag::None; }
  // A named view: the angles and the default framing (zoom 1, no pan).
  void snapTo(float yaw, float pitch, float roll = 0.0f) noexcept {
    snap_ = true;
    target_ = {yaw, pitch, roll, 1.0f, 0.0f, 0.0f};
  }
  // The release snap: only the angles (the zoom and pan the user set stay).
  void snapAngles(float yaw, float pitch, float roll = 0.0f) noexcept {
    snap_ = true;
    target_ = {yaw, pitch, roll, pose.zoom, pose.panX, pose.panY};
  }
  void stopSnap() noexcept { snap_ = false; }
  void begin(Drag d, ui::Vec2 at) noexcept {
    drag_ = d;
    last_ = at;
    snap_ = false;
  }
  // `w`, `h`: the plot's size in the pointer's units (pan moves one half-width per half-width dragged).
  void move(ui::Vec2 at, float w, float h) noexcept {
    const float dx = at.x - last_.x, dy = at.y - last_.y;
    last_ = at;
    switch (drag_) {
      case Drag::Orbit:
        pose.yaw = wrap180(pose.yaw + dx * 0.4f);  // round and round (pitch stops at the poles)
        pose.pitch = std::clamp(pose.pitch + dy * 0.4f, -90.0f, 90.0f);
        break;
      case Drag::Pan:
        pose.panX = std::clamp(pose.panX + dx / std::max(1.0f, 0.5f * w), -1.5f, 1.5f);
        pose.panY = std::clamp(pose.panY - dy / std::max(1.0f, 0.5f * h), -1.5f, 1.5f);
        break;
      case Drag::Roll: pose.roll = wrap180(pose.roll + dx * 0.4f); break;
      case Drag::None: break;
    }
  }
  // Released: an orbit or roll that ends near a canonical view eases into it (PQ's release snap).
  void end() noexcept {
    if (drag_ == Drag::Orbit || drag_ == Drag::Roll) {
      const bool flat = std::fabs(pose.pitch) < 6.0f && std::fabs(pose.roll) < 6.0f;
      if (flat && std::fabs(pose.yaw) < 5.0f) snapAngles(0.0f, 0.0f);
      else if (flat && std::fabs(pose.yaw - 90.0f) < 5.0f) snapAngles(90.0f, 0.0f);
      else if (std::fabs(pose.pitch) > 84.0f && std::fabs(pose.roll) < 6.0f) snapAngles(pose.yaw, pose.pitch < 0.0f ? -90.0f : 90.0f);
    }
    drag_ = Drag::None;
  }
  void wheel(float notches) noexcept {
    pose.zoom = std::clamp(pose.zoom * std::exp(notches * 0.12f), 0.3f, 4.0f);
    snap_ = false;
  }
  // Double-click: the nearest of the named views (yaw / pitch distance); from that view, the last one (3D).
  void snapNearest(const NamedView* views, int n) noexcept {
    int best = 0;
    float bestD = 1e30f;
    for (int i = 0; i < n; ++i) {
      const float d = std::fabs(wrap180(pose.yaw - views[i].yaw)) + std::fabs(pose.pitch - views[i].pitch);
      if (d < bestD) bestD = d, best = i;
    }
    if (bestD < 0.5f && std::fabs(pose.roll) < 0.5f) best = n - 1;
    snapTo(views[best].yaw, views[best].pitch);
  }
  // Advances an eased snap by dt; true while it moves.
  bool tick(float dt) noexcept {
    if (!snap_) return false;
    const float a = 1.0f - std::exp(-std::clamp(dt, 0.0f, 0.1f) / 0.05f);
    pose.yaw = wrap180(pose.yaw + wrap180(target_.yaw - pose.yaw) * a);  // the short way round
    pose.pitch += (target_.pitch - pose.pitch) * a;
    pose.roll = wrap180(pose.roll + wrap180(target_.roll - pose.roll) * a);
    pose.zoom += (target_.zoom - pose.zoom) * a;
    pose.panX += (target_.panX - pose.panX) * a;
    pose.panY += (target_.panY - pose.panY) * a;
    if (std::fabs(wrap180(pose.yaw - target_.yaw)) < 0.05f && std::fabs(pose.pitch - target_.pitch) < 0.05f &&
        std::fabs(wrap180(pose.roll - target_.roll)) < 0.05f && std::fabs(pose.zoom - target_.zoom) < 0.003f &&
        std::fabs(pose.panX - target_.panX) < 0.002f && std::fabs(pose.panY - target_.panY) < 0.002f) {
      pose = target_;
      snap_ = false;
    }
    return true;
  }

private:
  Drag drag_ = Drag::None;
  ui::Vec2 last_{};
  bool snap_ = false;
  Pose target_;
};

// The view a snap chip names is the one shown (or the one a snap is easing to): its angles within half a degree.
inline bool atView(const Orbit& o, const NamedView& v) noexcept {
  const Pose& p = o.snapping() ? o.target() : o.pose;
  return std::fabs(wrap180(p.yaw - v.yaw)) < 0.5f && std::fabs(p.pitch - v.pitch) < 0.5f && std::fabs(wrap180(p.roll)) < 0.5f;
}

}  // namespace pq::views::cam3d
