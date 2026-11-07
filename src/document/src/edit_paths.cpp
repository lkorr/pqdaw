// TX.all-free (SPEC §15 2026-11-03 "everything is free; the chain is just cables"): the main path's edit builders —
// wire a track in series, insert a block on its path, move it along / off / onto it, dissolve, splice into a wire.
// pq/document/edit_ops.h "The main path"; pq/document/main_path.h.
#include <algorithm>

#include "edit_common.h"
#include "edit_paths.h"
#include "pq/document/main_path.h"

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
  const End self{device, {}};
  if (path.links.empty()) {  // no path at all: the source → it → the mixer
    PQ_DOC_TRY(k.add(AddWire{mainLink(ids, End{{}, track}, self)}));
    return k.add(AddWire{mainLink(ids, self, End{{}, track})});
  }
  const size_t at = std::min<size_t>(slot, path.blocks.size());
  if (at < path.links.size()) {  // the link into that slot: link.from → it → link.to
    const Wire* l = findWire(k.doc(), path.links[at]);
    const End a = endFrom(*l), b = endTo(*l);
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

EditResult setDeviceGroup(const Project& p, IdSource& ids, const DeviceLibrary* lib, DeviceId device, DeviceId group) {
  const DeviceLoc l = findDevice(p, device);
  if (l.device == nullptr) return notFound("device not found");
  Edit e = makeEdit(group.valid() ? "Into Group" : "Out of Group");
  e.devices.push_back(device);
  if (l.device->group == group) return e;
  Steps k(p);
  // A block of the main path leaves it first (a group has no order inside; its neighbours are joined).
  if (group.valid() && mainPath(p, *l.track).contains(device)) PQ_DOC_TRY(detach(k, ids, lib, device));
  PQ_DOC_TRY(k.add(SetDeviceGroup{device, group}));
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
  PQ_DOC_TRY(k.add(AddWire{a}));
  Wire b = mainLink(ids, End{device, {}}, endTo(old));
  b.param = old.param;
  b.reducer = old.reducer;
  PQ_DOC_TRY(k.add(AddWire{b}));
  batchOf(e).commands = std::move(k.commands);
  return e;
}

}  // namespace pq::doc::ops
