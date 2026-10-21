#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <cmath>

#include "pq/graph/types.h"

using namespace pq::graph;

TEST_CASE("types: unit suffixes round-trip and normalise as PATCH_LANGUAGE §1 says") {
  const Unit all[] = {Unit::Hz, Unit::kHz, Unit::ms, Unit::s, Unit::smp, Unit::dB, Unit::cents, Unit::st, Unit::percent};
  for (Unit u : all) {
    Unit back = Unit::None;
    CHECK(unitFromSuffix(unitSuffix(u), back));
    CHECK(back == u);
  }
  Unit u = Unit::Hz;
  CHECK_FALSE(unitFromSuffix("", u));
  CHECK_FALSE(unitFromSuffix("Khz", u));
  CHECK(unitSuffix(Unit::cents) == "c");
  CHECK(unitSuffix(Unit::None).empty());

  CHECK(Constant::real(440, Unit::Hz).value() == 440.0);
  CHECK(Constant::real(1.5, Unit::kHz).value() == 1500.0);
  CHECK(Constant::real(5, Unit::ms).value() == doctest::Approx(0.005));
  CHECK(Constant::real(2, Unit::s).value() == 2.0);
  CHECK(Constant::real(64, Unit::smp).value() == 64.0);
  CHECK(Constant::real(-6, Unit::dB).value() == doctest::Approx(0.501187233627));
  CHECK(Constant::real(0, Unit::dB).value() == 1.0);
  CHECK(Constant::real(702, Unit::cents).value() == 702.0);
  CHECK(Constant::real(7, Unit::st).value() == 700.0);
  CHECK(Constant::real(50, Unit::percent).value() == 0.5);
  CHECK(Constant::boolean(true).value() == 1.0);
}

TEST_CASE("types: constants compare what was written, bit-exactly") {
  CHECK(Constant::real(5, Unit::ms) == Constant::real(5, Unit::ms));
  CHECK_FALSE(Constant::real(5, Unit::ms) == Constant::real(0.005, Unit::s));  // same value, different text
  CHECK_FALSE(Constant::real(0.0) == Constant::real(-0.0));
  CHECK(Constant::real(std::nan("")) == Constant::real(std::nan("")));
  CHECK_FALSE(Constant::real(1) == Constant::boolean(true));
}

TEST_CASE("types: port type text") {
  CHECK(toText(PortType{}) == "?");
  CHECK(toText(portType(Kind::Real, Rate::Audio, Poly::Voice, kWidthMono)) == "real audio voice mono");
  CHECK(toText(portType(Kind::Bool, Rate::Infer, Poly::Infer, kWidthStereo)) == "bool stereo");
  CHECK(toText(portType(Kind::Real, Rate::Param, Poly::Global, 6)) == "real param global ch(6)");
  CHECK(toText(Rate::Event) == "event");
}

TEST_CASE("types: identifiers, keywords, reserved names") {
  CHECK(isIdentifier("x"));
  CHECK(isIdentifier("_a1"));
  CHECK(isIdentifier("loopStart"));
  CHECK_FALSE(isIdentifier(""));
  CHECK_FALSE(isIdentifier("1a"));
  CHECK_FALSE(isIdentifier("a-b"));
  CHECK_FALSE(isIdentifier("voice.hz"));
  CHECK_FALSE(isIdentifier("é"));
  for (const char* k : {"patch", "import", "param", "in", "out", "export", "solve", "iterations", "layout", "true",
                        "false"})
    CHECK(isKeyword(k));
  CHECK_FALSE(isKeyword("audio"));  // port-type words are not reserved
  CHECK_FALSE(isKeyword("unknown"));
  CHECK(isKeywordNotName("export"));
  CHECK(isKeywordNotName("true"));
  CHECK_FALSE(isKeywordNotName("out"));  // `out out : audio` in PATCH_LANGUAGE §4
  CHECK_FALSE(isKeywordNotName("in"));
  CHECK_FALSE(isKeywordNotName("x"));
  CHECK(isReservedName("voice"));
  CHECK(isReservedName("transport"));
  CHECK(isReservedName("sr"));
  CHECK_FALSE(isReservedName("voices"));
}
