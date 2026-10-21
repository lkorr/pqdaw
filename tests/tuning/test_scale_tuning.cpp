// Scale tuning (T2.4): .scl degrees resolved to Hz, with .kbm mappings deciding the key → degree assignment and the
// reference pitch.

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "pq/tuning/scala.h"
#include "pq/tuning/scale_tuning.h"
#include "pq/tuning/tuning.h"

using namespace pq::tuning;

namespace {

Scale scaleFrom(std::string_view scl) {
  auto parsed = parseScl(scl);
  REQUIRE(parsed.ok());
  auto s = Scale::make(*parsed);
  REQUIRE(s.ok());
  return std::move(s).value();
}

Scale scaleFile(const char* name) {
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/tuning/fixtures/scl/" + name;
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream ss;
  ss << in.rdbuf();
  return scaleFrom(ss.str());
}

KeyboardMapping standardKbm() {
  KeyboardMapping k;
  k.mapSize = 12;
  k.mapping = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  return k;  // keys 0..127, middle 60, reference 69 = 440 Hz, formal octave 12
}

double keyHz(const Tuning& t, const KeyboardMapping& k, int32_t key) {
  const auto p = mapKey(k, key, t.scale()->size());
  REQUIRE(p.has_value());
  const auto hz = resolveHzDouble(*p, t, {});
  REQUIRE(hz.has_value());
  return *hz;
}

double centsBetween(double a, double b) { return 1200.0 * std::log2(a / b); }

}  // namespace

TEST_CASE("scale tuning: 12-EDO .scl with the standard mapping matches 12-TET at A4 = 440") {
  const Scale s = scaleFrom("12-EDO\n12\n100.\n200.\n300.\n400.\n500.\n600.\n700.\n800.\n900.\n1000.\n1100.\n2/1\n");
  const KeyboardMapping k = standardKbm();
  const auto t = makeScalaTuning(s, k);
  REQUIRE(t.ok());
  REQUIRE(t->scale() != nullptr);
  CHECK(t->referenceHz() == doctest::Approx(261.6255653005986).epsilon(1e-14));
  for (int32_t key = 0; key <= 127; ++key) {
    CAPTURE(key);
    const double want = 440.0 * std::exp2((key - 69) / 12.0);
    CHECK(std::fabs(centsBetween(keyHz(*t, k, key), want)) < 1e-9);
    // float32 result within 0.001 cents (T2.1 precision rule).
    const float f = resolveHz(*mapKey(k, key, 12), *t, {});
    CHECK(std::fabs(centsBetween(static_cast<double>(f), want)) < 1e-3);
  }
}

TEST_CASE("scale tuning: meantone file, exact ratios stay exact relative to degree 0") {
  const Scale s = scaleFile("meantone_qc.scl");
  const KeyboardMapping k = standardKbm();
  const Tuning t = *makeScalaTuning(s, k);
  CHECK(keyHz(t, k, 69) == doctest::Approx(440.0).epsilon(1e-14));
  const double c4 = keyHz(t, k, 60);
  CHECK(keyHz(t, k, 64) / c4 == doctest::Approx(1.25).epsilon(1e-15));        // 5/4
  CHECK(keyHz(t, k, 68) / c4 == doctest::Approx(25.0 / 16.0).epsilon(1e-15));  // 25/16
  CHECK(keyHz(t, k, 72) / c4 == doctest::Approx(2.0).epsilon(1e-15));
  CHECK(keyHz(t, k, 48) / c4 == doctest::Approx(0.5).epsilon(1e-15));
  // A4 is degree 9 (889.73529 cents above C4).
  CHECK(centsBetween(440.0, c4) == doctest::Approx(889.73529).epsilon(1e-12));
  // Every key against the file numbers (quarter-comma meantone rounded to 5 decimals; 5/4 and 25/16 exact).
  const double degreeCents[12] = {0.0,       76.049,   193.15686, 310.26471, 386.3137138648348, 503.42157,
                                  579.47057, 696.57843, 772.6274277296696, 889.73529, 1006.84314, 1082.89214};
  for (int32_t key = 0; key <= 127; ++key) {
    const int32_t rel = key - 60;
    const int32_t oct = rel >= 0 ? rel / 12 : -((11 - rel) / 12);
    const int32_t deg = rel - 12 * oct;
    const double want = 440.0 * std::exp2((degreeCents[deg] + 1200.0 * oct - 889.73529) / 1200.0);
    CAPTURE(key);
    CHECK(std::fabs(centsBetween(keyHz(t, k, key), want)) < 1e-9);
  }
}

