#pragma once

// T6.2 (SPEC §7.6, docs/plans/M6.md T6.2): plugin-delay compensation between compile units. Pure, non-RT.
//
// Every unit reports, per bus it produces, the latency of its longest path from its inputs and sources to that bus
// (UnitLatency::out: graph::pathLatency of its optimised patch; ProjectCompiler::latencies()). Walking the units in
// layout (dependency) order:
//   A_u          = max over the buses u reads through AUDIO wires (track routing, sends, sidechains: UnitLatency::
//                  compensated) of L(bus); 0 without any
//   delay(u, b)  = round(A_u − L(b)) for each of those buses (the READER delays its input: ProjectProgram)
//   L(b)         = A_u + out(u, b) + offset(u) for each bus b that u produces
//   total        = round(L(output bus)): what the device hears late (ProjectProgram::outputLatency)
// Buses read only through modulation wires are not delayed (SPEC §15 2026-10-24: cross-track modulation adds no
// delay). Fractional latencies (an oversampled group: 3.62 samples) are rounded per input to the nearest sample, so
// the inputs of a sum align within half a sample (QUESTIONS Q?-M6-a item 7).
// A unit's own SOURCES follow its inputs (T6.2b): each member track t of u has an internal shift T(t) ≥ 0 (0 unless u
// is merged and audio reaches t through a member's later path, e.g. a latent kick keying a bass: the compiler's
// `target`, project_compile.h), and everything t plays from itself is delayed by S(t) = A_u + T(t):
//   clip slot k of member t   clipDelays[u] ∋ {k, round(S(t))} (ProjectProgram delays ProcessContext::clipIn[k])
//   the instrument's notes    eventDelay[u] = round(A_u + eventShift)
//   automation of t's params  round(arrival[u] + T(t)) (the link: AutomationLaneDesc::shiftFrames)
// (round(A_u): unitLatency, the readouts.) Cost: O(units + bus reads + clip slots).

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "pq/engine/project_program.h"

namespace pq::engine {

struct UnitLatency {
  std::vector<std::pair<uint32_t, double>> out;  // (bus, frames) for the buses it produces (missing: 0)
  std::vector<uint32_t> compensated;             // buses it reads through Audio / Sidechain wires
  // T6.2b: its sources. (clip slot, T of the member playing it) for every clip slot it reads; T of its instrument
  // member (its notes); (track id, T) of the members with T > 0 (their automation, read by the app).
  std::vector<std::pair<uint32_t, double>> clips;
  double eventShift = 0.0;
  std::vector<std::pair<uint32_t, double>> trackShift;
};

struct InputDelay {
  uint32_t bus = kNoBus;
  uint32_t frames = 0;
  friend bool operator==(const InputDelay&, const InputDelay&) = default;
};

// T6.2b: a delayed clip slot (ProcessContext::clipIn) of a unit.
struct SourceDelay {
  uint32_t slot = 0;
  uint32_t frames = 0;
  friend bool operator==(const SourceDelay&, const SourceDelay&) = default;
};

struct CompensationPlan {
  std::vector<std::vector<InputDelay>> delays;       // per unit (layout order): its delayed input buses (frames ≥ 1)
  std::vector<std::vector<SourceDelay>> clipDelays;  // per unit: its delayed clip slots (frames ≥ 1)
  std::vector<uint32_t> eventDelay;                  // per unit: its notes' delay, round(A_u + eventShift)
  std::vector<uint32_t> unitLatency;                 // per unit: round(A_u)
  std::vector<double> arrival;                       // per unit: A_u
  std::vector<double> busLatency;                    // per bus: L(b)
  uint32_t total = 0;                                // round(L(output bus))
  // No delay anywhere: the program runs exactly as without compensation (no rings).
  bool none() const noexcept;
  // Same delays (busLatency, arrival aside): publishing it again would change nothing in the program.
  bool sameAs(const CompensationPlan& o) const noexcept;
};

// `unitOffset` (empty, or per unit): frames added to everything the unit produces (T6.6's Track Delay, negated). A
// negative offset can make L(b) negative: the bus is early, so its reader delays it by −L(b) more (A_u starts at 0, so
// no reader is ever early: delays and totals stay ≥ 0). Units whose latencies are missing count as 0. Not RT-safe
// (allocates).
CompensationPlan planCompensation(const ProjectLayout& layout, std::span<const UnitLatency> units,
                                  std::span<const int32_t> unitOffset = {});

}  // namespace pq::engine
