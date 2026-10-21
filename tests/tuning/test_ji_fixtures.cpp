// The fewest-keypress BFS and interval naming against the ORIGINAL JI composition trainer JavaScript, run under Node
// (T2.5 acceptance). Fixtures: tools/fixtures/gen_ji_fixtures.mjs → tests/tuning/fixtures/ji_bfs.json, ji_names.json.
// Also checks the gallery table generated from "interval names.txt" (tools/fixtures/gen_ji_names_cpp.mjs).

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pq/tuning/fewest_keypress.h"
#include "pq/tuning/interval_names.h"
#include "pq/tuning/ji.h"

using namespace pq::tuning;
using Json = nlohmann::json;

namespace {

Json load(const char* name) {
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/tuning/fixtures/" + name;
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "missing fixture " << path);
  return Json::parse(in);
}

std::vector<KeyRatio> keyList(const Json& j) {
  std::vector<KeyRatio> out;
  for (const Json& e : j) out.push_back({e[0].get<std::string>(), Rational(e[1].get<int64_t>(), e[2].get<int64_t>())});
  return out;
}

}  // namespace

TEST_CASE("ji fixtures: JS object key order of the merged keymaps") {
  const Json f = load("ji_bfs.json");
  size_t n = 0;
  for (const auto& [name, km] : f["keymaps"].items()) {
    CAPTURE(name);
    std::vector<KeyRatio> insertions = keyList(km["prime"]);
    for (KeyRatio& k : keyList(km["reciprocal"])) insertions.push_back(std::move(k));
    CHECK(jsObjectKeyOrder(insertions) == keyList(km["order"]));
    ++n;
  }
  CHECK(n == 4);
  // The C++ default keymap is the trainer's, as the BFS iterates it ('0' = 29/16 first).
  CHECK(jsObjectKeyOrder(trainerDefaultKeymap()) == keyList(f["keymaps"]["default"]["order"]));
}

TEST_CASE("ji fixtures: ratioToEdoSteps matches jiRatioToEdoSteps") {
  const Json f = load("ji_bfs.json");
  for (const Json& c : f["steps"]) {
    CAPTURE(c.dump());
    CHECK(ratioToEdoSteps(Rational(c[1].get<int64_t>(), c[2].get<int64_t>()), c[0].get<uint32_t>()) ==
          c[3].get<int64_t>());
  }
}

TEST_CASE("ji fixtures: fewest-keypress BFS matches prBuildStepToRatioMap for every case") {
  const Json f = load("ji_bfs.json");
  std::map<std::string, std::vector<KeyRatio>> ordered;
  for (const auto& [name, km] : f["keymaps"].items()) ordered[name] = keyList(km["order"]);

  size_t cases = 0, labels = 0;
  for (const Json& c : f["cases"]) {
    const std::string km = c["keymap"];
    const uint32_t edo = c["edo"];
    const uint64_t limit = c["limit"];
    CAPTURE(km);
    CAPTURE(edo);
    CAPTURE(limit);
    const std::vector<KeyRatio>& keys = ordered.at(km);
    const auto map = FewestKeypressMap::build(keys, edo, limit);
    REQUIRE(map.ok());
    // Same set of steps, same ratio string for each.
    CHECK(map->size() == c["labels"].size());
    for (const Json& l : c["labels"]) {
      const int64_t step = l[0];
      CAPTURE(step);
      CHECK(map->ratioLabel(step) == l[1].get<std::string>());
      ++labels;
    }
    // The recorded key sequence reproduces each entry: as many presses as counted, product = ratio, steps add up.
    for (int64_t s = -map->maxRange(); s <= map->maxRange(); ++s) {
      const FewestKeypressMap::Entry* e = map->find(s);
      if (!e) continue;
      const std::vector<uint32_t> seq = map->keySequence(s);
      CHECK(static_cast<int32_t>(seq.size()) == e->presses);
      Rational prod{1};
      int64_t steps = 0;
      for (uint32_t k : seq) {
        prod = *mul(prod, keys[k].ratio);
        steps += ratioToEdoSteps(keys[k].ratio, edo);
      }
      CHECK(prod == e->ratio);
      CHECK(steps == s);
    }
    ++cases;
  }
  MESSAGE("compared " << cases << " BFS cases, " << labels << " step labels");
  CHECK(cases == 600);
}

