// T4.18: device automation in the document — lanes on device params, rack macros, chain volume / pan and wire amounts:
// where they may live, how they follow their devices (move, group, ungroup, hot-swap, delete; one undo step each), the
// project file (schema 9) and the lowering (a chain's mixer as volume / pan params while automated).

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
#include "pq/document/store.h"

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

// Two MIDI tracks: the first with its instrument, an LFO, an Auto Filter and a Utility (the LFO's jack wired to the
// filter's frequency); the second with its instrument.
struct Auto {
  DocumentStore store;
  TrackId a, b;
  DeviceId inst, lfo, filter, util;
  WireId wire;
  Auto() {
    a = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    b = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    inst = track(a).devices.at(0)->id;
    lfo = add(a, "lfo_device");
    filter = add(a, "auto_filter");
    util = add(a, "utility");
    wire = edit(ops::connectWire(doc(), store.ids(), lfo, "lfo", filter, "freq", 0.25)).wires.at(0);
  }
  const Project& doc() const { return *store.current(); }
  const Track& track(TrackId id) const { return *findTrackPtr(doc(), id); }
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    const pq::Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    CHECK(store.history().cursor() == before + (e->empty() ? 0u : 1u));  // one undo step
    return *e;
  }
  DeviceId add(TrackId t, const char* patch, ChainId chain = {}) {
    return edit(ops::addDevice(doc(), store.ids(), t, patch, DeviceKind::AudioEffect, kAppend, chain)).devices.at(0);
  }
  // A lane automating `r` on `t` (two points), as one undo step.
  LaneId lane(TrackId t, ParamRef r, std::string name, float v0, float v1) {
    auto l = std::make_shared<AutomationLane>();
    l->id = store.ids().lane();
    l->param = r;
    l->paramName = std::move(name);
    l->points = {{0, v0, 0.0f, store.ids().breakpoint()}, {pq::kPpq * 4, v1, 0.0f, store.ids().breakpoint()}};
    const size_t before = store.history().cursor();
    const pq::Status st = store.dispatch(AddAutomationLane{t, l});
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    CHECK(store.history().cursor() == before + 1);
    return l->id;
  }
  pq::Status tryLane(TrackId t, ParamRef r, std::string name) {
    auto l = std::make_shared<AutomationLane>();
    l->id = store.ids().lane();
    l->param = r;
    l->paramName = std::move(name);
    return store.dispatch(AddAutomationLane{t, l});
  }
  // The track holding lane `id` (invalid: none) and the lane.
  std::pair<TrackId, const AutomationLane*> where(LaneId id) const {
    std::pair<TrackId, const AutomationLane*> out{};
    forEachTrack(doc(), [&](const Track& t, const Track*) {
      for (const LanePtr& l : t.lanes)
        if (l->id == id) out = {t.id, l.get()};
    });
    return out;
  }
};

}  // namespace

