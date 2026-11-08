// Rack edit builders (GU.9 v2: a rack is a set of parallel groups, racks.h): pq/document/edit_ops.h, "Racks".
#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "edit_common.h"
#include "edit_paths.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"
#include "pq/document/racks.h"
#include "pq/dsp/curve.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

struct RackAt {
  const Track* track = nullptr;
  RackInfo rack;
};
Result<RackAt> rackAt(const Project& p, DeviceId id) {
  const DeviceLoc d = findDevice(p, id);
  if (d.device == nullptr || d.track == nullptr) return notFound("rack not found");
  std::optional<RackInfo> r = rackOf(p, id);
  if (!r) return invalid("not a rack: a rack is a set of parallel groups (racks.h)");
  return RackAt{d.track, std::move(*r)};
}
struct ChainAt {
  const Track* track = nullptr;
  RackInfo rack;
  RackChain chain;
};
Result<ChainAt> chainAt(const Project& p, DeviceId chain) {
  const DeviceLoc d = findDevice(p, chain);
  if (d.device == nullptr || d.track == nullptr) return notFound("chain not found");
  std::optional<RackInfo> r = rackOfChain(p, *d.track, chain);
  if (!r) return invalid("not a rack chain");
  const RackChain* c = r->findChain(chain);
  return ChainAt{d.track, *r, *c};
}

Wire link(IdSource& ids, DeviceId from, std::string_view port, DeviceId to, std::string_view param) {
  Wire w;
  w.id = ids.wire();
  w.from = from;
  w.port = std::string(port);
  w.to = to;
  w.param = std::string(param);
  w.chain = true;
  return w;
}

// A wire that is no chain link (it crosses group edges: the selector into a chain zone).
Wire plainWire(IdSource& ids, DeviceId from, std::string_view port, DeviceId to, std::string_view param) {
  Wire w = link(ids, from, port, to, param);
  w.chain = false;
  return w;
}

double positionOf(const Device& d, std::string_view param, double def = 0.0) {
  const DeviceValue* v = d.param(param);
  return v != nullptr ? v->value : def;
}

// A stage of a chain's series: one block, or (a nested rack) its chains in parallel.
using Stage = std::vector<DeviceId>;

// The stages a list of a chain's devices stands for (a nested rack's first chain: all its chains).
std::vector<Stage> stagesOf(const Project& p, const Track& t, const std::vector<DeviceId>& devices) {
  const std::vector<RackInfo> racks = findRacks(p, t);
  std::vector<Stage> out;
  for (const DeviceId d : devices) {
    Stage s{d};
    for (const RackInfo& r : racks)
      if (r.findChain(d) != nullptr) {
        s.clear();
        for (const RackChain& c : r.chains) s.push_back(c.id);
      }
    out.push_back(std::move(s));
  }
  return out;
}

// Whether wire `w` is one of chain `chain`'s series links: main ports between its In terminal, its direct members and
// its Out terminal.
bool seriesLink(const Track& t, DeviceId chain, const Wire& w) {
  if (w.toMixer() || w.fromNoteSource()) return false;
  auto member = [&](DeviceId d) {
    for (const DevicePtr& x : t.devices)
      if (x->id == d) return x->group == chain;
    return false;
  };
  const bool fromOk = (w.from == chain && w.port == kGroupInPort) || (w.port == kMainPort && member(w.from));
  const bool toOk = (w.to == chain && w.param == kGroupOutPort) || (w.param == kMainPort && member(w.to));
  return fromOk && toOk;
}

// Re-lays chain `chain`'s series (k.doc()): In → `devices` (its user blocks in order, an Instrument Rack's instrument
// first) with its zones and mixer where racks.h puts them → Out. `instrumentFirst`: devices[0] is the instrument (In
// feeds it); an Instrument Rack's chain without one has no wire from In. `voiceChains`: the instrument stage's out wire
// stays per voice. Every other wire (modulation, the selector, cables into params) stays.
Status relay(Steps& k, IdSource& ids, DeviceId chain, const std::vector<DeviceId>& devices, bool instrumentRack,
             bool instrumentFirst, bool voiceChains) {
  const DeviceLoc loc = findDevice(k.doc(), chain);
  if (loc.track == nullptr) return notFound("chain not found");
  std::vector<WireId> gone;
  for (const Wire& w : wiresOf(k.doc()))
    if (seriesLink(*loc.track, chain, w)) gone.push_back(w.id);
  for (const WireId w : gone) PQ_DOC_TRY(k.add(RemoveWire{w}));
  auto member = [&](RackRole role) -> DeviceId {
    for (const DevicePtr& d : findDevice(k.doc(), chain).track->devices)
      if (d->group == chain && rackRole(*d) == role && isGroupDevice(*d)) return d->id;
    return {};
  };
  const DeviceId keyZ = member(RackRole::KeyZone), velZ = member(RackRole::VelocityZone),
                 selZ = member(RackRole::SelectZone), mixer = member(RackRole::Mixer);
  std::vector<Stage> user = stagesOf(k.doc(), *findDevice(k.doc(), chain).track, devices);
  std::vector<Stage> stages;
  int32_t voiceEnd = -1;  // the stage whose out wire is single-signal
  bool feed = true;
  if (instrumentRack) {
    size_t i = 0;
    if (instrumentFirst && !user.empty()) {
      stages.push_back(user[0]);
      i = 1;
      if (keyZ.valid()) stages.push_back({keyZ});
      if (velZ.valid()) stages.push_back({velZ});
      voiceEnd = static_cast<int32_t>(stages.size()) - 1;
    } else {
      feed = false;
    }
    for (; i < user.size(); ++i) stages.push_back(user[i]);
    if (selZ.valid()) stages.push_back({selZ});
  } else {
    if (selZ.valid()) stages.push_back({selZ});
    for (const Stage& s : user) stages.push_back(s);
  }
  if (mixer.valid()) stages.push_back({mixer});
  // The links: In → stage 0 → … → Out (a parallel stage: every one of its chains fed, every one summed).
  Stage prev{chain};
  bool prevIsIn = true;
  for (size_t i = 0; i < stages.size(); ++i) {
    if (!(prevIsIn && !feed))
      for (const DeviceId a : prev)
        for (const DeviceId b : stages[i]) {
          Wire w = link(ids, a, prevIsIn ? kGroupInPort : kMainPort, b, kMainPort);
          if (!prevIsIn && static_cast<int32_t>(i) - 1 == voiceEnd && !voiceChains) w.single = true;
          PQ_DOC_TRY(k.add(AddWire{std::move(w)}));
        }
    prev = stages[i];
    prevIsIn = false;
  }
  if (!(prevIsIn && !feed))
    for (const DeviceId a : prev) {
      Wire w = link(ids, a, prevIsIn ? kGroupInPort : kMainPort, chain, kGroupOutPort);
      if (!prevIsIn && static_cast<int32_t>(stages.size()) - 1 == voiceEnd && !voiceChains) w.single = true;
      PQ_DOC_TRY(k.add(AddWire{std::move(w)}));
    }
  return ok();
}

