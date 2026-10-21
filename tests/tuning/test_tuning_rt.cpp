// Allocation-trap tests for every RT-safe entry point of pq_tuning (CLAUDE.md rule 2): each runs inside an RtScope
// and must not allocate or free. Inputs are built outside the scope.

#include <doctest/doctest.h>

#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/tuning/edo.h"
#include "pq/tuning/js_number.h"
#include "pq/tuning/monzo.h"
#include "pq/tuning/mos.h"
#include "pq/tuning/mos_regions.h"
#include "pq/tuning/mos_tuning.h"
#include "pq/tuning/rational.h"
#include "pq/tuning/tuning.h"

using namespace pq::tuning;
using pq::alloc_trap::countRtViolations;

namespace {
// Volatile sinks so the calls cannot be optimised away.
volatile double gD = 0.0;
volatile float gF = 0.0f;
volatile int64_t gI = 0;
}  // namespace

TEST_CASE("rt: resolveHz / resolveHzDouble for every pitch kind, EDO and MOS") {
  REQUIRE(pq::alloc_trap::installed());
  const Tuning edo = *Tuning::make(*Edo::make(31), kDefaultReferenceHz);
  MosParams p;
  p.generator = 696.0;
  p.n = 7;
  p.rotation = 3;
  p.modmos = {0, 0, 1};
  p.octaveStretch = 1.5;
  const Tuning mos = *Tuning::make(*Mos::make(p), 220.0);
  const uint32_t chord[] = {4, 5, 6, 7};
  ResolveContext ctx;
  ctx.anchorHz = 330.0;
  ctx.harmonic = {55.0, chord, 4, false};
  const PitchId ids[] = {PitchId::fromDegree(5, -1), PitchId::fromRatio(Rational(3, 2)),
                         PitchId::fromRatio(Rational(7, 4), AnchorKind::Note, 3), PitchId::fromCents(-250.0),
                         PitchId::fromHarmonic(0, 2), PitchId::fromHarmonic(0, 9)};
  CHECK(countRtViolations([&] {
          for (const PitchId& id : ids) {
            gF = resolveHz(id, edo, ctx);
            gF = resolveHz(id, mos, ctx);
            const auto a = resolveHzDouble(id, edo, ctx);
            const auto b = resolveHzDouble(id, mos, ctx);
            gD = a.value_or(0.0) + b.value_or(0.0);
          }
          gD = mos.mos()->centsOfDegree(-23);
        }) == 0);
}

TEST_CASE("rt: Rational arithmetic, cents, monzo cents") {
  const Rational a(81, 80), b(-7, 4), big(int64_t{1} << 61, 3);
  const Monzo m = *toMonzo(Rational(81, 80));
  CHECK(countRtViolations([&] {
          gI = mul(a, b).has_value() + mul(big, big).has_value();
          gI = div(a, b).has_value() + div(a, Rational(0)).has_value();
          gI = add(a, b).has_value() + sub(a, b).has_value();
          gI = pow(a, 5).has_value() + pow(b, -3).has_value() + pow(big, 40).has_value();
          gD = cents(a) + cents(Rational(3, 2));
          gD = cents(m);
          gI = static_cast<int64_t>(primeLimitOf(m));
          gI = (a <=> b) == std::strong_ordering::less;
          gI = Rational::tryMake(6, -4).has_value();
        }) == 0);
}

TEST_CASE("rt: EDO helpers") {
  const Edo e31 = *Edo::make(31);
  const Edo bp = *Edo::make(13, Rational(3));
  const Monzo m = *toMonzo(Rational(5, 4));
  CHECK(countRtViolations([&] {
          const Val v = patentVal(e31);
          const Val w = patentVal(bp);
          gI = mapToSteps(v, m).value_or(0) + w.steps[1];
          const EdoRow r = edoRow(-45, 31);
          gI = r.equave + r.degree;
          gD = e31.centsOfStep(17) + e31.log2OfStep(-3);
        }) == 0);
}

TEST_CASE("rt: MOS linear form lookup, rotation and evaluation") {
  const MosLinearTable table(12);  // built off the RT scope: allocates
  std::vector<double> cents(12);
  std::vector<int32_t> k(12), m(12);
  CHECK(countRtViolations([&] {
          for (double g = -1300.0; g < 2500.0; g += 37.3) {
            const MosLinearSpan s = table.lookup(g, 1200.0);
            evaluateLinearForm(s, g, 1200.0, cents.data());
            rotateLinearForm(s, 5, k.data(), m.data());
            gD = cents[3] + s.cents(7, g, 1200.0) + k[2] * g - m[2] * 1200.0;
          }
        }) == 0);
}

TEST_CASE("rt: small MOS helpers") {
  const std::vector<double> modmos = {0, 1, 0, -1};
  CHECK(countRtViolations([&] {
          gD = gmnGenerator(705.0, 1200.0) + gmnGenerator(2650.0, 1200.0);
          gI = findParentMos(12, 7).has_value() + isDescendantMos({12, 7}, {2, 3});
          gI = childMoses({5, 2})[0].nL;
          gI = hasModmos(modmos);
          gD = stretchCents(701.955, 3.0) + stretchedOctave(-2.0);
          gI = periodRepetitions(3600.0, 1800.0);
          gD = js::round(2.5) + js::wrap(-30.0, 1200.0) + js::round2(1.23456) + js::mod(-5.0, 3.0);
        }) == 0);
}