TEST_CASE("device automation: which lanes a track may carry") {
  Auto f;
  CHECK(isAutomatable(ParamRef::device(f.filter, "freq")));
  CHECK(isAutomatable(ParamRef::wireAmount(f.wire)));
  CHECK(isAutomatable(ParamRef::chain(ChainId{1}, kChainVolume)));
  CHECK(isAutomatable(ParamRef::chain(ChainId{1}, kChainPan)));
  CHECK_FALSE(isAutomatable(ParamRef::chain(ChainId{1}, 0)));  // the mixer as a whole and the zones are not
  CHECK_FALSE(isAutomatable(ParamRef::chain(ChainId{1}, 1)));

  f.lane(f.a, ParamRef::device(f.filter, "freq"), "freq", 200.0f, 8000.0f);
  f.lane(f.a, ParamRef::wireAmount(f.wire), "", -0.5f, 0.5f);
  CHECK(laneHome(f.doc(), ParamRef::device(f.filter, "freq")) == &f.track(f.a));
  CHECK(laneHome(f.doc(), ParamRef::wireAmount(f.wire)) == &f.track(f.a));
  // Only on the device's own track, with the matching name, once in the project.
  CHECK_FALSE(f.tryLane(f.b, ParamRef::device(f.util, "gain"), "gain").ok());
  CHECK_FALSE(f.tryLane(f.a, ParamRef::device(f.util, "gain"), "").ok());
  CHECK_FALSE(f.tryLane(f.a, ParamRef::device(f.util, "gain"), "width").ok());
  CHECK_FALSE(f.tryLane(f.a, ParamRef::device(f.filter, "freq"), "freq").ok());
  CHECK_FALSE(f.tryLane(f.b, ParamRef::wireAmount(f.wire), "").ok());
  CHECK_FALSE(f.tryLane(f.a, ParamRef::track(ParamKind::TrackVolume, f.a), "volume").ok());  // only device lanes have a name
  CHECK(f.tryLane(f.a, ParamRef::device(f.util, "gain"), "gain").ok());

  // A rack: its macros and selector, its chains' volume / pan.
  const DeviceId rack = f.edit(ops::groupDevices(f.doc(), f.store.ids(), std::vector{f.util})).devices.at(0);
  const ChainId chain = findDevice(f.doc(), rack).device->rack->chains.at(0)->id;
  CHECK(f.tryLane(f.a, ParamRef::device(rack, "macro3"), "macro3").ok());
  CHECK(f.tryLane(f.a, ParamRef::device(rack, "select"), "select").ok());
  CHECK_FALSE(f.tryLane(f.a, ParamRef::device(rack, "gain"), "gain").ok());
  CHECK(f.tryLane(f.a, ParamRef::chain(chain, kChainVolume), "").ok());
  CHECK(f.tryLane(f.a, ParamRef::chain(chain, kChainPan), "").ok());
  CHECK_FALSE(f.tryLane(f.b, ParamRef::chain(chain, kChainPan), "").ok());
  CHECK_FALSE(f.tryLane(f.a, ParamRef::chain(chain, 0), "").ok());
  // Breakpoint values: a chain's volume in dB.
  auto l = std::make_shared<AutomationLane>();
  l->id = f.store.ids().lane();
  l->param = ParamRef::chain(ChainId{chain}, kChainVolume);
  l->points = {{0, 12.0f, 0.0f, f.store.ids().breakpoint()}};
  ChangeSet scratch;
  CHECK_FALSE(apply(f.store.current(), AddAutomationLane{f.a, l}, scratch).ok());
  CHECK(deviceParamNameOf(f.doc(), &library(), ParamRef::device(f.filter, "freq")) == "freq");
  CHECK(deviceParamNameOf(f.doc(), nullptr, ParamRef::device(rack, "macro3")) == "macro3");
  CHECK(deviceParamNameOf(f.doc(), &library(), ParamRef::device(f.filter, "nope")).empty());
}

TEST_CASE("device automation: lanes follow their device — move, group, ungroup, hot-swap (one undo step each)") {
  Auto f;
  const LaneId freq = f.lane(f.a, ParamRef::device(f.filter, "freq"), "freq", 200.0f, 8000.0f);
  const LaneId amount = f.lane(f.a, ParamRef::wireAmount(f.wire), "", -0.5f, 0.5f);
  const LaneId gain = f.lane(f.a, ParamRef::device(f.util, "gain"), "gain", 0.5f, 1.0f);
  const AutomationLane before = *f.where(freq).second;

  SUBCASE("move to another track: its lanes go with it, the wire's lane with the wire's target; undo brings them back") {
    f.edit(ops::moveDevice(f.doc(), f.filter, f.b, kAppend));
    CHECK(f.where(freq).first == f.b);
    CHECK(*f.where(freq).second == before);
    CHECK(f.where(amount).first == f.b);  // the wire's amount is a param of its target's track
    CHECK(f.where(gain).first == f.a);
    REQUIRE(f.store.undo());
    CHECK(f.where(freq).first == f.a);
    CHECK(f.where(amount).first == f.a);
    REQUIRE(f.store.redo());
    CHECK(f.where(freq).first == f.b);
  }
  SUBCASE("group into a rack, ungroup: the lanes stay; a moved rack takes its devices' and chains' lanes along") {
    const DeviceId rack = f.edit(ops::groupDevices(f.doc(), f.store.ids(), std::vector{f.filter, f.util})).devices.at(0);
    CHECK(f.where(freq).first == f.a);
    CHECK(*f.where(freq).second == before);
    CHECK(f.where(amount).first == f.a);
    CHECK(f.where(gain).first == f.a);
    const ChainId chain = findDevice(f.doc(), rack).device->rack->chains.at(0)->id;
    const LaneId vol = f.lane(f.a, ParamRef::chain(chain, kChainVolume), "", -6.0f, 0.0f);
    const LaneId macro = f.lane(f.a, ParamRef::device(rack, "macro1"), "macro1", 0.0f, 127.0f);
    f.edit(ops::moveDevice(f.doc(), rack, f.b, kAppend));
    for (const LaneId id : {freq, amount, gain, vol, macro}) CHECK(f.where(id).first == f.b);
    f.edit(ops::moveDevice(f.doc(), rack, f.a, kAppend));
    // Ungroup: the devices' lanes stay; the rack's own (macro, chain volume) go with the rack.
    f.edit(ops::ungroupDevices(f.doc(), rack));
    for (const LaneId id : {freq, amount, gain}) CHECK(f.where(id).first == f.a);
    CHECK_FALSE(f.where(vol).second);
    CHECK_FALSE(f.where(macro).second);
    REQUIRE(f.store.undo());
    CHECK(f.where(vol).first == f.a);
    CHECK(f.where(macro).first == f.a);
  }
  SUBCASE("hot-swap: lanes of params the new device still has stay, the others go") {
    Device with = *findDevice(f.doc(), f.util).device;  // a Utility preset without its gain (a stand-in device)
    with.params.clear();
    const std::vector<std::string> keeps{"width", "balance"};  // the new patch's params: no `gain`
    f.edit(ops::replaceDevice(f.doc(), f.store.ids(), f.util, with, keeps));
    CHECK_FALSE(f.where(gain).second);
    CHECK(f.where(freq).second);
    REQUIRE(f.store.undo());
    const std::vector<std::string> all{"gain", "width"};
    f.edit(ops::replaceDevice(f.doc(), f.store.ids(), f.util, with, all));
    CHECK(f.where(gain).first == f.a);
  }
}

