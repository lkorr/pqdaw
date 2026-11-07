// Device edit builders (T4.1; inside racks T4.8): pq/document/edit_ops.h, "Devices".
#include <algorithm>
#include <cmath>
#include <optional>
#include <tuple>
#include <unordered_map>

#include "edit_common.h"
#include "edit_paths.h"
#include "pq/document/devices.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"

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
    // TMM.10-fix1: a chain meta device first is not replaced: it follows the instrument (its chain input then takes
    // the instrument's signal; AddDevice makes it an audio effect).
    return Slot{0, hasInstrument && !isMetaDevice(*d.front())};
  }
  const auto n = static_cast<uint32_t>(d.size());
  uint32_t at = std::min(index, n);
  if (hasInstrument && at == 0) at = 1;
  return Slot{at, false};
}

// TMM.10-fix1: where a chain meta device lands for `index`: exactly there. First where instruments go it takes the note
// source; a stock instrument or Instrument Rack there takes no chain signal (it could not follow it), so it is
// replaced; a chain meta device there moves on (AddDevice re-kinds it).
uint32_t metaSlotAt(const ListView& l, uint32_t index) {
  return std::min(index, static_cast<uint32_t>(l.devices->size()));
}
bool metaReplacesInstrument(const ListView& l, uint32_t at) {
  const std::vector<DevicePtr>& d = *l.devices;
  return at == 0 && l.instrumentAllowed && !d.empty() && d.front()->kind == DeviceKind::Instrument &&
         !isMetaDevice(*d.front());
}

