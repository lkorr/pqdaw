// Project serialization (T3.5): load(save(p)) == p for generated projects with every entity kind and every PitchId
// kind; committed fixture projects load (and the writer reproduces the golden byte for byte); number edge cases;
// view state; the migration hook table.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "pq/document/serialize.h"
#include "project_gen.h"

using namespace pq::doc;
using pq::Result;
using doctest_fixture::RandomProject;
using doctest_fixture::RichProject;

namespace {

std::string fixturePath(const std::string& name) { return std::string(PQ_TEST_DATA_DIR) + "/document/fixtures/" + name; }

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool viewEq(const ViewState& a, const ViewState& b) {
  if (a.tracks.size() != b.tracks.size() || a.lanes.size() != b.lanes.size()) return false;
  for (const auto& [id, v] : a.tracks) {
    auto it = b.tracks.find(id);
    if (it == b.tracks.end() || it->second.folded != v.folded || it->second.height != v.height) return false;
  }
  for (const auto& [id, v] : a.lanes) {
    auto it = b.lanes.find(id);
    if (it == b.lanes.end() || it->second.visible != v.visible || it->second.height != v.height) return false;
  }
  return true;
}

LoadedProject roundTrip(const Project& p, const ViewState& v, std::string* textOut = nullptr) {
  Result<std::string> text = serializeProject(p, v);
  REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  if (textOut) *textOut = *text;
  return std::move(back).value();
}

// Bitwise comparison of every note (operator== treats -0.0 == 0.0; the file must keep the sign too).
void checkNotesBitwise(const Project& a, const Project& b) {
  std::vector<Note> na, nb;
  forEachTrack(a, [&](const Track& t, const Track*) {
    for (const ClipSlot& c : t.clips) na.insert(na.end(), c.clip->notes.begin(), c.clip->notes.end());
  });
  forEachTrack(b, [&](const Track& t, const Track*) {
    for (const ClipSlot& c : t.clips) nb.insert(nb.end(), c.clip->notes.begin(), c.clip->notes.end());
  });
  REQUIRE(na.size() == nb.size());
  for (size_t i = 0; i < na.size(); ++i) {
    CHECK(std::memcmp(&na[i], &nb[i], sizeof(Note)) == 0);  // Note has no padding (static_asserted 64 B layout)
  }
}

}  // namespace

TEST_CASE("serialize: the rich project covers every entity, pitch, anchor, tuning and parameter kind") {
  RichProject f;
  const Project& p = f.doc();
  std::set<int> pitchKinds, anchorKinds, paramKinds, tuningKinds, trackKinds, outputKinds, modes;
  forEachTrack(p, [&](const Track& t, const Track*) {
    trackKinds.insert(static_cast<int>(t.kind));
    outputKinds.insert(static_cast<int>(t.mixer.output.kind));
    modes.insert(static_cast<int>(t.tuning.mode));
    for (const LanePtr& l : t.lanes) paramKinds.insert(static_cast<int>(l->param.kind));
    for (const ClipSlot& c : t.clips) {
      modes.insert(static_cast<int>(c.clip->tuning.mode));
      if (c.clip->tuning.privateDef) tuningKinds.insert(static_cast<int>(c.clip->tuning.privateDef->system.index()));
      for (const Note& n : c.clip->notes) {
        pitchKinds.insert(static_cast<int>(n.pitch.kind()));
        if (n.pitch.kind() == pq::tuning::PitchKind::Ratio) anchorKinds.insert(static_cast<int>(n.pitch.anchorKind()));
      }
    }
    if (t.tuning.privateDef) tuningKinds.insert(static_cast<int>(t.tuning.privateDef->system.index()));
  });
  for (const TuningSlotPtr& s : *p.tunings) tuningKinds.insert(static_cast<int>(s->def.system.index()));
  CHECK(pitchKinds.size() == 4);
  CHECK(anchorKinds.size() == 3);
  CHECK(paramKinds.size() == 9);  // every automatable kind (TrackSolo is not automatable)
  CHECK(tuningKinds.size() == 3);
  CHECK(trackKinds.size() == 4);  // midi, group, return, master (audio is not supported until M6)
  CHECK(outputKinds.size() == 4);
  CHECK(modes.size() == 3);
  CHECK(p.locators->size() == 3);
}