TEST_CASE("device automation: deleting a device removes its lanes (and its wires' lanes) in one undo step") {
  Auto f;
  const LaneId freq = f.lane(f.a, ParamRef::device(f.filter, "freq"), "freq", 200.0f, 8000.0f);
  const LaneId amount = f.lane(f.a, ParamRef::wireAmount(f.wire), "", -0.5f, 0.5f);
  const LaneId vol = f.lane(f.a, ParamRef::track(ParamKind::TrackVolume, f.a), "", -6.0f, 0.0f);
  SUBCASE("the wire's source") {
    f.edit(ops::deleteDevices(f.doc(), std::vector{f.lfo}));
    CHECK(f.where(freq).first == f.a);
    CHECK_FALSE(f.where(amount).second);
  }
  SUBCASE("the filter") {
    ChangeSet cs;
    Result<Snapshot> r = apply(f.store.current(), RemoveDevice{f.filter}, cs);
    REQUIRE(r.ok());
    CHECK(cs.structural());
    CHECK(std::find(cs.lanes.begin(), cs.lanes.end(), freq) != cs.lanes.end());
    f.edit(ops::deleteDevices(f.doc(), std::vector{f.filter}));
    CHECK_FALSE(f.where(freq).second);
    CHECK_FALSE(f.where(amount).second);
    CHECK(f.where(vol).first == f.a);
    REQUIRE(f.store.undo());
    CHECK(f.where(freq).first == f.a);
    CHECK(f.where(amount).first == f.a);
  }
  SUBCASE("the whole track") {
    f.edit(ops::moveDevice(f.doc(), f.lfo, f.b, kAppend));  // the wire now crosses tracks; its lane stays on a
    CHECK(f.where(amount).first == f.a);
    f.edit(ops::deleteTracks(f.doc(), std::vector{f.b}));  // the source's track goes: the wire and its lane go
    CHECK_FALSE(f.where(amount).second);
    CHECK(f.where(freq).first == f.a);
  }
}

