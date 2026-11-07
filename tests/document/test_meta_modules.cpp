// TMM.3: meta modules in the document — the project file (schema 10: a native engine's own state deep-equal after save
// → load, strict errors with JSON paths, type versions), the edit builders (one undo step each, a drag gesture one
// step), the lowering (the golden text of a chain `meta:envelope`; an attack drag keeps the IR and its hash; settings and
// static-ness change the expansion; resources and their slots). GU.4: the example module was the Phase accumulator (a
// group preset now); the Envelope plays its part. GU.11: a native engine (`meta:<type>`) is a leaf block holding only
// its own state (path ""); nested meta calls in a device's own patch text are gone with user devices.
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../meta/meta_test_type.h"
#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/graph/parse.h"
#include "pq/meta/envelope.h"
#include "pq/meta/registry.h"

using namespace pq::doc;
using pq::Result;
namespace meta = pq::meta;

namespace {

std::string stdlibDir() { return std::string(PQ_TEST_DATA_DIR) + "/../stdlib"; }
std::string fixturePath(const std::string& rel) { return std::string(PQ_TEST_DATA_DIR) + "/document/fixtures/" + rel; }

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool updateGoldens() {
  const char* v = std::getenv("PQ_UPDATE_GOLDENS");
  return v != nullptr && *v != '\0' && *v != '0';
}

void checkGolden(const std::string& name, const std::string& text) {
  const std::string path = fixturePath("meta/" + name);
  if (updateGoldens()) std::ofstream(path, std::ios::binary) << text;
  const std::string gold = readFile(path);
  CAPTURE(text);
  CHECK(text == gold);
}

DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    meta::testing::registerTestType();  // before the library takes its prelude snapshot
    auto r = loadDeviceLibrary(stdlibDir());
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// A MIDI track "Synth" (its default instrument group), a MIDI track "Acc" whose instrument is a chain Envelope (its
// default instrument group deleted first, so the Envelope takes the notes).
struct Song {
  DocumentStore store;
  TrackId synthTrack, accTrack;
  DeviceId synth, acc;
  Song() {
    meta::testing::registerTestType();
    synthTrack = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    accTrack = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    synth = mainPath(doc(), *findTrackPtr(doc(), synthTrack)).blocks.at(0)->id;
    const DeviceId inst = mainPath(doc(), *findTrackPtr(doc(), accTrack)).blocks.at(0)->id;
    edit(ops::deleteDevices(doc(), std::vector<DeviceId>{inst}));
    acc = edit(ops::addMetaDevice(doc(), store.ids(), accTrack, "envelope", 0)).devices.at(0);
  }
  const Project& doc() const { return *store.current(); }
  const Device& dev(DeviceId d) const { return *findDevice(doc(), d).device; }
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    REQUIRE(store.dispatch(e->command).ok());
    CHECK(store.history().cursor() == before + 1);  // one undo step
    return *e;
  }
  // A chain test module (settings, data, a resource) as an audio effect after the synth's instrument group (added off
  // the path, then spliced in at its end: two steps).
  DeviceId addTestShape() {
    const DeviceId d = edit(ops::addMetaDevice(doc(), store.ids(), synthTrack, "test_shape", ops::kOffPath)).devices.at(0);
    edit(ops::insertOnPath(doc(), store.ids(), d, kAppend));
    return d;
  }
};

LoweredProject lowered(const Project& p, const LowerOptions* base = nullptr, const LoweredProject* previous = nullptr) {
  LowerOptions o = base != nullptr ? *base : LowerOptions{};
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  Result<LoweredProject> lp = lower(p, o, previous);
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  for (const LoweredProject::DeviceError& e : lp->deviceErrors) FAIL_CHECK(e.message);
  return std::move(*lp);
}

size_t trackIndex(const LoweredProject& lp, TrackId t) {
  for (size_t i = 0; i < lp.trackIds.size(); ++i)
    if (lp.trackIds[i] == t) return i;
  FAIL("track not lowered");
  return 0;
}

