#pragma once

// T6.2: the latency rules of the registry's atoms (AtomSignature::latency; pq/graph/latency.h). RT-safe arithmetic.

#include "pq/graph/atoms.h"

namespace pq::graph::latency_rules {

double frames(const LatencyArgs& a) noexcept;     // latency(x, frames): frames / the region's factor
double converter(const LatencyArgs& a) noexcept;  // oversample_up / _down: half the region's round trip

}  // namespace pq::graph::latency_rules
