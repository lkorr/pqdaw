// Scala .scl / .kbm parsing and writing (T2.4): the format spec's rules, edge cases, 28 fixture files authored
// for pqdaw (tests/tuning/fixtures/scl, see SOURCES.txt there) and round trips.

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pq/tuning/scala.h"
#include "pq/tuning/scale_tuning.h"

using namespace pq::tuning;

namespace {

ScalaPitch R(int64_t n, int64_t d) { return ScalaPitch::fromRatio(Rational(n, d)); }
ScalaPitch C(double c) { return ScalaPitch::fromCents(c); }
ScalaPitch L(const char* n, const char* d) {
  ScalaPitch p;
  p.kind = ScalaPitch::Kind::LargeRatio;
  p.largeNum = n;
  p.largeDen = d;
  return p;
}

std::string readFixture(const std::string& name) {
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/tuning/fixtures/scl/" + name;
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "missing fixture " << path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string rtrim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}

ScalaScale mustParse(std::string_view text) {
  auto r = parseScl(text, "test.scl");
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(r).value();
}

// parse → write → parse gives the same scale, and writing is idempotent.
void checkRoundTrip(const ScalaScale& s) {
  const auto text = writeScl(s, "rt.scl");
  REQUIRE(text.ok());
  const auto back = parseScl(*text);
  REQUIRE(back.ok());
  CHECK(*back == s);
  const auto text2 = writeScl(*back, "rt.scl");
  REQUIRE(text2.ok());
  CHECK(*text2 == *text);
}

}  // namespace

TEST_CASE("scl: a mixed cents/ratio file in the spec's layout (quarter-comma meantone)") {
  const ScalaScale s = mustParse(
      "! meantone.scl\n!\nQuarter-comma meantone, Eb to G#\n 12\n!\n 76.04900\n"
      " 193.15686\n 310.26471\n 5/4\n 503.42157\n 579.47057\n 696.57843\n 25/16\n 889.73529\n 1006.84314\n"
      " 1082.89214\n 2/1\n");
  CHECK(s.description == "Quarter-comma meantone, Eb to G#");
  REQUIRE(s.pitches.size() == 12);
  CHECK(s.pitches[0] == C(76.049));
  CHECK(s.pitches[3] == R(5, 4));
  CHECK(s.pitches[7] == R(25, 16));
  CHECK(s.pitches[11] == R(2, 1));
  CHECK(s.pitches[3].cents == doctest::Approx(386.3137138648348).epsilon(1e-15));
  checkRoundTrip(s);
}

TEST_CASE("scl: every valid pitch line listed in the format spec") {
  const ScalaScale s = mustParse(
      "spec lines\n9\n81/64\n408.0\n408.\n5\n-5.0\n10/20\n100.0 cents\n 100.0 C#\n 5/4   E\\\n");
  REQUIRE(s.pitches.size() == 9);
  CHECK(s.pitches[0] == R(81, 64));
  CHECK(s.pitches[1] == C(408.0));
  CHECK(s.pitches[2] == C(408.0));
  CHECK(s.pitches[3] == R(5, 1));
  CHECK(s.pitches[4] == C(-5.0));
  CHECK(s.pitches[5] == R(1, 2));
  CHECK(s.pitches[6] == C(100.0));
  CHECK(s.pitches[7] == C(100.0));
  CHECK(s.pitches[8] == R(5, 4));
}

TEST_CASE("scl: line endings, BOM, comments, blank lines, whitespace") {
  const std::vector<ScalaPitch> want = {R(9, 8), C(701.955), R(2, 1)};
  const char* variants[] = {
      "! x\r\n!\r\ndesc\r\n 3\r\n!\r\n 9/8\r\n 701.955\r\n 2/1\r\n",      // CRLF
      "! x\n!\ndesc\n 3\n!\n 9/8\n 701.955\n 2/1\n",                      // LF
      "! x\rdesc\r3\r9/8\r701.955\r2/1",                                    // lone CR, no final newline
      "\xEF\xBB\xBF! x\ndesc\n3\n9/8\n701.955\n2/1",                        // UTF-8 BOM, no final newline
      "desc\n\t3\t\n!c\n\t9/8\t\n! between\n\n  701.955 fifth\n!\n2/1 ! octave\n",  // tabs, comments, blank line
      "!\n!\ndesc\n3\n9/8\n701.955\n2/1\n! trailing comment\nextra line ignored\n\n",
  };
  for (const char* v : variants) {
    CAPTURE(v);
    const ScalaScale s = mustParse(v);
    CHECK(s.description == "desc");
    CHECK(s.pitches == want);
  }
}

