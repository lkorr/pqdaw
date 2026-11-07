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

#include "pq/document/devices.h"
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
    // (Schema 3; GU.11: its one device a value block.)
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
  const Migration table[] = {{0, migrate0to1}, builtinMigrations()[0], builtinMigrations()[1], builtinMigrations()[2],
                             builtinMigrations()[3], builtinMigrations()[4], builtinMigrations()[5],
                             builtinMigrations()[6], builtinMigrations()[7], builtinMigrations()[8],
                             builtinMigrations()[9], builtinMigrations()[10], builtinMigrations()[11],
                             builtinMigrations()[12], builtinMigrations()[13], builtinMigrations()[14],
                             builtinMigrations()[15], builtinMigrations()[16], builtinMigrations()[17],
                             builtinMigrations()[18], builtinMigrations()[19]};
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
    const size_t at = newer.find("\"schemaVersion\": 21");
    REQUIRE(at != std::string::npos);
    newer.replace(at, 19, "\"schemaVersion\": 22");
    Result<LoadedProject> r = parseProject(newer);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == pq::ErrorCode::Unsupported);
    CHECK(r.error().message ==
          "$.schemaVersion: the project was saved by a newer pqdaw (schema 22; this build reads up to 21)");
  }
  SUBCASE("a migration's output is validated like any file") {
    const Migration broken[] = {{0, [](nlohmann::json& d) -> pq::Status {
                                   d["timeline"]["tempo"] = nlohmann::json::array();
                                   d["locators"] = nlohmann::json::array();
                                   d.erase("bpm");
                                   return pq::Ok{};
                                 }},
                                builtinMigrations()[0], builtinMigrations()[1], builtinMigrations()[2],
                                builtinMigrations()[3], builtinMigrations()[4], builtinMigrations()[5],
                             builtinMigrations()[6], builtinMigrations()[7], builtinMigrations()[8],
                             builtinMigrations()[9], builtinMigrations()[10], builtinMigrations()[11],
                             builtinMigrations()[12], builtinMigrations()[13], builtinMigrations()[14],
                             builtinMigrations()[15], builtinMigrations()[16], builtinMigrations()[17],
                             builtinMigrations()[18], builtinMigrations()[19]};
    Result<LoadedProject> r = parseProject(old, LoadOptions{broken, {}});
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.rfind("$.timeline.tempo: ", 0) == 0);
  }
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
  Wire a{ids.wire(), devs[0], "lfo", devs[1], "cutoff", -1.0 / 3.0, WireReducer::Sum, {}, WireLaw::Travel};
  Wire c{ids.wire(), devs[1], "out2", devs[1], "level", 1.0, WireReducer::Loudest, {}, WireLaw::Travel};
  Wire d{ids.wire(), devs[1], "x", devs[0], "q", -0.0, WireReducer::Highest, {}, WireLaw::Travel};
  b.commands.push_back(AddWire{a});
  b.commands.push_back(AddWire{c});
  b.commands.push_back(AddWire{d});
  REQUIRE(f.store.dispatch(b).ok());
  const LoadedProject back = roundTrip(f.doc(), f.store.view());
  CHECK(*back.project == f.doc());
  WireList drawn;  // the three drawn here (TX.all-free: beside the main paths' links and the instruments' inner wires)
  for (const Wire& x : wiresOf(*back.project))
    if (x.id == a.id || x.id == c.id || x.id == d.id) drawn.push_back(x);
  REQUIRE(drawn.size() == 3);
  CHECK(drawn[0] == a);
  CHECK(drawn[1] == c);
  CHECK(std::signbit(drawn[2].amount));
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

