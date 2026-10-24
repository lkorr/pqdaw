// MOS port: JavaScript number semantics, known musical facts, the per-region linear form (T2.6's input) and the MOS
// tuning context. The bulk comparison against the original JavaScript is in test_mos_fixtures.cpp.

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

#include "pq/tuning/js_number.h"
#include "pq/tuning/mos.h"
#include "pq/tuning/mos_data.h"
#include "pq/tuning/mos_regions.h"
#include "pq/tuning/tuning.h"

using namespace pq::tuning;

TEST_CASE("js number: Math.round, toFixed, toString") {
  CHECK(js::round(2.5) == 3.0);
  CHECK(js::round(-2.5) == -2.0);
  CHECK(js::round(0.49999999999999994) == 0.0);
  CHECK(js::round(-0.6) == -1.0);
  // toFixed rounds the exact binary value: 1.005 is 1.00499999999999989…, 0.125 is an exact tie (rounds up).
  CHECK(js::toFixed(1.005, 2) == "1.00");
  CHECK(js::toFixed(0.125, 2) == "0.13");
  CHECK(js::toFixed(2.5, 0) == "3");
  CHECK(js::toFixed(9.995, 2) == "9.99");
  CHECK(js::toFixed(99.999, 2) == "100.00");
  CHECK(js::toFixed(-0.001, 2) == "-0.00");
  CHECK(js::toFixed(-0.0, 2) == "0.00");
  CHECK(js::toFixed(1.0 / 3.0, 4) == "0.3333");
  CHECK(js::toFixed(INFINITY, 2) == "Infinity");
  CHECK(js::toString(1.5) == "1.5");
  CHECK(js::toString(2.0) == "2");
  CHECK(js::toString(-0.0) == "0");
  CHECK(js::toString(0.1 + 0.2) == "0.30000000000000004");
  CHECK(js::toString(1e21) == "1e+21");
  CHECK(js::toString(1e-7) == "1e-7");
  CHECK(js::toString(0.000001) == "0.000001");
  CHECK(js::toString(123456789012345680000.0) == "123456789012345680000");
}

TEST_CASE("mos: diatonic facts") {
  // 700 cents, 7 notes: Lydian on rotation 0 with the bright generator.
  const MosScale sc = buildMosScale(700.0, 7, 1200.0);
  const std::vector<double> want = {0, 200, 400, 600, 700, 900, 1100};
  REQUIRE(sc.pitches.size() == 7);
  for (size_t i = 0; i < 7; ++i) CHECK(sc.pitches[i] == doctest::Approx(want[i]).epsilon(1e-12));
  CHECK(computeStepPattern(700.0, 7, 0, 1200.0) == "LLLsLLs");
  CHECK(computeUdp(700.0, 7, 0, 1200.0) == "6|0");
  CHECK(modeName({5, 2}, 700.0, 0, 1200.0) == "Lydian");
  CHECK(modeName({5, 2}, 700.0, 2, 1200.0) == "Aeolian");  // F G A B C D E: degree 2 is A
  CHECK(modeName({5, 2}, 700.0, 5, 1200.0) == "Dorian");
  CHECK(computeHardness(700.0, 7, 1200.0).str == "2.00");
  CHECK(computeGmn(700.0, 7, 0, 1200.0) == 1);  // Lydian is 1st Meantone[7]
  CHECK(findParentMos(5, 2) == MosKey{2, 3});
  CHECK(isDescendantMos({12, 7}, {5, 2}));
  CHECK(mosName({5, 2}) == "Diatonic");
  CHECK(formatModmosAlterations(std::vector<double>{0, 0, 1, 0, 0, -2, 0.5}) == "+3 -2\xC3\x97" "6 +0.5\xC3\x97" "7");
}