TEST_CASE("device automation: the project file keeps device, macro, chain and wire lanes (schema 9)") {
  Auto f;
  const DeviceId rack = f.edit(ops::groupDevices(f.doc(), f.store.ids(), std::vector{f.util})).devices.at(0);
  const ChainId chain = findDevice(f.doc(), rack).device->rack->chains.at(0)->id;
  f.lane(f.a, ParamRef::device(f.filter, "freq"), "freq", 200.0f, 8000.0f);
  f.lane(f.a, ParamRef::wireAmount(f.wire), "", -0.5f, 0.5f);
  f.lane(f.a, ParamRef::device(rack, "macro2"), "macro2", 0.0f, 127.0f);
  f.lane(f.a, ParamRef::chain(chain, kChainVolume), "", -12.0f, 0.0f);
  f.lane(f.a, ParamRef::chain(chain, kChainPan), "", -1.0f, 1.0f);
  Result<std::string> text = serializeProject(f.doc());
  REQUIRE(text.ok());
  CHECK(text->rfind("{\n  \"schemaVersion\": 9,", 0) == 0);
  CHECK(text->find("\"kind\": \"deviceParam\", \"owner\": " + std::to_string(f.filter.value) + ", \"name\": \"freq\"") !=
        std::string::npos);
  CHECK(text->find("\"kind\": \"chainMixer\", \"owner\": " + std::to_string(chain.value) + ", \"name\": \"pan\"") !=
        std::string::npos);
  CHECK(text->find("\"kind\": \"wireAmount\", \"owner\": " + std::to_string(f.wire.value) + "}") != std::string::npos);
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == f.doc());
  Result<std::string> again = serializeProject(*back->project);
  REQUIRE(again.ok());
  CHECK(*again == *text);

  // Strict reading: a name where none belongs, a missing name, an unknown chain value, a lane off its device's track.
  auto broken = [&](const std::string& from, const std::string& to) {
    std::string t = *text;
    const size_t at = t.find(from);
    REQUIRE(at != std::string::npos);
    t.replace(at, from.size(), to);
    return parseProject(t);
  };
  CHECK_FALSE(broken("\"name\": \"pan\"", "\"name\": \"width\"").ok());
  CHECK_FALSE(broken(", \"name\": \"freq\"", "").ok());
  CHECK(broken("\"name\": \"freq\"", "\"name\": \"fraq\"").ok());  // the library-free reader takes any identifier …
  CHECK_FALSE(broken("\"name\": \"freq\"", "\"name\": \"2x\"").ok());  // … but only an identifier
  CHECK_FALSE(broken("\"name\": \"macro2\"", "\"name\": \"gain\"").ok());  // a rack's own params only on a rack
}

TEST_CASE("device automation: lowering — an automated chain mixer lowers volume / pan params; lane keys") {
  Auto f;
  const DeviceId rack = f.edit(ops::groupDevices(f.doc(), f.store.ids(), std::vector{f.util})).devices.at(0);
  const ChainId chain = findDevice(f.doc(), rack).device->rack->chains.at(0)->id;
  f.edit(ops::setChainMixer(f.doc(), chain, -6.0f, 0.5f));
  auto keys = [&] {
    std::vector<std::string> k;
    for (const LoweredParam& p : lowerParams(f.doc(), true, library())) k.push_back(p.key);
    return k;
  };
  auto has = [](const std::vector<std::string>& v, const std::string& k) { return std::find(v.begin(), v.end(), k) != v.end(); };
  const std::string c = "chain" + std::to_string(chain.value) + ":";
  CHECK(has(keys(), c + "gl"));
  CHECK_FALSE(has(keys(), c + "volume"));
  const LaneId lane = f.lane(f.a, ParamRef::chain(chain, kChainPan), "", -1.0f, 1.0f);
  CHECK(has(keys(), c + "volume"));
  CHECK(has(keys(), c + "pan"));
  CHECK_FALSE(has(keys(), c + "gl"));
  for (const LoweredParam& p : chainParamValues(f.doc(), chain)) {
    if (p.key == c + "volume") CHECK(p.value == -6.0f);
    if (p.key == c + "pan") CHECK(p.value == 0.5f);
  }
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  Result<LoweredProject> lp = lower(f.doc(), o);
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  const LoweredPatch* tp = nullptr;
  for (size_t i = 0; i < lp->patches.size(); ++i)
    if (lp->trackIds[i] == f.a) tp = lp->patches[i].get();
  REQUIRE(tp != nullptr);
  const std::string text = patchText(*tp);
  CHECK(text.find("c" + std::to_string(chain.value) + "_volume") != std::string::npos);
  CHECK(text.find("db_to_gain") != std::string::npos);

  // Lane keys: what the link writes.
  AutomationLane dl;
  dl.param = ParamRef::device(f.filter, "freq");
  dl.paramName = "freq";
  CHECK(paramKey(dl) == "dev" + std::to_string(f.filter.value) + ":freq");
  CHECK(paramKey(ParamRef::wireAmount(f.wire)) == "wire" + std::to_string(f.wire.value) + ":amount");
  CHECK(paramKey(ParamRef::chain(chain, kChainVolume)) == c + "volume");
  CHECK(paramKey(ParamRef::chain(chain, 0)).empty());
  (void)lane;
}
