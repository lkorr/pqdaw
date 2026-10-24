// Shared Newton pieces (pq/engine/newton.h): the exact helper functions, the tangent-rule tables and the plan.

#include "pq/engine/newton.h"

#include <bit>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace pq::engine::newton {

namespace g = pq::graph;
using dsp::Atom;

// Out of line on purpose (newton.h): one compiled copy for the interpreter and the generated code.
__declspec(noinline) float cosExact(float x) noexcept { return std::cos(x); }  // RT-safe
__declspec(noinline) float logExact(float x) noexcept { return std::log(x); }  // RT-safe

void linearUpdate(float* u, const float* J, const float* r0, double* M, uint32_t n, uint32_t lanes) noexcept {  // RT-safe
  const uint32_t w = n + 1;
  for (uint32_t l = 0; l < lanes; ++l) {
    for (uint32_t i = 0; i < n; ++i) {
      for (uint32_t j = 0; j < n; ++j) M[i * w + j] = J[(i * n + j) * 8 + l];
      M[i * w + n] = r0[i * 8 + l];
    }
    bool ok = true;
    for (uint32_t c = 0; c < n && ok; ++c) {
      uint32_t piv = c;
      for (uint32_t i = c + 1; i < n; ++i)
        if (std::fabs(M[i * w + c]) > std::fabs(M[piv * w + c])) piv = i;
      if (!(std::fabs(M[piv * w + c]) > 0.0)) {
        ok = false;
        break;
      }
      if (piv != c)
        for (uint32_t j = 0; j < w; ++j) std::swap(M[c * w + j], M[piv * w + j]);
      for (uint32_t i = c + 1; i < n; ++i) {
        const double f = M[i * w + c] / M[c * w + c];
        for (uint32_t j = c; j < w; ++j) M[i * w + j] -= f * M[c * w + j];
      }
    }
    if (!ok) continue;
    for (uint32_t i = n; i-- > 0;) {
      double v = M[i * w + n];
      for (uint32_t j = i + 1; j < n; ++j) v -= M[i * w + j] * M[j * w + n];
      M[i * w + n] = v / M[i * w + i];
    }
    for (uint32_t i = 0; i < n; ++i) {
      const double nu = static_cast<double>(u[i * 8 + l]) - M[i * w + n];
      if (!std::isfinite(nu) || !std::isfinite(static_cast<float>(nu))) ok = false;
    }
    if (!ok) continue;
    for (uint32_t i = 0; i < n; ++i) u[i * 8 + l] = static_cast<float>(static_cast<double>(u[i * 8 + l]) - M[i * w + n]);
  }
}

float ScalarOps::floor(float a) noexcept { return std::floor(a); }      // RT-safe
float ScalarOps::round(float a) noexcept { return std::nearbyint(a); }  // RT-safe (default rounding: ties to even)
float ScalarOps::sel(float m, float a, float b) noexcept {              // RT-safe
  return (std::bit_cast<uint32_t>(m) >> 31) != 0 ? a : b;
}
float ScalarOps::mask(bool b) noexcept { return std::bit_cast<float>(b ? 0xFFFFFFFFu : 0u); }  // RT-safe

namespace {

template <Atom A>
void tangentStep(const TangentStep& g) noexcept {  // RT-safe
  ScalarOps o;
  Tan<float> dl[4], yl[4];
  Tan<float>* d = g.numIn <= 4 ? dl : g.dScratch;
  Tan<float>* dy = g.numOut <= 4 ? yl : g.dyScratch;
  for (uint32_t j = 0; j < g.n; ++j) {
    if (((g.outAny >> j) & 1u) == 0) continue;
    for (uint32_t l = 0; l < g.lanes; ++l) {
      const float a[3] = {g.a[l], g.a[8 + l], g.a[16 + l]};
      for (uint32_t i = 0; i < g.numIn; ++i) {  // field by field (a whole-struct temporary is packed through memory)
        const uint32_t on = (g.in[i].nz >> j) & 1u;
        d[i].on = on;
        d[i].v = on != 0 ? g.T[(g.in[i].slot * g.n + j) * 8 + l] : 0.0f;
      }
      tangentOp(o, A, g.numIn, g.numOut, a, g.y + l, d, dy);
      for (uint32_t k = 0; k < g.numOut; ++k)
        if (((g.out[k].nz >> j) & 1u) != 0) g.T[(g.out[k].slot * g.n + j) * 8 + l] = dy[k].v;
    }
  }
}

}  // namespace

