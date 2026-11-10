// PF.6: the lowering after a sequence of edits, given the previous lowering (what the project patch view and the playback
// link pass), equals a fresh lowering of the same snapshot, and the tracks an edit did not touch keep their patch
// objects. The project is a small version of bench/document/bench_lower.cpp's heavy fixture: MIDI tracks with module
// chains (Saturator, VCA, Utility, EQ: value blocks lowered as literals, mux choices), Operator instruments, groups, a
// return fed by sends, and an LFO on its own track modulating devices on other tracks (cross-track wires).

#include <doctest/doctest.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/store.h"

using namespace pq::doc;

namespace {

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

struct Built {
  DocumentStore store;
  std::vector<TrackId> tracks;
  std::vector<DeviceId> effects;  // the first effect of each track
  DeviceId lfo;
  TrackId ret;

  std::optional<ops::Edit> run(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  DeviceId device(TrackId t, const char* patch, DeviceKind kind, uint32_t index = kAppend) {
    auto e = run(ops::addDevice(*store.current(), store.ids(), t, patch, kind, index));
    REQUIRE(!e->devices.empty());
    return e->devices[0];
  }
  DeviceId member(DeviceId group, std::string_view name) {
    const DeviceLoc l = findDevice(*store.current(), group);
    REQUIRE(l.track != nullptr);
    for (const Device* d : groupMembersDeep(l.track->devices, group))
      if (d->name == name) return d->id;
    FAIL("no member " << name);
    return {};
  }

  Built() {
    store.setDeviceLibrary(&library());
    static const char* const fx[] = {"module:saturator", "module:vca", "module:eq", "module:utility"};
    TrackId after;
    for (uint32_t i = 0; i < 6; ++i) {
      after = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, after, "T" + std::to_string(i)))->tracks.at(0);
      tracks.push_back(after);
      if (i % 2 == 1) (void)device(after, "module:operator", DeviceKind::Instrument);
      for (uint32_t k = 0; k < 4; ++k) {
        const DeviceId d = device(after, fx[(i + k) % 4], DeviceKind::AudioEffect);
        if (k == 0) effects.push_back(d);
      }
    }
    const TrackId two[2] = {tracks[0], tracks[1]};
    const TrackId g = run(ops::groupTracks(*store.current(), store.ids(), two, "Bus"))->tracks.at(0);
    (void)device(g, "module:utility", DeviceKind::AudioEffect);
    ret = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Return, {}, "Space"))->tracks.at(0);
    (void)device(ret, "module:saturator", DeviceKind::AudioEffect);
    for (const TrackId t : tracks) REQUIRE(store.dispatch(SetSendLevel{t, ret, -12.0f}).ok());
    const TrackId mod = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Mod"))->tracks.at(0);
    lfo = device(mod, "module:lfo", DeviceKind::AudioEffect, ops::kOffPath);
    // The Saturators' Drive on two other tracks (effects 0 and 4: fx[0]): cross-track modulation.
    for (const DeviceId e : {effects[0], effects[4]}) {
      (void)run(ops::connectWire(*store.current(), store.ids(), lfo, "y", member(e, "Drive"), std::string(kValueParam)));
    }
  }
};

LowerOptions options() {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  return lo;
}

// Everything a lowering hands its users, compared field by field.
void checkSame(const LoweredProject& a, const LoweredProject& b) {
  REQUIRE(a.patches.size() == b.patches.size());
  for (size_t i = 0; i < a.patches.size(); ++i) {
    CHECK(a.trackIds[i] == b.trackIds[i]);
    CHECK(a.patches[i]->hash == b.patches[i]->hash);
    CHECK(a.patches[i]->patch == b.patches[i]->patch);
    const pq::graph::ProjectTrack &x = a.graph.tracks[i], &y = b.graph.tracks[i];
    CHECK(x.id == y.id);
    CHECK(x.role == y.role);
    CHECK(x.eventSlot == y.eventSlot);
    CHECK(x.maxVoices == y.maxVoices);
    CHECK(x.steal == y.steal);
  }
  REQUIRE(a.graph.wires.size() == b.graph.wires.size());
  for (size_t i = 0; i < a.graph.wires.size(); ++i) {
    CHECK(a.graph.wires[i].from == b.graph.wires[i].from);
    CHECK(a.graph.wires[i].to == b.graph.wires[i].to);
    CHECK(a.graph.wires[i].kind == b.graph.wires[i].kind);
  }
  CHECK(a.graph.taps == b.graph.taps);
  REQUIRE(a.params.size() == b.params.size());
  for (size_t i = 0; i < a.params.size(); ++i) {
    CHECK(a.params[i].key == b.params[i].key);
    CHECK(a.params[i].track == b.params[i].track);
    CHECK(a.params[i].name == b.params[i].name);
    CHECK(a.params[i].value == b.params[i].value);
    CHECK(a.params[i].position == b.params[i].position);
  }
  CHECK(a.master == b.master);
  CHECK(a.wires == b.wires);
  REQUIRE(a.taps.size() == b.taps.size());
  for (size_t i = 0; i < a.taps.size(); ++i) {
    CHECK(a.taps[i].device == b.taps[i].device);
    CHECK(a.taps[i].param == b.taps[i].param);
    CHECK(a.taps[i].port == b.taps[i].port);
  }
  REQUIRE(a.curves.size() == b.curves.size());
  for (size_t i = 0; i < a.curves.size(); ++i) {
    CHECK(a.curves[i].device == b.curves[i].device);
    CHECK(a.curves[i].param == b.curves[i].param);
    CHECK(a.curves[i].slot == b.curves[i].slot);
    CHECK(a.curves[i].value == b.curves[i].value);
  }
  CHECK(a.missingDevices == b.missingDevices);
  CHECK(a.deviceErrors.size() == b.deviceErrors.size());
  CHECK(a.metaResources.size() == b.metaResources.size());
}

}  // namespace