TEST_CASE("serialize: load(save(p)) == p for the rich project, ids preserved, view state kept") {
  RichProject f;
  std::string text;
  const LoadedProject back = roundTrip(f.doc(), f.store.view(), &text);
  CHECK(*back.project == f.doc());
  checkNotesBitwise(*back.project, f.doc());
  CHECK(maxEntityId(*back.project) == maxEntityId(f.doc()));
  CHECK(viewEq(back.view, f.store.view()));
  CHECK(back.fileSchemaVersion == kSchemaVersion);
  // Stable: writing the loaded project gives the same bytes.
  Result<std::string> again = serializeProject(*back.project, back.view);
  REQUIRE(again.ok());
  CHECK(*again == text);
  // The built maps work (they are not stored).
  CHECK(back.project->timeline->tempoMap.segmentCount() == 3);
  CHECK(back.project->timeline->sigMap.changeCount() == 3);
  // Every tuning resolves.
  for (const TuningSlotPtr& s : *back.project->tunings) CHECK(buildTuning(s->def).ok());
}

TEST_CASE("serialize: random projects round-trip exactly (50 seeds)") {
  for (uint32_t seed = 1; seed <= 50; ++seed) {
    CAPTURE(seed);
    RandomProject f(seed);
    const LoadedProject back = roundTrip(f.doc(), f.store.view());
    CHECK(*back.project == f.doc());
    checkNotesBitwise(*back.project, f.doc());
  }
}

TEST_CASE("serialize: the default project and a project after edits/undo round-trip") {
  DocumentStore store;
  LoadedProject back = roundTrip(*store.current(), store.view());
  CHECK(*back.project == *store.current());
  RichProject f;
  REQUIRE(f.store.undo());  // back to the plain fixture
  back = roundTrip(f.doc(), f.store.view());
  CHECK(*back.project == f.doc());
}

TEST_CASE("serialize: number edge cases are exact (shortest round-trip, -0.0 kept, denormals, extremes)") {
  RichProject f;
  std::string text;
  const LoadedProject back = roundTrip(f.doc(), f.store.view(), &text);
  checkNotesBitwise(*back.project, f.doc());
  // Shortest forms, always with a fraction or exponent so they read back as floating point.
  CHECK(text.find("\"cents\": -0.0}") != std::string::npos);
  CHECK(text.find("\"cents\": 5e-324}") != std::string::npos);
  CHECK(text.find("\"cents\": 1.7976931348623157e+308}") != std::string::npos);
  CHECK(text.find("\"cents\": 386.3137138648348}") != std::string::npos);
  CHECK(text.find("\"velocity\": 1e-45") != std::string::npos);  // float denormal
  CHECK(text.find("\"velocity\": 0.1") != std::string::npos);    // float 0.1f, not 0.10000000149011612
  CHECK(text.find("\"releaseVelocity\": -0.0") != std::string::npos);
  CHECK(text.find("\"velocity\": 127.0") != std::string::npos);
  // Ticks are integers; rationals are "n/d".
  CHECK(text.find("\"start\": 4503599627370496") != std::string::npos);
  CHECK(text.find("\"ratio\": \"9223372036854775807/9223372036854775806\"") != std::string::npos);
  CHECK(text.find("\"equave\": \"3/1\"") != std::string::npos);
  // The Scala ratio beyond int64 keeps its digits (Q23).
  CHECK(text.find("\"123456789012345678901234/98765432109876543210\"") != std::string::npos);
  // The MOS private tuning keeps -0.0 and a denormal in its MODMOS list.
  CHECK(text.find("\"modmos\": [0.0, -0.0, 1.0, 0.5, 5e-324]") != std::string::npos);
}