TEST_CASE("fewest-keypress map: labels, sequences and edge cases") {
  const auto keys = jsObjectKeyOrder(trainerDefaultKeymap());
  const auto m = FewestKeypressMap::build(keys, 12, 13);
  REQUIRE(m.ok());
  CHECK(m->maxRange() == 24);
  CHECK(m->ratioLabel(7) == "3/2");
  CHECK(m->ratioLabel(0) == "1/1");
  CHECK(m->cursorLabel(7) == "3/2");
  CHECK(m->find(25) == nullptr);
  CHECK(m->find(-25) == nullptr);
  CHECK(m->cursorLabel(30) == "+30\\12");
  CHECK(m->cursorLabel(-30) == "-30\\12");
  CHECK(m->keySequence(0).empty());
  // 12 steps = '1' (2/1); '1' is keys[1] in JS order ('0' = 29/16 comes first).
  CHECK(keys[0].key == "0");
  const auto seq = m->keySequence(12);
  REQUIRE(seq.size() == 1);
  CHECK(keys[seq[0]].key == "1");
  // Prime limit 2: only octaves, so only multiples of 12 steps are reachable.
  const auto oct = FewestKeypressMap::build(keys, 12, 2);
  REQUIRE(oct.ok());
  CHECK(oct->size() == 5);
  CHECK(oct->ratioLabel(-24) == "1/4");
  CHECK(oct->cursorLabel(5) == "+5\\12");
  // Invalid input.
  CHECK_FALSE(FewestKeypressMap::build(keys, 0, 13).ok());
  CHECK_FALSE(FewestKeypressMap::build(keys, FewestKeypressMap::kMaxEdo + 1, 13).ok());
  const std::vector<KeyRatio> negative = {{"a", Rational(-3, 2)}};
  CHECK_FALSE(FewestKeypressMap::build(negative, 12, 13).ok());
  // A product that overflows int64 is skipped instead of going inexact (the JS would continue with rounded doubles):
  // a = P/(P − 2^24) with P = 2^31 − 1 (prime) moves 11 steps of 1000-EDO; a² fits in int64, a³ does not.
  const std::vector<KeyRatio> big = {{"a", Rational(2147483647, 2147483647 - (int64_t{1} << 24))}};
  const auto h = FewestKeypressMap::build(big, 1000, UINT64_MAX);
  REQUIRE(h.ok());
  REQUIRE(h->moves().size() == 1);
  CHECK(h->moves()[0].steps == 11);
  REQUIRE(h->find(22) != nullptr);
  CHECK(h->find(22)->ratio == *pow(big[0].ratio, 2));
  CHECK(h->find(33) == nullptr);
  CHECK(h->find(-11) == nullptr);
}

TEST_CASE("ji fixtures: getIntervalName port matches the JS exactly") {
  const Json f = load("ji_names.json");
  size_t n = 0, named = 0;
  for (const Json& c : f["cases"]) {
    const int64_t num = c[0], den = c[1];
    CAPTURE(num);
    CAPTURE(den);
    const std::string want = c[2];
    CHECK(trainerIntervalName(num, den) == want);
    ++n;
    named += !want.empty();
  }
  MESSAGE("compared " << n << " names (" << named << " non-empty)");
  CHECK(n > 1600);
  CHECK(named > 100);
  CHECK(trainerIntervalNames().size() == 68);
}

TEST_CASE("interval gallery: table integrity and lookups") {
  const auto g = jiIntervalGallery();
  CHECK(g.size() == 425);
  for (size_t i = 0; i < g.size(); ++i) {
    const JiIntervalInfo& e = g[i];
    CAPTURE(e.num);
    CAPTURE(e.den);
    // Ratios are reduced and strictly ascending (the generator checked them against the listed cents).
    REQUIRE(Rational::tryMake(e.num, e.den) == e.ratio());
    if (i > 0) CHECK(g[i - 1].ratio() < e.ratio());
    CHECK_FALSE(e.colourAbbrev.empty());
    CHECK(findJiInterval(e.ratio()) == &e);
  }
  CHECK(findJiInterval(Rational(1000003, 1000000)) == nullptr);
  CHECK(findJiInterval(Rational(1, 2)) == nullptr);  // the gallery lists ascending intervals only

  const JiIntervalInfo* m3 = findJiInterval(Rational(5, 4));
  REQUIRE(m3);
  CHECK(m3->colourAbbrev == "y3");
  CHECK(m3->colourName == "yo 3rd");
  CHECK(m3->fjs == "M35");
  CHECK(m3->commonNames == "classic major third, just major third, octave-reduced 5th harmonic");
  const JiIntervalInfo* sept = findJiInterval(Rational(7, 4));
  REQUIRE(sept);
  CHECK(sept->colourAbbrev == "z7");
  CHECK(sept->colourName == "zo 7th");
  const JiIntervalInfo* tritave = findJiInterval(Rational(3));
  REQUIRE(tritave);
  CHECK(tritave->colourAbbrev == "w12, cw5");
  CHECK(tritave->colourName == "wa 12th, cowa 5th");
  CHECK(tritave->fjs == "P12");
  // Rows whose colour column was "name, abbreviation" are normalised.
  const JiIntervalInfo* swapped = findJiInterval(Rational(27, 23));
  REQUIRE(swapped);
  CHECK(swapped->colourAbbrev == "23u2");
  CHECK(swapped->colourName == "twethu 2nd");
  // Abbreviation-only rows.
  const JiIntervalInfo* h83 = findJiInterval(Rational(83, 64));
  REQUIRE(h83);
  CHECK(h83->colourAbbrev == "83o4");
  CHECK(h83->colourName.empty());
}