// Whether chain `c`'s first device is its instrument (an Instrument Rack's chain whose In feeds it).
bool instrumentFirst(const Project& p, const RackInfo& r, const RackChain& c) {
  if (!r.instrument || c.devices.empty()) return false;
  for (const Wire& w : wiresOf(p))
    if (w.from == c.id && w.port == kGroupInPort && w.to == c.devices.front()->id && w.param == kMainPort) return true;
  return false;
}
std::vector<DeviceId> idsOf(const std::vector<const Device*>& v) {
  std::vector<DeviceId> out;
  for (const Device* d : v) out.push_back(d->id);
  return out;
}

// The chain's Gate: on, not silenced by solo elsewhere, its volume above −∞.
double gateOf(const RackInfo& r, const RackChain& c) {
  const bool anySolo = std::any_of(r.chains.begin(), r.chains.end(), [](const RackChain& x) { return x.solo; });
  return c.active && (!anySolo || c.solo) && c.volumeDb > kMinVolumeDb ? 1.0 : 0.0;
}
// Writes every chain's Gate that changed (r: the rack as it is now).
Status regate(Steps& k, const RackInfo& r) {
  for (const RackChain& c : r.chains) {
    if (!c.gateBlock.valid()) continue;
    const Device* g = findDevice(k.doc(), c.gateBlock).device;
    if (g == nullptr) continue;
    const double want = gateOf(r, c);
    if (positionOf(*g, kValueParam, 1.0) != want) PQ_DOC_TRY(k.add(SetDeviceParam{c.gateBlock, std::string(kValueParam), want}));
  }
  return ok();
}
Status regate(Steps& k, DeviceId anyChain) {
  std::optional<RackInfo> r = rackOf(k.doc(), anyChain);
  return r ? regate(k, *r) : ok();
}

std::string freshChainName(const RackInfo& r) {
  for (uint32_t n = 1;; ++n) {
    std::string c = "Chain " + std::to_string(n);
    if (std::none_of(r.chains.begin(), r.chains.end(), [&](const RackChain& x) { return x.name == c; })) return c;
  }
}

// The list index (Track::devices) chain number `index` of `r` has, for a MoveDevice of `moving` (after its removal).
uint32_t listIndexFor(const Project& p, const Track& t, const RackInfo& r, uint32_t index, DeviceId moving) {
  std::vector<DeviceId> others;
  for (const RackChain& c : r.chains)
    if (c.id != moving) others.push_back(c.id);
  std::vector<DeviceId> list;
  for (const DevicePtr& d : t.devices)
    if (d->id != moving) list.push_back(d->id);
  (void)p;
  if (index >= others.size()) {
    // after the last chain
    const DeviceId last = others.empty() ? DeviceId{} : others.back();
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i] == last) return static_cast<uint32_t>(i + 1);
    return kAppend;
  }
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i] == others[index]) return static_cast<uint32_t>(i);
  return kAppend;
}

// The feed and sum wires of chain `c` (its one main input wire, its one main output wire into the sum point).
struct Ends {
  const Wire* feed = nullptr;
  const Wire* sum = nullptr;
};
Ends endsOf(const Project& p, DeviceId c) {
  Ends e;
  for (const Wire& w : wiresOf(p)) {
    if (!w.toMixer() && w.to == c && w.param == kMainPort) e.feed = &w;
    if (!w.fromNoteSource() && w.from == c && w.port == kMainPort &&
        (w.toMixer() || w.param == kMainPort || w.param == kGroupOutPort))
      e.sum = &w;
  }
  return e;
}
// Wires `chain` in parallel with `like` (the same feed and sum ends).
Status parallel(Steps& k, IdSource& ids, DeviceId like, DeviceId chain) {
  const Ends e = endsOf(k.doc(), like);
  if (e.feed != nullptr) {
    Wire w = *e.feed;
    w.id = ids.wire();
    w.to = chain;
    w.param = std::string(kMainPort);
    PQ_DOC_TRY(k.add(AddWire{std::move(w)}));
  }
  if (const Ends e2 = endsOf(k.doc(), like); e2.sum != nullptr) {
    Wire w = *e2.sum;
    w.id = ids.wire();
    w.from = chain;
    w.noteSource = {};
    w.port = std::string(kMainPort);
    PQ_DOC_TRY(k.add(AddWire{std::move(w)}));
  }
  return ok();
}

Status addBlocks(Steps& k, TrackId track, const BlockSet& set) {
  for (const DevicePtr& d : set.devices) PQ_DOC_TRY(k.add(AddDevice{track, d, kAppend}));
  for (const Wire& w : set.wires) PQ_DOC_TRY(k.add(AddWire{w}));
  return ok();
}

// A macro's block (made when missing: "Macro <k>" at the rack's level, 0 … 1 at `position`).
Result<DeviceId> macroBlock(Steps& k, IdSource& ids, const RackInfo& r, uint32_t macro, double position) {
  if (r.macros[macro].block.valid()) return r.macros[macro].block;
  const DeviceId id = ids.device();
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kValueDevice), DeviceKind::AudioEffect));
  d->name = "Macro " + std::to_string(macro + 1);
  d->group = r.parent;
  d->params = {DeviceValue{std::string(kValueParam), std::clamp(position, 0.0, 1.0)}};
  d->defaultPosition = d->params.front().value;  // TX.reset-default: a reset returns to where it was made
  d->curves = {ControlCurve{std::string(kValueParam), macroCurve()}};
  d->settings = {DeviceValue{std::string(kMacroSetting), static_cast<double>(macro + 1)}};
  PQ_DOC_TRY(k.add(AddDevice{r.track, std::move(d), kAppend}));
  return id;
}