const LoweredParam* paramOf(const LoweredProject& lp, const std::string& key) {
  for (const LoweredParam& p : lp.params)
    if (p.key == key) return &p;
  return nullptr;
}

// The callee a track patch's device call `d<id>` calls.
std::string calleeOf(const LoweredProject& lp, TrackId t, DeviceId d) {
  const pq::graph::Patch& p = lp.patches[trackIndex(lp, t)]->patch;
  const auto n = p.findByName("d" + std::to_string(d.value));
  REQUIRE(n.has_value());
  return p.subcircuit(*n).patch;
}

// The resources of one type (TMM.5: a chain Envelope lists its voice tap too).
std::vector<LoweredProject::MetaResource> resourcesOf(const LoweredProject& lp, std::string_view type) {
  std::vector<LoweredProject::MetaResource> out;
  for (const LoweredProject::MetaResource& r : lp.metaResources)
    if (r.type == type) out.push_back(r);
  return out;
}

meta::DataPtr testData(std::vector<float> v) {
  return std::make_shared<const meta::DataValue>(meta::DataValue::record({{"values", meta::DataValue::floatArray(std::move(v))}}));
}

}  // namespace

TEST_CASE("meta modules: save -> load is deep-equal with native engines' own state (schema 10)") {
  Song s;
  s.edit(ops::setMetaParamValue(s.doc(), s.acc, "", "envelope", "acurve", 0.5));
  // A test module with settings and data.
  const DeviceId ts = s.addTestShape();
  s.edit(ops::setMetaSetting(s.doc(), ts, "", "test_shape", "stages", 3.0));
  const std::vector<float> cycle = {0.0f, 0.5f, -0.25f, 1e-30f, -3.5f, 1.0f / 3.0f, 7.0f};
  s.edit(ops::setMetaData(s.doc(), ts, "", "test_shape", testData(cycle)));
  Result<std::string> text = serializeProject(s.doc(), s.store.view());
  REQUIRE(text.ok());
  CHECK(text->rfind("{\n  \"schemaVersion\": 21,", 0) == 0);
  CHECK(text->find("\"patch\": \"meta:envelope\"") != std::string::npos);
  CHECK(text->find("\"$f32\"") != std::string::npos);
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == s.doc());
  const Device& tb = *findDevice(*back->project, ts).device;
  const MetaInstance* inst = tb.metaAt("");
  REQUIRE(inst != nullptr);
  CHECK(inst->data->field("values")->floats == cycle);  // bit-exact
  REQUIRE(tb.setting("stages") != nullptr);
  CHECK(tb.setting("stages")->value == 3.0);  // a chain device's settings are its own
  // The writer is stable: a second save is byte-equal.
  Result<std::string> again = serializeProject(*back->project, back->view);
  REQUIRE(again.ok());
  CHECK(*again == *text);
}

