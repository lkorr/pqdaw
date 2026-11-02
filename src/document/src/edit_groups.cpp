// Group edit builders (TX.groups; model.h "Groups"): pq/document/edit_ops.h, "Groups".
#include <algorithm>
#include <optional>

#include "edit_common.h"
#include "pq/document/devices.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

// A device's place in a series: the input its chain feeds and the output it gives on (names; "" = none).
struct ChainPorts {
  std::string in, out;
};

ChainPorts chainPortsOf(const Device& d, const DeviceLibrary& lib) {
  ChainPorts c;
  if (isMetaDevice(d)) {
    const meta::Type* t = meta::findType(metaDeviceType(d));
    if (t == nullptr) return c;
    const int32_t i = metaChainInput(d, *t);
    if (i >= 0) c.in = std::string(t->inputs[static_cast<size_t>(i)].name);
    if (!t->outputs.empty()) c.out = std::string(t->outputs.front().name);
    return c;
  }
  std::shared_ptr<const ResolvedDevice> hold;
  const graph::Patch* p = lib.patchOf(d, hold);
  if (p == nullptr) return c;
  // The lowering's rule (lower.cpp effectCall): the first non-const `in`, the first `out`.
  for (graph::NodeIndex v = 0; v < p->nodeCount(); ++v) {
    const graph::Node& n = p->node(v);
    if (n.kind == graph::NodeKind::Input && c.in.empty() && p->outputType(v, 0).rate != graph::Rate::Const)
      c.in = p->nodeName(v);
    else if (n.kind == graph::NodeKind::Output && c.out.empty())
      c.out = p->nodeName(v);
  }
  return c;
}

std::string displayName(const Device& d, const DeviceLibrary& lib) {
  if (!d.name.empty()) return d.name;
  std::shared_ptr<const ResolvedDevice> hold;
  if (const DeviceInfo* i = lib.infoOf(d, hold)) return i->name;
  return d.patch;
}

Wire addedWire(IdSource& ids, DeviceId from, std::string port, DeviceId to, std::string param) {
  Wire w;
  w.id = ids.wire();
  w.from = from;
  w.port = std::move(port);
  w.to = to;
  w.param = std::move(param);
  w.law = WireLaw::Add;
  return w;
}

// The wire states of `p` after `e` (the edit applied to a scratch snapshot), or an error when the edit does not apply.
Result<std::vector<WireState>> statesAfter(const Snapshot& p, const Edit& e, const DeviceLibrary& lib) {
  ChangeSet cs;
  Result<Snapshot> next = apply(p, e.command, cs);
  if (!next) return std::move(next).error();
  return wireStates(**next, lib);
}

}  // namespace