TEST_CASE("serialize: values JSON cannot hold are save errors with the JSON path") {
  RichProject f;
  // Build a project directly (commands would reject these values).
  auto p = std::make_shared<Project>(f.doc());
  {
    Track t = *p->tracks[0];
    t.mixer.volumeDb = NAN;
    p->tracks[0] = std::make_shared<const Track>(t);
    Result<std::string> r = serializeProject(*p);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message == "$.tracks[0].mixer.volumeDb: number is not finite (NaN and infinities cannot be stored)");
  }
  p = std::make_shared<Project>(f.doc());
  {
    Track t = *p->tracks[1];
    t.name = "bad \xFF utf8";
    p->tracks[1] = std::make_shared<const Track>(t);
    Result<std::string> r = serializeProject(*p);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message == "$.tracks[1].name: string is not valid UTF-8");
  }
  p = std::make_shared<Project>(f.doc());
  {
    Track t = *p->tracks[0];
    NoteClip c = *t.clips[0].clip;
    std::vector<Note> notes(c.notes.begin(), c.notes.end());
    notes[3].pitch = pq::tuning::PitchId::fromCents(INFINITY);
    NoteSeq seq;
    {
      // fromNotes rejects it; build the sequence from valid notes and patch one through edited() instead.
      std::vector<Note> ok = notes;
      ok[3].pitch = pq::tuning::PitchId::fromCents(0.0);
      seq = NoteSeq::fromNotes(ok).value();
      seq = seq.withReplaced(keyOf(ok[3]), notes[3]);
    }
    c.notes = seq;
    t.clips[0].clip = std::make_shared<const NoteClip>(c);
    p->tracks[0] = std::make_shared<const Track>(t);
    Result<std::string> r = serializeProject(*p);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message ==
          "$.tracks[0].clips[0].notes[3].pitch: number is not finite (NaN and infinities cannot be stored)");
  }
}

TEST_CASE("serialize: view state for ids that no longer exist is not written; loading checks view ids") {
  RichProject f;
  ViewState v = f.store.view();
  v.tracks[TrackId{999999}] = TrackView{true, 5};
  v.lanes[LaneId{888888}] = LaneView{false, 5};
  const LoadedProject back = roundTrip(f.doc(), v);
  CHECK(viewEq(back.view, f.store.view()));
}

// ---- Fixtures ------------------------------------------------------------------------------------------------------

TEST_CASE("serialize: fixtures/full matches the rich project and the writer reproduces it byte for byte") {
  RichProject f;
  Result<std::string> text = serializeProject(f.doc(), f.store.view());
  REQUIRE(text.ok());
  const std::string path = fixturePath("full/project.pqd");
  if (std::getenv("PQ_UPDATE_FIXTURES")) {
    std::ofstream(path, std::ios::binary) << *text;
    MESSAGE("rewrote " << path);
  }
  const std::string golden = readFile(path);
  REQUIRE_FALSE(golden.empty());
  CHECK(golden == *text);  // format stability: a schema change must bump schemaVersion and add a migration
  Result<LoadedProject> loaded = loadProject(fixturePath("full"));
  REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
  CHECK(*loaded->project == f.doc());
  CHECK(viewEq(loaded->view, f.store.view()));
}

TEST_CASE("serialize: hand-written fixtures load") {
  SUBCASE("minimal: the smallest valid project") {
    Result<LoadedProject> r = loadProject(fixturePath("minimal"));
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    const Project& p = *r->project;
    CHECK(p.tracks.empty());
    CHECK(p.tunings->size() == 1);
    CHECK(p.master->kind == TrackKind::Master);
    CHECK(r->view.tracks.empty());
    DocumentStore store;
    CHECK(p == *store.current());  // the default project (ids 1 and 2)
    REQUIRE(openProject(fixturePath("minimal"), store).ok());
    CHECK(store.ids().peek() == maxEntityId(p) + 1);
  }
  SUBCASE("hand-edited: CRLF, BOM, keys in any order, schemaVersion not first, escapes, notes unsorted") {
    Result<LoadedProject> r = loadProject(fixturePath("hand_edited"));
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    const Project& p = *r->project;
    REQUIRE(p.tracks.size() == 1);
    const Track& t = *p.tracks[0];
    CHECK(t.name == "Caf\xC3\xA9 \xF0\x9F\x8E\xB9 \"lead\"");
    REQUIRE(t.clips.size() == 1);
    const NoteSeq& notes = t.clips[0].clip->notes;
    REQUIRE(notes.size() == 3);
    pq::Ticks last = -1;
    for (const Note& n : notes) {  // sorted on load
      CHECK(n.start > last);
      last = n.start;
    }
    CHECK(notes.begin()->pitch == pq::tuning::PitchId::fromRatio(pq::tuning::Rational{5, 4}));
    CHECK(notes.begin()->releaseVelocity == 64.0f);  // defaults for omitted note fields
    CHECK(notes.begin()->chance == 1.0f);
    CHECK(r->view.tracks.at(t.id).folded);
  }
}

// ---- Migrations ----------------------------------------------------------------------------------------------------

