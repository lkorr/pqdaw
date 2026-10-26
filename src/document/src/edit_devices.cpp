// Device edit builders (T4.1; inside racks T4.8): pq/document/edit_ops.h, "Devices".
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

// A device list (a track's chain or a rack chain): its devices and whether it may hold an instrument first.
struct ListView {
  const std::vector<DevicePtr>* devices = nullptr;
  bool instrumentAllowed = false;  // a MIDI track or an Instrument Rack's chain
  TrackId track;
  ChainId chain;
};
Result<ListView> listOf(const Project& p, TrackId track, ChainId chain) {
  ListView v;
  if (chain.valid()) {
    const ChainLoc c = findChain(p, chain);
    if (!c.chain) return notFound("chain not found");
    v.devices = &c.chain->devices;
    v.instrumentAllowed = c.rack->kind == DeviceKind::Instrument;
    v.track = c.track->id;
    v.chain = chain;
    return v;
  }
  const Track* t = findTrackPtr(p, track);
  if (!t) return notFound("track not found");
  v.devices = &t->devices;
  v.instrumentAllowed = t->kind == TrackKind::Midi;
  v.track = track;
  return v;
}

// Where a new device of `kind` goes in a list, and whether it replaces the instrument there.
struct Slot {
  uint32_t index = 0;
  bool replacesInstrument = false;
};
Result<Slot> slotFor(const ListView& l, DeviceKind kind, uint32_t index) {
  const std::vector<DevicePtr>& d = *l.devices;
  const bool hasInstrument = !d.empty() && d.front()->kind == DeviceKind::Instrument;
  if (kind == DeviceKind::Instrument) {
    if (!l.instrumentAllowed)
      return invalid(l.chain.valid() ? "an Audio Effect Rack's chains hold audio effects only" : "instruments go on MIDI tracks");
    return Slot{0, hasInstrument};
  }
  const auto n = static_cast<uint32_t>(d.size());
  uint32_t at = std::min(index, n);
  if (hasInstrument && at == 0) at = 1;
  return Slot{at, false};
}

}  // namespace

DevicePtr cloneDevice(const Device& d, IdSource& ids) {
  std::unordered_map<uint32_t, DeviceId> remap;  // old device id → new (racks: mapping targets follow)
  struct Clone {
    IdSource& ids;
    std::unordered_map<uint32_t, DeviceId>& remap;
    std::shared_ptr<Device> operator()(const Device& src) {
      auto c = std::make_shared<Device>(src);
      c->id = ids.device();
      remap[src.id.value] = c->id;
      if (!src.rack) return c;
      auto r = std::make_shared<Rack>(*src.rack);
      for (ChainPtr& ch : r->chains) {
        auto cc = std::make_shared<Chain>(*ch);
        cc->id = ids.chain();
        for (DevicePtr& x : cc->devices) x = (*this)(*x);
        ch = std::move(cc);
      }
      c->rack = std::move(r);
      return c;
    }
    // Mappings after every device has its new id.
    void fixMappings(Device& dev) {
      if (!dev.rack) return;
      auto r = std::make_shared<Rack>(*dev.rack);
      for (MacroMapping& m : r->mappings) {
        m.id = ids.mapping();
        if (const auto it = remap.find(m.device.value); it != remap.end()) m.device = it->second;
      }
      for (ChainPtr& ch : r->chains) {
        auto cc = std::make_shared<Chain>(*ch);
        for (DevicePtr& x : cc->devices) {
          auto xc = std::make_shared<Device>(*x);
          fixMappings(*xc);
          x = std::move(xc);
        }
        ch = std::move(cc);
      }
      dev.rack = std::move(r);
    }
  } clone{ids, remap};
  std::shared_ptr<Device> out = clone(d);
  clone.fixMappings(*out);
  return out;
}

EditResult addDevice(const Project& p, IdSource& ids, TrackId track, std::string patch, DeviceKind kind, uint32_t index,
                     ChainId chain) {
  Result<ListView> l = listOf(p, track, chain);
  if (!l) return std::move(l).error();
  if (patch.empty()) return invalid("no device patch");
  Result<Slot> slot = slotFor(*l, kind, index);
  if (!slot) return std::move(slot).error();
  Edit e = makeEdit(patch == kAudioEffectRack || patch == kInstrumentRack ? "Add Rack" : "Add Device");
  if (slot->replacesInstrument) batchOf(e).commands.push_back(RemoveDevice{l->devices->front()->id});
  const bool rack = patch == kAudioEffectRack || patch == kInstrumentRack;
  if (rack && (patch == kInstrumentRack) != (kind == DeviceKind::Instrument)) return invalid("rack kind mismatch");
  DevicePtr d = rack ? makeRack(ids.device(), kind) : makeDevice(ids.device(), std::move(patch), kind);
  e.devices.push_back(d->id);
  e.tracks.push_back(l->track);
  batchOf(e).commands.push_back(AddDevice{l->track, std::move(d), slot->index, chain});
  return e;
}