TEST_CASE("scl: description edge cases") {
  // Empty description line; description may contain anything but cannot start with '!'.
  ScalaScale s = mustParse("! name\n\n 1\n 2/1\n");
  CHECK(s.description.empty());
  CHECK(s.pitches == std::vector<ScalaPitch>{R(2, 1)});
  checkRoundTrip(s);
  s = mustParse("  spaced  out 3/2 description \n1\n3/1\n");
  CHECK(s.description == "  spaced  out 3/2 description ");
  checkRoundTrip(s);
  // Count 0 is valid (degree 0 = 1/1 is implicit).
  s = mustParse("empty scale\n 0\n");
  CHECK(s.pitches.empty());
  checkRoundTrip(s);
}

TEST_CASE("scl: cents forms and precision") {
  const ScalaScale s = mustParse("c\n6\n+100.5\n-0.0\n0.\n1199.99999999999999999\n00012.50\n 3.14159265358979323846\n");
  CHECK(s.pitches[0] == C(100.5));
  CHECK(s.pitches[1].kind == ScalaPitch::Kind::Cents);
  CHECK(s.pitches[1].cents == 0.0);
  CHECK(s.pitches[2] == C(0.0));
  CHECK(s.pitches[3] == C(1200.0));  // correctly rounded
  CHECK(s.pitches[4] == C(12.5));
  CHECK(s.pitches[5] == C(3.141592653589793));
  checkRoundTrip(s);
  // An exponent is not part of the format: "1.5e3" reads as 1.5 cents (the rest of the line is ignored).
  CHECK(mustParse("e\n1\n1.5e3\n").pitches[0] == C(1.5));
  // A ratio followed by a period is a ratio ("anything after a valid pitch value should be ignored").
  CHECK(mustParse("e\n1\n3/2.\n").pitches[0] == R(3, 2));
}

TEST_CASE("scl: ratio forms, reduction and large ratios") {
  const ScalaScale s = mustParse(
      "r\n5\n2147483647/2147483646\n9223372036854775807/4611686018427387904\n1968/1683\n"
      "9223372036854775808/9223372036854775807\n1709671705179880612640625/1208925819614629174706176\n");
  CHECK(s.pitches[0] == R(2147483647, 2147483646));  // the spec's minimum supported size
  CHECK(s.pitches[1] == R(INT64_MAX, int64_t{1} << 62));
  CHECK(s.pitches[2] == R(656, 561));                 // reduced
  CHECK(s.pitches[3] == L("9223372036854775808", "9223372036854775807"));
  CHECK(s.pitches[4] == L("1709671705179880612640625", "1208925819614629174706176"));
  // Large-ratio cents are computed from the digits: 2^63/(2^63−1) is 1.9e-16 cents.
  CHECK(std::fabs(s.pitches[3].cents) < 1e-12);
  const double exact = 1200.0 * std::log2(1709671705179880612640625.0 / 1208925819614629174706176.0);
  CHECK(s.pitches[4].cents == doctest::Approx(exact).epsilon(1e-13));
  checkRoundTrip(s);
  // Leading zeros are not significant.
  CHECK(mustParse("z\n1\n0003/0002\n").pitches[0] == R(3, 2));
  CHECK(mustParse("z\n1\n00000000000000000000000000003/2\n").pitches[0] == R(3, 2));
}