TEST_CASE("mos linear form: k·g − m·P reproduces buildMosScale inside every interval") {
  std::mt19937_64 rng(7);
  for (int32_t n = 1; n <= 30; ++n) {
    const MosLinearTable table(n);
    CAPTURE(n);
    CHECK(table.boundaries().front() == 0.0);
    CHECK(table.boundaries().back() == 1.0);
    std::vector<double> cents(static_cast<size_t>(n));
    std::vector<int32_t> k(static_cast<size_t>(n)), m(static_cast<size_t>(n));
    for (int32_t t = 0; t < table.intervalCount(); ++t) {
      const double lo = table.boundaries()[static_cast<size_t>(t)], hi = table.boundaries()[static_cast<size_t>(t) + 1];
      for (int rep = 0; rep < 3; ++rep) {
        const double P = rep == 0 ? 1200.0 : rep == 1 ? 600.0 : 1901.955;
        const double u = 0.02 + 0.96 * std::uniform_real_distribution<double>(0.0, 1.0)(rng);
        const int32_t shift = static_cast<int32_t>(rng() % 3) - 1;  // generators below 0 and above the period too
        const double g = (shift + lo + u * (hi - lo)) * P;
        const MosLinearSpan span = table.lookup(g, P);
        CHECK(span.periodShift == shift);
        CHECK(span.lowGenerator <= g);
        CHECK(g < span.highGenerator);
        evaluateLinearForm(span, g, P, cents.data());
        const MosScale sc = buildMosScale(g, n, P);
        for (int32_t j = 0; j < n; ++j)
          CHECK(std::fabs(cents[static_cast<size_t>(j)] - sc.pitches[static_cast<size_t>(j)]) < 1e-9);
        // Rotated coefficients reproduce rotateMosScale.
        const int32_t rot = static_cast<int32_t>(rng() % static_cast<uint64_t>(n));
        rotateLinearForm(span, rot, k.data(), m.data());
        const MosScale r = rotateMosScale(sc, rot, P);
        for (int32_t j = 0; j < n; ++j) {
          const double c = k[static_cast<size_t>(j)] * g - m[static_cast<size_t>(j)] * P;
          CHECK(std::fabs(c - r.pitches[static_cast<size_t>(j)]) < 1e-9);
        }
      }
    }
  }
}

TEST_CASE("Mos::retuneLinear (T3.12 live morph) equals Mos::make inside an interval, every period, any rotation") {
  std::mt19937_64 rng(31);
  for (int32_t n : {2, 5, 7, 12, 24}) {
    const MosLinearTable table(n);
    std::vector<int32_t> k(static_cast<size_t>(n)), m(static_cast<size_t>(n));
    for (int rep = 0; rep < 40; ++rep) {
      const int32_t ppe = 1 + static_cast<int32_t>(rng() % 3);
      const double P = 1200.0 / ppe;
      const int32_t t = static_cast<int32_t>(rng() % static_cast<uint64_t>(table.intervalCount()));
      const double lo = table.boundaries()[static_cast<size_t>(t)], hi = table.boundaries()[static_cast<size_t>(t) + 1];
      auto at = [&](double u) { return (lo + (0.02 + 0.96 * u) * (hi - lo)) * P; };
      std::uniform_real_distribution<double> U(0.0, 1.0);
      MosParams p;
      p.n = n;
      p.period = P;
      p.periodsPerEquave = ppe;
      p.rotation = static_cast<int32_t>(rng() % static_cast<uint64_t>(n));
      p.generator = at(U(rng));
      Result<Mos> mos = Mos::make(p);
      REQUIRE(mos.ok());
      const double g2 = at(U(rng));
      rotateLinearForm(table.lookup(g2, P), p.rotation, k.data(), m.data());
      mos->retuneLinear(g2, k, m, std::nullopt);
      p.generator = g2;
      const Result<Mos> ref = Mos::make(p);
      REQUIRE(ref.ok());
      CAPTURE(n);
      CAPTURE(ppe);
      CHECK(mos->params().generator == g2);
      REQUIRE(mos->size() == ref->size());
      for (int32_t d = 0; d < ref->size(); ++d)
        CHECK(std::fabs(mos->pitches()[static_cast<size_t>(d)] - ref->pitches()[static_cast<size_t>(d)]) < 1e-9);
    }
  }
}

