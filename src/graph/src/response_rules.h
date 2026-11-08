#pragma once

// EQ.2: the response rules of the registry's atoms (pq/graph/response_rule.h). The table in atoms.cpp names them as the
// last field of an atom's entry. Each is RT-safe (no allocation; pure arithmetic).

#include "pq/graph/response_rule.h"

namespace pq::graph::rules {

ResponseKind add(ResponseArgs& a) noexcept;       // a + b
ResponseKind sub(ResponseArgs& a) noexcept;       // a − b
ResponseKind neg(ResponseArgs& a) noexcept;       // −x
ResponseKind mul(ResponseArgs& a) noexcept;       // a · b, one of them not a signal
ResponseKind div(ResponseArgs& a) noexcept;       // a / b, b not a signal
ResponseKind muladd(ResponseArgs& a) noexcept;    // a · b + c, a or b not a signal
ResponseKind lerp(ResponseArgs& a) noexcept;      // a + (b − a)·t, t not a signal
ResponseKind select(ResponseArgs& a) noexcept;    // cond ? a : b, cond known
ResponseKind mux(ResponseArgs& a) noexcept;       // in[pick(index)], index known
ResponseKind demux(ResponseArgs& a) noexcept;     // x to out[pick(index)], index known
ResponseKind identity(ResponseArgs& a) noexcept;  // every input to every output, gain 1 (channel, pack: the evaluator
                                                  // maps their channels)
ResponseKind z1(ResponseArgs& a) noexcept;        // z
ResponseKind delayLinear(ResponseArgs& a) noexcept;   // z^n ((1 − f) + f z), the kernel's clamps
ResponseKind delayCubic(ResponseArgs& a) noexcept;    // the 4-point Hermite taps
ResponseKind delayAllpass(ResponseArgs& a) noexcept;  // z^n (c + z) / (1 + c z)
ResponseKind gain(ResponseArgs& a) noexcept;      // x · g, settled
ResponseKind gainAdd(ResponseArgs& a) noexcept;   // acc + x · g, settled
// Nonlinear atoms with a slope at 0 (Approximate).
ResponseKind slopeOne(ResponseArgs& a) noexcept;  // tanh, fast_tanh, sin, fast_sin: slope 1 at 0
ResponseKind clip(ResponseArgs& a) noexcept;      // clamp(x, lo, hi): 1 when lo < 0 < hi, else 0

}  // namespace pq::graph::rules