// TX.all-free: a new block `dev` (already added to `track`) onto its main path at `slot`. `notes`: a stock instrument
// (it takes the notes: slot 0). A stock instrument or Instrument Rack heading the path is replaced by an instrument, or
// by a meta module landing at slot 0 (it takes the notes: TMM.10-fix1's rule); an effect at slot 0 goes after it.
Status placeOnPath(Steps& k, IdSource& ids, TrackId track, DeviceId dev, bool notes, bool meta, uint32_t slot) {
  const Track* t = findTrackPtr(k.doc(), track);
  if (t == nullptr) return notFound("track not found");
  const MainPath path = mainPath(k.doc(), *t);
  const Device* head = path.blocks.empty() ? nullptr : path.blocks.front();
  const bool stockHead = head != nullptr && head->kind == DeviceKind::Instrument && !isMetaDevice(*head);
  if (notes) slot = 0;
  // IG.1-2: a MIDI track headed by its instrument group — an instrument goes INTO the group, taking the place of the
  // stock instrument there (its cables, In → it → Out, move to the new one), or wired In → it → Out when there is none;
  // anything else at slot 0 goes after the group.
  if (head != nullptr && isGroupDevice(*head) && t->kind == TrackKind::Midi) {
    // A meta module where the instrument is (slot 0), or appended while the group is all the path holds, builds the
    // instrument: it goes into the group — in place of its stock instrument at slot 0, else at the end of its series
    // (into what reached Out; it now feeds Out).
    const bool intoGroup = meta && (slot == 0 || (slot >= path.blocks.size() && path.blocks.size() == 1));
    if (!notes && !intoGroup) return splice(k, ids, track, dev, slot == 0 ? 1 : slot);
    if (!notes) {
      const DeviceId g = head->id;
      DeviceId stock;
      uint32_t index = 0;  // (the stock instrument's place in the device list: the new one takes it)
      for (uint32_t i = 0; i < t->devices.size(); ++i) {
        const Device& d = *t->devices[i];
        if (d.group == g && d.kind == DeviceKind::Instrument && !isMetaDevice(d)) {
          stock = d.id;
          index = i;
        }
      }
      PQ_DOC_TRY(k.add(SetDeviceGroup{dev, g}));
      // (its ports by name, as Ctrl+G's series names them: ungroup reads the series back)
      std::string chainIn, firstOut;
      if (const DeviceLoc dl = findDevice(k.doc(), dev); dl.device != nullptr)
        if (const meta::Type* ty = meta::findType(metaDeviceType(*dl.device))) {
          if (const int32_t ci = metaChainInput(*dl.device, *ty); ci >= 0) chainIn = std::string(ty->inputs[static_cast<size_t>(ci)].name);
          if (!ty->outputs.empty()) firstOut = std::string(ty->outputs.front().name);
        }
      if (slot == 0 && stock.valid()) {
        const DeviceId gone = stock;
        std::vector<Wire> moved;
        for (const Wire& w : wiresOf(k.doc()))
          if ((!w.fromNoteSource() && w.from == gone) || (!w.toMixer() && w.to == gone)) moved.push_back(w);
        PQ_DOC_TRY(k.add(RemoveDevice{gone}));
        PQ_DOC_TRY(k.add(MoveDevice{dev, track, index, {}}));
        for (Wire w : moved) {
          w.id = ids.wire();
          if (!w.fromNoteSource() && w.from == gone) {
            w.from = dev;
            w.port = firstOut;
          }
          if (!w.toMixer() && w.to == gone) {
            w.to = dev;
            w.param = chainIn;  // (the notes from In reach its chain input)
          }
          PQ_DOC_TRY(k.add(AddWire{w}));
        }
        return ok();
      }
      std::optional<Wire> last;
      for (const Wire& w : wiresOf(k.doc()))
        if (!w.toMixer() && w.to == g && w.param == kGroupOutPort) last = w;
      Wire in;
      in.id = ids.wire();
      in.chain = true;
      in.to = dev;
      in.param = chainIn;
      if (last) {
        PQ_DOC_TRY(k.add(RemoveWire{last->id}));
        in.from = last->from;
        in.noteSource = last->noteSource;
        in.port = last->port;
      } else {
        in.from = g;
        in.port = std::string(kGroupInPort);
      }
      PQ_DOC_TRY(k.add(AddWire{in}));
      Wire out;
      out.id = ids.wire();
      out.chain = true;
      out.from = dev;
      out.port = firstOut;
      out.to = g;
      out.param = std::string(kGroupOutPort);
      PQ_DOC_TRY(k.add(AddWire{out}));
      return ok();
    }
    const DeviceId g = head->id;
    const Device* old = nullptr;
    for (const DevicePtr& d : t->devices)
      if (d->group == g && d->kind == DeviceKind::Instrument && !isMetaDevice(*d)) {
        old = d.get();
        break;
      }
    PQ_DOC_TRY(k.add(SetDeviceGroup{dev, g}));
    if (old != nullptr) {
      const DeviceId gone = old->id;
      std::vector<Wire> moved;
      for (const Wire& w : wiresOf(k.doc()))
        if ((!w.fromNoteSource() && w.from == gone) || (!w.toMixer() && w.to == gone)) moved.push_back(w);
      PQ_DOC_TRY(k.add(RemoveDevice{gone}));
      for (Wire w : moved) {
        w.id = ids.wire();
        if (!w.fromNoteSource() && w.from == gone) w.from = dev;
        if (!w.toMixer() && w.to == gone) w.to = dev;
        PQ_DOC_TRY(k.add(AddWire{w}));
      }
      return ok();
    }
    for (const auto& [from, port, to, param] : {std::tuple{g, kGroupInPort, dev, kMainPort}, std::tuple{dev, kMainPort, g, kGroupOutPort}}) {
      Wire w;
      w.id = ids.wire();
      w.from = from;
      w.port = std::string(port);
      w.to = to;
      w.param = std::string(param);
      w.chain = true;
      PQ_DOC_TRY(k.add(AddWire{w}));
    }
    return ok();
  }
  if (stockHead && slot == 0 && (notes || meta) && t->kind == TrackKind::Midi) {
    const DeviceId gone = head->id;
    PQ_DOC_TRY(detach(k, ids, nullptr, gone));
    PQ_DOC_TRY(k.add(RemoveDevice{gone}));
  } else if (stockHead && slot == 0) {
    slot = 1;
  }
  return splice(k, ids, track, dev, slot);
}

}  // namespace

