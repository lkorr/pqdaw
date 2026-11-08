#pragma once

// GU.11 test support: a built-in module's block set (pq/document/modules.h: utilityBlocks, saturatorBlocks, lfoBlocks,
// defaultInstrumentBlocks …, built with the group's id the test chose) pushed into a raw Batch as AddDevice commands (the
// group first, then its members) and AddWire commands (its inner wires), for the tests that build a project in one
// batch. The members name the group, so the series builders (ops::wireSeries, playback_fixture::wireSeries) leave them
// off the path and wire only the group. `at(name)`: the member block named `name` (a module's value blocks are named
// after its controls: Utility "Gain", "Width", "Balance"; Saturator "Drive"; LFO "Rate", "Shape", "Depth" …).

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <utility>

#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"

namespace module_batch {

struct Module {
  pq::doc::DeviceId id;  // the group
  pq::doc::BlockSet set;
  pq::doc::DeviceId at(std::string_view name) const {
    for (const pq::doc::DevicePtr& d : set.devices)
      if (d->id != id && d->name == name) return d->id;
    FAIL("no block named " << std::string(name));
    return {};
  }
};

inline Module push(pq::doc::Batch& b, pq::doc::TrackId t, pq::doc::BlockSet set) {
  using namespace pq::doc;
  REQUIRE_FALSE(set.devices.empty());
  for (const DevicePtr& d : set.devices) b.commands.push_back(AddDevice{t, d, kAppend});
  for (const Wire& w : set.wires) b.commands.push_back(AddWire{w});
  const DeviceId g = set.devices.front()->id;
  return Module{g, std::move(set)};
}

// The module of kind `k` (utility / saturator / lfo / operator) with group id `id`, pushed into `b` on track `t`.
enum class Kind { Utility, Saturator, Lfo };
inline Module push(pq::doc::Batch& b, pq::doc::IdSource& ids, pq::doc::TrackId t, Kind k, pq::doc::DeviceId id) {
  using namespace pq::doc;
  StockModuleOptions o;
  o.id = id;
  switch (k) {
    case Kind::Utility: return push(b, t, utilityBlocks(ids, o));
    case Kind::Saturator: return push(b, t, saturatorBlocks(ids, o));
    case Kind::Lfo: return push(b, t, lfoBlocks(ids, o));
  }
  return {};
}

// Whether `w` is a wire INSIDE a group (between two blocks of the same group, from its In terminal or into its Out
// terminal): a module's own wiring, not one a test draws between blocks.
inline bool innerWire(const pq::doc::Project& p, const pq::doc::Wire& w) {
  using namespace pq::doc;
  if (w.fromNoteSource() || w.toMixer()) return false;
  const Device* from = findDevice(p, w.from).device;
  const Device* to = findDevice(p, w.to).device;
  if (from == nullptr || to == nullptr) return false;
  if (from->group.valid() && from->group == to->group) return true;
  if (w.from == to->group && w.port == kGroupInPort) return true;
  return w.to == from->group && w.param == kGroupOutPort;
}

}  // namespace module_batch
