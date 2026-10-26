// Malformed project files (T3.5): precise errors with the JSON path of the offending value, and 10⁴ mutated files
// (byte flips, truncations, span deletions/duplications, type swaps) that never crash or hang and always fail with a
// JSON path.

#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "pq/document/serialize.h"
#include "project_gen.h"

using namespace pq::doc;
using pq::Result;
using nlohmann::json;

namespace {

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

const std::string& minimalText() {
  static const std::string t = readFile(std::string(PQ_TEST_DATA_DIR) + "/document/fixtures/minimal/project.pqd");
  return t;
}

// The rich project as a JSON DOM, to mutate one value at a time.
const json& richDom() {
  static const json d = [] {
    doctest_fixture::RichProject f;
    Result<std::string> t = serializeProject(f.doc(), f.store.view());
    REQUIRE(t.ok());
    return json::parse(*t);
  }();
  return d;
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
  const size_t at = text.find(from);
  REQUIRE_MESSAGE(at != std::string::npos, from);
  text.replace(at, from.size(), to);
  return text;
}

pq::Error expectFail(const std::string& text) {
  Result<LoadedProject> r = parseProject(text, LoadOptions{builtinMigrations(), "test.pqd"});
  if (r.ok()) {
    FAIL_CHECK("the file loaded but should not have");
    return {};
  }
  CHECK(r.error().location.file == "test.pqd");
  return r.error();
}

void expectError(const std::string& text, const std::string& message) {
  INFO("expected: " << message);
  const pq::Error e = expectFail(text);
  CHECK(e.message == message);
}

void expectDomError(const std::function<void(json&)>& mutate, const std::string& message) {
  json d = richDom();
  mutate(d);
  expectError(d.dump(1), message);
}

}  // namespace

TEST_CASE("serialize errors: syntax") {
  expectError("", "$: expected an object, found the end of the file");
  expectError("   \n ", "$: expected an object, found the end of the file");
  CHECK(expectFail("[]").code == pq::ErrorCode::Type);
  expectError("[]", "$: expected an object, found an array");
  expectError(minimalText() + "x", "$: unexpected content after the end of the document");
  expectError(replaced(minimalText(), "\"bpm\": 120.0", "\"bpm\": 0120.0"), "$.timeline.tempo[0].bpm: invalid number");
  expectError(replaced(minimalText(), "\"bpm\": 120.0", "\"bpm\": 120."),
              "$.timeline.tempo[0].bpm: invalid number (digits must follow '.')");
  expectError(replaced(minimalText(), "\"bpm\": 120.0", "\"bpm\": NaN"),
              "$.timeline.tempo[0].bpm: expected a number, found an invalid character");
  expectError(replaced(minimalText(), "\"output\": \"default\"}", "\"output\": \"default\",}"),
              "$.master.mixer: trailing comma in object");
  expectError(replaced(minimalText(), "\"sends\": []", "\"sends\": [{\"target\": 1, \"levelDb\": 0.0, \"preFader\": false},]"),
              "$.master.mixer.sends: trailing comma in array");
  expectError(replaced(minimalText(), "\"name\": \"Master\"", "\"name\": \"Mas\xFFter\""),
              "$.master.name: invalid UTF-8 in a string");
  expectError(replaced(minimalText(), "\"name\": \"Master\"", "\"name\": \"Mas\tter\""),
              "$.master.name: control character in a string (must be escaped)");
  expectError(replaced(minimalText(), "\"name\": \"Master\"", "\"name\": \"\\ud800x\""),
              "$.master.name: unpaired surrogate in a \\u escape");
  expectError(replaced(minimalText(), "\"name\": \"Master\"", "\"name\": \"\\q\""), "$.master.name: invalid escape in a string");
  expectError(replaced(minimalText(), "\"locators\": []", "\"locators\" []"), "$: expected ':' after a key");
  expectError(replaced(minimalText(), "\"active\": true", "\"active\": tru"), "$.master.active: expected true or false");
  // Truncation inside a nested container names that container.
  const std::string& m = minimalText();
  expectError(m.substr(0, m.find("\"curve\"")), "$.timeline.tempo[0]: expected a key (a string in double quotes)");
  // Line and column of a syntax error (1-based; the 4th line holds the tempo).
  const pq::Error e = expectFail(replaced(minimalText(), "\"bpm\": 120.0", "\"bpm\": 0120.0"));
  CHECK(e.location.line == 4);
  CHECK(e.location.col == 35);  // the offending '1' after the leading zero
}

