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
// the inputs of a sum align within half a sample (QUESTIONS Q?-M6-a item 7). A unit's notes and automation follow its
// A_u (round(A_u): unitLatency; ProjectProgram delays its events, the timeline evaluates its lanes earlier).
// Cost: O(units + bus reads).

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "pq/engine/project_program.h"

namespace pq::engine {

struct UnitLatency {
  std::vector<std::pair<uint32_t, double>> out;  // (bus, frames) for the buses it produces (missing: 0)
  std::vector<uint32_t> compensated;             // buses it reads through Audio / Sidechain wires
};

struct InputDelay {
  uint32_t bus = kNoBus;
  uint32_t frames = 0;
  friend bool operator==(const InputDelay&, const InputDelay&) = default;
};

struct CompensationPlan {
  std::vector<std::vector<InputDelay>> delays;  // per unit (layout order): its delayed input buses (frames ≥ 1)
  std::vector<uint32_t> unitLatency;            // per unit: round(A_u), its notes' and automation's shift
  std::vector<double> busLatency;               // per bus: L(b)
  uint32_t total = 0;                           // round(L(output bus))
  // No delay and no shift anywhere: the program runs exactly as without compensation (no rings).
  bool none() const noexcept;
  // Same delays and shifts (busLatency aside): publishing it again would change nothing.
  bool sameAs(const CompensationPlan& o) const noexcept;
};

// `unitOffset` (empty, or per unit): frames added to everything the unit produces (T6.6's Track Delay). Units whose
// latencies are missing count as 0. Not RT-safe (allocates).
CompensationPlan planCompensation(const ProjectLayout& layout, std::span<const UnitLatency> units,
                                  std::span<const int32_t> unitOffset = {});

}  // namespace pq::engine