namespace {

// A pretend schema 0 that stored the tempo as a top-level "bpm" and had no "locators": 0 → 1 moves it.
pq::Status migrate0to1(nlohmann::json& doc) {
  if (!doc.contains("bpm")) return pq::Error{pq::ErrorCode::InvalidArgument, "no bpm", {}};
  doc["timeline"]["tempo"] = nlohmann::json::array({{{"tick", 0}, {"bpm", doc["bpm"]}, {"curve", "step"}}});
  doc.erase("bpm");
  doc["locators"] = nlohmann::json::array();
  return pq::Ok{};
}

std::string schema0Text() {
  // The minimal fixture rewritten in the pretend schema 0.
  nlohmann::json doc = nlohmann::json::parse(readFile(fixturePath("minimal/project.pqd")));
  doc["schemaVersion"] = 0;
  doc["bpm"] = 98.5;
  doc["timeline"].erase("tempo");
  doc.erase("locators");
  return doc.dump();
}

}  // namespace

TEST_CASE("serialize: migration hook table (n -> n+1)") {
  const Migration table[] = {{0, migrate0to1}, builtinMigrations()[0], builtinMigrations()[1], builtinMigrations()[2]};
  const std::string old = schema0Text();
  SUBCASE("an old file is migrated, then decoded") {
    Result<LoadedProject> r = parseProject(old, LoadOptions{table, "old.pqd"});
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r->fileSchemaVersion == 0);
    CHECK(r->project->timeline->tempo.front().bpm == 98.5);
    // Saving writes the current schema.
    Result<std::string> again = serializeProject(*r->project, r->view);
    REQUIRE(again.ok());
    CHECK(again->rfind("{\n  \"schemaVersion\": " + std::to_string(kSchemaVersion) + ",", 0) == 0);
  }
  SUBCASE("no migration for the file's version") {
    Result<LoadedProject> r = parseProject(old);  // built-in table: empty
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == pq::ErrorCode::Unsupported);
    CHECK(r.error().message == "$.schemaVersion: no migration from schema version 0");
  }
  SUBCASE("a failing migration aborts the load") {
    nlohmann::json doc = nlohmann::json::parse(old);
    doc.erase("bpm");
    Result<LoadedProject> r = parseProject(doc.dump(), LoadOptions{table, {}});
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message == "$: migrating schema 0 to 1 failed: no bpm");
  }
  SUBCASE("a newer file is refused") {
    std::string newer = readFile(fixturePath("minimal/project.pqd"));
    const size_t at = newer.find("\"schemaVersion\": 4");
    REQUIRE(at != std::string::npos);
    newer.replace(at, 18, "\"schemaVersion\": 7");
    Result<LoadedProject> r = parseProject(newer);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == pq::ErrorCode::Unsupported);
    CHECK(r.error().message ==
          "$.schemaVersion: the project was saved by a newer pqdaw (schema 7; this build reads up to 4)");
  }
  SUBCASE("a migration's output is validated like any file") {
    const Migration broken[] = {{0, [](nlohmann::json& d) -> pq::Status {
                                   d["timeline"]["tempo"] = nlohmann::json::array();
                                   d["locators"] = nlohmann::json::array();
                                   d.erase("bpm");
                                   return pq::Ok{};
                                 }},
                                builtinMigrations()[0], builtinMigrations()[1], builtinMigrations()[2]};
    Result<LoadedProject> r = parseProject(old, LoadOptions{broken, {}});
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.rfind("$.timeline.tempo: ", 0) == 0);
  }
}

// ---- Schema 1 -> 2 (T0.5-fix1: PPQ 27,720 -> 221,760) --------------------------------------------------------------

namespace {

// Walks the schema-1 document and its re-saved form in parallel: same structure and values everywhere, except
// schemaVersion (1 -> the current one) and every tick member, which must be exactly 8x the old value. (T4.1: the
// re-saved form passes 2 -> 3 as well, which adds "devices" to every track; the caller strips them first and checks
// them on their own.)
struct ScaleCheck {
  size_t scaled = 0;              // tick values compared
  size_t scaledNonZero = 0;       // of which != 0 (so the factor is really observed)
  std::set<std::string> keys;     // tick member names seen
  std::set<std::string> owners;   // where they were seen
  std::vector<std::string> mismatches;

  static bool isTickKey(std::string_view k) {
    return k == "tick" || k == "start" || k == "end" || k == "length" || k == "contentStart";
  }