TEST_CASE("serialize errors: structure, types and ranges") {
  expectError(replaced(minimalText(), "  \"schemaVersion\": 3,\n", ""), "$: missing key \"schemaVersion\"");
  CHECK(expectFail(replaced(minimalText(), "\"schemaVersion\": 3", "\"schemaVersion\": \"3\"")).code ==
        pq::ErrorCode::Type);
  expectError(replaced(minimalText(), "\"schemaVersion\": 3", "\"schemaVersion\": \"3\""),
              "$.schemaVersion: expected an integer, found a string");
  expectError(replaced(minimalText(), "\"tracks\": [],", "\"tracks\": [], \"bogus\": 1,"), "$.bogus: unknown key");
  expectError(replaced(minimalText(), "\"tracks\": [],", "\"tracks\": [], \"tracks\": [],"), "$.tracks: duplicate key");
  expectError(replaced(minimalText(), "\"returns\": [],", "\"returns\": [], \"view\": {},"), "$.view: missing key \"tracks\"");
  expectError(replaced(minimalText(), "\"curve\": \"step\"", "\"curve\": \"cubic\""),
              "$.timeline.tempo[0].curve: curve must be \"step\" or \"linear\"");
  CHECK(expectFail(replaced(minimalText(), "\"bpm\": 120.0", "\"bpm\": 0.5")).message.rfind("$.timeline.tempo: ", 0) == 0);
  CHECK(expectFail(replaced(minimalText(), "\"denominator\": 4}", "\"denominator\": 512}")).message.rfind(
            "$.timeline.signatures: ", 0) == 0);
  expectError(replaced(minimalText(), "\"start\": 0, \"end\": 3548160", "\"start\": 9, \"end\": 9"),
              "$.loop: loop end must be after loop start");
  expectError(replaced(minimalText(), "\"start\": 0, \"end\"", "\"start\": 1.5, \"end\""),
              "$.loop.start: expected an integer, found a number with a fraction or exponent");
  expectError(replaced(minimalText(), "\"start\": 0, \"end\"", "\"start\": 4503599627370497, \"end\""),
              "$.loop.start: tick out of range");
  expectError(replaced(minimalText(), "\"start\": 0, \"end\"", "\"start\": -99999999999999999999, \"end\""),
              "$.loop.start: integer out of range");
  expectError(replaced(minimalText(), "\"referenceHz\": 261.626", "\"referenceHz\": 1e400"),
              "$.tunings[0].def.referenceHz: number out of range");
  expectError(replaced(minimalText(), "\"referenceHz\": 261.626", "\"referenceHz\": -1.0"),
              "$.tunings[0].def: reference pitch must be a finite positive frequency");
  expectError(replaced(minimalText(), "\"id\": 1,", "\"id\": 0,"), "$.tunings[0].id: id must be in [1, 4294967294]");
  expectError(replaced(minimalText(), "\"id\": 1,", "\"id\": 4294967295,"), "$.tunings[0].id: id must be in [1, 4294967294]");
  expectError(replaced(minimalText(), "\"colour\": \"#5E9BD6\"", "\"colour\": \"red\""),
              "$.tunings[0].colour: colour must be \"#RRGGBB\"");
  expectError(replaced(minimalText(), "\"equave\": \"2/1\"", "\"equave\": \"2/0\""),
              "$.tunings[0].def.equave: equave must be a ratio \"n/d\" of positive integers");
  expectError(replaced(minimalText(), "\"equave\": \"2/1\"", "\"equave\": \"1/2\""),
              "$.tunings[0].def.equave: EDO equave must be greater than 1/1");
  expectError(replaced(minimalText(), "\"equave\": \"2/1\"", "\"equave\": \"2/1\", \"equaveCents\": 1200.0"),
              "$.tunings[0].def: an EDO needs exactly one of \"equave\" and \"equaveCents\"");
  expectError(replaced(minimalText(), "\"equave\": \"2/1\"", "\"equave\": \"2/1\", \"generator\": 700.0"),
              "$.tunings[0].def.generator: does not apply to a tuning of kind \"edo\"");
  expectError(replaced(minimalText(), "\"kind\": \"edo\"", "\"kind\": \"ji\""),
              "$.tunings[0].def.kind: kind must be \"edo\", \"mos\" or \"scale\"");
  expectError(replaced(minimalText(), "{\"id\": 1, \"name\": \"12-EDO\", \"colour\": \"#5E9BD6\",\n     \"def\": {\"kind\": \"edo\", \"n\": 12, \"equave\": \"2/1\", \"referenceHz\": 261.626}}", ""),
              "$.tunings: a project needs at least one main tuning");
  expectError(replaced(minimalText(), "\"kind\": \"master\"", "\"kind\": \"midi\""), "$.master.kind: must be \"master\" here");
  CHECK(expectFail(replaced(minimalText(), "\"kind\": \"master\"", "\"kind\": \"audio\"")).code == pq::ErrorCode::Unsupported);
  expectError(replaced(minimalText(), "\"mute\": false", "\"mute\": true"), "$.master.mixer: the master track has no mute or solo");
  expectError(replaced(minimalText(), "\"volumeDb\": 0.0", "\"volumeDb\": 7.0"),
              "$.master.mixer.volumeDb: parameter value out of range");
  expectError(replaced(minimalText(), "\"id\": 2,", "\"id\": 1,"),
              "$.master: id 1 is used by more than one entity (also $.tunings[0])");
  expectError(replaced(minimalText(), "\"returns\": [],", "\"returns\": [], \"view\": {\"tracks\": [{\"id\": 5, \"folded\": true, \"height\": 0}], \"lanes\": []},"),
              "$.view.tracks[0].id: no track has this id");
  expectError(replaced(minimalText(), "\"returns\": [],", "\"returns\": [], \"view\": {\"tracks\": [{\"id\": 2, \"folded\": true, \"height\": -1}], \"lanes\": []},"),
              "$.view.tracks[0].height: height must be >= 0 (0 = the theme's default)");
}

