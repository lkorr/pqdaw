#pragma once

// Inspection of running programs (T4.5): live probes and cost meters, the compiler's half.
//
// ---- Live probes (SPEC §7.3 / §11.1): which value of a running program a probe reads, through the compiler.
//
// A probe names an output of a node in the patch AS WRITTEN (a wire's source on the circuit canvas): the instance it is
// in (the call path from the root patch, NodeIds of the subcircuit call nodes) and the node's NodeId and output. The
// compiler carries it to the value the program computes:
//   infer     InferOptions::probes → InferResult::probeSources: the flattened output that carries the value (through
//             subcircuit boundaries, `in`/`param` pass-throughs and inserted voice reducers);
//   optimise  OptimiseOptions::valueMap: where each flattened value ended up (folding, CSE and fusion forward values;
//             a value fused into another op, e.g. a mul inside a muladd, or a dead one has none);
//   schedule  ScheduleOptions::probes → Schedule::probes: the value's operand, kept alive (and, in generated code,
//             stored) until the host copies it, after the last stage (global values) or after each voice group of its
//             stage (voice values). The probe changes buffer assignment only: every op, region and the sleep plan are
//             those of the schedule without it, so the audio is bit-identical (tests/graph/test_probe.cpp,
//             tests/engine/test_probes.cpp).
// probePoints() does the infer → optimise step for a caller that ran both (the circuit editor's check).
//
// ---- Cost meters (SPEC §7.3 / §7.6): the engine measures time per REGION of a schedule (pq/engine/inspect.h: rdtsc
// around each region while a meter is shown). opCostWeight() is the static estimate that splits one region's measured
// time among the ops in it (a region usually holds ops of several subcircuits): relative units, comparable only between
// ops of one region (a Control region's ops run once per block, a voice op once per frame per group).
//
// Not RT-safe: compiler / UI thread.

#include <cstdint>
#include <span>
#include <vector>

#include "pq/graph/patch.h"
#include "pq/graph/types.h"

namespace pq::graph {
struct Schedule;
}

namespace pq::graph {

struct ProbeTarget {
  std::vector<NodeId> path;  // subcircuit call nodes from the root patch to the instance (empty: the root patch)
  NodeId node;               // a node of that instance's patch
  uint16_t output = 0;       // which of its outputs (a subcircuit call: the callee's `out`s, then its `export`s)
};

// One probe of the scheduled program: a node output of the TypedPatch given to schedule() (after optimise()).
struct ProbePoint {
  NodeIndex node = kNone;
  uint16_t output = 0;
  uint16_t channel = 0;   // of a multi-channel value
  uint32_t id = 0;        // the caller's name for it (Schedule::probes keeps it; the engine binds taps by it)
  bool voiceSum = false;  // a per-voice value: the sum over the sounding voices (default: the newest voice)
};

// The optimised patch's output carrying each target's value: `sources` = InferResult::probeSources (flat output ports
// of the inferred patch, kNone: none), `valueMap` = OptimiseOptions::valueMap of the optimise() run on that inferred
// patch. A target whose value has no node any more (optimised away) gets node = kNone. ids[i] (when given) become the
// points' ids, else i.
std::vector<ProbePoint> probePoints(const Patch& optimised, std::span<const uint32_t> sources,
                                    std::span<const uint32_t> valueMap, std::span<const uint32_t> ids = {});

// Relative cost of op `op` of `s` per frame it processes (> 0): ~1 for an add, more for divisions, transcendentals,
// table reads, delay lines, mux width. An estimate for splitting a measured region, not a prediction.
float opCostWeight(const Schedule& s, uint32_t op) noexcept;

}  // namespace pq::graph
