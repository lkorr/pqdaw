// GU.2: block sets — copy / paste of a subgraph, group presets (pq/document/edit_ops.h "block sets").
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "edit_common.h"

namespace pq::doc {

bool operator==(const BlockSet& a, const BlockSet& b) noexcept {
  if (a.devices.size() != b.devices.size() || a.wires != b.wires) return false;
  for (size_t i = 0; i < a.devices.size(); ++i)
    if (!(*a.devices[i] == *b.devices[i])) return false;
  return true;
}

}  // namespace pq::doc

namespace pq::doc::ops {

using namespace detail;

namespace {

// The depth of `d` inside the groups of `list` (0: a top-level block of it). Bounded by kMaxGroupDepth.
uint32_t depthIn(const std::vector<DevicePtr>& list, const Device& d) {
  uint32_t depth = 0;
  const Device* at = &d;
  while (at->group.valid() && depth < kMaxGroupDepth) {
    const auto g = std::find_if(list.begin(), list.end(), [&](const DevicePtr& x) { return x->id == at->group; });
    if (g == list.end()) break;
    at = g->get();
    ++depth;
  }
  return depth;
}

}  // namespace

Result<BlockSet> copyBlocks(const Project& p, std::span<const DeviceId> devices) {
  BlockSet out;
  if (devices.empty()) return out;
  const Track* track = nullptr;
  std::unordered_set<uint32_t> picked;
  for (const DeviceId id : devices) {
    const DeviceLoc l = findDevice(p, id);
    if (l.device == nullptr) return notFound("Copy: device not found");
    if (l.chain != nullptr) return invalid("Copy: a device inside a rack is copied with its rack");
    if (track != nullptr && l.track != track) return invalid("Copy: the blocks are on different tracks");
    track = l.track;
    picked.insert(id.value);
    if (isGroupDevice(*l.device))
      for (const Device* m : groupMembersDeep(track->devices, id)) picked.insert(m->id.value);
  }
  // The set's top-level blocks first, then what is inside them, each in list order; a block whose group is not in the
  // set is a top-level block of the set (its tag cleared).
  std::unordered_set<uint32_t> inside;  // every device id of the set, racks' inner devices included
  for (const bool top : {true, false})
    for (const DevicePtr& d : track->devices) {
      if (picked.count(d->id.value) == 0) continue;
      const bool isTop = !d->group.valid() || picked.count(d->group.value) == 0;
      if (isTop != top) continue;
      if (d->group.valid() && isTop) {
        auto c = std::make_shared<Device>(*d);
        c->group = {};
        out.devices.push_back(std::move(c));
      } else {
        out.devices.push_back(d);
      }
      forEachDeviceIn(*d, [&](const Device& x) { inside.insert(x.id.value); });
    }
  for (const Wire& w : wiresOf(p)) {
    if (w.fromNoteSource() || w.toMixer()) continue;  // (a track's own ends: not part of a block set)
    if (inside.count(w.from.value) != 0 && inside.count(w.to.value) != 0) out.wires.push_back(w);
  }
  return out;
}

// GU.3: a panel's bindings (DeviceId) are rewritten here with the device ids — the one place a set's ids change.
BlockSet remapBlocks(const BlockSet& set, IdSource& ids, std::vector<std::pair<DeviceId, DeviceId>>* remapOut) {
  BlockSet out;
  std::vector<std::pair<DeviceId, DeviceId>> remap;
  for (const DevicePtr& d : set.devices) out.devices.push_back(cloneDevice(*d, ids, &remap));
  std::unordered_map<uint32_t, DeviceId> to;
  for (const auto& [a, b] : remap) to.emplace(a.value, b);
  auto mapped = [&](DeviceId id) {
    const auto it = to.find(id.value);
    return it != to.end() ? it->second : DeviceId{};
  };
  for (DevicePtr& d : out.devices)
    if (d->group.valid() || d->panel) {
      auto c = std::make_shared<Device>(*d);
      if (d->group.valid()) c->group = mapped(d->group);  // (a group outside the set: none — copyBlocks never leaves one)
      // GU.3: the panel's bindings follow the copies (a binding to a block outside the set keeps its id: apply's final
      // pass takes it off the pasted panel, which binds only what is inside its group).
      if (d->panel) c->panel = std::make_shared<const Panel>(remapPanel(*d->panel, remap));
      d = std::move(c);
    }
  for (const Wire& w : set.wires) {
    const DeviceId a = mapped(w.from), b = mapped(w.to);
    if (!a.valid() || !b.valid()) continue;
    Wire c = w;
    c.id = ids.wire();
    c.from = a;
    c.to = b;
    out.wires.push_back(std::move(c));
  }
  if (remapOut != nullptr) remapOut->insert(remapOut->end(), remap.begin(), remap.end());
  return out;
}

EditResult pasteBlocks(const Project& p, IdSource& ids, TrackId track, const BlockSet& set, DeviceId intoGroup) {
  const Track* t = findTrackPtr(p, track);
  if (t == nullptr) return notFound("Paste: track not found");
  if (set.empty()) return invalid("Paste: nothing to paste");
  if (intoGroup.valid()) {
    const int32_t gi = t->deviceIndex(intoGroup);
    if (gi < 0 || !isGroupDevice(*t->devices[static_cast<size_t>(gi)]))
      return invalid("Paste: the capsule to paste into is not a group of this track");
  }
  BlockSet fresh = remapBlocks(set, ids);
  for (DevicePtr& d : fresh.devices) {
    if (d->kind == DeviceKind::Instrument && !isMetaDevice(*d) && t->kind != TrackKind::Midi)
      return invalid("Paste: instruments go on MIDI tracks (the notes are their main input)");
    if (!d->group.valid() && intoGroup.valid()) {
      auto c = std::make_shared<Device>(*d);
      c->group = intoGroup;
      d = std::move(c);
    }
  }
  // Groups before their members (each AddDevice is checked against the list so far): by depth inside the set.
  std::vector<std::pair<uint32_t, size_t>> order;
  for (size_t i = 0; i < fresh.devices.size(); ++i) order.emplace_back(depthIn(fresh.devices, *fresh.devices[i]), i);
  std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  Edit e = makeEdit(fresh.devices.size() == 1 ? "Paste Block" : "Paste Blocks");
  Batch& b = batchOf(e);
  for (const auto& [depth, i] : order) {
    if (depth == 0) e.devices.push_back(fresh.devices[i]->id);
    b.commands.push_back(AddDevice{track, fresh.devices[i], kAppend, {}});
  }
  for (const auto& [depth, i] : order)
    if (depth != 0) e.devices.push_back(fresh.devices[i]->id);
  for (Wire& w : fresh.wires) {
    e.wires.push_back(w.id);
    b.commands.push_back(AddWire{std::move(w)});
  }
  e.tracks.push_back(track);
  return e;
}

}  // namespace pq::doc::ops