TEST_CASE("serialize errors: tracks, clips, notes, lanes and references") {
  // Paths in the rich project: tracks[0] = Lead (clips A, B; lanes volume, send, tuning reference),
  // tracks[1] = Drums > Kick, Inner > innerMidi, tracks[2] = Bass; returns[0] = Reverb.
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["notes"][5]["velocity"] = 0; },
                 "$.tracks[0].clips[0].notes[5]: note velocity must be in (0, 127]");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["notes"][5]["length"] = 0; },
                 "$.tracks[0].clips[0].notes[5]: note length must be at least one tick");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["notes"][5]["pitch"]["cents"] = 1.0; },
                 "$.tracks[0].clips[0].notes[5].pitch: a pitch needs exactly one of \"degree\", \"ratio\", \"cents\" and \"jiObject\"");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["notes"][5]["pitch"] = {{"ratio", "0/1"}}; },
                 "$.tracks[0].clips[0].notes[5].pitch.ratio: ratio must be \"n/d\" with positive integers n, d");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["notes"][5]["pitch"] = {{"degree", 1}, {"anchor", "note"}}; },
                 "$.tracks[0].clips[0].notes[5].pitch.anchor: only allowed with \"ratio\"");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["notes"][5]["pitch"] = {{"jiObject", 1}}; },
                 "$.tracks[0].clips[0].notes[5].pitch: missing key \"voice\"");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["notes"][5]["id"] = d["tracks"][0]["clips"][0]["notes"][6]["id"]; },
                 "$.tracks[0].clips[0].notes: duplicate note id");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["notes"][5]["id"] = d["tracks"][0]["id"]; },
                 "$.tracks[0].clips[0].notes: id 4 is used by more than one entity (also $.tracks[0])");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["notes"][5]["velocity"] = "loud"; },
                 "$.tracks[0].clips[0].notes[5].velocity: expected a number, found a string");
  expectDomError([](json& d) { std::swap(d["tracks"][0]["clips"][0], d["tracks"][0]["clips"][1]); },
                 "$.tracks[0].clips[1]: clips must be sorted by (start, id)");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["loop"]["end"] = 0; },
                 "$.tracks[0].clips[0].loop: loop end must be after loop start");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["signature"]["denominator"] = 512; },
                 "$.tracks[0].clips[0].signature: invalid time signature");
  expectDomError([](json& d) { d["tracks"][0]["clips"][0]["tuning"] = {{"main", 999}}; },
                 "$.tracks[0].clips[0].tuning: main tuning not found");
  expectDomError([](json& d) { d["tracks"][0]["tuning"] = {{"main", 1}, {"private", json::object()}}; },
                 "$.tracks[0].tuning.private: missing key \"kind\"");
  expectDomError([](json& d) { d["tracks"][0]["mixer"]["sends"][0]["levelDb"] = -70.0; d["tracks"][0]["mixer"]["sends"][0]["preFader"] = false; },
                 "$.tracks[0].mixer.sends[0]: a send at the default (-70 dB, post-fader) is implicit and must not be stored");
  expectDomError([](json& d) { d["tracks"][0]["mixer"]["sends"][0]["target"] = d["tracks"][2]["id"]; },
                 "$.tracks[0].mixer.sends[0].target: send target must be another return track");
  expectDomError([](json& d) { d["tracks"][2]["mixer"]["output"] = {{"track", d["tracks"][0]["id"]}}; },
                 "$.tracks[2].mixer.output: output target must be another group track");
  expectDomError([](json& d) { d["tracks"][1]["mixer"]["output"] = {{"track", d["tracks"][2]["id"]}};
                               d["tracks"][2]["kind"] = "group"; d["tracks"][2].erase("clips");
                               d["tracks"][2]["devices"] = json::array();  // T4.1: a group holds no instrument
                               d["tracks"][2]["children"] = json::array(); d["tracks"][2]["mixer"]["arm"] = false;
                               d["tracks"][2]["mixer"]["output"] = {{"track", d["tracks"][1]["id"]}}; },
                 "$: routing would create a cycle");
  expectDomError([](json& d) { d["tracks"][2]["children"] = json::array(); },
                 "$.tracks[2].children: only groups have child tracks");
  expectDomError([](json& d) { d["returns"][0]["kind"] = "midi"; }, "$.returns[0].kind: must be \"return\" here");
  expectDomError([](json& d) { d["returns"][0]["mixer"]["arm"] = true; }, "$.returns[0].mixer: only MIDI tracks can be armed");
  expectDomError([](json& d) { d["tracks"][0]["lanes"][1]["param"] = d["tracks"][0]["lanes"][0]["param"]; },
                 "$.tracks[0].lanes[1].param: this parameter already has an automation lane");
  expectDomError([](json& d) { d["tracks"][0]["lanes"][0]["param"]["kind"] = "trackSolo"; },
                 "$.tracks[0].lanes[0].points[0].value: value out of the parameter's range");
  expectDomError([](json& d) { d["tracks"][0]["lanes"][0]["param"]["kind"] = "trackSend"; },
                 "$.tracks[0].lanes[0].param: missing key \"target\"");
  expectDomError([](json& d) { std::swap(d["tracks"][0]["lanes"][0]["points"][0], d["tracks"][0]["lanes"][0]["points"][1]); },
                 "$.tracks[0].lanes[0].points[1]: breakpoints must be sorted by (tick, id)");
  expectDomError([](json& d) { d["tracks"][0]["lanes"][0]["points"][0]["curve"] = 2.0; },
                 "$.tracks[0].lanes[0].points[0].curve: curve must be in [-1, 1]");
  expectDomError([](json& d) { d["tracks"][2]["lanes"][1]["param"]["owner"] = d["tracks"][0]["id"]; },
                 "$.tracks[2].lanes[1].param: a track parameter can only be automated on its own track");
  expectDomError([](json& d) { std::swap(d["locators"][0], d["locators"][1]); },
                 "$.locators[1]: locators must be sorted by (tick, id)");
  expectDomError([](json& d) { d["tunings"][3]["def"]["pitches"][2] = "12/x"; },
                 "$.tunings[3].def.pitches[2]: scale pitch: expected a ratio of decimal digits like 5/4");
  expectDomError([](json& d) { d["tunings"][3]["def"]["pitches"][2] = true; },
                 "$.tunings[3].def.pitches[2]: expected a ratio string like \"5/4\" or a number of cents");
  expectDomError([](json& d) { d["tunings"][3]["def"]["mapping"]["mapping"][1] = nullptr; },
                 "$.tunings[3].def.mapping: the reference key is unmapped");
  expectDomError([](json& d) { d["tunings"][2]["def"]["rotation"] = 9; },
                 "$.tunings[2].def: " + pq::tuning::Mos::make([] { pq::tuning::MosParams m; m.rotation = 9; return m; }()).error().message);
  // Groups nested deeper than kMaxTrackDepth.
  json d = richDom();
  json* at = &d["tracks"];
  for (uint32_t level = 0; level < kMaxTrackDepth; ++level) {
    json g = d["tracks"][1];
    g["id"] = 100000 + level;
    g["children"] = json::array();
    g.erase("lanes");
    at->push_back(g);
    at = &at->back()["children"];
  }
  expectError(d.dump(), "$.tracks[3]" + [] {
    std::string s;
    for (uint32_t i = 1; i < kMaxTrackDepth; ++i) s += ".children[0]";
    return s;
  }() + ".children: groups nested more than 32 levels deep");
}