TEST_CASE("scl: read errors") {
  struct Bad {
    const char* text;
    uint32_t line;
  };
  const Bad bad[] = {
      {"", 0},                                  // no description
      {"! only comments\n!\n", 0},              // no description
      {"desc\n", 0},                            // no count
      {"desc\ntwelve\n", 2},                    // count not a number
      {"desc\n-1\n", 2},                        // negative count
      {"desc\n12 notes\n", 2},                  // trailing text on the count line is not allowed
      {"desc\n3\n9/8\n3/2\n", 2},               // fewer pitch lines than the count (reported at the count)
      {"desc\n99999999999999999999999\n", 2},  // count too large
      {"desc\n1\n-3/2\n", 3},                   // negative ratio
      {"desc\n1\n0/1\n", 3},                    // zero
      {"desc\n1\n3/0\n", 3},                    // zero denominator
      {"desc\n1\n3/\n", 3},                     // missing denominator
      {"desc\n1\n.5\n", 3},                     // no leading period (Scala rule)
      {"desc\n1\n-.5\n", 3},
      {"desc\n1\nC#\n", 3},                     // not a pitch
      {"desc\n1\n+3/2\n", 3},                   // signed ratio
  };
  for (const Bad& b : bad) {
    CAPTURE(b.text);
    const auto r = parseScl(b.text, "bad.scl");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == pq::ErrorCode::Parse);
    CHECK(r.error().location.file == "bad.scl");
    if (b.line != 0) CHECK(r.error().location.line == b.line);
  }
  // Cents too large for a double.
  const std::string huge = "desc\n1\n1" + std::string(400, '0') + ".0\n";
  CHECK_FALSE(parseScl(huge).ok());
}

TEST_CASE("scl: canonical writer") {
  ScalaScale s;
  s.description = "Test scale";
  s.pitches = {R(9, 8), C(386.3137), C(-5.0), C(700.0), R(3, 1), L("100000000000000000000", "3")};
  const auto text = writeScl(s, "test.scl");
  REQUIRE(text.ok());
  CHECK(*text ==
        "! test.scl\r\n!\r\nTest scale\r\n 6\r\n!\r\n 9/8\r\n 386.3137\r\n -5.0\r\n 700.0\r\n 3/1\r\n"
        " 100000000000000000000/3\r\n");
  checkRoundTrip(s);
  // Doubles that need all 17 significant digits, tiny and huge values still read back exactly.
  ScalaScale t;
  t.description = "precision";
  t.pitches = {C(0.1 + 0.2), C(1e-300), C(-1e300), C(701.9550008653874), C(5e-324)};
  checkRoundTrip(t);

  ScalaScale badDesc;
  badDesc.description = "two\nlines";
  CHECK_FALSE(writeScl(badDesc).ok());
  badDesc.description = "! looks like a comment";
  CHECK_FALSE(writeScl(badDesc).ok());
  ScalaScale badPitch;
  badPitch.pitches = {C(std::nan(""))};
  CHECK_FALSE(writeScl(badPitch).ok());
  badPitch.pitches = {L("12a", "1")};
  CHECK_FALSE(writeScl(badPitch).ok());
}

TEST_CASE("scl: Latin-1 descriptions convert to UTF-8") {
  ScalaScale s;
  s.description = "temp\xE9rament L\xFC";
  CHECK(s.descriptionUtf8() == "temp\xC3\xA9rament L\xC3\xBC");
  s.description = "already UTF-8: L\xC3\xBC \xE2\x9F\xA9";
  CHECK(s.descriptionUtf8() == s.description);
  s.description = "\xC3";  // truncated sequence: Latin-1
  CHECK(s.descriptionUtf8() == "\xC3\x83");
}

// ---- Fixture files ---------------------------------------------------------------------------------------------
// Authored for pqdaw by tools/fixtures/gen_scl_fixtures.mjs: factual pitch sets laid out to cover the format's edge
// cases (tests/tuning/fixtures/scl/SOURCES.txt lists what each file covers).