EditResult groupInCircuit(const Snapshot& snap, IdSource& ids, const DeviceLibrary& lib, std::span<const DeviceId> devices) {
  const Project& p = *snap;
  if (devices.empty()) return invalid("Group: nothing selected");
  const Track* track = nullptr;
  std::vector<const Device*> picked;
  for (const DeviceId id : devices) {
    const DeviceLoc l = findDevice(p, id);
    if (!l.track) return notFound("Group: device not found");
    if (l.chain != nullptr) return invalid("Group: a device inside a rack stays in its rack");
    if (track != nullptr && l.track != track) return invalid("Group: the devices are on different tracks (a group is one track's)");
    track = l.track;
    if (std::find(picked.begin(), picked.end(), l.device) != picked.end()) continue;
    if (isGroupDevice(*l.device)) return invalid("Group: groups do not nest (a group cannot go into a group)");
    if (l.device->group.valid()) return invalid("Group: " + displayName(*l.device, lib) + " is in a group already");
    if (l.device->rack) return invalid("Group: a rack cannot go into a group (its chains are a series; a group has none)");
    if (l.device->kind == DeviceKind::Instrument && !isMetaDevice(*l.device))
      return invalid("Group: a group is an audio effect; leave the instrument out");
    picked.push_back(l.device);
  }
  const std::vector<DevicePtr>& list = track->devices;
  // The selected series devices in list order (S) and the selected free ones (F).
  std::vector<const Device*> series, freeOnes;
  std::vector<size_t> seriesAt;  // their indices among the track's series devices
  size_t firstIndex = list.size();
  {
    size_t si = 0;
    for (size_t i = 0; i < list.size(); ++i) {
      const Device* d = list[i].get();
      const bool sel = std::find(picked.begin(), picked.end(), d) != picked.end();
      if (sel) firstIndex = std::min(firstIndex, i);
      if (!d->free) {
        if (sel) {
          series.push_back(d);
          seriesAt.push_back(si);
        }
        ++si;
      } else if (sel) {
        freeOnes.push_back(d);
      }
    }
  }
  for (size_t k = 1; k < seriesAt.size(); ++k)
    if (seriesAt[k] != seriesAt[k - 1] + 1)
      return invalid("Group: the chain devices must be next to each other (a group takes their place in the chain)");
  // Where the group goes: the first series device's place (or, a free group, the first selected device's).
  size_t groupIndex = firstIndex;
  if (!series.empty())
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i].get() == series.front()) groupIndex = i;
  std::vector<ChainPorts> ports;
  if (!series.empty()) {
    const std::vector<ChainStage> stages = chainStages(list, track->kind == TrackKind::Midi);
    for (const ChainStage& st : stages)
      if (st.device == series.front() && isMetaDevice(*st.device) &&
          (st.in == ChainSignal::Notes || st.in == ChainSignal::Voices))
        return invalid(std::string("Group: ") +
                       (st.in == ChainSignal::Notes ? "the notes reach " : "voices reach ") + displayName(*st.device, lib) +
                       "; a group is an audio effect (what reaches it is audio): group the devices after it");
    for (size_t k = 0; k < series.size(); ++k) {
      const Device& d = *series[k];
      if (!d.active)
        return invalid("Group: " + displayName(d, lib) + " is switched off (in the chain it passes the signal by); "
                       "switch it on or leave it out");
      ChainPorts c = chainPortsOf(d, lib);
      if (k > 0 && c.in.empty())
        return invalid("Group: " + displayName(d, lib) + " takes no chain signal (unplugged); plug it in or leave it out");
      if (c.out.empty()) return invalid("Group: " + displayName(d, lib) + " has no output to pass on");
      ports.push_back(std::move(c));
    }
  }
  Edit e = makeEdit("Group");
  Batch& b = batchOf(e);
  const DeviceId gid = ids.device();
  {
    auto g = std::make_shared<Device>(*makeGroup(gid));
    g->free = series.empty();  // only free devices: the group is free too (it has no place in the chain)
    b.commands.push_back(AddDevice{track->id, std::move(g), static_cast<uint32_t>(groupIndex)});
  }
  for (const Device* d : series) b.commands.push_back(SetDeviceFree{d->id, true});
  for (const Device* d : picked) b.commands.push_back(SetDeviceGroup{d->id, gid});
  // The series as wires: In → the first's chain input, each output → the next's chain input, the last's → Out.
  std::vector<Wire> spine;
  for (size_t k = 0; k < series.size(); ++k) {
    if (!ports[k].in.empty()) {
      if (k == 0) spine.push_back(addedWire(ids, gid, std::string(kGroupInPort), series[k]->id, ports[k].in));
      else spine.push_back(addedWire(ids, series[k - 1]->id, ports[k - 1].out, series[k]->id, ports[k].in));
    }
  }
  if (!series.empty()) spine.push_back(addedWire(ids, series.back()->id, ports.back().out, gid, std::string(kGroupOutPort)));
  for (const Wire& w : spine) {
    for (const Wire& x : wiresOf(p))
      if (x.sameEnds(w))
        return invalid("Group: a wire already joins those ports beside the chain (it would play once instead of twice); "
                       "delete it first");
    b.commands.push_back(AddWire{w});
    e.wires.push_back(w.id);
  }
  e.devices.push_back(gid);
  e.tracks.push_back(track->id);
  // The sound must not change: every wire that played before plays the same way after (a loop through the grouped
  // series keeps its 1-sample delay on the same wire), and the series' new wires carry no delay.
  const std::vector<WireState> before = wireStates(p, lib);
  Result<std::vector<WireState>> after = statesAfter(snap, e, lib);
  if (!after) return std::move(after).error();
  for (const WireState& s : *after) {
    const auto old = std::find_if(before.begin(), before.end(), [&](const WireState& x) { return x.id == s.id; });
    const bool fresh = old == before.end();
    if ((fresh && s.status != WireStatus::Lowered) || (!fresh && old->status != s.status))
      return invalid("Group: a feedback loop runs through these devices; grouping would move its 1-sample delay "
                     "(QUESTIONS Q?-TX.groups-a)");
  }
  return e;
}

