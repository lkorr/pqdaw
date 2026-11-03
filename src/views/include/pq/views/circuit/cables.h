#pragma once

// Hanging cables (TX.cables, SPEC §15 2026-11-02 "cables and the node menu"): the shape and the motion of a patch cable
// between two ports, for the circuit canvas (pq/views/circuit/circuit_canvas.h draws them; the project patch view's scene
// marks which wires are cables).
//
//   rest shape  cableRest(): the static solve — a uniform, inextensible rope of length chord × kCableSlack hanging under
//               gravity between its two ends is a catenary, and its shape depends only on the chord's angle (an
//               inextensible rope's equilibrium does not depend on the strength of gravity or on the scale). So the
//               catenaries of a unit chord are solved once (double precision, 1° apart from straight up to straight
//               down, kRestTablePoints points at equal arc length each) and a cable's rest shape is a table lookup:
//               two neighbouring angles blended, resampled to its point count, scaled, rotated into place, ends exact.
//               Deterministic, so goldens show it, and ~n multiply-adds per cable.
//   motion      CableSim: one rope per cable, Verlet integration at a fixed kCableStep with damping, its ends pinned to
//               the ports, length constraints (no segment longer than the rest shape's: it does not stretch, a cable
//               rather than an elastic string, but it may go slack) and gravity's restoring force about the hanging shape: each point is pulled
//               toward its place on the rest shape as a pendulum of the rope's sag would be (ω² = kCableGravity / sag:
//               a short, tight cable swings quickly, a long one slowly). So the rest shape is the motion's exact
//               equilibrium: a settled cable and a freshly built one look the same. When a port moves the rope is
//               carried most of the way (kCableCarry) and swings over the rest. Each cable sleeps once it is calm (no
//               point moved more than kCableSleepSpeed per step for kCableCalmSteps steps, within kCableSleepOffset of
//               its rest shape) and then sits exactly on it; a cable awake kCableBoostSeconds after its last
//               disturbance is damped harder, and at kCableMaxAwakeSeconds it is put to rest, so motion always ends
//               within 2 s of the last move. Asleep cables cost nothing; the canvas runs frames only while some cable
//               is awake.
//
// Units: world units (the canvas's; logical px at zoom 1), seconds. UI thread only; plain data, no allocation per step
// (the pools grow when cables are added).

#include <cstdint>
#include <span>
#include <vector>

#include "pq/graph/types.h"

namespace pq::views::circuit {

inline constexpr float kCableSlack = 1.06f;          // rest length / chord: a 6 % droop (sag ≈ 0.15 × the chord level)
inline constexpr uint32_t kCableMinPoints = 12, kCableMaxPoints = 24;
inline constexpr uint32_t kRestTablePoints = 33;     // per tabulated catenary
inline constexpr double kCableStep = 1.0 / 120.0;    // the simulation's fixed time step
inline constexpr uint32_t kCableMaxStepsPerAdvance = 4;  // a late frame slows the motion rather than taking big steps
inline constexpr float kCableGravity = 1800.0f;      // world units / s² (the swing's restoring force: ω² = g / sag)
inline constexpr float kCableDamping = 0.96f;        // velocity kept per step
inline constexpr float kCableCarry = 0.85f;          // how much of an end's move the rope takes at once
inline constexpr float kCableSleepSpeed = 0.02f;     // world units per step
inline constexpr float kCableSleepOffset = 0.5f;     // world units from the rest shape (the snap is invisible)
inline constexpr uint32_t kCableCalmSteps = 10;
inline constexpr double kCableBoostSeconds = 1.2, kCableMaxAwakeSeconds = 1.75;
inline constexpr uint32_t kCableIterations = 3;      // constraint passes per step

// The point count of a cable whose chord is `chord` world units long (kCableMinPoints … kCableMaxPoints).
uint32_t cablePointCount(float chord) noexcept;

// The rest shape (static solve): `n` (≥ 2) points from `a` to `b` at equal arc length along the hanging rope of length
// |b − a| × slack (slack ≥ 1; kCableSlack's table is cached, another slack is solved directly). out[0] == a and
// out[n − 1] == b exactly. Straight up / down chords hang as a narrow loop under the lower end (as a slack rope does).
void cableRest(graph::Point2 a, graph::Point2 b, uint32_t n, graph::Point2* out, float slack = kCableSlack) noexcept;

class CableSim {
public:
  // A cable by its identity (stable across rebuilds of the scene: a kept cable keeps its motion), its ends now.
  struct Spec {
    uint64_t key = 0;
    graph::Point2 a, b;
  };
  // The set of cables becomes `specs` (index i = specs[i]). A key seen before keeps its points and velocity and
  // moves its ends (setEnds: it swings when they moved); a new key starts asleep on its rest shape.
  void sync(std::span<const Spec> specs);
  uint32_t size() const noexcept { return static_cast<uint32_t>(cables_.size()); }

