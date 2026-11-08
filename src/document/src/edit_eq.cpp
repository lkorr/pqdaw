// EQ.3: the EQ's band ops (pq/document/edit_ops.h "EQ.3"; pq/document/modules.h "the band and the EQ").
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "edit_common.h"
#include "edit_paths.h"
#include "pq/document/modules.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

Wire seriesWire(IdSource& ids, DeviceId from, std::string_view port, DeviceId to, std::string_view param) {
  Wire w;
  w.id = ids.wire();
  w.from = from;
  w.port = std::string(port);
  w.to = to;
  w.param = std::string(param);
  w.law = WireLaw::Add;
  return w;
}

// Chain `chain`'s series made again (k.doc()): its main-port wires between its In terminal, its direct members and its
// Out terminal removed, then In → bands → Out (an empty list: In → Out). Every other wire stays.
Status relaySeries(Steps& k, IdSource& ids, DeviceId chain, const std::vector<DeviceId>& bands) {
  const DeviceLoc loc = findDevice(k.doc(), chain);
  if (loc.track == nullptr) return notFound("chain not found");
  auto member = [&](DeviceId d) {
    for (const DevicePtr& x : loc.track->devices)
      if (x->id == d) return x->group == chain;
    return false;
  };
  std::vector<WireId> gone;
  for (const Wire& w : wiresOf(k.doc())) {
    if (w.toMixer() || w.fromNoteSource()) continue;
    const bool fromOk = (w.from == chain && w.port == kGroupInPort) || (w.port == kMainPort && member(w.from));
    const bool toOk = (w.to == chain && w.param == kGroupOutPort) || (w.param == kMainPort && member(w.to));
    if (fromOk && toOk) gone.push_back(w.id);
  }
  for (const WireId w : gone) PQ_DOC_TRY(k.add(RemoveWire{w}));
  DeviceId at = chain;
  std::string_view port = kGroupInPort;
  for (const DeviceId b : bands) {
    PQ_DOC_TRY(k.add(AddWire{seriesWire(ids, at, port, b, kMainPort)}));
    at = b;
    port = kMainPort;
  }
  return k.add(AddWire{seriesWire(ids, at, port, chain, kGroupOutPort)});
}

// The EQ's band strip laid out again for its bands now (one SetGroupPanel; nothing when it is unchanged).
Status relayPanel(Steps& k, DeviceId eq) {
  const std::optional<EqInfo> e = eqInfo(k.doc(), eq);
  if (!e) return notFound("EQ not found");
  const Device* g = findDevice(k.doc(), eq).device;
  const Panel before = g->panel ? *g->panel : Panel{};
  Panel after = layEqStrip(before, e->bands);
  if (after == before) return ok();
  return k.add(SetGroupPanel{eq, std::make_shared<const Panel>(std::move(after))});
}

std::vector<DeviceId> groupsOf(const std::vector<BandInfo>& bands) {
  std::vector<DeviceId> out;
  for (const BandInfo& b : bands) out.push_back(b.group);
  return out;
}

double frequencyOf(const Project& p, const BandInfo& b) {
  const Device* f = findDevice(p, b.frequency).device;
  return f != nullptr ? valueBlockUnits(*f) : 0.0;
}

// A value block set to `value` in its curve's units (the knob position that gives it).
Status setUnits(Steps& k, DeviceId block, double value) {
  const Device* d = findDevice(k.doc(), block).device;
  if (d == nullptr || d->curve(kValueParam) == nullptr) return notFound("value block not found");
  const dsp::Curve& c = *d->curve(kValueParam);
  const DeviceValue* now = d->param(kValueParam);
  const double pos = dsp::curvePosition(c, dsp::curveOutput(c, value), now != nullptr ? now->value : 0.0);
  if (now != nullptr && now->value == pos) return ok();
  return k.add(SetDeviceParam{block, std::string(kValueParam), pos});
}

bool soloed(const Project& p, DeviceId band) {
  const Device* d = findDevice(p, band).device;
  const DeviceValue* v = d != nullptr ? d->setting(kBandSoloSetting) : nullptr;
  return v != nullptr && v->value != 0.0;
}