// M4 review M9: iterative (explicit stacks), in the recursive version's id order: devices and chains pre-order (a
// device, then per chain its id and its devices), then the mappings pre-order (a rack's own, then its chains' racks).
DevicePtr cloneDevice(const Device& d, IdSource& ids) {
  std::unordered_map<uint32_t, DeviceId> remap;  // old device id → new (racks: mapping targets follow)
  auto fresh = [&](const Device& src) {
    auto c = std::make_shared<Device>(src);
    c->id = ids.device();
    remap[src.id.value] = c->id;
    return c;
  };
  // A rack being copied: its fresh rack, its next chain, the chain being filled and its next device.
  struct Open {
    std::shared_ptr<Device> device;
    std::shared_ptr<Rack> rack;
    size_t chain = 0;
    std::shared_ptr<Chain> current;
    size_t next = 0;
  };
  std::shared_ptr<Device> out = fresh(d);
  std::vector<Open> stack;
  if (out->rack) stack.push_back(Open{out, std::make_shared<Rack>(*out->rack)});
  while (!stack.empty()) {
    Open& f = stack.back();
    if (f.current && f.next < f.current->devices.size()) {
      DevicePtr& slot = f.current->devices[f.next++];
      std::shared_ptr<Device> c = fresh(*slot);
      slot = c;
      if (c->rack) stack.push_back(Open{c, std::make_shared<Rack>(*c->rack)});  // invalidates f
      continue;
    }
    if (f.current) f.rack->chains[f.chain - 1] = std::move(f.current);
    if (f.chain < f.rack->chains.size()) {
      f.current = std::make_shared<Chain>(*f.rack->chains[f.chain++]);
      f.current->id = ids.chain();
      f.next = 0;
      continue;
    }
    f.device->rack = std::move(f.rack);
    stack.pop_back();
  }
  // Mappings after every device has its new id (the tree is this function's own: its racks are rewritten in place,
  // as fresh copies).
  std::vector<Device*> todo{out.get()};
  while (!todo.empty()) {
    Device& dev = *todo.back();
    todo.pop_back();
    if (!dev.rack) continue;
    auto r = std::make_shared<Rack>(*dev.rack);
    for (MacroMapping& m : r->mappings) {
      m.id = ids.mapping();
      if (const auto it = remap.find(m.device.value); it != remap.end()) m.device = it->second;
    }
    std::vector<Device*> inner;
    for (ChainPtr& ch : r->chains) {
      auto cc = std::make_shared<Chain>(*ch);
      for (DevicePtr& x : cc->devices) {
        auto xc = std::make_shared<Device>(*x);
        inner.push_back(xc.get());
        x = std::move(xc);
      }
      ch = std::move(cc);
    }
    dev.rack = std::move(r);
    todo.insert(todo.end(), inner.rbegin(), inner.rend());  // the first inner device next (pre-order)
  }
  return out;
}

EditResult addDevice(const Project& p, IdSource& ids, TrackId track, std::string patch, DeviceKind kind, uint32_t index,
                     ChainId chain) {
  Result<ListView> l = listOf(p, track, chain);
  if (!l) return std::move(l).error();
  if (patch.empty()) return invalid("no device patch");
  const bool rack = patch == kAudioEffectRack || patch == kInstrumentRack;
  if (rack && (patch == kInstrumentRack) != (kind == DeviceKind::Instrument)) return invalid("rack kind mismatch");
  Edit e = makeEdit(rack ? "Add Rack" : "Add Device");
  if (chain.valid()) {  // a rack chain: a list (its rule)
    Result<Slot> slot = slotFor(*l, kind, index);
    if (!slot) return std::move(slot).error();
    if (slot->replacesInstrument) batchOf(e).commands.push_back(RemoveDevice{l->devices->front()->id});
    DevicePtr d = rack ? makeRack(ids.device(), kind) : makeDevice(ids.device(), std::move(patch), kind);
    e.devices.push_back(d->id);
    e.tracks.push_back(l->track);
    batchOf(e).commands.push_back(AddDevice{l->track, std::move(d), slot->index, chain});
    return e;
  }
  // TX.all-free: a track's block, spliced into its main path at slot `index` (kOffPath: unwired).
  if (kind == DeviceKind::Instrument && !l->instrumentAllowed) return invalid("instruments go on MIDI tracks");
  DevicePtr d = rack ? makeRack(ids.device(), kind) : makeDevice(ids.device(), std::move(patch), kind);
  const DeviceId id = d->id;
  e.devices.push_back(id);
  e.tracks.push_back(l->track);
  Steps k(p);
  PQ_DOC_TRY(k.add(AddDevice{l->track, std::move(d), kAppend}));
  if (index != kOffPath) PQ_DOC_TRY(placeOnPath(k, ids, l->track, id, kind == DeviceKind::Instrument, false, index));
  batchOf(e).commands = std::move(k.commands);
  return e;
}

