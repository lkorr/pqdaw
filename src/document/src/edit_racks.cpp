// Rack edit builders (GU.9: a rack is a group of parallel chain groups): pq/document/edit_ops.h, "Racks".
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "edit_common.h"
#include "edit_paths.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

struct RackAt {
  const Track* track = nullptr;
  const Device* rack = nullptr;
};
Result<RackAt> rackOf(const Project& p, DeviceId id) {
  const DeviceLoc d = findDevice(p, id);
  if (d.device == nullptr) return notFound("rack not found");
  if (!isGroupDevice(*d.device)) return invalid("a rack is a group");
  return RackAt{d.track, d.device};
}

// The selector of rack `rack`: value blocks directly in it — kChainSelectName and its constants "1" / "0" (the gates'
// clip bounds) — or invalid ones.
constexpr std::string_view kOne = "1", kZero = "0";
struct Selector {
  DeviceId select, one, zero;
};
Selector selectorOf(const Track& t, DeviceId rack) {
  Selector s;
  for (const DevicePtr& d : t.devices) {
    if (d->group != rack || !isValueDevice(*d)) continue;
    if (d->name == kChainSelectName) s.select = d->id;
    else if (d->name == kOne) s.one = d->id;
    else if (d->name == kZero) s.zero = d->id;
  }
  if (!s.one.valid() || !s.zero.valid()) s.select = {};
  return s;
}

Wire wire(IdSource& ids, DeviceId from, std::string_view port, DeviceId to, std::string_view param) {
  Wire w;
  w.id = ids.wire();
  w.from = from;
  w.port = std::string(port);
  w.to = to;
  w.param = std::string(param);
  return w;
}

DevicePtr block(DeviceId id, DeviceId group, std::string patch, std::string name) {
  auto d = std::make_shared<Device>(*makeDevice(id, std::move(patch), DeviceKind::AudioEffect));
  d->name = std::move(name);
  d->group = group;
  return d;
}

// A value block: 0 … kSelectMax in steps of 1 at `value`, or (`constant`) always `value`.
DevicePtr valueBlock(DeviceId id, DeviceId group, std::string name, double value, bool constant = false) {
  auto d = std::make_shared<Device>(*block(id, group, std::string(kValueDevice), std::move(name)));
  dsp::Curve c = constant ? dsp::curvePreset(dsp::CurvePreset::Linear, value, value) : dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, kSelectMax);
  if (constant) value = kSelectMax;  // (the knob's position: 1)
  else c.step = 1.0;
  d->curves = {ControlCurve{std::string(kValueParam), c}};
  d->params = {DeviceValue{std::string(kValueParam), value / kSelectMax}};
  return d;
}

