// Device edit builders (T4.1): pq/document/edit_ops.h, "Devices".
#include <algorithm>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

// Where a new device of `kind` goes in `t`'s chain, and whether it replaces the instrument there.
struct Slot {
  uint32_t index = 0;
  bool replacesInstrument = false;
};
Result<Slot> slotFor(const Track& t, DeviceKind kind, uint32_t index) {
  const bool hasInstrument = !t.devices.empty() && t.devices.front()->kind == DeviceKind::Instrument;
  if (kind == DeviceKind::Instrument) {
    if (t.kind != TrackKind::Midi) return invalid("instruments go on MIDI tracks");
    return Slot{0, hasInstrument};
  }
  const auto n = static_cast<uint32_t>(t.devices.size());
  uint32_t at = std::min(index, n);
  if (hasInstrument && at == 0) at = 1;
  return Slot{at, false};
}

Device copyOf(const Device& d, DeviceId id) {
  Device c = d;
  c.id = id;
  return c;
}

}  // namespace

EditResult addDevice(const Project& p, IdSource& ids, TrackId track, std::string patch, DeviceKind kind, uint32_t index) {
  const Track* t = findTrackPtr(p, track);
  if (!t) return notFound("track not found");
  if (patch.empty()) return invalid("no device patch");
  Result<Slot> slot = slotFor(*t, kind, index);
  if (!slot) return std::move(slot).error();
  Edit e = makeEdit("Add Device");
  if (slot->replacesInstrument) batchOf(e).commands.push_back(RemoveDevice{t->devices.front()->id});
  DevicePtr d = makeDevice(ids.device(), std::move(patch), kind);
  e.devices.push_back(d->id);
  e.tracks.push_back(track);
  batchOf(e).commands.push_back(AddDevice{track, std::move(d), slot->index});
  return e;
}

EditResult deleteDevices(const Project& p, std::span<const DeviceId> devices) {
  Edit e = makeEdit(devices.size() == 1 ? "Delete Device" : "Delete Devices");
  std::vector<DeviceId> seen;
  for (const DeviceId id : devices) {
    if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
    if (!findDevice(p, id).track) return notFound("device not found");
    seen.push_back(id);
    batchOf(e).commands.push_back(RemoveDevice{id});
  }
  return e;
}

EditResult moveDevice(const Project& p, DeviceId device, TrackId toTrack, uint32_t index) {
  const DeviceLoc d = findDevice(p, device);
  if (!d.track) return notFound("device not found");
  const Track* to = toTrack.valid() ? findTrackPtr(p, toTrack) : d.track;
  if (!to) return notFound("track not found");
  Edit e = makeEdit("Move Device");
  e.devices.push_back(device);
  // A no-op move gives an empty batch (no history entry).
  if (to == d.track) {
    const auto after = std::min<uint32_t>(index, static_cast<uint32_t>(d.track->devices.size() - 1));
    if (after == d.index) return e;
  }
  batchOf(e).commands.push_back(MoveDevice{device, to->id, index});
  return e;
}

EditResult setDevicesActive(const Project& p, std::span<const DeviceId> devices, bool active) {
  Edit e = makeEdit(active ? "Activate Device" : "Deactivate Device");
  for (const DeviceId id : devices) {
    const DeviceLoc d = findDevice(p, id);
    if (!d.track) return notFound("device not found");
    e.devices.push_back(id);
    if (d.device->active != active) batchOf(e).commands.push_back(SetDeviceActive{id, active});
  }
  return e;
}

EditResult toggleDevicesActive(const Project& p, std::span<const DeviceId> devices) {
  bool anyActive = false;
  for (const DeviceId id : devices) {
    const DeviceLoc d = findDevice(p, id);
    if (!d.track) return notFound("device not found");
    anyActive = anyActive || d.device->active;
  }
  return setDevicesActive(p, devices, !anyActive);
}

