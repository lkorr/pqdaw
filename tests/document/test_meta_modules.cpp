// TMM.3: meta modules in the document — the project file (schema 10: nested and chain-level instances deep-equal after
// save → load, a schema-9 file loads unchanged, strict errors with JSON paths, type versions), the edit builders (one undo
// step each, a drag gesture one step, a fresh call's first drag no compile), the lowering (golden texts for a chain
// `meta:phase_acc` and a nested `meta_phase_acc` in a user device; a freq drag keeps the IR and its hash; settings and
// static-ness change the expansion; resources and their slots), and renames that keep the state.
#include <doctest/doctest.h>

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
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/graph/parse.h"
#include "pq/meta/phase_acc.h"
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

std::string replaced(std::string s, const std::string& from, const std::string& to) {
  const size_t at = s.find(from);
  REQUIRE_MESSAGE(at != std::string::npos, from);
  s.replace(at, from.size(), to);
  return s;
}

// BasicSynth with its phasor replaced by a Phase accumulator named `name`.
std::string synthWithAcc(const std::string& name) {
  return replaced(library().stockText("BasicSynth"), "ph  = phasor(voice.hz)",
                  name + " = meta_phase_acc()\n  ph  = " + name);
}

// A MIDI track "Synth" (BasicSynth), a MIDI track "Acc" whose instrument is a chain Phase accumulator, an audio track
// with a chain test module after nothing.
struct Song {
  DocumentStore store;
  TrackId synthTrack, accTrack;
  DeviceId synth, acc;
  Song() {
    meta::testing::registerTestType();
    synthTrack = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    accTrack = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    synth = findTrackPtr(doc(), synthTrack)->devices.at(0)->id;
    acc = edit(ops::addMetaDevice(doc(), store.ids(), accTrack, "phase_acc")).devices.at(0);
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
  // Forks the synth into a user device calling a Phase accumulator `name` (one step), via the patch editor's builder.
  void addAccCall(const std::string& name) { edit(ops::addMetaCall(doc(), synth, synthWithAcc(name), name, "phase_acc")); }
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

// The resources of one type (TMM.5: the Song's chain Phase accumulator lists its voice tap too).
std::vector<LoweredProject::MetaResource> resourcesOf(const LoweredProject& lp, std::string_view type) {
  std::vector<LoweredProject::MetaResource> out;
  for (const LoweredProject::MetaResource& r : lp.metaResources)
    if (r.type == type) out.push_back(r);
  return out;
}

}  // namespace

TEST_CASE("meta modules: save -> load is deep-equal with nested and chain-level instances (schema 10)") {
  Song s;
  s.addAccCall("acc1");
  s.edit(ops::setMetaParam(s.doc(), s.synth, "acc1", "phase_acc", "freq", 123.5));
  s.edit(ops::setMetaParam(s.doc(), s.acc, "", "phase_acc", "amp", 2.0));
  // A test module with settings and data, nested in the synth (its call need not exist: the document is library-free).
  s.edit(ops::setMetaSetting(s.doc(), s.synth, "opA.ts", "test_shape", "stages", 3.0));
  const std::vector<float> cycle = {0.0f, 0.5f, -0.25f, 1e-30f, -3.5f, 1.0f / 3.0f, 7.0f};
  s.edit(ops::setMetaData(s.doc(), s.synth, "opA.ts", "test_shape",
                          std::make_shared<const meta::DataValue>(meta::DataValue::record({{"values", meta::DataValue::floatArray(cycle)}}))));
  Result<std::string> text = serializeProject(s.doc(), s.store.view());
  REQUIRE(text.ok());
  CHECK(text->rfind("{\n  \"schemaVersion\": 10,", 0) == 0);
  CHECK(text->find("\"patch\": \"meta:phase_acc\"") != std::string::npos);
  CHECK(text->find("\"$f32\"") != std::string::npos);
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == s.doc());
  const MetaInstance* ts = findDevice(*back->project, s.synth).device->metaAt("opA.ts");
  REQUIRE(ts != nullptr);
  CHECK(ts->data->field("values")->floats == cycle);  // bit-exact
  CHECK(ts->setting("stages")->value == 3.0);
  // The writer is stable: a second save is byte-equal.
  Result<std::string> again = serializeProject(*back->project, back->view);
  REQUIRE(again.ok());
  CHECK(*again == *text);
}