// TMM.10-fix2 (schema 12): a wire from a MIDI track's note source is written with "notes" instead of "from", round-trips,
// and is validated (a MIDI track, a note output, not both ends kinds at once). A schema-11 file (every wire has "from")
// loads unchanged.
TEST_CASE("serialize: wires from a note source (schema 12) round-trip and are validated; schema 11 loads unchanged") {
  RichProject f;
  IdSource& ids = f.store.ids();
  std::vector<DeviceId> devs;
  TrackId midi, other;
  forEachTrack(f.doc(), [&](const Track& t, const Track*) {
    if (t.kind == TrackKind::Midi && !midi.valid()) midi = t.id;
    if (t.kind != TrackKind::Midi && !other.valid()) other = t.id;
    for (const DevicePtr& d : t.devices) devs.push_back(d->id);
  });
  REQUIRE(midi.valid());
  REQUIRE(other.valid());
  REQUIRE(devs.size() >= 2);
  // Schema 11: the project as it is (device wires only) — the same text with the old version loads to the same project.
  {
    Batch b{"wire", {}};
    b.commands.push_back(AddWire{Wire{ids.wire(), devs[0], "lfo", devs[1], "cutoff", 0.25, WireReducer::Sum, {}, WireLaw::Travel}});
    REQUIRE(f.store.dispatch(b).ok());
    Result<std::string> text = serializeProject(f.doc(), f.store.view());
    REQUIRE(text.ok());
    std::string old = *text;
    old.replace(old.find("\"schemaVersion\": 21"), 19, "\"schemaVersion\": 11");
    Result<LoadedProject> r = parseProject(old);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r->fileSchemaVersion == 11);
    CHECK(*r->project == f.doc());
  }
  Wire n;
  n.id = ids.wire();
  n.noteSource = midi;
  n.port = "hz";
  n.to = devs[1];
  n.param = "cutoff";
  n.amount = 0.125;
  n.law = WireLaw::Travel;
  n.reducer = WireReducer::Highest;
  Batch b{"notes", {}};
  b.commands.push_back(AddWire{n});
  REQUIRE(f.store.dispatch(b).ok());
  // The same ends again is refused; a device wire with the same port names is a different wire.
  Wire twice = n;
  twice.id = ids.wire();
  CHECK_FALSE(f.store.dispatch(Batch{"again", {AddWire{twice}}}).ok());
  Wire bad = n;
  bad.id = ids.wire();
  bad.noteSource = other;  // not a MIDI track
  CHECK_FALSE(f.store.dispatch(Batch{"other", {AddWire{bad}}}).ok());
  bad.noteSource = midi;
  bad.port = "pitch";  // not a note output
  CHECK_FALSE(f.store.dispatch(Batch{"port", {AddWire{bad}}}).ok());

  const LoadedProject back = roundTrip(f.doc(), f.store.view());
  CHECK(*back.project == f.doc());
  const Wire* nb = findWire(*back.project, n.id);
  REQUIRE(nb != nullptr);
  CHECK(*nb == n);
  CHECK(nb->fromNoteSource());
  CHECK_FALSE(nb->from.valid());
  Result<std::string> text = serializeProject(f.doc(), f.store.view());
  REQUIRE(text.ok());
  // (TX.all-free: the note output's wire, not the track's main link from its source.)
  const std::string notes = "\"notes\": " + std::to_string(midi.value) + ", \"port\": \"hz\"";
  CHECK(text->find(notes) != std::string::npos);
  auto broken = [&](const std::string& from, const std::string& to) {
    std::string t = *text;
    const size_t at = t.find(from);
    REQUIRE(at != std::string::npos);
    t.replace(at, from.size(), to);
    return parseProject(t);
  };
  CHECK_FALSE(broken(notes, "\"notes\": " + std::to_string(other.value) + ", \"port\": \"hz\"").ok());  // not a MIDI track
  CHECK_FALSE(broken(notes, "\"notes\": 999999, \"port\": \"hz\"").ok());                           // no such track
  CHECK_FALSE(broken(notes, notes + ", \"from\": " + std::to_string(devs[0].value)).ok());  // both kinds of source
  CHECK_FALSE(broken(notes, "\"notes\": " + std::to_string(midi.value) + ", \"port\": \"pitch\"").ok());  // no such note output
  // Removing the track removes the wires from its note source in the same command (one undo step).
  REQUIRE(f.store.dispatch(RemoveTrack{midi}).ok());
  CHECK(findWire(f.doc(), n.id) == nullptr);
}

// ---- Device lists (T4.1; GU.11: blocks only) ----------------------------------------------------------------------

