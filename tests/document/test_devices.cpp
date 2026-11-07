// T4.1: device chains in the document — commands (chain rule, ChangeSet classes, no-ops), the edit builders (one undo
// step each), the block library, and the lowering of chains (stable NodeIds, params). GU.11: blocks only (groups, atom
// blocks, value blocks, native engines); the effects are the built-in modules' group presets (tests/support/blocks.h).

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/main_path.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/store.h"
#include "pq/document/modules.h"
#include "pq/meta/envelope.h"
#include "pq/graph/parse.h"
#include "../support/blocks.h"

using namespace pq::doc;
using pq::Result;

namespace {

std::string stdlibDir() { return std::string(PQ_TEST_DATA_DIR) + "/../stdlib"; }
DevicePtr valueBlock(DeviceId id) { return makeDevice(id, std::string(kValueDevice), DeviceKind::AudioEffect); }
// A native engine taking the notes (an Instrument-kind block: only on MIDI tracks).
DevicePtr instrumentEngine(DeviceId id) { return makeDevice(id, "meta:wavetable", DeviceKind::Instrument); }

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(stdlibDir());
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// A store with one MIDI track (the default instrument group "Basic Synth"), one group and one return.
struct Chains {
  DocumentStore store;
  TrackId midi, group, ret;
  DeviceId inst;
  Chains() {
    auto run = [&](Result<ops::Edit> e) {
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
      REQUIRE(store.dispatch(e->command).ok());
      return *e;
    };
    midi = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi)).tracks.at(0);
    group = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Group)).tracks.at(0);
    ret = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Return)).tracks.at(0);
    inst = onPath(midi, 0)->id;
  }
  const Track& track(TrackId id) const { return *findTrackPtr(*store.current(), id); }
  // TX.all-free: the track's main path (what the Device view lists), in signal order: the blocks' names (a module
  // group's name is the module's).
  std::vector<std::string> chain(TrackId id) const {
    std::vector<std::string> v;
    for (const Device* d : mainPath(*store.current(), track(id)).blocks)
      v.push_back((d->name.empty() ? d->patch : d->name) + (d->active ? "" : "(off)"));
    return v;
  }
  DevicePtr onPath(TrackId id, size_t i) const {
    for (const DevicePtr& d : track(id).devices)
      if (d.get() == mainPath(*store.current(), track(id)).blocks.at(i)) return d;
    return nullptr;
  }
  // Dispatches a builder's edit: exactly one history entry (or none for an empty batch).
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    REQUIRE(store.dispatch(e->command).ok());
    CHECK(store.history().cursor() == before + (e->empty() ? 0u : 1u));
    return *e;
  }
};

}  // namespace

TEST_CASE("devices: a new MIDI track has the default instrument; other tracks start empty") {
  Chains c;
  CHECK(c.chain(c.midi) == std::vector<std::string>{std::string(kDefaultInstrumentName)});
  // GU.10: the default instrument is a group preset — the instrument group "Basic Synth" holding the Phase
  // Accumulator, the Wavetable and the Envelope (each a module: a group around its blocks or engine), no stock patch.
  const Track& t = c.track(c.midi);
  const Device& g = *findDevice(*c.store.current(), c.inst).device;
  CHECK(isGroupDevice(g));
  CHECK(g.name == std::string(kDefaultInstrumentName));
  std::vector<std::string> modules;
  for (const Device* m : groupMembers(t.devices, g.id)) modules.push_back(m->name);
  CHECK(modules == std::vector<std::string>{"Phase Accumulator", "Wavetable", "Envelope"});
  for (const DevicePtr& d : t.devices) CHECK(d->kind == DeviceKind::AudioEffect);  // blocks and groups, no instrument patch
  CHECK(c.chain(c.group).empty());
  CHECK(c.chain(c.ret).empty());
  CHECK(c.store.current()->master->devices.empty());
  CHECK(findDevice(*c.store.current(), c.inst).track == &c.track(c.midi));
  CHECK(maxEntityId(*c.store.current()) >= c.inst.value);
}

