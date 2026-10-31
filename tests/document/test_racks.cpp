// T4.8: racks in the document — the rules the commands keep (chain kinds, mappings inside their rack, nesting), the
// edit builders (group, chains, mappings, variations; one undo step each), the project file (schema 6) and the lowering
// of racks (inline chains with stable ids, a macro = one runtime param, mapped params replaced, chain params derived).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/serialize.h"
#include "pq/core/alloc_trap.h"
#include "pq/document/store.h"

using namespace pq::doc;
using pq::Result;

namespace {

std::string stdlibDir() { return std::string(PQ_TEST_DATA_DIR) + "/../stdlib"; }
std::string fixturePath(const std::string& rel) { return std::string(PQ_TEST_DATA_DIR) + "/document/fixtures/" + rel; }
std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(stdlibDir());
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// A store with one MIDI track (default instrument) and one return.
struct Racks {
  DocumentStore store;
  TrackId midi, ret;
  DeviceId inst;
  Racks() {
    midi = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    ret = edit(ops::createTrack(doc(), store.ids(), TrackKind::Return)).tracks.at(0);
    inst = track(midi).devices.at(0)->id;
  }
  const Project& doc() const { return *store.current(); }
  const Track& track(TrackId id) const { return *findTrackPtr(doc(), id); }
  const Device& device(DeviceId id) const { return *findDevice(doc(), id).device; }
  const Rack& rack(DeviceId id) const { return *device(id).rack; }
  // Dispatches a builder's edit: exactly one history entry (or none for an empty batch).
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    const pq::Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    CHECK(store.history().cursor() == before + (e->empty() ? 0u : 1u));
    return *e;
  }
  DeviceId add(TrackId t, const char* patch, DeviceKind k = DeviceKind::AudioEffect, ChainId chain = {}) {
    return edit(ops::addDevice(doc(), store.ids(), t, patch, k, kAppend, chain)).devices.at(0);
  }
};

pq::Status dispatch(DocumentStore& s, Command c) { return s.dispatch(std::move(c)); }

}  // namespace