TEST_CASE("meta modules: a schema-9 file loads unchanged") {
  Result<LoadedProject> old = loadProject(fixturePath("schema9"));
  REQUIRE_MESSAGE(old.ok(), (old.ok() ? std::string() : old.error().message));
  CHECK(old->fileSchemaVersion == 9);
  Result<LoadedProject> cur = loadProject(fixturePath("full"));
  REQUIRE(cur.ok());
  CHECK(*old->project == *cur->project);
  bool anyMeta = false;
  forEachDevice(*old->project, [&](const Device& d, const Track&) { anyMeta = anyMeta || d.meta || isMetaDevice(d); });
  CHECK_FALSE(anyMeta);
}

TEST_CASE("meta modules: the reader is strict, with JSON paths") {
  Song s;
  s.addAccCall("acc1");
  s.edit(ops::setMetaParam(s.doc(), s.synth, "acc1", "phase_acc", "freq", 10.0));
  const std::string good = *serializeProject(s.doc(), s.store.view());
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
  fails("\"type\": \"phase_acc\", \"version\": 1, \"params\"", "\"type\": \"nope\", \"version\": 1, \"params\"",
        "unknown meta module type 'nope'", pq::ErrorCode::Unsupported);
  fails("\"version\": 1, \"params\": {\"freq\"", "\"version\": 7, \"params\": {\"freq\"", "newer pqdaw",
        pq::ErrorCode::Unsupported);
  fails("\"version\": 1, \"params\": {\"freq\"", "\"version\": 0, \"params\": {\"freq\"", "at least 1");
  fails("\"params\": {\"freq\"", "\"params\": {\"frq\"", "'frq' is not a param of phase_acc");
  fails("\"acc1\": {", "\"a..b\": {", "not a meta module path");
  fails("\"acc1\": {", "\"\": {", "only a 'meta:phase_acc' device");
  fails("\"params\": {\"freq\": 10.0}", "\"params\": {\"freq\": 10.0}, \"data\": 1.0", "keeps no data");
  // The chain device's kind must be one the type allows. TMM.10: a Phase accumulator may be an audio effect (it
  // continues a meta voice run: pm), so the refusal is checked with an instrument-only copy of its type.
  {
    static const meta::Type instOnly = [] {
      meta::Type x = meta::phaseAccType();
      x.id = "tmm10_inst_only";
      x.chainVoiceInput = -1;
      return x;
    }();
    if (meta::findType(instOnly.id) == nullptr) (void)meta::registerType(instOnly);
    auto asEffect = [&](const std::string& type) {
      std::string t = good;
      const size_t dev = t.find("\"meta:phase_acc\"");
      REQUIRE(dev != std::string::npos);
      t.replace(dev, 16, "\"meta:" + type + "\"");
      const size_t kind = t.find("\"instrument\"", dev);
      REQUIRE(kind != std::string::npos);
      t.replace(kind, 12, "\"audioEffect\"");
      const size_t inst = t.find("\"type\": \"phase_acc\"", dev);
      REQUIRE(inst != std::string::npos);
      t.replace(inst, 19, "\"type\": \"" + type + "\"");
      return parseProject(t);
    };
    CHECK(asEffect("phase_acc").ok());
    Result<LoadedProject> r = asEffect(std::string(instOnly.id));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("cannot be an audio effect") != std::string::npos);
  }
}

