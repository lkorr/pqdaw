#pragma once

// FD.1: the range of values an output of a TypedPatch can take, by interval analysis. The scheduler bounds delay times
// with it (schedule.h "Loop": a delay line whose time is at least D samples can be cut and its loop run in sub-blocks);
// tests read it directly. Not RT code: compiler thread.
//
// ---- What a range promises ----------------------------------------------------------------------------------------
// [lo, hi] (closed, float32) holds every value the program can compute for that output and channel, in every block, on
// every voice lane, on both backends, PROVIDED every value the expression reads is finite (no NaN, no ±inf: a signal
// that is NaN or infinite is broken on any schedule, and what a delay tap then reads is meaningless either way; it is
// still inside its ring). Unknown is [−inf, +inf]. A range is never optimistic:
//   - endpoints are computed in float32 with the runtime's own operations (add / sub / mul / div round to nearest and
//     are monotonic, muladd is an fma, lerp is fma(t, b − a, a): pq/dsp kernels_math.cpp), so for an expression of
//     constants the range is the one value the program computes, bit for bit (M1 review M10: 20.75 ms · 48 kHz is 996
//     in double, 995.99994 at runtime);
//   - a product that feeds an add or a sub (directly or through a neg) may be contracted into an fma by the JIT
//     (CompileOptions::contract): both roundings are inside the range;
//   - a result whose rounding is not monotonic in its inputs (lerp over ranges: a appears twice) is widened by a
//     relative 2^−20 of the largest magnitude involved (its error is below 2^−22);
//   - approximated sources (sine, sine_hz, tanh, sin and their fast_ forms, noise, noise_step) are [−1, 1] widened by
//     1e−6 (fast_sin's error is 1.5e−7; an fma-contracted polynomial may differ in the last bits);
//   - FTZ / DAZ: an endpoint that is denormal is moved out to zero's side (the runtime flushes such a value to 0).
// Param ramps (schedule.h "Param ramps") stay inside: a ramp runs between two values its source took and never passes
// either (pq/dsp kernels_ramp.cpp: target − inc · remaining).
//
// ---- What has a range ------------------------------------------------------------------------------------------------
//   literals, `sr` (the sample rate times the node's oversample factor), a real param with a declared range
//   (ValueRangeOptions::trustParamRanges: the engine clamps params to their ranges);
//   add, sub, mul, div (a divisor range without 0), neg, abs, min, max, clip, muladd, lerp, select (the hull of its
//   two sides), phasor and frac [0, 1], gate_to_real [0, 1], sign [−1, 1], the bounded sources above;
//   PER CHANNEL through the structural atoms: channel c of `pack(a, b, …)` is the range of its c-th input channel,
//   `channel(x, i)` that of x's channel i, a mono value read as channel c is its channel 0, an `in` with a driver is
//   its driver.
// Everything else is unknown: engine sources (transport.bpm, voice.*, inputs), bools, stateful atoms (a z1's value
// depends on its past), tables, and `curve` — a driven control's curve reads its range from its CurveSlot at run time
// (pq/dsp/curve.h: dragging a range never recompiles), so no compile-time range holds for it. A value that depends on
// itself (feedback) is unknown.

#include <cstdint>
#include <limits>
#include <unordered_map>

#include "pq/graph/infer.h"

namespace pq::graph {

struct ValueRange {
  float lo = -std::numeric_limits<float>::infinity();
  float hi = std::numeric_limits<float>::infinity();
  bool unknown() const noexcept { return lo == -std::numeric_limits<float>::infinity() && hi == std::numeric_limits<float>::infinity(); }
  friend bool operator==(const ValueRange&, const ValueRange&) = default;
};

struct ValueRangeOptions {
  double sampleRate = 48000.0;   // values `sr`
  bool trustParamRanges = true;  // a ranged real param is [min, max]
};

// Ranges of one TypedPatch (flattened: infer(), any stage of optimise()). `adj` is tp.patch.adjacency() and, like `tp`,
// must outlive the object. Results are memoised per (output, channel).
class ValueRanges {
public:
  ValueRanges(const TypedPatch& tp, const Adjacency& adj, const ValueRangeOptions& options = {});

  // Output `port` of node n, channel ch (a channel past the output's width reads its last one, as an op does).
  ValueRange output(NodeIndex n, uint32_t port, uint32_t ch);
  // Input i of node n as its channel-ch op reads it: the driver's range, or the atom's default when unconnected.
  ValueRange input(NodeIndex n, uint32_t i, uint32_t ch);

private:
  struct Src {
    NodeIndex node = kNone;  // kNone: an unconnected input, `value` its default
    uint32_t port = 0, ch = 0;
    float value = 0.0f;
  };
  struct Term;
  Src source(NodeIndex n, uint32_t i, uint32_t ch) const;
  ValueRange of(const Src& s);
  ValueRange eval(NodeIndex n, uint32_t port, uint32_t ch);
  Term term(NodeIndex n, uint32_t i, uint32_t ch, bool negate);

  const TypedPatch* tp_;
  const Adjacency* adj_;
  ValueRangeOptions opt_;
  uint32_t depth_ = 0;
  std::unordered_map<uint64_t, ValueRange> memo_;  // (flat output, channel) → range; in progress: unknown
};

}  // namespace pq::graph
