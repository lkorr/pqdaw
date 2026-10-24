// Shared Newton pieces (pq/engine/newton.h): the exact helper functions, the tangent-rule tables and the plan.

#include "pq/engine/newton.h"

#include <bit>
#include <cmath>
#include <unordered_map>

namespace pq::engine::newton {

namespace g = pq::graph;
using dsp::Atom;

// Out of line on purpose (newton.h): one compiled copy for the interpreter and the generated code.
__declspec(noinline) float cosExact(float x) noexcept { return std::cos(x); }  // RT-safe
__declspec(noinline) float logExact(float x) noexcept { return std::log(x); }  // RT-safe

float ScalarOps::floor(float a) noexcept { return std::floor(a); }      // RT-safe
float ScalarOps::round(float a) noexcept { return std::nearbyint(a); }  // RT-safe (default rounding: ties to even)
float ScalarOps::sel(float m, float a, float b) noexcept {              // RT-safe
  return (std::bit_cast<uint32_t>(m) >> 31) != 0 ? a : b;
}
float ScalarOps::mask(bool b) noexcept { return std::bit_cast<float>(b ? 0xFFFFFFFFu : 0u); }  // RT-safe

bool hasTangent(Atom a) noexcept {
  switch (a) {
    case Atom::Add: case Atom::Sub: case Atom::Mul: case Atom::Div: case Atom::Min: case Atom::Max: case Atom::Mod:
    case Atom::Pow: case Atom::PowFast: case Atom::CentsToHz: case Atom::CentsToHzFast: case Atom::HzToCents:
    case Atom::HzToCentsFast: case Atom::Neg: case Atom::Abs: case Atom::Sign: case Atom::Floor: case Atom::Frac:
    case Atom::Sqrt: case Atom::SqrtFast: case Atom::Exp: case Atom::ExpFast: case Atom::Log: case Atom::LogFast:
    case Atom::Sin: case Atom::SinFast: case Atom::Tan: case Atom::TanFast: case Atom::Tanh: case Atom::TanhFast:
    case Atom::DbToGain: case Atom::DbToGainFast: case Atom::GainToDb: case Atom::GainToDbFast: case Atom::Const:
    case Atom::Clip: case Atom::Lerp: case Atom::Select: case Atom::MulAdd: case Atom::And: case Atom::Or:
    case Atom::Xor: case Atom::Nand: case Atom::Nor: case Atom::Not: case Atom::Gt: case Atom::Lt: case Atom::Ge:
    case Atom::Le: case Atom::Eq: case Atom::Ne: case Atom::GateToReal: case Atom::Mux: case Atom::Demux:
    case Atom::SinePhase:
      return true;
    default:
      return false;
  }
}

bool differentiableInput(Atom a, uint32_t k) noexcept {
  switch (a) {
    case Atom::Sign: case Atom::Floor: case Atom::And: case Atom::Or: case Atom::Xor: case Atom::Nand: case Atom::Nor:
    case Atom::Not: case Atom::Gt: case Atom::Lt: case Atom::Ge: case Atom::Le: case Atom::Eq: case Atom::Ne:
    case Atom::GateToReal:
      return false;
    case Atom::Select: case Atom::Mux:
      return k >= 1;  // not the mask / index
    case Atom::Demux:
      return k == 1;
    default:
      return hasTangent(a);
  }
}

TangentPlan planTangents(const g::Schedule& s, const g::SolveBlock& sb) {
  TangentPlan p;
  p.n = sb.numUnknowns;
  p.ad = p.n >= 1 && p.n <= kMaxAdUnknowns;
  std::unordered_map<uint32_t, uint32_t> nz;  // buffer → direction bits of its current value
  for (uint32_t j = 0; j < sb.numUnknowns; ++j) {
    const g::Operand& v = s.unknowns[sb.firstUnknown + j].value;
    if (v.kind == g::Operand::Kind::Buffer) nz[v.index] |= 1u << (j & 31u);
  }
  auto bits = [&](const g::Operand& o) -> uint32_t {
    if (o.kind != g::Operand::Kind::Buffer) return 0;
    const auto it = nz.find(o.index);
    return it == nz.end() ? 0u : it->second;
  };
  for (uint32_t i = sb.firstOp; i < sb.firstOp + sb.numOps && i < s.ops.size(); ++i) {
    const g::Op& op = s.ops[i];
    const auto atom = static_cast<Atom>(op.kernel);
    if (op.kind != g::OpKind::Kernel || !hasTangent(atom)) p.ad = false;
    p.firstIn.push_back(static_cast<uint32_t>(p.inNz.size()));
    p.firstOut.push_back(static_cast<uint32_t>(p.outNz.size()));
    uint32_t out = 0;
    const auto ins = s.inputs(op);
    for (uint32_t k = 0; k < ins.size(); ++k) {
      const uint32_t b = bits(ins[k]);
      p.inNz.push_back(b);
      if (op.kind == g::OpKind::Kernel && differentiableInput(atom, k)) out |= b;
    }
    for (const g::Operand& o : s.outputsOf(op)) {
      p.outNz.push_back(out);
      if (o.kind == g::Operand::Kind::Buffer) nz[o.index] = out;
    }
  }
  for (uint32_t q = 0; q < sb.numEquations; ++q) {
    const g::SolveEquation& e = s.equations[sb.firstEquation + q];
    p.lhsNz.push_back(bits(e.lhs));
    p.rhsNz.push_back(bits(e.rhs));
  }
  return p;
}

}  // namespace pq::engine::newton