EditResult deleteDevices(const Project& p, std::span<const DeviceId> devices) {
  Edit e = makeEdit(devices.size() == 1 ? "Delete Device" : "Delete Devices");
  std::vector<DeviceId> seen;
  for (const DeviceId id : devices) {
    if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
    if (!findDevice(p, id).track) return notFound("device not found");
    seen.push_back(id);
  }
  for (const DeviceId id : seen) {
    // T4.8: a device inside another selected rack goes with that rack.
    const bool inner = std::any_of(seen.begin(), seen.end(), [&](DeviceId o) {
      return o != id && deviceContains(*findDevice(p, o).device, id);
    });
    if (!inner) batchOf(e).commands.push_back(RemoveDevice{id});
  }
  return e;
}

EditResult moveDevice(const Project& p, DeviceId device, TrackId toTrack, uint32_t index, ChainId toChain) {
  const DeviceLoc d = findDevice(p, device);
  if (!d.track) return notFound("device not found");
  Edit e = makeEdit("Move Device");
  e.devices.push_back(device);
  if (toChain.valid()) {
    const ChainLoc c = findChain(p, toChain);
    if (!c.chain) return notFound("chain not found");
    if (c.chain == d.chain) {
      const auto after = std::min<uint32_t>(index, static_cast<uint32_t>(d.list().size() - 1));
      if (after == d.index) return e;
    }
    batchOf(e).commands.push_back(MoveDevice{device, {}, index, toChain});
    return e;
  }
  const Track* to = toTrack.valid() ? findTrackPtr(p, toTrack) : d.track;
  if (!to) return notFound("track not found");
  // A no-op move gives an empty batch (no history entry).
  if (to == d.track && d.chain == nullptr) {
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
                        uint32_t index, ChainId chain) {
  Result<ListView> l = listOf(p, track, chain);
  if (!l) return std::move(l).error();
  Edit e = makeEdit(devices.size() == 1 ? "Paste Device" : "Paste Devices");
  e.tracks.push_back(l->track);
  const std::vector<DevicePtr>& list = *l->devices;
  const bool hasInstrument = !list.empty() && list.front()->kind == DeviceKind::Instrument;
  uint32_t at = std::min<uint32_t>(index, static_cast<uint32_t>(list.size()));
  if (hasInstrument && at == 0) at = 1;
  bool instrumentPasted = false;
  for (const DevicePtr& src : devices) {
    if (!src) continue;
    DevicePtr d = cloneDevice(*src, ids);
    e.devices.push_back(d->id);
    if (d->kind == DeviceKind::Instrument) {
      if (!l->instrumentAllowed) return invalid(chain.valid() ? "an Audio Effect Rack's chains hold audio effects only"
                                                              : "instruments go on MIDI tracks");
      if (instrumentPasted) return invalid("a track holds one instrument");
      instrumentPasted = true;
      if (hasInstrument) batchOf(e).commands.push_back(RemoveDevice{list.front()->id});
      batchOf(e).commands.push_back(AddDevice{l->track, std::move(d), 0, chain});
      if (!hasInstrument) ++at;  // the effects pasted so far moved one place right
      continue;
    }
    batchOf(e).commands.push_back(AddDevice{l->track, std::move(d), at++, chain});
  }
  return e;
}

EditResult duplicateDevices(const Project& p, IdSource& ids, std::span<const DeviceId> devices) {
  Edit e = makeEdit(devices.size() == 1 ? "Duplicate Device" : "Duplicate Devices");
  // Per list, in its order: the copies go right after the last selected device of that list.
  struct Pick {
    const std::vector<DevicePtr>* list;
    TrackId track;
    ChainId chain;
    uint32_t index;
  };
  std::vector<Pick> picks;
  for (const DeviceId id : devices) {
    const DeviceLoc d = findDevice(p, id);
    if (!d.track) return notFound("device not found");
    if (d.device->kind == DeviceKind::Instrument) continue;  // one instrument per list
    const std::vector<DevicePtr>* list = &d.list();
    if (std::none_of(picks.begin(), picks.end(), [&](const Pick& k) { return k.list == list && k.index == d.index; }))
      picks.push_back(Pick{list, d.track->id, d.chain ? d.chain->id : ChainId{}, d.index});
  }
  std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) {
    if (a.track != b.track) return a.track < b.track;
    return a.list != b.list ? std::less<>()(a.list, b.list) : a.index < b.index;
  });
  for (size_t i = 0; i < picks.size();) {
    size_t j = i;
    while (j < picks.size() && picks[j].list == picks[i].list) ++j;
    uint32_t at = picks[j - 1].index + 1;
    for (size_t k = i; k < j; ++k) {
      DevicePtr d = cloneDevice(*(*picks[k].list)[picks[k].index], ids);
      e.devices.push_back(d->id);
      batchOf(e).commands.push_back(AddDevice{picks[i].track, std::move(d), at++, picks[i].chain});
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

EditResult editDevicePatch(const Project& p, DeviceId device, std::string text, std::shared_ptr<const graph::PatchFile> ids) {
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  if (l.device->rack) return invalid("a rack has no patch text (its chains are document structure)");  // T4.8
  if (text.empty()) return invalid("a device patch text must not be empty");
  Edit e = makeEdit("Edit Device Patch");
  if (l.device->source && l.device->source->text == text) return e;  // unchanged: nothing to undo
  e.devices.push_back(device);
  e.tracks.push_back(l.track->id);
  auto src = std::make_shared<UserPatch>();
  src->text = std::move(text);
  src->ids = std::move(ids);
  batchOf(e).commands.push_back(SetDevicePatch{device, std::move(src)});
  return e;
}

EditResult revertDevicesToStock(const Project& p, std::span<const DeviceId> devices) {
  Edit e = makeEdit("Revert to Stock");
  std::vector<DeviceId> seen;
  for (const DeviceId id : devices) {
    if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
    const DeviceLoc l = findDevice(p, id);
    if (!l.track) return notFound("device not found");
    seen.push_back(id);
    if (!l.device->edited()) continue;
    e.devices.push_back(id);
    batchOf(e).commands.push_back(SetDevicePatch{id, nullptr});
  }
  return e;
}

// ---- Modulation wires (T4.7) --------------------------------------------------------------------------------------

EditResult connectWire(const Project& p, IdSource& ids, DeviceId from, std::string port, DeviceId to, std::string param,
                       double amount) {
  if (!findDevice(p, from).track) return notFound("wire source device not found");
  if (!findDevice(p, to).track) return notFound("wire target device not found");
  for (const Wire& w : wiresOf(p))
    if (w.from == from && w.port == port && w.to == to && w.param == param) return invalid("these two ports are already wired");
  Wire w;
  w.id = ids.wire();
  w.from = from;
  w.port = std::move(port);
  w.to = to;
  w.param = std::move(param);
  w.amount = std::isfinite(amount) ? std::clamp(amount, -1.0, 1.0) : kDefaultWireAmount;
  PQ_DOC_TRY(checkWire(w));
  Edit e = makeEdit("Add Wire");
  e.wires.push_back(w.id);
  batchOf(e).commands.push_back(AddWire{std::move(w)});
  return e;
}

EditResult deleteWires(const Project& p, std::span<const WireId> wires) {
  Edit e = makeEdit(wires.size() == 1 ? "Delete Wire" : "Delete Wires");
  std::vector<WireId> seen;
  for (const WireId id : wires) {
    if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
    if (!findWire(p, id)) return notFound("wire not found");
    seen.push_back(id);
    batchOf(e).commands.push_back(RemoveWire{id});
  }
  return e;
}

EditResult setWireAmount(const Project& p, WireId wire, double amount) {
  if (!findWire(p, wire)) return notFound("wire not found");
  if (!std::isfinite(amount)) return invalid("wire amount is not finite");
  Edit e = makeEdit("Wire Amount");
  e.wires.push_back(wire);
  batchOf(e).commands.push_back(SetWireAmount{wire, std::clamp(amount, -1.0, 1.0)});
  return e;
}

EditResult setWireReducer(const Project& p, WireId wire, WireReducer reducer) {
  if (!findWire(p, wire)) return notFound("wire not found");
  Edit e = makeEdit("Wire Reducer");
  e.wires.push_back(wire);
  batchOf(e).commands.push_back(SetWireReducer{wire, reducer});
  return e;
}

std::vector<WireId> wiresInto(const Project& p, DeviceId device, std::string_view param) {
  std::vector<WireId> out;
  for (const Wire& w : wiresOf(p))
    if (w.to == device && w.param == param) out.push_back(w.id);
  return out;
}

std::vector<WireId> wiresFrom(const Project& p, DeviceId device, std::string_view port) {
  std::vector<WireId> out;
  for (const Wire& w : wiresOf(p))
    if (w.from == device && w.port == port) out.push_back(w.id);
  return out;
}

}  // namespace pq::doc::ops