// ---- Fuzz ----------------------------------------------------------------------------------------------------------

namespace {

// Every value node of a DOM (pointers stay valid while the DOM is not resized).
void collect(json& j, std::vector<json*>& out) {
  out.push_back(&j);
  if (j.is_object())
    for (auto& [k, v] : j.items()) collect(v, out);
  else if (j.is_array())
    for (auto& v : j) collect(v, out);
}

}  // namespace

TEST_CASE("serialize errors: 10^4 mutated files never crash, hang or fail without a JSON path" * doctest::test_suite("slow")) {
  // A smaller rich project (clip A trimmed) keeps the fuzz fast in the /Od dev build.
  json base = richDom();
  auto& notesA = base["tracks"][0]["clips"][0]["notes"];
  notesA.erase(notesA.begin() + 12, notesA.end());
  const std::string baseText = base.dump(1);
  REQUIRE(parseProject(baseText).ok());

  std::mt19937 rng(20261021);
  auto pick = [&](size_t n) { return static_cast<size_t>(rng() % static_cast<uint32_t>(n)); };
  static const char* const kSwaps[] = {"\"x\"", "1", "-1", "1.5", "true", "null", "[]", "{}", "1e300", "\"1/0\"",
                                       "4294967295", "-0.0", "\"\"", "[1]", "{\"a\": 1}"};
  static const char kBytes[] = "{}[],:\"\\0123456789-+.eE ntfx\x80\xFF";
  int ok = 0, failed = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int iter = 0; iter < 10000; ++iter) {
    std::string text = baseText;
    switch (iter % 6) {
      case 0:  // byte flips
        for (size_t k = 1 + pick(3); k > 0; --k) text[pick(text.size())] = kBytes[pick(sizeof kBytes - 1)];
        break;
      case 1:  // truncation
        text.resize(pick(text.size()));
        break;
      case 2: {  // delete a span
        const size_t a = pick(text.size());
        text.erase(a, 1 + pick(64));
        break;
      }
      case 3: {  // duplicate a span
        const size_t a = pick(text.size());
        text.insert(a, text.substr(a, 1 + pick(64)));
        break;
      }
      default: {  // type swaps in the JSON
        json d = base;
        for (size_t k = 1 + pick(2); k > 0; --k) {
          std::vector<json*> nodes;  // re-collected: a swap invalidates pointers into the replaced subtree
          collect(d, nodes);
          *nodes[1 + pick(nodes.size() - 1)] = json::parse(kSwaps[pick(std::size(kSwaps))]);
        }
        text = d.dump(iter % 2 ? 1 : -1);
        if (iter % 12 == 4) {  // and raw out-of-range numbers, which a DOM cannot hold
          const size_t at = text.find(": 0");
          if (at != std::string::npos) text.replace(at, 3, ": 1e400");
        }
        break;
      }
    }
    Result<LoadedProject> r = parseProject(text);
    if (r.ok()) {
      ++ok;
      // Whatever loads is a valid project: it saves, and loads back equal.
      Result<std::string> again = serializeProject(*r->project, r->view);
      REQUIRE(again.ok());
      Result<LoadedProject> back = parseProject(*again);
      REQUIRE(back.ok());
      CHECK(*back->project == *r->project);
    } else {
      ++failed;
      const std::string& msg = r.error().message;
      if (msg.rfind("$", 0) != 0) FAIL_CHECK("error without a JSON path: " << msg);
    }
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE("fuzz: " << ok << " loaded, " << failed << " rejected, " << s << " s");
  CHECK(ok + failed == 10000);
  CHECK(failed > 5000);
}
