#pragma once

// Constant evaluation shared by the optimiser (optimise.cpp: constant folding, select / mux with a literal selector)
// and the structural-constant analysis of the circuit editor (structure.cpp, T4.15), so both decide the same way.
// Values are in the kernels' domain: real as float, bool as a lane mask (dsp::boolMask / maskIsTrue). Not RT-safe.

#include <cstdint>

#include "pq/graph/types.h"

namespace pq::graph::detail {

inline constexpr uint32_t kMaxFoldPorts = 64;

// The value a const- or param-rate evaluation of a pure kernel atom produces at runtime for these inputs: its own
// 8-lane dsp kernel run for one frame (lane 0), FTZ/DAZ on (see optimise.cpp). False when the atom is not a foldable
// kernel (stateful, a resource, a side effect, non-input arguments) or has more than kMaxFoldPorts ports. Results are
// not checked for finiteness (callers decide).
bool foldPureAtom(AtomId atom, const float* in, uint32_t nIn, float* out, uint32_t nOut);

// The input a mux with `count` data inputs picks for `index`: clamp(floor(index), 0, count − 1), NaN → 0.
uint32_t muxPick(float index, uint32_t count) noexcept;

}  // namespace pq::graph::detail
