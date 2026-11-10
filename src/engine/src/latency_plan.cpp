// T6.2: plugin-delay compensation plan (pq/engine/latency_plan.h).
#include "pq/engine/latency_plan.h"

#include <algorithm>
#include <cmath>

namespace pq::engine {

bool CompensationPlan::none() const noexcept {
  for (const auto& d : delays)
    if (!d.empty()) return false;
  for (const auto& d : clipDelays)
    if (!d.empty()) return false;
  for (uint32_t a : eventDelay)
    if (a != 0) return false;
  for (uint32_t a : unitLatency)
    if (a != 0) return false;
  return true;
}

bool CompensationPlan::sameAs(const CompensationPlan& o) const noexcept {
  return delays == o.delays && clipDelays == o.clipDelays && eventDelay == o.eventDelay && unitLatency == o.unitLatency &&
         total == o.total;
}

CompensationPlan planCompensation(const ProjectLayout& layout, std::span<const UnitLatency> units,
                                  std::span<const int32_t> unitOffset) {
  const auto numUnits = static_cast<uint32_t>(layout.units.size());
  const auto numBuses = static_cast<uint32_t>(layout.busWidth.size());
  CompensationPlan plan;
  plan.delays.resize(numUnits);
  plan.clipDelays.resize(numUnits);
  plan.eventDelay.assign(numUnits, 0);
  plan.unitLatency.assign(numUnits, 0);
  plan.arrival.assign(numUnits, 0.0);
  plan.busLatency.assign(numBuses, 0.0);
  std::vector<std::vector<uint32_t>> produced(numUnits);
  for (uint32_t b = 0; b < numBuses; ++b)
    if (layout.busProducer[b] < numUnits) produced[layout.busProducer[b]].push_back(b);
  const UnitLatency none;
  for (uint32_t u = 0; u < numUnits; ++u) {
    const UnitLatency& ul = u < units.size() ? units[u] : none;
    double a = 0.0;
    for (uint32_t b : ul.compensated)
      if (b < numBuses) a = std::max(a, plan.busLatency[b]);
    for (uint32_t b : ul.compensated) {
      if (b >= numBuses) continue;
      const double d = std::round(a - plan.busLatency[b]);
      if (d >= 1.0 && std::none_of(plan.delays[u].begin(), plan.delays[u].end(), [&](const InputDelay& x) { return x.bus == b; }))
        plan.delays[u].push_back(InputDelay{b, static_cast<uint32_t>(d)});
    }
    plan.unitLatency[u] = static_cast<uint32_t>(std::lround(a));
    plan.arrival[u] = a;
    // T6.2b: its sources wait for its inputs (and a merged member's for the later audio inside the unit).
    for (const auto& [slot, shift] : ul.clips) {
      const long d = std::lround(a + std::max(0.0, shift));
      if (d >= 1 && std::none_of(plan.clipDelays[u].begin(), plan.clipDelays[u].end(), [&](const SourceDelay& x) { return x.slot == slot; }))
        plan.clipDelays[u].push_back(SourceDelay{slot, static_cast<uint32_t>(d)});
    }
    plan.eventDelay[u] = static_cast<uint32_t>(std::lround(a + std::max(0.0, ul.eventShift)));
    const double offset = u < unitOffset.size() ? static_cast<double>(unitOffset[u]) : 0.0;
    for (uint32_t b : produced[u]) {
      double frames = 0.0;
      for (const auto& [ob, f] : ul.out)
        if (ob == b) frames = f;
      plan.busLatency[b] = a + frames + offset;  // T6.6: negative with a negative offset (a positive Track Delay)
    }
  }
  if (layout.outputBus != kNoBus && layout.outputBus < numBuses)
    plan.total = static_cast<uint32_t>(std::lround(std::max(0.0, plan.busLatency[layout.outputBus])));
  return plan;
}

}  // namespace pq::engine
