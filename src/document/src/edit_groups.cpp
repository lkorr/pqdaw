// Group edit builders (TX.groups; model.h "Groups"): pq/document/edit_ops.h, "Groups". TX.groups-fix1: groups where the
// notes or voices reach, switched-off and unplugged devices inside the series, the series' wires as chain links.
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

// One link of a series as a wire: (from, port) → (to, param).
struct Link {
  DeviceId from;
  std::string port;
  DeviceId to;
  std::string param;
  bool same(const Wire& w) const noexcept {
    return !w.fromNoteSource() && w.from == from && w.port == port && w.to == to && w.param == param;
  }
};

// The links a series `series` (chain order) makes inside group `gid` — Ctrl+G's rule, the chain's flow as wires: the
// signal so far starts at In; a device whose chain is plugged in takes it (a link into its chain input) and gives its
// main output on — switched off too: a switched-off free device passes its chain input there (lower.h "Free devices"),
// as the chain passes it by —; an unplugged device takes nothing and gives its output on (the signal so far ends there,
// as in the chain); the last signal goes to Out. An error names what has no such form: a switched-off device that is
// unplugged (the chain passes it by, but no wire into it could), a device with no output, and a device no link touches
// (nothing would say where it sits in the series).
Result<std::vector<Link>> seriesLinks(DeviceId gid, std::span<const Device* const> series, const DeviceLibrary& lib,
                                      const char* verb) {
  std::vector<Link> links;
  DeviceId at = gid;
  std::string port(kGroupInPort);
  for (const Device* d : series) {
    const ChainPorts c = chainPortsOf(*d, lib);
    if (c.out.empty())
      return Error{ErrorCode::InvalidArgument, std::string(verb) + displayName(*d, lib) + " has no output to pass on", {}};
    if (!c.in.empty()) {
      links.push_back(Link{at, port, d->id, c.in});
    } else if (!d->active) {
      return Error{ErrorCode::InvalidArgument,
                   std::string(verb) + displayName(*d, lib) +
                       " is switched off and unplugged: the chain passes it by, but no wire into it could carry the "
                       "series past it; plug it in, switch it on or leave it out",
                   {}};
    }
    at = d->id;
    port = c.out;
  }
  links.push_back(Link{at, port, gid, std::string(kGroupOutPort)});
  for (const Device* d : series)
    if (std::none_of(links.begin(), links.end(), [&](const Link& l) { return l.from == d->id || l.to == d->id; }))
      return Error{ErrorCode::InvalidArgument,
                   std::string(verb) + displayName(*d, lib) +
                       " takes no chain signal (unplugged) and its output goes nowhere (the next device is unplugged "
                       "too): nothing in a group would say where it sits in the series; leave it out",
                   {}};
  return links;
}

Wire chainWire(IdSource& ids, const Link& l) {
  Wire w;
  w.id = ids.wire();
  w.from = l.from;
  w.port = l.port;
  w.to = l.to;
  w.param = l.param;
  w.law = WireLaw::Add;
  w.chain = true;  // TX.groups-fix1: planned first and summed first, as the chain's own link (model.h "Modulation wires")
  return w;
}

// The wire states of `p` after `e` (the edit applied to a scratch snapshot), or an error when the edit does not apply.
Result<std::vector<WireState>> statesAfter(const Snapshot& p, const Edit& e, const DeviceLibrary& lib) {
  ChangeSet cs;
  Result<Snapshot> next = apply(p, e.command, cs);
  if (!next) return std::move(next).error();
  return wireStates(**next, lib);
}

// What the note source gives input `param` of `d` (a chain input at the note source; lower.h "Groups": the In
// terminal of a group there gives the same): its meaning, Signal for anything that reads none.
meta::Reads noteReads(const Device& d, std::string_view param) {
  if (!isMetaDevice(d)) return meta::Reads::Signal;
  const meta::Type* t = meta::findType(metaDeviceType(d));
  if (t == nullptr) return meta::Reads::Signal;
  const int32_t i = t->inputIndex(param);
  return i < 0 ? meta::Reads::Signal : t->inputs[static_cast<size_t>(i)].reads;
}