  void walk(const nlohmann::json& a, const nlohmann::json& b, std::string_view key, const std::string& path) {
    if (a.is_object()) {
      if (!b.is_object() || a.size() != b.size()) return fail(path, "object shape differs");
      for (const auto& [k, v] : a.items()) {
        if (!b.contains(k)) return fail(path, "missing key " + k);
        walk(v, b.at(k), k, path + "." + k);
      }
    } else if (a.is_array()) {
      if (!b.is_array() || a.size() != b.size()) return fail(path, "array shape differs");
      for (size_t i = 0; i < a.size(); ++i) walk(a[i], b[i], key, path + "[" + std::to_string(i) + "]");
    } else if (a.is_number_integer()) {
      if (!b.is_number_integer()) return fail(path, "type differs");
      const int64_t x = a.get<int64_t>(), y = b.get<int64_t>();
      if (path == "$.schemaVersion") {
        if (x != 1 || y != kSchemaVersion) fail(path, "schema version");
      } else if (isTickKey(key)) {
        ++scaled;
        if (x != 0) ++scaledNonZero;
        keys.insert(std::string(key));
        for (const char* o : {"$.tracks", "$.returns", "$.master", "$.loop", "$.locators", "$.timeline", ".children",
                              ".notes", ".points", ".clips"})
          if (path.find(o) != std::string::npos) owners.insert(o);
        if (y != 8 * x) fail(path, std::to_string(y) + " != 8 * " + std::to_string(x));
      } else if (x != y) {
        fail(path, "value changed");
      }
    } else if (a != b) {
      fail(path, "value changed");
    }
  }
  void fail(const std::string& path, const std::string& what) { mismatches.push_back(path + ": " + what); }
};

}  // namespace

// fixtures/schema1 is fixtures/full as the schema-1 build wrote it (main at 39797d7), except that its two edge notes
// at ±2^52 (kMaxTicks) are reduced to ±2^49, the largest schema-1 ticks that still fit kMaxTicks after × 8, and the
// return track got an automation lane (ids 365–367) so every place a tick can live is covered.
// T4.1: the device chains the 2 -> 3 step adds, removed from every track of a re-saved document (and counted).
struct StrippedDevices {
  size_t midi = 0, other = 0;  // tracks whose chain was [BasicSynth] / []
  size_t unexpected = 0;       // any other chain
};
void stripDevices(nlohmann::json& t, StrippedDevices& s) {
  if (!t.is_object()) return;
  if (auto it = t.find("devices"); it != t.end()) {
    const bool midi = t.value("kind", std::string()) == "midi";
    if (midi && it->size() == 1 && (*it)[0].value("patch", std::string()) == "BasicSynth") ++s.midi;
    else if (!midi && it->empty()) ++s.other;
    else ++s.unexpected;
    t.erase(it);
  }
  if (auto c = t.find("children"); c != t.end() && c->is_array())
    for (nlohmann::json& child : *c) stripDevices(child, s);
}
StrippedDevices stripAllDevices(nlohmann::json& doc) {
  StrippedDevices s;
  for (const char* list : {"tracks", "returns"})
    for (nlohmann::json& t : doc[list]) stripDevices(t, s);
  stripDevices(doc["master"], s);
  return s;
}

TEST_CASE("serialize: a schema-1 project (PPQ 27720) loads, migrates every tick x8 and re-saves as the current schema") {
  static_assert(kSchemaVersion == 4);
  const std::string oldText = readFile(fixturePath("schema1/project.pqd"));
  REQUIRE(oldText.rfind("{\n  \"schemaVersion\": 1,", 0) == 0);

  Result<LoadedProject> r = loadProject(fixturePath("schema1"));
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  CHECK(r->fileSchemaVersion == 1);
  Result<std::string> again = serializeProject(*r->project, r->view);
  REQUIRE(again.ok());
  CHECK(again->rfind("{\n  \"schemaVersion\": 4,", 0) == 0);

  nlohmann::json resaved = nlohmann::json::parse(*again);
  const StrippedDevices sd = stripAllDevices(resaved);  // 2 -> 3: checked on its own below
  CHECK(sd.midi > 0);
  CHECK(sd.unexpected == 0);
  ScaleCheck c;
  c.walk(nlohmann::json::parse(oldText), resaved, {}, "$");
  for (const std::string& m : c.mismatches) MESSAGE(m);
  CHECK(c.mismatches.empty());
  MESSAGE(c.scaled << " tick values scaled (" << c.scaledNonZero << " non-zero)");
  CHECK(c.scaled > 300);
  CHECK(c.scaledNonZero > 300);
  CHECK(c.keys == std::set<std::string>{"tick", "start", "end", "length", "contentStart"});
  CHECK(c.owners == std::set<std::string>{"$.tracks", "$.returns", "$.master", "$.loop", "$.locators", "$.timeline",
                                          ".children", ".notes", ".points", ".clips"});

  // The re-saved file is current: it loads without migration to the same project.
  Result<LoadedProject> back = parseProject(*again);
  REQUIRE(back.ok());
  CHECK(back->fileSchemaVersion == kSchemaVersion);
  CHECK(*back->project == *r->project);
  CHECK(viewEq(back->view, r->view));
  // Musical positions are unchanged: the old loop (bar 2 to bar 4 at PPQ 27720) is bar 2 to bar 4 now.
  CHECK(r->project->loop.start == pq::kTicksPerWhole);
  CHECK(r->project->loop.end == 3 * pq::kTicksPerWhole);
}

TEST_CASE("serialize: schema 1 -> 2 migration range errors") {
  const std::string oldText = readFile(fixturePath("schema1/project.pqd"));
  const std::string loop = "\"loop\": {\"start\": 110880,";
  REQUIRE(oldText.find(loop) != std::string::npos);
  auto withLoopStart = [&](const std::string& v) {
    std::string t = oldText;
    t.replace(t.find(loop), loop.size(), "\"loop\": {\"start\": " + v + ",");
    return t;
  };
  // x8 would overflow int64: refused by the migration.
  Result<LoadedProject> big = parseProject(withLoopStart("2305843009213693952"));  // 2^61
  REQUIRE_FALSE(big.ok());
  CHECK(big.error().message == "$: migrating schema 1 to 2 failed: $.loop.start: tick out of range");
  Result<LoadedProject> neg = parseProject(withLoopStart("-2305843009213693952"));
  REQUIRE_FALSE(neg.ok());
  CHECK(neg.error().message == "$: migrating schema 1 to 2 failed: $.loop.start: tick out of range");
  // Fits int64 after x8 but beyond kMaxTicks: the decoder rejects the migrated value with its path.
  Result<LoadedProject> far = parseProject(withLoopStart(std::to_string(-(kMaxTicks / 8 + 1))));
  REQUIRE_FALSE(far.ok());
  CHECK(far.error().message == "$.loop.start: tick out of range");
  // The most negative schema-1 tick that still fits migrates.
  Result<LoadedProject> edge = parseProject(withLoopStart(std::to_string(-(kMaxTicks / 8))));
  REQUIRE_MESSAGE(edge.ok(), (edge.ok() ? std::string() : edge.error().message));
  CHECK(edge->project->loop.start == -kMaxTicks);
}

// ---- Schema 3 -> 4 (T4.7: modulation wires) -------------------------------------------------------------------------

// fixtures/schema3 is fixtures/full as the schema-3 build wrote it (main at 01210d7). Schema 4 only adds the optional
// "wires" list, so the migrated project is the rich project itself and re-saves as fixtures/full.
TEST_CASE("serialize: a schema-3 project loads unchanged into schema 4 (no wires)") {
  const std::string oldText = readFile(fixturePath("schema3/project.pqd"));
  REQUIRE(oldText.rfind("{\n  \"schemaVersion\": 3,", 0) == 0);
  Result<LoadedProject> r = loadProject(fixturePath("schema3"));
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  CHECK(r->fileSchemaVersion == 3);
  RichProject f;
  CHECK(*r->project == f.doc());
  CHECK(wiresOf(*r->project).empty());
  Result<std::string> again = serializeProject(*r->project, r->view);
  REQUIRE(again.ok());
  CHECK(*again == readFile(fixturePath("full/project.pqd")));
}

