#pragma once

// The optimiser (T1.4, SPEC §7.4 step 3): TypedPatch → TypedPatch, semantics preserved, run on the compiler thread
// between inference (infer.h) and scheduling (T1.5). Not RT-safe.
//
// Passes (a fixpoint driver repeats the sweep until nothing changes, at most OptimiseOptions::maxPasses times):
//   0. Durations (once, first): every registry input that kernel_map.h marks InputSeconds (delay `time`, timer `dur`,
//      pulse `period`) gets `· sr` inserted in front of it, so after optimise() those inputs carry SAMPLES
//      (TypedPatch::durationsInSamples). The product has the rate of the duration, so a const duration folds and a
//      param one is computed once per block; kernels never convert per sample. A literal written in `smp` that
//      feeds such an input directly is already in samples and is wired as is (QUESTIONS Q30.6). With a known sample
//      rate, an `smp` literal into a delay's `max` becomes seconds (n / sr; the builder's ceil(max · sr) can then
//      only round up by one frame). A value computed from an `smp` literal (`5smp * 2`, `c ? 5smp : 9smp`) that
//      reaches a duration input is an error: the unit is lost in arithmetic, so it would be read as seconds. (A
//      param can never be in `smp`: the parser and Patch::validate reject it.)
//   1. Sweep, in topological order (feedback edges into delayed inputs ignored; nodes on other cycles last):
//      - constant folding: a pure kernel atom whose inputs are all literals is evaluated by ITS OWN dsp kernel, in the
//        variant the program runs it with (const/param values run in Control regions on the 8-lane kernels: lane 0,
//        1 frame, FTZ/DAZ on), so the folded value is bit-identical to the same expression on a param at runtime.
//        `sr` folds when the sample rate is known. Params, inputs, stateful and side-effecting atoms are never folded.
//      - algebraic simplification, value-exact in float32 (no nnan/ninf/nsz, see optimise.cpp for the list): x·1,
//        x/1, x+(−0), x−(+0), x+(+0) and x−(−0) when x cannot be −0, x·(±0) when x is provably finite and ≥ +0 (so the
//        sign of the zero is known), x/2^k → x·2^−k, x·(−1) → −x, −(−x), !!x, bool identities, select/mux with a
//        literal selector, channel of a mono value or of a pack, a voice reducer of a global value; pow(x, 2) → x·x
//        is the one approximate-function (afn) rewrite.
//      - select sinking: f(…, select(c, a, b), …) → select(c, f(…, a, …), f(…, b, …)), also through mux and nested
//        selects (≤ 16 copies), when f is a pure stateless element-wise kernel at audio rate and every copy's inputs
//        are below audio rate: the copies run once per block, only the select per sample (adsr's coefficient
//        `1 − exp(−1/max(time·sr, 1))` with `time = attacking ? a : gate ? d : r`). Exact: f's value is the same
//        whichever arm is picked; evaluating it on every arm has no effect. Mono values only.
//      - common subexpression elimination: pure atoms with the same atom and inputs (commutative inputs sorted,
//        except min/max whose NaN behaviour is order-dependent) and equal literals of the same unit are merged.
//        Stateful atoms, params and inputs are never merged.
//      - dead-node elimination: nodes that reach no `out`, `export` or side effect (send) are removed. Params and
//        root inputs are kept (they are the program's interface); a solve block lives or dies as a whole.
//   2. Rate refinement (hoisting): the rate of every pure atom is recomputed as the max of its inputs' rates, so
//      an expression whose audio-rate input was simplified away drops to param (once per block) or const. Inference
//      already types `tan(π·cutoff/sr)` param when `cutoff` is unmodulated; this keeps that true after rewrites.
//      Reassociation (`(x·a)·b` → `x·(a·b)`) would hoist more but is not exact in float32, so it is not done.
//   3. Fusion (contract): add(mul(a, b), c) → muladd(a, b, c) when the mul has no other consumer and the same rate,
//      poly and width as the add (a param-rate mul feeding an audio add stays hoisted).
//
// Invariants kept (T1.5/T1.7 rely on them): no Subcircuit nodes; `types` parallel to the port array and concrete;
// every input's type is its driver's output type (unconnected: const, global, mono); `stateKey`/`origin` per node,
// and every stateful node keeps its stateKey, atom, inputs' meaning and output types (state transfer by key); a
// replacement never changes a value's kind, width or poly (a voice value is never replaced by a global one: the
// voice reducers treat them differently), and never raises a rate.

#include <cstdint>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/infer.h"

namespace pq::graph {

struct OptimiseOptions {
  double sampleRate = 0.0;      // > 0: the program is specialised for this rate and `sr` folds; 0: unknown
  bool lowerDurations = true;   // pass 0 (TypedPatch::durationsInSamples)
  bool fuseMulAdd = true;       // pass 3 (one rounding instead of two: the JIT's `contract` policy)
  bool sinkSelects = true;      // select sinking in the sweep (off: for comparisons and benchmarks)
  uint32_t maxPasses = 16;      // cap of the sweep fixpoint
  // T4.5 (out, optional; pq/graph/inspect.h): per flat port of the INPUT patch, the flat output port of the result that
  // carries the same value (kNone: an input port, or the value has no node any more: dead, or fused into another op).
  // Filling it changes nothing about the optimisation.
  std::vector<uint32_t>* valueMap = nullptr;
};

struct OptimiseStats {
  uint32_t nodesBefore = 0, nodesAfter = 0;
  uint32_t passes = 0;          // sweeps run
  bool converged = false;       // the last sweep changed nothing (false: stopped by maxPasses)
  uint32_t folded = 0;          // nodes replaced by literals
  uint32_t simplified = 0;      // algebraic rewrites
  uint32_t merged = 0;          // CSE merges
  uint32_t removed = 0;         // dead nodes removed
  uint32_t ratesLowered = 0;    // outputs whose rate dropped in rate refinement
  uint32_t fused = 0;           // muladd fusions
  uint32_t sunk = 0;            // ops sunk below a select/mux (select sinking)
  uint32_t durations = 0;       // `· sr` conversions inserted (or smp literals wired through)
};

// Errors: a channel index that is a constant expression (folded here) but not a channel of the signal it selects
// from (ErrorCode::Type; literal indices are already checked by infer()); a value computed from an `smp` literal at
// a duration input (ErrorCode::InvalidArgument, pass 0).
Result<TypedPatch> optimise(TypedPatch patch, const OptimiseOptions& options = {}, OptimiseStats* stats = nullptr);

}  // namespace pq::graph
