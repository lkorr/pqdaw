// TX.all-free (SPEC §15 2026-11-03 "everything is free; the chain is just cables"): the main path's edit builders —
// wire a track in series, insert a block on its path, move it along / off / onto it, dissolve, splice into a wire.
// pq/document/edit_ops.h "The main path"; pq/document/main_path.h.
#include <algorithm>

#include "edit_common.h"
#include "edit_paths.h"
#include "pq/document/note_devices.h"
#include "pq/document/racks.h"
#include "pq/document/main_path.h"
#include "pq/document/wire_types.h"

namespace pq::doc::ops {

using namespace detail;

namespace detail {

Steps::Steps(const Project& p) : snap_(std::make_shared<const Project>(p)) {}

Status Steps::add(const Command& c) {
  ChangeSet cs;
  auto next = apply(snap_, c, cs);
  if (!next) return std::move(next).error();
  snap_ = std::move(*next);
  commands.push_back(c);
  return ok();
}

Status Steps::add(const EditResult& e) {
  if (!e) return e.error();
  for (const Command& c : e->batch().commands) PQ_DOC_TRY(add(c));
  return ok();
}

Wire mainLink(IdSource& ids, const End& from, const End& to) {
  Wire w;
  w.id = ids.wire();
  w.from = from.device;
  w.noteSource = from.device.valid() ? TrackId{} : from.track;
  w.port = std::string(kMainPort);
  w.to = to.device;
  w.mixer = to.device.valid() ? TrackId{} : to.track;
  w.param = std::string(kMainPort);
  return w;
}

End endFrom(const Wire& w) { return w.fromNoteSource() ? End{{}, w.noteSource} : End{w.from, {}}; }
End endTo(const Wire& w) { return w.toMixer() ? End{{}, w.mixer} : End{w.to, {}}; }

bool hasEnds(const Project& p, const Wire& w) {
  for (const Wire& x : wiresOf(p))
    if (x.sameEnds(w)) return true;
  return false;
}

Status splice(Steps& k, IdSource& ids, TrackId track, DeviceId device, uint32_t slot) {
  const Track* t = findTrackPtr(k.doc(), track);
  if (t == nullptr) return notFound("track not found");
  const MainPath path = mainPath(k.doc(), *t);
  if (path.contains(device)) return ok();  // already on it
  // AP.2 (note_devices.h): the notes reach the notes link's stages first — a note device goes no further than the end
  // of the path's note run (after the stages there, before the first block that takes the notes), any other block
  // no earlier than it.
  if (const DeviceLoc dl = findDevice(k.doc(), device); dl.device != nullptr) {
    const uint32_t run = noteRun(path);
    slot = isNoteDevice(*dl.device) ? std::min(slot, run) : std::max(slot, run);
  }
  const End self{device, {}};
  if (path.links.empty()) {  // no path at all: the source → it → the mixer
    PQ_DOC_TRY(k.add(AddWire{mainLink(ids, End{{}, track}, self)}));
    return k.add(AddWire{mainLink(ids, self, End{{}, track})});
  }
  const size_t at = std::min<size_t>(slot, path.blocks.size());
  if (at < path.links.size()) {  // the link into that slot: link.from → it → link.to
    const Wire* l = findWire(k.doc(), path.links[at]);
    const End a = endFrom(*l), b = endTo(*l);
    // GU.9 (racks.h): the link is a rack's feed or its sum — every chain's (they stay parallel: one feed, one sum).
    if (b.device.valid())
      if (const std::optional<RackInfo> r = rackOfChain(k.doc(), *t, b.device)) {
        std::vector<WireId> feeds;
        for (const RackChain& c : r->chains)
          for (const Wire& w : wiresOf(k.doc()))
            if (!w.toMixer() && w.to == c.id && w.param == kMainPort) feeds.push_back(w.id);
        for (const WireId w : feeds) PQ_DOC_TRY(k.add(RemoveWire{w}));
        PQ_DOC_TRY(k.add(AddWire{mainLink(ids, a, self)}));
        for (const RackChain& c : r->chains) PQ_DOC_TRY(k.add(AddWire{mainLink(ids, self, End{c.id, {}})}));
        return ok();
      }
    if (a.device.valid())
      if (const std::optional<RackInfo> r = rackOfChain(k.doc(), *t, a.device)) {
        std::vector<WireId> sums;
        for (const RackChain& c : r->chains)
          for (const Wire& w : wiresOf(k.doc()))
            if (!w.fromNoteSource() && w.from == c.id && w.port == kMainPort && (w.toMixer() || w.param == kMainPort))
              sums.push_back(w.id);
        for (const WireId w : sums) PQ_DOC_TRY(k.add(RemoveWire{w}));
        for (const RackChain& c : r->chains) PQ_DOC_TRY(k.add(AddWire{mainLink(ids, End{c.id, {}}, self)}));
        return k.add(AddWire{mainLink(ids, self, b)});
      }
    PQ_DOC_TRY(k.add(RemoveWire{l->id}));
    PQ_DOC_TRY(k.add(AddWire{mainLink(ids, a, self)}));
    return k.add(AddWire{mainLink(ids, self, b)});
  }
  // At the end of a broken path: after its last block; on into the mixer when nothing reaches its main input yet.
  PQ_DOC_TRY(k.add(AddWire{mainLink(ids, End{path.blocks.back()->id, {}}, self)}));
  for (const Wire& w : wiresOf(k.doc()))
    if (w.toMixer() && w.mixer == track && w.mainLink()) return ok();
  return k.add(AddWire{mainLink(ids, self, End{{}, track})});
}

namespace {
bool sameSource(const Wire& a, const Wire& b) noexcept {
  return a.from == b.from && a.noteSource == b.noteSource && a.port == b.port;
}
bool sameTarget(const Wire& a, const Wire& b) noexcept { return a.to == b.to && a.mixer == b.mixer && a.param == b.param; }
// Whether a block other than `gone` is fed from `s`'s source (into its main input) and feeds `t`'s target from its main
// output: the two ends stay joined through it.
bool parallelRemains(const Project& p, DeviceId gone, const Wire& s, const Wire& t) {
  for (const Wire& a : wiresOf(p)) {
    if (a.toMixer() || a.to == gone || !a.param.empty() || !sameSource(a, s)) continue;
    for (const Wire& b : wiresOf(p))
      if (!b.fromNoteSource() && b.from == a.to && b.port.empty() && sameTarget(b, t)) return true;
  }
  return false;
}
}  // namespace

Status detach(Steps& k, IdSource& ids, const DeviceLibrary* lib, DeviceId device) {
  const DeviceLoc loc = findDevice(k.doc(), device);
  if (loc.device == nullptr) return notFound("device not found");
  const MainPorts mp = mainPortsOf(*loc.device, lib);
  std::vector<Wire> sources, targets;
  for (const Wire& w : wiresOf(k.doc())) {
    const bool selfLoop = !w.fromNoteSource() && w.from == device && !w.toMixer() && w.to == device;
    if (selfLoop) continue;
    if (!w.toMixer() && w.to == device && (w.param.empty() || (!mp.in.empty() && w.param == mp.in))) sources.push_back(w);
    if (!w.fromNoteSource() && w.from == device && (w.port.empty() || (!mp.out.empty() && w.port == mp.out))) targets.push_back(w);
  }
  for (const Wire& w : sources) PQ_DOC_TRY(k.add(RemoveWire{w.id}));
  for (const Wire& w : targets) PQ_DOC_TRY(k.add(RemoveWire{w.id}));
  for (const Wire& s : sources)
    for (const Wire& t : targets) {
      Wire j;
      j.from = s.from;
      j.noteSource = s.noteSource;
      j.port = s.port;
      j.to = t.to;
      j.mixer = t.mixer;
      j.param = t.param;
      j.reducer = t.reducer;
      j.chain = s.chain && t.chain;
      if (j.toMixer()) {  // a mixer takes its own track's blocks and source only
        const Track* src = j.fromNoteSource() ? findTrackPtr(k.doc(), j.noteSource) : findDevice(k.doc(), j.from).track;
        if (src == nullptr || src->id != j.mixer) continue;
      }
      if (j.fromNoteSource() && !j.port.empty() && j.toMixer()) continue;  // a note output is not audio for a mixer
      if (hasEnds(k.doc(), j)) continue;
      // GU.9 (racks.h): a parallel branch already joins the two ends (another group fed from the same point and summed
      // into the same one, a rack's other chain): no bypass beside it.
      if (parallelRemains(k.doc(), device, s, t)) continue;
      j.id = ids.wire();
      if (const Status st = k.add(AddWire{j}); !st) return st;
    }
  return ok();
}

}  // namespace detail

EditResult wireSeries(const Project& p, IdSource& ids) {
  Edit e = makeEdit("Wire in Series");
  Steps k(p);
  std::vector<TrackId> todo;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const Wire& w : wiresOf(p))
      if ((w.fromNoteSource() && w.noteSource == t.id && w.port.empty()) || (w.toMixer() && w.mixer == t.id)) return;
    todo.push_back(t.id);
  });
  for (const TrackId id : todo) {
    const Track& t = *findTrackPtr(p, id);
    End at{{}, id};
    for (const DevicePtr& d : t.devices) {
      if (d->group.valid() && t.deviceIndex(d->group) >= 0) continue;  // inside a group
      PQ_DOC_TRY(k.add(AddWire{mainLink(ids, at, End{d->id, {}})}));
      at = End{d->id, {}};
    }
    PQ_DOC_TRY(k.add(AddWire{mainLink(ids, at, End{{}, id})}));
    e.tracks.push_back(id);
  }
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult insertOnPath(const Project& p, IdSource& ids, DeviceId device, uint32_t slot) {
  const DeviceLoc loc = findDevice(p, device);
  if (loc.device == nullptr) return notFound("device not found");
  Edit e = makeEdit("Insert on Path");
  Steps k(p);
  PQ_DOC_TRY(splice(k, ids, loc.track->id, device, slot));
  e.devices.push_back(device);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult moveOnPath(const Project& p, IdSource& ids, const DeviceLibrary* lib, DeviceId device, TrackId track,
                      uint32_t slot) {
  const DeviceLoc loc = findDevice(p, device);
  if (loc.device == nullptr) return notFound("device not found");
  const TrackId to = track.valid() ? track : loc.track->id;
  const Track* dst = findTrackPtr(p, to);
  if (dst == nullptr) return notFound("track not found");
  if (loc.device->kind == DeviceKind::Instrument && dst->kind != TrackKind::Midi)
    return invalid("instruments go on MIDI tracks (the notes are their main input)");
  if (isNoteDevice(*loc.device) && dst->kind != TrackKind::Midi)  // AP.2
    return invalid("a note device goes on a MIDI track (no notes reach it elsewhere)");
  Edit e = makeEdit("Move Device");
  e.devices.push_back(device);
  // Where it is now: on its path at `from` (an unchanged place is no edit).
  const MainPath before = mainPath(p, *loc.track);
  const int32_t from = before.indexOf(device);
  if (from >= 0 && to == loc.track->id) {
    const size_t at = std::min<size_t>(slot, before.blocks.size());
    if (at == static_cast<size_t>(from) || at == static_cast<size_t>(from) + 1) return e;
  }
  Steps k(p);
  if (from >= 0) PQ_DOC_TRY(detach(k, ids, lib, device));
  // GU.9: a member of a group (a rack's chain) leaves its group's series (joined there) and the group.
  if (from < 0 && loc.device->group.valid()) {
    PQ_DOC_TRY(detach(k, ids, lib, device));
    PQ_DOC_TRY(k.add(SetDeviceGroup{device, {}}));
  }
  if (to != loc.track->id) PQ_DOC_TRY(k.add(MoveDevice{device, to, kAppend}));
  // The slot counted on the path without it.
  uint32_t s = slot;
  if (from >= 0 && to == loc.track->id && slot != kAppend && slot > static_cast<uint32_t>(from)) --s;
  PQ_DOC_TRY(splice(k, ids, to, device, s));
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult dissolveDevices(const Project& p, IdSource& ids, const DeviceLibrary* lib, std::span<const DeviceId> devices) {
  Edit e = makeEdit(devices.size() == 1 ? "Dissolve Device" : "Dissolve Devices");
  Steps k(p);
  for (const DeviceId id : devices) {
    if (findDevice(k.doc(), id).device == nullptr) continue;  // went with a rack or a group removed before it
    PQ_DOC_TRY(detach(k, ids, lib, id));
    PQ_DOC_TRY(k.add(deleteDevices(k.doc(), std::span<const DeviceId>(&id, 1))));
  }
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult takeOffPath(const Project& p, IdSource& ids, const DeviceLibrary* lib, std::span<const DeviceId> devices) {
  Edit e = makeEdit("Take Off Path");
  Steps k(p);
  for (const DeviceId id : devices) {
    const DeviceLoc l = findDevice(k.doc(), id);
    if (l.device == nullptr) return notFound("device not found");
    if (!mainPath(k.doc(), *l.track).contains(id)) continue;
    PQ_DOC_TRY(detach(k, ids, lib, id));
    e.devices.push_back(id);
  }
  batchOf(e).commands = std::move(k.commands);
  return e;
}

namespace {

// BR.K (bug 2026-11-28_00-43-14): a block crossing a group's boundary keeps its signal path, routed through the group's
// ports. The rule reads only wires and main ports (nothing per block type):
//   out of g, at its head   (its main input reads g's In, and nothing else reads that In): g's sources feed it, it
//                           feeds g's main input, and what it fed inside reads g's In: it now plays BEFORE g. A Sampler
//                           leaving an instrument group takes the notes and g gets its audio (g's kind follows its input:
//                           an audio group, no longer polyphonic);
//   out of g, at its tail   (its main output feeds g's Out, and nothing else does): what fed it feeds g's Out, g's main
//                           output feeds it, and it feeds what g fed: it now plays AFTER g;
//   out of g, elsewhere     its main neighbours inside are joined (as a block leaving a path); it leaves unwired there;
//   into g, right before it (its main output feeds only g's main input, its only source): the exact inverse of "out at
//                           its head"; into g, right after it (g's main output feeds only it, its only source): of "out
//                           at its tail"; otherwise a block of the main path leaves it first (joined there).
// Any other wire (a modulation, a second reader of g's In) keeps its ends: a wire may cross a boundary. When g's In has
// other readers too, a block leaving at the head reads what g's In carried, directly (QUESTIONS Q?-BR.K-a).

// Whether `x` is inside group `g` (at any depth).
bool within(const Project& p, DeviceId x, DeviceId g) {
  for (const Device* d = findDevice(p, x).device; d != nullptr && d->group.valid(); d = findDevice(p, d->group).device)
    if (d->group == g) return true;
  return false;
}
bool fromDevice(const Wire& w) { return !w.fromNoteSource() && !w.fromMixerOut(); }
bool toDevice(const Wire& w) { return !w.toMixer() && !w.toRecordInput(); }
bool readsGroupIn(const Wire& w, DeviceId g) { return fromDevice(w) && w.from == g && w.port == kGroupInPort; }
bool feedsGroupOut(const Wire& w, DeviceId g) { return toDevice(w) && w.to == g && w.param == kGroupOutPort; }
bool intoMain(const Wire& w, DeviceId d, const MainPorts& mp) {
  return toDevice(w) && w.to == d && (w.param.empty() || (!mp.in.empty() && w.param == mp.in));
}
bool outOfMain(const Wire& w, DeviceId d, const MainPorts& mp) {
  return fromDevice(w) && w.from == d && (w.port.empty() || (!mp.out.empty() && w.port == mp.out));
}
bool intoGroupMain(const Wire& w, DeviceId g) { return toDevice(w) && w.to == g && w.param.empty(); }
bool outOfGroupMain(const Wire& w, DeviceId g) { return fromDevice(w) && w.from == g && w.port.empty(); }
// Inside g, or g itself (its Out terminal from inside, its In terminal).
bool inOrIs(const Project& p, DeviceId x, DeviceId g) { return x == g || within(p, x, g); }

// `w` again with a fresh id (its law, amount, reducer and flags kept; new ends set by the caller).
Wire again(IdSource& ids, const Wire& w) {
  Wire x = w;
  x.id = ids.wire();
  return x;
}
void setFrom(Wire& w, DeviceId d, std::string_view port) {
  w.from = d;
  w.noteSource = {};
  w.mixerOut = {};
  w.port = std::string(port);
  w.tap.reset();
}
void setFrom(Wire& w, const Wire& source) {
  w.from = source.from;
  w.noteSource = source.noteSource;
  w.mixerOut = source.mixerOut;
  w.port = source.port;
  w.tap = source.tap;
}
void setTo(Wire& w, DeviceId d, std::string_view param) {
  w.to = d;
  w.mixer = {};
  w.recordInput = {};
  w.param = std::string(param);
}

struct Rewire {
  std::vector<WireId> remove;
  std::vector<Wire> add;
};
Status applyRewire(Steps& k, const Rewire& r) {
  for (const WireId w : r.remove)
    if (findWire(k.doc(), w) != nullptr) PQ_DOC_TRY(k.add(RemoveWire{w}));
  for (const Wire& w : r.add)
    if (!hasEnds(k.doc(), w)) PQ_DOC_TRY(k.add(AddWire{w}));
  return ok();
}

// `d` (a member of g) leaves g for g's own group.
Status leaveGroup(Steps& k, IdSource& ids, const DeviceLibrary* lib, DeviceId d, DeviceId g) {
  const Project& p = k.doc();
  const Device* dev = findDevice(p, d).device;
  const Device* grp = findDevice(p, g).device;
  if (dev == nullptr || grp == nullptr) return notFound("device not found");
  const DeviceId outer = grp->group;
  const MainPorts mp = mainPortsOf(*dev, lib);
  const WireList& ws = wiresOf(p);
  bool head = false, otherReader = false, tail = false, otherFeeder = false;
  for (const Wire& w : ws) {
    if (readsGroupIn(w, g)) {
      if (intoMain(w, d, mp)) head = true;
      else if (!(toDevice(w) && w.to == d)) otherReader = true;
    }
    if (feedsGroupOut(w, g)) (outOfMain(w, d, mp) ? tail : otherFeeder) = true;
  }
  Rewire r;
  if (head) {
    std::vector<const Wire*> sources;  // into g's main input
    for (const Wire& w : ws)
      if (intoGroupMain(w, g)) sources.push_back(&w);
    for (const Wire& w : ws) {  // d reads what g's In carried (every input of d that read it)
      if (!readsGroupIn(w, g) || !toDevice(w) || w.to != d) continue;
      r.remove.push_back(w.id);
      for (const Wire* s : sources) {
        Wire x = again(ids, w);
        setFrom(x, *s);
        r.add.push_back(x);
      }
    }
    if (!otherReader) {  // d plays before g: g's sources are d's now, d feeds g, what d fed inside reads g's In
      for (const Wire* s : sources) r.remove.push_back(s->id);
      for (const Wire& w : ws) {
        if (!outOfMain(w, d, mp) || !toDevice(w) || !inOrIs(p, w.to, g)) continue;
        r.remove.push_back(w.id);
        Wire x = again(ids, w);
        setFrom(x, g, kGroupInPort);
        r.add.push_back(x);
      }
      r.add.push_back(mainLink(ids, End{d, {}}, End{g, {}}));
    }
  } else if (tail && !otherFeeder) {  // d plays after g: what fed it feeds g's Out, g feeds it, it feeds what g fed
    for (const Wire& w : ws) {
      if (feedsGroupOut(w, g) && outOfMain(w, d, mp)) r.remove.push_back(w.id);
      if (intoMain(w, d, mp) && fromDevice(w) && inOrIs(p, w.from, g)) {
        r.remove.push_back(w.id);
        Wire x = again(ids, w);
        setTo(x, g, kGroupOutPort);
        r.add.push_back(x);
      }
      if (outOfGroupMain(w, g)) {
        r.remove.push_back(w.id);
        Wire x = again(ids, w);
        setFrom(x, d, kMainPort);
        r.add.push_back(x);
      }
    }
    r.add.push_back(mainLink(ids, End{g, {}}, End{d, {}}));
  } else {
    // In the middle of g's series: its main neighbours inside are joined; its other wires keep their ends.
    bool inside = false;
    for (const Wire& w : ws)
      if ((intoMain(w, d, mp) && fromDevice(w) && inOrIs(p, w.from, g)) || (outOfMain(w, d, mp) && toDevice(w) && inOrIs(p, w.to, g)))
        inside = true;
    if (inside) PQ_DOC_TRY(detach(k, ids, lib, d));
  }
  PQ_DOC_TRY(k.add(SetDeviceGroup{d, outer}));
  return applyRewire(k, r);
}

// `d` (beside g: in the same group) joins g right before or after it. False (nothing done): it is neither.
Result<bool> joinAtEnd(Steps& k, IdSource& ids, const DeviceLibrary* lib, DeviceId d, DeviceId g) {
  const Project& p = k.doc();
  const Device* dev = findDevice(p, d).device;
  if (dev == nullptr) return notFound("device not found");
  const MainPorts mp = mainPortsOf(*dev, lib);
  const WireList& ws = wiresOf(p);
  auto count = [&](auto pred) { return std::count_if(ws.begin(), ws.end(), pred); };
  const auto dToG = [&](const Wire& w) { return outOfMain(w, d, mp) && intoGroupMain(w, g); };
  const auto gToD = [&](const Wire& w) { return outOfGroupMain(w, g) && intoMain(w, d, mp); };
  Rewire r;
  if (count(dToG) == 1 && count([&](const Wire& w) { return outOfMain(w, d, mp); }) == 1 &&
      count([&](const Wire& w) { return intoGroupMain(w, g); }) == 1) {
    // Before g: its sources feed g, g's In feeds it, it feeds what g's In fed (the inverse of leaving at the head).
    for (const Wire& w : ws) {
      if (dToG(w)) r.remove.push_back(w.id);
      if (intoMain(w, d, mp)) {
        r.remove.push_back(w.id);
        Wire x = again(ids, w);
        setTo(x, g, kMainPort);
        r.add.push_back(x);
      }
      if (readsGroupIn(w, g)) {
        r.remove.push_back(w.id);
        Wire x = again(ids, w);
        setFrom(x, d, kMainPort);
        r.add.push_back(x);
      }
    }
    Wire link = mainLink(ids, End{g, {}}, End{d, {}});
    link.port = std::string(kGroupInPort);
    r.add.push_back(link);
  } else if (count(gToD) == 1 && count([&](const Wire& w) { return intoMain(w, d, mp); }) == 1 &&
             count([&](const Wire& w) { return outOfGroupMain(w, g); }) == 1) {
    // After g: what fed g's Out feeds it, it feeds g's Out, g feeds what it fed (the inverse of leaving at the tail).
    for (const Wire& w : ws) {
      if (gToD(w)) r.remove.push_back(w.id);
      if (feedsGroupOut(w, g)) {
        r.remove.push_back(w.id);
        Wire x = again(ids, w);
        setTo(x, d, kMainPort);
        r.add.push_back(x);
      }
      if (outOfMain(w, d, mp)) {
        r.remove.push_back(w.id);
        Wire x = again(ids, w);
        setFrom(x, g, kMainPort);
        r.add.push_back(x);
      }
    }
    Wire link = mainLink(ids, End{d, {}}, End{g, {}});
    link.param = std::string(kGroupOutPort);
    r.add.push_back(link);
  } else {
    return false;
  }
  PQ_DOC_TRY(k.add(SetDeviceGroup{d, g}));
  PQ_DOC_TRY(applyRewire(k, r));
  return true;
}

}  // namespace

EditResult setDeviceGroup(const Project& p, IdSource& ids, const DeviceLibrary* lib, DeviceId device, DeviceId group) {
  const DeviceLoc l = findDevice(p, device);
  if (l.device == nullptr) return notFound("device not found");
  Edit e = makeEdit(group.valid() ? "Into Group" : "Out of Group");
  e.devices.push_back(device);
  if (l.device->group == group) return e;
  Steps k(p);
  // BR.K: out of its groups one level at a time (each boundary crossed through that group's ports) while `group` is not
  // around it: up to `group`, or out of every group.
  for (const Device* d = l.device; d != nullptr && d->group.valid() && d->group != group && !within(k.doc(), group, d->group);
       d = findDevice(k.doc(), device).device)
    PQ_DOC_TRY(leaveGroup(k, ids, lib, device, d->group));
  if (const Device* d = findDevice(k.doc(), device).device; d != nullptr && d->group != group) {
    bool joined = false;
    if (const Device* g = group.valid() ? findDevice(k.doc(), group).device : nullptr; g != nullptr && g->group == d->group) {
      Result<bool> j = joinAtEnd(k, ids, lib, device, group);
      if (!j) return std::move(j).error();
      joined = *j;
    }
    if (!joined) {
      // A block of the main path leaves it first (a group has no order inside; its neighbours are joined).
      if (group.valid() && mainPath(k.doc(), *findDevice(k.doc(), device).track).contains(device))
        PQ_DOC_TRY(detach(k, ids, lib, device));
      PQ_DOC_TRY(k.add(SetDeviceGroup{device, group}));
    }
  }
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult spliceIntoWire(const Project& p, IdSource& ids, const DeviceLibrary* lib, DeviceId device, WireId wire) {
  const Wire* w0 = findWire(p, wire);
  if (w0 == nullptr) return notFound("wire not found");
  const Wire old = *w0;
  const DeviceLoc loc = findDevice(p, device);
  if (loc.device == nullptr) return notFound("device not found");
  if ((!old.fromNoteSource() && old.from == device) || (!old.toMixer() && old.to == device))
    return invalid("that cable is the block's own");
  // The track whose path a link belongs to (a link of another track's path takes the block there).
  const Track* home = old.toMixer() ? findTrackPtr(p, old.mixer)
                      : old.fromNoteSource() ? findTrackPtr(p, old.noteSource)
                                             : findDevice(p, old.from).track;
  if (home == nullptr) home = loc.track;
  if (loc.device->kind == DeviceKind::Instrument && home->kind != TrackKind::Midi)
    return invalid("instruments go on MIDI tracks (the notes are their main input)");
  Edit e = makeEdit("Splice");
  e.devices.push_back(device);
  Steps k(p);
  // Off its own path first (a block of a path is taken out where it is: its neighbours joined).
  if (mainPath(p, *loc.track).contains(device)) PQ_DOC_TRY(detach(k, ids, lib, device));
  if (old.mainLink() && home->id != loc.track->id) PQ_DOC_TRY(k.add(MoveDevice{device, home->id, kAppend}));
  if (findWire(k.doc(), old.id) == nullptr) return invalid("the cable went with the block's own");
  PQ_DOC_TRY(k.add(RemoveWire{old.id}));
  Wire a = mainLink(ids, endFrom(old), End{device, {}});
  a.port = old.port;
  a.tap = old.tap;  // TL.1: a lane's cable keeps where it leaves the lane
  Wire b = mainLink(ids, End{device, {}}, endTo(old));
  b.param = old.param;
  b.reducer = old.reducer;
  // AR.1 (wire_types.h): the notes rule on the two cables it makes (a note device goes only into a notes cable, and
  // nothing but one into a cable between two notes ports).
  for (const Wire* w : {&a, &b})
    if (const WireClash c = notesWireClash(k.doc(), *w); c != WireClash::None) return invalid(std::string(wireClashText(c)));
  PQ_DOC_TRY(k.add(AddWire{a}));
  PQ_DOC_TRY(k.add(AddWire{b}));
  batchOf(e).commands = std::move(k.commands);
  return e;
}

}  // namespace pq::doc::ops
