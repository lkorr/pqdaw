// Math atoms (SPEC §7.2): stateless, element-wise. Semantics are documented in pq/dsp/atoms.h.

#include "kernels_common.h"

namespace pq::dsp::detail {
namespace {

// ln 2 and log2 e as float (the fast variants are built on exp2/log2).
constexpr float kLn2 = 0.693147180559945309f;
constexpr float kLog2e = 1.44269504088896341f;
constexpr float kTwoPi = 6.28318530717958648f;
constexpr float kBelowOne = 0.99999994f;  // nextafter(1, 0)

#define PQ_UNARY(Name, expr)                                                    \
  struct Name {                                                                 \
    static constexpr uint32_t kArity = 1;                                       \
    template <class S>                                                          \
    static typename S::F f(typename S::F x) noexcept { /* RT-safe */ return expr; } \
  };
#define PQ_BINARY(Name, expr)                                                                     \
  struct Name {                                                                                   \
    static constexpr uint32_t kArity = 2;                                                         \
    template <class S>                                                                            \
    static typename S::F f(typename S::F a, typename S::F b) noexcept { /* RT-safe */ return expr; } \
  };
#define PQ_TERNARY(Name, expr)                                                                   \
  struct Name {                                                                                  \
    static constexpr uint32_t kArity = 3;                                                        \
    template <class S>                                                                           \
    static typename S::F f(typename S::F a, typename S::F b, typename S::F c) noexcept { /* RT-safe */ \
      return expr;                                                                               \
    }                                                                                            \
  };

PQ_BINARY(AddOp, S::add(a, b))
PQ_BINARY(SubOp, S::sub(a, b))
PQ_BINARY(MulOp, S::mul(a, b))
PQ_BINARY(DivOp, S::div(a, b))
PQ_BINARY(MinOp, S::min(a, b))
PQ_BINARY(MaxOp, S::max(a, b))
PQ_BINARY(ModOp, S::fnma(b, S::floor(S::div(a, b)), a))
PQ_BINARY(PowOp, S::exactPow(a, b))
PQ_BINARY(PowFastOp, S::fastExp2(S::mul(b, S::fastLog2(a))))
PQ_BINARY(CentsToHzOp, S::exactCentsToHz(a, b))
PQ_BINARY(CentsToHzFastOp, S::mul(b, S::fastExp2(S::mul(a, S::set(1.0f / 1200.0f)))))
PQ_BINARY(HzToCentsOp, S::exactHzToCents(a, b))
PQ_BINARY(HzToCentsFastOp, S::mul(S::set(1200.0f), S::fastLog2(S::div(a, b))))

PQ_UNARY(NegOp, S::neg(x))
PQ_UNARY(AbsOp, S::abs(x))
PQ_UNARY(SignOp, S::sub(S::band(S::gt(x, S::zero()), S::set(1.0f)), S::band(S::lt(x, S::zero()), S::set(1.0f))))
PQ_UNARY(FloorOp, S::floor(x))
PQ_UNARY(FracOp, S::min(S::sub(x, S::floor(x)), S::set(kBelowOne)))
PQ_UNARY(SqrtOp, S::sqrt(x))
PQ_UNARY(ExpOp, S::exactExp(x))
PQ_UNARY(ExpFastOp, S::fastExp2(S::mul(x, S::set(kLog2e))))
PQ_UNARY(LogOp, S::exactLog(x))
PQ_UNARY(LogFastOp, S::mul(S::fastLog2(x), S::set(kLn2)))
PQ_UNARY(SinOp, S::exactSin(x))
PQ_UNARY(SinFastOp, S::fastSin(x))
PQ_UNARY(TanOp, S::exactTan(x))
PQ_UNARY(TanFastOp, S::fastTan(x))
PQ_UNARY(TanhOp, S::exactTanh(x))
PQ_UNARY(TanhFastOp, S::fastTanh(x))
PQ_UNARY(DbToGainOp, S::exactDbToGain(x))
PQ_UNARY(DbToGainFastOp, S::fastExp2(S::mul(x, S::set(0.166096404744368118f))))  // log2(10)/20
PQ_UNARY(GainToDbOp, S::exactGainToDb(x))
PQ_UNARY(GainToDbFastOp, S::mul(S::fastLog2(x), S::set(6.02059991327962390f)))  // 20·log10(2)
PQ_UNARY(ConstOp, x)
// sine(phase): reduce to r in [−0.5, 0.5] cycles first, so the radian argument stays in [−π, π] where
// fast_sin is most accurate, whatever the phase input's range.
PQ_UNARY(SinePhaseOp, S::fastSin(S::mul(S::sub(x, S::round(x)), S::set(kTwoPi))))

PQ_TERNARY(ClipOp, S::min(S::max(a, b), c))
PQ_TERNARY(LerpOp, S::fma(c, S::sub(b, a), a))
PQ_TERNARY(SelectOp, S::select(a, b, c))
PQ_TERNARY(MulAddOp, S::fma(a, b, c))

#undef PQ_UNARY
#undef PQ_BINARY
#undef PQ_TERNARY

}  // namespace

KernelFn findMathKernel(Atom a, uint32_t lanes, uint32_t m) noexcept {
  switch (a) {
    case Atom::Add: return pick<Ew<AddOp>>(lanes, m);
    case Atom::Sub: return pick<Ew<SubOp>>(lanes, m);
    case Atom::Mul: return pick<Ew<MulOp>>(lanes, m);
    case Atom::Div: return pick<Ew<DivOp>>(lanes, m);
    case Atom::Min: return pick<Ew<MinOp>>(lanes, m);
    case Atom::Max: return pick<Ew<MaxOp>>(lanes, m);
    case Atom::Mod: return pick<Ew<ModOp>>(lanes, m);
    case Atom::Pow: return pick<Ew<PowOp>>(lanes, m);
    case Atom::PowFast: return pick<Ew<PowFastOp>>(lanes, m);
    case Atom::CentsToHz: return pick<Ew<CentsToHzOp>>(lanes, m);
    case Atom::CentsToHzFast: return pick<Ew<CentsToHzFastOp>>(lanes, m);
    case Atom::HzToCents: return pick<Ew<HzToCentsOp>>(lanes, m);
    case Atom::HzToCentsFast: return pick<Ew<HzToCentsFastOp>>(lanes, m);
    case Atom::Neg: return pick<Ew<NegOp>>(lanes, m);
    case Atom::Abs: return pick<Ew<AbsOp>>(lanes, m);
    case Atom::Sign: return pick<Ew<SignOp>>(lanes, m);
    case Atom::Floor: return pick<Ew<FloorOp>>(lanes, m);
    case Atom::Frac: return pick<Ew<FracOp>>(lanes, m);
    case Atom::Sqrt: return pick<Ew<SqrtOp>>(lanes, m);
    case Atom::SqrtFast: return pick<Ew<SqrtOp>>(lanes, m);
    case Atom::Exp: return pick<Ew<ExpOp>>(lanes, m);
    case Atom::ExpFast: return pick<Ew<ExpFastOp>>(lanes, m);
    case Atom::Log: return pick<Ew<LogOp>>(lanes, m);
    case Atom::LogFast: return pick<Ew<LogFastOp>>(lanes, m);
    case Atom::Sin: return pick<Ew<SinOp>>(lanes, m);
    case Atom::SinFast: return pick<Ew<SinFastOp>>(lanes, m);
    case Atom::Tan: return pick<Ew<TanOp>>(lanes, m);
    case Atom::TanFast: return pick<Ew<TanFastOp>>(lanes, m);
    case Atom::Tanh: return pick<Ew<TanhOp>>(lanes, m);
    case Atom::TanhFast: return pick<Ew<TanhFastOp>>(lanes, m);
    case Atom::DbToGain: return pick<Ew<DbToGainOp>>(lanes, m);
    case Atom::DbToGainFast: return pick<Ew<DbToGainFastOp>>(lanes, m);
    case Atom::GainToDb: return pick<Ew<GainToDbOp>>(lanes, m);
    case Atom::GainToDbFast: return pick<Ew<GainToDbFastOp>>(lanes, m);
    case Atom::Const: return pick<Ew<ConstOp>>(lanes, m);
    case Atom::SinePhase: return pick<Ew<SinePhaseOp>>(lanes, m);
    case Atom::Clip: return pick<Ew<ClipOp>>(lanes, m);
    case Atom::Lerp: return pick<Ew<LerpOp>>(lanes, m);
    case Atom::Select: return pick<Ew<SelectOp>>(lanes, m);
    case Atom::MulAdd: return pick<Ew<MulAddOp>>(lanes, m);
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail
