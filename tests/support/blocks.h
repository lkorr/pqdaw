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

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
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

}  // namespace test_blocks
