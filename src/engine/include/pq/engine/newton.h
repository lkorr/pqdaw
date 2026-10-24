#pragma once

// Newton for `solve` blocks, shared by the interpreter (interpreter.cpp) and the JIT (src/jit/src/codegen.cpp), so
// both backends compute the same Jacobian bit for bit (T1b.1-fix1; QUESTIONS Q53).
//
// Per frame (Loop region) or once per block (Control region), per lane, `iterations` times:
//   1. write the guess into the unknowns' buffers and run the body once, carrying forward-mode tangents
//      (dual numbers) along: one tangent per unknown (direction j = ∂/∂u_j), seeded 1 at u_j. Each body op's tangent
//      comes from its value and its inputs' values and tangents (tangentOp below). A tangent that is zero by
//      construction (the op does not depend on u_j) is never computed (TangentPlan::...Nz bits), in both backends.
//   2. residual_q = lhs_q − rhs_q and J_qj = tangent(lhs_q)_j − tangent(rhs_q)_j: the exact Jacobian of the float
//      body (up to rounding of the tangent arithmetic), for 1 body evaluation per iteration instead of 1 + 2n.
//   3. If some lane's J is not finite (a derivative singularity: log/sqrt at 0, pow at 0, …), the Jacobian of that
//      lane comes from finite differences instead (below); the other lanes keep the AD Jacobian.
//   4. J·Δ = residual per lane (n = 1: a double division; n > 1: Gaussian elimination with partial pivoting in
//      double); u −= Δ when every component is finite (else the lane keeps its value for this iteration).
// After the last iteration the body runs once more (values read after the block match the solution).
//
// Finite differences (bodies with an op that has no tangent rule — a wavetable or shaper read — or more than
// kMaxAdUnknowns unknowns, and the step-3 fallback): per unknown j and lane, h = 2^-8 · max(|u_j|, 1e-6) (relative
// to the unknown, so unknowns ≪ 1 — currents, charges — get a step that stays in their domain; the old floor of 1
// left log's domain for u = 0.001, M1 review M2), the body evaluated at u_j ± h, J_qj = (r⁺ − r⁻) / ((u + h) −
// (u − h)) with the step actually taken in float. Where r⁺ or r⁻ is not finite the one-sided difference with the
// finite side is used ((r⁺ − r⁰) / ((u + h) − u) or (r⁰ − r⁻) / (u − (u − h))).
//
// Not RT code except cosExact/logExact and tangentOp's scalar instantiation (called per lane by the interpreter's
// audio-thread solver); planTangents runs when an image is built.

#include <cstdint>
#include <vector>

#include "pq/dsp/atoms.h"
#include "pq/graph/schedule.h"