TEST_CASE("racks: the rules — chain kinds, nesting, mappings inside their rack, one mapping per param") {
  Racks r;
  IdSource& ids = r.store.ids();
  const DeviceId fx = r.add(r.midi, "utility");
  const DeviceId aer = r.add(r.midi, std::string(kAudioEffectRack).c_str());
  CHECK(r.device(aer).isRack());
  CHECK(r.rack(aer).chains.empty());
  const ChainId c1 = ids.chain();
  REQUIRE(dispatch(r.store, AddChain{aer, makeChain(c1, "A")}).ok());
  SUBCASE("an Audio Effect Rack's chains hold no instrument") {
    CHECK_FALSE(dispatch(r.store, AddDevice{{}, makeDefaultInstrument(ids.device()), 0, c1}).ok());
    CHECK_FALSE(ops::addDevice(r.doc(), ids, {}, "BasicSynth", DeviceKind::Instrument, kAppend, c1).ok());
  }
  SUBCASE("an Instrument Rack goes where instruments go, its chains start with one") {
    CHECK_FALSE(dispatch(r.store, AddDevice{r.ret, makeRack(ids.device(), DeviceKind::Instrument), 0}).ok());
    const DeviceId ir = r.edit(ops::addDevice(r.doc(), ids, r.midi, std::string(kInstrumentRack), DeviceKind::Instrument)).devices[0];
    CHECK(r.track(r.midi).devices.front()->id == ir);  // it replaced the instrument
    const ChainId ic = ids.chain();
    REQUIRE(dispatch(r.store, AddChain{ir, makeChain(ic, "Keys")}).ok());
    const DeviceId syn = r.add({}, "BasicSynth", DeviceKind::Instrument, ic);
    const DeviceId sat = r.add({}, "saturator", DeviceKind::AudioEffect, ic);
    CHECK(findDevice(r.doc(), syn).index == 0);
    CHECK(findDevice(r.doc(), sat).index == 1);
    // A second instrument in that chain replaces the first (the builder), a raw command after effects is refused.
    CHECK_FALSE(dispatch(r.store, AddDevice{{}, makeDefaultInstrument(ids.device()), kAppend, ic}).ok());
    const DeviceId syn2 = r.add({}, "BasicSynth", DeviceKind::Instrument, ic);
    CHECK_FALSE(findDevice(r.doc(), syn).device);
    CHECK(findDevice(r.doc(), syn2).index == 0);
    // Key / velocity zones only in Instrument Racks.
    CHECK(dispatch(r.store, SetChainZone{ic, ZoneKind::Velocity, Zone{0, 0, 64, 64}}).ok());
    CHECK_FALSE(dispatch(r.store, SetChainZone{c1, ZoneKind::Velocity, Zone{0, 0, 64, 64}}).ok());
    CHECK_FALSE(dispatch(r.store, SetChainZone{c1, ZoneKind::Select, Zone{0, 50, 40, 127}}).ok());  // out of order
  }
  SUBCASE("racks nest; a rack never goes into its own chain") {
    const DeviceId inner = r.add({}, std::string(kAudioEffectRack).c_str(), DeviceKind::AudioEffect, c1);
    const ChainId c2 = ids.chain();
    REQUIRE(dispatch(r.store, AddChain{inner, makeChain(c2, "Inner")}).ok());
    const DeviceId deep = r.add({}, "saturator", DeviceKind::AudioEffect, c2);
    const DeviceLoc loc = findDevice(r.doc(), deep);
    CHECK(loc.track == &r.track(r.midi));
    CHECK(loc.rack->id == inner);
    CHECK(deviceContains(r.device(aer), deep));
    CHECK_FALSE(dispatch(r.store, MoveDevice{aer, {}, kAppend, c2}).ok());
    CHECK_FALSE(dispatch(r.store, MoveDevice{aer, {}, kAppend, c1}).ok());
    // A deep device can be edited (path-copied), and the edit is one Param change.
    const Snapshot before = r.store.current();
    REQUIRE(r.store.dispatch(SetDeviceParam{deep, "drive", 0.5}).ok());
    CHECK(r.store.lastChanges().paramOnly());
    CHECK(r.device(deep).param("drive")->value == 0.5);
    CHECK(findTrackPtr(*before, r.midi)->devices[0] == r.track(r.midi).devices[0]);  // the instrument is shared
    CHECK(maxEntityId(r.doc()) >= deep.value);
  }
  SUBCASE("mappings: inside the rack only, one per param, the rack's own selector allowed") {
    const DeviceId u = r.add({}, "utility", DeviceKind::AudioEffect, c1);
    MacroMapping m{ids.mapping(), 0, u, "width", 0.0, 1.0, 0.0};
    CHECK(dispatch(r.store, AddMacroMapping{aer, m}).ok());
    MacroMapping twice{ids.mapping(), 1, u, "width", 0.0, 1.0, 0.0};
    CHECK_FALSE(dispatch(r.store, AddMacroMapping{aer, twice}).ok());
    MacroMapping outside{ids.mapping(), 1, fx, "width", 0.0, 1.0, 0.0};
    CHECK_FALSE(dispatch(r.store, AddMacroMapping{aer, outside}).ok());
    MacroMapping self{ids.mapping(), 1, aer, "select", 0.0, 1.0, 0.0};
    CHECK(dispatch(r.store, AddMacroMapping{aer, self}).ok());
    MacroMapping selfMacro{ids.mapping(), 1, aer, "macro2", 0.0, 1.0, 0.0};
    CHECK_FALSE(dispatch(r.store, AddMacroMapping{aer, selfMacro}).ok());
    MacroMapping bad{ids.mapping(), 16, u, "gain", 0.0, 1.0, 0.0};
    CHECK_FALSE(dispatch(r.store, AddMacroMapping{aer, bad}).ok());
    // Rack params: macros and the selector only, 0 … 127.
    CHECK(dispatch(r.store, SetDeviceParam{aer, "macro16", 127.0}).ok());
    CHECK_FALSE(dispatch(r.store, SetDeviceParam{aer, "macro17", 1.0}).ok());
    CHECK_FALSE(dispatch(r.store, SetDeviceParam{aer, "select", 128.0}).ok());
    CHECK_FALSE(dispatch(r.store, SetDeviceSetting{aer, "x", 1.0}).ok());
    // Moving the mapped device to the track takes its mapping away (in the same step); undo restores both.
    REQUIRE(r.store.dispatch(MoveDevice{u, r.midi, kAppend}).ok());
    CHECK(r.rack(aer).mappings.size() == 1);  // the selector's stays
    REQUIRE(r.store.undo());
    CHECK(r.rack(aer).mappings.size() == 2);
  }
}

