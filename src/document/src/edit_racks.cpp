// Rack edit builders (T4.8): pq/document/edit_ops.h, "Racks".
#include <algorithm>
#include <cmath>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

Result<const Device*> rackOf(const Project& p, DeviceId id) {
  const DeviceLoc d = findDevice(p, id);
  if (!d.device) return notFound("rack not found");
  if (!d.device->rack) return invalid("the device is not a rack");
  return d.device;
}

// "Chain <n>": the lowest n not taken by another chain of the rack.
std::string freshChainName(const Rack& r) {
  for (uint32_t n = 1;; ++n) {
    const std::string name = "Chain " + std::to_string(n);
    if (std::none_of(r.chains.begin(), r.chains.end(), [&](const ChainPtr& c) { return c->name == name; })) return name;
  }
}

EditResult withOptions(const Project& p, DeviceId rack, const char* label, auto&& edit) {
  Result<const Device*> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  RackOptions o = rackOptions(*(*r)->rack);
  PQ_DOC_TRY(edit(o));
  Edit e = makeEdit(label);
  e.devices.push_back(rack);
  if (!(o == rackOptions(*(*r)->rack))) batchOf(e).commands.push_back(SetRackOptions{rack, std::move(o)});
  return e;
}

// splitmix64: a small reproducible generator for Rand.
uint64_t splitmix(uint64_t& s) noexcept {
  uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // namespace

EditResult groupDevices(const Project& p, IdSource& ids, std::span<const DeviceId> devices) {
  if (devices.empty()) return invalid("nothing to group");
  const DeviceLoc first = findDevice(p, devices.front());
  if (!first.device) return notFound("device not found");
  const std::vector<DevicePtr>& list = first.list();
  std::vector<uint32_t> picked;
  for (const DeviceId id : devices) {
    const DeviceLoc d = findDevice(p, id);
    if (!d.device) return notFound("device not found");
    if (&d.list() != &list) return invalid("group devices of one chain at a time");
    if (std::find(picked.begin(), picked.end(), d.index) == picked.end()) picked.push_back(d.index);
  }
  std::sort(picked.begin(), picked.end());
  std::vector<DevicePtr> inner;
  for (const uint32_t i : picked) inner.push_back(list[i]);
  const bool instrument = inner.front()->kind == DeviceKind::Instrument;
  Edit e = makeEdit("Group Devices");
  Batch& b = batchOf(e);
  // Out of the list (their wires and the mappings of outer racks onto them are dropped here and added back below:
  // the devices keep their ids, so their state, wires and mappings survive).
  std::vector<DeviceId> gone;
  for (const DevicePtr& d : inner) {
    b.commands.push_back(RemoveDevice{d->id});
    forEachDeviceIn(*d, [&](const Device& x) { gone.push_back(x.id); });
  }
  std::sort(gone.begin(), gone.end());
  DevicePtr rack = makeRack(ids.device(), instrument ? DeviceKind::Instrument : DeviceKind::AudioEffect,
                            {makeChain(ids.chain(), "Chain 1", inner)});
  e.devices.push_back(rack->id);
  b.commands.push_back(AddDevice{first.track->id, rack, picked.front(), first.chain ? first.chain->id : ChainId{}});
  auto in = [&](DeviceId id) { return std::binary_search(gone.begin(), gone.end(), id); };
  for (const Wire& w : wiresOf(p))
    if (in(w.from) || in(w.to)) b.commands.push_back(AddWire{w});
  forEachDevice(p, [&](const Device& d, const Track&) {
    if (!d.rack || in(d.id)) return;
    for (const MacroMapping& m : d.rack->mappings)
      if (in(m.device)) b.commands.push_back(AddMacroMapping{d.id, m});
  });
  return e;
}

EditResult addChain(const Project& p, IdSource& ids, DeviceId rack, uint32_t index, std::string name) {
  Result<const Device*> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  Edit e = makeEdit("Add Chain");
  e.devices.push_back(rack);
  batchOf(e).commands.push_back(
      AddChain{rack, makeChain(ids.chain(), name.empty() ? freshChainName(*(*r)->rack) : std::move(name)), index});
  return e;
}

EditResult addChainWithDevice(const Project& p, IdSource& ids, DeviceId rack, std::string patch, DeviceKind kind) {
  Result<const Device*> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  if (kind == DeviceKind::Instrument && (*r)->kind != DeviceKind::Instrument)
    return invalid("an Audio Effect Rack's chains hold audio effects only");
  const bool isRack = patch == kAudioEffectRack || patch == kInstrumentRack;
  DevicePtr d = isRack ? makeRack(ids.device(), kind) : makeDevice(ids.device(), patch, kind);
  const std::string name = isRack ? std::string(patch == kInstrumentRack ? "Instrument Rack" : "Audio Effect Rack") : patch;
  Edit e = makeEdit("Add Chain");
  e.devices.push_back(d->id);
  batchOf(e).commands.push_back(AddChain{rack, makeChain(ids.chain(), name, {d})});
  return e;
}

EditResult moveDeviceToNewChain(const Project& p, IdSource& ids, DeviceId device, DeviceId rack) {
  Result<const Device*> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  const DeviceLoc d = findDevice(p, device);
  if (!d.device) return notFound("device not found");
  if (deviceContains(*d.device, rack)) return invalid("a rack cannot go into itself");
  const ChainId chain = ids.chain();
  Edit e = makeEdit("Move Device");
  e.devices.push_back(device);
  batchOf(e).commands.push_back(AddChain{rack, makeChain(chain, freshChainName(*(*r)->rack))});
  batchOf(e).commands.push_back(MoveDevice{device, {}, kAppend, chain});
  return e;
}

EditResult deleteChains(const Project& p, std::span<const ChainId> chains) {
  Edit e = makeEdit(chains.size() == 1 ? "Delete Chain" : "Delete Chains");
  std::vector<ChainId> seen;
  for (const ChainId id : chains) {
    const ChainLoc c = findChain(p, id);
    if (!c.chain) return notFound("chain not found");
    if (std::find(seen.begin(), seen.end(), id) == seen.end()) seen.push_back(id);
  }
  for (const ChainId id : seen) {
    // A chain inside another deleted chain goes with it.
    const bool inner = std::any_of(seen.begin(), seen.end(), [&](ChainId o) {
      if (o == id) return false;
      const ChainLoc oc = findChain(p, o);
      const ChainLoc ic = findChain(p, id);
      return std::any_of(oc.chain->devices.begin(), oc.chain->devices.end(),
                         [&](const DevicePtr& d) { return deviceContains(*d, ic.rack->id); });
    });
    if (!inner) batchOf(e).commands.push_back(RemoveChain{id});
  }
  return e;
}

EditResult moveChain(const Project& p, ChainId chain, uint32_t index) {
  const ChainLoc c = findChain(p, chain);
  if (!c.chain) return notFound("chain not found");
  Edit e = makeEdit("Move Chain");
  e.devices.push_back(c.rack->id);
  if (std::min<size_t>(index, c.rack->rack->chains.size() - 1) != c.index) batchOf(e).commands.push_back(MoveChain{chain, index});
  return e;
}

EditResult renameChain(const Project& p, ChainId chain, std::string name) {
  const ChainLoc c = findChain(p, chain);
  if (!c.chain) return notFound("chain not found");
  Edit e = makeEdit("Rename Chain");
  if (c.chain->name != name) batchOf(e).commands.push_back(RenameChain{chain, std::move(name)});
  return e;
}

EditResult duplicateChain(const Project& p, IdSource& ids, ChainId chain) {
  const ChainLoc c = findChain(p, chain);
  if (!c.chain) return notFound("chain not found");
  auto copy = std::make_shared<Chain>(*c.chain);
  copy->id = ids.chain();
  for (DevicePtr& d : copy->devices) d = cloneDevice(*d, ids);
  Edit e = makeEdit("Duplicate Chain");
  for (const DevicePtr& d : copy->devices) e.devices.push_back(d->id);
  batchOf(e).commands.push_back(AddChain{c.rack->id, std::move(copy), c.index + 1});
  return e;
}

EditResult setChainMixer(const Project& p, ChainId chain, float volumeDb, float pan) {
  const ChainLoc c = findChain(p, chain);
  if (!c.chain) return notFound("chain not found");
  if (!std::isfinite(volumeDb) || !std::isfinite(pan)) return invalid("chain mixer value is not finite");
  Edit e = makeEdit("Chain Mixer");
  const float v = std::clamp(volumeDb, kMinVolumeDb, kMaxVolumeDb), pn = std::clamp(pan, -1.0f, 1.0f);
  if (v != c.chain->volumeDb || pn != c.chain->pan) batchOf(e).commands.push_back(SetChainMixer{chain, v, pn});
  return e;
}

EditResult setChainActive(const Project& p, ChainId chain, bool active) {
  const ChainLoc c = findChain(p, chain);
  if (!c.chain) return notFound("chain not found");
  Edit e = makeEdit(active ? "Activate Chain" : "Deactivate Chain");
  if (c.chain->active != active) batchOf(e).commands.push_back(SetChainActive{chain, active});
  return e;
}

EditResult setChainSolo(const Project& p, ChainId chain, bool solo, bool exclusive) {
  const ChainLoc c = findChain(p, chain);
  if (!c.chain) return notFound("chain not found");
  Edit e = makeEdit("Solo Chain");
  if (solo && exclusive)
    for (const ChainPtr& o : c.rack->rack->chains)
      if (o->id != chain && o->solo) batchOf(e).commands.push_back(SetChainSolo{o->id, false});
  if (c.chain->solo != solo) batchOf(e).commands.push_back(SetChainSolo{chain, solo});
  return e;
}

EditResult setChainZone(const Project& p, ChainId chain, ZoneKind kind, Zone zone) {
  const ChainLoc c = findChain(p, chain);
  if (!c.chain) return notFound("chain not found");
  if (!zoneValid(zone, kind, c.rack->rack->keyUnit)) return invalid("zone out of order or out of range");
  Edit e = makeEdit("Chain Zone");
  if (!(c.chain->zone(kind) == zone)) batchOf(e).commands.push_back(SetChainZone{chain, kind, zone});
  return e;
}

EditResult mapMacro(const Project& p, IdSource& ids, DeviceId rack, uint32_t macro, DeviceId device, std::string param,
                    std::optional<double> macroValue) {
  Result<const Device*> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  if (macro >= kMacroCount) return invalid("a rack has 16 macros");
  if (device == rack ? param != kSelectParam : !deviceContains(**r, device))
    return invalid("a macro maps only devices inside its rack (or the rack's chain selector)");
  Edit e = makeEdit("Map Macro");
  e.devices.push_back(device);
  const MappingLoc old = mappingOf(p, device, param);
  if (old.mapping) {
    if (old.rack->id == rack && old.mapping->macro == macro) return e;  // already
    batchOf(e).commands.push_back(RemoveMacroMapping{old.mapping->id});
  }
  MacroMapping m;
  m.id = ids.mapping();
  m.macro = static_cast<uint8_t>(macro);
  m.device = device;
  m.param = std::move(param);
  batchOf(e).commands.push_back(AddMacroMapping{rack, std::move(m)});
  if (macroValue && std::isfinite(*macroValue))
    batchOf(e).commands.push_back(SetDeviceParam{rack, macroParamName(macro), std::clamp(*macroValue, 0.0, kMacroMax)});
  return e;
}

EditResult unmapMacro(const Project& p, MappingId mapping) {
  const MappingLoc m = findMapping(p, mapping);
  if (!m.mapping) return notFound("mapping not found");
  Edit e = makeEdit("Unmap Macro");
  e.devices.push_back(m.mapping->device);
  batchOf(e).commands.push_back(RemoveMacroMapping{mapping});
  return e;
}

EditResult unmapParam(const Project& p, DeviceId device, std::string_view param) {
  const MappingLoc m = mappingOf(p, device, param);
  Edit e = makeEdit("Unmap Macro");
  e.devices.push_back(device);
  if (m.mapping) batchOf(e).commands.push_back(RemoveMacroMapping{m.mapping->id});
  return e;
}

EditResult setMacroMapping(const Project& p, MappingId mapping, double min, double max, double curve) {
  const MappingLoc m = findMapping(p, mapping);
  if (!m.mapping) return notFound("mapping not found");
  if (!std::isfinite(min) || !std::isfinite(max) || !std::isfinite(curve)) return invalid("mapping value is not finite");
  MacroMapping x = *m.mapping;
  x.min = std::clamp(min, 0.0, 1.0);
  x.max = std::clamp(max, 0.0, 1.0);
  x.curve = std::clamp(curve, -1.0, 1.0);
  Edit e = makeEdit("Edit Macro Mapping");
  if (!(x == *m.mapping)) batchOf(e).commands.push_back(SetMacroMapping{std::move(x)});
  return e;
}

EditResult setMacroValue(const Project& p, DeviceId rack, uint32_t macro, double value) {
  Result<const Device*> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  if (macro >= kMacroCount) return invalid("a rack has 16 macros");
  if (!std::isfinite(value)) return invalid("macro value is not finite");
  Edit e = makeEdit("Device Parameter");
  e.devices.push_back(rack);
  batchOf(e).commands.push_back(SetDeviceParam{rack, macroParamName(macro), std::clamp(value, 0.0, kMacroMax)});
  return e;
}

EditResult renameMacro(const Project& p, DeviceId rack, uint32_t macro, std::string name) {
  if (macro >= kMacroCount) return invalid("a rack has 16 macros");
  return withOptions(p, rack, "Rename Macro", [&](RackOptions& o) {
    o.macroNames[macro] = std::move(name);
    return ok();
  });
}

EditResult setVisibleMacros(const Project& p, DeviceId rack, uint32_t count) {
  return withOptions(p, rack, "Macro Count", [&](RackOptions& o) {
    o.visibleMacros = static_cast<uint8_t>(std::clamp<uint32_t>(count, 1, kMacroCount));
    return ok();
  });
}

EditResult setRackShow(const Project& p, DeviceId rack, bool chains, bool macros) {
  return withOptions(p, rack, "Rack View", [&](RackOptions& o) {
    o.showChains = chains;
    o.showMacros = macros;
    return ok();
  });
}

EditResult setKeyZoneUnit(const Project& p, DeviceId rack, KeyZoneUnit unit) {
  return withOptions(p, rack, "Key Zone Unit", [&](RackOptions& o) {
    o.keyUnit = unit;
    return ok();
  });
}

EditResult setVoiceChains(const Project& p, DeviceId rack, bool perVoice) {
  Result<const Device*> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  if (perVoice && (*r)->kind != DeviceKind::Instrument) return invalid("only Instrument Rack chains run per voice");
  return withOptions(p, rack, "Per-Voice Chains", [&](RackOptions& o) {
    o.voiceChains = perVoice;
    return ok();
  });
}

EditResult storeVariation(const Project& p, DeviceId rack, std::string name, uint32_t index) {
  Result<const Device*> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  MacroVariation v;
  for (uint32_t k = 0; k < kMacroCount; ++k) v.values[k] = rackParamValue(**r, macroParamName(k));
  return withOptions(p, rack, "Store Variation", [&](RackOptions& o) {
    if (index < o.variations.size()) {
      v.name = name.empty() ? o.variations[index].name : std::move(name);
      o.variations[index] = std::move(v);
    } else {
      v.name = name.empty() ? "Variation " + std::to_string(o.variations.size() + 1) : std::move(name);
      o.variations.push_back(std::move(v));
    }
    return ok();
  });
}

EditResult recallVariation(const Project& p, DeviceId rack, uint32_t index) {
  Result<const Device*> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  const Rack& rk = *(*r)->rack;
  if (index >= rk.variations.size()) return notFound("no such variation");
  Edit e = makeEdit("Recall Variation");
  e.devices.push_back(rack);
  for (uint32_t k = 0; k < kMacroCount; ++k) {
    const double v = rk.variations[index].values[k];
    if (rackParamValue(**r, macroParamName(k)) != v) batchOf(e).commands.push_back(SetDeviceParam{rack, macroParamName(k), v});
  }
  return e;
}

EditResult deleteVariation(const Project& p, DeviceId rack, uint32_t index) {
  return withOptions(p, rack, "Delete Variation", [&](RackOptions& o) -> Status {
    if (index >= o.variations.size()) return notFound("no such variation");
    o.variations.erase(o.variations.begin() + index);
    return ok();
  });
}

EditResult renameVariation(const Project& p, DeviceId rack, uint32_t index, std::string name) {
  return withOptions(p, rack, "Rename Variation", [&](RackOptions& o) -> Status {
    if (index >= o.variations.size()) return notFound("no such variation");
    o.variations[index].name = std::move(name);
    return ok();
  });
}

EditResult randomizeMacros(const Project& p, DeviceId rack, uint64_t seed) {
  Result<const Device*> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  const Rack& rk = *(*r)->rack;
  Edit e = makeEdit("Randomize Macros");
  e.devices.push_back(rack);
  uint64_t s = seed;
  for (uint32_t k = 0; k < rk.visibleMacros; ++k) {
    const uint64_t x = splitmix(s);
    if ((rk.randomizeExclude >> k) & 1u) continue;
    const double v = std::floor(static_cast<double>(x >> 11) * (1.0 / 9007199254740992.0) * (kMacroMax + 1.0));
    batchOf(e).commands.push_back(SetDeviceParam{rack, macroParamName(k), std::min(v, kMacroMax)});
  }
  return e;
}

EditResult setMacroExcluded(const Project& p, DeviceId rack, uint32_t macro, bool excluded) {
  if (macro >= kMacroCount) return invalid("a rack has 16 macros");
  return withOptions(p, rack, "Exclude Macro", [&](RackOptions& o) {
    const auto bit = static_cast<uint16_t>(1u << macro);
    o.randomizeExclude = static_cast<uint16_t>(excluded ? (o.randomizeExclude | bit) : (o.randomizeExclude & ~bit));
    return ok();
  });
}

}  // namespace pq::doc::ops
