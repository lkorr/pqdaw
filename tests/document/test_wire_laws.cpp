// TX.patching (SPEC §15 2026-11-01 "a wire adds its value in the target param's own units", "free patching in the
// circuit"): the document side — wire laws (Add for new wires, Travel only from migration), schema 13 and its
// 12 → 13 migration (an old project lowers to exactly the same graph), wires from any output into params and inputs,
// TX.all-free: blocks off the main path (takeOffPath, the file, trackStages). GU.11: blocks only — the LFO module's
// main output is the source; the targets are native engines (an Envelope "env" on the path: attack curved, sustain
// linear; a Wavetable "wt"), atom blocks and modules.
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/meta/type.h"
#include "../support/blocks.h"

using namespace pq::doc;
using pq::Result;

namespace {

std::string stdlibDir() { return std::string(PQ_TEST_DATA_DIR) + "/../stdlib"; }

DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(stdlibDir());
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// A MIDI track: its instrument a Wavetable engine in an instrument group (inst; test_blocks::addEngineInstrument: no
// knob curves, so the project may be relabelled as an older schema), an LFO (a Wavetable engine off the path: its
// `out` is the modulation source) and an Envelope engine after the instrument (env).
struct Doc {
  DocumentStore store;
  TrackId keys;
  DeviceId inst, lfo, env;
  Doc() {
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys");
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    keys = t->tracks.at(0);
    inst = test_blocks::addEngineInstrument(store, keys, "wavetable", library(), true);  // (relabelled as schema 12)
    auto o = ops::addMetaDevice(*store.current(), store.ids(), keys, "wavetable", ops::kOffPath);
    REQUIRE(o.ok());
    REQUIRE(store.dispatch(o->command).ok());
    lfo = o->devices.at(0);
    env = engine("envelope");
  }
  DeviceId add(std::string patch, uint32_t slot = kAppend) {
    auto e = ops::addDevice(*store.current(), store.ids(), keys, std::move(patch), DeviceKind::AudioEffect, slot);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return e->devices.at(0);
  }
  // A native engine spliced in at the path's end (after the instrument group).
  DeviceId engine(const char* type) { return test_blocks::appendEngineAfter(store, keys, type); }
  WireId run(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return e->wires.empty() ? WireId{} : e->wires.at(0);
  }
  const Project& doc() const { return *store.current(); }
};

LoweredProject lowerDoc(const Project& p) {
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  auto r = lower(p, o);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(*r);
}

WireStatus statusOf(const LoweredProject& l, WireId w) {
  for (const WireState& s : l.wires)
    if (s.id == w) return s.status;
  FAIL("no state for the wire");
  return WireStatus::Invalid;
}

}  // namespace

TEST_CASE("wire laws: a new wire adds in units and has no amount; a Travel wire keeps its depth") {
  Doc d;
  const WireId add = d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "out", d.env, "attack"));
  const WireId travel = d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.lfo, "out", d.env, "sustain", 0.4));
  const Wire* a = findWire(d.doc(), add);
  const Wire* t = findWire(d.doc(), travel);
  REQUIRE(a != nullptr);
  REQUIRE(t != nullptr);
  CHECK(a->law == WireLaw::Add);
  CHECK(a->amount == 1.0);
  CHECK(t->law == WireLaw::Travel);
  CHECK(t->amount == 0.4);
  // An added wire has no amount to set or automate (a VCA scales its signal).
  CHECK_FALSE(ops::setWireAmount(d.doc(), add, 0.3).ok());
  CHECK_FALSE(d.store.dispatch(SetWireAmount{add, 0.3}).ok());
  CHECK(ops::setWireAmount(d.doc(), travel, -0.2).ok());
  AutomationLane lane;
  lane.id = d.store.ids().lane();
  lane.param = ParamRef::wireAmount(add);
  CHECK_FALSE(d.store.dispatch(AddAutomationLane{d.keys, std::make_shared<const AutomationLane>(lane)}).ok());
  // Lowering: the Add wire has no amount param, its tap is in units; the Travel one keeps its `w<id>` and travel tap.
  const LoweredProject l = lowerDoc(d.doc());
  CHECK(statusOf(l, add) == WireStatus::Lowered);
  CHECK(statusOf(l, travel) == WireStatus::Lowered);
  bool addAmount = false, travelAmount = false;
  for (const LoweredParam& p : l.params) {
    addAmount = addAmount || p.key == "wire" + std::to_string(add.value) + ":amount";
    travelAmount = travelAmount || p.key == "wire" + std::to_string(travel.value) + ":amount";
  }
  CHECK_FALSE(addAmount);
  CHECK(travelAmount);
  REQUIRE(l.taps.size() == 2);
  // attack is on its declared Exp0 curve, so the added wire adds in knob position (GU.1; its tap too).
  for (const ModulationTap& tap : l.taps) CHECK(tap.domain == ModulationTap::Domain::Position);
}