TEST_CASE("racks: group, chains, mappings, variations — one undo step each") {
  Racks r;
  IdSource& ids = r.store.ids();
  const DeviceId lfo = r.add(r.midi, "lfo_device");
  const DeviceId sat = r.add(r.midi, "saturator");
  const DeviceId util = r.add(r.midi, "utility");
  const WireId w = r.edit(ops::connectWire(r.doc(), ids, lfo, "lfo", sat, "drive", 0.25)).wires.at(0);
  SUBCASE("Ctrl+G keeps ids, wires and order; the instrument makes an Instrument Rack") {
    const ops::Edit e = r.edit(ops::groupDevices(r.doc(), ids, std::vector<DeviceId>{util, sat}));
    const DeviceId rack = e.devices.at(0);
    const Track& t = r.track(r.midi);
    REQUIRE(t.devices.size() == 3);
    CHECK(t.devices[2]->id == rack);
    CHECK(r.device(rack).kind == DeviceKind::AudioEffect);
    const Chain& c = *r.rack(rack).chains.at(0);
    REQUIRE(c.devices.size() == 2);
    CHECK(c.devices[0]->id == sat);  // list order, not selection order
    CHECK(c.devices[1]->id == util);
    REQUIRE(findWire(r.doc(), w) != nullptr);  // the wire survived (re-added with its id)
    CHECK(findWire(r.doc(), w)->to == sat);
    const ops::Edit g2 = r.edit(ops::groupDevices(r.doc(), ids, std::vector<DeviceId>{r.inst}));
    CHECK(r.device(g2.devices[0]).kind == DeviceKind::Instrument);
    CHECK(r.track(r.midi).devices[0]->id == g2.devices[0]);
    REQUIRE(r.store.undo());
    REQUIRE(r.store.undo());
    CHECK(r.track(r.midi).devices.size() == 4);
  }
  SUBCASE("chains: add, duplicate (fresh ids, mappings follow), solo exclusive, mixer, zones, delete") {
    const DeviceId rack = r.edit(ops::groupDevices(r.doc(), ids, std::vector<DeviceId>{sat})).devices.at(0);
    const ChainId c1 = r.rack(rack).chains[0]->id;
    r.edit(ops::mapMacro(r.doc(), ids, rack, 2, sat, "drive", 100.0));
    CHECK(rackParamValue(r.device(rack), "macro3") == 100.0);
    r.edit(ops::duplicateChain(r.doc(), ids, c1));
    REQUIRE(r.rack(rack).chains.size() == 2);
    const Chain& copy = *r.rack(rack).chains[1];
    CHECK(copy.id != c1);
    CHECK(copy.devices[0]->id != sat);
    CHECK(copy.devices[0]->patch == "saturator");
    CHECK(r.rack(rack).mappings.size() == 1);  // the copy's device is not mapped (the mapping lives on the rack)
    const ChainId c2 = copy.id;
    r.edit(ops::setChainSolo(r.doc(), c1, true));
    r.edit(ops::setChainSolo(r.doc(), c2, true));  // exclusive: c1 off
    CHECK_FALSE(r.rack(rack).chains[0]->solo);
    CHECK(r.rack(rack).chains[1]->solo);
    r.edit(ops::setChainMixer(r.doc(), c2, -100.0f, 2.0f));  // clamped
    CHECK(r.rack(rack).chains[1]->volumeDb == kMinVolumeDb);
    CHECK(r.rack(rack).chains[1]->pan == 1.0f);
    r.edit(ops::setChainZone(r.doc(), c2, ZoneKind::Select, Zone{0, 10, 60, 70}));
    CHECK(r.rack(rack).chains[1]->select.fadeHi == 60.0);
    CHECK_FALSE(ops::setChainZone(r.doc(), c2, ZoneKind::Select, Zone{0, 10, 60, 200}).ok());
    r.edit(ops::addChain(r.doc(), ids, rack));
    CHECK(r.rack(rack).chains.back()->name == "Chain 2");  // "Chain 1" is taken
    r.edit(ops::moveChain(r.doc(), r.rack(rack).chains.back()->id, 0));
    CHECK(r.rack(rack).chains[0]->name == "Chain 2");
    r.edit(ops::deleteChains(r.doc(), std::vector<ChainId>{c1}));
    CHECK(r.rack(rack).mappings.empty());  // it mapped the deleted chain's saturator
    CHECK_FALSE(findWire(r.doc(), w));     // and the wire into it went too
  }
  SUBCASE("mapping: map moves a param between macros, unmap, set range") {
    const DeviceId rack = r.edit(ops::groupDevices(r.doc(), ids, std::vector<DeviceId>{sat, util})).devices.at(0);
    r.edit(ops::mapMacro(r.doc(), ids, rack, 0, util, "gain"));
    r.edit(ops::mapMacro(r.doc(), ids, rack, 5, util, "gain"));
    REQUIRE(r.rack(rack).mappings.size() == 1);
    CHECK(r.rack(rack).mappings[0].macro == 5);
    const MappingId m = r.rack(rack).mappings[0].id;
    r.edit(ops::setMacroMapping(r.doc(), m, 0.8, 0.2, -2.0));
    CHECK(findMapping(r.doc(), m).mapping->min == 0.8);
    CHECK(findMapping(r.doc(), m).mapping->curve == -1.0);
    CHECK_FALSE(ops::mapMacro(r.doc(), ids, rack, 0, lfo, "rate").ok());  // not inside the rack
    r.edit(ops::unmapParam(r.doc(), util, "gain"));
    CHECK(r.rack(rack).mappings.empty());
  }
  SUBCASE("variations: store, recall (one step), randomize leaves excluded macros, delete") {
    const DeviceId rack = r.edit(ops::groupDevices(r.doc(), ids, std::vector<DeviceId>{sat})).devices.at(0);
    r.edit(ops::setMacroValue(r.doc(), rack, 0, 10.0));
    r.edit(ops::setMacroValue(r.doc(), rack, 1, 20.0));
    r.edit(ops::storeVariation(r.doc(), rack));
    REQUIRE(r.rack(rack).variations.size() == 1);
    CHECK(r.rack(rack).variations[0].name == "Variation 1");
    CHECK(r.rack(rack).variations[0].values[1] == 20.0);
    r.edit(ops::setMacroValue(r.doc(), rack, 0, 99.0));
    r.edit(ops::setMacroValue(r.doc(), rack, 1, 98.0));
    const size_t before = r.store.history().cursor();
    r.edit(ops::recallVariation(r.doc(), rack, 0));
    CHECK(r.store.history().cursor() == before + 1);
    CHECK(r.store.lastChanges().paramOnly());
    CHECK(rackParamValue(r.device(rack), "macro1") == 10.0);
    CHECK(rackParamValue(r.device(rack), "macro2") == 20.0);
    r.edit(ops::setMacroExcluded(r.doc(), rack, 0, true));
    r.edit(ops::randomizeMacros(r.doc(), rack, 12345));
    CHECK(rackParamValue(r.device(rack), "macro1") == 10.0);  // excluded
    bool moved = false;
    for (uint32_t k = 1; k < 8; ++k) {
      const double v = rackParamValue(r.device(rack), macroParamName(k));
      CHECK(v >= 0.0);
      CHECK(v <= kMacroMax);
      moved = moved || v != (k == 1 ? 20.0 : 0.0);
    }
    CHECK(moved);
    // Reproducible from the seed.
    REQUIRE(r.store.undo());
    const Snapshot a = r.store.current();
    r.edit(ops::randomizeMacros(r.doc(), rack, 12345));
    const double first = rackParamValue(r.device(rack), "macro2");
    REQUIRE(r.store.undo());
    r.edit(ops::randomizeMacros(r.doc(), rack, 12345));
    CHECK(rackParamValue(r.device(rack), "macro2") == first);
    r.edit(ops::deleteVariation(r.doc(), rack, 0));
    CHECK(r.rack(rack).variations.empty());
  }
  SUBCASE("copy / paste a rack: everything gets fresh ids, its mappings follow the copies") {
    const DeviceId rack = r.edit(ops::groupDevices(r.doc(), ids, std::vector<DeviceId>{sat, util})).devices.at(0);
    r.edit(ops::mapMacro(r.doc(), ids, rack, 0, util, "gain"));
    const DevicePtr src = findTrackPtr(r.doc(), r.midi)->devices.back();
    const ops::Edit e = r.edit(ops::pasteDevices(r.doc(), ids, r.ret, std::vector<DevicePtr>{src}));
    const Device& copy = r.device(e.devices.at(0));
    REQUIRE(copy.isRack());
    CHECK(copy.rack->chains[0]->id != r.rack(rack).chains[0]->id);
    const DeviceId copiedUtil = copy.rack->chains[0]->devices[1]->id;
    CHECK(copiedUtil != util);
    REQUIRE(copy.rack->mappings.size() == 1);
    CHECK(copy.rack->mappings[0].device == copiedUtil);
    CHECK(copy.rack->mappings[0].id != r.rack(rack).mappings[0].id);
    // Deleting a rack and a device inside it at once is one step (the inner one goes with the rack).
    r.edit(ops::deleteDevices(r.doc(), std::vector<DeviceId>{copiedUtil, copy.id}));
    CHECK(r.track(r.ret).devices.empty());
  }
}