TEST_CASE("devices: commands keep the chain rule and classify their changes") {
  Chains c;
  const Snapshot s0 = c.store.current();
  IdSource& ids = c.store.ids();
  auto apply1 = [&](const Snapshot& s, Command cmd, ChangeSet& cs) { return apply(s, cmd, cs); };
  SUBCASE("AddDevice: structural; instruments only on MIDI tracks (TX.all-free: a set of blocks, no position rule)") {
    ChangeSet cs;
    const DevicePtr fx = valueBlock(ids.device());
    auto r = apply1(s0, AddDevice{c.midi, fx, kAppend}, cs);
    REQUIRE(r.ok());
    CHECK(cs.structural());
    CHECK(cs.tracks == std::vector<TrackId>{c.midi});
    CHECK(cs.devices == std::vector<DeviceId>{fx->id});
    ChangeSet e1, e2, e3, e4;
    // TX.all-free: a list position means nothing on a track (its cables are the signal path): an effect at 0 and a
    // second instrument are blocks like any (the path decides what takes the notes).
    CHECK(apply1(s0, AddDevice{c.midi, valueBlock(ids.device()), 0}, e1).ok());
    CHECK_FALSE(apply1(s0, AddDevice{c.group, instrumentEngine(ids.device()), 0}, e2).ok());
    CHECK(apply1(s0, AddDevice{c.midi, instrumentEngine(ids.device()), kAppend}, e3).ok());
    CHECK_FALSE(apply1(s0, AddDevice{c.ret, nullptr, 0}, e4).ok());  // no device
    ChangeSet e5;
    CHECK_FALSE(apply1(*r, AddDevice{c.ret, fx, 0}, e5).ok());  // the id is in use
    // GU.11: only blocks — a stock patch name or an unknown atom is not one.
    ChangeSet e6, e7;
    CHECK_FALSE(apply1(s0, AddDevice{c.ret, makeDevice(ids.device(), "utility", DeviceKind::AudioEffect), 0}, e6).ok());
    CHECK_FALSE(apply1(s0, AddDevice{c.ret, makeDevice(ids.device(), "atom:no_such_atom", DeviceKind::AudioEffect), 0}, e7).ok());
  }
  SUBCASE("SetDeviceParam is a Param change with the device's ParamRef; no-ops return the same snapshot") {
    const DevicePtr v = valueBlock(ids.device());
    ChangeSet add;
    auto s1 = apply1(s0, AddDevice{c.midi, v, kAppend}, add);
    REQUIRE(s1.ok());
    const std::string kv(kValueParam);
    ChangeSet cs;
    auto r = apply1(*s1, SetDeviceParam{v->id, kv, 0.5}, cs);
    REQUIRE(r.ok());
    CHECK(cs.paramOnly());
    REQUIRE(cs.params.size() == 1);
    CHECK(cs.params[0] == ParamRef::device(v->id, kv));
    CHECK(cs.params[0].kind == ParamKind::DeviceParam);
    CHECK(isAutomatable(ParamKind::DeviceParam));  // T4.18: device automation
    const Device& d = *findDevice(**r, v->id).device;
    REQUIRE(d.param(kv) != nullptr);
    CHECK(d.param(kv)->value == 0.5);
    ChangeSet same;
    auto again = apply1(*r, SetDeviceParam{v->id, kv, 0.5}, same);
    CHECK((again->get() == r->get()));
    CHECK(same.empty());
    ChangeSet reset;
    auto back = apply1(*r, SetDeviceParam{v->id, kv, std::nullopt}, reset);
    REQUIRE(back.ok());
    CHECK(findDevice(**back, v->id).device->params.empty());
    ChangeSet bad;
    CHECK_FALSE(apply1(*s1, SetDeviceParam{v->id, "val ue", 1.0}, bad).ok());
    CHECK_FALSE(apply1(*s1, SetDeviceParam{v->id, kv, std::numeric_limits<double>::infinity()}, bad).ok());
    CHECK_FALSE(apply1(*s1, SetDeviceParam{DeviceId{999999}, kv, 1.0}, bad).ok());
    CHECK_FALSE(apply1(*s1, SetDeviceParam{v->id, "level", 0.5}, bad).ok());  // a value block's one control is `value`
  }
  SUBCASE("settings and activation are structural; values stay sorted") {
    const DeviceId g = ids.device();
    ChangeSet add;
    auto s1 = apply1(s0, AddDevice{c.midi, makeGroup(g), kAppend}, add);
    REQUIRE(s1.ok());
    ChangeSet cs;
    auto r = apply1(*s1, SetDeviceSetting{g, std::string(kGroupVoices), 4.0}, cs);
    REQUIRE(r.ok());
    CHECK(cs.structural());
    ChangeSet cs2;
    r = apply1(*r, SetDeviceSetting{g, std::string(kGroupGlide), 0.25}, cs2);
    REQUIRE(r.ok());
    const Device& d = *findDevice(**r, g).device;
    REQUIRE(d.settings.size() == 2);
    CHECK(d.settings[0].name < d.settings[1].name);
    ChangeSet cs3;
    auto off = apply1(*r, SetDeviceActive{c.inst, false}, cs3);
    REQUIRE(off.ok());
    CHECK(cs3.structural());
    CHECK_FALSE(findDevice(**off, c.inst).device->active);
  }
  SUBCASE("MoveDevice: within a track and across tracks (TX.all-free: the list order of the blocks; instruments only on MIDI tracks)") {
    const size_t n = c.track(c.midi).devices.size();
    Batch b{"fx", {}};
    const DevicePtr a = valueBlock(ids.device());
    const DevicePtr s = makeDevice(ids.device(), "atom:mul", DeviceKind::AudioEffect);
    const DevicePtr ins = instrumentEngine(ids.device());
    b.commands = {AddDevice{c.midi, a, kAppend}, AddDevice{c.midi, s, kAppend}, AddDevice{c.midi, ins, kAppend}};
    ChangeSet cs0;
    auto r = apply(s0, Command{b}, cs0);
    REQUIRE(r.ok());
    ChangeSet cs;
    auto m = apply1(*r, MoveDevice{s->id, {}, 1}, cs);
    REQUIRE(m.ok());
    CHECK(cs.tracks == std::vector<TrackId>{c.midi});
    const Track& t = *findTrackPtr(**m, c.midi);
    CHECK(t.devices[1]->id == s->id);
    CHECK(t.devices[n + 1]->id == a->id);  // (the default instrument's n blocks, s moved to 1, then a)
    CHECK((t.devices[1] == s));  // moved, not copied: pointer-equal
    ChangeSet bad;
    CHECK(apply1(*m, MoveDevice{s->id, {}, 0}, bad).ok());  // TX.all-free: before the instrument in the list: allowed
    CHECK_FALSE(apply1(*m, MoveDevice{ins->id, c.group, 0}, bad).ok());  // an instrument onto a group
    ChangeSet across;
    auto x = apply1(*m, MoveDevice{a->id, c.ret, 0}, across);
    REQUIRE(x.ok());
    CHECK(across.tracks == std::vector<TrackId>{c.midi, c.ret});
    CHECK(findDevice(**x, a->id).track->id == c.ret);
    ChangeSet none;
    auto same = apply1(*x, MoveDevice{s->id, {}, 1}, none);
    CHECK((same->get() == x->get()));
  }
  SUBCASE("RemoveTrack lists the track's devices") {
    ChangeSet cs;
    REQUIRE(apply1(s0, RemoveTrack{c.midi}, cs).ok());
    CHECK(std::find(cs.devices.begin(), cs.devices.end(), c.inst) != cs.devices.end());
  }
}