TEST_CASE("wire laws: any output into any param or input; a Travel wire goes into params only") {
  Doc d;
  const DeviceId util = d.engine("wavetable");
  const DeviceId cmp = d.add("atom:gt", ops::kOffPath);  // a bool output
  // An effect's main output (an `out`, not an export) into a param and into another device's audio input.
  const WireId outToParam = d.run(ops::connectWire(d.doc(), d.store.ids(), d.env, "out", util, "position"));
  const WireId outToInput = d.run(ops::connectWire(d.doc(), d.store.ids(), d.inst, std::string(kMainPort), util, "pos"));
  // A Travel wire into an input does not play; GU.9: from an `out` into a param it does (a rack macro's mapping).
  const WireId oldOut = d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.env, "out", util, "fold", 0.5));
  const WireId oldIn = d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.lfo, "out", util, "x", 0.5));
  // TX.bool-real: a bool output (a comparison's) is a signal, its 0 / 1; a const input (a setting) is not a target.
  const WireId done = d.run(ops::connectWire(d.doc(), d.store.ids(), cmp, "out", util, "warp"));
  const WireId setting = d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "out", util, "mode"));
  const LoweredProject l = lowerDoc(d.doc());
  CHECK(statusOf(l, outToParam) == WireStatus::Lowered);
  CHECK(statusOf(l, outToInput) == WireStatus::Lowered);
  CHECK(statusOf(l, oldOut) == WireStatus::Lowered);
  CHECK(statusOf(l, oldIn) == WireStatus::Invalid);
  CHECK(statusOf(l, done) == WireStatus::Lowered);
  CHECK(statusOf(l, setting) == WireStatus::Invalid);
  // The Envelope's output into a later engine's param is no loop; that engine's output back into the Envelope's input
  // is one.
  const WireId back = d.run(ops::connectWire(d.doc(), d.store.ids(), util, "out", d.env, "in"));
  CHECK(statusOf(lowerDoc(d.doc()), back) == WireStatus::Delayed);
}

