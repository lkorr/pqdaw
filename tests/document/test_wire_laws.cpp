// TX.patching (SPEC §15 2026-11-01 "a wire adds its value in the target param's own units", "free patching in the
// circuit"): the document side — wire laws (Add for new wires, Travel only from migration), schema 13 and its
// 12 → 13 migration (an old project lowers to exactly the same graph), wires from any output into params and inputs,
// free devices (SetDeviceFree, the file, the rules, chainStages).
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/meta/type.h"

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

struct Doc {
  DocumentStore store;
  TrackId keys;
  DeviceId inst, lfo, eq;
  Doc() {
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys");
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    keys = t->tracks.at(0);
    inst = findTrackPtr(*store.current(), keys)->devices.at(0)->id;
    lfo = add("lfo_device");
    eq = add("eq_eight");
  }
  DeviceId add(const char* patch) {
    auto e = ops::addDevice(*store.current(), store.ids(), keys, patch, DeviceKind::AudioEffect);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return e->devices.at(0);
  }
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
  const WireId add = d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "freq1"));
  const WireId travel = d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "gain2", 0.4));
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
  // TX.knob-curves: freq1 is on its declared octave curve, so the added wire adds in knob travel (its tap too).
  for (const ModulationTap& tap : l.taps) CHECK_FALSE(tap.units);
}

TEST_CASE("wire laws: any output into any param or input; a migrated Travel wire keeps T4.7's ends") {
  Doc d;
  const DeviceId util = d.add("utility");
  // An effect's main output (an `out`, not an export) into a param and into another device's audio input.
  const WireId outToParam = d.run(ops::connectWire(d.doc(), d.store.ids(), d.eq, "out", util, "balance"));
  const WireId outToInput = d.run(ops::connectWire(d.doc(), d.store.ids(), d.inst, "out", util, "x"));
  // A Travel wire (a migrated one) from an `out`, or into an input, does not play: T4.7 took exports into params only.
  const WireId oldOut = d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.eq, "out", util, "width", 0.5));
  const WireId oldIn = d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.lfo, "lfo", util, "x", 0.5));
  // A bool output (an instrument's `done`) is not a signal; a const input (a setting) is not a target.
  const WireId done = d.run(ops::connectWire(d.doc(), d.store.ids(), d.inst, "done", util, "gain"));
  const WireId setting = d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", util, "bassMono"));
  const LoweredProject l = lowerDoc(d.doc());
  CHECK(statusOf(l, outToParam) == WireStatus::Lowered);
  CHECK(statusOf(l, outToInput) == WireStatus::Lowered);
  CHECK(statusOf(l, oldOut) == WireStatus::Invalid);
  CHECK(statusOf(l, oldIn) == WireStatus::Invalid);
  CHECK(statusOf(l, done) == WireStatus::Invalid);
  CHECK(statusOf(l, setting) == WireStatus::Invalid);
  // The EQ's output into a later utility's param is no loop; the utility's output back into the EQ's input is one.
  const WireId back = d.run(ops::connectWire(d.doc(), d.store.ids(), util, "out", d.eq, "x"));
  CHECK(statusOf(lowerDoc(d.doc()), back) == WireStatus::Delayed);
}

TEST_CASE("wire laws: schema 13 writes the law, 12 -> 13 makes every wire Travel and lowers to the same graph") {
  Doc d;
  d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "freq1", 0.4));
  d.run(ops::connectTravelNoteWire(d.doc(), d.store.ids(), d.keys, "velocity", d.eq, "gain2", -0.25));
  const DeviceId util = d.add("utility");
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->find("\"law\": \"travel\"") != std::string::npos);
  // The same project as schema 12 wrote it: no "law" (every wire was the knob-travel law), version 12.
  nlohmann::json j = nlohmann::json::parse(*text);
  for (nlohmann::json& w : j["wires"]) {
    REQUIRE(w.contains("law"));
    w.erase("law");
  }
  j["schemaVersion"] = 12;
  Result<LoadedProject> old = parseProject(j.dump(2));
  REQUIRE_MESSAGE(old.ok(), (old.ok() ? std::string() : old.error().message));
  CHECK(old->fileSchemaVersion == 12);
  CHECK(*old->project == d.doc());
  for (const Wire& w : wiresOf(*old->project)) CHECK(w.law == WireLaw::Travel);
  // The lowered graph of the migrated file is the one the Travel law built before TX.patching (the same IR, so the
  // same audio: tests/app/test_wires.cpp renders that law bit for bit like a hand-built graph).
  const LoweredProject a = lowerDoc(d.doc()), b = lowerDoc(*old->project);
  REQUIRE(a.patches.size() == b.patches.size());
  for (size_t i = 0; i < a.patches.size(); ++i) CHECK(a.patches[i]->hash == b.patches[i]->hash);
  // An added wire is written without "law" and "amount"; reading one with an amount is refused, as is a Travel wire
  // without one.
  const WireId w = d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", util, "balance"));
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

