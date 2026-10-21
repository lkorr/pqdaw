// The C++ MOS port against the original mos-explorer JavaScript, run under Node (T2.3 acceptance):
// every number within 1e-6 cents, every string exactly. Fixtures: tools/fixtures/gen_mos_fixtures.mjs.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "mos_fixture.h"
#include "pq/tuning/mos.h"
#include "pq/tuning/mos_data.h"
#include "pq/tuning/mos_regions.h"

using namespace pq::tuning;
using mos_fixture::doubles;
using mos_fixture::hint;
using mos_fixture::ints;
using mos_fixture::Json;
using mos_fixture::near;
using mos_fixture::nearAll;
using mos_fixture::optString;

namespace {

const RegionHint* ptr(const std::optional<RegionHint>& h) { return h ? &*h : nullptr; }

std::optional<MosKey> optKey(const Json& j) {
  if (j.is_null()) return std::nullopt;
  return parseMosKey(j.get<std::string>());
}

}  // namespace

TEST_CASE("mos fixtures: buildMOSScale, chainSafeGen, computeHardness, chain, brightness") {
  const Json f = mos_fixture::load("mos_scale.json");
  size_t count = 0;
  for (const Json& c : f["cases"]) {
    const double g = c["g"], P = c["P"];
    const int32_t n = c["n"];
    const auto sel = hint(c["sel"]);
    CAPTURE(count);
    CAPTURE(g);
    CAPTURE(n);
    CAPTURE(P);
    CHECK(near(chainSafeGenerator(g, n, P, ptr(sel)), c["safeGen"]));
    const MosScale sc = buildMosScale(g, n, P, ptr(sel));
    CHECK(nearAll(sc.pitches, c["pitches"]));
    CHECK(nearAll(sc.steps, c["steps"]));
    CHECK(near(sc.L, c["L"]));
    CHECK(near(sc.s, c["s"]));
    const Hardness h = computeHardness(g, n, P);
    CHECK(near(h.L, c["hardness"]["L"]));
    CHECK(near(h.s, c["hardness"]["s"]));
    CHECK(near(h.ratio, c["hardness"]["ratio"]));
    CHECK(h.str == c["hardness"]["str"].get<std::string>());
    CHECK(generatorChain(g, n, P, ptr(sel)) == ints(c["chain"]));
    CHECK(isBrightGenerator(g, n, P, ptr(sel)) == c["bright"].get<bool>());
    ++count;
  }
  MESSAGE("mos_scale.json cases compared: " << count);
  CHECK(count >= 1000);
}

TEST_CASE("mos fixtures: rotation, UDP, GMN, step pattern, degree map, mode names") {
  const Json f = mos_fixture::load("mos_modes.json");
  size_t count = 0;
  for (const Json& c : f["cases"]) {
    const double g = c["g"], P = c["P"];
    const int32_t n = c["n"], rot = c["rot"];
    const auto sel = hint(c["sel"]);
    CAPTURE(count);
    CAPTURE(g);
    CAPTURE(n);
    CAPTURE(rot);
    CAPTURE(P);
    const MosScale r = rotateMosScale(buildMosScale(g, n, P, ptr(sel)), rot, P);
    CHECK(nearAll(r.pitches, c["pitches"]));
    CHECK(near(r.L, c["L"]));
    CHECK(near(r.s, c["s"]));
    CHECK(computeUdp(g, n, rot, P, ptr(sel)) == optString(c["udp"]));
    const auto gmn = computeGmn(g, n, rot, P, ptr(sel));
    if (c["gmn"].is_null()) CHECK_FALSE(gmn.has_value());
    else CHECK(gmn == c["gmn"].get<int32_t>());
    CHECK(computeStepPattern(g, n, rot, P, ptr(sel)) == c["pattern"].get<std::string>());
    CHECK(rotationDegreeMap(g, n, rot, P, ptr(sel)) == ints(c["degreeMap"]));
    CHECK(rotateByGenerator(g, n, rot, 1, P, ptr(sel)) == c["genUp"].get<int32_t>());
    CHECK(rotateByGenerator(g, n, rot, -1, P, ptr(sel)) == c["genDown"].get<int32_t>());
    if (const auto key = optKey(c["key"])) {
      const auto name = modeName(*key, g, rot, P, ptr(sel));
      CHECK(name.has_value() == !c["modeName"].is_null());
      if (name && !c["modeName"].is_null()) CHECK(std::string(*name) == c["modeName"].get<std::string>());
    }
    ++count;
  }
  MESSAGE("mos_modes.json (generator, size, period, mode) cases compared: " << count);
  CHECK(count >= 2000);
}