TEST_CASE("wire laws: schema 13 writes the law, 12 -> 13 makes every wire Travel and lowers to the same graph") {
  Doc d;
  d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.lfo, "out", d.env, "attack", 0.4));
  d.run(ops::connectTravelNoteWire(d.doc(), d.store.ids(), d.keys, "velocity", d.env, "sustain", -0.25));
  const DeviceId util = d.engine("wavetable");
  // TX.pan-laws: a MIDI track of schema 12 panned with constant power (the 17 -> 18 step gives it that law back).
  Batch laws{"laws", {}};
  forEachTrack(d.doc(), [&](const Track& t, const Track*) {
    if (t.kind == TrackKind::Midi) laws.commands.push_back(SetTrackPanMode{t.id, PanMode::ConstantPower});
  });
  REQUIRE(d.store.dispatch(laws).ok());
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->find("\"law\": \"travel\"") != std::string::npos);
  // The same project as schema 12 wrote it: no "law" (every wire was the knob-travel law), version 12.
  nlohmann::json j = nlohmann::json::parse(*text);
  for (nlohmann::json& w : j["wires"]) {
    // A wire on a main port (a main link, an instrument group's In → engine → Out: one end "") adds, as it did then.
    if (w.value("port", std::string("?")).empty() || w.value("param", std::string("?")).empty()) continue;
    if (w.value("chain", false)) continue;  // (IG.1-2: an instrument group's chain links: added, no "law")
    REQUIRE(w.contains("law"));
    w.erase("law");
  }
  j["schemaVersion"] = 12;
  Result<LoadedProject> old = parseProject(j.dump(2));
  REQUIRE_MESSAGE(old.ok(), (old.ok() ? std::string() : old.error().message));
  CHECK(old->fileSchemaVersion == 12);
  // (IG.1-2: the 18 -> 19 step wraps an instrument into an instrument group, as for every older file; this one's is one
  // already.)
  Project want = *d.store.current();
  (void)migrateToInstrumentGroups(want);
  CHECK(*old->project == want);
  for (const Wire& w : wiresOf(*old->project))
    if (!w.port.empty() && !w.param.empty() && !w.chain) CHECK(w.law == WireLaw::Travel);  // (the main links of this relabelled text add)
  // The lowered graph of the migrated file is the one the Travel law built before TX.patching (the same IR, so the
  // same audio: tests/app/test_wires.cpp renders that law bit for bit like a hand-built graph).
  const LoweredProject a = lowerDoc(want), b = lowerDoc(*old->project);
  REQUIRE(a.patches.size() == b.patches.size());
  for (size_t i = 0; i < a.patches.size(); ++i) CHECK(a.patches[i]->hash == b.patches[i]->hash);
  // An added wire is written without "law" and "amount"; reading one with an amount is refused, as is a Travel wire
  // without one.
  const WireId w = d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "out", util, "position"));
  Result<std::string> now = serializeProject(d.doc());
  REQUIRE(now.ok());
  nlohmann::json k = nlohmann::json::parse(*now);
  nlohmann::json* added = nullptr;
  for (nlohmann::json& x : k["wires"])
    if (x["id"] == w.value) added = &x;
  REQUIRE(added != nullptr);
  CHECK_FALSE(added->contains("law"));
  CHECK_FALSE(added->contains("amount"));
  Result<LoadedProject> back = parseProject(*now);
  REQUIRE(back.ok());
  CHECK(*back->project == d.doc());
  (*added)["amount"] = 0.5;
  CHECK_FALSE(parseProject(k.dump(2)).ok());
  added->erase("amount");
  (*added)["law"] = "travel";
  CHECK_FALSE(parseProject(k.dump(2)).ok());
  (*added)["law"] = "bend";
  CHECK_FALSE(parseProject(k.dump(2)).ok());
}