TEST_CASE("meta modules: data values round-trip through the file; a type's older state is upgraded") {
  const meta::Type& t = meta::testing::registerTestType();
  Song s;
  s.edit(ops::setMetaSetting(s.doc(), s.synth, "ts", "test_shape", "stages", 2.0));
  std::string text = *serializeProject(s.doc(), s.store.view());
  // The test type is at version 2; a version-1 state is upgraded by its hook.
  const std::string v2 = "\"type\": \"test_shape\", \"version\": 2";
  REQUIRE(text.find(v2) != std::string::npos);
  text.replace(text.find(v2), v2.size(), "\"type\": \"test_shape\", \"version\": 1");
  Result<LoadedProject> r = parseProject(text);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  CHECK(findDevice(*r->project, s.synth).device->metaAt("ts")->version == t.version);
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
  s.addAccCall("acc1");                                                  // fork + default state: one step
  REQUIRE(s.dev(s.synth).metaAt("acc1") != nullptr);
  s.edit(ops::setMetaParam(s.doc(), s.synth, "acc1", "phase_acc", "phase", 0.25));
  s.edit(ops::setMetaParam(s.doc(), s.acc, "", "phase_acc", "freq", 30.0));
  s.edit(ops::setMetaSetting(s.doc(), s.synth, "x.ts", "test_shape", "stages", 4.0));  // creates "x.ts" too
  s.edit(ops::setMetaData(s.doc(), s.synth, "x.ts", "test_shape",
                          std::make_shared<const meta::DataValue>(meta::DataValue::record({{"values", meta::DataValue::floatArray({1.0f})}}))));
  s.edit(ops::renameMetaCall(s.doc(), s.synth, synthWithAcc("osc"), "acc1", "osc"));
  CHECK(s.dev(s.synth).metaAt("acc1") == nullptr);
  CHECK(s.dev(s.synth).metaAt("osc")->param("phase")->value == 0.25);
  s.edit(ops::removeMetaCall(s.doc(), s.synth, library().stockText("BasicSynth"), "osc"));
  CHECK(s.dev(s.synth).metaAt("osc") == nullptr);
  // A drag: many param writes inside one gesture, one history entry.
  s.addAccCall("acc1");
  const size_t before = s.store.history().cursor();
  REQUIRE(s.store.beginGesture().ok());
  for (int i = 1; i <= 20; ++i) {
    auto e = ops::setMetaParam(s.doc(), s.synth, "acc1", "phase_acc", "freq", 10.0 * i);
    REQUIRE(e.ok());
    REQUIRE(s.store.dispatch(e->command).ok());
    CHECK(s.store.lastChanges().paramOnly());
  }
  REQUIRE(s.store.commitGesture().ok());
  CHECK(s.store.history().cursor() == before + 1);
  CHECK(s.dev(s.synth).metaAt("acc1")->param("freq")->value == 200.0);
  REQUIRE(s.store.undo());
  CHECK(s.dev(s.synth).metaAt("acc1")->param("freq") == nullptr);
  // A fresh call (no state yet): its first drag adds the default state in the same step, still only a param write.
  s.edit(ops::removeMetaCall(s.doc(), s.synth, synthWithAcc("acc9"), "acc1"));
  REQUIRE(s.dev(s.synth).metaAt("acc9") == nullptr);
  auto first = ops::setMetaParam(s.doc(), s.synth, "acc9", "phase_acc", "freq", 5.0);
  REQUIRE(first.ok());
  REQUIRE(s.store.dispatch(first->command).ok());
  CHECK(s.store.lastChanges().paramOnly());
  CHECK(s.dev(s.synth).metaAt("acc9")->param("freq")->value == 5.0);
  // Undo every step: the stock synth again (the same snapshot).
  while (s.store.history().cursor() > freshCursor) REQUIRE(s.store.undo());
  CHECK(s.store.current() == fresh);
}