TEST_CASE("racks: the project file (schema 6) round-trips racks; broken racks are refused; schemas 4 and 5 load") {
  Racks r;
  IdSource& ids = r.store.ids();
  const DeviceId sat = r.add(r.midi, "saturator");
  const DeviceId util = r.add(r.midi, "utility");
  const DeviceId rack = r.edit(ops::groupDevices(r.doc(), ids, std::vector<DeviceId>{sat, util})).devices.at(0);
  const ChainId c1 = r.rack(rack).chains[0]->id;
  r.edit(ops::addChain(r.doc(), ids, rack));
  const ChainId c2 = r.rack(rack).chains[1]->id;
  const DeviceId inner = r.add({}, std::string(kAudioEffectRack).c_str(), DeviceKind::AudioEffect, c2);
  r.edit(ops::addChain(r.doc(), ids, inner, kAppend, "Deep"));
  r.edit(ops::mapMacro(r.doc(), ids, rack, 3, util, "gain"));
  r.edit(ops::mapMacro(r.doc(), ids, rack, 4, inner, "macro2"));
  r.edit(ops::setMacroMapping(r.doc(), r.rack(rack).mappings[0].id, 0.25, 0.75, 0.5));
  r.edit(ops::setChainMixer(r.doc(), c1, -3.5f, -0.25f));
  r.edit(ops::setChainActive(r.doc(), c2, false));
  r.edit(ops::setChainZone(r.doc(), c1, ZoneKind::Select, Zone{0, 0, 63, 64}));
  r.edit(ops::renameMacro(r.doc(), rack, 3, "Gain"));
  r.edit(ops::setMacroValue(r.doc(), rack, 3, 77.0));
  r.edit(ops::storeVariation(r.doc(), rack, "Loud"));
  r.edit(ops::setMacroExcluded(r.doc(), rack, 2, true));
  r.edit(ops::setVisibleMacros(r.doc(), rack, 16));
  r.edit(ops::setRackShow(r.doc(), rack, false, true));
  const DeviceId ir = r.edit(ops::groupDevices(r.doc(), ids, std::vector<DeviceId>{r.inst})).devices.at(0);
  r.edit(ops::setKeyZoneUnit(r.doc(), ir, KeyZoneUnit::Degree));
  r.edit(ops::setVoiceChains(r.doc(), ir, true));
  r.edit(ops::setChainZone(r.doc(), r.rack(ir).chains[0]->id, ZoneKind::Key, Zone{-12, -10, 10, 12}));
  r.edit(ops::setChainZone(r.doc(), r.rack(ir).chains[0]->id, ZoneKind::Velocity, Zone{1, 20, 100, 127}));

  Result<std::string> text = serializeProject(r.doc(), r.store.view());
  REQUIRE(text.ok());
  CHECK(text->rfind("{\n  \"schemaVersion\": " + std::to_string(kSchemaVersion) + ",", 0) == 0);
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == r.doc());
  Result<std::string> again = serializeProject(*back->project, back->view);
  REQUIRE(again.ok());
  CHECK(*again == *text);  // the writer is stable
  CHECK(text->find("\"keyUnit\": \"degree\"") != std::string::npos);
  CHECK(text->find("\"select\": [0.0, 0.0, 63.0, 64.0]") != std::string::npos);

  auto broken = [&](const std::string& from, const std::string& to) {
    std::string t = *text;
    const size_t at = t.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, from);
    t.replace(at, from.size(), to);
    return parseProject(t);
  };
  CHECK_FALSE(broken("\"select\": [0.0, 0.0, 63.0, 64.0]", "\"select\": [0.0, 70.0, 63.0, 64.0]").ok());
  CHECK_FALSE(broken("\"select\": [0.0, 0.0, 63.0, 64.0]", "\"select\": [0.0, 63.0, 64.0]").ok());
  CHECK_FALSE(broken("\"macro\": 4", "\"macro\": 17").ok());
  CHECK_FALSE(broken("\"device\": " + std::to_string(util.value), "\"device\": " + std::to_string(r.inst.value)).ok());
  CHECK_FALSE(broken("\"keyUnit\": \"degree\"", "\"keyUnit\": \"mel\"").ok());
  CHECK_FALSE(broken("\"patch\": \"AudioEffectRack\"", "\"patch\": \"InstrumentRack\"").ok());
  CHECK_FALSE(broken("\"id\": " + std::to_string(c2.value), "\"id\": " + std::to_string(c1.value)).ok());  // duplicate

  // fixtures/schema4 / schema5 / schema6 are fixtures/full as the schema-4 build (main at 9619d7a), the schema-5 build
  // (main at d2daf37: user devices) and the schema-6 build (main at 70ce131: racks) wrote it: schemas 6 and 7 only add
  // racks and audio (T5.3), so all three re-save as fixtures/full.
  for (const int32_t v : {4, 5, 6}) {
    CAPTURE(v);
    const std::string dir = "schema" + std::to_string(v);
    const std::string oldText = readFile(fixturePath(dir + "/project.pqd"));
    REQUIRE(oldText.rfind("{\n  \"schemaVersion\": " + std::to_string(v) + ",", 0) == 0);
    Result<LoadedProject> old = loadProject(fixturePath(dir));
    REQUIRE_MESSAGE(old.ok(), (old.ok() ? std::string() : old.error().message));
    CHECK(old->fileSchemaVersion == v);
    Result<std::string> resaved = serializeProject(*old->project, old->view);
    REQUIRE(resaved.ok());
    CHECK(*resaved == readFile(fixturePath("full/project.pqd")));
  }
  // A rack carries no patch text.
  CHECK_FALSE(broken("\"patch\": \"AudioEffectRack\"", "\"source\": [\"x\"], \"patch\": \"AudioEffectRack\"").ok());
}