namespace {

struct FixtureFile {
  const char* name;
  const char* description;  // right-trimmed, as UTF-8
  size_t count;
  ScalaPitch first;
  ScalaPitch last;
};

std::vector<FixtureFile> fixtureFiles() {
  return {
      {"big_ratios.scl", "Powers of three over powers of two, beyond 64-bit", 5, R(531441, 524288), R(2, 1)},
      {"bp_equal.scl", "Bohlen-Pierce: 13 equal steps of 3/1", 13, C(146.30423), R(3, 1)},
      {"bp_just.scl", "Bohlen-Pierce just tuning, odd harmonics 3 5 7", 13, R(27, 25), R(3, 1)},
      {"cents_units.scl", "Major scale in 12-EDO cents with units", 7, C(200.0), R(2, 1)},
      {"descending.scl", "Descending scale with formal octave 1/2", 3, R(8, 9), R(1, 2)},
      {"edo12.scl", "12 equal divisions of the octave", 12, C(100.0), R(2, 1)},
      {"edo15_trailing_dot.scl", "15-EDO with cents written as 80.", 15, C(80.0), R(2, 1)},
      {"edo19_lf.scl", "19-EDO, LF line ends", 19, C(63.157895), R(2, 1)},
      {"edo22.scl", "22 equal divisions of the octave", 22, C(54.54545), R(2, 1)},
      {"edo31_tabs.scl", "31-EDO written with tabs", 31, C(38.70968), R(2, 1)},
      {"edo53_annotated.scl", "53-EDO with step labels", 53, C(22.641509), R(2, 1)},
      {"edo7_nofinalnl.scl", "7-EDO, file ends without a newline", 7, C(171.42857), R(2, 1)},
      {"empty_description.scl", "", 2, R(3, 2), R(2, 1)},
      {"fifth_by_nine.scl", "Pure fifth divided in nine, two fifths repeat", 18, C(77.995), C(1403.91)},
      {"harm_subharm16.scl", "Harmonics 2-16 and subharmonics 2-15, formal octave 1/1", 30, R(2, 1), R(1, 1)},
      {"harmonics16_32.scl", "Harmonic series segment 16 to 32, unreduced", 16, R(17, 16), R(2, 1)},
      {"hexany_raw.scl", "Hexany 1.3.5.7 as raw pair products over 3", 6, R(5, 3), R(2, 1)},
      {"just5_12.scl", "5-limit just chromatic scale on C", 12, R(16, 15), R(2, 1)},
      {"latin1.scl", "Gamme temp\xC3\xA9r\xC3\xA9" "e \xC3\xA0 douze demi-tons \xC3\xA9gaux", 12, C(100.0), R(2, 1)},
      {"meantone_qc.scl", "Quarter-comma meantone, Eb to G#", 12, C(76.049), R(2, 1)},
      {"negative.scl", "Pitches below and above the unison", 5, C(-240.0), C(1200.0)},
      {"partch43.scl", "Partch's 43-tone monophonic scale (ratios)", 43, R(81, 80), R(2, 1)},
      {"powers_10_9.scl", "Powers of 10/9 up to the 14th", 14, R(10, 9), R(100000000000000, 22876792454961)},
      {"ptolemy_diatonic.scl", "Ptolemy's intense diatonic (5-limit major)", 7, R(9, 8), R(2, 1)},
      {"pythagorean12.scl", "Pythagorean chain of 3/2 fifths, 12 notes", 12, R(2187, 2048), R(2, 1)},
      {"stretched10.scl", "10 equal steps of a 1202.5 cent octave", 10, C(120.25), C(1202.5)},
      {"tetrachord.scl", "Diatonic tetrachord 16/15 9/8 10/9, repeats at 4/3", 3, R(16, 15), R(4, 3)},
      {"unreduced_annotated.scl", "Pythagorean diatonic written with unreduced ratios", 7, R(9, 8), R(2, 1)},
  };
}

}  // namespace

TEST_CASE("scl: 28 fixture files parse, build a Scale and round-trip") {
  const auto files = fixtureFiles();
  CHECK(files.size() == 28);  // T2.4 asks for 20+
  for (const FixtureFile& f : files) {
    CAPTURE(f.name);
    const std::string text = readFixture(f.name);
    const auto r = parseScl(text, f.name);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    const ScalaScale& s = *r;
    CHECK(rtrim(s.descriptionUtf8()) == f.description);
    REQUIRE(s.pitches.size() == f.count);
    CHECK(s.pitches.front() == f.first);
    CHECK(s.pitches.back() == f.last);
    checkRoundTrip(s);
    const auto scale = Scale::make(s);
    REQUIRE(scale.ok());
    CHECK(scale->size() == static_cast<int32_t>(f.count));
    CHECK(scale->equaveCents() == f.last.cents);
  }
}

