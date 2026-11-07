#pragma once

// GU.11 test support (docs/plans/GU.md "One hierarchy"): the model has blocks, groups and native engines only, so tests
// build what they play from the built-in modules (pq/document/modules.h: Utility, Saturator, LFO, Operator, the Basic
// Synth default instrument, the Wavetable Synth, the Phase Accumulator, VCA, Wavetable, Envelope, analysers):
//   addModule        a module's group preset onto a track's main path at a slot (ops::addDevice "module:<key>"), or
//                    off it (ops::kOffPath); returns the group's id
//   member           a block inside a group (any depth) by its name (Device::name: the module's value blocks are named
//                    after their controls — Utility's "Gain", Saturator's "Drive" …)
//   defaultInstrumentCommands / addDefaultInstrument
//                    the default instrument group's blocks and inner wires as commands for a track (the group first,
//                    then its members: each AddDevice is checked against the list so far), unwired to the track (a
//                    builder wires the path: ops::wireSeries); what ops::createTrack gives a new MIDI track
//   addEngineInstrument
//                    a lone native engine as a MIDI track's instrument, in an instrument group at the notes (the
//                    track's path cleared first): a small instrument with no knob curves (old-schema relabel tests:
//                    a schema-19 file stores no curve objects); `plainLinks`: its In → engine → Out links written as
//                    plain main-port wires instead of Ctrl+G's chain links into the engine's named ports (a schema-12
//                    file has no chain links: every wire off a main port had a Travel amount)
//   appendAfter / appendEngineAfter
//                    a block (a native engine) added off the path, then spliced in at the path's end: after an
//                    instrument group, not into it (ops::addMetaDevice appends a native engine to the group's voice run)

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"
#include "pq/document/store.h"