TEST_CASE("racks: lowering — inline chains with stable ids, one runtime param per macro, mapped params replaced") {
  Racks r;
  IdSource& ids = r.store.ids();
  const DeviceId sat = r.add(r.midi, "saturator");
  const DeviceId util = r.add(r.midi, "utility");
  const DeviceId rack = r.edit(ops::groupDevices(r.doc(), ids, std::vector<DeviceId>{sat, util})).devices.at(0);
  const ChainId c1 = r.rack(rack).chains[0]->id;
  r.edit(ops::addChain(r.doc(), ids, rack));
  const ChainId c2 = r.rack(rack).chains[1]->id;
  const DeviceId util2 = r.add({}, "utility", DeviceKind::AudioEffect, c2);
  // One macro drives 8 targets: utility gain, width, balance, ms, …
  for (const char* p : {"gain", "width", "balance", "ms"}) r.edit(ops::mapMacro(r.doc(), ids, rack, 0, util, p));
  for (const char* p : {"gain", "width", "balance", "ms"}) r.edit(ops::mapMacro(r.doc(), ids, rack, 0, util2, p));
  LowerOptions o;
  o.library = &library().library();
  o.slotOf = [](TrackId) { return 0u; };
  Result<LoweredProject> lp = lower(r.doc(), o);
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  uint32_t ti = 0;
  while (lp->trackIds[ti] != r.midi) ++ti;
  const pq::graph::Patch& patch = lp->patches[ti]->patch;
  auto has = [&](uint32_t id) { return patch.indexOf(pq::graph::NodeId{id}).has_value(); };
  CHECK(has(nodeIdFor(NodeRole::Device, sat.value, 0)));
  CHECK(has(nodeIdFor(NodeRole::Device, util.value, 0)));
  CHECK(has(nodeIdFor(NodeRole::Device, util2.value, 0)));
  CHECK(has(nodeIdFor(NodeRole::Rack, rack.value, 0)));  // macro 1's param
  CHECK_FALSE(has(nodeIdFor(NodeRole::Rack, rack.value, 1)));  // macro 2: read by nothing, no param
  CHECK(has(nodeIdFor(NodeRole::Chain, c1.value, 5)));  // c1's mixer gain stage
  CHECK(has(nodeIdFor(NodeRole::Chain, c2.value, 6)));  // c2 summed with c1
  // The macro is one linked param (dev<rack>:macro1); the 8 mapped params are not params any more.
  auto keyed = [&](const std::string& k) {
    return std::count_if(lp->params.begin(), lp->params.end(), [&](const LoweredParam& p) { return p.key == k; });
  };
  CHECK(keyed("dev" + std::to_string(rack.value) + ":macro1") == 1);
  CHECK(keyed("dev" + std::to_string(rack.value) + ":macro2") == 0);
  CHECK(keyed("dev" + std::to_string(util.value) + ":gain") == 0);
  CHECK(keyed("dev" + std::to_string(util2.value) + ":width") == 0);
  CHECK(keyed("dev" + std::to_string(util.value) + ":bassFreq") == 1);  // unmapped: still a param
  CHECK(keyed("chain" + std::to_string(c1.value) + ":gl") == 1);
  CHECK(keyed("chain" + std::to_string(c1.value) + ":s0") == 0);  // no select zone: no zone params
  // lowerParams agrees with lower() (the link's param fast path relies on it).
  const std::vector<LoweredParam> fast = lowerParams(r.doc(), false, &library().library());
  CHECK(fast.size() == lp->params.size());
  for (size_t i = 0; i < std::min(fast.size(), lp->params.size()); ++i) {
    CHECK(fast[i].key == lp->params[i].key);
    CHECK(fast[i].name == lp->params[i].name);
    CHECK(fast[i].value == lp->params[i].value);
  }
  // The rack's circuit view (rackSubcircuit) is built by the same code: every entity node of it (devices, chains,
  // mappings, the rack's params) is a node of the track patch with the same NodeId.
  Result<pq::graph::Patch> sub = rackSubcircuit(r.doc(), rack, library().library());
  REQUIRE_MESSAGE(sub.ok(), (sub.ok() ? std::string() : sub.error().message));
  uint32_t entityNodes = 0;
  for (pq::graph::NodeIndex n = 0; n < sub->nodeCount(); ++n) {
    const uint32_t id = sub->node(n).id.value;
    if (id < kEntityNodeBase) continue;
    ++entityNodes;
    CHECK_MESSAGE(has(id), "node " << id << " of the rack's subcircuit is not in the track patch");
  }
  CHECK(entityNodes > 20);
  // A macro turn changes no IR (no compile): the same patch object is shared.
  r.edit(ops::setMacroValue(r.doc(), rack, 0, 90.0));
  Result<LoweredProject> lp2 = lower(r.doc(), o, &*lp);
  REQUIRE(lp2.ok());
  CHECK(lp2->patches[ti] == lp->patches[ti]);
  // A select zone switched on adds its params; moving it is a param write (the same IR again).
  r.edit(ops::setChainZone(r.doc(), c2, ZoneKind::Select, Zone{0, 0, 60, 70}));
  Result<LoweredProject> lp3 = lower(r.doc(), o, &*lp2);
  REQUIRE(lp3.ok());
  CHECK(lp3->patches[ti] != lp2->patches[ti]);
  r.edit(ops::setChainZone(r.doc(), c2, ZoneKind::Select, Zone{0, 10, 50, 90}));
  CHECK(r.store.lastChanges().paramOnly());
  Result<LoweredProject> lp4 = lower(r.doc(), o, &*lp3);
  REQUIRE(lp4.ok());
  CHECK(lp4->patches[ti] == lp3->patches[ti]);
  const std::vector<LoweredParam> zone = chainParamValues(r.doc(), c2);
  CHECK(std::any_of(zone.begin(), zone.end(), [&](const LoweredParam& p) { return p.key == "chain" + std::to_string(c2.value) + ":s1"; }));
  CHECK(keyed("dev" + std::to_string(rack.value) + ":select") == 0);
  const std::vector<LoweredParam> after = lowerParams(r.doc(), false, &library().library());
  CHECK(std::count_if(after.begin(), after.end(), [&](const LoweredParam& p) {
          return p.key == "dev" + std::to_string(rack.value) + ":select";
        }) == 1);
  // A muted chain is left out (structural, zero cost): its devices have no node.
  r.edit(ops::setChainActive(r.doc(), c2, false));
  Result<LoweredProject> lp5 = lower(r.doc(), o, &*lp4);
  REQUIRE(lp5.ok());
  CHECK_FALSE(lp5->patches[ti]->patch.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Device, util2.value, 0)}).has_value());
  CHECK(lp5->patches[ti]->patch.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Device, sat.value, 0)}).has_value());
  // T4.12-fix1 with racks: an Instrument Rack's track takes the largest `voices` setting of its chains' instruments.
  const DeviceId ir = r.edit(ops::groupDevices(r.doc(), ids, std::vector<DeviceId>{r.inst})).devices.at(0);
  const DeviceId op = r.edit(ops::addChainWithDevice(r.doc(), ids, ir, "operator", DeviceKind::Instrument)).devices.at(0);
  r.edit(ops::setDeviceSetting(r.doc(), op, "voices", 12.0));
  Result<LoweredProject> lp6 = lower(r.doc(), o);
  REQUIRE(lp6.ok());
  CHECK(lp6->graph.tracks[ti].role == pq::graph::TrackRole::Instrument);
  CHECK(lp6->graph.tracks[ti].maxVoices == 12);
}