TEST_CASE("devices: add, reorder, remove, deactivate, copy / paste, duplicate are one undo step each") {
  Chains c;
  const Project* p = c.store.current().get();
  const std::string kv(kValueParam);
  auto module = [](std::string_view key) { return std::string(kModuleItemPrefix) + std::string(key); };
  // Add (the device menu: a module's group preset): effects append after the instrument (TX.all-free: spliced into the
  // main path).
  const DeviceId utility = c.edit(ops::addDevice(*c.store.current(), c.store.ids(), c.midi, module("utility"), DeviceKind::AudioEffect)).devices.at(0);
  const DeviceId sat = c.edit(ops::addDevice(*c.store.current(), c.store.ids(), c.midi, module("saturator"), DeviceKind::AudioEffect, 1)).devices.at(0);
  CHECK(c.chain(c.midi) == std::vector<std::string>{"Basic Synth", "Saturator", "Utility"});
  // An instrument replaces the track's instrument (Live): the instrument group heading the path.
  const DeviceId synth2 = c.edit(ops::addDevice(*c.store.current(), c.store.ids(), c.midi, module("operator"), DeviceKind::Instrument)).devices.at(0);
  CHECK(c.onPath(c.midi, 0)->id == synth2);
  CHECK(findDevice(*c.store.current(), c.inst).device == nullptr);
  CHECK(c.chain(c.midi) == std::vector<std::string>{"Operator", "Saturator", "Utility"});
  CHECK_FALSE(ops::addDevice(*c.store.current(), c.store.ids(), c.group, module("operator"), DeviceKind::Instrument).ok());
  // Reorder (TX.all-free: rewired along the main path).
  c.edit(ops::moveOnPath(*c.store.current(), c.store.ids(), &library(), utility, c.midi, 1));
  CHECK(c.chain(c.midi) == std::vector<std::string>{"Operator", "Utility", "Saturator"});
  const auto noop = ops::moveOnPath(*c.store.current(), c.store.ids(), &library(), utility, c.midi, 1);
  REQUIRE(noop.ok());
  CHECK(noop->empty());
  // Deactivate (toggle), and the controls inside (the Saturator's value blocks).
  c.edit(ops::toggleDevicesActive(*c.store.current(), std::vector<DeviceId>{sat}));
  CHECK(c.chain(c.midi) == std::vector<std::string>{"Operator", "Utility", "Saturator(off)"});
  const DeviceId drive = test_blocks::member(*c.store.current(), sat, "Drive");
  REQUIRE(drive.valid());
  c.edit(ops::setDeviceParam(*c.store.current(), drive, kv, 0.75));
  // Copy / paste onto the group (fresh ids, the same values, the group's blocks with it), then duplicate there.
  std::vector<DevicePtr> clip{c.onPath(c.midi, 2)};
  REQUIRE(clip[0]->id == sat);
  const size_t satBlocks = groupMembersDeep(c.track(c.midi).devices, sat).size();
  const ops::Edit pasted = c.edit(ops::pasteDevices(*c.store.current(), c.store.ids(), c.group, clip));
  REQUIRE_FALSE(pasted.devices.empty());
  DeviceId copyId;  // the pasted group (the Edit lists its blocks too)
  for (const DeviceId id : pasted.devices)
    if (const Device* x = findDevice(*c.store.current(), id).device; x != nullptr && isGroupDevice(*x) && !x->group.valid())
      copyId = id;
  REQUIRE(copyId.valid());
  const Device& copy = *findDevice(*c.store.current(), copyId).device;
  CHECK(copy.id != sat);
  CHECK(copy.name == "Saturator");
  CHECK_FALSE(copy.active);
  CHECK(groupMembersDeep(c.track(c.group).devices, copy.id).size() == satBlocks);
  const DeviceId copyDrive = test_blocks::member(*c.store.current(), copy.id, "Drive");
  REQUIRE(copyDrive.valid());
  CHECK(copyDrive != drive);
  CHECK(findDevice(*c.store.current(), copyDrive).device->param(kv)->value == 0.75);  // GU.1: its position
  c.edit(ops::duplicateDevices(*c.store.current(), c.store.ids(), std::vector<DeviceId>{copy.id}));
  CHECK(c.chain(c.group) == std::vector<std::string>{"Saturator(off)", "Saturator(off)"});
  // Pasting an instrument onto a group is refused.
  std::vector<DevicePtr> instClip{instrumentEngine(c.store.ids().device())};
  CHECK_FALSE(ops::pasteDevices(*c.store.current(), c.store.ids(), c.group, instClip).ok());
  // Remove.
  c.edit(ops::deleteDevices(*c.store.current(), std::vector<DeviceId>{utility, sat}));
  CHECK(c.chain(c.midi) == std::vector<std::string>{"Operator"});
  // Undo walks every step back to the start.
  while (c.store.history().cursor() > 3) REQUIRE(c.store.undo());  // (3: the three new tracks)
  CHECK(*c.store.current() == *p);
}