// The rack's selector (made when missing: role Selector, named kChainSelectName, at the rack's level, 0 … kSelectMax
// at 0).
DeviceId selectorBlock(Steps& k, IdSource& ids, const RackInfo& r, Status& st) {
  if (r.selector.valid()) return r.selector;
  // One that exists but feeds nothing yet (this edit makes the first chain zone).
  for (const DevicePtr& d : findTrackPtr(k.doc(), r.track)->devices)
    if (isValueDevice(*d) && d->group == r.parent && rackRole(*d) == RackRole::Selector) return d->id;
  const DeviceId id = ids.device();
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kValueDevice), DeviceKind::AudioEffect));
  d->name = std::string(kChainSelectName);
  d->group = r.parent;
  d->params = {DeviceValue{std::string(kValueParam), 0.0}};
  d->defaultPosition = 0.0;  // TX.reset-default
  d->curves = {ControlCurve{std::string(kValueParam), selectorCurve()}};
  d->settings = {DeviceValue{std::string(kRoleSetting), static_cast<double>(RackRole::Selector)}};
  st = k.add(AddDevice{r.track, std::move(d), kAppend});
  return id;
}

Status relayChain(Steps& k, IdSource& ids, const RackInfo& r, const RackChain& c, std::vector<DeviceId> devices,
                  bool instFirst) {
  return relay(k, ids, c.id, devices, r.instrument, instFirst, r.voiceChains);
}

}  // namespace

// ---- Racks ---------------------------------------------------------------------------------------------------------