// M4 review H1: MoveDevice took `Track& ft = mutableTrack(from)`, then `mutableTrack(to)`. With `to` a child of the
// group `from`, the second call re-copied the group and freed the object `ft` referred to; pruneMappings(ft.devices)
// then ran on freed memory and the rack kept its mapping onto the moved device (a file checkRack refuses on load).
// Under the poisoning deleter a read of the freed group sees 0xDD… pointers and crashes deterministically.
TEST_CASE("racks: moving a mapped device out of a rack between a group and its tracks (M4 review H1)") {
  REQUIRE(pq::alloc_trap::installed());
  DocumentStore store;
  IdSource& ids = store.ids();
  auto edit = [&](Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const pq::Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    return *e;
  };
  const TrackId a = edit(ops::createTrack(*store.current(), ids, TrackKind::Midi)).tracks.at(0);
  const TrackId b = edit(ops::createTrack(*store.current(), ids, TrackKind::Midi)).tracks.at(0);
  edit(ops::groupTracks(*store.current(), ids, std::vector<TrackId>{a, b}));
  TrackId g;
  for (const TrackPtr& t : store.current()->tracks)
    if (t->kind == TrackKind::Group) g = t->id;
  REQUIRE(g.valid());
  REQUIRE(findTrackPtr(*store.current(), a) != nullptr);

  // A rack on `host` holding a Utility mapped to macro 1, then the Utility moved to `dest` (out of the rack).
  auto moveOut = [&](TrackId host, TrackId dest) {
    const DeviceId rack =
        edit(ops::addDevice(*store.current(), ids, host, std::string(kAudioEffectRack), DeviceKind::AudioEffect)).devices.at(0);
    const DeviceId u = edit(ops::addChainWithDevice(*store.current(), ids, rack, "utility", DeviceKind::AudioEffect)).devices.at(0);
    edit(ops::mapMacro(*store.current(), ids, rack, 0, u, "width"));
    const Snapshot before = store.current();
    {
      pq::alloc_trap::PoisonFrees poison;
      edit(ops::moveDevice(*store.current(), u, dest, kAppend));
      const Project& p = *store.current();
      const DeviceLoc where = findDevice(p, u);
      REQUIRE(where.track != nullptr);
      CHECK(where.track->id == dest);
      CHECK(where.rack == nullptr);
      CHECK(mappingOf(p, u, "width").mapping == nullptr);  // pruned in the same step
      CHECK(findDevice(p, rack).device->rack->mappings.empty());
      // The input snapshot is untouched (undo target): the Utility is still in the rack, mapped.
      CHECK(findDevice(*before, u).rack != nullptr);
      CHECK(mappingOf(*before, u, "width").mapping != nullptr);
      // Save + reopen: the file loads (checkRack refuses a mapping onto a device outside its rack).
      Result<std::string> text = serializeProject(p);
      REQUIRE(text.ok());
      Result<LoadedProject> back = parseProject(*text);
      REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
      CHECK(findDevice(*back->project, u).track->id == dest);
    }
    REQUIRE(store.undo());
    CHECK(mappingOf(*store.current(), u, "width").mapping != nullptr);
    REQUIRE(store.redo());
  };
  SUBCASE("group -> its child") { moveOut(g, a); }
  SUBCASE("child -> its group") { moveOut(a, g); }
  SUBCASE("sibling -> sibling") { moveOut(a, b); }
  SUBCASE("group -> its child, then back, then to a sibling (repeated path copies)") {
    moveOut(g, b);
    moveOut(b, g);
    moveOut(g, a);
  }
}