TEST_CASE("serialize: device chains round-trip; the reader checks the chain rule and the values") {
  RichProject f;
  IdSource& ids = f.store.ids();
  const Snapshot s = f.store.current();
  TrackId other;
  forEachTrack(*s, [&](const Track& t, const Track*) {
    if (t.kind == TrackKind::Group && !other.valid()) other = t.id;
  });
  REQUIRE(other.valid());
  Device fx = *makeDevice(ids.device(), std::string(kValueDevice), DeviceKind::AudioEffect);
  fx.active = false;
  fx.params = {{std::string(kValueParam), 0.3981071705534973}};
  const DevicePtr atom = makeDevice(ids.device(), std::string(kAtomDevicePrefix) + "mul", DeviceKind::AudioEffect);
  Batch b{"chain", {AddDevice{other, std::make_shared<const Device>(fx), 0}, AddDevice{other, atom, kAppend}}};
  REQUIRE(f.store.dispatch(b).ok());
  std::string text;
  const LoadedProject back = roundTrip(*f.store.current(), f.store.view(), &text);
  CHECK(*back.project == *f.store.current());
  CHECK(text.find("{\"id\": " + std::to_string(fx.id.value) +
                  ", \"patch\": \"Value\", \"kind\": \"audioEffect\", \"active\": false, \"params\": {\"value\": "
                  "0.3981071705534973}}") != std::string::npos);
  CHECK(text.find("{\"id\": " + std::to_string(atom->id.value) + ", \"patch\": \"atom:mul\"") != std::string::npos);

  auto withChain = [&](const std::string& chain) {
    std::string t = text;
    const std::string key = "\"id\": " + std::to_string(fx.id.value) + ", \"patch\": \"Value\"";
    const size_t at = t.find(key);
    REQUIRE(at != std::string::npos);
    // The device list holding it: its '[' and the matching ']'.
    const size_t open = t.rfind('[', at);
    size_t close = open;
    for (int depth = 0; close < t.size(); ++close) {
      if (t[close] == '[') ++depth;
      if (t[close] == ']' && --depth == 0) break;
    }
    REQUIRE(close < t.size());
    t.replace(open, close + 1 - open, chain);
    return t;
  };
  auto fails = [&](const std::string& chain, const std::string& message) {
    Result<LoadedProject> r = parseProject(withChain(chain));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find(message) != std::string::npos);
    if (r.error().message.find(message) == std::string::npos) MESSAGE(r.error().message);
  };
  REQUIRE(parseProject(withChain("[{\"id\": " + std::to_string(fx.id.value) + ", \"patch\": \"Value\", \"kind\": \"audioEffect\"}]")).ok());
  const std::string idA = std::to_string(fx.id.value);
  // (TX.all-free: a track's blocks have no order rule — the cables are the signal path; test_devices.cpp checks it.)
  fails("[{\"id\": " + idA + ", \"patch\": \"\", \"kind\": \"audioEffect\"}]", "must not be empty");
  fails("[{\"id\": " + idA + ", \"patch\": \"Value\", \"kind\": \"midiEffect\"}]", "device kind must be");
  fails("[{\"id\": " + idA + ", \"patch\": \"Value\", \"kind\": \"audioEffect\", \"params\": {\"a b\": 1}}]",
        "is not a device param or setting name");
  fails("[{\"id\": " + idA + ", \"patch\": \"Value\", \"kind\": \"audioEffect\", \"params\": {\"a\": 1, \"a\": 2}}]",
        "duplicate key");
  fails("[{\"id\": " + idA + ", \"patch\": \"Value\", \"kind\": \"audioEffect\"}, {\"id\": " + idA +
            ", \"patch\": \"atom:add\", \"kind\": \"audioEffect\"}]",
        "device id twice in a chain");
  fails("[{\"patch\": \"Value\", \"kind\": \"audioEffect\"}]", "missing key \"id\"");
  // GU.11: only blocks (a group, an atom block, a value block, a native engine); a stock patch name, an unknown atom,
  // a device's own patch text and a chain-input choice are refused.
  fails("[{\"id\": " + idA + ", \"patch\": \"utility\", \"kind\": \"audioEffect\"}]", "is not a block");
  fails("[{\"id\": " + idA + ", \"patch\": \"atom:no_such_atom\", \"kind\": \"audioEffect\"}]", "is not a block");
  fails("[{\"id\": " + idA + ", \"patch\": \"Value\", \"kind\": \"audioEffect\", \"source\": [\"x\"]}]",
        "no patch text of its own");
  fails("[{\"id\": " + idA + ", \"patch\": \"meta:wavetable\", \"kind\": \"audioEffect\", \"chainInput\": \"x\"}]",
        "main input is its type's");
}