TEST_CASE("devices: the block library (GU.11: no stock devices)") {
  const DeviceLibrary& lib = library();
  // A group, a value block, an atom block and a native engine each have a description; anything else has none.
  const DevicePtr g = makeGroup(DeviceId{4});
  const DevicePtr v = makeDevice(DeviceId{5}, std::string(kValueDevice), DeviceKind::AudioEffect);
  const DevicePtr a = makeDevice(DeviceId{6}, "atom:mul", DeviceKind::AudioEffect);
  const DevicePtr e = makeDevice(DeviceId{7}, "meta:wavetable", DeviceKind::AudioEffect);
  for (const DevicePtr& d : {g, v, a, e}) {
    CAPTURE(d->patch);
    CHECK(lib.patchOf(*d) != nullptr);
    CHECK(lib.infoOf(*d) != nullptr);
  }
  for (const char* stock : {"utility", "saturator", "BasicSynth", "operator", "eq_eight", "atom:no_such_atom"}) {
    CAPTURE(stock);
    const DevicePtr d = makeDevice(DeviceId{8}, stock, DeviceKind::AudioEffect);
    CHECK(lib.patchOf(*d) == nullptr);
    CHECK(lib.infoOf(*d) == nullptr);
  }
  // The value block's one control, its position and value through its curve.
  const DeviceParamInfo* value = lib.infoOf(*v)->param(kValueParam);
  REQUIRE(value != nullptr);
  Device d = *v;
  d.params = {{std::string(kValueParam), 0.5}};
  CHECK(deviceParamPosition(d, *value) == 0.5);
  CHECK(deviceParamValue(d, *value) == doctest::Approx(pq::dsp::evalCurve(deviceParamCurve(d, *value), 0.5)));
  // A patch that is not a device is refused.
  auto notDevice = pq::graph::parse("patch p {\n  in a : real const = 1\n}\n", "p.pqp");
  REQUIRE(notDevice.ok());
  CHECK_FALSE(describeDevice(notDevice->patches[0], "P").ok());
  // GU.11: the library reads nothing from disk: any directory loads.
  auto none = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/document");
  CHECK(none.ok());
}

