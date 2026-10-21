#include <doctest/doctest.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <random>

#include "pq/tuning/tuning.h"

using pq::tuning::AnchorKind;
using pq::tuning::Edo;
using pq::tuning::PitchId;
using pq::tuning::PitchKind;
using pq::tuning::Rational;
using pq::tuning::ResolveContext;
using pq::tuning::Tuning;

namespace {

Tuning tuningOf(uint32_t n, Rational equave = Rational(2), double ref = pq::tuning::kDefaultReferenceHz) {
  auto edo = Edo::make(n, equave);
  REQUIRE(edo.ok());
  auto t = Tuning::make(*edo, ref);
  REQUIRE(t.ok());
  return *t;
}

double centsBetween(double a, double b) { return 1200.0 * std::log2(a / b); }

}  // namespace

TEST_CASE("pitch id: layout and kinds") {
  static_assert(sizeof(PitchId) == 24);
  const PitchId d = PitchId::fromDegree(7, -1);
  CHECK(d.kind() == PitchKind::Degree);
  CHECK(d.degree() == 7);
  CHECK(d.equave() == -1);

  const PitchId r = PitchId::fromRatio(Rational(6, 4), AnchorKind::Note, 99);
  CHECK(r.kind() == PitchKind::Ratio);
  CHECK(r.ratio() == Rational(3, 2));
  CHECK(r.anchorKind() == AnchorKind::Note);
  CHECK(r.anchorId() == 99);

  const PitchId c = PitchId::fromCents(-386.3);
  CHECK(c.kind() == PitchKind::Cents);
  CHECK(c.cents() == -386.3);

  const PitchId h = PitchId::fromHarmonic(5, 2);
  CHECK(h.kind() == PitchKind::Harmonic);
  CHECK(h.jiObject() == 5);
  CHECK(h.voiceIndex() == 2);

  CHECK(PitchId{} == PitchId::fromDegree(0, 0));
  CHECK(d == PitchId::fromDegree(7, -1));
  CHECK_FALSE(d == PitchId::fromDegree(7, 0));
  CHECK(r == PitchId::fromRatio(Rational(3, 2), AnchorKind::Note, 99));
  CHECK_FALSE(r == PitchId::fromRatio(Rational(3, 2), AnchorKind::Note, 98));
  CHECK_FALSE(r == PitchId::fromRatio(Rational(3, 2)));
  CHECK_FALSE(h == PitchId::fromHarmonic(5, 3));
  CHECK_FALSE(PitchId::fromCents(0.0) == PitchId::fromDegree(0, 0));

  CHECK_FALSE(PitchId::tryFromRatio(Rational{}).has_value());
  CHECK_FALSE(PitchId::tryFromRatio(Rational(-3, 2)).has_value());
  CHECK(PitchId::tryFromRatio(Rational(3, 2)).has_value());
}

TEST_CASE("tuning: defaults and validation") {
  const Tuning t;
  CHECK(t.referenceHz() == 261.626);
  REQUIRE(t.edo() != nullptr);
  CHECK(t.edo()->n() == 12);
  CHECK_FALSE(Tuning::make(Edo::standard12(), 0.0).ok());
  CHECK_FALSE(Tuning::make(Edo::standard12(), -440.0).ok());
  CHECK_FALSE(Tuning::make(Edo::standard12(), std::nan("")).ok());
  CHECK(Tuning::make(Edo::standard12(), 440.0)->referenceHz() == 440.0);
}

TEST_CASE("resolve: degree in an EDO") {
  const Tuning t;
  const ResolveContext ctx;
  CHECK(pq::tuning::resolveHz(PitchId::fromDegree(0), t, ctx) == doctest::Approx(261.626f));
  // A3 = C3 · 2^(9/12) ≈ 440.0 Hz with C3 = 261.626 (Ableton naming).
  CHECK(pq::tuning::resolveHz(PitchId::fromDegree(9), t, ctx) == doctest::Approx(440.0).epsilon(1e-5));
  CHECK(pq::tuning::resolveHz(PitchId::fromDegree(0, 1), t, ctx) == doctest::Approx(523.252f));
  CHECK(pq::tuning::resolveHz(PitchId::fromDegree(0, -1), t, ctx) == doctest::Approx(130.813f));
  // degree outside [0, n) is tolerated: (12, 0) == (0, 1)
  CHECK(pq::tuning::resolveHz(PitchId::fromDegree(12, 0), t, ctx) ==
        pq::tuning::resolveHz(PitchId::fromDegree(0, 1), t, ctx));

  const Tuning t31 = tuningOf(31);
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromDegree(18), t31, ctx) ==
        doctest::Approx(261.626 * std::pow(2.0, 18.0 / 31.0)).epsilon(1e-14));

  // Bohlen–Pierce: one equave up is exactly 3×.
  const Tuning bp = tuningOf(13, Rational(3), 100.0);
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromDegree(0, 1), bp, ctx) == doctest::Approx(300.0).epsilon(1e-14));
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromDegree(13, -2), bp, ctx) ==
        doctest::Approx(100.0 / 3.0).epsilon(1e-14));
}