TEST_CASE("mos fixtures: MODMOS, alteration labels, mode buttons") {
  const Json f = mos_fixture::load("mos_modmos.json");
  size_t count = 0;
  for (const Json& c : f["cases"]) {
    const double g = c["g"], P = c["P"];
    const int32_t n = c["n"], rot = c["rot"];
    const auto sel = hint(c["sel"]);
    const std::vector<double> modmos = doubles(c["modmos"]);
    CAPTURE(count);
    const MosScale rotated = rotateMosScale(buildMosScale(g, n, P, ptr(sel)), rot, P);
    const ModmosScale mm = applyModmos(rotated, modmos, P);
    CHECK(nearAll(mm.pitches, c["pitches"]));
    CHECK(nearAll(mm.steps, c["steps"]));
    CHECK(near(mm.L, c["L"]));
    CHECK(near(mm.s, c["s"]));
    if (c["alts"].is_null()) {
      CHECK(mm.alts.empty());
      CHECK(mm.origDegs.empty());
    } else {
      CHECK(nearAll(mm.alts, c["alts"]));
      CHECK(mm.origDegs == ints(c["origDegs"]));
    }
    CHECK(formatModmosAlterations(modmos) == c["label"].get<std::string>());
    CHECK(hasModmos(modmos) == c["hasModmos"].get<bool>());
    const std::string a = c["action"];
    const ModeAction action = a == "step-up"     ? ModeAction::StepUp
                              : a == "step-down" ? ModeAction::StepDown
                              : a == "gen-up"    ? ModeAction::GenUp
                                                 : ModeAction::GenDown;
    const ModeChange ch = applyModeAction(action, g, n, rot, modmos, P, ptr(sel));
    CHECK(ch.rotation == c["newRot"].get<int32_t>());
    CHECK(nearAll(ch.modmos, c["newModmos"]));
    ++count;
  }
  MESSAGE("mos_modmos.json cases compared: " << count);
  CHECK(count >= 300);
}

TEST_CASE("mos fixtures: generator sequences") {
  const Json f = mos_fixture::load("mos_genseq.json");
  size_t count = 0;
  for (const Json& c : f["cases"]) {
    const std::vector<double> seq = doubles(c["seq"]);
    const double P = c["P"];
    const int32_t n = c["n"];
    CAPTURE(count);
    const MosScale sc = buildGenSequenceScale(seq, n, P);
    CHECK(nearAll(sc.pitches, c["pitches"]));
    CHECK(nearAll(sc.steps, c["steps"]));
    CHECK(near(sc.L, c["L"]));
    CHECK(near(sc.s, c["s"]));
    std::vector<double> edited = seq;
    updateGenSequenceEntry(edited, c["editIdx"].get<size_t>(), c["editCents"].get<double>());
    CHECK(nearAll(edited, c["edited"]));
    ++count;
  }
  MESSAGE("mos_genseq.json cases compared: " << count);
  CHECK(count >= 300);
}

TEST_CASE("mos fixtures: transferModeRotation") {
  const Json f = mos_fixture::load("mos_transfer.json");
  size_t count = 0;
  for (const Json& c : f["cases"]) {
    const auto sel = hint(c["sel"]);
    CAPTURE(count);
    CHECK(transferModeRotation(c["oldG"], c["oldN"], c["oldRot"], c["newG"], c["newN"], c["P"], ptr(sel)) ==
          c["newRot"].get<int32_t>());
    ++count;
  }
  MESSAGE("mos_transfer.json cases compared: " << count);
  CHECK(count >= 600);
}