// ---- Schema 17 -> 18 (TX.pan-laws: a track's pan law) -------------------------------------------------------------

namespace {
void stripPanModes(nlohmann::json& node) {
  if (node.is_object()) {
    if (auto m = node.find("mixer"); m != node.end() && m->is_object()) m->erase("panMode");
    for (auto it = node.begin(); it != node.end(); ++it) stripPanModes(*it);
  } else if (node.is_array()) {
    for (nlohmann::json& e : node) stripPanModes(e);
  }
}
}  // namespace

TEST_CASE("serialize: pan laws (schema 18) round-trip on every kind of track; schema 17 keeps constant power on MIDI tracks") {
  RichProject f;
  Batch b{"laws", {}};
  b.commands.push_back(SetTrackPanMode{f.drums, PanMode::Stereo});       // a group
  b.commands.push_back(SetTrackPanMode{f.reverb, PanMode::Linear});      // a return
  b.commands.push_back(SetTrackPanMode{f.master, PanMode::ConstantPower});
  b.commands.push_back(SetTrackPanMode{f.lead, PanMode::Balance});       // a MIDI track back on the default
  REQUIRE(f.store.dispatch(b).ok());
  std::string text;
  const LoadedProject back = roundTrip(f.doc(), f.store.view(), &text);
  CHECK(*back.project == f.doc());
  CHECK(findTrackPtr(*back.project, f.drums)->mixer.panMode == PanMode::Stereo);
  CHECK(findTrackPtr(*back.project, f.reverb)->mixer.panMode == PanMode::Linear);
  CHECK(findTrackPtr(*back.project, f.master)->mixer.panMode == PanMode::ConstantPower);
  CHECK(findTrackPtr(*back.project, f.lead)->mixer.panMode == PanMode::Balance);
  CHECK(findTrackPtr(*back.project, f.kick)->mixer.panMode == PanMode::ConstantPower);  // RichProject: every MIDI track
  CHECK(text.find("\"panMode\": \"stereo\"") != std::string::npos);
  CHECK(text.find("\"panMode\": \"balance\"") == std::string::npos);  // the default is not written

  // An unknown law is refused with its path.
  std::string bad = text;
  bad.replace(bad.find("\"panMode\": \"stereo\""), 19, "\"panMode\": \"wide\"");
  Result<LoadedProject> refused = parseProject(bad);
  REQUIRE_FALSE(refused.ok());
  CHECK(refused.error().message.find("unknown pan law \"wide\"") != std::string::npos);

  // The same project as schema 17 wrote it (no "panMode" anywhere): every MIDI track gets constant power (its law then),
  // everything else Balance — the project sounds as it did.
  nlohmann::json old = nlohmann::json::parse(text);
  stripPanModes(old);
  old["schemaVersion"] = 17;
  Result<LoadedProject> r = parseProject(old.dump(2));
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  CHECK(r->fileSchemaVersion == 17);
  size_t midi = 0;
  forEachTrack(*r->project, [&](const Track& t, const Track*) {
    CAPTURE(t.name);
    CHECK(t.mixer.panMode == (t.kind == TrackKind::Midi ? PanMode::ConstantPower : PanMode::Balance));
    midi += t.kind == TrackKind::Midi ? 1u : 0u;
  });
  CHECK(midi >= 3);
  CHECK(r->project->master->mixer.panMode == PanMode::Balance);
  for (const TrackPtr& t : r->project->returns) CHECK(t->mixer.panMode == PanMode::Balance);
}