EditResult pasteDevices(const Project& p, IdSource& ids, TrackId track, std::span<const DevicePtr> devices,
                        uint32_t index) {
  const Track* t = findTrackPtr(p, track);
  if (!t) return notFound("track not found");
  Edit e = makeEdit(devices.size() == 1 ? "Paste Device" : "Paste Devices");
  e.tracks.push_back(track);
  const bool hasInstrument = !t->devices.empty() && t->devices.front()->kind == DeviceKind::Instrument;
  uint32_t at = std::min<uint32_t>(index, static_cast<uint32_t>(t->devices.size()));
  if (hasInstrument && at == 0) at = 1;
  bool instrumentPasted = false;
  for (const DevicePtr& src : devices) {
    if (!src) continue;
    auto d = std::make_shared<const Device>(copyOf(*src, ids.device()));
    e.devices.push_back(d->id);
    if (d->kind == DeviceKind::Instrument) {
      if (t->kind != TrackKind::Midi) return invalid("instruments go on MIDI tracks");
      if (instrumentPasted) return invalid("a track holds one instrument");
      instrumentPasted = true;
      if (hasInstrument) batchOf(e).commands.push_back(RemoveDevice{t->devices.front()->id});
      batchOf(e).commands.push_back(AddDevice{track, std::move(d), 0});
      if (!hasInstrument) ++at;  // the effects pasted so far moved one place right
      continue;
    }
    batchOf(e).commands.push_back(AddDevice{track, std::move(d), at++});
  }
  return e;
}

EditResult duplicateDevices(const Project& p, IdSource& ids, std::span<const DeviceId> devices) {
  Edit e = makeEdit(devices.size() == 1 ? "Duplicate Device" : "Duplicate Devices");
  // Per track, in chain order: the copies go right after the last selected device of that track.
  struct Pick {
    const Track* track;
    uint32_t index;
  };
  std::vector<Pick> picks;
  for (const DeviceId id : devices) {
    const DeviceLoc d = findDevice(p, id);
    if (!d.track) return notFound("device not found");
    if (d.device->kind == DeviceKind::Instrument) continue;  // one instrument per track
    if (std::none_of(picks.begin(), picks.end(), [&](const Pick& k) { return k.track == d.track && k.index == d.index; }))
      picks.push_back(Pick{d.track, d.index});
  }
  std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) {
    return a.track != b.track ? a.track->id < b.track->id : a.index < b.index;
  });
  for (size_t i = 0; i < picks.size();) {
    size_t j = i;
    while (j < picks.size() && picks[j].track == picks[i].track) ++j;
    uint32_t at = picks[j - 1].index + 1;
    for (size_t k = i; k < j; ++k) {
      auto d = std::make_shared<const Device>(copyOf(*picks[k].track->devices[picks[k].index], ids.device()));
      e.devices.push_back(d->id);
      batchOf(e).commands.push_back(AddDevice{picks[i].track->id, std::move(d), at++});
    }
    i = j;
  }
  return e;
}

EditResult setDeviceParam(const Project& p, DeviceId device, std::string param, std::optional<double> value) {
  if (!findDevice(p, device).track) return notFound("device not found");
  PQ_DOC_TRY(checkDeviceValue(param, value));
  Edit e = makeEdit("Device Parameter");
  e.devices.push_back(device);
  batchOf(e).commands.push_back(SetDeviceParam{device, std::move(param), value});
  return e;
}

EditResult setDeviceSetting(const Project& p, DeviceId device, std::string setting, std::optional<double> value) {
  if (!findDevice(p, device).track) return notFound("device not found");
  PQ_DOC_TRY(checkDeviceValue(setting, value));
  Edit e = makeEdit("Device Setting");
  e.devices.push_back(device);
  batchOf(e).commands.push_back(SetDeviceSetting{device, std::move(setting), value});
  return e;
}

}  // namespace pq::doc::ops