TEST_CASE("mos fixtures: updateHeldNotes across generator moves") {
  const Json f = mos_fixture::load("mos_held.json");
  size_t scenarios = 0, steps = 0;
  for (const Json& sc : f["scenarios"]) {
    const double P = sc["P"], viewLimit = sc["viewLimit"], stretch = sc["stretch"];
    std::vector<HeldNote> notes;
    for (const Json& v : sc["voices"]) notes.push_back({v["deg"].get<int32_t>(), v["oct"].get<int32_t>(), std::nullopt});
    int32_t heldScaleSize = -1;
    for (const Json& st : sc["steps"]) {
      CAPTURE(scenarios);
      CAPTURE(steps);
      MosParams p;
      p.generator = st["g"];
      p.n = st["n"];
      p.period = P;
      p.periodsPerEquave = periodRepetitions(viewLimit, P);
      p.rotation = st["rot"];
      p.modmos = doubles(st["modmos"]);
      p.octaveStretch = stretch;
      p.region = hint(st["sel"]);
      const FullScale full = buildFullScale(p);
      CHECK(nearAll(full.pitches, st["fullPitches"]));
      CHECK(full.size == st["fullSize"].get<int32_t>());
      CHECK(near(full.equaveCents, st["octave"]));
      updateHeldNotes(notes, full, heldScaleSize);
      CHECK(heldScaleSize == st["heldScaleSize"].get<int32_t>());
      REQUIRE(notes.size() == st["voices"].size());
      for (size_t i = 0; i < notes.size(); ++i) {
        const Json& v = st["voices"][i];
        CHECK(notes[i].degree == v["deg"].get<int32_t>());
        CHECK(notes[i].octave == v["oct"].get<int32_t>());
        REQUIRE(notes[i].currentCents.has_value());
        CHECK(near(*notes[i].currentCents, v["cents"]));
        CHECK(near(stretchCents(*notes[i].currentCents, stretch), v["tuned"]));
      }
      ++steps;
    }
    ++scenarios;
  }
  MESSAGE("mos_held.json scenarios/steps compared: " << scenarios << "/" << steps);
  CHECK(scenarios >= 150);
}

TEST_CASE("mos fixtures: buildMOSRegions, equalized end, adjacent region") {
  const Json f = mos_fixture::load("mos_regions.json");
  size_t regionsCompared = 0;
  for (const Json& set : f["sets"]) {
    const double P = set["P"];
    const int32_t maxSize = set["maxSize"], maxDepth = set["maxDepth"];
    const int32_t reps = periodRepetitions(set["viewLimit"], P);
    CAPTURE(P);
    CAPTURE(maxSize);
    const MosRegionSet rs = buildMosRegions(maxSize, maxDepth, P, reps);
    REQUIRE(rs.regions.size() == set["regions"].size());
    for (size_t i = 0; i < rs.regions.size(); ++i) {
      const MosRegion& r = rs.regions[i];
      const Json& e = set["regions"][i];
      CAPTURE(i);
      CHECK(mosKeyString(r.key) == e["key"].get<std::string>());
      CHECK(r.key.nL == e["nL"].get<int32_t>());
      CHECK(r.key.nS == e["nS"].get<int32_t>());
      CHECK(r.size == e["size"].get<int32_t>());
      CHECK(mosKeyString(r.perPeriodKey) == e["perPeriodKey"].get<std::string>());
      CHECK(r.perPeriodSize == e["perPeriodSize"].get<int32_t>());
      CHECK(r.lowFrac[0] == e["lowFrac"][0].get<int32_t>());
      CHECK(r.lowFrac[1] == e["lowFrac"][1].get<int32_t>());
      CHECK(r.highFrac[0] == e["highFrac"][0].get<int32_t>());
      CHECK(r.highFrac[1] == e["highFrac"][1].get<int32_t>());
      CHECK(near(r.lowCents, e["lowCents"]));
      CHECK(near(r.highCents, e["highCents"]));
      CHECK(std::string(kMosPalette[r.collapsedColor]) == e["collapsedColor"].get<std::string>());
      CHECK(std::string(kMosPalette[r.equalizedColor]) == e["equalizedColor"].get<std::string>());
      CHECK(r.depth == e["depth"].get<int32_t>());
      if (e["parent"].is_null()) CHECK_FALSE(r.parent.has_value());
      else CHECK((r.parent && mosKeyString(*r.parent) == e["parent"].get<std::string>()));
      CHECK(r.isMirror == e["isMirror"].get<bool>());
      CHECK((equalizedEnd(r, P) == RegionEnd::Low ? "low" : "high") == set["eqEnds"][i].get<std::string>());
      ++regionsCompared;
    }
    REQUIRE(rs.colorMap.size() == set["colorMap"].size());
    for (size_t i = 0; i < rs.colorMap.size(); ++i) {
      CHECK(rs.colorMap[i].first == set["colorMap"][i][0].get<int32_t>());
      CHECK(std::string(kMosPalette[rs.colorMap[i].second]) == set["colorMap"][i][1].get<std::string>());
    }
    for (const Json& a : set["adjacent"]) {
      const MosRegion& sel = rs.regions[a["sel"].get<size_t>()];
      CHECK(findAdjacentRegion(rs.regions, sel, a["cents"].get<double>(), 2, maxSize) == a["result"].get<int32_t>());
    }
  }
  MESSAGE("mos_regions.json regions compared: " << regionsCompared);
}