TEST_CASE("meta modules: the reader is strict, with JSON paths") {
  Song s;
  const DeviceId ts = s.addTestShape();
  const std::string good = *serializeProject(s.doc(), s.store.view());
  const std::string head = "\"\": {\"type\": \"test_shape\", \"version\": 2";
  REQUIRE_MESSAGE(good.find(head) != std::string::npos, good);
  auto broken = [&](const std::string& from, const std::string& to) {
    std::string t = good;
    const size_t at = t.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, from);
    t.replace(at, from.size(), to);
    return parseProject(t);
  };
  auto fails = [&](const std::string& from, const std::string& to, const std::string& part,
                   pq::ErrorCode code = pq::ErrorCode::InvalidArgument) {
    CAPTURE(to);
    Result<LoadedProject> r = broken(from, to);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find(part) != std::string::npos);
    CHECK(r.error().message.find(".meta") != std::string::npos);  // the path names the meta member
    CHECK(r.error().code == code);
  };
  fails(head, "\"\": {\"type\": \"nope\", \"version\": 2", "unknown meta module type 'nope'",
        pq::ErrorCode::Unsupported);
  fails(head, "\"\": {\"type\": \"test_shape\", \"version\": 7", "newer pqdaw", pq::ErrorCode::Unsupported);
  fails(head, "\"\": {\"type\": \"test_shape\", \"version\": 0", "at least 1");
  fails(head, head + ", \"params\": {\"gain\": 0.5}", "as the device's own");
  fails(head, "\"a..b\": {\"type\": \"test_shape\", \"version\": 2", "not a meta module path");
  fails(head, "\"x\": {\"type\": \"test_shape\", \"version\": 2", "holds only its own state");
  fails(head, "\"\": {\"type\": \"envelope\", \"version\": 1", "only a 'meta:envelope' device");
  // TMM.10-fix1 (SPEC §15 2026-11-01 "no instrument special cases"): a chain meta device's kind is never refused for
  // its type (it is its position; what it plays follows from what reaches its main input). GU.11: its main input is
  // its type's, so a file choosing another ("chainInput") is refused.
  {
    const nlohmann::json j = nlohmann::json::parse(good);
    auto edited = [&](auto&& change) {
      nlohmann::json k = j;
      bool found = false;
      for (nlohmann::json& t : k["tracks"])
        for (nlohmann::json& d : t["devices"])
          if (d["id"] == ts.value) {
            change(d);
            found = true;
          }
      REQUIRE(found);
      return parseProject(k.dump(2));
    };
    CHECK(edited([](nlohmann::json& d) { d["kind"] = d["kind"] == "instrument" ? "audioEffect" : "instrument"; }).ok());
    Result<LoadedProject> r = edited([](nlohmann::json& d) { d["chainInput"] = "x"; });
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("main input is its type's") != std::string::npos);
  }
}

TEST_CASE("meta modules: data values round-trip through the file; a type's older state is upgraded") {
  const meta::Type& t = meta::testing::registerTestType();
  Song s;
  const DeviceId ts = s.addTestShape();
  s.edit(ops::setMetaSetting(s.doc(), ts, "", "test_shape", "stages", 2.0));
  std::string text = *serializeProject(s.doc(), s.store.view());
  // The test type is at version 2; a version-1 state is upgraded by its hook.
  const std::string v2 = "\"type\": \"test_shape\", \"version\": 2";
  REQUIRE(text.find(v2) != std::string::npos);
  text.replace(text.find(v2), v2.size(), "\"type\": \"test_shape\", \"version\": 1");
  Result<LoadedProject> r = parseProject(text);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  CHECK(findDevice(*r->project, ts).device->metaAt("")->version == t.version);
  CHECK(*r->project == s.doc());
  // Data the type rejects fails with its reason.
  std::string bad = *serializeProject(s.doc(), s.store.view());
  const std::string data = "\"data\": {\"values\": ";
  REQUIRE(bad.find(data) != std::string::npos);
  bad.replace(bad.find(data), data.size(), "\"data\": {\"other\": 1.0, \"values\": ");
  Result<LoadedProject> rb = parseProject(bad);
  REQUIRE_FALSE(rb.ok());
  CHECK(rb.error().message.find("test data is") != std::string::npos);
}