TEST_CASE("scl: fixture details") {
  // Unreduced ratios with '!' annotations after the values and comment lines between pitches.
  ScalaScale s = *parseScl(readFixture("unreduced_annotated.scl"));
  CHECK(s.pitches == std::vector<ScalaPitch>{R(9, 8), R(81, 64), R(4, 3), R(3, 2), R(27, 16), R(243, 128), R(2, 1)});
  s = *parseScl(readFixture("harmonics16_32.scl"));
  CHECK(s.pitches[1] == R(9, 8));  // written 18/16
  // Comment lines between pitch lines, note names after the values.
  s = *parseScl(readFixture("just5_12.scl"));
  CHECK(s.pitches[5] == R(45, 32));
  CHECK(s.pitches[6] == R(3, 2));
  // Tritave equave.
  s = *parseScl(readFixture("bp_just.scl"));
  CHECK(s.pitches.back().cents == doctest::Approx(1901.955000865387).epsilon(1e-15));
  // Three of the five ratios exceed int64.
  s = *parseScl(readFixture("big_ratios.scl"));
  int large = 0;
  for (const ScalaPitch& p : s.pitches) large += p.kind == ScalaPitch::Kind::LargeRatio;
  CHECK(large == 3);
  CHECK(s.pitches[1] == L("12157665459056928801", "9223372036854775808"));
  CHECK(s.pitches[2] == L("36893488147419103232", "36472996377170786403"));
  CHECK(s.pitches[3] == L("19383245667680019896796723", "19342813113834066795298816"));
  // Large-ratio cents from the digits: 3^40/2^63 = 40·1200·log2 3 − 63·1200; Mercator's comma 3^53/2^84.
  CHECK(s.pitches[1].cents == doctest::Approx(40 * 1200.0 * std::log2(3.0) - 63 * 1200.0).epsilon(1e-12));
  CHECK(s.pitches[3].cents == doctest::Approx(53 * 1200.0 * std::log2(3.0) - 84 * 1200.0).epsilon(1e-9));
  // "200.000 cents" style lines.
  s = *parseScl(readFixture("cents_units.scl"));
  CHECK(s.pitches[2] == C(500.0));
  // No final newline, and comment lines that look like pitches after the counted ones.
  s = *parseScl(readFixture("edo7_nofinalnl.scl"));
  CHECK(s.pitches.size() == 7);
  // Comment between description and count, count with a trailing space, bare integers, pitches above the octave.
  s = *parseScl(readFixture("hexany_raw.scl"));
  CHECK(s.pitches == std::vector<ScalaPitch>{R(5, 3), R(7, 3), R(5, 1), R(7, 1), R(35, 3), R(2, 1)});
  // Bare integer equave.
  s = *parseScl(readFixture("ptolemy_diatonic.scl"));
  CHECK(s.pitches.back() == R(2, 1));
  // The quarter-comma meantone file agrees with its definition (fifth = 5^(1/4)) to the 5 written decimals.
  s = *parseScl(readFixture("meantone_qc.scl"));
  const double fifth = 1200.0 * std::log2(5.0) / 4.0;
  CHECK(std::fabs(s.pitches[6].cents - fifth) <= 5e-6);                 // G
  CHECK(std::fabs(s.pitches[1].cents - (2 * fifth - 1200.0)) <= 5e-6);  // D
  CHECK(std::fabs(s.pitches[0].cents - (7 * fifth - 4800.0)) <= 5e-6);  // C#
}

// ---- .kbm ------------------------------------------------------------------------------------------------------