TEST_CASE("free devices: out of the series, one undo step, in the file, the rules") {
  Doc d;
  const DeviceId sat = d.add("saturator");
  const DeviceId devs[] = {d.lfo, sat};
  auto e = ops::setDevicesFree(d.doc(), devs, true);
  REQUIRE(e.ok());
  const size_t steps = d.store.history().entries().size();
  REQUIRE(d.store.dispatch(e->command).ok());
  CHECK(d.store.history().entries().size() == steps + 1);
  CHECK(findDevice(d.doc(), d.lfo).device->free);
  CHECK(findDevice(d.doc(), sat).device->free);
  CHECK_FALSE(findDevice(d.doc(), d.eq).device->free);
  // chainStages lists them but leaves them out of the flow: the EQ after the free LFO receives the instrument's audio.
  const std::vector<ChainStage> st = chainStages(findTrackPtr(d.doc(), d.keys)->devices, true);
  REQUIRE(st.size() == 4);
  CHECK(st[1].free);
  CHECK(st[1].in == ChainSignal::None);
  CHECK(st[2].in == ChainSignal::Audio);
  // The lowering calls them but chains only the others (the EQ's input is the instrument's voices summed).
  const LoweredProject l = lowerDoc(d.doc());
  const pq::graph::Patch& tp = *l.graph.tracks.at(0).patch;
  CHECK(tp.findByName("d" + std::to_string(d.lfo.value)).has_value());
  CHECK(tp.findByName("d" + std::to_string(sat.value)).has_value());
  CHECK(l.missingDevices.empty());
  // The file keeps it; an old file has none.
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->find("\"free\": true") != std::string::npos);
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE(back.ok());
  CHECK(*back->project == d.doc());
  // Undo puts them back.
  REQUIRE(d.store.undo());
  CHECK_FALSE(findDevice(d.doc(), d.lfo).device->free);
  // Refused: a stock instrument, a rack, a device in a rack chain.
  const DeviceId inst[] = {d.inst};
  CHECK_FALSE(ops::setDevicesFree(d.doc(), inst, true).ok());
  CHECK_FALSE(d.store.dispatch(SetDeviceFree{d.inst, true}).ok());
  auto g = ops::groupDevices(d.doc(), d.store.ids(), std::vector<DeviceId>{d.eq});
  REQUIRE(g.ok());
  REQUIRE(d.store.dispatch(g->command).ok());
  const DeviceId rack = g->devices.at(0);
  const DeviceId r1[] = {rack}, inner[] = {d.eq};
  CHECK_FALSE(ops::setDevicesFree(d.doc(), r1, true).ok());
  CHECK_FALSE(ops::setDevicesFree(d.doc(), inner, true).ok());
  CHECK_FALSE(d.store.dispatch(SetDeviceFree{d.eq, true}).ok());
}

TEST_CASE("free devices: a free meta device is never where instruments go; the next one in the series is") {
  DocumentStore store;
  auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys");
  REQUIRE(t.ok());
  REQUIRE(store.dispatch(t->command).ok());
  const TrackId keys = t->tracks.at(0);
  auto a = ops::addMetaDevice(*store.current(), store.ids(), keys, "phase_acc", 0);  // replaces the instrument
  REQUIRE(a.ok());
  REQUIRE(store.dispatch(a->command).ok());
  auto b = ops::addMetaDevice(*store.current(), store.ids(), keys, "wavetable");
  REQUIRE(b.ok());
  REQUIRE(store.dispatch(b->command).ok());
  const DeviceId pa = a->devices.at(0), wt = b->devices.at(0);
  CHECK(findDevice(*store.current(), pa).device->kind == DeviceKind::Instrument);
  REQUIRE(store.dispatch(SetDeviceFree{pa, true}).ok());
  CHECK(findDevice(*store.current(), pa).device->kind == DeviceKind::AudioEffect);
  CHECK(findDevice(*store.current(), wt).device->kind == DeviceKind::Instrument);  // now first in the series
  const std::vector<ChainStage> st = chainStages(findTrackPtr(*store.current(), keys)->devices, true);
  REQUIRE(st.size() == 2);
  CHECK(st[0].free);
  CHECK(st[1].in == ChainSignal::Notes);  // the Wavetable takes the note source
  REQUIRE(store.dispatch(SetDeviceFree{pa, false}).ok());
  CHECK(findDevice(*store.current(), pa).device->kind == DeviceKind::Instrument);
  CHECK(findDevice(*store.current(), wt).device->kind == DeviceKind::AudioEffect);
}
