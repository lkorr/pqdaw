// T6.2: plugin-delay compensation plan (pq/engine/latency_plan.h).
#include "pq/engine/latency_plan.h"

#include <algorithm>
#include <cmath>

namespace pq::engine {

bool CompensationPlan::none() const noexcept {
  for (const auto& d : delays)
    if (!d.empty()) return false;
  for (uint32_t a : unitLatency)
    if (a != 0) return false;
  return true;
}

bool CompensationPlan::sameAs(const CompensationPlan& o) const noexcept {
  return delays == o.delays && unitLatency == o.unitLatency && total == o.total;
}

CompensationPlan planCompensation(const ProjectLayout& layout, std::span<const UnitLatency> units,
                                  std::span<const int32_t> unitOffset) {
  const auto numUnits = static_cast<uint32_t>(layout.units.size());
  const auto numBuses = static_cast<uint32_t>(layout.busWidth.size());
  CompensationPlan plan;
  plan.delays.resize(numUnits);
  plan.unitLatency.assign(numUnits, 0);
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
    const double offset = u < unitOffset.size() ? static_cast<double>(unitOffset[u]) : 0.0;
    for (uint32_t b : produced[u]) {
      double frames = 0.0;
      for (const auto& [ob, f] : ul.out)
        if (ob == b) frames = f;
      plan.busLatency[b] = std::max(0.0, a + frames + offset);
    }
  }
  if (layout.outputBus != kNoBus && layout.outputBus < numBuses)
    plan.total = static_cast<uint32_t>(std::lround(plan.busLatency[layout.outputBus]));
  return plan;
}

}  // namespace pq::engine
