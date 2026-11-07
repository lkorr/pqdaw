// curve (GU.1): a driven control's curve — knob position → value — read from its CurveSlot (pq/dsp/curve.h has the
// definition, the coefficient form and the double evaluator this agrees with; pq/dsp/atoms.h the atom's contract).
//
// Element-wise and stateless, so one loop serves both lane counts (frames · L floats, 8 at a time, the remainder
// scalar: S1 and S8 are the same bits per element). One acquire load of the slot per call, then the call's
// specialisation by the data's class: one segment (no search) or several (a compare per segment boundary, the
// coefficients gathered per lane), and the shape — Linear without a transcendental, one exact expm1 / log1p / pow
// otherwise (one exp for a single Exp segment through zero, the Octave preset); mixed shapes compute each shape present
// and blend. The flags are per-call branches outside the
// arithmetic (uniform over the call).

#include <algorithm>

#include "kernels_common.h"
#include "pq/dsp/curve.h"

namespace pq::dsp::detail {
namespace {

constexpr uint32_t kMixed = kCurveShapeCount;
constexpr uint32_t kPureExp = kCurveShapeCount + 1;  // one Exp segment through zero: a · e^(c·x) (CurveData::pureExp)

template <class S>
struct Coeffs {
  typename S::F t, a, b, c, e, f, shape;
};

// x = p − t0, the position inside the segment (curve.h "Evaluation").
template <class S, CurveShape Sh>
typename S::F shapeValue(const Coeffs<S>& k, typename S::F x) noexcept {
  using F = typename S::F;
  if constexpr (Sh == CurveShape::Linear) {
    return S::fma(k.b, x, k.a);
  } else if constexpr (Sh == CurveShape::Exp) {
    return S::fma(k.b, S::exactExpm1(S::mul(k.c, x)), k.a);
  } else if constexpr (Sh == CurveShape::Log) {
    return S::fma(k.b, S::exactLog1p(S::mul(k.c, x)), k.a);
  } else if constexpr (Sh == CurveShape::Power) {
    const F u = S::min(S::max(S::mul(k.c, x), S::zero()), S::set(1.0f));
    return S::fma(k.b, S::exactPow(u, k.e), k.a);
  } else {
    const F one = S::set(1.0f);
    const F u = S::min(S::max(S::mul(k.c, x), S::zero()), one);
    const F h = S::mul(k.f, S::exactExpm1(S::mul(k.e, S::min(u, S::sub(one, u)))));
    const F g = S::select(S::lt(u, S::set(0.5f)), h, S::sub(one, h));
    return S::fma(k.b, g, k.a);
  }
}

template <class S, uint32_t kShape>
typename S::F segValue(const Coeffs<S>& k, typename S::F p, uint32_t shapes) noexcept {
  using F = typename S::F;
  if constexpr (kShape == kPureExp) {
    return S::mul(k.a, S::exactExp(S::mul(k.c, p)));
  } else if constexpr (kShape != kMixed) {
    return shapeValue<S, static_cast<CurveShape>(kShape)>(k, p);
  } else {
    F y = S::zero();
    auto blend = [&](CurveShape sh, F v) { y = S::select(S::eq(k.shape, S::set(static_cast<float>(sh))), v, y); };
    if (shapes & (1u << static_cast<uint32_t>(CurveShape::Linear))) blend(CurveShape::Linear, shapeValue<S, CurveShape::Linear>(k, p));
    if (shapes & (1u << static_cast<uint32_t>(CurveShape::Exp))) blend(CurveShape::Exp, shapeValue<S, CurveShape::Exp>(k, p));
    if (shapes & (1u << static_cast<uint32_t>(CurveShape::Log))) blend(CurveShape::Log, shapeValue<S, CurveShape::Log>(k, p));
    if (shapes & (1u << static_cast<uint32_t>(CurveShape::Power))) blend(CurveShape::Power, shapeValue<S, CurveShape::Power>(k, p));
    if (shapes & (1u << static_cast<uint32_t>(CurveShape::SCurve))) blend(CurveShape::SCurve, shapeValue<S, CurveShape::SCurve>(k, p));
    return y;
  }
}

// One element group (S: 8 floats or 1): position x → value.
template <class S, bool kMulti, uint32_t kShape>
typename S::F evalOne(const CurveData& D, typename S::F x, bool convert) noexcept {
  using F = typename S::F;
  const F zero = S::zero(), one = S::set(1.0f);
  F p, sign = one;
  switch (static_cast<CurveMode>(D.mode)) {
    case CurveMode::Wrap:
      p = S::sub(x, S::floor(x));
      p = S::max(S::min(p, S::set(0.99999994f)), zero);  // frac (the atom's clamp below 1); NaN → 0
      break;
    case CurveMode::Signed:
      sign = S::select(S::gt(x, zero), one, S::select(S::lt(x, zero), S::set(-1.0f), zero));
      p = S::max(S::min(S::abs(x), one), zero);
      break;
    case CurveMode::Clip:
    default: p = S::min(S::max(x, zero), one); break;  // max(NaN, 0) = 0
  }
  Coeffs<S> k;
  if constexpr (kMulti) {
    typename S::I idx = S::seti(0);
    for (uint32_t i = 1; i < D.segments; ++i) idx = S::subi(idx, S::asI(S::ge(p, S::set(D.t[i]))));
    k.t = S::gather(D.t, idx);
    k.a = S::gather(D.a, idx);
    k.b = S::gather(D.b, idx);
    if constexpr (kShape != static_cast<uint32_t>(CurveShape::Linear)) k.c = S::gather(D.c, idx);
    if constexpr (kShape == kMixed || kShape == static_cast<uint32_t>(CurveShape::Power) ||
                  kShape == static_cast<uint32_t>(CurveShape::SCurve)) {
      k.e = S::gather(D.e, idx);
      k.f = S::gather(D.f, idx);
    }
    if constexpr (kShape == kMixed) k.shape = S::gather(D.shape, idx);
  } else {
    k.a = S::set(D.a[0]);
    k.b = S::set(D.b[0]);
    k.c = S::set(D.c[0]);
    k.e = S::set(D.e[0]);
    k.f = S::set(D.f[0]);
    k.shape = S::set(D.shape[0]);
  }
  F y;
  if constexpr (kMulti) y = segValue<S, kShape>(k, S::sub(p, k.t), D.shapes);
  else y = segValue<S, kShape>(k, p, D.shapes);  // t[0] = 0
  if (D.mode == static_cast<uint32_t>(CurveMode::Signed)) y = S::mul(y, sign);
  if (D.step > 0.0f) y = S::mul(S::floor(S::fma(y, S::set(D.invStep), S::set(0.5f))), S::set(D.step));
  if (convert && D.output == static_cast<uint32_t>(CurveOutput::DbToGain)) y = S::exactDbToGain(y);
  return y;
}

template <bool kSpl, bool kMulti, uint32_t kShape>
void run(const KernelArgs& a, uint32_t count, const CurveData& D, bool convert) noexcept {  // RT-safe
  float* out = a.out[0];
  const uint32_t vecEnd = count & ~7u;
  const In<S8, kSpl> x(a.in[0]);
  for (uint32_t i = 0; i < vecEnd; i += 8) S8::store(out + i, evalOne<S8, kMulti, kShape>(D, x.at(i >> 3), convert));
  for (uint32_t i = vecEnd; i < count; ++i) {
    const float* px = kSpl ? a.in[0] : a.in[0] + i;
    S1::store(out + i, evalOne<S1, kMulti, kShape>(D, S1::load(px), convert));
  }
}

template <bool kSpl, bool kMulti>
void runShape(const KernelArgs& a, uint32_t count, const CurveData& D, bool convert) noexcept {  // RT-safe
  if constexpr (!kMulti)
    if (D.pureExp != 0) return run<kSpl, false, kPureExp>(a, count, D, convert);
  switch (D.single) {
    case static_cast<uint32_t>(CurveShape::Linear): return run<kSpl, kMulti, 0>(a, count, D, convert);
    case static_cast<uint32_t>(CurveShape::Exp): return run<kSpl, kMulti, 1>(a, count, D, convert);
    case static_cast<uint32_t>(CurveShape::Log): return run<kSpl, kMulti, 2>(a, count, D, convert);
    case static_cast<uint32_t>(CurveShape::Power): return run<kSpl, kMulti, 3>(a, count, D, convert);
    case static_cast<uint32_t>(CurveShape::SCurve): return run<kSpl, kMulti, 4>(a, count, D, convert);
    default: return run<kSpl, kMulti, kMixed>(a, count, D, convert);
  }
}

template <uint32_t L, bool kSpl>
void curveKernel(const KernelArgs& a) noexcept {  // RT-safe
  const uint32_t count = a.frames * L;
  if (count == 0) return;
  const auto* slot = static_cast<const CurveSlot*>(a.resource);
  const CurveData* D = slot != nullptr ? slot->acquire() : nullptr;
  if (D == nullptr) {  // no slot (a lowering without the link's curve slots): the identity on the position
    for (uint32_t i = 0; i < count; ++i) a.out[0][i] = a.in[0][kSpl ? i % kPatternFloats : i];
    return;
  }
  const bool convert = a.in[1][0] != 0.0f;  // a const
  if (D->segments > 1) runShape<kSpl, true>(a, count, *D, convert);
  else runShape<kSpl, false>(a, count, *D, convert);
}

}  // namespace

KernelFn findCurveKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept {
  if (a != Atom::Curve || specMask > 3u) return nullptr;
  const bool spl = (specMask & 1u) != 0;  // x; `convert` is read from its first float either way
  if (lanes == 1) return spl ? &curveKernel<1, true> : &curveKernel<1, false>;
  if (lanes == kGroupLanes) return spl ? &curveKernel<kGroupLanes, true> : &curveKernel<kGroupLanes, false>;
  return nullptr;
}

}  // namespace pq::dsp::detail