// Every band of the chain holding a restore state gets it back (the solo released).
Status releaseSolo(Steps& k, const std::vector<BandInfo>& bands) {
  for (const BandInfo& b : bands) {
    const Device* d = findDevice(k.doc(), b.group).device;
    if (d == nullptr) continue;
    if (const DeviceValue* r = d->setting(kSoloRestoreSetting)) {
      const bool on = r->value != 0.0;
      PQ_DOC_TRY(k.add(SetDeviceSetting{b.group, std::string(kSoloRestoreSetting), std::nullopt}));
      if (d->active != on) PQ_DOC_TRY(k.add(SetDeviceActive{b.group, on}));
    }
    if (d->setting(kBandSoloSetting) != nullptr)
      PQ_DOC_TRY(k.add(SetDeviceSetting{b.group, std::string(kBandSoloSetting), std::nullopt}));
  }
  return ok();
}

Result<std::pair<EqInfo, uint32_t>> bandAt(const Project& p, DeviceId band) {
  std::optional<std::pair<EqInfo, uint32_t>> e = eqOfBand(p, band);
  if (!e) return invalid("not a band of an EQ");
  return std::move(*e);
}

}  // namespace

EditResult addBand(const Project& p, IdSource& ids, DeviceId eq, uint32_t chain, uint32_t type, double freqHz, double gainDb) {
  const std::optional<EqInfo> e = eqInfo(p, eq);
  if (!e) return invalid("not an EQ");
  if (chain > 1) return invalid("an EQ has chains A and B");
  if (type > 7) return invalid("a band's type is 0 … 7");
  if (!std::isfinite(freqHz) || !std::isfinite(gainDb)) return invalid("a band's frequency and gain are finite");
  if (e->bands[chain].size() >= kMaxBandsPerChain) return invalid("A chain holds at most 32 bands");
  const Track* track = findDevice(p, eq).track;
  Edit out = makeEdit("Add Band");
  Steps k(p);
  EqBandOptions bo;
  bo.inGroup = e->chain[chain];
  bo.type = type;
  bo.freqHz = std::clamp(freqHz, 0.0, kBandMaxHz);
  bo.gainDb = std::clamp(gainDb, -24.0, 24.0);
  bo.name = "Band " + std::to_string(e->bands[0].size() + e->bands[1].size() + 1);
  const BlockSet set = eqBandBlocks(ids, bo);
  const DeviceId band = set.devices.front()->id;
  for (const DevicePtr& d : set.devices) PQ_DOC_TRY(k.add(AddDevice{track->id, d, kAppend}));
  for (const Wire& w : set.wires) PQ_DOC_TRY(k.add(AddWire{w}));
  std::vector<DeviceId> order = groupsOf(e->bands[chain]);
  size_t at = 0;
  while (at < e->bands[chain].size() && frequencyOf(p, e->bands[chain][at]) <= bo.freqHz) ++at;
  order.insert(order.begin() + static_cast<std::ptrdiff_t>(at), band);
  PQ_DOC_TRY(relaySeries(k, ids, e->chain[chain], order));
  // A band added to a soloed chain is held off like the others.
  for (const BandInfo& b : e->bands[chain])
    if (soloed(p, b.group)) {
      PQ_DOC_TRY(k.add(SetDeviceSetting{band, std::string(kSoloRestoreSetting), 1.0}));
      PQ_DOC_TRY(k.add(SetDeviceActive{band, false}));
      break;
    }
  PQ_DOC_TRY(relayPanel(k, eq));
  out.devices.push_back(band);
  for (const DevicePtr& d : set.devices)
    if (d->id != band) out.devices.push_back(d->id);
  batchOf(out).commands = std::move(k.commands);
  return out;
}

EditResult removeBand(const Project& p, IdSource& ids, DeviceId band) {
  Result<std::pair<EqInfo, uint32_t>> at = bandAt(p, band);
  if (!at) return std::move(at).error();
  const auto& [e, c] = *at;
  Edit out = makeEdit("Delete Band");
  Steps k(p);
  if (soloed(p, band)) PQ_DOC_TRY(releaseSolo(k, e.bands[c]));
  PQ_DOC_TRY(k.add(RemoveDevice{band}));
  std::vector<DeviceId> order;
  for (const BandInfo& b : e.bands[c])
    if (b.group != band) order.push_back(b.group);
  PQ_DOC_TRY(relaySeries(k, ids, e.chain[c], order));
  PQ_DOC_TRY(relayPanel(k, e.group));
  batchOf(out).commands = std::move(k.commands);
  return out;
}

