#pragma once

// Orbiting main ports (TX.orbit-ports, SPEC §15 2026-11-03 "orbiting ports, a round VCA, Delete on a cable"): a block's
// main input and main output (doc::mainPortsOf) are not fixed on its left / right edge; each sits just outside the
// block's OUTLINE (a circle for a round face, the rounded rect of a panel or plain block) at the angle that makes its
// cables shortest, and slides along the outline as blocks move. The rest of a block's pins keep their places.
//
// One rule, shared by the layout (pq/views/project_patch: where the settled ports are, which the scene's pins and the
// cables' rest shapes start from) and the canvas (pq/views/circuit/circuit_canvas.h: the same rule live while blocks are
// dragged, the slide animated):
//   direction   from the outline's centre toward each of the port's cable ends (the other end's node centre when that
//               end orbits too, else the other pin or control); several cables: the mean of the unit directions; none
//               (or directions that cancel): the default — left for an input, right for an output
//   place       the outline point in that direction (a ray from the centre), pushed out along the outline's normal by
//               the port's radius and a gap: just outside, never on the block
//   pinned      a port the user dragged along the outline stays at its stored angle (doc::PatchNudge's pins)
//   spacing     a port keeps kOrbitSpacing between its rim and every other pin of its block and the other main port: it
//               steps along the outline (both ways, nearest first) to the first clear place; pinned ports place first
// Angles are radians, atan2(dy, dx) with y down (0 = right, π/2 = down). World units (the canvas's).
//
// Pure functions, no allocation; UI thread.

#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>

#include "pq/graph/types.h"

namespace pq::views::circuit {

// A block's outline (world units): a circle (round: centre, radius) or a rect with rounded corners.
struct Outline {
  bool round = false;
  float cx = 0.0f, cy = 0.0f, r = 0.0f;              // round
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // rect
  float corner = 0.0f;                               // rect: the corner radius
  bool valid() const noexcept { return round ? r > 0.0f : x1 > x0 && y1 > y0; }
  graph::Point2 centre() const noexcept { return round ? graph::Point2{cx, cy} : graph::Point2{(x0 + x1) * 0.5f, (y0 + y1) * 0.5f}; }
  Outline shifted(float dx, float dy) const noexcept {
    Outline o = *this;
    o.cx += dx, o.cy += dy, o.x0 += dx, o.x1 += dx, o.y0 += dy, o.y1 += dy;
    return o;
  }
};

inline constexpr float kOrbitPortR = 4.5f;      // a port's radius (world units at block scale 1)
inline constexpr float kOrbitGap = 1.5f;        // between the outline and a port's rim
inline constexpr float kOrbitSpacing = 3.0f;    // between a port's rim and another pin (or the other port's rim)
inline constexpr float kOrbitPinR = 3.5f;       // the reach of a fixed pin (its socket) for the spacing
inline constexpr float kOrbitDefaultIn = std::numbers::pi_v<float>, kOrbitDefaultOut = 0.0f;
inline constexpr uint32_t kOrbitSearchSteps = 90;  // spacing: up to ±90 steps of 2° (bounded)
// The animation (canvas): the angle eases toward its target with this time constant, and snaps when within kOrbitSnap.
inline constexpr double kOrbitEaseSeconds = 0.07;
inline constexpr float kOrbitSnap = 0.002f;  // radians

// The outline point in direction `angle` from its centre and the outline's outward unit normal there.
graph::Point2 outlinePoint(const Outline& o, float angle, graph::Point2* normal = nullptr) noexcept;
// The signed distance from p to the outline (world units; > 0 outside).
float outlineDistance(const Outline& o, graph::Point2 p) noexcept;
// Where a port at `angle` sits: the outline point pushed out by (kOrbitPortR + kOrbitGap) × scale.
graph::Point2 orbitPoint(const Outline& o, float angle, float scale) noexcept;
// The angle toward `targets` (the mean of the unit directions from the centre); `fallback` when there are none or they
// cancel.
float orbitAngle(const Outline& o, std::span<const graph::Point2> targets, float fallback) noexcept;
// The signed shortest turn from angle a to angle b (−π … π).
float angleDelta(float a, float b) noexcept;
// An angle in [0, 2π).
float wrapAngle(float a) noexcept;

// One block's two main ports, solved together (the spacing rule above). `on[k]`: port k (0 = input, 1 = output)
// orbits; `pinned[k]`: it keeps angles[k] as given; else angles[k] is set from targets[k] (with the default fallback).
// `fixed`: the block's other visible pins (world points). `scale`: the block's scale.
void solveOrbits(const Outline& o, float scale, const bool on[2], const bool pinned[2], const std::span<const graph::Point2> targets[2],
                 std::span<const graph::Point2> fixed, float angles[2]) noexcept;

}  // namespace pq::views::circuit