TEST_CASE("resolve: ratio relative to its anchor") {
  const Tuning t = tuningOf(12, Rational(2), 200.0);
  ResolveContext ctx;
  // Reference anchor: the tuning's reference pitch.
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromRatio(Rational(3, 2)), t, ctx) == 300.0);
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromRatio(Rational(1, 3)), t, ctx) ==
        doctest::Approx(200.0 / 3.0).epsilon(1e-15));
  // Note / JI fundamental anchors need the anchor's Hz from the caller.
  const PitchId onNote = PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, 7);
  CHECK_FALSE(pq::tuning::resolveHzDouble(onNote, t, ctx).has_value());
  CHECK(pq::tuning::resolveHz(onNote, t, ctx) == 0.0f);
  ctx.anchorHz = 440.0;
  CHECK(*pq::tuning::resolveHzDouble(onNote, t, ctx) == 550.0);
  const PitchId onFundamental = PitchId::fromRatio(Rational(7), AnchorKind::JiFundamental, 1);
  ctx.anchorHz = 55.0;
  CHECK(pq::tuning::resolveHz(onFundamental, t, ctx) == 385.0f);
  // The tuning's step structure does not matter for a ratio: it stays at true pitch.
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromRatio(Rational(3, 2)), tuningOf(31, Rational(2), 200.0), ctx) ==
        300.0);
  // Extreme but representable ratios.
  const int64_t big = int64_t{1} << 62;
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromRatio(Rational(big + 1, big)), t, ctx) ==
        doctest::Approx(200.0).epsilon(1e-15));
}

TEST_CASE("resolve: cents from the reference") {
  const Tuning t;
  const ResolveContext ctx;
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromCents(0.0), t, ctx) == 261.626);
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromCents(1200.0), t, ctx) == doctest::Approx(523.252).epsilon(1e-15));
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromCents(-2400.0), t, ctx) ==
        doctest::Approx(65.4065).epsilon(1e-15));
  CHECK(*pq::tuning::resolveHzDouble(PitchId::fromCents(701.955), t, ctx) ==
        doctest::Approx(261.626 * std::pow(2.0, 701.955 / 1200.0)).epsilon(1e-14));
  // Out of float32 range: unresolvable rather than inf/denormal.
  CHECK_FALSE(pq::tuning::resolveHzDouble(PitchId::fromCents(1.0e6), t, ctx).has_value());
  CHECK_FALSE(pq::tuning::resolveHzDouble(PitchId::fromCents(-1.0e6), t, ctx).has_value());
  CHECK_FALSE(pq::tuning::resolveHzDouble(PitchId::fromCents(std::nan("")), t, ctx).has_value());
}

TEST_CASE("resolve: harmonic identity follows its JI object") {
  const Tuning t;
  const uint32_t chord[] = {4, 5, 6, 7};  // a 4:5:6:7 JI object
  ResolveContext ctx;
  ctx.harmonic = {100.0, chord, 4, false};
  CHECK(pq::tuning::resolveHz(PitchId::fromHarmonic(1, 0), t, ctx) == 400.0f);
  CHECK(pq::tuning::resolveHz(PitchId::fromHarmonic(1, 3), t, ctx) == 700.0f);
  // Moving the object's fundamental moves its voices.
  ctx.harmonic.fundamentalHz = 50.0;
  CHECK(pq::tuning::resolveHz(PitchId::fromHarmonic(1, 1), t, ctx) == 250.0f);
  // Undertone object: voices sit at fundamental / h.
  ctx.harmonic.undertone = true;
  ctx.harmonic.fundamentalHz = 840.0;
  CHECK(pq::tuning::resolveHz(PitchId::fromHarmonic(1, 0), t, ctx) == 210.0f);
  CHECK(pq::tuning::resolveHz(PitchId::fromHarmonic(1, 3), t, ctx) == 120.0f);
  // Missing frame, out-of-range voice, harmonic 0, missing fundamental: unresolvable, resolveHz gives 0.
  CHECK(pq::tuning::resolveHz(PitchId::fromHarmonic(1, 4), t, ctx) == 0.0f);
  CHECK_FALSE(pq::tuning::resolveHzDouble(PitchId::fromHarmonic(1, 0), t, ResolveContext{}).has_value());
  const uint32_t zero[] = {0};
  ResolveContext bad;
  bad.harmonic = {100.0, zero, 1, false};
  CHECK_FALSE(pq::tuning::resolveHzDouble(PitchId::fromHarmonic(1, 0), t, bad).has_value());
  bad.harmonic = {0.0, chord, 4, false};
  CHECK_FALSE(pq::tuning::resolveHzDouble(PitchId::fromHarmonic(1, 0), t, bad).has_value());
}

