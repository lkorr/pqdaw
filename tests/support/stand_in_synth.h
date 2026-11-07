#pragma once

// GU.10 test support (docs/plans/GU.md "One hierarchy"): a new MIDI track's default instrument is a group preset
// (pq/document/modules.h defaultInstrumentBlocks: its instrument group holds In → Phase Accumulator → Wavetable →
// Envelope → Out). The tests written before GU.10 build on the IG.1-2 layout instead — the instrument group holding ONE
// Basic Synth (now a stand-in device, tests/stand_in_devices) wired In → synth → Out by chain links, listed
// [synth, group]. useBasicSynth(store, track) turns a new track's default instrument into exactly that (one undo step:
// the group keeps its id, its members are replaced), and returns the synth's id. A track without the default instrument
// (no top-level group named kDefaultInstrumentName): nothing changes, invalid id.

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"
#include "pq/document/store.h"

namespace stand_in {

inline pq::doc::Batch basicSynthBatch(const pq::doc::Project& p, pq::doc::IdSource& ids, pq::doc::TrackId track,
                                      pq::doc::DeviceId* synthOut = nullptr) {
  using namespace pq::doc;
  Batch b{"Basic Synth", {}};
  const Track* tr = findTrackPtr(p, track);
  if (tr == nullptr) return b;
  DeviceId g;
  for (const DevicePtr& d : tr->devices)
    if (isGroupDevice(*d) && !d->group.valid() && d->name == kDefaultInstrumentName) g = d->id;
  if (!g.valid()) return b;  // (no default instrument: a loaded or hand-made group is left alone)
  {  // (members before the groups they sit in)
    const std::vector<const Device*> inside = groupMembersDeep(tr->devices, g);
    for (auto it = inside.rbegin(); it != inside.rend(); ++it) b.commands.push_back(RemoveDevice{(*it)->id});
  }
  const DeviceId synth = ids.device();
  auto d = std::make_shared<Device>(*makeDefaultInstrument(synth));
  d->group = g;
  b.commands.push_back(AddDevice{track, std::move(d), 0});
  for (const auto& [from, port, to, param] : {std::tuple{g, kGroupInPort, synth, kMainPort}, std::tuple{synth, kMainPort, g, kGroupOutPort}}) {
    Wire w;
    w.id = ids.wire();
    w.from = from;
    w.port = std::string(port);
    w.to = to;
    w.param = std::string(param);
    w.chain = true;
    b.commands.push_back(AddWire{w});
  }
  if (synthOut != nullptr) *synthOut = synth;
  return b;
}

inline pq::doc::DeviceId useBasicSynth(pq::doc::DocumentStore& s, pq::doc::TrackId track) {
  pq::doc::DeviceId synth;
  pq::doc::Batch b = basicSynthBatch(*s.current(), s.ids(), track, &synth);
  if (b.commands.empty() || !s.dispatch(b).ok()) return {};
  return synth;
}

}  // namespace stand_in
