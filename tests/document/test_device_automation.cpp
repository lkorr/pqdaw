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
#include "pq/document/modules.h"
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

// Two MIDI tracks: the first with its instrument, an LFO (a Wavetable engine off the path), a control (a value block
// standing for a filter's frequency) and a Utility (GU.11: the module's group preset); the LFO's `out` wired to the
// control (a Travel wire); the second with its instrument.
struct Auto {
  DocumentStore store;
  TrackId a, b;
  DeviceId inst, lfo, filter, util;
  WireId wire;
  Auto() {
    a = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    b = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    inst = track(a).devices.at(0)->id;
    lfo = edit(ops::addMetaDevice(doc(), store.ids(), a, "wavetable", ops::kOffPath)).devices.at(0);
    filter = add(a, std::string(kValueDevice));
    util = add(a, std::string(kModuleItemPrefix) + "utility");
    wire = edit(ops::connectTravelWire(doc(), store.ids(), lfo, "out", filter, std::string(kValueParam), 0.25)).wires.at(0);
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
  DeviceId add(TrackId t, std::string patch) {
    return edit(ops::addDevice(doc(), store.ids(), t, patch, DeviceKind::AudioEffect, kAppend)).devices.at(0);
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

TEST_CASE("device automation: deleting a device removes its lanes (and its wires' lanes) in one undo step") {
  Auto f;
  const LaneId freq = f.lane(f.a, ParamRef::device(f.filter, std::string(kValueParam)), std::string(kValueParam), 0.2f, 0.8f);
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