namespace test_blocks {

// Dispatches ops::addDevice(kModuleItemPrefix + key) on `s` (one undo step); the new group's id.
inline pq::doc::DeviceId addModule(pq::doc::DocumentStore& s, pq::doc::TrackId track, std::string_view key,
                                   uint32_t slot = pq::doc::kAppend) {
  using namespace pq::doc;
  const ModuleInfo* m = findModule(key);
  REQUIRE_MESSAGE(m != nullptr, std::string(key));
  auto e = ops::addDevice(*s.current(), s.ids(), track, std::string(kModuleItemPrefix) + std::string(m->key),
                          m->instrument ? DeviceKind::Instrument : DeviceKind::AudioEffect, slot);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
  REQUIRE_FALSE(e->devices.empty());
  return e->devices.front();
}

// The block named `name` inside group `group` of `p` (at any depth; the first in list order), or an invalid id.
inline pq::doc::DeviceId member(const pq::doc::Project& p, pq::doc::DeviceId group, std::string_view name) {
  using namespace pq::doc;
  const DeviceLoc l = findDevice(p, group);
  if (l.track == nullptr) return {};
  for (const Device* d : groupMembersDeep(l.track->devices, group))
    if (d->name == name) return d->id;
  return {};
}

// The default instrument (modules.h defaultInstrumentBlocks) as commands adding it to `track`: the group (its id `id`,
// fresh when invalid) first, then its blocks, then the wires among them. Not wired to the track.
inline pq::doc::DeviceId defaultInstrumentCommands(pq::doc::Batch& b, pq::doc::IdSource& ids, pq::doc::TrackId track,
                                                   pq::doc::DeviceId id = {}) {
  using namespace pq::doc;
  StockModuleOptions o;
  o.id = id.valid() ? id : ids.device();
  BlockSet set = defaultInstrumentBlocks(ids, o);
  for (DevicePtr& d : set.devices) b.commands.push_back(AddDevice{track, std::move(d), kAppend});
  for (Wire& w : set.wires) b.commands.push_back(AddWire{std::move(w)});
  return o.id;
}

// The same, dispatched on `s` (one undo step); the group's id.
inline pq::doc::DeviceId addDefaultInstrument(pq::doc::DocumentStore& s, pq::doc::TrackId track) {
  pq::doc::Batch b{"Default Instrument", {}};
  const pq::doc::DeviceId g = defaultInstrumentCommands(b, s.ids(), track);
  REQUIRE(s.dispatch(b).ok());
  return g;
}

// GU.11: a PROBE instrument (what the tests' probe stdlib's BasicSynth was): an instrument group whose Out is the atom
// block voice.hz, so a rendered track is the tuning node's pitch per sample (× the track's constant gains; the voices end
// by the 5 ms release after note-off). Pasted at MIDI track `track`'s notes (its path must be empty: clear it first);
// returns the group.
inline pq::doc::DeviceId addProbeInstrument(pq::doc::DocumentStore& s, pq::doc::TrackId track) {
  using namespace pq::doc;
  BlockSet set;
  auto g = std::make_shared<Device>(*makeInstrumentGroup(DeviceId{1}));
  g->name = "Probe";
  set.devices.push_back(g);
  auto hz = std::make_shared<Device>(*makeDevice(DeviceId{2}, std::string(kAtomDevicePrefix) + "voice.hz", DeviceKind::AudioEffect));
  hz->group = DeviceId{1};
  set.devices.push_back(hz);
  Wire w;
  w.id = WireId{1};
  w.from = DeviceId{2};
  w.port = "out";
  w.to = DeviceId{1};
  w.param = std::string(kGroupOutPort);
  set.wires.push_back(w);
  auto e = ops::pasteBlocksAtNotes(*s.current(), s.ids(), track, set);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
  return e->devices.front();
}

// Every MIDI track of `s` with an empty main path gets a probe instrument (one undo step per track).
inline void addProbeInstruments(pq::doc::DocumentStore& s) {
  using namespace pq::doc;
  std::vector<TrackId> midi;
  forEachTrack(*s.current(), [&](const Track& t, const Track*) {
    if (t.kind == TrackKind::Midi && mainPath(*s.current(), t).blocks.empty()) midi.push_back(t.id);
  });
  for (const TrackId t : midi) (void)addProbeInstrument(s, t);
}

inline void dispatchEdit(pq::doc::DocumentStore& s, const pq::doc::ops::EditResult& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  const pq::Status st = s.dispatch(e->command);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
}

// Clears `track`'s main path (deletes its blocks on it: a new MIDI track's default instrument group), adds a native
// engine of `type` at the notes and Ctrl+G's it into an instrument group; the group's id.
inline pq::doc::DeviceId addEngineInstrument(pq::doc::DocumentStore& s, pq::doc::TrackId track, std::string_view type,
                                             const pq::doc::DeviceLibrary& lib, bool plainLinks = false) {
  using namespace pq::doc;
  std::vector<DeviceId> on;
  for (const Device* d : mainPath(*s.current(), *findTrackPtr(*s.current(), track)).blocks) on.push_back(d->id);
  if (!on.empty()) dispatchEdit(s, ops::deleteDevices(*s.current(), on));
  auto e = ops::addMetaDevice(*s.current(), s.ids(), track, type, 0);
  dispatchEdit(s, e);
  const DeviceId engine[1] = {e->devices.at(0)};
  auto g = ops::groupInCircuit(s.current(), s.ids(), lib, engine);
  dispatchEdit(s, g);
  const DeviceId group = g->devices.at(0);
  if (plainLinks) {
    Batch b{"plain links", {}};
    for (const Wire& w : wiresOf(*s.current())) {
      if (!w.chain || (w.from != group && w.to != group)) continue;
      Wire x = w;
      x.chain = false;
      if (x.to == engine[0]) x.param = std::string(kMainPort);
      if (x.from == engine[0]) x.port = std::string(kMainPort);
      b.commands.push_back(RemoveWire{w.id});
      b.commands.push_back(AddWire{x});
    }
    const pq::Status st = s.dispatch(b);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  return group;
}

// Adds `patch` (a block, a "module:<key>") to `track` off its path, then inserts it at the path's end; its id.
inline pq::doc::DeviceId appendAfter(pq::doc::DocumentStore& s, pq::doc::TrackId track, std::string patch) {
  using namespace pq::doc;
  auto e = ops::addDevice(*s.current(), s.ids(), track, std::move(patch), DeviceKind::AudioEffect, ops::kOffPath);
  dispatchEdit(s, e);
  const DeviceId d = e->devices.at(0);
  dispatchEdit(s, ops::insertOnPath(*s.current(), s.ids(), d, kAppend));
  return d;
}

// The same for a native engine of `type`.
inline pq::doc::DeviceId appendEngineAfter(pq::doc::DocumentStore& s, pq::doc::TrackId track, std::string_view type) {
  using namespace pq::doc;
  auto e = ops::addMetaDevice(*s.current(), s.ids(), track, type, ops::kOffPath);
  dispatchEdit(s, e);
  const DeviceId d = e->devices.at(0);
  dispatchEdit(s, ops::insertOnPath(*s.current(), s.ids(), d, kAppend));
  return d;
}

}  // namespace test_blocks