// Where the series goes back when group `g` (at index `gi` of `list`) is ungrouped: nothing moves when the series
// devices already sit where the group's place in the series is (every series device of the list before the group comes
// before them, every one after it after them: what Ctrl+G leaves, free devices between them included); else they are
// moved to the group's place, in order.
void moveSeriesBack(Batch& b, const std::vector<DevicePtr>& list, size_t gi, DeviceId g,
                    std::span<const Device* const> series, std::span<const Device* const> members) {
  if (series.empty()) return;
  auto indexOf = [&](const Device* d) {
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i].get() == d) return i;
    return list.size();
  };
  const size_t first = indexOf(series.front()), last = indexOf(series.back());
  bool inPlace = true;
  for (size_t i = 0; i < list.size() && inPlace; ++i) {
    const Device& d = *list[i];
    if (d.free || d.id == g || std::find(members.begin(), members.end(), &d) != members.end()) continue;
    if (i < gi && i > first) inPlace = false;
    if (i > gi && i < last) inPlace = false;
  }
  if (inPlace) return;
  for (size_t k = 0; k < series.size(); ++k)
    b.commands.push_back(MoveDevice{series[k]->id, TrackId{}, static_cast<uint32_t>(gi + k)});
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
      return invalid("Group: a stock instrument stays in the chain (its voices are summed at its output, which no wire "
                     "inside a group can reproduce); group the devices after it");
    picked.push_back(l.device);
  }
  const std::vector<DevicePtr>& list = track->devices;
  const bool midi = track->kind == TrackKind::Midi;
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
  // TX.groups-fix1: what reaches the group is what reached the first series device (meta.h chainStages). Notes or
  // voices: the group continues them (lower.h "Groups"), so the series must be exactly a stretch of the voice run —
  // meta modules that each continue the voices; the run's sum (before a stock device, at an unplugged device) cannot
  // be inside a group.
  const ChainSignal reaching =
      series.empty() ? ChainSignal::Audio : chainSignalAt(list, midi, static_cast<uint32_t>(groupIndex));
  const bool voiced = reaching == ChainSignal::Notes || reaching == ChainSignal::Voices;
  if (voiced) {
    const std::vector<ChainStage> stages = chainStages(list, midi);
    auto stageOf = [&](const Device* d) -> const ChainStage* {
      for (const ChainStage& s : stages)
        if (s.device == d) return &s;
      return nullptr;
    };
    bool seenActive = false;
    for (const Device* d : series) {
      if (!d->active) {
        if (!seenActive && reaching == ChainSignal::Notes)
          return invalid("Group: " + displayName(*d, lib) +
                         " is switched off where the notes arrive: the chain gives the notes to the device after it, "
                         "read by what that one's input means; switch it on or leave it out");
        continue;
      }
      const ChainStage* s = stageOf(d);
      const ChainSignal want = seenActive ? ChainSignal::Voices : reaching;
      if (!isMetaDevice(*d))
        return invalid("Group: the voices are summed before " + displayName(*d, lib) +
                       (reaching == ChainSignal::Notes && !seenActive ? " (the notes reach it, and it plays none)"
                                                                      : " (a stock device takes their sum)") +
                       ", which a group cannot do inside; group the meta modules before it, or the devices from it on");
      if (s == nullptr || s->in != want)
        return invalid("Group: " + displayName(*d, lib) + " is unplugged: " +
                       (seenActive ? "the voices end before it (the chain sums them there)"
                                   : "the notes reach nothing there (the track plays no voices)") +
                       ", which a group cannot do inside; plug it in or leave it out");
      seenActive = true;
    }
  }
  const DeviceId gid = ids.device();
  std::vector<Link> links;
  if (!series.empty()) {
    Result<std::vector<Link>> made = seriesLinks(gid, series, lib, "Group: ");
    if (!made) return std::move(made).error();
    links = std::move(*made);
  }
  Edit e = makeEdit("Group");
  Batch& b = batchOf(e);
  {
    auto g = std::make_shared<Device>(*makeGroup(gid));
    g->free = series.empty();  // only free devices: the group is free too (it has no place in the chain)
    b.commands.push_back(AddDevice{track->id, std::move(g), static_cast<uint32_t>(groupIndex)});
  }
  for (const Device* d : series) b.commands.push_back(SetDeviceFree{d->id, true});
  for (const Device* d : picked) b.commands.push_back(SetDeviceGroup{d->id, gid});
  // The series as chain links (Wire::chain): planned and summed before every other wire, as the chain's links are.
  std::vector<WireId> inactiveIn;  // In links at the note source into an input that reads no note signal
  for (const Link& l : links) {
    for (const Wire& x : wiresOf(p))
      if (l.same(x))
        return invalid("Group: a wire already joins those ports beside the chain (it would play once instead of twice); "
                       "delete it first");
    const Wire w = chainWire(ids, l);
    if (reaching == ChainSignal::Notes && l.from == gid) {
      const DeviceLoc t = findDevice(p, l.to);
      const meta::Reads r = t.device != nullptr ? noteReads(*t.device, l.param) : meta::Reads::Signal;
      if (r != meta::Reads::Frequency && r != meta::Reads::Phase) inactiveIn.push_back(w.id);
    }
    b.commands.push_back(AddWire{w});
    e.wires.push_back(w.id);
  }
  e.devices.push_back(gid);
  e.tracks.push_back(track->id);
  // The sound must not change: every wire that played before plays the same way after (a loop through the grouped
  // series keeps its 1-sample delay on the same wire: the chain links are planned first), and the series' new wires
  // carry no delay (an In link at the note source into an input that reads no note signal carries nothing, as the
  // chain gives that input nothing).
  const std::vector<WireState> before = wireStates(p, lib);
  Result<std::vector<WireState>> after = statesAfter(snap, e, lib);
  if (!after) return std::move(after).error();
  for (const WireState& s : *after) {
    const auto old = std::find_if(before.begin(), before.end(), [&](const WireState& x) { return x.id == s.id; });
    if (old == before.end()) {
      const bool quiet = std::find(inactiveIn.begin(), inactiveIn.end(), s.id) != inactiveIn.end();
      if (s.status != (quiet ? WireStatus::Inactive : WireStatus::Lowered))
        return invalid("Group: a feedback loop runs through these devices; grouping would move its 1-sample delay "
                       "(QUESTIONS Q?-TX.groups-a)");
      continue;
    }
    if (old->status == s.status) continue;
    if (old->status == WireStatus::Inactive && s.status == WireStatus::Lowered)
      return invalid("Group: a wire from a switched-off device's output would start to play (inside a group a "
                     "switched-off device passes its signal on); delete that wire or switch the device on first");
    return invalid("Group: a feedback loop runs through these devices; grouping would move its 1-sample delay "
                   "(QUESTIONS Q?-TX.groups-a)");
  }
  return e;
}

