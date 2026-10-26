// T4.1: device chains in the document — commands (chain rule, ChangeSet classes, no-ops), the edit builders (one undo
// step each), the device library and catalog, and the lowering of chains (stable NodeIds, params, settings).

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/store.h"
#include "pq/graph/parse.h"

using namespace pq::doc;
using pq::Result;

namespace {

std::string stdlibDir() { return std::string(PQ_TEST_DATA_DIR) + "/../stdlib"; }

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(stdlibDir());
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// A store with one MIDI track (default instrument), one group and one return.
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
    inst = track(midi).devices.at(0)->id;
  }
  const Track& track(TrackId id) const { return *findTrackPtr(*store.current(), id); }
  std::vector<std::string> chain(TrackId id) const {
    std::vector<std::string> v;
    for (const DevicePtr& d : track(id).devices) v.push_back(d->patch + (d->active ? "" : "(off)"));
    return v;
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
  CHECK(c.chain(c.midi) == std::vector<std::string>{"BasicSynth"});
  CHECK(c.track(c.midi).devices[0]->kind == DeviceKind::Instrument);
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
  SUBCASE("AddDevice: structural; instruments only first on MIDI tracks") {
    ChangeSet cs;
    const DevicePtr fx = makeDevice(ids.device(), "utility", DeviceKind::AudioEffect);
    auto r = apply1(s0, AddDevice{c.midi, fx, kAppend}, cs);
    REQUIRE(r.ok());
    CHECK(cs.structural());
    CHECK(cs.tracks == std::vector<TrackId>{c.midi});
    CHECK(cs.devices == std::vector<DeviceId>{fx->id});
    ChangeSet e1, e2, e3, e4;
    CHECK_FALSE(apply1(s0, AddDevice{c.midi, makeDevice(ids.device(), "utility", DeviceKind::AudioEffect), 0}, e1).ok());
    CHECK_FALSE(apply1(s0, AddDevice{c.group, makeDefaultInstrument(ids.device()), 0}, e2).ok());
    CHECK_FALSE(apply1(s0, AddDevice{c.midi, makeDefaultInstrument(ids.device()), kAppend}, e3).ok());
    CHECK_FALSE(apply1(s0, AddDevice{c.ret, nullptr, 0}, e4).ok());  // no device
    ChangeSet e5;
    CHECK_FALSE(apply1(*r, AddDevice{c.ret, fx, 0}, e5).ok());  // the id is in use
  }
  SUBCASE("SetDeviceParam is a Param change with the device's ParamRef; no-ops return the same snapshot") {
    ChangeSet cs;
    auto r = apply1(s0, SetDeviceParam{c.inst, "level", 0.5}, cs);
    REQUIRE(r.ok());
    CHECK(cs.paramOnly());
    REQUIRE(cs.params.size() == 1);
    CHECK(cs.params[0] == ParamRef::device(c.inst, "level"));
    CHECK(cs.params[0].kind == ParamKind::DeviceParam);
    CHECK_FALSE(isAutomatable(ParamKind::DeviceParam));
    const Device& d = *findDevice(**r, c.inst).device;
    REQUIRE(d.param("level") != nullptr);
    CHECK(d.param("level")->value == 0.5);
    ChangeSet same;
    auto again = apply1(*r, SetDeviceParam{c.inst, "level", 0.5}, same);
    CHECK((again->get() == r->get()));
    CHECK(same.empty());
    ChangeSet reset;
    auto back = apply1(*r, SetDeviceParam{c.inst, "level", std::nullopt}, reset);
    REQUIRE(back.ok());
    CHECK(findDevice(**back, c.inst).device->params.empty());
    ChangeSet bad;
    CHECK_FALSE(apply1(s0, SetDeviceParam{c.inst, "lev el", 1.0}, bad).ok());
    CHECK_FALSE(apply1(s0, SetDeviceParam{c.inst, "level", std::numeric_limits<double>::infinity()}, bad).ok());
    CHECK_FALSE(apply1(s0, SetDeviceParam{DeviceId{999999}, "level", 1.0}, bad).ok());
  }
  SUBCASE("settings and activation are structural; values stay sorted") {
    ChangeSet cs;
    auto r = apply1(s0, SetDeviceSetting{c.inst, "zeta", 1.0}, cs);
    REQUIRE(r.ok());
    CHECK(cs.structural());
    ChangeSet cs2;
    r = apply1(*r, SetDeviceSetting{c.inst, "alpha", 2.0}, cs2);
    REQUIRE(r.ok());
    const Device& d = *findDevice(**r, c.inst).device;
    REQUIRE(d.settings.size() == 2);
    CHECK(d.settings[0].name == "alpha");
    CHECK(d.settings[1].name == "zeta");
    ChangeSet cs3;
    auto off = apply1(*r, SetDeviceActive{c.inst, false}, cs3);
    REQUIRE(off.ok());
    CHECK(cs3.structural());
    CHECK_FALSE(findDevice(**off, c.inst).device->active);
  }
  SUBCASE("MoveDevice: within a track and across tracks; the chain rule holds") {
    Batch b{"fx", {}};
    const DevicePtr a = makeDevice(ids.device(), "utility", DeviceKind::AudioEffect);
    const DevicePtr s = makeDevice(ids.device(), "saturator", DeviceKind::AudioEffect);
    b.commands = {AddDevice{c.midi, a, kAppend}, AddDevice{c.midi, s, kAppend}};
    ChangeSet cs0;
    auto r = apply(s0, Command{b}, cs0);
    REQUIRE(r.ok());
    ChangeSet cs;
    auto m = apply1(*r, MoveDevice{s->id, {}, 1}, cs);
    REQUIRE(m.ok());
    CHECK(cs.tracks == std::vector<TrackId>{c.midi});
    const Track& t = *findTrackPtr(**m, c.midi);
    CHECK(t.devices[1]->id == s->id);
    CHECK(t.devices[2]->id == a->id);
    CHECK((t.devices[1] == s));  // moved, not copied: pointer-equal
    ChangeSet bad;
    CHECK_FALSE(apply1(*m, MoveDevice{s->id, {}, 0}, bad).ok());  // before the instrument
    CHECK_FALSE(apply1(*m, MoveDevice{c.inst, c.group, 0}, bad).ok());  // an instrument onto a group
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
  // Add (the device menu): effects append after the instrument.
  const DeviceId utility = c.edit(ops::addDevice(*c.store.current(), c.store.ids(), c.midi, "utility", DeviceKind::AudioEffect)).devices.at(0);
  const DeviceId sat = c.edit(ops::addDevice(*c.store.current(), c.store.ids(), c.midi, "saturator", DeviceKind::AudioEffect, 1)).devices.at(0);
  CHECK(c.chain(c.midi) == std::vector<std::string>{"BasicSynth", "saturator", "utility"});
  // An instrument replaces the track's instrument (Live).
  const DeviceId synth2 = c.edit(ops::addDevice(*c.store.current(), c.store.ids(), c.midi, "BasicSynth", DeviceKind::Instrument)).devices.at(0);
  CHECK(c.track(c.midi).devices[0]->id == synth2);
  CHECK(c.chain(c.midi).size() == 3);
  CHECK_FALSE(ops::addDevice(*c.store.current(), c.store.ids(), c.group, "BasicSynth", DeviceKind::Instrument).ok());
  // Reorder.
  c.edit(ops::moveDevice(*c.store.current(), utility, {}, 1));
  CHECK(c.chain(c.midi) == std::vector<std::string>{"BasicSynth", "utility", "saturator"});
  const auto noop = ops::moveDevice(*c.store.current(), utility, {}, 1);
  REQUIRE(noop.ok());
  CHECK(noop->empty());
  // Deactivate (toggle), param, setting.
  c.edit(ops::toggleDevicesActive(*c.store.current(), std::vector<DeviceId>{sat}));
  CHECK(c.chain(c.midi) == std::vector<std::string>{"BasicSynth", "utility", "saturator(off)"});
  c.edit(ops::setDeviceParam(*c.store.current(), sat, "drive", 2.0));
  c.edit(ops::setDeviceSetting(*c.store.current(), sat, "type", 6.0));
  // Copy / paste onto the group (fresh ids, the same values), then duplicate there.
  std::vector<DevicePtr> clip{c.track(c.midi).devices[2]};
  REQUIRE(clip[0]->id == sat);
  const ops::Edit pasted = c.edit(ops::pasteDevices(*c.store.current(), c.store.ids(), c.group, clip));
  REQUIRE(pasted.devices.size() == 1);
  const Device& copy = *findDevice(*c.store.current(), pasted.devices[0]).device;
  CHECK(copy.id != sat);
  CHECK(copy.patch == "saturator");
  CHECK_FALSE(copy.active);
  CHECK(copy.param("drive")->value == 2.0);
  CHECK(copy.setting("type")->value == 6.0);
  c.edit(ops::duplicateDevices(*c.store.current(), c.store.ids(), pasted.devices));
  CHECK(c.chain(c.group) == std::vector<std::string>{"saturator(off)", "saturator(off)"});
  // Pasting an instrument onto a group is refused; onto a MIDI track it replaces the instrument.
  std::vector<DevicePtr> instClip{c.track(c.midi).devices[0]};
  CHECK_FALSE(ops::pasteDevices(*c.store.current(), c.store.ids(), c.group, instClip).ok());
  // Remove.
  c.edit(ops::deleteDevices(*c.store.current(), std::vector<DeviceId>{utility, sat}));
  CHECK(c.chain(c.midi) == std::vector<std::string>{"BasicSynth"});
  // Undo walks every step back to the start.
  while (c.store.history().cursor() > 3) REQUIRE(c.store.undo());
  CHECK(*c.store.current() == *p);
}

TEST_CASE("devices: the stock device library and catalog") {
  const DeviceLibrary& lib = library();
  CHECK(lib.missing().empty());
  const DeviceCatalog& cat = lib.catalog();
  REQUIRE(cat.devices().size() == stockDevices().size());
  const DeviceInfo* synth = cat.find("BasicSynth");
  REQUIRE(synth != nullptr);
  CHECK(synth->kind == DeviceKind::Instrument);
  CHECK(synth->name == "Basic Synth");
  REQUIRE(synth->param("attack") != nullptr);
  CHECK(synth->param("attack")->defaultValue == doctest::Approx(0.005));  // 5ms in seconds
  CHECK(synth->param("attack")->unit == pq::graph::Unit::ms);
  CHECK(synth->params.size() == 6);
  const DeviceInfo* op = cat.find("operator");  // T4.12: a stereo instrument
  REQUIRE(op != nullptr);
  CHECK(op->kind == DeviceKind::Instrument);
  CHECK(op->name == "Operator");
  CHECK(op->outWidth == 2);
  REQUIRE(op->setting("algorithm") != nullptr);
  CHECK(op->setting("algorithm")->defaultValue == 1.0);
  CHECK(op->setting("onA")->defaultValue == 1.0);
  CHECK(op->param("levelA")->unit == pq::graph::Unit::dB);
  const DeviceInfo* util = cat.find("utility");
  REQUIRE(util != nullptr);
  CHECK(util->kind == DeviceKind::AudioEffect);
  CHECK(util->inWidth == 2);
  CHECK(util->outWidth == 2);
  REQUIRE(util->param("chan") != nullptr);
  CHECK(util->param("chan")->steps == 4);
  CHECK(util->param("chan")->label == "Channel Mode");
  CHECK(util->param("mute")->kind == pq::graph::Kind::Bool);
  CHECK(util->param("gain")->unit == pq::graph::Unit::dB);
  CHECK(util->param("gain")->defaultValue == doctest::Approx(1.0));  // 0 dB as a linear gain
  CHECK(util->param("bassFreq")->exponential);
  REQUIRE(util->setting("bassMono") != nullptr);
  CHECK(util->setting("bassMono")->hasDefault);
  const DeviceInfo* eq = cat.find("eq_eight");
  REQUIRE(eq != nullptr);
  CHECK(eq->settings.size() == 34);
  CHECK(eq->setting("type8")->defaultValue == 7.0);
  CHECK(eq->params.size() == 51);
  // The value a device plays: set, else the default.
  Device d = *makeDevice(DeviceId{5}, "utility", DeviceKind::AudioEffect);
  CHECK(deviceParamValue(d, *util->param("width")) == doctest::Approx(1.0));
  d.params = {{"width", 2.0}};
  CHECK(deviceParamValue(d, *util->param("width")) == 2.0);
  // A patch that is not a device is refused.
  auto notDevice = pq::graph::parse("patch p {\n  in a : real const = 1\n}\n", "p.pqp");
  REQUIRE(notDevice.ok());
  CHECK_FALSE(describeDevice(notDevice->patches[0], "P").ok());
  // A stdlib directory without a device file leaves it out.
  auto none = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/document");
  CHECK_FALSE(none.ok());
}

TEST_CASE("devices: lowering a chain — stable NodeIds, params as runtime params, settings as literals") {
  Chains c;
  const ops::Edit u = c.edit(ops::addDevice(*c.store.current(), c.store.ids(), c.midi, "utility", DeviceKind::AudioEffect));
  const ops::Edit s = c.edit(ops::addDevice(*c.store.current(), c.store.ids(), c.midi, "saturator", DeviceKind::AudioEffect));
  const DeviceId util = u.devices[0], sat = s.devices[0];
  c.edit(ops::setDeviceParam(*c.store.current(), sat, "drive", 2.0));
  c.edit(ops::setDeviceSetting(*c.store.current(), sat, "type", 6.0));
  LowerOptions lo;
  lo.library = &library().library();
  uint32_t slot = 0;
  lo.slotOf = [&](TrackId) { return slot++; };
  auto low = lower(*c.store.current(), lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  const LoweredProject& lp = *low;
  CHECK(lp.missingDevices.empty());
  uint32_t ti = 0;
  while (lp.trackIds[ti] != c.midi) ++ti;
  const pq::graph::Patch& patch = lp.patches[ti]->patch;
  CHECK(lp.graph.tracks[ti].role == pq::graph::TrackRole::Instrument);
  auto callOf = [&](DeviceId d) -> std::optional<pq::graph::NodeIndex> {
    return patch.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Device, d.value, 0)});
  };
  for (const DeviceId d : {c.inst, util, sat}) {
    const auto n = callOf(d);
    REQUIRE(n.has_value());
    CHECK(patch.node(*n).kind == pq::graph::NodeKind::Subcircuit);
    CHECK(patch.nodeName(*n) == "d" + std::to_string(d.value));
  }
  CHECK(patch.subcircuit(*callOf(sat)).patch == "saturator");
  // The mono voice sum meets Utility's stereo input through pack(x, x) (k = 1).
  CHECK(patch.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Device, util.value, 1)}).has_value());
  CHECK_FALSE(patch.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Device, sat.value, 1)}).has_value());
  // Params: one per callee param, keyed by device; the set value, else the default.
  auto param = [&](const std::string& key) -> const LoweredParam* {
    for (const LoweredParam& p : lp.params)
      if (p.key == key) return &p;
    return nullptr;
  };
  const std::string ks = "dev" + std::to_string(sat.value) + ":";
  REQUIRE(param(ks + "drive") != nullptr);
  CHECK(param(ks + "drive")->value == 2.0f);
  CHECK(param(ks + "drive")->name == "d" + std::to_string(sat.value) + "__drive");
  CHECK(param(ks + "drive")->ref == ParamRef::device(sat, "drive"));
  CHECK(param(ks + "output")->value == doctest::Approx(1.0f));
  CHECK(param("dev" + std::to_string(c.inst.value) + ":level")->value == doctest::Approx(0.2f));
  CHECK(lowerParams(*c.store.current(), false, &library().library()).size() == lp.params.size());
  // The setting is a literal wired into the saturator's `type`.
  const pq::graph::NodeIndex satCall = *callOf(sat);
  const auto typeIn = patch.findInput(satCall, "type");
  REQUIRE(typeIn.has_value());
  bool wired = false;
  for (const pq::graph::Wire& w : patch.wires())
    if (w.to.node == satCall && w.to.port == *typeIn) {
      wired = patch.node(w.from.node).kind == pq::graph::NodeKind::Const && patch.constant(w.from.node).value() == 6.0;
    }
  CHECK(wired);

  // A param change is not a graph change: the patch is shared with the previous lowering.
  c.edit(ops::setDeviceParam(*c.store.current(), util, "width", 0.5));
  auto again = lower(*c.store.current(), lo, &lp);
  REQUIRE(again.ok());
  for (size_t i = 0; i < lp.patches.size(); ++i) CHECK((again->patches[i] == lp.patches[i]));
  // Reorder: only that track's patch changes, and every device keeps its NodeId (its state).
  c.edit(ops::moveDevice(*c.store.current(), sat, {}, 1));
  auto moved = lower(*c.store.current(), lo, &*again);
  REQUIRE(moved.ok());
  for (size_t i = 0; i < lp.patches.size(); ++i) CHECK((moved->patches[i] == again->patches[i]) == (i != ti));
  const pq::graph::Patch& mp = moved->patches[ti]->patch;
  for (const DeviceId d : {c.inst, util, sat})
    CHECK(mp.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Device, d.value, 0)}).has_value());
  // Deactivated: left out (no call, no params).
  c.edit(ops::setDevicesActive(*c.store.current(), std::vector<DeviceId>{util}, false));
  auto off = lower(*c.store.current(), lo, &*moved);
  REQUIRE(off.ok());
  CHECK_FALSE(off->patches[ti]->patch.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Device, util.value, 0)}).has_value());
  CHECK(std::none_of(off->params.begin(), off->params.end(),
                     [&](const LoweredParam& p) { return p.key.rfind("dev" + std::to_string(util.value) + ":", 0) == 0; }));
  // Without its instrument the MIDI track plays nothing: no voices, no event slot.
  c.edit(ops::setDevicesActive(*c.store.current(), std::vector<DeviceId>{c.inst}, false));
  auto silent = lower(*c.store.current(), lo, &*off);
  REQUIRE(silent.ok());
  CHECK(silent->graph.tracks[ti].role == pq::graph::TrackRole::Audio);
  CHECK(silent->graph.tracks[ti].eventSlot == pq::graph::kNone);
  // A device whose patch is not in the library is bypassed and reported.
  c.edit(ops::addDevice(*c.store.current(), c.store.ids(), c.group, "no_such_device", DeviceKind::AudioEffect));
  auto missing = lower(*c.store.current(), lo, &*silent);
  REQUIRE(missing.ok());
  CHECK(missing->missingDevices.size() == 1);
}