TangentStepFn scalarTangentStep(Atom a) noexcept {
  switch (a) {
#define PQ_TAN(X)   case Atom::X: return &tangentStep<Atom::X>;
    PQ_TAN(Add) PQ_TAN(Sub) PQ_TAN(Mul) PQ_TAN(Div) PQ_TAN(Min) PQ_TAN(Max) PQ_TAN(Mod) PQ_TAN(Pow) PQ_TAN(PowFast)
    PQ_TAN(CentsToHz) PQ_TAN(CentsToHzFast) PQ_TAN(HzToCents) PQ_TAN(HzToCentsFast) PQ_TAN(Neg) PQ_TAN(Abs) PQ_TAN(Sign)
    PQ_TAN(Floor) PQ_TAN(Frac) PQ_TAN(Sqrt) PQ_TAN(SqrtFast) PQ_TAN(Exp) PQ_TAN(ExpFast) PQ_TAN(Log) PQ_TAN(LogFast)
    PQ_TAN(Sin) PQ_TAN(SinFast) PQ_TAN(Tan) PQ_TAN(TanFast) PQ_TAN(Tanh) PQ_TAN(TanhFast) PQ_TAN(DbToGain)
    PQ_TAN(DbToGainFast) PQ_TAN(GainToDb) PQ_TAN(GainToDbFast) PQ_TAN(Const) PQ_TAN(Clip) PQ_TAN(Lerp) PQ_TAN(Select)
    PQ_TAN(MulAdd) PQ_TAN(And) PQ_TAN(Or) PQ_TAN(Xor) PQ_TAN(Nand) PQ_TAN(Nor) PQ_TAN(Not) PQ_TAN(Gt) PQ_TAN(Lt)
    PQ_TAN(Ge) PQ_TAN(Le) PQ_TAN(Eq) PQ_TAN(Ne) PQ_TAN(GateToReal) PQ_TAN(Mux) PQ_TAN(Demux) PQ_TAN(SinePhase)
#undef PQ_TAN
    default: return nullptr;
  }
}

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

bool bodyReadAfter(const g::Schedule& s, const g::SolveBlock& sb) {
  std::unordered_map<uint32_t, bool> written;
  const uint32_t end = sb.firstOp + sb.numOps;
  for (uint32_t i = sb.firstOp; i < end && i < s.ops.size(); ++i)
    for (const g::Operand& o : s.outputsOf(s.ops[i]))
      if (o.kind == g::Operand::Kind::Buffer) written[o.index] = true;
  auto hit = [&](const g::Operand& o) { return o.kind == g::Operand::Kind::Buffer && written.count(o.index) != 0; };
  // In op order after the body: a read of a body-written buffer is a reader; a write first replaces the value
  // (buffer reuse), so later reads of that buffer read something else. Outputs and other blocks' operands are read
  // at unknown points: any body-written buffer still holding its body value counts.
  for (uint32_t i = end; i < s.ops.size(); ++i) {
    for (const g::Operand& o : s.inputs(s.ops[i]))
      if (hit(o)) return true;
    for (const g::Operand& o : s.outputsOf(s.ops[i]))
      if (o.kind == g::Operand::Kind::Buffer) written.erase(o.index);
  }
  for (const g::OutputRef& o : s.outputs)
    if (hit(o.value)) return true;
  const g::SolveBlock* self = &sb;
  for (const g::SolveBlock& other : s.solves) {
    if (&other == self || (other.firstOp == sb.firstOp && other.firstUnknown == sb.firstUnknown)) continue;
    for (uint32_t k = 0; k < other.numUnknowns; ++k)
      if (hit(s.unknowns[other.firstUnknown + k].init)) return true;
    for (uint32_t k = 0; k < other.numEquations; ++k)
      if (hit(s.equations[other.firstEquation + k].lhs) || hit(s.equations[other.firstEquation + k].rhs)) return true;
  }
  return false;
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