TEST_CASE("mos linear form: boundaries are the Farey fractions of order n − 1") {
  // Order 4 (n = 5): 0, 1/4, 1/3, 1/2, 2/3, 3/4, 1.
  const MosLinearTable t(5);
  const std::vector<double> want = {0.0, 0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75, 1.0};
  REQUIRE(t.boundaries().size() == want.size());
  for (size_t i = 0; i < want.size(); ++i) CHECK(t.boundaries()[i] == want[i]);
  // Every MOS region boundary of size ≤ n lies on one of them, so a region never straddles two intervals.
  const MosRegionSet rs = buildMosRegions(12, 12, 1200.0, 1);
  for (const MosRegion& r : rs.regions) {
    const MosLinearTable tn(r.perPeriodSize);
    const MosLinearSpan a = tn.lookup(r.lowCents + 1e-6, 1200.0);
    const MosLinearSpan b = tn.lookup(r.highCents - 1e-6, 1200.0);
    CHECK(a.k == b.k);
  }
}

TEST_CASE("mos tuning: degree resolution through resolveHz") {
  MosParams p;
  p.generator = 696.578;  // quarter-comma meantone fifth
  p.n = 7;
  p.rotation = 1;
  p.octaveStretch = 2.5;
  auto mos = Mos::make(p);
  REQUIRE(mos.ok());
  CHECK(mos->size() == 7);
  auto t = Tuning::make(*mos, 220.0);
  REQUIRE(t.ok());
  REQUIRE(t->mos() != nullptr);
  CHECK(t->edo() == nullptr);

  const FullScale full = buildFullScale(p);
  for (int32_t e = -2; e <= 2; ++e)
    for (int32_t d = 0; d < 7; ++d) {
      const double cents = stretchCents(full.pitches[static_cast<size_t>(d)] + e * 1200.0, 2.5);
      const auto hz = resolveHzDouble(PitchId::fromDegree(d, e), *t, {});
      REQUIRE(hz.has_value());
      CHECK(*hz == doctest::Approx(220.0 * std::exp2(cents / 1200.0)).epsilon(1e-14));
      // Degrees outside [0, size) wrap into neighbouring equaves.
      CHECK(*resolveHzDouble(PitchId::fromDegree(d + 7, e - 1), *t, {}) == *hz);
      CHECK(resolveHz(PitchId::fromDegree(d, e), *t, {}) == static_cast<float>(*hz));
    }

  // Multi-period: 2 periods of 600 cents, 5 notes each (a 10-note equave).
  MosParams q;
  q.generator = 110.0;
  q.n = 5;
  q.period = 600.0;
  q.periodsPerEquave = 2;
  auto mq = Mos::make(q);
  REQUIRE(mq.ok());
  CHECK(mq->size() == 10);
  CHECK(mq->equaveCents() == 1200.0);
  CHECK(mq->centsOfDegree(5) == doctest::Approx(600.0));
  CHECK(mq->centsOfDegree(-10) == doctest::Approx(-1200.0));
}

TEST_CASE("mos tuning: invalid parameters") {
  MosParams p;
  p.rotation = 7;
  CHECK_FALSE(Mos::make(p).ok());
  p.rotation = 0;
  p.period = 0.0;
  CHECK_FALSE(Mos::make(p).ok());
  p.period = 1200.0;
  p.generator = NAN;
  CHECK_FALSE(Mos::make(p).ok());
  p.generator = 700.0;
  p.periodsPerEquave = 0;
  CHECK_FALSE(Mos::make(p).ok());
  p.periodsPerEquave = 1;
  p.octaveStretch = -1200.0;
  CHECK_FALSE(Mos::make(p).ok());
  p.octaveStretch = 0.0;
  p.n = 0;
  CHECK_FALSE(Mos::make(p).ok());
}