EditResult addRack(const Project& p, IdSource& ids, TrackId track, bool instrument, uint32_t index) {
  const Track* t = findTrackPtr(p, track);
  if (t == nullptr) return notFound("track not found");
  if (instrument && t->kind != TrackKind::Midi) return invalid("instruments go on MIDI tracks");
  Edit e = makeEdit(instrument ? "Add Instrument Rack" : "Add Audio Effect Rack");
  Steps k(p);
  const BlockSet set = chainBlocks(ids, {}, "Chain 1", instrument);
  const DeviceId chain = set.devices.front()->id;
  PQ_DOC_TRY(addBlocks(k, track, set));
  if (index != kOffPath) {
    if (instrument) {  // it takes the notes: the instrument heading the path (a stock one or a group) gives way
      const MainPath path = mainPath(k.doc(), *findTrackPtr(k.doc(), track));
      if (!path.blocks.empty()) {
        const Device* head = path.blocks.front();
        if (head->kind == DeviceKind::Instrument || (isGroupDevice(*head) && isInstrumentGroup(k.doc(), head->id))) {
          const DeviceId gone = head->id;
          PQ_DOC_TRY(detach(k, ids, nullptr, gone));
          PQ_DOC_TRY(k.add(RemoveDevice{gone}));
        }
      }
      index = 0;
    }
    PQ_DOC_TRY(splice(k, ids, track, chain, index));
  }
  e.devices.push_back(chain);
  e.tracks.push_back(track);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult addChain(const Project& p, IdSource& ids, DeviceId rack, uint32_t index, std::string name) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  if (name.empty()) name = freshChainName(r->rack);
  Edit e = makeEdit("Add Chain");
  Steps k(p);
  const BlockSet set = chainBlocks(ids, r->rack.parent, std::move(name), r->rack.instrument);
  const DeviceId chain = set.devices.front()->id;
  PQ_DOC_TRY(addBlocks(k, r->track->id, set));
  PQ_DOC_TRY(parallel(k, ids, r->rack.chains.front().id, chain));
  if (r->rack.keyUnit == KeyZoneUnit::Degree) PQ_DOC_TRY(k.add(SetDeviceSetting{chain, std::string(kKeyUnitSetting), 1.0}));
  if (index < r->rack.chains.size())
    PQ_DOC_TRY(k.add(MoveDevice{chain, {}, listIndexFor(k.doc(), *findTrackPtr(k.doc(), r->track->id), r->rack, index, chain)}));
  PQ_DOC_TRY(regate(k, chain));
  e.devices.push_back(chain);
  e.tracks.push_back(r->track->id);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult addToChain(const Project& p, IdSource& ids, DeviceId chain, std::string patch, DeviceKind kind, uint32_t index) {
  Result<ChainAt> c = chainAt(p, chain);
  if (!c) return std::move(c).error();
  if (patch.empty()) return invalid("no device patch");
  if (kind == DeviceKind::Instrument && (!c->rack.instrument || c->track->kind != TrackKind::Midi))
    return invalid("An instrument goes into an Instrument Rack, not an Audio Effect Rack");
  Edit e = makeEdit("Add Device");
  Steps k(p);
  DeviceId id;
  if (patch.starts_with(kModuleItemPrefix)) {  // GU.11: a built-in module: its group preset (fresh ids) in the chain
    const ModuleInfo* m = findModule(std::string_view(patch).substr(kModuleItemPrefix.size()));
    if (m == nullptr) return invalid("no module '" + patch + "'");
    if (m->instrument != (kind == DeviceKind::Instrument))
      return invalid(m->instrument ? "an instrument module is added as an instrument" : "an effect module is added as an effect");
    BlockSet set = remapBlocks(moduleBlocks(m->kind), ids);
    id = set.devices.front()->id;
    auto g = std::make_shared<Device>(*set.devices.front());
    g->group = chain;
    set.devices.front() = std::move(g);
    PQ_DOC_TRY(addBlocks(k, c->track->id, set));
  } else {
    id = ids.device();
    auto d = std::make_shared<Device>(*makeDevice(id, std::move(patch), kind));
    d->group = chain;
    PQ_DOC_TRY(k.add(AddDevice{c->track->id, std::move(d), kAppend}));
  }
  std::vector<DeviceId> devs = idsOf(c->chain.devices);
  bool instFirst = instrumentFirst(p, c->rack, c->chain);
  if (kind == DeviceKind::Instrument) {  // first, replacing the chain's instrument (Live)
    if (instFirst) {
      PQ_DOC_TRY(k.add(RemoveDevice{devs.front()}));
      devs.erase(devs.begin());
    }
    devs.insert(devs.begin(), id);
    instFirst = true;
  } else {
    const size_t lo = instFirst ? 1 : 0;
    const size_t at = std::clamp<size_t>(index == kAppend ? devs.size() : index, lo, devs.size());
    devs.insert(devs.begin() + static_cast<std::ptrdiff_t>(at), id);
  }
  PQ_DOC_TRY(relayChain(k, ids, c->rack, c->chain, devs, instFirst));
  e.devices.push_back(id);
  e.tracks.push_back(c->track->id);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult addChainWithDevice(const Project& p, IdSource& ids, DeviceId rack, std::string patch, DeviceKind kind) {
  const EditResult a = addChain(p, ids, rack);
  if (!a) return a;
  Steps k(p);
  PQ_DOC_TRY(k.add(*a));
  const DeviceId chain = a->devices.front();
  const EditResult b = addToChain(k.doc(), ids, chain, std::move(patch), kind);
  if (!b) return b;
  PQ_DOC_TRY(k.add(*b));
  Edit e = makeEdit("Add Chain");
  e.devices = b->devices;
  e.tracks = b->tracks;
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult moveIntoChain(const Project& p, IdSource& ids, DeviceId device, uint32_t index, DeviceId chain) {
  Result<ChainAt> c = chainAt(p, chain);
  if (!c) return std::move(c).error();
  const DeviceLoc d = findDevice(p, device);
  if (d.device == nullptr) return notFound("device not found");
  if (d.track != c->track) return invalid("a chain takes devices of its own track");
  // A whole rack moves when one of its chains is dragged (its chains, macros and selector).
  std::vector<DeviceId> moving{device};
  std::optional<RackInfo> movingRack = rackOfChain(p, *d.track, device);
  if (movingRack) {
    moving.clear();
    for (const RackChain& x : movingRack->chains) moving.push_back(x.id);
  }
  for (const DeviceId m : moving)
    if (m == chain || groupContains(d.track->devices, m, chain)) return invalid("A rack cannot go inside itself");
  if (d.device->kind == DeviceKind::Instrument && !c->rack.instrument)
    return invalid("An instrument goes into an Instrument Rack, not an Audio Effect Rack");
  Edit e = makeEdit("Move Device");
  Steps k(p);
  // Out of where it is: another chain's series (re-laid without it), or a path / series (joined there).
  if (const std::optional<RackInfo> from = d.device->group.valid() ? rackOfChain(p, *d.track, d.device->group) : std::nullopt) {
    const RackChain& fc = *from->findChain(d.device->group);
    std::vector<DeviceId> rest;
    for (const Device* x : fc.devices)
      if (x->id != device) rest.push_back(x->id);
    const bool fi = instrumentFirst(p, *from, fc) && fc.devices.front()->id != device;
    // its own feed and sum wires go with the re-lay (they are series links of that chain)
    PQ_DOC_TRY(relay(k, ids, fc.id, rest, from->instrument, fi, from->voiceChains));
  } else if (movingRack) {
    // the rack's feed and sum: removed, its feed joined to its sum once
    const Ends en = endsOf(p, movingRack->chains.front().id);
    for (const RackChain& x : movingRack->chains) {
      const Ends ex = endsOf(p, x.id);
      if (ex.feed) PQ_DOC_TRY(k.add(RemoveWire{ex.feed->id}));
      if (ex.sum) PQ_DOC_TRY(k.add(RemoveWire{ex.sum->id}));
    }
    if (en.feed != nullptr && en.sum != nullptr) {
      Wire j = *en.sum;
      j.id = ids.wire();
      j.from = en.feed->from;
      j.noteSource = en.feed->noteSource;
      j.port = en.feed->port;
      if (!hasEnds(k.doc(), j)) PQ_DOC_TRY(k.add(AddWire{std::move(j)}));
    }
  } else {
    PQ_DOC_TRY(detach(k, ids, nullptr, device));
  }
  for (const DeviceId m : moving) PQ_DOC_TRY(k.add(SetDeviceGroup{m, chain}));
  if (movingRack) {
    for (const RackMacro& m : movingRack->macros)
      if (m.block.valid()) PQ_DOC_TRY(k.add(SetDeviceGroup{m.block, chain}));
    if (movingRack->selector.valid()) PQ_DOC_TRY(k.add(SetDeviceGroup{movingRack->selector, chain}));
  }
  // Into the target chain's series (read again: it may have lost the device above).
  std::optional<RackInfo> to = rackOfChain(k.doc(), *findTrackPtr(k.doc(), c->track->id), chain);
  if (!to) return invalid("the chain is gone");
  const RackChain& tc = *to->findChain(chain);
  std::vector<DeviceId> devs;
  for (const Device* x : tc.devices)
    if (std::find(moving.begin(), moving.end(), x->id) == moving.end()) devs.push_back(x->id);
  bool fi = to->instrument && !devs.empty() && instrumentFirst(k.doc(), *to, tc) &&
            std::find(moving.begin(), moving.end(), tc.devices.front()->id) == moving.end();
  if (d.device->kind == DeviceKind::Instrument) {
    if (fi) {
      PQ_DOC_TRY(k.add(RemoveDevice{devs.front()}));
      devs.erase(devs.begin());
    }
    devs.insert(devs.begin(), moving.front());
    fi = true;
  } else {
    const size_t lo = fi ? 1 : 0;
    const size_t at = std::clamp<size_t>(index == kAppend ? devs.size() : index, lo, devs.size());
    devs.insert(devs.begin() + static_cast<std::ptrdiff_t>(at), moving.front());
  }
  PQ_DOC_TRY(relay(k, ids, chain, devs, to->instrument, fi, to->voiceChains));
  e.devices.push_back(device);
  e.tracks.push_back(c->track->id);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult moveDeviceToNewChain(const Project& p, IdSource& ids, DeviceId device, DeviceId rack) {
  const EditResult a = addChain(p, ids, rack);
  if (!a) return a;
  Steps k(p);
  PQ_DOC_TRY(k.add(*a));
  const EditResult b = moveIntoChain(k.doc(), ids, device, kAppend, a->devices.front());
  if (!b) return b;
  PQ_DOC_TRY(k.add(*b));
  Edit e = makeEdit("Move Device");
  e.devices = {device};
  e.tracks = b->tracks;
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult deleteChains(const Project& p, IdSource& ids, std::span<const DeviceId> chains) {
  Edit e = makeEdit(chains.size() == 1 ? "Delete Chain" : "Delete Chains");
  Steps k(p);
  for (const DeviceId c : chains) {
    std::optional<RackInfo> r;
    if (const DeviceLoc l = findDevice(k.doc(), c); l.track != nullptr) r = rackOfChain(k.doc(), *l.track, c);
    if (!r) continue;
    if (r->chains.size() == 1) {  // the last one: the rack goes (its feed joined to its sum), with its macros
      const Ends en = endsOf(k.doc(), c);
      Wire j;
      bool join = en.feed != nullptr && en.sum != nullptr;
      if (join) {
        j = *en.sum;
        j.id = ids.wire();
        j.from = en.feed->from;
        j.noteSource = en.feed->noteSource;
        j.port = en.feed->port;
        j.chain = false;
        if (r->instrument) join = false;  // notes into the sum point would be no audio
      }
      PQ_DOC_TRY(k.add(deleteDevices(k.doc(), std::vector<DeviceId>{c})));
      for (const RackMacro& m : r->macros)
        if (m.block.valid()) PQ_DOC_TRY(k.add(RemoveDevice{m.block}));
      if (r->selector.valid()) PQ_DOC_TRY(k.add(RemoveDevice{r->selector}));
      if (join && !hasEnds(k.doc(), j)) PQ_DOC_TRY(k.add(AddWire{std::move(j)}));
      continue;
    }
    PQ_DOC_TRY(k.add(deleteDevices(k.doc(), std::vector<DeviceId>{c})));
    PQ_DOC_TRY(regate(k, r->chains[r->chains.front().id == c ? 1 : 0].id));
  }
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult moveChain(const Project& p, DeviceId chain, uint32_t index) {
  Result<ChainAt> c = chainAt(p, chain);
  if (!c) return std::move(c).error();
  Edit e = makeEdit("Move Chain");
  e.devices.push_back(chain);
  const int32_t at = c->rack.chainIndex(chain);
  if (at == static_cast<int32_t>(index) || index >= c->rack.chains.size()) return e;
  batchOf(e).commands.push_back(MoveDevice{chain, {}, listIndexFor(p, *c->track, c->rack, index, chain)});
  return e;
}

EditResult renameChain(const Project& p, DeviceId chain, std::string name) {
  Result<ChainAt> c = chainAt(p, chain);
  if (!c) return std::move(c).error();
  EditResult r = renameDevice(p, chain, std::move(name));
  if (r) r->devices = {chain};
  return r;
}

EditResult duplicateChain(const Project& p, IdSource& ids, DeviceId chain) {
  Result<ChainAt> c = chainAt(p, chain);
  if (!c) return std::move(c).error();
  Result<BlockSet> set = copyBlocks(p, std::vector<DeviceId>{chain});
  if (!set) return std::move(set).error();
  std::vector<std::pair<DeviceId, DeviceId>> remap;
  const BlockSet copy = remapBlocks(*set, ids, &remap);
  Edit e = makeEdit("Duplicate Chain");
  Steps k(p);
  PQ_DOC_TRY(addBlocks(k, c->track->id, copy));
  const DeviceId dup = copy.devices.front()->id;
  PQ_DOC_TRY(parallel(k, ids, chain, dup));
  // The selector into the copy's chain zone (its wire crossed the chain's edge: not in the copied set).
  if (c->chain.selectZone.valid() && c->rack.selector.valid())
    for (const DevicePtr& d : copy.devices)
      if (isValueDevice(*d) && rackRole(*d) == RackRole::ZoneValue) {
        PQ_DOC_TRY(k.add(AddWire{plainWire(ids, c->rack.selector, kValueOutput, d->id, kValueParam)}));
        break;
      }
  PQ_DOC_TRY(k.add(MoveDevice{dup, {}, listIndexFor(k.doc(), *findTrackPtr(k.doc(), c->track->id), c->rack,
                                                    static_cast<uint32_t>(c->rack.chainIndex(chain) + 1), dup)}));
  if (const std::string n = c->chain.name.empty() ? std::string("Chain") : c->chain.name; true)
    PQ_DOC_TRY(k.add(RenameDevice{dup, n}));
  PQ_DOC_TRY(regate(k, dup));
  e.devices.push_back(dup);
  e.tracks.push_back(c->track->id);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult setChainMixer(const Project& p, DeviceId chain, float volumeDb, float pan) {
  Result<ChainAt> c = chainAt(p, chain);
  if (!c) return std::move(c).error();
  if (!c->chain.volumeBlock.valid() || !c->chain.panBlock.valid()) return invalid("the chain has no mixer");
  Edit e = makeEdit("Chain Mixer");
  Steps k(p);
  const double db = std::clamp(static_cast<double>(volumeDb), static_cast<double>(kMinVolumeDb), static_cast<double>(kMaxVolumeDb));
  const double pn = std::clamp(static_cast<double>(pan), -1.0, 1.0);
  const Device& vb = *findDevice(p, c->chain.volumeBlock).device;
  const Device& pb = *findDevice(p, c->chain.panBlock).device;
  const dsp::Curve vc = vb.curve(kValueParam) ? *vb.curve(kValueParam) : chainVolumeCurve();
  const dsp::Curve pc = pb.curve(kValueParam) ? *pb.curve(kValueParam) : chainPanCurve();
  const double vpos = dsp::curvePosition(vc, dsp::curveOutput(vc, db), positionOf(vb, kValueParam));
  const double ppos = dsp::curvePosition(pc, pn, positionOf(pb, kValueParam));
  if (vpos != positionOf(vb, kValueParam, -1.0)) PQ_DOC_TRY(k.add(SetDeviceParam{vb.id, std::string(kValueParam), vpos}));
  if (ppos != positionOf(pb, kValueParam, -1.0)) PQ_DOC_TRY(k.add(SetDeviceParam{pb.id, std::string(kValueParam), ppos}));
  PQ_DOC_TRY(regate(k, chain));
  e.devices.push_back(chain);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult setChainActive(const Project& p, DeviceId chain, bool active) {
  Result<ChainAt> c = chainAt(p, chain);
  if (!c) return std::move(c).error();
  Edit e = makeEdit(active ? "Activate Chain" : "Deactivate Chain");
  e.devices.push_back(chain);
  if (c->chain.active == active) return e;
  Steps k(p);
  PQ_DOC_TRY(k.add(SetDeviceSetting{chain, std::string(kChainOnSetting), active ? std::nullopt : std::optional<double>(0.0)}));
  PQ_DOC_TRY(regate(k, chain));
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult setChainSolo(const Project& p, DeviceId chain, bool solo, bool exclusive) {
  Result<ChainAt> c = chainAt(p, chain);
  if (!c) return std::move(c).error();
  Edit e = makeEdit(solo ? "Solo Chain" : "Unsolo Chain");
  e.devices.push_back(chain);
  Steps k(p);
  for (const RackChain& x : c->rack.chains) {
    const bool want = x.id == chain ? solo : (exclusive && solo ? false : x.solo);
    if (want != x.solo)
      PQ_DOC_TRY(k.add(SetDeviceSetting{x.id, std::string(kChainSoloSetting), want ? std::optional<double>(1.0) : std::nullopt}));
  }
  PQ_DOC_TRY(regate(k, chain));
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult setChainZone(const Project& p, IdSource& ids, DeviceId chain, ZoneKind kind, Zone zone) {
  Result<ChainAt> c = chainAt(p, chain);
  if (!c) return std::move(c).error();
  const KeyZoneUnit unit = c->rack.keyUnit;
  if (!zoneValid(zone, kind, unit)) return invalid("a zone is lo ≤ fade lo ≤ fade hi ≤ hi inside its range");
  if (kind != ZoneKind::Select && !c->rack.instrument) return invalid("key and velocity zones belong to an Instrument Rack");
  Edit e = makeEdit("Chain Zone");
  e.devices.push_back(chain);
  Steps k(p);
  const DeviceId zg = c->chain.zoneGroup(kind);
  const bool off = zoneOff(zone, kind, unit);
  const bool fi = instrumentFirst(p, c->rack, c->chain);
  if (off) {
    if (!zg.valid()) return e;
    PQ_DOC_TRY(k.add(RemoveDevice{zg}));
    PQ_DOC_TRY(relayChain(k, ids, c->rack, c->chain, idsOf(c->chain.devices), fi));
  } else if (zg.valid()) {  // a drag: the four values (param writes)
    const Track& t = *c->track;
    const dsp::Curve zc = zoneCurve(kind, unit);
    auto setV = [&](RackRole role, double v) -> Status {
      for (const DevicePtr& d : t.devices)
        if (d->group == zg && rackRole(*d) == role && isValueDevice(*d)) {
          const double pos = dsp::curvePosition(d->curve(kValueParam) ? *d->curve(kValueParam) : zc, v, positionOf(*d, kValueParam));
          if (pos != positionOf(*d, kValueParam, -1.0)) return k.add(SetDeviceParam{d->id, std::string(kValueParam), pos});
        }
      return ok();
    };
    PQ_DOC_TRY(setV(RackRole::ZoneLo, zone.lo));
    PQ_DOC_TRY(setV(RackRole::ZoneFadeLo, zone.fadeLo));
    PQ_DOC_TRY(setV(RackRole::ZoneFadeHi, zone.fadeHi));
    PQ_DOC_TRY(setV(RackRole::ZoneHi, zone.hi));
  } else {
    const BlockSet set = zoneBlocks(ids, chain, kind, unit, zone);
    PQ_DOC_TRY(addBlocks(k, c->track->id, set));
    if (kind == ZoneKind::Select) {
      Status st = ok();
      const DeviceId sel = selectorBlock(k, ids, c->rack, st);
      PQ_DOC_TRY(st);
      for (const DevicePtr& d : set.devices)
        if (isValueDevice(*d) && rackRole(*d) == RackRole::ZoneValue)
          PQ_DOC_TRY(k.add(AddWire{plainWire(ids, sel, kValueOutput, d->id, kValueParam)}));
    }
    PQ_DOC_TRY(relayChain(k, ids, c->rack, c->chain, idsOf(c->chain.devices), fi));
  }
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult setKeyZoneUnit(const Project& p, IdSource& ids, DeviceId rack, KeyZoneUnit unit) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  Edit e = makeEdit("Key Zone Unit");
  if (r->rack.keyUnit == unit) return e;
  Steps k(p);
  for (const RackChain& c : r->rack.chains) {
    PQ_DOC_TRY(k.add(SetDeviceSetting{c.id, std::string(kKeyUnitSetting),
                                      unit == KeyZoneUnit::Degree ? std::optional<double>(1.0) : std::nullopt}));
    if (c.keyZone.valid()) {  // a key unit change resets the key zones (T4.8)
      PQ_DOC_TRY(k.add(RemoveDevice{c.keyZone}));
      PQ_DOC_TRY(relayChain(k, ids, r->rack, c, idsOf(c.devices), instrumentFirst(p, r->rack, c)));
    }
  }
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult setVoiceChains(const Project& p, DeviceId rack, bool perVoice) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  if (!r->rack.instrument) return invalid("per-voice chains belong to an Instrument Rack");
  Edit e = makeEdit("Per-Voice Chains");
  for (const RackChain& c : r->rack.chains) {
    if (!instrumentFirst(p, r->rack, c)) continue;
    const DeviceId last = c.velocityZone.valid() ? c.velocityZone : c.keyZone.valid() ? c.keyZone : c.devices.front()->id;
    for (const Wire& w : wiresOf(p))
      if (w.from == last && w.port == kMainPort && !w.toMixer() && w.single == perVoice)
        batchOf(e).commands.push_back(SetWireSignal{w.id, !perVoice, WireReducer::Sum});
  }
  return e;
}

// ---- Macros ----------------------------------------------------------------------------------------------------------

EditResult mapMacro(const Project& p, IdSource& ids, DeviceId rack, uint32_t macro, DeviceId device, std::string param,
                    std::optional<double> macroValue) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  if (macro >= kMacroCount) return invalid("a rack has 16 macros");
  const Device* target = findDevice(p, device).device;
  if (target == nullptr) return notFound("Map: the target device is gone");
  if (!r->rack.holds(p, device) || std::any_of(r->rack.macros.begin(), r->rack.macros.end(),
                                                [&](const RackMacro& m) { return m.block == device; }))
    return invalid("A macro maps controls inside its rack");
  Edit e = makeEdit("Map Macro");
  Steps k(p);
  // Whatever mapped the control before lets go of it.
  for (const RackMapping& m : r->rack.mappings)
    if (m.device == device && m.param == param) PQ_DOC_TRY(k.add(RemoveWire{m.wire}));
  const double was = positionOf(*target, param);
  const bool fresh = !r->rack.macros[macro].block.valid();
  Result<DeviceId> mb = macroBlock(k, ids, r->rack, macro, macroValue ? *macroValue / kMacroMax : was);
  if (!mb) return std::move(mb).error();
  if (!fresh && macroValue) PQ_DOC_TRY(k.add(SetDeviceParam{*mb, std::string(kValueParam), std::clamp(*macroValue / kMacroMax, 0.0, 1.0)}));
  Wire w = link(ids, *mb, kValueOutput, device, param);
  w.chain = false;
  w.law = WireLaw::Travel;
  w.amount = 1.0;
  PQ_DOC_TRY(k.add(SetDeviceParam{device, param, 0.0}));
  e.wires.push_back(w.id);
  PQ_DOC_TRY(k.add(AddWire{std::move(w)}));
  e.devices.push_back(device);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult unmapMacro(const Project& p, WireId mapping) {
  const Wire* w = findWire(p, mapping);
  if (w == nullptr) return notFound("mapping not found");
  Edit e = makeEdit("Unmap Macro");
  Steps k(p);
  // The control keeps where the macro put it.
  const Device* macro = findDevice(p, w->from).device;
  const Device* target = findDevice(p, w->to).device;
  if (macro != nullptr && target != nullptr) {
    const double pos = std::clamp(positionOf(*target, w->param) + w->amount * positionOf(*macro, kValueParam), 0.0, 1.0);
    PQ_DOC_TRY(k.add(SetDeviceParam{w->to, w->param, pos}));
  }
  PQ_DOC_TRY(k.add(RemoveWire{mapping}));
  // A macro block nothing reads any more goes (an unmapped macro is no block: racks.h).
  if (macro != nullptr && macro->setting(kMacroSetting) != nullptr) {
    bool used = false;
    for (const Wire& x : wiresOf(k.doc())) used = used || (!x.fromNoteSource() && x.from == macro->id);
    if (!used) PQ_DOC_TRY(k.add(RemoveDevice{macro->id}));
  }
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult unmapParam(const Project& p, DeviceId device, std::string_view param) {
  for (const Wire& w : wiresOf(p))
    if (w.to == device && w.param == param && w.travels() && !w.fromNoteSource())
      if (const Device* m = findDevice(p, w.from).device; m != nullptr && isValueDevice(*m) && m->setting(kMacroSetting))
        return unmapMacro(p, w.id);
  return makeEdit("Unmap Macro");
}

EditResult setMacroMapping(const Project& p, WireId mapping, double min, double max) {
  const Wire* w = findWire(p, mapping);
  if (w == nullptr || !w->travels()) return notFound("mapping not found");
  if (!(min >= 0.0 && min <= 1.0 && max >= 0.0 && max <= 1.0)) return invalid("Map: min / max are knob positions in [0, 1]");
  Edit e = makeEdit("Macro Mapping");
  batchOf(e).commands.push_back(SetDeviceParam{w->to, w->param, min});
  batchOf(e).commands.push_back(SetWireAmount{mapping, max - min});
  return e;
}

EditResult setMacroValue(const Project& p, DeviceId rack, uint32_t macro, double value) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  Edit e = makeEdit("Macro");
  if (macro >= kMacroCount || !r->rack.macros[macro].block.valid()) return e;  // unmapped: nothing to turn
  batchOf(e).commands.push_back(
      SetDeviceParam{r->rack.macros[macro].block, std::string(kValueParam), std::clamp(value, 0.0, kMacroMax) / kMacroMax});
  return e;
}

EditResult setSelectorValue(const Project& p, DeviceId rack, double value) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  Edit e = makeEdit("Chain Selector");
  if (!r->rack.selector.valid()) return e;
  batchOf(e).commands.push_back(
      SetDeviceParam{r->rack.selector, std::string(kValueParam), std::clamp(std::round(value), 0.0, kSelectMax) / kSelectMax});
  return e;
}

EditResult renameMacro(const Project& p, DeviceId rack, uint32_t macro, std::string name) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  if (macro >= kMacroCount || !r->rack.macros[macro].block.valid()) return invalid("Map the macro first (an unmapped macro has no block)");
  return renameDevice(p, r->rack.macros[macro].block, std::move(name));
}

EditResult storeVariation(const Project& p, DeviceId rack, uint32_t index) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  Edit e = makeEdit("Store Variation");
  const uint32_t n = index == kAppend || index >= r->rack.variations ? r->rack.variations + 1 : index + 1;
  for (const RackMacro& m : r->rack.macros)
    if (m.block.valid())
      batchOf(e).commands.push_back(SetDeviceSetting{m.block, std::string(kVariationPrefix) + std::to_string(n), m.value / kMacroMax});
  return e;
}

EditResult recallVariation(const Project& p, DeviceId rack, uint32_t index) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  if (index >= r->rack.variations) return invalid("no such variation");
  Edit e = makeEdit("Recall Variation");
  const std::string key = std::string(kVariationPrefix) + std::to_string(index + 1);
  for (const RackMacro& m : r->rack.macros) {
    if (!m.block.valid()) continue;
    const Device* d = findDevice(p, m.block).device;
    if (const DeviceValue* v = d != nullptr ? d->setting(key) : nullptr)
      batchOf(e).commands.push_back(SetDeviceParam{m.block, std::string(kValueParam), std::clamp(v->value, 0.0, 1.0)});
  }
  return e;
}

EditResult deleteVariation(const Project& p, DeviceId rack, uint32_t index) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  if (index >= r->rack.variations) return invalid("no such variation");
  Edit e = makeEdit("Delete Variation");
  auto key = [](uint32_t n) { return std::string(kVariationPrefix) + std::to_string(n); };
  for (const RackMacro& m : r->rack.macros) {
    if (!m.block.valid()) continue;
    const Device& d = *findDevice(p, m.block).device;
    for (uint32_t n = index + 1; n <= r->rack.variations; ++n) {
      const DeviceValue* next = n < r->rack.variations ? d.setting(key(n + 1)) : nullptr;
      batchOf(e).commands.push_back(SetDeviceSetting{m.block, key(n), next ? std::optional<double>(next->value) : std::nullopt});
    }
  }
  return e;
}

EditResult randomizeMacros(const Project& p, DeviceId rack, uint64_t seed) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  Edit e = makeEdit("Randomize Macros");
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int> step(0, static_cast<int>(kMacroMax));
  for (const RackMacro& m : r->rack.macros)
    if (m.block.valid() && !m.excluded)
      batchOf(e).commands.push_back(SetDeviceParam{m.block, std::string(kValueParam), step(rng) / kMacroMax});
  return e;
}

EditResult setMacroExcluded(const Project& p, DeviceId rack, uint32_t macro, bool excluded) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  if (macro >= kMacroCount || !r->rack.macros[macro].block.valid()) return invalid("Map the macro first");
  Edit e = makeEdit(excluded ? "Exclude Macro" : "Include Macro");
  batchOf(e).commands.push_back(SetDeviceSetting{r->rack.macros[macro].block, std::string(kRandExcludeSetting),
                                                 excluded ? std::optional<double>(1.0) : std::nullopt});
  return e;
}

// ---- Ctrl+G / Ctrl+Shift+G in the Device view -------------------------------------------------------------------------

EditResult groupDevices(const Project& p, IdSource& ids, std::span<const DeviceId> devices) {
  if (devices.empty()) return invalid("Group: nothing selected");
  const DeviceLoc first = findDevice(p, devices.front());
  if (first.track == nullptr) return notFound("device not found");
  const Track& t = *first.track;
  const MainPath path = mainPath(p, t);
  std::vector<int32_t> at;
  for (const DeviceId d : devices) {
    const DeviceLoc l = findDevice(p, d);
    if (l.track != first.track) return invalid("Group: devices of one track");
    const int32_t i = path.indexOf(d);
    if (i < 0) return invalid("Group: devices of the track's chain (its main path)");
    at.push_back(i);
  }
  std::sort(at.begin(), at.end());
  for (size_t i = 1; i < at.size(); ++i)
    if (at[i] != at[i - 1] + 1) return invalid("Group: neighbouring devices of the chain");
  std::vector<DeviceId> order;
  for (const int32_t i : at) order.push_back(path.blocks[static_cast<size_t>(i)]->id);
  const Device& head = *path.blocks[static_cast<size_t>(at.front())];
  const bool instrument = head.kind == DeviceKind::Instrument || (isGroupDevice(head) && isInstrumentGroup(p, head.id));
  Edit e = makeEdit("Group");
  Steps k(p);
  const BlockSet set = chainBlocks(ids, {}, "Chain 1", instrument);
  const DeviceId chain = set.devices.front()->id;
  PQ_DOC_TRY(addBlocks(k, t.id, set));
  for (const DeviceId d : order) PQ_DOC_TRY(detach(k, ids, nullptr, d));
  PQ_DOC_TRY(splice(k, ids, t.id, chain, static_cast<uint32_t>(at.front())));
  for (const DeviceId d : order) PQ_DOC_TRY(k.add(SetDeviceGroup{d, chain}));
  PQ_DOC_TRY(relay(k, ids, chain, order, instrument, instrument, false));
  e.devices.push_back(chain);
  e.tracks.push_back(t.id);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult ungroupDevices(const Project& p, IdSource& ids, DeviceId rack) {
  Result<RackAt> r = rackAt(p, rack);
  if (!r) return std::move(r).error();
  if (r->rack.chains.size() != 1) return invalid("Ungroup: a rack with one chain");
  const RackChain& c = r->rack.chains.front();
  Edit e = makeEdit("Ungroup");
  Steps k(p);
  const Ends en = endsOf(p, c.id);
  Wire feed, sum;
  if (en.feed) feed = *en.feed;
  if (en.sum) sum = *en.sum;
  const std::vector<DeviceId> devs = idsOf(c.devices);
  for (const DeviceId d : devs) PQ_DOC_TRY(k.add(SetDeviceGroup{d, r->rack.parent}));
  for (const RackMacro& m : r->rack.macros)
    if (m.block.valid()) PQ_DOC_TRY(k.add(RemoveDevice{m.block}));
  if (r->rack.selector.valid()) PQ_DOC_TRY(k.add(RemoveDevice{r->rack.selector}));
  PQ_DOC_TRY(k.add(RemoveDevice{c.id}));  // with its mixer and zones (and every wire into them)
  auto join = [&](Wire w) -> Status {
    w.id = ids.wire();
    w.chain = false;
    w.single = false;
    if (hasEnds(k.doc(), w)) return ok();
    return k.add(AddWire{std::move(w)});
  };
  if (devs.empty()) {
    if (en.feed && en.sum && !r->rack.instrument) {
      Wire j = sum;
      j.from = feed.from;
      j.noteSource = feed.noteSource;
      j.port = feed.port;
      PQ_DOC_TRY(join(j));
    }
  } else {
    if (en.feed) {
      Wire j = feed;
      j.to = devs.front();
      j.param = std::string(kMainPort);
      PQ_DOC_TRY(join(j));
    }
    if (en.sum) {
      Wire j = sum;
      j.from = devs.back();
      j.noteSource = {};
      j.port = std::string(kMainPort);
      PQ_DOC_TRY(join(j));
    }
    // (the links between the devices stay as they were inside the chain)
  }
  e.devices = devs;
  e.tracks.push_back(r->track->id);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

}  // namespace pq::doc::ops