TEST_CASE("mos fixtures: tree navigation, labels, names, GMN generator, EDO generators, stretch") {
  const Json f = mos_fixture::load("mos_misc.json");
  auto keyOrNull = [](const std::optional<MosKey>& k) { return k ? Json(mosKeyString(*k)) : Json(nullptr); };
  for (const Json& p : f["parents"]) {
    const auto parent = findParentMos(p[0], p[1]);
    CHECK(keyOrNull(parent) == p[2]);  // mos-calc.js
    CHECK(keyOrNull(parent) == p[3]);  // index.html
  }
  for (const Json& c : f["children"]) {
    const auto ch = childMoses({c[0], c[1]});
    CHECK(mosKeyString(ch[0]) == c[2][0].get<std::string>());
    CHECK(mosKeyString(ch[1]) == c[2][1].get<std::string>());
  }
  for (const Json& d : f["descendants"]) CHECK(isDescendantMos({d[0], d[1]}, {d[2], d[3]}) == d[4].get<bool>());
  for (const Json& a : f["ancestors"]) {
    const auto chain = ancestorChain({a[0], a[1]});
    REQUIRE(chain.size() == a[2].size());
    for (size_t i = 0; i < chain.size(); ++i) CHECK(mosKeyString(chain[i]) == a[2][i].get<std::string>());
  }
  for (const Json& l : f["labels"]) {
    const auto key = parseMosKey(l[0].get<std::string>());
    REQUIRE(key.has_value());
    CHECK(mosKeyString(*key) == l[0].get<std::string>());
    CHECK(mosDisplayLabel(*key) == l[1].get<std::string>());
  }
  for (const Json& d : f["displayNames"]) {
    const double P = d[0];
    const auto name = mosDisplayName(*parseMosKey(d[2].get<std::string>()), periodRepetitions(d[1], P), P);
    CHECK(name.has_value() == !d[3].is_null());
    if (name && !d[3].is_null()) CHECK(std::string(*name) == d[3].get<std::string>());
  }
  for (const Json& g : f["gmnGenerator"]) CHECK(near(gmnGenerator(g[0], g[1]), g[2]));
  for (const Json& l : f["gmnLabels"]) CHECK(gmnLabel(l[0]) == l[1].get<std::string>());
  for (const Json& e : f["edoGenerators"]) {
    const auto gens = edoGenerators(e["maxEDO"], e["P"]);
    REQUIRE(gens.size() == e["gens"].size());
    for (size_t i = 0; i < gens.size(); ++i) {
      CHECK(gens[i].step == e["gens"][i][0].get<int32_t>());
      CHECK(gens[i].edo == e["gens"][i][1].get<int32_t>());
      CHECK(near(gens[i].cents, e["gens"][i][2]));
    }
  }
  const auto edos53 = edoGenerators(53, 1200.0);
  for (const Json& e : f["edoStepRatio"]) CHECK(edoStepRatio(e[0], e[1], 1200.0, edos53) == optString(e[2]));
  for (const Json& s : f["stretch"]) {
    CHECK(near(stretchCents(s[1], s[0]), s[2]));
    CHECK(near(stretchedOctave(s[0]), s[3]));
  }
  size_t hs = 0;
  for (const Json& h : f["hardnessSearch"]) {
    CAPTURE(hs++);
    const auto r = mos_calc::generatorForHardness(h["nL"], h["nS"], h["target"], h["P"]);
    CHECK(r.has_value() == !h["result"].is_null());
    if (r && !h["result"].is_null()) {
      CHECK(near(r->generator, h["result"]["generator"]));
      CHECK(near(r->L, h["result"]["L"]));
      CHECK(near(r->s, h["result"]["s"]));
      CHECK(near(r->ratio, h["result"]["ratio"]));
    }
    const auto b = mos_calc::mosRegionBounds(h["nL"], h["nS"], h["P"]);
    CHECK(b.has_value() == !h["bounds"].is_null());
    if (b && !h["bounds"].is_null()) {
      CHECK(near(b->lowCents, h["bounds"]["lowCents"]));
      CHECK(near(b->highCents, h["bounds"]["highCents"]));
      CHECK(std::to_string(b->lowFrac[0]) + "/" + std::to_string(b->lowFrac[1]) == h["bounds"]["lowFrac"].get<std::string>());
      CHECK(std::to_string(b->highFrac[0]) + "/" + std::to_string(b->highFrac[1]) == h["bounds"]["highFrac"].get<std::string>());
      CHECK(b->mirror == h["bounds"]["mirror"].get<bool>());
    }
  }
  for (const Json& h : f["calcHardness"]) {
    const Hardness r = mos_calc::computeHardness(h[0], h[1], h[2]);
    CHECK(near(r.L, h[3]));
    CHECK(near(r.s, h[4]));
    CHECK(near(r.ratio, h[5]));
    CHECK(r.str == h[6].get<std::string>());
  }
  for (const Json& m : f["modeNames"]) {
    const auto name = modeName(*parseMosKey(m[0].get<std::string>()), m[1], m[2], 1200.0);
    CHECK(name.has_value() == !m[3].is_null());
    if (name && !m[3].is_null()) CHECK(std::string(*name) == m[3].get<std::string>());
  }
  auto matches = [](const std::vector<TemperamentMatch>& got, const Json& want) {
    if (got.size() != want.size()) return false;
    for (size_t i = 0; i < got.size(); ++i)
      if (got[i].index != want[i][0].get<int32_t>() || !near(got[i].low, want[i][1]) || !near(got[i].high, want[i][2]))
        return false;
    return true;
  };
  for (const Json& q : f["temperamentQueries"]) {
    const MosKey key = *parseMosKey(q["key"].get<std::string>());
    const CentsRange region{q["low"], q["high"]};
    CAPTURE(q["key"].get<std::string>());
    CHECK(matches(temperamentsForMos(key, q["P"], region), q["forMos"]));
    CHECK(matches(temperamentsAtCursor(key, q["cursor"], q["P"], region), q["atCursor"]));
    CHECK(matches(temperamentsForMos(key, q["P"], std::nullopt), q["byTag"]));
  }
}