TEST_CASE("devices: lowering a chain — stable NodeIds, params as runtime params") {
  Chains c;
  const std::string kv(kValueParam);
  const DeviceId util = test_blocks::addModule(c.store, c.midi, "utility");
  const DeviceId sat = test_blocks::addModule(c.store, c.midi, "saturator");
  const DeviceId drive = test_blocks::member(*c.store.current(), sat, "Drive");
  const DeviceId gain = test_blocks::member(*c.store.current(), util, "Gain");
  REQUIRE(drive.valid());
  REQUIRE(gain.valid());
  c.edit(ops::setDeviceParam(*c.store.current(), drive, kv, 0.75));
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  uint32_t slot = 0;
  lo.slotOf = [&](TrackId) { return slot++; };
  auto low = lower(*c.store.current(), lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  const LoweredProject& lp = *low;
  CHECK(lp.missingDevices.empty());
  uint32_t ti = 0;
  while (lp.trackIds[ti] != c.midi) ++ti;
  CHECK(lp.graph.tracks[ti].role == pq::graph::TrackRole::Instrument);
  auto ids = [](const pq::graph::Patch& patch) {
    std::vector<uint32_t> v;
    for (const pq::graph::Node& n : patch.nodes()) v.push_back(n.id.value);
    std::sort(v.begin(), v.end());
    return v;
  };
  // Params: a value block's control is a runtime param keyed by its block.
  auto param = [](const LoweredProject& l, DeviceId d) -> const LoweredParam* {
    for (const LoweredParam& p : l.params)
      if (p.ref == ParamRef::device(d, std::string(kValueParam))) return &p;
    return nullptr;
  };
  REQUIRE(param(lp, drive) != nullptr);
  CHECK(param(lp, drive)->key.rfind("dev" + std::to_string(drive.value) + ":", 0) == 0);
  REQUIRE(param(lp, gain) != nullptr);
  CHECK(lowerParams(*c.store.current(), false, &library().library()).size() == lp.params.size());

  // A param change is not a graph change: the patch is shared with the previous lowering.
  c.edit(ops::setDeviceParam(*c.store.current(), gain, kv, 0.5));
  auto again = lower(*c.store.current(), lo, &lp);
  REQUIRE(again.ok());
  for (size_t i = 0; i < lp.patches.size(); ++i) CHECK((again->patches[i] == lp.patches[i]));
  // Reorder (TX.all-free: along the main path): only that track's patch changes, and every node keeps its NodeId.
  c.edit(ops::moveOnPath(*c.store.current(), c.store.ids(), &library(), sat, c.midi, 1));
  auto moved = lower(*c.store.current(), lo, &*again);
  REQUIRE(moved.ok());
  for (size_t i = 0; i < lp.patches.size(); ++i) CHECK((moved->patches[i] == again->patches[i]) == (i != ti));
  CHECK(ids(moved->patches[ti]->patch) == ids(again->patches[ti]->patch));
  // GU.11: a device whose patch is not a block cannot be added (so none is ever bypassed as missing).
  CHECK_FALSE(c.store.dispatch(AddDevice{c.group, makeDevice(c.store.ids().device(), "no_such_device", DeviceKind::AudioEffect), 0}).ok());
}

// KNOWN BUG (GU.11, reported to the lead; expected to fail until fixed — then drop the decorator): deactivating a
// GROUP must leave out everything inside it (model.h Device::active: "left out of the graph"), as deactivating the
// stock devices did. Today the lowering still lowers a switched-off group's blocks (their params are listed), and a
// switched-off instrument group whose modules are groups still gives the track voices (instrument_groups.cpp only skips
// the switched-off group itself; the groups inside it are fed notes and become instruments).
TEST_CASE("devices: a deactivated group leaves out everything inside it; a MIDI track without an active instrument "
          "group plays nothing" * doctest::should_fail()) {
  Chains c;
  const std::string kv(kValueParam);
  const DeviceId util = test_blocks::addModule(c.store, c.midi, "utility");
  const DeviceId gain = test_blocks::member(*c.store.current(), util, "Gain");
  REQUIRE(gain.valid());
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  uint32_t slot = 0;
  lo.slotOf = [&](TrackId) { return slot++; };
  auto on = lower(*c.store.current(), lo);
  REQUIRE(on.ok());
  uint32_t ti = 0;
  while (on->trackIds[ti] != c.midi) ++ti;
  auto param = [&](const LoweredProject& l, DeviceId d) {
    return std::any_of(l.params.begin(), l.params.end(), [&](const LoweredParam& p) { return p.ref == ParamRef::device(d, kv); });
  };
  REQUIRE(param(*on, gain));
  c.edit(ops::setDevicesActive(*c.store.current(), std::vector<DeviceId>{util}, false));
  auto off = lower(*c.store.current(), lo, &*on);
  REQUIRE(off.ok());
  CHECK_FALSE(param(*off, gain));
  // Without its instrument group (IG.1-2: the voice context) the MIDI track plays nothing: no voices, no event slot.
  c.edit(ops::setDevicesActive(*c.store.current(), std::vector<DeviceId>{c.inst}, false));
  auto silent = lower(*c.store.current(), lo, &*off);
  REQUIRE(silent.ok());
  CHECK(silent->graph.tracks[ti].role == pq::graph::TrackRole::Audio);
  CHECK(silent->graph.tracks[ti].eventSlot == pq::graph::kNone);
}

// KNOWN BUG (GU.11, reported to the lead; expected to fail until fixed — then drop the decorator): a group has no params,
// no settings but its polyphony and rack-chain ones, and no meta-module state (checks.cpp checkList, which the reader
// applies), but the store's SetDeviceParam / SetDeviceSetting / AddMetaInstance accept them on a group.
TEST_CASE("devices: the store refuses params, foreign settings and meta state on a group" * doctest::should_fail()) {
  Chains c;
  const Snapshot s0 = c.store.current();
  CHECK_FALSE(c.store.dispatch(SetDeviceParam{c.inst, "level", 0.5}).ok());
  CHECK_FALSE(c.store.dispatch(SetDeviceSetting{c.inst, "zeta", 1.0}).ok());
  CHECK_FALSE(c.store.dispatch(AddMetaInstance{c.inst, "acc1", defaultMetaInstance(pq::meta::envelopeType())}).ok());
  CHECK((c.store.current() == s0));
}