// T4.7: wires round-trip (ids, ends, ports, amounts to the bit, reducers), sorted by id; a broken file is refused.
TEST_CASE("serialize: modulation wires round-trip and are validated") {
  RichProject f;
  IdSource& ids = f.store.ids();
  std::vector<DeviceId> devs;
  forEachTrack(f.doc(), [&](const Track& t, const Track*) {
    for (const DevicePtr& d : t.devices) devs.push_back(d->id);
  });
  REQUIRE(devs.size() >= 2);
  Batch b{"wires", {}};
  Wire a{ids.wire(), devs[0], "lfo", devs[1], "cutoff", -1.0 / 3.0, WireReducer::Sum};
  Wire c{ids.wire(), devs[1], "out2", devs[1], "level", 1.0, WireReducer::Loudest};
  Wire d{ids.wire(), devs[1], "x", devs[0], "q", -0.0, WireReducer::Highest};
  b.commands.push_back(AddWire{a});
  b.commands.push_back(AddWire{c});
  b.commands.push_back(AddWire{d});
  REQUIRE(f.store.dispatch(b).ok());
  const LoadedProject back = roundTrip(f.doc(), f.store.view());
  CHECK(*back.project == f.doc());
  REQUIRE(wiresOf(*back.project).size() == 3);
  CHECK(wiresOf(*back.project)[0] == a);
  CHECK(wiresOf(*back.project)[1] == c);
  CHECK(std::signbit(wiresOf(*back.project)[2].amount));
  Result<std::string> text = serializeProject(f.doc(), f.store.view());
  REQUIRE(text.ok());
  CHECK(text->find("\"reducer\": \"loudest\"") != std::string::npos);
  CHECK(text->find("\"reducer\": \"sum\"") == std::string::npos);  // the default is not written
  auto broken = [&](const std::string& from, const std::string& to) {
    std::string t = *text;
    const size_t at = t.find(from);
    REQUIRE(at != std::string::npos);
    t.replace(at, from.size(), to);
    return parseProject(t);
  };
  const std::string fromA = "\"from\": " + std::to_string(devs[0].value);
  CHECK_FALSE(broken(fromA, "\"from\": 999999").ok());                       // no such device
  CHECK_FALSE(broken("\"amount\": 1.0", "\"amount\": 1.5").ok());         // out of [-1, 1]
  CHECK_FALSE(broken("\"reducer\": \"loudest\"", "\"reducer\": \"loud\"").ok());
  CHECK_FALSE(broken("\"id\": " + std::to_string(c.id.value), "\"id\": " + std::to_string(a.id.value)).ok());  // duplicate id
}

// ---- Schema 2 -> 3 (T4.1: device chains) ---------------------------------------------------------------------------

// fixtures/schema2 is fixtures/full as the schema-2 build wrote it (main at f775b67). Before T4.1 every MIDI track
// played the default instrument implicitly; the migration makes it the first device of the track's chain, with new
// ids above the file's largest id in track order, and gives every other track an empty chain.
namespace {
// The largest "id" anywhere in a JSON text (what the 2 -> 3 migration continues after).
uint32_t maxJsonId(const nlohmann::json& v) {
  uint32_t m = 0;
  if (v.is_object()) {
    for (const auto& [k, x] : v.items()) {
      if (k == "id" && x.is_number_unsigned()) m = std::max(m, x.get<uint32_t>());
      m = std::max(m, maxJsonId(x));
    }
  } else if (v.is_array()) {
    for (const nlohmann::json& x : v) m = std::max(m, maxJsonId(x));
  }
  return m;
}
}  // namespace

TEST_CASE("serialize: a schema-2 project migrates to device chains (the default instrument on every MIDI track)") {
  const std::string oldText = readFile(fixturePath("schema2/project.pqd"));
  REQUIRE(oldText.rfind("{\n  \"schemaVersion\": 2,", 0) == 0);
  Result<LoadedProject> r = loadProject(fixturePath("schema2"));
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  CHECK(r->fileSchemaVersion == 2);

  // The rich project gives its MIDI tracks their default instruments last (ids after everything, in track order):
  // the migrated file is exactly it.
  RichProject f;
  size_t midiTracks = 0;
  forEachTrack(f.doc(), [&](const Track& t, const Track*) {
    if (t.kind != TrackKind::Midi) {
      CHECK(t.devices.empty());
      return;
    }
    ++midiTracks;
    REQUIRE(t.devices.size() == 1);
    CHECK(t.devices[0]->patch == kDefaultInstrument);
    CHECK(t.devices[0]->id.value > maxJsonId(nlohmann::json::parse(oldText)));
  });
  REQUIRE(midiTracks >= 2);
  CHECK(*r->project == f.doc());
  CHECK(viewEq(r->view, f.store.view()));

  // Re-saved, it is the current schema and loads to the same project without migration.
  Result<std::string> again = serializeProject(*r->project, r->view);
  REQUIRE(again.ok());
  Result<LoadedProject> back = parseProject(*again);
  REQUIRE(back.ok());
  CHECK(back->fileSchemaVersion == kSchemaVersion);
  CHECK(*back->project == *r->project);
  // Apart from the chains, the re-saved file is the old one value for value.
  nlohmann::json resaved = nlohmann::json::parse(*again);
  nlohmann::json old = nlohmann::json::parse(oldText);
  const StrippedDevices sd = stripAllDevices(resaved);
  CHECK(sd.midi == midiTracks);
  CHECK(sd.unexpected == 0);
  resaved["schemaVersion"] = 2;
  CHECK(resaved == old);
}