namespace {

// The legacy series read-back (groups made before TX.groups-fix1: no chain links): from the one wire into Out back
// along each device's chain input (exactly one wire from In or a member) to In.
Result<std::pair<std::vector<const Device*>, std::vector<const Wire*>>> readSeriesBack(
    const Project& p, const DeviceLibrary& lib, DeviceId group, std::span<const Device* const> members,
    std::span<const Wire* const> intoOut, std::span<const Wire* const> fromIn) {
  auto isMember = [&](DeviceId id) {
    return std::any_of(members.begin(), members.end(), [&](const Device* m) { return m->id == id; });
  };
  auto memberOf = [&](DeviceId id) -> const Device* {
    for (const Device* m : members)
      if (m->id == id) return m;
    return nullptr;
  };
  const WireList& wires = wiresOf(p);
  std::vector<const Device*> series;  // Out first
  std::vector<const Wire*> spine;
  if (intoOut.size() != 1)
    return Error{ErrorCode::InvalidArgument,
                 intoOut.empty() ? "Ungroup: nothing reaches Out (the group passes no audio): wire Out first"
                                 : "Ungroup: several wires reach Out; a chain is one series: keep one",
                 {}};
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
      return Error{ErrorCode::InvalidArgument,
                   "Ungroup: the series to Out starts outside the group; it must run from In through the group's devices", {}};
    if (std::find(series.begin(), series.end(), d) != series.end())
      return Error{ErrorCode::InvalidArgument, "Ungroup: the series to Out runs in a loop", {}};
    const ChainPorts c = chainPortsOf(*d, lib);
    if (at->port != c.out)
      return Error{ErrorCode::InvalidArgument,
                   "Ungroup: " + displayName(*d, lib) + " reaches the next device from another output than its main one", {}};
    series.push_back(d);
    if (c.in.empty()) break;  // it takes no chain signal: the head (In feeds nothing)
    const Wire* prev = nullptr;
    size_t count = 0;
    for (const Wire& w : wires)
      if (w.to == d->id && w.param == c.in && (w.fromNoteSource() || w.from == group || isMember(w.from))) {
        prev = &w;
        ++count;
      }
    if (count != 1 || prev->travels())  // TX.knob-curves: a migrated Units link sums into an input as an added one
      return Error{ErrorCode::InvalidArgument,
                   "Ungroup: " + displayName(*d, lib) + "'s input is not reached by exactly one wire from In or a device "
                   "of the group; a chain is one series",
                   {}};
    at = prev;
  }
  for (const Wire* w : fromIn)
    if (std::find(spine.begin(), spine.end(), w) == spine.end())
      return Error{ErrorCode::InvalidArgument,
                   "Ungroup: In also feeds another input; in the chain only the first device hears the signal: delete "
                   "that wire first",
                   {}};
  if (!reachedIn && !fromIn.empty())
    return Error{ErrorCode::InvalidArgument, "Ungroup: In feeds a device off the series; delete that wire first", {}};
  std::reverse(series.begin(), series.end());  // chain order
  return std::pair{std::move(series), std::move(spine)};
}

}  // namespace

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
  const WireList& wires = wiresOf(p);
  std::vector<const Wire*> intoOut, fromIn, links;
  for (const Wire& w : wires) {
    if (w.to == group && w.param == kGroupOutPort) intoOut.push_back(&w);
    if (w.from == group && w.port == kGroupInPort && !w.fromNoteSource()) fromIn.push_back(&w);
    if (w.to == group && w.param != kGroupOutPort)
      return invalid("Ungroup: a wire reaches the group's own input; delete it first (after ungrouping there is no group)");
    if (w.from == group && w.port != kGroupInPort && !w.fromNoteSource())
      return invalid("Ungroup: a wire leaves the group's own output; delete it first (after ungrouping there is no group)");
    // TX.groups-fix1: the group's chain links (Ctrl+G's series).
    if (w.chain && !w.fromNoteSource() && (w.from == group || w.to == group || isMember(w.from) || isMember(w.to)))
      links.push_back(&w);
  }
  if (l.device->free && (!intoOut.empty() || !fromIn.empty()))
    return invalid("Ungroup: this group is free (not in the chain): delete its In / Out wires first");
  std::vector<const Device*> series;
  std::vector<const Wire*> spine;
  if (!l.device->free && !links.empty()) {
    // TX.groups-fix1: the series is the members its chain links touch, in list order (Ctrl+G keeps every device at its
    // place in the list); those links must be exactly the ones that series makes (seriesLinks), and In / Out must have
    // no other wires (after ungrouping, the chain feeds only the first device and only the last one's output goes on).
    for (const Device* m : members)
      if (std::any_of(links.begin(), links.end(), [&](const Wire* w) { return w->from == m->id || w->to == m->id; }))
        series.push_back(m);
    Result<std::vector<Link>> want = seriesLinks(group, series, lib, "Ungroup: ");
    if (!want) return std::move(want).error();
    bool same = want->size() == links.size();
    for (const Link& k : *want)
      same = same && std::count_if(links.begin(), links.end(), [&](const Wire* w) { return k.same(*w); }) == 1;
    if (!same)
      return invalid("Ungroup: the group's series was rewired (its chain links no longer run In → … → Out through its "
                     "devices in their order); a chain is one series: restore it or delete the group");
    for (const Wire* w : fromIn)
      if (!w->chain)
        return invalid("Ungroup: In also feeds another input; in the chain only the first device hears the signal: "
                       "delete that wire first");
    for (const Wire* w : intoOut)
      if (!w->chain) return invalid("Ungroup: several wires reach Out; a chain is one series: keep one");
    spine = links;
  } else if (!l.device->free) {
    auto back = readSeriesBack(p, lib, group, members, intoOut, fromIn);
    if (!back) return std::move(back).error();
    series = std::move(back->first);
    spine = std::move(back->second);
  }
  Edit e = makeEdit("Ungroup");
  Batch& b = batchOf(e);
  for (const Wire* w : spine) b.commands.push_back(RemoveWire{w->id});
  for (const Device* m : members) b.commands.push_back(SetDeviceGroup{m->id, DeviceId{}});
  b.commands.push_back(RemoveDevice{group});
  for (const Device* d : series) b.commands.push_back(SetDeviceFree{d->id, false});
  // The series at the group's place, in its order (nothing moves when it is there already).
  size_t at = 0;
  for (size_t i = 0; i < track.devices.size(); ++i)
    if (track.devices[i]->id == group) at = i;
  moveSeriesBack(b, track.devices, at, group, series, members);
  for (const Device* m : members) e.devices.push_back(m->id);
  e.tracks.push_back(track.id);
  return e;
}

}  // namespace pq::doc::ops