// TMM.3.
Result<DeviceKind> metaDeviceKindAt(const Project& p, const meta::Type& t, TrackId track, uint32_t index, ChainId chain) {
  Result<ListView> l = listOf(p, track, chain);
  if (!l) return std::move(l).error();
  // TMM.10-fix1: any type goes anywhere; in a rack chain its kind is its position (model.h "Meta modules"): the
  // instrument when it lands first in an Instrument Rack's chain, else an audio effect. TX.all-free: on a track the
  // kind means nothing (its cables decide): an audio effect.
  (void)t;
  if (!chain.valid()) return DeviceKind::AudioEffect;
  return metaSlotAt(*l, index) == 0 && l->instrumentAllowed ? DeviceKind::Instrument : DeviceKind::AudioEffect;
}

EditResult addMetaDevice(const Project& p, IdSource& ids, TrackId track, std::string_view type, uint32_t index,
                         ChainId chain) {
  const meta::Type* t = meta::findType(type);
  if (t == nullptr) return invalid("unknown meta module type '" + std::string(type) + "'");
  Result<ListView> l = listOf(p, track, chain);
  if (!l) return std::move(l).error();
  Edit e = makeEdit("Add Device");
  // What reaches its main input there (a rack chain: its list; a track: its main path's blocks up to the slot).
  std::vector<DevicePtr> before = *l->devices;
  uint32_t at = metaSlotAt(*l, index);
  if (!chain.valid()) {
    const Track* tr = findTrackPtr(p, l->track);
    const MainPath path = mainPath(p, *tr);
    before = flowOrder(*tr, path);
    before.resize(path.blocks.size());
    at = index == kOffPath ? 0 : std::min<uint32_t>(index, static_cast<uint32_t>(path.blocks.size()));
  }
  const DeviceKind kind = chain.valid() && at == 0 && l->instrumentAllowed ? DeviceKind::Instrument : DeviceKind::AudioEffect;
  auto d = std::make_shared<Device>(*makeDevice(ids.device(), std::string(kMetaDevicePrefix) + std::string(t->id), kind));
  d->meta = std::make_shared<const MetaMap>(MetaMap{MetaEntry{"", defaultMetaInstance(*t)}});
  // TMM.8 (docs/plans/MM.md): a Wavetable that receives audio starts in Shape mode (a waveshaper); one that receives the
  // note's phase or a voice signal (after a Phase accumulator: TMM.10) reads it as a phase in Phase mode. (Off the path:
  // as at its start.)
  if (chainSignalAt(before, l->instrumentAllowed, at) == ChainSignal::Audio && t->id == "wavetable" &&
      t->settingIndex("mode") >= 0)
    d->settings.push_back(DeviceValue{"mode", 1.0});
  const DeviceId id = d->id;
  e.devices.push_back(id);
  e.tracks.push_back(l->track);
  if (chain.valid()) {
    if (metaReplacesInstrument(*l, at)) batchOf(e).commands.push_back(RemoveDevice{l->devices->front()->id});
    batchOf(e).commands.push_back(AddDevice{l->track, std::move(d), at, chain});
    return e;
  }
  Steps k(p);
  PQ_DOC_TRY(k.add(AddDevice{l->track, std::move(d), kAppend}));
  if (index != kOffPath) PQ_DOC_TRY(placeOnPath(k, ids, l->track, id, false, true, index));
  batchOf(e).commands = std::move(k.commands);
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

namespace {
// TX.groups (model.h "Groups"): a copy of device `src` for a paste or a duplicate. A copied member is no member (its
// group is not copied with it): a free device. A copied group brings copies of its members (from `p`, appended to the
// destination list after it) and of the wires among them and its terminals — the group as it is, one step.
DevicePtr copyForPaste(const Project& p, IdSource& ids, const Device& src, TrackId track, ChainId chain, Edit& e,
                       std::vector<Command>& after) {
  DevicePtr d = cloneDevice(src, ids);
  if (d->group.valid()) {
    auto x = std::make_shared<Device>(*d);
    x->group = {};
    d = std::move(x);
  }
  if (!isGroupDevice(src) || chain.valid()) return d;
  const DeviceLoc g = findDevice(p, src.id);
  if (g.track == nullptr) return d;  // the group is gone: an empty one
  std::vector<std::pair<DeviceId, DeviceId>> remap{{src.id, d->id}};
  for (const Device* m : groupMembers(g.track->devices, src.id)) {
    auto c = std::make_shared<Device>(*cloneDevice(*m, ids));
    c->group = d->id;
    remap.emplace_back(m->id, c->id);
    e.devices.push_back(c->id);
    after.push_back(AddDevice{track, std::move(c), kAppend, {}});
  }
  auto mapped = [&](DeviceId id) {
    for (const auto& [a, b] : remap)
      if (a == id) return b;
    return DeviceId{};
  };
  for (const Wire& w : wiresOf(p)) {
    if (w.fromNoteSource()) continue;
    const DeviceId a = mapped(w.from), b = mapped(w.to);
    if (!a.valid() || !b.valid()) continue;
    Wire c = w;
    c.id = ids.wire();
    c.from = a;
    c.to = b;
    after.push_back(AddWire{c});
  }
  return d;
}
}  // namespace

EditResult pasteDevices(const Project& p, IdSource& ids, TrackId track, std::span<const DevicePtr> devices,
                        uint32_t index, ChainId chain) {
  Result<ListView> l = listOf(p, track, chain);
  if (!l) return std::move(l).error();
  Edit e = makeEdit(devices.size() == 1 ? "Paste Device" : "Paste Devices");
  e.tracks.push_back(l->track);
  const std::vector<DevicePtr>& list = *l->devices;
  bool instrumentPasted = false;
  std::vector<Command> after;  // TX.groups: a pasted group's members and wires, after the devices
  if (chain.valid()) {  // a rack chain: a list
    const bool hasInstrument = !list.empty() && list.front()->kind == DeviceKind::Instrument;
    uint32_t at = std::min<uint32_t>(index, static_cast<uint32_t>(list.size()));
    if (hasInstrument && at == 0) at = 1;
    for (const DevicePtr& src : devices) {
      if (!src) continue;
      DevicePtr d = copyForPaste(p, ids, *src, l->track, chain, e, after);
      e.devices.push_back(d->id);
      if (d->kind == DeviceKind::Instrument) {
        if (!l->instrumentAllowed) return invalid("an Audio Effect Rack's chains hold audio effects only");
        if (instrumentPasted) return invalid("a chain holds one instrument");
        instrumentPasted = true;
        if (hasInstrument) batchOf(e).commands.push_back(RemoveDevice{list.front()->id});
        batchOf(e).commands.push_back(AddDevice{l->track, std::move(d), 0, chain});
        if (!hasInstrument) ++at;  // the effects pasted so far moved one place right
        continue;
      }
      batchOf(e).commands.push_back(AddDevice{l->track, std::move(d), at++, chain});
    }
    for (Command& c : after) batchOf(e).commands.push_back(std::move(c));
    return e;
  }
  // TX.all-free: a track's blocks, spliced into its main path from slot `index` on, in order (kOffPath: unwired).
  Steps k(p);
  uint32_t slot = index;
  for (const DevicePtr& src : devices) {
    if (!src) continue;
    DevicePtr d = copyForPaste(p, ids, *src, l->track, chain, e, after);
    const DeviceId id = d->id;
    const bool notes = d->kind == DeviceKind::Instrument && !isMetaDevice(*d);
    const bool meta = isMetaDevice(*d);
    e.devices.push_back(id);
    if (d->kind == DeviceKind::Instrument && !l->instrumentAllowed) return invalid("instruments go on MIDI tracks");
    if (notes && instrumentPasted) return invalid("one instrument takes a track's notes (paste the others one by one)");
    instrumentPasted = instrumentPasted || notes;
    PQ_DOC_TRY(k.add(AddDevice{l->track, std::move(d), kAppend}));
    if (index == kOffPath) continue;
    PQ_DOC_TRY(placeOnPath(k, ids, l->track, id, notes, meta, slot));
    // The next one goes after it.
    const MainPath path = mainPath(k.doc(), *findTrackPtr(k.doc(), l->track));
    if (const int32_t i = path.indexOf(id); i >= 0) slot = static_cast<uint32_t>(i) + 1;
  }
  for (Command& c : after) PQ_DOC_TRY(k.add(c));
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult duplicateDevices(const Project& p, IdSource& ids, std::span<const DeviceId> devices) {
  Edit e = makeEdit(devices.size() == 1 ? "Duplicate Device" : "Duplicate Devices");
  // Per list, in its order: the copies go right after the last selected device of that list (TX.all-free: on a track,
  // right after the last selected block on its main path; blocks off the path are copied off it).
  struct Pick {
    const std::vector<DevicePtr>* list;
    TrackId track;
    ChainId chain;
    uint32_t index;  // in the list (a track: its place on the main path, kOffPath off it)
    const Device* device;
  };
  std::vector<Pick> picks;
  for (const DeviceId id : devices) {
    const DeviceLoc d = findDevice(p, id);
    if (!d.track) return notFound("device not found");
    if (d.device->kind == DeviceKind::Instrument) continue;  // one instrument per list
    const std::vector<DevicePtr>* list = &d.list();
    uint32_t index = d.index;
    if (d.chain == nullptr) {
      const int32_t i = mainPath(p, *d.track).indexOf(id);
      index = i >= 0 ? static_cast<uint32_t>(i) : kOffPath;
    }
    if (std::none_of(picks.begin(), picks.end(), [&](const Pick& k) { return k.device == d.device; }))
      picks.push_back(Pick{list, d.track->id, d.chain ? d.chain->id : ChainId{}, index, d.device});
  }
  std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) {
    if (a.track != b.track) return a.track < b.track;
    return a.list != b.list ? std::less<>()(a.list, b.list) : a.index < b.index;
  });
  Steps k(p);
  for (size_t i = 0; i < picks.size();) {
    size_t j = i;
    while (j < picks.size() && picks[j].list == picks[i].list) ++j;
    std::vector<Command> after;  // TX.groups: a duplicated group's members and wires
    if (picks[i].chain.valid()) {
      uint32_t at = picks[j - 1].index + 1;
      for (size_t q = i; q < j; ++q) {
        DevicePtr d = copyForPaste(p, ids, *picks[q].device, picks[i].track, picks[i].chain, e, after);
        e.devices.push_back(d->id);
        PQ_DOC_TRY(k.add(AddDevice{picks[i].track, std::move(d), at++, picks[i].chain}));
      }
    } else {
      // The path's copies after the last selected block on it, in path order; the others off the path.
      uint32_t last = 0;
      bool any = false;
      for (size_t q = i; q < j; ++q)
        if (picks[q].index != kOffPath) {
          last = std::max(last, picks[q].index);
          any = true;
        }
      uint32_t slot = last + 1;
      for (size_t q = i; q < j; ++q) {
        DevicePtr d = copyForPaste(p, ids, *picks[q].device, picks[i].track, {}, e, after);
        const DeviceId id = d->id;
        e.devices.push_back(id);
        PQ_DOC_TRY(k.add(AddDevice{picks[i].track, std::move(d), kAppend}));
        if (!any || picks[q].index == kOffPath) continue;
        PQ_DOC_TRY(splice(k, ids, picks[i].track, id, slot++));
      }
    }
    for (Command& c : after) PQ_DOC_TRY(k.add(c));
    i = j;
  }
  batchOf(e).commands = std::move(k.commands);
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

EditResult renameDevice(const Project& p, DeviceId device, std::string name) {
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  Edit e = makeEdit("Rename Device");
  e.devices.push_back(device);
  if (l.device->name != name) batchOf(e).commands.push_back(RenameDevice{device, std::move(name)});
  return e;
}

EditResult setDeviceChainInput(const Project& p, DeviceId device, std::string input) {  // TMM.10-fix1
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  if (!isMetaDevice(*l.device)) return invalid("only a chain meta device chooses its chain input");
  const meta::Type* t = meta::findType(metaDeviceType(*l.device));
  if (t == nullptr) return invalid("unknown meta module type");
  if (!input.empty() && input != kChainUnplugged && t->inputIndex(input) < 0)
    return invalid("a " + std::string(t->name) + " has no input '" + input + "'");
  if (!input.empty() && input != kChainUnplugged && t->inputIndex(input) == t->chainInput) input.clear();  // the default
  Edit e = makeEdit("Chain Input");
  e.devices.push_back(device);
  e.tracks.push_back(l.track->id);
  if (l.device->chainInput != input) batchOf(e).commands.push_back(SetDeviceChainInput{device, std::move(input)});
  return e;
}

namespace {
// GU.1: the declaration of `device`'s param `param` in the patch it plays (null: none).
const graph::ParamDecl* declOf(const DeviceLibrary& lib, const Device& d, std::string_view param,
                               std::shared_ptr<const ResolvedDevice>& hold) {
  const graph::Patch* callee = lib.patchOf(d, hold);
  return callee != nullptr ? findAnyParamDecl(*callee, param) : nullptr;
}
}  // namespace

EditResult setParamCurve(const Project& p, const DeviceLibrary& lib, DeviceId device, std::string param,
                         std::optional<dsp::Curve> curve) {  // GU.1
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  if (l.device->rack) return invalid("a rack's macros have no knob curve");
  if (isGroupDevice(*l.device)) return invalid("a group has no knobs");
  if (param.empty()) return invalid("no param");
  std::shared_ptr<const ResolvedDevice> hold;
  const graph::ParamDecl* decl = declOf(lib, *l.device, param, hold);
  if (decl == nullptr) return notFound("the device has no param '" + param + "'");
  if (curve && !dsp::validCurve(*curve)) return invalid("not a valid curve");
  const dsp::Curve declared = declaredCurve(*decl);
  if (curve && *curve == declared) curve.reset();
  const dsp::Curve before = controlCurve(*l.device, param, *decl);
  const dsp::Curve after = curve ? *curve : declared;
  Edit e = makeEdit("Knob Curve");
  e.devices.push_back(device);
  e.tracks.push_back(l.track->id);
  const dsp::Curve* stored = l.device->curve(param);
  const bool structural = dsp::curveClass(before) != dsp::curveClass(after);
  if (stored == nullptr ? curve.has_value() : !(curve && *stored == *curve))
    batchOf(e).commands.push_back(SetParamCurve{device, std::move(param), std::move(curve), structural});
  return e;
}

EditResult setParamCurvePreset(const Project& p, const DeviceLibrary& lib, DeviceId device, std::string param,
                               dsp::CurvePreset preset) {
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  std::shared_ptr<const ResolvedDevice> hold;
  const graph::ParamDecl* decl = l.device->rack ? nullptr : declOf(lib, *l.device, param, hold);
  if (decl == nullptr) return setParamCurve(p, lib, device, std::move(param), std::nullopt);  // its refusal
  return setParamCurve(p, lib, device, std::move(param), curveWithPreset(declaredCurve(*decl), preset));
}

EditResult setDeviceParamValue(const Project& p, const DeviceLibrary& lib, DeviceId device, std::string param, double value) {
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  if (l.device->rack) return setDeviceParam(p, device, std::move(param), value);
  std::shared_ptr<const ResolvedDevice> hold;
  const graph::ParamDecl* decl = declOf(lib, *l.device, param, hold);
  if (decl == nullptr) return notFound("the device has no param '" + param + "'");
  const double now = paramPosition(*l.device, param, *decl);
  const double pos = dsp::curvePosition(controlCurve(*l.device, param, *decl), value, now);
  return setDeviceParam(p, device, std::move(param), pos);
}

// ---- Modulation wires (T4.7) --------------------------------------------------------------------------------------

namespace {
// The "Add Wire" edit of `w` (its ends and law set; the source checked by the caller): a fresh id; a Travel wire's
// amount clamped.
EditResult addWire(const Project& p, IdSource& ids, Wire w, double amount) {
  if (w.toMixer()) {  // TX.all-free: a track's mixer input
    if (findTrackPtr(p, w.mixer) == nullptr) return notFound("wire target track not found");
  } else if (!findDevice(p, w.to).track) {
    return notFound("wire target device not found");
  }
  for (const Wire& x : wiresOf(p))
    if (x.sameEnds(w)) return invalid("these two ports are already wired");
  w.id = ids.wire();
  w.amount = !w.travels() ? 1.0 : std::isfinite(amount) ? std::clamp(amount, -1.0, 1.0) : kDefaultWireAmount;
  PQ_DOC_TRY(checkWire(w));
  Edit e = makeEdit("Add Wire");
  e.wires.push_back(w.id);
  batchOf(e).commands.push_back(AddWire{std::move(w)});
  return e;
}

EditResult deviceWire(const Project& p, IdSource& ids, DeviceId from, std::string port, DeviceId to, std::string param,
                      WireLaw law, double amount) {
  if (!findDevice(p, from).track) return notFound("wire source device not found");
  Wire w;
  w.from = from;
  w.port = std::move(port);
  w.to = to;
  w.param = std::move(param);
  w.law = law;
  return addWire(p, ids, std::move(w), amount);
}

EditResult noteWire(const Project& p, IdSource& ids, TrackId track, std::string port, DeviceId to, std::string param,
                    WireLaw law, double amount) {
  const Track* t = findTrackPtr(p, track);
  if (t == nullptr) return notFound("wire source track not found");
  if (t->kind != TrackKind::Midi && !port.empty()) return invalid("only a MIDI track has a note source");
  Wire w;
  w.noteSource = track;
  w.port = std::move(port);
  w.to = to;
  w.param = std::move(param);
  w.law = law;
  return addWire(p, ids, std::move(w), amount);
}
}  // namespace

EditResult connectWire(const Project& p, IdSource& ids, DeviceId from, std::string port, DeviceId to, std::string param) {
  return deviceWire(p, ids, from, std::move(port), to, std::move(param), WireLaw::Add, 1.0);
}

EditResult connectNoteWire(const Project& p, IdSource& ids, TrackId track, std::string port, DeviceId to, std::string param) {
  return noteWire(p, ids, track, std::move(port), to, std::move(param), WireLaw::Add, 1.0);
}

EditResult connect(const Project& p, IdSource& ids, Wire ends) {  // TX.all-free
  if (ends.fromNoteSource()) {
    const Track* t = findTrackPtr(p, ends.noteSource);
    if (t == nullptr) return notFound("wire source track not found");
    if (t->kind != TrackKind::Midi && !ends.port.empty()) return invalid("only a MIDI track has a note source");
  } else if (!findDevice(p, ends.from).track) {
    return notFound("wire source device not found");
  }
  ends.law = WireLaw::Add;
  ends.chain = false;
  return addWire(p, ids, std::move(ends), 1.0);
}

EditResult connectTravelWire(const Project& p, IdSource& ids, DeviceId from, std::string port, DeviceId to,
                             std::string param, double amount) {
  return deviceWire(p, ids, from, std::move(port), to, std::move(param), WireLaw::Travel, amount);
}

EditResult connectTravelNoteWire(const Project& p, IdSource& ids, TrackId track, std::string port, DeviceId to,
                                 std::string param, double amount) {
  return noteWire(p, ids, track, std::move(port), to, std::move(param), WireLaw::Travel, amount);
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
  const Wire* w = findWire(p, wire);
  if (!w) return notFound("wire not found");
  if (!w->travels()) return invalid("a wire that adds in its target's units has no amount (scale its signal with a VCA)");
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

EditResult setGroupPolyphony(const Project& p, DeviceId group, const GroupPolyphony& poly) {
  const DeviceLoc l = findDevice(p, group);
  if (!l.device) return notFound("group not found");
  if (!isGroupDevice(*l.device)) return invalid("only a group has polyphony settings");
  Edit e = makeEdit("Polyphony");
  e.devices.push_back(group);
  Batch& b = batchOf(e);
  b.commands.push_back(SetDeviceSetting{group, std::string(kGroupVoices),
                                        poly.voices == 0 ? std::optional<double>() : std::optional<double>(poly.voices)});
  b.commands.push_back(SetDeviceSetting{group, std::string(kGroupMono), poly.mono ? 1.0 : 0.0});
  b.commands.push_back(SetDeviceSetting{group, std::string(kGroupLegato), poly.legato ? 1.0 : 0.0});
  b.commands.push_back(SetDeviceSetting{group, std::string(kGroupGlide), poly.glide});
  b.commands.push_back(SetDeviceSetting{group, std::string(kGroupSteal), static_cast<double>(poly.steal)});
  return e;
}

EditResult setWireSignal(const Project& p, WireId wire, bool single, WireReducer reducer) {
  if (!findWire(p, wire)) return notFound("wire not found");
  Edit e = makeEdit(single ? "Single-Signal Cable" : "Per-Note Cable");
  e.wires.push_back(wire);
  batchOf(e).commands.push_back(SetWireSignal{wire, single, reducer});
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