TEST_CASE("meta modules: every edit is one undo step; a drag gesture is one; undo restores the state") {
  Song s;
  const Snapshot fresh = s.store.current();
  const size_t freshCursor = s.store.history().cursor();
  const DeviceId ts = s.addTestShape();
  s.edit(ops::setMetaParamValue(s.doc(), s.acc, "", "envelope", "sustain", 0.25));
  s.edit(ops::setMetaParamValue(s.doc(), s.acc, "", "envelope", "attack", 0.3));
  s.edit(ops::setMetaSetting(s.doc(), ts, "", "test_shape", "stages", 4.0));
  s.edit(ops::setMetaData(s.doc(), ts, "", "test_shape", testData({1.0f})));
  CHECK(s.dev(ts).metaAt("")->data->field("values")->floats == std::vector<float>{1.0f});
  const size_t envAttack = static_cast<size_t>(meta::envelopeType().paramIndex("attack"));
  CHECK(metaParamValue(s.dev(s.acc), "", meta::envelopeType(), envAttack) == doctest::Approx(0.3).epsilon(1e-12));  // GU.1
  // A drag: many param writes inside one gesture, one history entry.
  const size_t before = s.store.history().cursor();
  REQUIRE(s.store.beginGesture().ok());
  for (int i = 1; i <= 20; ++i) {
    auto e = ops::setMetaParamValue(s.doc(), s.acc, "", "envelope", "attack", 0.1 * i);
    REQUIRE(e.ok());
    REQUIRE(s.store.dispatch(e->command).ok());
    CHECK(s.store.lastChanges().paramOnly());
  }
  REQUIRE(s.store.commitGesture().ok());
  CHECK(s.store.history().cursor() == before + 1);
  CHECK(metaParamValue(s.dev(s.acc), "", meta::envelopeType(), envAttack) == doctest::Approx(2.0).epsilon(1e-12));
  REQUIRE(s.store.undo());
  CHECK(metaParamValue(s.dev(s.acc), "", meta::envelopeType(), envAttack) == doctest::Approx(0.3).epsilon(1e-12));
  // Undo every step: the fresh song again (the same snapshot).
  while (s.store.history().cursor() > freshCursor) REQUIRE(s.store.undo());
  CHECK(s.store.current() == fresh);
}

TEST_CASE("meta modules: golden lowering text (a chain meta:envelope)") {
  Song s;
  s.edit(ops::setMetaParamValue(s.doc(), s.acc, "", "envelope", "attack", 1.0));
  const LoweredProject lp = lowered(s.doc());
  auto textOf = [&](TrackId t, DeviceId d) {
    std::string out = patchText(*lp.patches[trackIndex(lp, t)]);
    // The callee the device calls, and every meta expansion below it.
    std::vector<std::string> todo{calleeOf(lp, t, d)};
    std::vector<std::string> seen;
    while (!todo.empty()) {
      const std::string name = todo.back();
      todo.pop_back();
      if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
      seen.push_back(name);
      const pq::graph::Patch* p = lp.graph.library->find(name);
      REQUIRE_MESSAGE(p != nullptr, name);
      out += "\n" + pq::graph::print(*p);
      for (pq::graph::NodeIndex n = 0; n < p->nodeCount(); ++n)
        if (p->node(n).kind == pq::graph::NodeKind::Subcircuit) todo.push_back(p->subcircuit(n).patch);
    }
    return out;
  };
  // Ids differ per run of the fixture only through the store's id source, which is deterministic.
  checkGolden("lower_chain_envelope.txt", textOf(s.accTrack, s.acc));
  // The chain device: an instrument expansion; its params are the device's own.
  CHECK(calleeOf(lp, s.accTrack, s.acc).starts_with("meta_envelope__d" + std::to_string(s.acc.value) + "__"));
  const LoweredParam* f = paramOf(lp, "dev" + std::to_string(s.acc.value) + ":attack");
  REQUIRE(f != nullptr);
  CHECK(f->value == doctest::Approx(1.0f));
  CHECK(f->ref == ParamRef::device(s.acc, "attack"));
  // The param fast path lists the same params with the same values.
  const std::vector<LoweredParam> fast = lowerParams(s.doc(), true, library());
  for (const char* name : {"attack", "sustain", "acurve"}) {
    const std::string key = "dev" + std::to_string(s.acc.value) + ":" + name;
    REQUIRE(paramOf(lp, key) != nullptr);
    const auto it = std::find_if(fast.begin(), fast.end(), [&](const LoweredParam& p) { return p.key == key; });
    REQUIRE(it != fast.end());
    CHECK(it->value == paramOf(lp, key)->value);
    CHECK(it->name == paramOf(lp, key)->name);
  }
}