  // Cable i's ends move to a, b: its rest shape follows at once; with animation on, the rope is carried kCableCarry of
  // the way and wakes to swing over the rest (off: it is on the new rest shape at once, asleep). No-op when they did
  // not move. `animate` false: onto the new rest shape at once (a control's anchor shifting with its face's layout).
  void setEnds(uint32_t i, graph::Point2 a, graph::Point2 b, bool animate = true);
  graph::Point2 endA(uint32_t i) const noexcept { return cables_[i].a; }
  graph::Point2 endB(uint32_t i) const noexcept { return cables_[i].b; }

  // Off (tests, goldens): every cable on its rest shape, asleep, and every later move lands there at once.
  void setAnimated(bool on);
  bool animated() const noexcept { return animated_; }

  // Runs the fixed steps due in `seconds` (≤ kCableMaxStepsPerAdvance; a remainder carries to the next call). Returns
  // whether any cable moved. Only awake cables are stepped.
  bool advance(double seconds);
  // Every cable on its rest shape, asleep (the far level of detail, where cables do not animate).
  void settle();
  bool awake() const noexcept { return awakeCount_ > 0; }
  bool awake(uint32_t i) const noexcept { return cables_[i].awake; }
  uint32_t awakeCount() const noexcept { return awakeCount_; }

  std::span<const graph::Point2> points(uint32_t i) const noexcept {
    return {pos_.data() + cables_[i].first, cables_[i].n};
  }
  // World bounds of cable i's points now (and, for damage, the bounds it had before the last advance / setEnds).
  struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  };
  const Box& bounds(uint32_t i) const noexcept { return cables_[i].box; }
  const Box& previousBounds(uint32_t i) const noexcept { return cables_[i].prevBox; }
  // Bumped whenever any point moved (hit-test grids cache against it).
  uint64_t version() const noexcept { return version_; }
  uint64_t steps() const noexcept { return steps_; }  // cable steps run (benchmarks, tests)

private:
  struct Cable {
    uint64_t key = 0;
    graph::Point2 a, b;
    uint32_t first = 0, n = 0;  // into the pools
    float seg = 0.0f;           // the longest a segment may be (the rest shape's longest)
    float pull = 0.0f;          // the restoring pull per step (ω² dt², ω² = g / sag)
    bool awake = false;
    uint32_t calm = 0, awakeSteps = 0;
    Box box, prevBox;
  };
  void place(Cable& c);         // rest shape for its ends into rest_, seg
  void toRest(Cable& c);        // pos = prev = rest
  void step(Cable& c);
  void updateBox(Cable& c);
  void sleep(Cable& c);

  std::vector<Cable> cables_;
  std::vector<graph::Point2> pos_, prev_, rest_;  // pools: cable i's points at [first, first + n)
  double pending_ = 0.0;
  uint32_t awakeCount_ = 0;
  uint64_t version_ = 0, steps_ = 0;
  bool animated_ = true;
};

}  // namespace pq::views::circuit