// The gate of `chain` (its wire into the rack's Out replaced): lo / hi value blocks (whole steps, as the selector) and
// gate = clip(min(select − lo, hi − select) + 1, 0, 1) — 1 inside [lo, hi], 0 outside — × the chain's output. Reals only
// (param rate but the mul).
Status gateChain(Steps& k, IdSource& ids, TrackId track, DeviceId rack, const Selector& sel, const Device& chain) {
  const std::string tag = chain.name.empty() ? std::string("chain") : chain.name;
  const DeviceId lo = ids.device(), hi = ids.device(), dLo = ids.device(), dHi = ids.device(), m = ids.device(),
                 plus = ids.device(), gate = ids.device(), mul = ids.device();
  PQ_DOC_TRY(k.add(AddDevice{track, valueBlock(lo, rack, tag + " lo", 0.0), kAppend}));
  PQ_DOC_TRY(k.add(AddDevice{track, valueBlock(hi, rack, tag + " hi", kSelectMax), kAppend}));
  const std::string a(kAtomDevicePrefix);
  PQ_DOC_TRY(k.add(AddDevice{track, block(dLo, rack, a + "sub", "select − lo"), kAppend}));
  PQ_DOC_TRY(k.add(AddDevice{track, block(dHi, rack, a + "sub", "hi − select"), kAppend}));
  PQ_DOC_TRY(k.add(AddDevice{track, block(m, rack, a + "min", "min"), kAppend}));
  PQ_DOC_TRY(k.add(AddDevice{track, block(plus, rack, a + "add", "+ 1"), kAppend}));
  PQ_DOC_TRY(k.add(AddDevice{track, block(gate, rack, a + "clip", tag + " gate"), kAppend}));
  PQ_DOC_TRY(k.add(AddDevice{track, block(mul, rack, a + "mul", tag + " × gate"), kAppend}));
  for (const Wire& w : wiresOf(k.doc()))
    if (w.from == chain.id && w.to == rack && w.param == kGroupOutPort) PQ_DOC_TRY(k.add(RemoveWire{w.id}));
  const std::string y(kValueOutput);
  for (Wire w : {wire(ids, sel.select, y, dLo, "a"), wire(ids, lo, y, dLo, "b"), wire(ids, hi, y, dHi, "a"),
                 wire(ids, sel.select, y, dHi, "b"), wire(ids, dLo, "out", m, "a"), wire(ids, dHi, "out", m, "b"),
                 wire(ids, m, "out", plus, "a"), wire(ids, sel.one, y, plus, "b"), wire(ids, plus, "out", gate, "x"),
                 wire(ids, sel.zero, y, gate, "lo"), wire(ids, sel.one, y, gate, "hi"),
                 wire(ids, chain.id, kMainPort, mul, "a"), wire(ids, gate, "out", mul, "b"),
                 wire(ids, mul, "out", rack, kGroupOutPort)})
    PQ_DOC_TRY(k.add(AddWire{std::move(w)}));
  return ok();
}

}  // namespace