TEST_CASE("meta modules: an attack drag lowers to the same IR (shared patch, same hash)") {
  Song s;
  const LoweredProject a = lowered(s.doc());
  s.edit(ops::setMetaParamValue(s.doc(), s.acc, "", "envelope", "attack", 0.07));
  CHECK(s.store.lastChanges().paramOnly());
  const LoweredProject b = lowered(s.doc(), nullptr, &a);
  const size_t ia = trackIndex(a, s.accTrack), ib = trackIndex(b, s.accTrack);
  CHECK(b.patches[ib] == a.patches[ia]);  // the same patch object: its unit does not recompile
  CHECK(b.patches[ib]->hash == a.patches[ia]->hash);
  CHECK(calleeOf(a, s.accTrack, s.acc) == calleeOf(b, s.accTrack, s.acc));
  CHECK(paramOf(b, "dev" + std::to_string(s.acc.value) + ":attack")->value == doctest::Approx(0.07f));
}

TEST_CASE("meta modules: settings, wires and automation make other expansions; resources get their slots") {
  meta::testing::registerTestType();
  Song s;
  const DeviceId ts = s.addTestShape();
  LowerOptions o;
  o.metaSlotOf = [](DeviceId d, std::string_view path, uint32_t index) {
    return static_cast<uint32_t>(d.value * 100 + path.size() * 10 + index + 1);
  };
  const LoweredProject a = lowered(s.doc(), &o);
  const std::string calleeA = calleeOf(a, s.synthTrack, ts);
  const std::vector<LoweredProject::MetaResource> ra = resourcesOf(a, "test_shape");
  REQUIRE(ra.size() == 1);
  const LoweredProject::MetaResource& r = ra[0];
  CHECK(r.device == ts);
  CHECK(r.path.empty());
  CHECK(r.type == "test_shape");
  CHECK(r.track == s.synthTrack);
  CHECK(r.request.kind == meta::ResourceKind::FrameTable);
  CHECK(r.slot == ts.value * 100 + 1);
  REQUIRE(r.data != nullptr);
  CHECK(*r.data == *meta::testing::testDefaultData());
  // A setting: another expansion (a structural change).
  s.edit(ops::setMetaSetting(s.doc(), ts, "", "test_shape", "stages", 3.0));
  CHECK(s.store.lastChanges().structural());
  const LoweredProject b = lowered(s.doc(), &o, &a);
  CHECK(calleeOf(b, s.synthTrack, ts) != calleeA);
  CHECK(b.patches[trackIndex(b, s.synthTrack)] != a.patches[trackIndex(a, s.synthTrack)]);
  // Data: never part of the expansion (MetaData only; the IR stays shared).
  s.edit(ops::setMetaData(s.doc(), ts, "", "test_shape", testData({0.5f})));
  CHECK(s.store.lastChanges().metaDataOnly());
  const LoweredProject c = lowered(s.doc(), &o, &b);
  CHECK(c.patches[trackIndex(c, s.synthTrack)] == b.patches[trackIndex(b, s.synthTrack)]);
  REQUIRE(resourcesOf(c, "test_shape").size() == 1);
  CHECK(resourcesOf(c, "test_shape")[0].data->field("values")->floats == std::vector<float>{0.5f});
  // A wired param is not static: the modulated (clipped) form.
  s.edit(ops::connectWire(s.doc(), s.store.ids(), s.acc, "out", ts, "gain"));
  const LoweredProject d = lowered(s.doc(), &o);
  const std::string calleeD = calleeOf(d, s.synthTrack, ts);
  CHECK(calleeD != calleeOf(c, s.synthTrack, ts));
  const pq::graph::Patch* exp = d.graph.library->find(calleeD);
  REQUIRE(exp != nullptr);
  bool clipped = false;
  std::vector<const pq::graph::Patch*> todo{exp};
  while (!todo.empty() && !clipped) {
    const pq::graph::Patch* p = todo.back();
    todo.pop_back();
    for (pq::graph::NodeIndex n = 0; n < p->nodeCount(); ++n) {
      if (p->node(n).kind == pq::graph::NodeKind::Atom && pq::graph::atomSignature(p->node(n).atom).name == "clip") clipped = true;
      if (p->node(n).kind == pq::graph::NodeKind::Subcircuit)
        if (const pq::graph::Patch* sub = d.graph.library->find(p->subcircuit(n).patch)) todo.push_back(sub);
    }
  }
  CHECK(clipped);
}