// Acceptance (T2.1): the float32 Hz result is within 0.001 ¢ of a double reference. The reference is computed by a
// different formula path (std::pow on the equave ratio / 2, not exp2 of a precomputed log2 step).
TEST_CASE("resolve: float32 Hz within 0.001 cents of a double reference") {
  std::mt19937_64 rng(7);
  std::uniform_int_distribution<uint32_t> nDist(1, 200);
  std::uniform_int_distribution<int32_t> eqDist(-4, 4);
  std::uniform_int_distribution<int64_t> partDist(1, 1'000'000);
  std::uniform_real_distribution<double> centsDist(-6000.0, 6000.0);
  std::uniform_real_distribution<double> refDist(8.0, 1000.0);
  std::uniform_int_distribution<uint32_t> harmDist(1, 128);
  const Rational equaves[] = {Rational(2), Rational(3), Rational(3, 2), Rational(5, 2), Rational(9, 4)};

  double worstFloat = 0.0, worstDouble = 0.0;
  for (int i = 0; i < 20000; ++i) {
    const Rational equave = equaves[rng() % 5];
    const uint32_t n = nDist(rng);
    const double ref = refDist(rng);
    const Tuning t = tuningOf(n, equave, ref);
    ResolveContext ctx;
    ctx.anchorHz = refDist(rng);
    const uint32_t harmonics[1] = {harmDist(rng)};
    ctx.harmonic = {refDist(rng) / 8.0, harmonics, 1, (rng() & 1) != 0};

    PitchId p;
    double expect = 0.0;
    switch (i % 4) {
      case 0: {
        // Degrees span ±1 equave around the chosen equave index, as on a piano roll (also outside [0, n)).
        const int32_t deg = std::uniform_int_distribution<int32_t>(-static_cast<int32_t>(n), static_cast<int32_t>(n))(rng);
        const int32_t eq = eqDist(rng);
        p = PitchId::fromDegree(deg, eq);
        const double step = static_cast<double>(static_cast<int64_t>(eq) * n + deg);
        expect = ref * std::pow(equave.toDouble(), step / n);
        break;
      }
      case 1: {
        const Rational r(partDist(rng), partDist(rng));
        const bool onNote = (rng() & 1) != 0;
        p = PitchId::fromRatio(r, onNote ? AnchorKind::Note : AnchorKind::Reference, 3);
        expect = (onNote ? ctx.anchorHz : ref) * static_cast<double>(r.num()) / static_cast<double>(r.den());
        break;
      }
      case 2: {
        const double c = centsDist(rng);
        p = PitchId::fromCents(c);
        expect = ref * std::pow(2.0, c / 1200.0);
        break;
      }
      default: {
        p = PitchId::fromHarmonic(0, 0);
        const double f = ctx.harmonic.fundamentalHz, h = harmonics[0];
        expect = ctx.harmonic.undertone ? f / h : f * h;
        break;
      }
    }
    const auto hzD = pq::tuning::resolveHzDouble(p, t, ctx);
    REQUIRE(hzD.has_value());
    const float hzF = pq::tuning::resolveHz(p, t, ctx);
    REQUIRE(hzF > 0.0f);
    const double errF = std::fabs(centsBetween(static_cast<double>(hzF), expect));
    const double errD = std::fabs(centsBetween(*hzD, expect));
    worstFloat = std::max(worstFloat, errF);
    worstDouble = std::max(worstDouble, errD);
    REQUIRE(errF < 0.001);
    REQUIRE(errD < 1e-9);
  }
  MESSAGE("worst error vs double reference: float32 ", worstFloat, " cents, double ", worstDouble, " cents");
}
