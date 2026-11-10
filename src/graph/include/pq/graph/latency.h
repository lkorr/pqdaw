#pragma once

// T6.2 (SPEC §7.6 "PDC", docs/plans/M6.md T6.2): the latency a patch REPORTS, for plugin-delay compensation.
//
// A path's latency is the sum of what the atoms on it report (AtomSignature::latency, in base-rate samples):
//   latency(x, frames)                frames / the region's factor (a lookahead, a device's reported delay)
//   oversample_up / oversample_down   half of dsp::os::roundTripLatency(factor) each (2× 3.62, 4× 4.74, 8× 5.17,
//                                     16× 5.31 samples up + down at DC)
//   everything else                   0 (z1, delay lines, filters: their delay is the effect, not latency)
// pathLatency gives every node the LONGEST such path from any source (root `in`s, voice sources, clipPlayer, track.in,
// consts, params) to its outputs: max over its inputs' drivers, plus its own. A root `out` / `export` node's value is
// what arrives at it. Nodes are visited by strongly connected component in dependency order (iterative Tarjan), so a
// feedback loop (a z1 in an envelope, a comb) passes on what reaches it and adds nothing by going round: every member
// of a cycle gets the latest entry into it plus its own.
// Run it on an OPTIMISED TypedPatch (constants folded: a `latency`'s frames and the converters' factors are literals);
// an input that is not a literal reads as 0. Not RT-safe (allocates). O(nodes + wires).

#include <span>
#include <vector>

#include "pq/graph/infer.h"

namespace pq::graph {

// Per node of typed.patch: the longest reported latency from a source to its outputs (base-rate samples, ≥ 0).
std::vector<double> pathLatency(const TypedPatch& typed);

// The latency arriving at root `out` / `export` node `node` (its input's driver), from a pathLatency() result.
double arrivalLatency(const TypedPatch& typed, std::span<const double> latency, NodeIndex node);

}  // namespace pq::graph