namespace {

// The standard 12-key mapping (the values of Scala's documented template; comments are ours).
constexpr const char* kTemplateKbm =
    "! standard.kbm\n!\n! pattern length in keys\n12\n"
    "! lowest key retuned\n0\n! highest key retuned\n127\n"
    "! key that plays degree 0\n60\n"
    "! reference key\n69\n"
    "! reference key frequency in Hz\n440.0\n"
    "! degrees per pattern repetition\n"
    "!\n12\n! degree for each key of the pattern\n"
    "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n";

}  // namespace

TEST_CASE("kbm: the standard mapping parses; linear mapping") {
  const auto k = parseKbm(kTemplateKbm, "standard.kbm");
  REQUIRE(k.ok());
  CHECK(k->mapSize == 12);
  CHECK(k->firstKey == 0);
  CHECK(k->lastKey == 127);
  CHECK(k->middleKey == 60);
  CHECK(k->referenceKey == 69);
  CHECK(k->referenceHz == 440.0);
  CHECK(k->formalOctaveDegree == 12);
  CHECK(k->mapping == std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
  CHECK(mapKeyToDegree(*k, 60) == 0);
  CHECK(mapKeyToDegree(*k, 72) == 12);
  CHECK(mapKeyToDegree(*k, 59) == -1);
  CHECK(mapKeyToDegree(*k, 0) == -60);
  CHECK(mapKeyToDegree(*k, 128) == std::nullopt);  // outside [first, last]
  CHECK(mapKeyToDegree(*k, -1) == std::nullopt);

  const KeyboardMapping lin = KeyboardMapping::linear();
  CHECK(mapKeyToDegree(lin, 61) == 1);
  CHECK(mapKeyToDegree(lin, 0) == -60);
}

TEST_CASE("kbm: unmapped keys, short mappings, formal octave, mapKey") {
  // A 7-note scale on the white keys: black keys unmapped ('x' or 'X'), the pattern moves 7 degrees per 12 keys.
  const auto k = parseKbm("12\n0\n127\n60\n69\n440\n7\n0\nx\n1\nx\n2\n3\nX\n4\nx\n5\nx\n6\n");
  REQUIRE(k.ok());
  CHECK(k->mapping.size() == 12);
  CHECK(mapKeyToDegree(*k, 60) == 0);
  CHECK(mapKeyToDegree(*k, 61) == std::nullopt);
  CHECK(mapKeyToDegree(*k, 64) == 2);
  CHECK(mapKeyToDegree(*k, 72) == 7);
  CHECK(mapKeyToDegree(*k, 59) == -1);  // B below middle C: 6 − 7
  CHECK(mapKeyToDegree(*k, 69) == 5);
  CHECK(mapKey(*k, 59, 7) == PitchId::fromDegree(6, -1));
  CHECK(mapKey(*k, 74, 7) == PitchId::fromDegree(1, 1));
  CHECK(mapKey(*k, 61, 7) == std::nullopt);
  CHECK(mapKey(*k, 60, 0) == std::nullopt);

  // Missing tail entries are unmapped; degrees can be negative or beyond the scale size.
  const auto s = parseKbm("! c\n5\n10\n20\n12\n12\n100.5 Hz\n-3\n0\n-1\n9\n");
  REQUIRE(s.ok());
  CHECK(s->mapping == std::vector<int32_t>{0, -1, 9});
  CHECK(mapKeyToDegree(*s, 12) == 0);
  CHECK(mapKeyToDegree(*s, 13) == -1);
  CHECK(mapKeyToDegree(*s, 14) == 9);
  CHECK(mapKeyToDegree(*s, 15) == std::nullopt);
  CHECK(mapKeyToDegree(*s, 17) == -3);           // next pattern: entry 0 + 1·(−3)
  CHECK(mapKeyToDegree(*s, 18) == -4);           // entry 1 + 1·(−3)
  CHECK(mapKeyToDegree(*s, 10) == std::nullopt);  // (10 − 12) mod 5 = entry 3: left out, so unmapped
  CHECK(mapKeyToDegree(*s, 19) == 6);            // entry 2 + 1·(−3)
  CHECK(mapKeyToDegree(*s, 20) == std::nullopt);  // entry 3 again
  CHECK(mapKeyToDegree(*s, 9) == std::nullopt);   // below firstKey
}

TEST_CASE("kbm: errors and round trip") {
  CHECK_FALSE(parseKbm("").ok());
  CHECK_FALSE(parseKbm("12\n0\n127\n60\n69\n").ok());                 // missing frequency
  CHECK_FALSE(parseKbm("12\n0\n127\n60\n69\n0\n12\n").ok());          // frequency must be positive
  CHECK_FALSE(parseKbm("12\n0\n127\n60\n69\nabc\n12\n").ok());
  CHECK_FALSE(parseKbm("-1\n0\n127\n60\n69\n440\n12\n").ok());        // negative size
  CHECK_FALSE(parseKbm("12\n100\n0\n60\n69\n440\n12\n").ok());        // first > last
  CHECK_FALSE(parseKbm("12\n0\n127\n60\n69\n440\n12\n0\n1\n?\n").ok());  // bad entry
  // Reference key unmapped: explicitly ('x') or because the mapping stops before it.
  CHECK_FALSE(parseKbm("12\n0\n127\n60\n69\n440\n12\n0\n1\n2\n3\n4\n5\n6\n7\n8\nx\n").ok());
  CHECK_FALSE(parseKbm("12\n0\n127\n60\n69\n440\n12\n0\n1\n2\n").ok());

  KeyboardMapping k = *parseKbm(kTemplateKbm);
  k.mapping[3] = kUnmappedKey;
  k.referenceHz = 432.123456789;
  auto text = writeKbm(k);
  REQUIRE(text.ok());
  auto back = parseKbm(*text);
  REQUIRE(back.ok());
  CHECK(*back == k);
  const KeyboardMapping lin = KeyboardMapping::linear(48, 57, 220.0);
  text = writeKbm(lin);
  REQUIRE(text.ok());
  CHECK(*parseKbm(*text) == lin);

  KeyboardMapping bad = k;
  bad.mapping.push_back(1);  // 13 entries in a size-12 map
  CHECK_FALSE(writeKbm(bad).ok());
  bad = k;
  bad.mapping[9] = kUnmappedKey;  // reference key 69 = entry 9
  CHECK_FALSE(writeKbm(bad).ok());
}

TEST_CASE("scala: parseScalaRatio reads one ratio exactly like a .scl pitch line (T3.5)") {
  auto r = parseScalaRatio("10/8");
  REQUIRE(r.ok());
  CHECK(r->kind == ScalaPitch::Kind::Ratio);
  CHECK(r->ratio == Rational(5, 4));
  r = parseScalaRatio("3");
  REQUIRE(r.ok());
  CHECK(r->ratio == Rational(3));
  r = parseScalaRatio("00123456789012345678901234/98765432109876543210");
  REQUIRE(r.ok());
  CHECK(r->kind == ScalaPitch::Kind::LargeRatio);
  CHECK(r->largeNum == "123456789012345678901234");
  CHECK(r->largeDen == "98765432109876543210");
  const auto parsed = parseScl("x\n1\n123456789012345678901234/98765432109876543210\n");
  REQUIRE(parsed.ok());
  CHECK(*r == parsed->pitches[0]);
  CHECK(r->cents == parsed->pitches[0].cents);
  for (const char* bad : {"", "/", "5/", "/4", "-5/4", "+5/4", "5/4 ", " 5/4", "5.0", "5/4/3", "0/1", "1/0", "a/b"}) {
    CAPTURE(bad);
    CHECK_FALSE(parseScalaRatio(bad).ok());
  }
}

TEST_CASE("scala: validateKbm makes the checks writeKbm makes") {
  KeyboardMapping k = KeyboardMapping::linear();
  CHECK(validateKbm(k).ok());
  k.firstKey = 100;
  k.lastKey = 10;
  CHECK_FALSE(validateKbm(k).ok());
  CHECK_FALSE(writeKbm(k).ok());
  k = KeyboardMapping::linear();
  k.mapSize = 2;
  k.mapping = {0, kUnmappedKey};
  k.middleKey = 60;
  k.referenceKey = 61;
  CHECK_FALSE(validateKbm(k).ok());
  k.referenceKey = 62;
  CHECK(validateKbm(k).ok());
}