TEST_CASE("meta modules: golden lowering texts (a chain meta:phase_acc; a nested meta_phase_acc in a user device)") {
  Song s;
  s.addAccCall("acc1");
  s.edit(ops::setMetaParam(s.doc(), s.synth, "acc1", "phase_acc", "freq", 100.0));
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
  checkGolden("lower_chain_phase_acc.txt", textOf(s.accTrack, s.acc));
  checkGolden("lower_nested_phase_acc.txt", textOf(s.synthTrack, s.synth));
  // The chain device: an instrument expansion; its params are the device's own.
  CHECK(calleeOf(lp, s.accTrack, s.acc).starts_with("meta_phase_acc__d" + std::to_string(s.acc.value) + "__"));
  CHECK(paramOf(lp, "dev" + std::to_string(s.acc.value) + ":freq") != nullptr);
  // The nested instance: a specialised root, its params "dev<id>:acc1.<name>" at their values.
  CHECK(calleeOf(lp, s.synthTrack, s.synth).find("__m" + std::to_string(s.synth.value) + "_") != std::string::npos);
  const LoweredParam* f = paramOf(lp, metaParamKey(s.synth, "acc1", "freq"));
  REQUIRE(f != nullptr);
  CHECK(f->value == 100.0f);
  CHECK(f->name == "d" + std::to_string(s.synth.value) + "__acc1__freq");
  CHECK(f->ref == ParamRef::metaParam(s.synth, "acc1", "freq"));
  REQUIRE(paramOf(lp, metaParamKey(s.synth, "acc1", "amp")) != nullptr);
  CHECK(paramOf(lp, metaParamKey(s.synth, "acc1", "amp"))->value == 1.0f);
  // The param fast path lists the same params with the same values.
  const std::vector<LoweredParam> fast = lowerParams(s.doc(), true, library());
  for (const char* name : {"freq", "phase", "amp"}) {
    const std::string key = metaParamKey(s.synth, "acc1", name);
    const auto it = std::find_if(fast.begin(), fast.end(), [&](const LoweredParam& p) { return p.key == key; });
    REQUIRE(it != fast.end());
    CHECK(it->value == paramOf(lp, key)->value);
    CHECK(it->name == paramOf(lp, key)->name);
  }
}

TEST_CASE("meta modules: a freq drag lowers to the same IR (shared patch, same hash); settings and static-ness do not") {
  Song s;
  s.addAccCall("acc1");
  const LoweredProject a = lowered(s.doc());
  s.edit(ops::setMetaParam(s.doc(), s.synth, "acc1", "phase_acc", "freq", 440.0));
  CHECK(s.store.lastChanges().paramOnly());
  const LoweredProject b = lowered(s.doc(), nullptr, &a);
  const size_t ia = trackIndex(a, s.synthTrack), ib = trackIndex(b, s.synthTrack);
  CHECK(b.patches[ib] == a.patches[ia]);  // the same patch object: its unit does not recompile
  CHECK(b.patches[ib]->hash == a.patches[ia]->hash);
  CHECK(calleeOf(a, s.synthTrack, s.synth) == calleeOf(b, s.synthTrack, s.synth));
  CHECK(paramOf(b, metaParamKey(s.synth, "acc1", "freq"))->value == 440.0f);
  // A chain device's param: the same.
  s.edit(ops::setMetaParam(s.doc(), s.acc, "", "phase_acc", "freq", 7.0));
  const LoweredProject c = lowered(s.doc(), nullptr, &b);
  CHECK(c.patches[trackIndex(c, s.accTrack)] == b.patches[trackIndex(b, s.accTrack)]);
  CHECK(paramOf(c, "dev" + std::to_string(s.acc.value) + ":freq")->value == 7.0f);
}