EditResult addRack(const Project& p, IdSource& ids, TrackId track, bool instrument, uint32_t index) {
  const Track* t = findTrackPtr(p, track);
  if (t == nullptr) return notFound("track not found");
  if (instrument && t->kind != TrackKind::Midi) return invalid("instruments go on MIDI tracks");
  Edit e = makeEdit(instrument ? "Add Instrument Rack" : "Add Audio Effect Rack");
  Steps k(p);
  const EditResult pasted = pasteBlocks(p, ids, track, rackBlocks(ids, instrument));
  if (!pasted) return pasted;
  PQ_DOC_TRY(k.add(pasted));
  const DeviceId rack = pasted->devices.front();
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
    PQ_DOC_TRY(splice(k, ids, track, rack, index));
  }
  e.devices = pasted->devices;
  e.wires = pasted->wires;
  e.tracks.push_back(track);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult addChain(const Project& p, IdSource& ids, DeviceId rack, std::string name) {
  Result<RackAt> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  const Track& t = *r->track;
  if (name.empty()) {
    const std::vector<const Device*> chains = rackChains(p, t, rack);
    for (uint32_t n = 1; name.empty(); ++n) {
      std::string c = "Chain " + std::to_string(n);
      if (std::none_of(chains.begin(), chains.end(), [&](const Device* d) { return d->name == c; })) name = std::move(c);
    }
  }
  BlockSet set = chainBlocks(ids, rack, std::move(name));
  Edit e = makeEdit("Add Chain");
  Steps k(p);
  const DeviceId chain = set.devices.front()->id;
  for (const DevicePtr& d : set.devices) PQ_DOC_TRY(k.add(AddDevice{t.id, d, kAppend}));
  for (const Wire& w : set.wires) PQ_DOC_TRY(k.add(AddWire{w}));
  if (const Selector sel = selectorOf(t, rack); sel.select.valid())
    PQ_DOC_TRY(gateChain(k, ids, t.id, rack, sel, *findDevice(k.doc(), chain).device));
  e.devices.push_back(chain);
  e.tracks.push_back(t.id);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult addChainSelector(const Project& p, IdSource& ids, DeviceId rack) {
  Result<RackAt> r = rackOf(p, rack);
  if (!r) return std::move(r).error();
  const Track& t = *r->track;
  if (selectorOf(t, rack).select.valid()) return invalid("the rack has a chain selector");
  Edit e = makeEdit("Add Chain Selector");
  Steps k(p);
  const Selector sel{ids.device(), ids.device(), ids.device()};
  PQ_DOC_TRY(k.add(AddDevice{t.id, valueBlock(sel.select, rack, std::string(kChainSelectName), 0.0), kAppend}));
  PQ_DOC_TRY(k.add(AddDevice{t.id, valueBlock(sel.one, rack, std::string(kOne), 1.0, true), kAppend}));
  PQ_DOC_TRY(k.add(AddDevice{t.id, valueBlock(sel.zero, rack, std::string(kZero), 0.0, true), kAppend}));
  for (const Device* c : rackChains(p, t, rack)) PQ_DOC_TRY(gateChain(k, ids, t.id, rack, sel, *c));
  e.devices.push_back(sel.select);
  e.tracks.push_back(t.id);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult addToChain(const Project& p, IdSource& ids, DeviceId chain, std::string patch, DeviceKind kind) {
  Result<RackAt> c = rackOf(p, chain);
  if (!c) return std::move(c).error();
  if (patch.empty()) return invalid("no device patch");
  if (kind == DeviceKind::Instrument && c->track->kind != TrackKind::Midi) return invalid("instruments go on MIDI tracks");
  Edit e = makeEdit("Add Device");
  Steps k(p);
  const DeviceId id = ids.device();
  auto d = std::make_shared<Device>(*makeDevice(id, std::move(patch), kind));
  d->group = chain;
  PQ_DOC_TRY(k.add(AddDevice{c->track->id, std::move(d), kAppend}));
  bool fed = false;
  for (const Wire& w : wiresOf(p))
    if (!w.toMixer() && w.to == chain && w.param == kGroupOutPort) {
      PQ_DOC_TRY(k.add(RemoveWire{w.id}));
      Wire in = w;
      in.id = ids.wire();
      in.to = id;
      in.param = std::string(kMainPort);
      in.chain = true;
      PQ_DOC_TRY(k.add(AddWire{std::move(in)}));
      fed = true;
    }
  if (!fed) {
    Wire in = wire(ids, chain, kGroupInPort, id, kMainPort);
    in.chain = true;
    PQ_DOC_TRY(k.add(AddWire{std::move(in)}));
  }
  Wire out = wire(ids, id, kMainPort, chain, kGroupOutPort);
  out.chain = true;
  PQ_DOC_TRY(k.add(AddWire{std::move(out)}));
  e.devices.push_back(id);
  e.tracks.push_back(c->track->id);
  batchOf(e).commands = std::move(k.commands);
  return e;
}

EditResult mapMacro(const Project& p, IdSource& ids, DeviceId macro, DeviceId device, std::string param, double min,
                    double max) {
  const DeviceLoc m = findDevice(p, macro);
  if (m.device == nullptr || !isValueDevice(*m.device)) return invalid("a macro is a value block");
  if (findDevice(p, device).device == nullptr) return notFound("Map: the target device is gone");
  if (!(min >= 0.0 && min <= 1.0 && max >= 0.0 && max <= 1.0)) return invalid("Map: min / max are knob positions in [0, 1]");
  Edit e = makeEdit("Map Macro");
  Wire w = wire(ids, macro, kValueOutput, device, param);
  w.law = WireLaw::Travel;
  w.amount = max - min;
  e.wires.push_back(w.id);
  batchOf(e).commands.push_back(SetDeviceParam{device, param, min});
  batchOf(e).commands.push_back(AddWire{std::move(w)});
  e.devices.push_back(device);
  return e;
}

}  // namespace pq::doc::ops