TEST_CASE("lowering (PF.6): after each edit, the lowering given the previous one equals a fresh lowering; untouched "
          "tracks keep their patch objects") {
  Built p;
  const LowerOptions lo = options();
  auto first = lower(*p.store.current(), lo);
  REQUIRE_MESSAGE(first.ok(), (first.ok() ? std::string() : first.error().message));
  CHECK(first->missingDevices.empty());
  CHECK(first->deviceErrors.empty());
  CHECK(first->taps.size() >= 2);  // the LFO's two cross-track targets are modulated
  auto prev = std::make_shared<const LoweredProject>(std::move(*first));

  // The edits: (name, the track it touches (none: several), the edit).
  struct Step {
    const char* name;
    std::optional<TrackId> only;
    std::function<void()> apply;
  };
  const DeviceId eq = p.effects[2];  // (track 2's first effect: fx[2])
  const std::vector<Step> steps = {
      {"a device off", p.tracks[3],
       [&] { p.run(ops::setDevicesActive(*p.store.current(), std::vector<DeviceId>{p.effects[3]}, false)); }},
      {"the device on again", p.tracks[3],
       [&] { p.run(ops::setDevicesActive(*p.store.current(), std::vector<DeviceId>{p.effects[3]}, true)); }},
      {"an EQ module off (its literals and choices)", std::nullopt,
       [&] { p.run(ops::setDevicesActive(*p.store.current(), std::vector<DeviceId>{eq}, false)); }},
      {"an effect added", p.tracks[5],
       [&] { (void)p.device(p.tracks[5], "module:vca", DeviceKind::AudioEffect); }},
      {"another cross-track wire (into a new Saturator)", std::nullopt,
       [&] {
         const DeviceId sat = p.device(p.tracks[2], "module:saturator", DeviceKind::AudioEffect);
         p.run(ops::connectWire(*p.store.current(), p.store.ids(), p.lfo, "y", p.member(sat, "Drive"), std::string(kValueParam)));
       }},
      {"a send level", std::nullopt, [&] { REQUIRE(p.store.dispatch(SetSendLevel{p.tracks[2], p.ret, -3.0f}).ok()); }},
      {"an effect deleted", std::nullopt,
       [&] { p.run(ops::deleteDevices(*p.store.current(), std::vector<DeviceId>{p.effects[1]})); }},
  };
  for (const Step& s : steps) {
    CAPTURE(s.name);
    s.apply();
    const Snapshot snap = p.store.current();
    auto inc = lower(*snap, lo, prev.get());
    auto fresh = lower(*snap, lo);
    REQUIRE(inc.ok());
    REQUIRE(fresh.ok());
    checkSame(*inc, *fresh);
    if (s.only) {  // one track's edit: every other track's patch is the previous object (its unit is not recompiled)
      uint32_t kept = 0, changed = 0;
      for (size_t i = 0; i < inc->patches.size(); ++i) {
        std::shared_ptr<const LoweredPatch> before;
        for (size_t j = 0; j < prev->trackIds.size(); ++j)
          if (prev->trackIds[j] == inc->trackIds[i]) before = prev->patches[j];
        if (inc->patches[i] == before) ++kept;
        else ++changed;
        if (inc->trackIds[i] != *s.only) CHECK(inc->patches[i] == before);
      }
      CHECK(changed == 1);
      CHECK(kept + 1 == inc->patches.size());
    }
    prev = std::make_shared<const LoweredProject>(std::move(*inc));
  }
}