TEST_CASE("meta modules: settings, wired pins and automation make other expansions; resources get their slots") {
  meta::testing::registerTestType();
  Song s;
  // A nested test module in the synth: its call `ts = meta_test_shape(…)` multiplies the oscillator.
  const std::string base = replaced(library().stockText("BasicSynth"), "  out = osc * env * vel * level",
                                    "  ts = meta_test_shape(osc)\n  out = ts * env * vel * level");
  s.edit(ops::addMetaCall(s.doc(), s.synth, base, "ts", "test_shape"));
  LowerOptions o;
  o.metaSlotOf = [](DeviceId d, std::string_view path, uint32_t index) {
    return static_cast<uint32_t>(d.value * 100 + path.size() * 10 + index + 1);
  };
  const LoweredProject a = lowered(s.doc(), &o);
  const std::string calleeA = calleeOf(a, s.synthTrack, s.synth);
  const std::vector<LoweredProject::MetaResource> ra = resourcesOf(a, "test_shape");
  REQUIRE(ra.size() == 1);
  const LoweredProject::MetaResource& r = ra[0];
  CHECK(r.device == s.synth);
  CHECK(r.path == "ts");
  CHECK(r.type == "test_shape");
  CHECK(r.track == s.synthTrack);
  CHECK(r.request.kind == meta::ResourceKind::FrameTable);
  CHECK(r.slot == s.synth.value * 100 + 20 + 1);
  REQUIRE(r.data != nullptr);
  CHECK(*r.data == *meta::testing::testDefaultData());
  // A setting: another expansion (a structural change).
  s.edit(ops::setMetaSetting(s.doc(), s.synth, "ts", "test_shape", "stages", 3.0));
  CHECK(s.store.lastChanges().structural());
  const LoweredProject b = lowered(s.doc(), &o, &a);
  CHECK(calleeOf(b, s.synthTrack, s.synth) != calleeA);
  CHECK(b.patches[trackIndex(b, s.synthTrack)] != a.patches[trackIndex(a, s.synthTrack)]);
  // Data: never part of the expansion (MetaData only; the IR stays shared).
  s.edit(ops::setMetaData(s.doc(), s.synth, "ts", "test_shape",
                          std::make_shared<const meta::DataValue>(meta::DataValue::record({{"values", meta::DataValue::floatArray({0.5f})}}))));
  CHECK(s.store.lastChanges().metaDataOnly());
  const LoweredProject c = lowered(s.doc(), &o, &b);
  CHECK(c.patches[trackIndex(c, s.synthTrack)] == b.patches[trackIndex(b, s.synthTrack)]);
  REQUIRE(resourcesOf(c, "test_shape").size() == 1);
  CHECK(resourcesOf(c, "test_shape")[0].data->field("values")->floats == std::vector<float>{0.5f});
  // A wired param pin is not static: `meta_test_shape(osc, gain: …)` expands to the modulated (clipped) form, and the
  // wired param is no runtime param.
  const std::string wired = replaced(base, "meta_test_shape(osc)", "meta_test_shape(osc, gain: env)");
  s.edit(ops::editDevicePatch(s.doc(), s.synth, wired));
  const LoweredProject d = lowered(s.doc(), &o);
  CHECK(paramOf(d, metaParamKey(s.synth, "ts", "gain")) == nullptr);
  CHECK(paramOf(c, metaParamKey(s.synth, "ts", "gain")) != nullptr);
  const pq::graph::Patch* exp = nullptr;
  for (const std::string& n : {calleeOf(d, s.synthTrack, s.synth)}) {
    const pq::graph::Patch* root = d.graph.library->find(n);
    REQUIRE(root != nullptr);
    const auto call = root->findByName("ts");
    REQUIRE(call.has_value());
    exp = d.graph.library->find(root->subcircuit(*call).patch);
  }
  REQUIRE(exp != nullptr);
  bool clipped = false;
  for (pq::graph::NodeIndex n = 0; n < exp->nodeCount(); ++n)
    clipped = clipped || (exp->node(n).kind == pq::graph::NodeKind::Atom &&
                          pq::graph::atomSignature(exp->node(n).atom).name == "clip");
  CHECK(clipped);
}