TEST_CASE("scale tuning: white-key mapping of a 7-note JI scale (formal octave 7, unmapped black keys)") {
  const Scale s = scaleFrom("JI major\n7\n9/8\n5/4\n4/3\n3/2\n5/3\n15/8\n2/1\n");
  const KeyboardMapping k = *parseKbm("12\n0\n127\n60\n69\n440.0\n7\n0\nx\n1\nx\n2\n3\nx\n4\nx\n5\nx\n6\n");
  const Tuning t = *makeScalaTuning(s, k);
  // A (key 69) is degree 5 = 5/3 at 440 Hz, so degree 0 (C, key 60) is 264 Hz.
  CHECK(t.referenceHz() == doctest::Approx(264.0).epsilon(1e-14));
  CHECK(keyHz(t, k, 60) == doctest::Approx(264.0).epsilon(1e-14));
  CHECK(keyHz(t, k, 62) == doctest::Approx(297.0).epsilon(1e-14));
  CHECK(keyHz(t, k, 67) == doctest::Approx(396.0).epsilon(1e-14));
  CHECK(keyHz(t, k, 72) == doctest::Approx(528.0).epsilon(1e-14));
  CHECK(keyHz(t, k, 59) == doctest::Approx(247.5).epsilon(1e-14));  // B below: 15/8 / 2
  CHECK(keyHz(t, k, 36) == doctest::Approx(66.0).epsilon(1e-14));
  CHECK(mapKey(k, 61, 7) == std::nullopt);
}

TEST_CASE("scale tuning: non-octave equave (Bohlen-Pierce tritave), linear mapping") {
  const Scale s = scaleFile("bp_equal.scl");
  CHECK(s.size() == 13);
  CHECK(s.equaveCents() == doctest::Approx(1901.955000865387).epsilon(1e-15));
  const KeyboardMapping k = KeyboardMapping::linear(60, 60, 261.626);
  const Tuning t = *makeScalaTuning(s, k);
  CHECK(keyHz(t, k, 60) == doctest::Approx(261.626).epsilon(1e-14));
  CHECK(keyHz(t, k, 73) == doctest::Approx(3.0 * 261.626).epsilon(1e-14));
  CHECK(keyHz(t, k, 47) == doctest::Approx(261.626 / 3.0).epsilon(1e-14));
  CHECK(keyHz(t, k, 61) == doctest::Approx(261.626 * std::exp2(146.30423 / 1200.0)).epsilon(1e-14));
}

TEST_CASE("scale tuning: degree wrapping, Degree identities and the default reference") {
  const Scale s = scaleFile("partch43.scl");
  CHECK(s.size() == 43);
  CHECK(s.cents()[0] == 0.0);
  CHECK(s.centsOf(-1, 0) == s.centsOf(42, -1));
  CHECK(s.centsOf(45, 0) == s.centsOf(2, 1));
  CHECK(s.centsOfDegree(-43 * 3 + 5) == s.centsOf(5, -3));
  CHECK(s.centsOf(5, -3) == doctest::Approx(s.cents()[5] - 3 * 1200.0).epsilon(1e-15));
  const Tuning t = *Tuning::make(s);  // reference C3 = 261.626 Hz on degree 0
  const auto hz = resolveHzDouble(PitchId::fromDegree(42, 0), t, {});
  REQUIRE(hz);
  CHECK(*hz == doctest::Approx(261.626 * 160.0 / 81.0).epsilon(1e-14));  // Partch 43 degree 42 = 160/81
  // Scales with pitches below 1/1 and out of order are allowed (negative.scl starts at −240 cents).
  const Scale d = scaleFile("negative.scl");
  CHECK(d.cents()[1] < 0.0);
  CHECK(resolveHz(PitchId::fromDegree(1, 0), *Tuning::make(d), {}) > 0.0f);
}

TEST_CASE("scale tuning: equaves at or below 1/1 extend as Scala does") {
  // A descending scale (equave 1/2): degree 2 of equave 1 is two equaves' worth lower.
  const Scale down = scaleFrom("down\n2\n3/4\n1/2\n");
  CHECK(down.equaveCents() == doctest::Approx(-1200.0).epsilon(1e-15));
  CHECK(down.centsOf(1, 1) == doctest::Approx(-1200.0 + 1200.0 * std::log2(0.75)).epsilon(1e-15));
  CHECK(down.centsOfDegree(-1) == doctest::Approx(1200.0 * std::log2(0.75) + 1200.0).epsilon(1e-15));
  // Equave 1/1 (as in harm_subharm16.scl): every equave repeats the same pitches.
  const Scale flat = scaleFrom("flat\n2\n16/15\n1/1\n");
  CHECK(flat.centsOf(1, 5) == flat.centsOf(1, -3));
}

TEST_CASE("scale tuning: errors") {
  ScalaScale empty;
  CHECK_FALSE(Scale::make(empty).ok());
  ScalaScale nan;
  nan.pitches = {ScalaPitch::fromCents(std::nan("")), ScalaPitch::fromCents(1200.0)};
  CHECK_FALSE(Scale::make(nan).ok());

  const Scale s = scaleFrom("x\n1\n2/1\n");
  KeyboardMapping k = standardKbm();
  k.mapping[9] = kUnmappedKey;
  CHECK_FALSE(makeScalaTuning(s, k).ok());  // reference key unmapped
  k = standardKbm();
  k.lastKey = 68;
  CHECK_FALSE(makeScalaTuning(s, k).ok());  // reference key outside the retuned range
  k = standardKbm();
  k.referenceHz = 0.0;
  CHECK_FALSE(makeScalaTuning(s, k).ok());
}