// M4 review M9 (the user: no depth cap, "it should be able to go forever as one pleases"): racks nested 20,000 deep load,
// save, round-trip, lower and edit without a stack overflow (every rack walk is iterative; the destructor of a nest
// frees it level by level). An Audio Effect Rack nest with a Utility at the bottom (after the track's instrument), and
// an Instrument Rack nest with the instrument at the bottom; the outer racks' macro 1 maps each inner rack's macro 1
// in a shallower nest (a mapping chain through every level).
TEST_CASE("racks: 20,000 nested racks load, save, round-trip, lower and edit without a stack overflow (M9)" * doctest::test_suite("slow")) {
  constexpr int kDepth = 20000;
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId fx = ids.track(), inst = ids.track();
  DeviceId bottomUtil, bottomSynth, outerFx;
  {
    // Inside out: no recursion while building either.
    bottomUtil = ids.device();
    DevicePtr inner = makeDevice(bottomUtil, "utility", DeviceKind::AudioEffect);
    for (int k = 0; k < kDepth; ++k) {
      const DeviceId id = ids.device();
      inner = makeRack(id, DeviceKind::AudioEffect, {makeChain(ids.chain(), "C", {inner})});
      outerFx = id;
    }
    Track t = makeTrack(fx, TrackKind::Midi, "Fx");
    t.devices = {makeDefaultInstrument(ids.device()), inner};
    bottomSynth = ids.device();
    DevicePtr in2 = makeDefaultInstrument(bottomSynth);
    for (int k = 0; k < kDepth; ++k) in2 = makeRack(ids.device(), DeviceKind::Instrument, {makeChain(ids.chain(), "C", {in2})});
    Track t2 = makeTrack(inst, TrackKind::Midi, "Inst");
    t2.devices = {in2};
    Batch b{"nests", {}};
    b.commands.push_back(AddTrack{std::make_shared<const Track>(std::move(t)), {}, kAppend});
    b.commands.push_back(AddTrack{std::make_shared<const Track>(std::move(t2)), {}, kAppend});
    REQUIRE(dispatch(store, b).ok());
  }
  // Save, load, compare (deep equality is iterative too), save again: the same text.
  const Result<std::string> text = serializeProject(*store.current());
  REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
  MESSAGE("20,000-deep nest: " << text->size() / 1024 << " KB of project text");
  CHECK(text->size() < 64u * 1024 * 1024);  // the indentation stops growing (not O(depth²))
  Result<LoadedProject> loaded = parseProject(*text);
  REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
  CHECK(*loaded->project == *store.current());
  const Result<std::string> again = serializeProject(*loaded->project);
  REQUIRE(again.ok());
  CHECK(*again == *text);
  // Lowering: the bottom devices are called, inside every rack.
  LowerOptions o;
  o.library = &library().library();
  o.slotOf = [](TrackId) { return 0u; };
  {
    Result<LoweredProject> lp = lower(*loaded->project, o);
    REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
    for (size_t i = 0; i < lp->trackIds.size(); ++i) {
      const pq::graph::Patch& patch = lp->patches[i]->patch;
      if (lp->trackIds[i] == fx) CHECK(patch.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Device, bottomUtil.value, 0)}).has_value());
      if (lp->trackIds[i] == inst) CHECK(patch.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Device, bottomSynth.value, 0)}).has_value());
    }
  }
  // Edits at the bottom: a param (path copy down 20,000 levels), then the Utility moved out to the track (taken from
  // the tree, mappings pruned on the way up), then undone; a duplicate of the whole nest (a deep clone).
  {
    auto e = ops::setDeviceParam(*store.current(), bottomUtil, "gain", -6.0);
    REQUIRE(e.ok());
    REQUIRE(dispatch(store, e->command).ok());
    CHECK(findDevice(*store.current(), bottomUtil).device->param("gain")->value == -6.0);
  }
  {
    auto e = ops::moveDevice(*store.current(), bottomUtil, fx, kAppend);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(dispatch(store, e->command).ok());
    const DeviceLoc l = findDevice(*store.current(), bottomUtil);
    CHECK(l.rack == nullptr);  // now on the track's own list
    REQUIRE(store.undo());
    CHECK(findDevice(*store.current(), bottomUtil).rack != nullptr);
  }
  {
    const DeviceId one[] = {outerFx};
    auto e = ops::duplicateDevices(*store.current(), ids, one);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(dispatch(store, e->command).ok());
    size_t devices = 0;
    forEachDeviceIn(findTrackPtr(*store.current(), fx)->devices, [&](const Device&) { ++devices; });
    CHECK(devices == 2u * (kDepth + 1) + 1);
  }
  // A mapping chain through 2,000 levels (each rack's macro 1 maps the next inner rack's macro 1): lowered as one
  // chain of mapped values (the lowering follows it iteratively).
  {
    DocumentStore s2;
    IdSource& i2 = s2.ids();
    const TrackId t = i2.track();
    DevicePtr inner = makeDevice(i2.device(), "utility", DeviceKind::AudioEffect);
    DeviceId innerId = inner->id;
    std::string innerParam = "gain";
    for (int k = 0; k < 2000; ++k) {
      DevicePtr r = makeRack(i2.device(), DeviceKind::AudioEffect, {makeChain(i2.chain(), "C", {inner})});
      Device d = *r;
      Rack rk = *d.rack;
      rk.mappings.push_back(MacroMapping{i2.mapping(), 0, innerId, innerParam, 0.0, 1.0, 0.0});
      d.rack = std::make_shared<const Rack>(std::move(rk));
      innerId = d.id;
      innerParam = "macro1";
      inner = std::make_shared<const Device>(std::move(d));
    }
    Track tr = makeTrack(t, TrackKind::Midi, "Map");
    tr.devices = {makeDefaultInstrument(i2.device()), inner};
    REQUIRE(dispatch(s2, AddTrack{std::make_shared<const Track>(std::move(tr)), {}, kAppend}).ok());
    Result<LoweredProject> lp = lower(*s2.current(), o);
    REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
    const auto params = std::count_if(lp->params.begin(), lp->params.end(), [](const LoweredParam& p) { return p.key.ends_with(":macro1"); });
    CHECK(params == 1);  // only the outermost macro is a param: every inner one is mapped
  }
}