TEST_CASE("meta modules: a chain device's static-ness follows its automation lanes") {
  meta::testing::registerTestType();
  Song s;
  // A chain test module as an audio effect after the synth.
  const DeviceId ts = s.edit(ops::addMetaDevice(s.doc(), s.store.ids(), s.synthTrack, "test_shape")).devices.at(0);
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

TEST_CASE("meta modules: renaming a nested instance keeps its state (the patch editor's rename and a hand edit)") {
  Song s;
  s.addAccCall("acc1");
  s.edit(ops::setMetaParam(s.doc(), s.synth, "acc1", "phase_acc", "freq", 55.0));
  // A hand edit of the text pane: the binding renamed; the new parse carries the NodeIds of the previous one.
  pq::graph::ParseOptions po;
  po.externals = library().signatures();
  po.lenientLayout = true;
  po.deferMissingInputs = true;
  const std::string t1 = synthWithAcc("acc1"), t2 = synthWithAcc("lfo");
  pq::graph::ParseResult p1 = pq::graph::parseDetailed(t1, "basic_synth.pqp", po);
  REQUIRE(p1.lowered);
  po.previous = &p1.file;
  auto p2 = std::make_shared<pq::graph::ParseResult>(pq::graph::parseDetailed(t2, "basic_synth.pqp", po));
  REQUIRE(p2->lowered);
  const std::vector<MetaRename> renames = metaRenames(p1.file, p2->file, "BasicSynth");
  REQUIRE(renames.size() == 1);
  CHECK(renames[0] == MetaRename{"acc1", "lfo"});
  const auto ids = std::shared_ptr<const pq::graph::PatchFile>(p2, &p2->file);
  s.edit(ops::editDevicePatchKeepingMeta(s.doc(), s.synth, t2, ids, p1.file));
  CHECK(s.dev(s.synth).metaAt("acc1") == nullptr);
  REQUIRE(s.dev(s.synth).metaAt("lfo") != nullptr);
  CHECK(s.dev(s.synth).metaAt("lfo")->param("freq")->value == 55.0);
  const LoweredProject lp = lowered(s.doc());
  REQUIRE(paramOf(lp, metaParamKey(s.synth, "lfo", "freq")) != nullptr);
  CHECK(paramOf(lp, metaParamKey(s.synth, "lfo", "freq"))->value == 55.0f);
  // Nested renames: an outer call renamed moves everything below it.
  Song n;
  n.edit(ops::setMetaParam(n.doc(), n.synth, "opA.acc", "phase_acc", "amp", 3.0));
  n.edit(ops::setMetaParam(n.doc(), n.synth, "opA.ts", "test_shape", "gain", 1.0));
  REQUIRE(n.store.dispatch(RenameMetaInstance{n.synth, "opA", "opB"}).ok());
  CHECK(n.dev(n.synth).metaAt("opB.acc")->param("amp")->value == 3.0);
  CHECK(n.dev(n.synth).metaAt("opB.ts") != nullptr);
  CHECK(n.dev(n.synth).metaAt("opA.acc") == nullptr);
  CHECK_FALSE(n.store.dispatch(RenameMetaInstance{n.synth, "opB", "opB.x"}).ok());  // into itself
}

TEST_CASE("meta modules: commands refuse what the state cannot hold") {
  Song s;
  auto fails = [&](const Command& c) {
    const Snapshot before = s.store.current();
    CHECK_FALSE(s.store.dispatch(c).ok());
    CHECK(s.store.current() == before);
  };
  MetaInstance acc = defaultMetaInstance(meta::phaseAccType());
  fails(AddMetaInstance{s.synth, "", acc});  // "" only on a meta device
  fails(AddMetaInstance{s.synth, "a..b", acc});
  MetaInstance badParam = acc;
  badParam.params = {DeviceValue{"nope", 1.0}};
  fails(AddMetaInstance{s.synth, "acc1", badParam});
  MetaInstance badVersion = acc;
  badVersion.version = 9;
  fails(AddMetaInstance{s.synth, "acc1", badVersion});
  MetaInstance unknown = acc;
  unknown.type = "nope";
  fails(AddMetaInstance{s.synth, "acc1", unknown});
  fails(SetMetaParam{s.synth, "acc1", "freq", 1.0});  // no state there yet (ops::setMetaParam adds it)
  REQUIRE(s.store.dispatch(AddMetaInstance{s.synth, "acc1", acc}).ok());
  fails(AddMetaInstance{s.synth, "acc1", acc});  // twice
  fails(SetMetaParam{s.synth, "acc1", "nope", 1.0});
  fails(SetMetaSetting{s.synth, "acc1", "nope", 1.0});
  fails(SetMetaData{s.synth, "acc1", std::make_shared<const meta::DataValue>(meta::DataValue::num(1.0))});  // keeps none
  fails(SetMetaParam{s.acc, "", "nope", 1.0});
  // A Phase accumulator is an instrument only: no audio-effect form.
  CHECK_FALSE(ops::addMetaDevice(s.doc(), s.store.ids(), s.synthTrack, "nope").ok());
}