TEST_CASE("meta modules: a chain device's static-ness follows its automation lanes") {
  meta::testing::registerTestType();
  Song s;
  // A chain test module as an audio effect after the synth.
  const DeviceId ts = s.addTestShape();
  CHECK(s.dev(ts).kind == DeviceKind::AudioEffect);
  const LoweredProject a = lowered(s.doc());
  const std::string before = calleeOf(a, s.synthTrack, ts);
  CHECK(paramOf(a, "dev" + std::to_string(ts.value) + ":gain") != nullptr);
  auto lane = std::make_shared<AutomationLane>();
  lane->id = s.store.ids().lane();
  lane->param = ParamRef::device(ts, "gain");
  lane->paramName = "gain";
  REQUIRE(s.store.dispatch(AddAutomationLane{s.synthTrack, lane}).ok());
  const LoweredProject b = lowered(s.doc());
  CHECK(calleeOf(b, s.synthTrack, ts) != before);  // modulated: the live form
  REQUIRE(resourcesOf(b, "test_shape").size() == 1);
  CHECK(resourcesOf(b, "test_shape")[0].path.empty());
}

TEST_CASE("meta modules: commands refuse what the state cannot hold") {
  Song s;
  int step = 0;
  auto fails = [&](const Command& c) {
    ++step;
    CAPTURE(step);
    const Snapshot before = s.store.current();
    CHECK_FALSE(s.store.dispatch(c).ok());
    CHECK(s.store.current() == before);
  };
  // A bare engine (no state yet).
  const DeviceId bare = s.store.ids().device();
  REQUIRE(s.store.dispatch(AddDevice{s.synthTrack, makeDevice(bare, "meta:envelope", DeviceKind::AudioEffect), kAppend}).ok());
  MetaInstance env = defaultMetaInstance(meta::envelopeType());
  fails(AddMetaInstance{s.synth, "", env});      // "" only on a meta device (a group holds no state: see test_devices.cpp)
  fails(AddMetaInstance{bare, "a..b", env});
  fails(AddMetaInstance{bare, "acc1", env});     // GU.11: a native engine holds only its own state
  MetaInstance badParam = env;
  badParam.params = {DeviceValue{"nope", 1.0}};
  fails(AddMetaInstance{bare, "", badParam});
  MetaInstance badVersion = env;
  badVersion.version = 9;
  fails(AddMetaInstance{bare, "", badVersion});
  MetaInstance unknown = env;
  unknown.type = "nope";
  fails(AddMetaInstance{bare, "", unknown});
  fails(SetMetaData{bare, "", testData({1.0f})});  // no state there yet
  REQUIRE(s.store.dispatch(AddMetaInstance{bare, "", env}).ok());
  fails(AddMetaInstance{bare, "", env});  // twice
  fails(SetMetaParam{bare, "", "nope", 1.0});
  fails(SetMetaSetting{bare, "", "nope", 1.0});
  fails(SetMetaParam{s.acc, "", "nope", 1.0});
  // An unknown type is refused.
  CHECK_FALSE(ops::addMetaDevice(s.doc(), s.store.ids(), s.synthTrack, "nope").ok());
}