namespace pq::engine::newton {

inline constexpr float kStep = 0.00390625f;   // 2^-8: finite-difference step relative to max(|u|, kStepFloor)
inline constexpr float kStepFloor = 1e-6f;
inline constexpr uint32_t kMaxAdUnknowns = 32;  // tangent directions are a uint32 bit set

// The exact cos / natural log the tangent rules use (sin' and pow's ∂/∂exponent), compiled once out of line: the
// interpreter calls them per lane and the generated code calls these very functions through its pointer table, so
// the two backends cannot differ by a vectorised or inlined libm variant. RT-safe.
float cosExact(float x) noexcept;
float logExact(float x) noexcept;

// Whether atom `a` has a tangent rule, and whether its input k carries a derivative (select's mask, mux's index,
// comparisons … do not). Every element-wise atom has a rule; stateful atoms cannot be in a solve body (the
// scheduler rejects them) and table reads (wavetable, shaper) have none (finite differences).
bool hasTangent(dsp::Atom a) noexcept;
bool differentiableInput(dsp::Atom a, uint32_t k) noexcept;

// Which tangents of a solve block are non-zero, per direction (bit j: depends on unknown j). Built once per block
// from the Schedule, in body order, keyed by buffer the way the body's values are (a buffer written in place carries
// its new value's tangent from then on). Both backends skip exactly the zero terms this plan names.
struct TangentPlan {
  bool ad = false;                    // every body op is a kernel with a tangent rule and n ≤ kMaxAdUnknowns
  uint32_t n = 0;
  std::vector<uint32_t> firstIn, firstOut;  // per body op: into inNz / outNz
  std::vector<uint32_t> inNz, outNz;        // per body op input / output: direction bits (0 for non-buffers)
  std::vector<uint32_t> lhsNz, rhsNz;       // per equation
};
TangentPlan planTangents(const graph::Schedule& s, const graph::SolveBlock& sb);

// A tangent: `on` false means zero by construction (the value is not used).
template <class F>
struct Tan {
  F v{};
  bool on = false;
};

// The tangent rules, written once for both backends. `O` supplies the arithmetic: the interpreter's scalar floats
// (ScalarOps below) or the JIT's IR builder (codegen.cpp), so the order of every operation is the same and no
// fast-math is involved. Inputs: a[0..2] (only the first three input values are ever read), y[0] the op's value,
// d[k] the input tangents (d[k].on only where differentiableInput), dy[0..numOut) the result. Every rule is the
// derivative of the kernel's own formula (pq/dsp/atoms.h): min/max/abs/clip/select/mux follow the branch the value
// took (same comparisons, same NaN order); floor, sign, comparisons and logic have zero derivative; frac's clamp
// below 1 and mod's discontinuities are ignored (their one-sided derivative is the rule).
template <class O>
void tangentOp(O& o, dsp::Atom atom, uint32_t numIn, uint32_t numOut, const typename O::F* a, const typename O::F* y,
               const Tan<typename O::F>* d, Tan<typename O::F>* dy) {
  using F = typename O::F;
  using T = Tan<F>;
  using dsp::Atom;
  const T off{};
  auto on = [](F v) { return T{v, true}; };
  auto tadd = [&](const T& x, const T& z) -> T {
    if (!x.on) return z;
    if (!z.on) return x;
    return on(o.add(x.v, z.v));
  };
  auto tsub = [&](const T& x, const T& z) -> T {
    if (!z.on) return x;
    if (!x.on) return on(o.neg(z.v));
    return on(o.sub(x.v, z.v));
  };
  auto tneg = [&](const T& x) -> T { return x.on ? on(o.neg(x.v)) : off; };
  auto tmul = [&](const T& x, F s) -> T { return x.on ? on(o.mul(x.v, s)) : off; };
  auto tdiv = [&](const T& x, F s) -> T { return x.on ? on(o.div(x.v, s)) : off; };
  auto tsel = [&](F m, const T& x, const T& z) -> T {  // m ? x : z by the mask's sign bit
    if (!x.on && !z.on) return off;
    return on(o.sel(m, x.on ? x.v : o.c(0.0f), z.on ? z.v : o.c(0.0f)));
  };
  constexpr float kLn2Over1200 = 5.77622650466621091e-4f;   // ln 2 / 1200
  constexpr float k1200OverLn2 = 1731.23404906675624f;      // 1200 / ln 2
  constexpr float kLn10Over20 = 0.115129254649702284f;      // ln 10 / 20
  constexpr float k20OverLn10 = 8.68588963806503655f;       // 20 / ln 10
  constexpr float kTwoPi = 6.28318530717958648f;
  for (uint32_t k = 0; k < numOut; ++k) dy[k] = off;
  switch (atom) {
    case Atom::Add: dy[0] = tadd(d[0], d[1]); return;
    case Atom::Sub: dy[0] = tsub(d[0], d[1]); return;
    case Atom::Mul: dy[0] = tadd(tmul(d[0], a[1]), tmul(d[1], a[0])); return;
    case Atom::Div: dy[0] = tdiv(tsub(d[0], tmul(d[1], y[0])), a[1]); return;  // (da − y·db) / b
    case Atom::Min: dy[0] = tsel(o.lt(a[0], a[1]), d[0], d[1]); return;
    case Atom::Max: dy[0] = tsel(o.gt(a[0], a[1]), d[0], d[1]); return;
    case Atom::Mod: dy[0] = tsub(d[0], tmul(d[1], o.floor(o.div(a[0], a[1])))); return;  // a − b·floor(a/b)
    case Atom::Pow:
    case Atom::PowFast: {  // y = a^b: y·b·da/a + y·ln(a)·db
      const T ta = tmul(tdiv(tmul(d[0], a[1]), a[0]), y[0]);
      const T tb = d[1].on ? tmul(d[1], o.mul(y[0], o.logE(a[0]))) : off;
      dy[0] = tadd(ta, tb);
      return;
    }
    case Atom::CentsToHz:
    case Atom::CentsToHzFast:  // y = ref·2^(c/1200)
      dy[0] = tadd(tmul(d[0], o.mul(y[0], o.c(kLn2Over1200))), tmul(d[1], o.div(y[0], a[1])));
      return;
    case Atom::HzToCents:
    case Atom::HzToCentsFast:  // y = 1200·log2(hz/ref)
      dy[0] = tmul(tsub(tdiv(d[0], a[0]), tdiv(d[1], a[1])), o.c(k1200OverLn2));
      return;
    case Atom::Neg: dy[0] = tneg(d[0]); return;
    case Atom::Abs: dy[0] = tsel(o.lt(a[0], o.c(0.0f)), tneg(d[0]), d[0]); return;
    case Atom::Frac:
    case Atom::Const: dy[0] = d[0]; return;
    case Atom::Sqrt:
    case Atom::SqrtFast: dy[0] = tdiv(d[0], o.add(y[0], y[0])); return;
    case Atom::Exp:
    case Atom::ExpFast: dy[0] = tmul(d[0], y[0]); return;
    case Atom::Log:
    case Atom::LogFast: dy[0] = tdiv(d[0], a[0]); return;
    case Atom::Sin:
    case Atom::SinFast: dy[0] = d[0].on ? tmul(d[0], o.cosE(a[0])) : off; return;
    case Atom::Tan:
    case Atom::TanFast: dy[0] = d[0].on ? tmul(d[0], o.add(o.c(1.0f), o.mul(y[0], y[0]))) : off; return;
    case Atom::Tanh:
    case Atom::TanhFast: dy[0] = d[0].on ? tmul(d[0], o.sub(o.c(1.0f), o.mul(y[0], y[0]))) : off; return;
    case Atom::DbToGain:
    case Atom::DbToGainFast: dy[0] = d[0].on ? tmul(d[0], o.mul(y[0], o.c(kLn10Over20))) : off; return;
    case Atom::GainToDb:
    case Atom::GainToDbFast: dy[0] = tdiv(tmul(d[0], o.c(k20OverLn10)), a[0]); return;
    case Atom::SinePhase:  // y = sin(2π·(x − round(x)))
      dy[0] = d[0].on ? tmul(d[0], o.mul(o.cosE(o.mul(o.sub(a[0], o.round(a[0])), o.c(kTwoPi))), o.c(kTwoPi))) : off;
      return;
    case Atom::Clip: {  // min(max(x, lo), hi)
      const F m = o.gt(a[0], a[1]);
      const F t = o.sel(m, a[0], a[1]);
      dy[0] = tsel(o.lt(t, a[2]), tsel(m, d[0], d[1]), d[2]);
      return;
    }
    case Atom::Lerp:  // a + t·(b − a)
      dy[0] = tadd(tadd(d[0], tmul(d[2], o.sub(a[1], a[0]))), tmul(tsub(d[1], d[0]), a[2]));
      return;
    case Atom::Select: dy[0] = tsel(a[0], d[1], d[2]); return;
    case Atom::MulAdd: dy[0] = tadd(tadd(tmul(d[0], a[1]), tmul(d[1], a[0])), d[2]); return;
    case Atom::Mux: {  // the kernel's select chain: index ≥ k picks input k
      T r = d[1];
      for (uint32_t k = 1; k + 1 < numIn; ++k) r = tsel(o.ge(a[0], o.c(static_cast<float>(k))), d[k + 1], r);
      dy[0] = r;
      return;
    }
    case Atom::Demux: {  // output k = x where clamp(floor(index), 0, n − 1) == k
      if (!d[1].on) return;
      const F mx = o.sel(o.gt(o.floor(a[0]), o.c(0.0f)), o.floor(a[0]), o.c(0.0f));
      const F nm1 = o.c(static_cast<float>(numOut - 1));
      const F c = o.sel(o.lt(mx, nm1), mx, nm1);
      for (uint32_t k = 0; k < numOut; ++k) dy[k] = tsel(o.eq(c, o.c(static_cast<float>(k))), d[1], off);
      return;
    }
    default: return;  // sign, floor, logic, comparisons, gate_to_real: zero
  }
}

// The interpreter's per-lane arithmetic for tangentOp: plain IEEE float (MSVC /fp:precise, no contraction), masks as
// in pq/dsp (all ones / zero), selects by the sign bit like the JIT's and the kernels' blendv.
struct ScalarOps {
  using F = float;
  static F c(float x) noexcept { return x; }
  static F add(F a, F b) noexcept { return a + b; }
  static F sub(F a, F b) noexcept { return a - b; }
  static F mul(F a, F b) noexcept { return a * b; }
  static F div(F a, F b) noexcept { return a / b; }
  static F neg(F a) noexcept { return -a; }
  static F floor(F a) noexcept;
  static F round(F a) noexcept;  // nearest, ties to even
  static F lt(F a, F b) noexcept { return mask(a < b); }
  static F gt(F a, F b) noexcept { return mask(a > b); }
  static F ge(F a, F b) noexcept { return mask(a >= b); }
  static F eq(F a, F b) noexcept { return mask(a == b); }
  static F sel(F m, F a, F b) noexcept;
  static F cosE(F x) noexcept { return cosExact(x); }
  static F logE(F x) noexcept { return logExact(x); }
  static F mask(bool b) noexcept;
};

}  // namespace pq::engine::newton