TEST_CASE("serialize: device chains round-trip; the reader checks the chain rule and the values") {
  RichProject f;
  IdSource& ids = f.store.ids();
  const Snapshot s = f.store.current();
  TrackId midi, other;
  forEachTrack(*s, [&](const Track& t, const Track*) {
    if (t.kind == TrackKind::Midi && !midi.valid()) midi = t.id;
    if (t.kind == TrackKind::Group && !other.valid()) other = t.id;
  });
  REQUIRE(midi.valid());
  const DeviceId old = findTrackPtr(*s, midi)->devices.at(0)->id;
  Device inst = *makeDefaultInstrument(ids.device());
  inst.params = {{"attack", 0.02}, {"level", 0.5}};
  Device fx = *makeDevice(ids.device(), "saturator", DeviceKind::AudioEffect);
  fx.active = false;
  fx.settings = {{"hq", 4.0}, {"type", 6.0}};
  fx.params = {{"drive", 3.981071705534973}};
  Batch b{"chain", {RemoveDevice{old}, AddDevice{midi, std::make_shared<const Device>(inst), 0},
                    AddDevice{midi, std::make_shared<const Device>(fx), kAppend}}};
  if (other.valid()) b.commands.push_back(AddDevice{other, makeDevice(ids.device(), "utility", DeviceKind::AudioEffect), 0});
  REQUIRE(f.store.dispatch(b).ok());
  std::string text;
  const LoadedProject back = roundTrip(*f.store.current(), f.store.view(), &text);
  CHECK(*back.project == *f.store.current());
  CHECK(text.find("{\"id\": " + std::to_string(fx.id.value) +
                  ", \"patch\": \"saturator\", \"kind\": \"audioEffect\", \"active\": false, \"params\": {\"drive\": "
                  "3.981071705534973}, \"settings\": {\"hq\": 4.0, \"type\": 6.0}}") != std::string::npos);

  auto withChain = [&](const std::string& chain) {
    std::string t = text;
    const std::string key = "\"id\": " + std::to_string(inst.id.value) + ", \"patch\": \"BasicSynth\"";
    const size_t at = t.find(key);
    REQUIRE(at != std::string::npos);
    const size_t open = t.rfind('[', at), close = t.find("]\n", at);
    t.replace(open, close + 1 - open, chain);
    return t;
  };
  auto fails = [&](const std::string& chain, const std::string& message) {
    Result<LoadedProject> r = parseProject(withChain(chain));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find(message) != std::string::npos);
    if (r.error().message.find(message) == std::string::npos) MESSAGE(r.error().message);
  };
  const std::string idA = std::to_string(inst.id.value), idB = std::to_string(fx.id.value);
  fails("[{\"id\": " + idB + ", \"patch\": \"utility\", \"kind\": \"audioEffect\"}, {\"id\": " + idA +
            ", \"patch\": \"BasicSynth\", \"kind\": \"instrument\"}]",
        "the instrument must be the first device of a MIDI track");
  fails("[{\"id\": " + idA + ", \"patch\": \"\", \"kind\": \"instrument\"}]", "must not be empty");
  fails("[{\"id\": " + idA + ", \"patch\": \"x\", \"kind\": \"midiEffect\"}]", "device kind must be");
  fails("[{\"id\": " + idA + ", \"patch\": \"x\", \"kind\": \"instrument\", \"params\": {\"a b\": 1}}]",
        "is not a device param or setting name");
  fails("[{\"id\": " + idA + ", \"patch\": \"x\", \"kind\": \"instrument\", \"params\": {\"a\": 1, \"a\": 2}}]",
        "duplicate key");
  fails("[{\"id\": " + idA + ", \"patch\": \"x\", \"kind\": \"instrument\"}, {\"id\": " + idA +
            ", \"patch\": \"y\", \"kind\": \"audioEffect\"}]",
        "device id twice in a chain");
  fails("[{\"patch\": \"x\", \"kind\": \"instrument\"}]", "missing key \"id\"");
}