EditResult ungroupInCircuit(const Snapshot& snap, const DeviceLibrary& lib, DeviceId group) {
  const Project& p = *snap;
  const DeviceLoc l = findDevice(p, group);
  if (!l.track) return notFound("Ungroup: group not found");
  if (!isGroupDevice(*l.device)) return invalid("Ungroup: not a group");
  const Track& track = *l.track;
  const std::vector<const Device*> members = groupMembers(track.devices, group);
  auto isMember = [&](DeviceId id) {
    return std::any_of(members.begin(), members.end(), [&](const Device* m) { return m->id == id; });
  };
  auto memberOf = [&](DeviceId id) -> const Device* {
    for (const Device* m : members)
      if (m->id == id) return m;
    return nullptr;
  };
  const WireList& wires = wiresOf(p);
  // The series: from the one wire into Out back along each device's chain input to In.
  std::vector<const Wire*> intoOut, fromIn;
  for (const Wire& w : wires) {
    if (w.to == group && w.param == kGroupOutPort) intoOut.push_back(&w);
    if (w.from == group && w.port == kGroupInPort && !w.fromNoteSource()) fromIn.push_back(&w);
    if (w.to == group && w.param != kGroupOutPort)
      return invalid("Ungroup: a wire reaches the group's own input; delete it first (after ungrouping there is no group)");
    if (w.from == group && w.port != kGroupInPort && !w.fromNoteSource())
      return invalid("Ungroup: a wire leaves the group's own output; delete it first (after ungrouping there is no group)");
  }
  if (l.device->free && (!intoOut.empty() || !fromIn.empty()))
    return invalid("Ungroup: this group is free (not in the chain): delete its In / Out wires first");
  std::vector<const Device*> series;   // Out first
  std::vector<const Wire*> spine;
  if (!l.device->free) {
    if (intoOut.size() != 1)
      return invalid(intoOut.empty() ? "Ungroup: nothing reaches Out (the group passes no audio): wire Out first"
                                     : "Ungroup: several wires reach Out; a chain is one series: keep one");
    const Wire* at = intoOut.front();
    bool reachedIn = false;
    while (true) {
      spine.push_back(at);
      if (at->from == group && at->port == kGroupInPort) {
        reachedIn = true;
        break;
      }
      const Device* d = memberOf(at->from);
      if (d == nullptr || at->fromNoteSource())
        return invalid("Ungroup: the series to Out starts outside the group; it must run from In through the group's devices");
      if (std::find(series.begin(), series.end(), d) != series.end())
        return invalid("Ungroup: the series to Out runs in a loop");
      const ChainPorts c = chainPortsOf(*d, lib);
      if (at->port != c.out)
        return invalid("Ungroup: " + displayName(*d, lib) + " reaches the next device from another output than its main one");
      series.push_back(d);
      if (c.in.empty()) break;  // it takes no chain signal: the head (In feeds nothing)
      const Wire* prev = nullptr;
      size_t count = 0;
      for (const Wire& w : wires)
        if (w.to == d->id && w.param == c.in && (w.fromNoteSource() || w.from == group || isMember(w.from))) {
          prev = &w;
          ++count;
        }
      if (count != 1 || prev->law != WireLaw::Add)
        return invalid("Ungroup: " + displayName(*d, lib) + "'s input is not reached by exactly one wire from In or a device "
                       "of the group; a chain is one series");
      at = prev;
    }
    for (const Wire* w : fromIn)
      if (std::find(spine.begin(), spine.end(), w) == spine.end())
        return invalid("Ungroup: In also feeds another input; in the chain only the first device hears the signal: delete "
                       "that wire first");
    if (!reachedIn && !fromIn.empty())
      return invalid("Ungroup: In feeds a device off the series; delete that wire first");
  }
  std::reverse(series.begin(), series.end());  // chain order
  Edit e = makeEdit("Ungroup");
  Batch& b = batchOf(e);
  for (const Wire* w : spine) b.commands.push_back(RemoveWire{w->id});
  for (const Device* m : members) b.commands.push_back(SetDeviceGroup{m->id, DeviceId{}});
  b.commands.push_back(RemoveDevice{group});
  for (const Device* d : series) b.commands.push_back(SetDeviceFree{d->id, false});
  // The series at the group's place, in its order (a device already there does not move: MoveDevice is a no-op).
  uint32_t at = 0;
  for (size_t i = 0; i < track.devices.size(); ++i)
    if (track.devices[i]->id == group) at = static_cast<uint32_t>(i);
  for (size_t k = 0; k < series.size(); ++k)
    b.commands.push_back(MoveDevice{series[k]->id, TrackId{}, at + static_cast<uint32_t>(k)});
  for (const Device* m : members) e.devices.push_back(m->id);
  e.tracks.push_back(track.id);
  return e;
}

}  // namespace pq::doc::ops
