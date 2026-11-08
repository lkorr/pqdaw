#pragma once

// EQ.2 (docs/plans/EQ.md "Analytic responses"): the RESPONSE RULE of an atom — its complex small-signal gain from each
// input to each output at one frequency, given the values of its inputs that do not depend on the signal. Every atom
// def of the registry (atoms.h AtomSignature::response) may have one; graph::LinearResponse (pq/graph/response.h)
// evaluates a whole graph from them. An atom without a rule is nonlinear: a path from the source to the sink through
// it makes the response Unavailable.
//
// How a rule is called (once per node and output channel, per evaluation; again per frequency only when it sets
// `dependsOnOmega`):
//   - `gain` holds numOutputs × numInputs zeros (row-major: gain[o · numInputs + i] = ∂out_o / ∂in_i). The rule writes
//     the entries of the inputs marked `signal`; the others are never read (writing them is harmless).
//   - `value[i]` is input i's value at this channel when `known[i]` (a const, a param, or what the evaluator computed
//     from them with the kernels: same float domain as the runtime, bools as lane masks, dsp::maskIsTrue). Signal
//     inputs and values the evaluator cannot know (another audio signal, a stateful atom's output) have known = 0.
//     DURATIONS ARE IN SAMPLES here (the evaluator converts the language's seconds, as optimise() would, keeping an
//     `smp` literal as is): a delay's `time` and `max`.
//   - The gains must have the same pattern (which entries are used) at every frequency; a rule whose gains depend on
//     omega (z1: `z`, a delay line, a filter) sets dependsOnOmega and is called again per frequency.
//   - EQ.1b: `cache` is the rule's own scratch (AtomSignature::responseCacheBytes, 8-byte aligned), per node and channel,
//     kept between the calls of one evaluation: on the first call (`fresh`) the rule computes what does not depend on
//     ω from the values (svf: its design) and stores it there; the calls at the other frequencies read it.
//   - Return Exact for a linear, time-invariant atom (at the given values); Approximate for a nonlinear atom
//     linearised at 0 (tanh: slope 1; the display draws it dashed); Unavailable when the atom is not linear in its
//     signal inputs here (mul of two signals, a gain that is itself an unknown audio signal, a choice made by a
//     signal). A rule never allocates, throws or blocks.
// Channels: atoms are element-wise (an input of width 1 broadcasts); `channel` and `pack` are the only atoms that
// move signals between channels, and the evaluator maps their channels itself (their rules only give the gain 1).
//
// Adding a rule (e.g. EQ.1's `svf`): write `ResponseKind rule(ResponseArgs&) noexcept` (src/graph/src/response_rules.h /
// .cpp: the declarations the registry table names), and put it as the last field of the atom's entry in the table
// (src/graph/src/atoms.cpp). Test it against an impulse measurement (tests/engine/test_linear_response.cpp).

#include <complex>
#include <cstdint>

namespace pq::graph {

enum class ResponseKind : uint8_t {
  Exact,        // linear and time-invariant: H is the circuit's
  Approximate,  // a nonlinear atom on the path, linearised at 0 (its small-signal slope)
  Unavailable,  // no response (a nonlinear atom without a slope, a time-varying gain, a structure not handled)
};

// The worse of two kinds (Exact < Approximate < Unavailable).
constexpr ResponseKind worse(ResponseKind a, ResponseKind b) noexcept { return a > b ? a : b; }

struct ResponseArgs {
  double omega = 0.0;                // the frequency, in radians per sample (0 … π)
  std::complex<double> z{1.0, 0.0};  // e^{−jω}: the response of one sample of delay
  std::complex<double> ejw1{0.0, 0.0};  // EQ.1b: e^{jω} − 1 as dsp::svfEjwMinus1(ω) (no cancellation near DC), per point
  double sampleRate = 48000.0;
  uint32_t numInputs = 0;
  uint32_t numOutputs = 0;
  const float* value = nullptr;      // per input (see above)
  const uint8_t* known = nullptr;    // per input: value[i] is meaningful
  const uint8_t* signal = nullptr;   // per input: depends on the source
  std::complex<double>* gain = nullptr;  // numOutputs × numInputs, zeroed by the caller
  void* cache = nullptr;             // EQ.1b: AtomSignature::responseCacheBytes of scratch (see above)
  bool fresh = true;                 // the evaluation's first call for this node and channel (fill the cache)
  bool dependsOnOmega = false;       // out: the gains change with omega
};

using ResponseRule = ResponseKind (*)(ResponseArgs& a) noexcept;

}  // namespace pq::graph