TEST_CASE("TX.all-free: blocks off the main path — taken off it in one undo step, out of the flow, called, in the file") {
  Doc d;
  const DeviceId mod = test_blocks::appendEngineAfter(d.store, d.keys, "wavetable");
  const DeviceId sat = test_blocks::appendAfter(d.store, d.keys, "atom:tanh");
  const DeviceId devs[] = {mod, sat};
  auto e = ops::takeOffPath(d.doc(), d.store.ids(), nullptr, devs);
  REQUIRE(e.ok());
  const size_t steps = d.store.history().entries().size();
  REQUIRE(d.store.dispatch(e->command).ok());
  CHECK(d.store.history().entries().size() == steps + 1);
  const Track& t = *findTrackPtr(d.doc(), d.keys);
  const MainPath path = mainPath(d.doc(), t);
  CHECK_FALSE(path.contains(mod));
  CHECK_FALSE(path.contains(sat));
  REQUIRE(path.blocks.size() == 2);  // the instrument → the Envelope (the path closed up where they were)
  CHECK(path.blocks[1]->id == d.env);
  CHECK(path.complete);
  // trackStages lists them but leaves them out of the flow: the Envelope receives the instrument's audio.
  const std::vector<ChainStage> st = trackStages(d.doc(), t);
  auto stageOf = [&](DeviceId id) -> const ChainStage* {
    for (const ChainStage& x : st)
      if (x.device->id == id) return &x;
    return nullptr;
  };
  REQUIRE(stageOf(d.env) != nullptr);
  CHECK(stageOf(d.env)->in == ChainSignal::Audio);
  CHECK_FALSE(stageOf(d.env)->offPath);
  for (const DeviceId x : {mod, sat, d.lfo}) {
    REQUIRE(stageOf(x) != nullptr);
    CHECK(stageOf(x)->offPath);
    CHECK(stageOf(x)->in == ChainSignal::None);
  }
  // The lowering calls them (the views draw them) but they are not on the path.
  const LoweredProject l = lowerDoc(d.doc());
  const pq::graph::Patch& tp = *l.graph.tracks.at(0).patch;
  CHECK(tp.findByName("d" + std::to_string(mod.value)).has_value());
  CHECK(tp.findByName("d" + std::to_string(sat.value)).has_value());
  CHECK(l.missingDevices.empty());
  // The file keeps the wiring (there is no "free" flag any more); undo puts them back on the path.
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->find("\"free\"") == std::string::npos);
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE(back.ok());
  CHECK(*back->project == d.doc());
  REQUIRE(d.store.undo());
  CHECK(mainPath(d.doc(), *findTrackPtr(d.doc(), d.keys)).contains(mod));
  CHECK(mainPath(d.doc(), *findTrackPtr(d.doc(), d.keys)).contains(sat));
}

TEST_CASE("TX.all-free: a meta device taken off the path no longer takes the notes; the next one on the path does") {
  DocumentStore store;
  auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys");
  REQUIRE(t.ok());
  REQUIRE(store.dispatch(t->command).ok());
  const TrackId keys = t->tracks.at(0);
  {  // the default instrument group out: the path empty (source → mixer)
    const DeviceId inst = mainPath(*store.current(), *findTrackPtr(*store.current(), keys)).blocks.at(0)->id;
    auto del = ops::deleteDevices(*store.current(), std::vector<DeviceId>{inst});
    REQUIRE(del.ok());
    REQUIRE(store.dispatch(del->command).ok());
    REQUIRE(mainPath(*store.current(), *findTrackPtr(*store.current(), keys)).blocks.empty());
  }
  auto a = ops::addMetaDevice(*store.current(), store.ids(), keys, "envelope", 0);  // takes the notes (GU.4: was a PA)
  REQUIRE(a.ok());
  REQUIRE(store.dispatch(a->command).ok());
  auto b = ops::addMetaDevice(*store.current(), store.ids(), keys, "wavetable");
  REQUIRE(b.ok());
  REQUIRE(store.dispatch(b->command).ok());
  const DeviceId pa = a->devices.at(0), wt = b->devices.at(0);
  CHECK(trackStages(*store.current(), *findTrackPtr(*store.current(), keys)).at(0).in == ChainSignal::Notes);
  auto off = ops::takeOffPath(*store.current(), store.ids(), nullptr, std::vector<DeviceId>{pa});
  REQUIRE(off.ok());
  REQUIRE(store.dispatch(off->command).ok());
  const std::vector<ChainStage> st = trackStages(*store.current(), *findTrackPtr(*store.current(), keys));
  REQUIRE(st.size() == 2);
  CHECK(st[0].device->id == wt);
  CHECK(st[0].in == ChainSignal::Notes);  // the Wavetable takes the note source
  CHECK(st[1].offPath);
  REQUIRE(store.undo());
  CHECK(trackStages(*store.current(), *findTrackPtr(*store.current(), keys)).at(0).device->id == pa);
}