TEST_CASE("mos fixtures: data tables equal mos-data.js") {
  const Json f = mos_fixture::load("mos_data.json");
  const auto temps = temperaments();
  REQUIRE(temps.size() == f["temperaments"].size());
  for (size_t i = 0; i < temps.size(); ++i) {
    const Temperament& t = temps[i];
    const Json& e = f["temperaments"][i];
    CAPTURE(i);
    CHECK(std::string(t.name) == e["name"].get<std::string>());
    REQUIRE(t.mos.size() == e["mos"].size());
    for (size_t j = 0; j < t.mos.size(); ++j) CHECK(mosKeyString(t.mos[j]) == e["mos"][j].get<std::string>());
    CHECK(t.genLow == e["genRange"][0].get<double>());
    CHECK(t.genHigh == e["genRange"][1].get<double>());
    CHECK(t.hasPeriod == !e["period"].is_null());
    CHECK(t.period == (e["period"].is_null() ? 1200.0 : e["period"].get<double>()));
    REQUIRE(t.commas.size() == e["commas"].size());
    for (size_t j = 0; j < t.commas.size(); ++j) CHECK(std::string(t.commas[j]) == e["commas"][j].get<std::string>());
    REQUIRE(t.commaNames.size() == e["commaNames"].size());
    for (size_t j = 0; j < t.commaNames.size(); ++j)
      CHECK(std::string(t.commaNames[j]) == e["commaNames"][j].get<std::string>());
    CHECK(std::string(t.wikiUrl) == (e["wikiUrl"].is_null() ? std::string() : e["wikiUrl"].get<std::string>()));
    CHECK(std::string(t.description) == e["description"].get<std::string>());
    CHECK(t.disabled == e["disabled"].get<bool>());
  }
  const auto colors = temperamentColors();
  REQUIRE(colors.size() == f["tempColors"].size());
  for (const auto& [name, color] : f["tempColors"].items()) {
    const auto c = temperamentColor(name);
    REQUIRE(c.has_value());
    CHECK(std::string(*c) == color.get<std::string>());
  }
  auto checkNames = [](std::span<const MosNameEntry> table, const Json& want) {
    REQUIRE(table.size() == want.size());
    for (const auto& [key, name] : want.items()) {  // nlohmann sorts keys; compare by lookup
      const auto it = std::find_if(table.begin(), table.end(), [&](const MosNameEntry& e) { return mosKeyString(e.key) == key; });
      REQUIRE(it != table.end());
      CHECK(std::string(it->name) == name.get<std::string>());
    }
  };
  checkNames(mosNameTable(), f["mosNames"]);
  checkNames(multiPeriodMosNameTable(), f["multiPeriodMosNames"]);
  REQUIRE(modeNameTable().size() == f["modeNames"].size());
  for (const auto& [key, names] : f["modeNames"].items()) {
    const auto got = modeNames(*parseMosKey(key));
    REQUIRE(got.size() == names.size());
    for (size_t j = 0; j < got.size(); ++j) CHECK(std::string(got[j]) == names[j].get<std::string>());
  }
  REQUIRE(f["palette"].size() == kMosPalette.size());
  for (size_t i = 0; i < kMosPalette.size(); ++i) CHECK(std::string(kMosPalette[i]) == f["palette"][i].get<std::string>());
}