EditResult setBandType(const Project& p, DeviceId band, uint32_t type) {
  const std::optional<BandInfo> b = bandInfo(p, band);
  if (!b || !b->type.valid()) return invalid("not a band with a Type");
  if (type > 7) return invalid("a band's type is 0 … 7");
  Edit out = makeEdit("Band Type");
  Steps k(p);
  PQ_DOC_TRY(setUnits(k, b->type, static_cast<double>(type)));
  batchOf(out).commands = std::move(k.commands);
  return out;
}

EditResult setBandStages(const Project& p, DeviceId band, uint32_t stages) {
  const std::optional<BandInfo> b = bandInfo(p, band);
  if (!b || !b->stages.valid()) return invalid("not a band with Stages");
  if (stages < 1 || stages > 64) return invalid("a band has 1 … 64 stages");
  Edit out = makeEdit("Band Stages");
  Steps k(p);
  PQ_DOC_TRY(setUnits(k, b->stages, static_cast<double>(stages)));
  batchOf(out).commands = std::move(k.commands);
  return out;
}

EditResult setEqMode(const Project& p, IdSource& ids, DeviceId eq, EqMode mode) {
  const std::optional<EqInfo> e = eqInfo(p, eq);
  if (!e || !e->mode.valid()) return invalid("not an EQ");
  Edit out = makeEdit("EQ Mode");
  Steps k(p);
  PQ_DOC_TRY(setUnits(k, e->mode, static_cast<double>(mode)));
  if (mode != EqMode::Stereo && e->bands[1].empty() && !e->bands[0].empty()) {
    // The first split: chain B starts as a copy of chain A.
    const Track* track = findDevice(p, eq).track;
    std::vector<DeviceId> copies;
    for (const BandInfo& b : e->bands[0]) {
      const DeviceId one[] = {b.group};
      Result<BlockSet> set = copyBlocks(p, one);
      if (!set) return std::move(set).error();
      BlockSet fresh = remapBlocks(*set, ids);
      auto top = std::make_shared<Device>(*fresh.devices.front());
      top->group = e->chain[1];
      fresh.devices.front() = std::move(top);
      copies.push_back(fresh.devices.front()->id);
      for (const DevicePtr& d : fresh.devices) PQ_DOC_TRY(k.add(AddDevice{track->id, d, kAppend}));
      for (const Wire& w : fresh.wires) PQ_DOC_TRY(k.add(AddWire{w}));
      out.devices.push_back(copies.back());
    }
    PQ_DOC_TRY(relaySeries(k, ids, e->chain[1], copies));
    PQ_DOC_TRY(relayPanel(k, eq));
  }
  batchOf(out).commands = std::move(k.commands);
  return out;
}

EditResult setBandBypass(const Project& p, DeviceId band, bool bypass) {
  const Device* d = findDevice(p, band).device;
  if (d == nullptr || !bandInfo(p, band)) return invalid("not a band");
  Edit out = makeEdit(bypass ? "Bypass Band" : "Enable Band");
  if (d->setting(kSoloRestoreSetting) != nullptr) {  // held off by a solo: what the unsolo restores
    batchOf(out).commands.push_back(SetDeviceSetting{band, std::string(kSoloRestoreSetting), bypass ? 0.0 : 1.0});
    return out;
  }
  if (d->active == !bypass) return out;
  batchOf(out).commands.push_back(SetDeviceActive{band, !bypass});
  return out;
}

EditResult setBandSolo(const Project& p, DeviceId band, bool solo) {
  Result<std::pair<EqInfo, uint32_t>> at = bandAt(p, band);
  if (!at) return std::move(at).error();
  const auto& [e, c] = *at;
  Edit out = makeEdit(solo ? "Solo Band" : "Unsolo Band");
  if (soloed(p, band) == solo) return out;
  Steps k(p);
  if (!solo) {
    PQ_DOC_TRY(releaseSolo(k, e.bands[c]));
  } else {
    PQ_DOC_TRY(releaseSolo(k, e.bands[c]));  // (another band's solo first)
    for (const BandInfo& b : e.bands[c]) {
      const Device* d = findDevice(k.doc(), b.group).device;
      PQ_DOC_TRY(k.add(SetDeviceSetting{b.group, std::string(kSoloRestoreSetting), d->active ? 1.0 : 0.0}));
      const bool on = b.group == band;
      if (d->active != on) PQ_DOC_TRY(k.add(SetDeviceActive{b.group, on}));
    }
    PQ_DOC_TRY(k.add(SetDeviceSetting{band, std::string(kBandSoloSetting), 1.0}));
  }
  batchOf(out).commands = std::move(k.commands);
  return out;
}

}  // namespace pq::doc::ops
