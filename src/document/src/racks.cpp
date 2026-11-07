// GU.9 v2: racks are parallel groups, derived from the wiring (pq/document/racks.h).
#include "pq/document/racks.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "pq/document/instrument_groups.h"
#include "pq/dsp/curve.h"

namespace pq::doc {

namespace {

constexpr double kZoneEps = 1e-3;     // select / velocity / degree units: a bound is inclusive (T4.8's)
constexpr double kZoneEpsLog = 1e-4;  // Hz key zones, in ln(Hz): ≈ 0.17 ¢

const DeviceValue* settingOf(const Device& d, std::string_view name) noexcept { return d.setting(name); }
double settingOr(const Device& d, std::string_view name, double def) noexcept {
  const DeviceValue* v = settingOf(d, name);
  return v != nullptr ? v->value : def;
}

// The value a value block plays undriven: its curve at its knob.
double valueOf(const Device& d, bool units = false) {
  const DeviceValue* v = d.param(kValueParam);
  const double pos = v != nullptr ? v->value : 0.0;
  const dsp::Curve* c = d.curve(kValueParam);
  if (c == nullptr) return pos;
  return units ? dsp::evalCurveUnits(*c, pos) : dsp::evalCurve(*c, pos);
}

struct TrackIndex {
  const Track& t;
  std::unordered_map<uint32_t, const Device*> byId;
  std::unordered_map<uint32_t, std::vector<const Wire*>> into, outOf;  // main input / main output wires per device
  std::unordered_map<uint32_t, std::vector<const Wire*>> from;         // every wire out of a device
  std::unordered_map<uint32_t, std::vector<const Wire*>> inTerm;       // wires out of a group's In terminal
  explicit TrackIndex(const Project& p, const Track& tr) : t(tr) {
    for (const DevicePtr& d : t.devices) byId[d->id.value] = d.get();
    for (const Wire& w : wiresOf(p)) {
      const bool fromHere = w.from.valid() ? byId.count(w.from.value) != 0 : w.noteSource == t.id;
      const bool toHere = w.toMixer() ? w.mixer == t.id : byId.count(w.to.value) != 0;
      if (!fromHere && !toHere) continue;
      if (toHere && !w.toMixer() && w.param == kMainPort) into[w.to.value].push_back(&w);
      if (w.from.valid() && byId.count(w.from.value) != 0) {
        from[w.from.value].push_back(&w);
        if (w.port == kMainPort) {
          const bool toMain = w.toMixer() || w.param == kMainPort ||
                              (w.param == kGroupOutPort && byId.count(w.to.value) && isGroupDevice(*byId[w.to.value]));
          if (toMain) outOf[w.from.value].push_back(&w);
        }
        if (w.port == kGroupInPort && isGroupDevice(*byId[w.from.value])) inTerm[w.from.value].push_back(&w);
      }
    }
  }
  const Device* dev(DeviceId id) const {
    const auto it = byId.find(id.value);
    return it == byId.end() ? nullptr : it->second;
  }
  static const std::vector<const Wire*>& of(const std::unordered_map<uint32_t, std::vector<const Wire*>>& m, DeviceId id) {
    static const std::vector<const Wire*> kNone;
    const auto it = m.find(id.value);
    return it == m.end() ? kNone : it->second;
  }
  // A direct member of `group` named `name` (a group when `group` is true, else a value block).
  const Device* member(DeviceId group, std::string_view name) const {
    for (const DevicePtr& d : t.devices)
      if (d->group == group && d->name == name) return d.get();
    return nullptr;
  }
};

using FeedKey = std::tuple<uint32_t, std::string, uint32_t>;          // from, port, noteSource
using SumKey = std::tuple<uint32_t, std::string, uint32_t>;           // to, param, mixer
using SetKey = std::tuple<uint32_t, FeedKey, SumKey>;                  // parent, feed, sum

FeedKey feedKey(const Wire& w) { return {w.from.value, w.port, w.noteSource.value}; }
SumKey sumKey(const Wire& w) { return {w.to.value, w.param, w.mixer.value}; }

// Whether group `g` qualifies as a chain on its own (one parallel group: it holds a chain mixer).
bool holdsMixer(const TrackIndex& ix, const Device& g) {
  const Device* m = ix.member(g.id, kChainMixerName);
  return m != nullptr && isGroupDevice(*m);
}

// The sets of parallel groups of a track (each sorted by list order), outermost first.
std::vector<std::vector<const Device*>> parallelSets(const TrackIndex& ix) {
  std::map<SetKey, std::vector<const Device*>> sets;
  for (const DevicePtr& d : ix.t.devices) {
    if (!isGroupDevice(*d)) continue;
    const auto& in = TrackIndex::of(ix.into, d->id);
    const auto& out = TrackIndex::of(ix.outOf, d->id);
    if (in.size() != 1 || out.size() != 1) continue;
    if (in[0]->from == d->id || out[0]->to == d->id) continue;
    sets[SetKey{d->group.value, feedKey(*in[0]), sumKey(*out[0])}].push_back(d.get());
  }
  std::vector<std::vector<const Device*>> out;
  for (auto& [key, groups] : sets) {
    if (groups.size() == 1 && !holdsMixer(ix, *groups[0])) continue;
    out.push_back(std::move(groups));
  }
  auto depth = [&](const Device* d) {
    uint32_t n = 0;
    for (const Device* at = d; at != nullptr && at->group.valid() && n <= kMaxGroupDepth; at = ix.dev(at->group)) ++n;
    return n;
  };
  std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) { return depth(a[0]) < depth(b[0]); });
  return out;
}

std::vector<const Device*> seriesOf(const TrackIndex& ix, DeviceId group,
                                    const std::vector<std::vector<const Device*>>& sets) {
  std::vector<const Device*> out;
  std::unordered_set<uint32_t> seen;
  auto setOf = [&](const Device* d) -> const std::vector<const Device*>* {
    for (const auto& s : sets)
      if (std::find(s.begin(), s.end(), d) != s.end()) return &s;
    return nullptr;
  };
  // The main targets of the current point (wires into main inputs of members of `group`, or the Out terminal).
  std::vector<const Wire*> next;
  for (const Wire* w : TrackIndex::of(ix.inTerm, group))
    if (!w->toMixer() && w->param == kMainPort) next.push_back(w);
  for (uint32_t guard = 0; guard < 4096 && !next.empty(); ++guard) {
    const Device* to = nullptr;
    const Wire* pick = nullptr;
    for (const Wire* w : next) {
      const Device* d = ix.dev(w->to);
      if (d == nullptr || d->group != group || seen.count(d->id.value)) continue;
      if (pick == nullptr || w->id < pick->id) pick = w, to = d;
    }
    if (to == nullptr) break;
    const std::vector<const Device*>* s = setOf(to);
    if (s != nullptr && s->size() > 1) {  // a nested rack: its first chain, then on from where it sums
      to = s->front();
      for (const Device* c : *s) seen.insert(c->id.value);
    }
    seen.insert(to->id.value);
    out.push_back(to);
    next.clear();
    for (const Wire* w : TrackIndex::of(ix.outOf, to->id))
      if (!w->toMixer() && w->param == kMainPort) next.push_back(w);
    if (s != nullptr && s->size() > 1 && !next.empty()) {  // the sum point: a block's main input (it is next)
      const Device* sum = ix.dev(next.front()->to);
      next.clear();
      if (sum == nullptr || sum->group != group || seen.count(sum->id.value)) break;
      seen.insert(sum->id.value);
      out.push_back(sum);
      for (const Wire* w : TrackIndex::of(ix.outOf, sum->id))
        if (!w->toMixer() && w->param == kMainPort) next.push_back(w);
    }
  }
  return out;
}

bool isRoleGroup(const Device& d) {
  return isGroupDevice(d) &&
         (d.name == kChainMixerName || d.name == kKeyZoneName || d.name == kVelocityZoneName || d.name == kSelectZoneName);
}

Zone zoneFrom(const TrackIndex& ix, const Device& zg) {
  auto v = [&](std::string_view n) {
    const Device* d = ix.member(zg.id, n);
    return d != nullptr && isValueDevice(*d) ? valueOf(*d) : 0.0;
  };
  return Zone{v(kZoneLoName), v(kZoneFadeLoName), v(kZoneFadeHiName), v(kZoneHiName)};
}

RackInfo makeInfo(const Project& p, const TrackIndex& ix, const std::vector<const Device*>& set,
                  const std::vector<std::vector<const Device*>>& sets) {
  RackInfo r;
  r.track = ix.t.id;
  r.parent = set.front()->group;
  r.id = set.front()->id;
  for (const Device* c : set) r.id = std::min(r.id, c->id);
  {
    const Wire* feed = TrackIndex::of(ix.into, set.front()->id).front();
    r.instrument = feed->fromNoteSource() || isInstrumentGroup(p, set.front()->id);
  }
  r.keyUnit = settingOr(*set.front(), kKeyUnitSetting, 0.0) >= 0.5 ? KeyZoneUnit::Degree : KeyZoneUnit::Hz;
  bool sawVoiceWire = false;
  for (const Device* g : set) {
    RackChain c;
    c.group = g;
    c.id = g->id;
    c.name = g->name;
    c.active = settingOr(*g, kChainOnSetting, 1.0) >= 0.5;
    c.solo = settingOr(*g, kChainSoloSetting, 0.0) >= 0.5;
    if (const Device* m = ix.member(g->id, kChainMixerName); m != nullptr && isGroupDevice(*m)) {
      c.mixer = m->id;
      if (const Device* v = ix.member(m->id, kChainVolumeName)) {
        c.volumeBlock = v->id;
        c.volumeDb = static_cast<float>(valueOf(*v, true));
      }
      if (const Device* v = ix.member(m->id, kChainPanName)) {
        c.panBlock = v->id;
        c.pan = static_cast<float>(valueOf(*v));
      }
      if (const Device* v = ix.member(m->id, kChainGateName)) c.gateBlock = v->id;
    }
    const ZoneKind kinds[3] = {ZoneKind::Key, ZoneKind::Velocity, ZoneKind::Select};
    for (ZoneKind k : kinds) {
      Zone& z = k == ZoneKind::Key ? c.key : k == ZoneKind::Velocity ? c.velocity : c.select;
      DeviceId& zid = k == ZoneKind::Key ? c.keyZone : k == ZoneKind::Velocity ? c.velocityZone : c.selectZone;
      z = fullZone(k, r.keyUnit);
      if (const Device* zg = ix.member(g->id, zoneGroupName(k)); zg != nullptr && isGroupDevice(*zg)) {
        zid = zg->id;
        z = zoneFrom(ix, *zg);
      }
    }
    for (const Device* d : seriesOf(ix, g->id, sets))
      if (!isRoleGroup(*d)) c.devices.push_back(d);
    // An Instrument Rack's chain: the wire out of its per-voice stage (the instrument, then its key / velocity zones).
    if (r.instrument && !sawVoiceWire && !c.devices.empty()) {
      const Device* last = c.devices.front();
      if (c.velocityZone.valid()) last = ix.dev(c.velocityZone);
      else if (c.keyZone.valid()) last = ix.dev(c.keyZone);
      if (last != nullptr)
        for (const Wire* w : TrackIndex::of(ix.outOf, last->id))
          if (!w->toMixer() && (w->param == kMainPort || w->param == kGroupOutPort)) {
            r.voiceChains = !w->single;
            sawVoiceWire = true;
          }
    }
    r.chains.push_back(std::move(c));
  }
  // Macros and the selector: value blocks at the chains' level wired into the rack.
  auto inRack = [&](DeviceId d) {
    for (const Device* c : set)
      if (d == c->id || groupContains(ix.t.devices, c->id, d)) return true;
    return false;
  };
  for (const DevicePtr& d : ix.t.devices) {
    if (!isValueDevice(*d) || d->group != r.parent) continue;
    const auto& ws = TrackIndex::of(ix.from, d->id);
    const bool wired = std::any_of(ws.begin(), ws.end(), [&](const Wire* w) { return !w->toMixer() && inRack(w->to); });
    if (!wired) continue;
    if (d->name == kChainSelectName) {
      r.selector = d->id;
      r.selectorValue = valueOf(*d);
      continue;
    }
    const DeviceValue* slot = d->setting(kMacroSetting);
    if (slot == nullptr) continue;
    const auto k = static_cast<int32_t>(std::lround(slot->value)) - 1;
    if (k < 0 || k >= static_cast<int32_t>(kMacroCount) || r.macros[static_cast<size_t>(k)].block.valid()) continue;
    RackMacro& m = r.macros[static_cast<size_t>(k)];
    m.block = d->id;
    m.name = d->name;
    const DeviceValue* pos = d->param(kValueParam);
    m.value = std::clamp(pos != nullptr ? pos->value : 0.0, 0.0, 1.0) * kMacroMax;
    m.excluded = settingOr(*d, kRandExcludeSetting, 0.0) >= 0.5;
    for (const DeviceValue& s : d->settings)
      if (s.name.size() > kVariationPrefix.size() && s.name.compare(0, kVariationPrefix.size(), kVariationPrefix) == 0) {
        const int n = std::atoi(s.name.c_str() + kVariationPrefix.size());
        if (n > 0) r.variations = std::max(r.variations, static_cast<uint32_t>(n));
      }
    for (const Wire* w : ws) {
      if (w->toMixer() || !w->travels() || !inRack(w->to) && w->to != r.selector) continue;
      RackMapping mp;
      mp.wire = w->id;
      mp.macro = static_cast<uint32_t>(k);
      mp.device = w->to;
      mp.param = w->param;
      const Device* target = ix.dev(w->to);
      const DeviceValue* tp = target != nullptr ? target->param(w->param) : nullptr;
      mp.min = tp != nullptr ? tp->value : 0.0;
      mp.max = mp.min + w->amount;
      r.mappings.push_back(std::move(mp));
    }
  }
  // Mappings onto the selector (a macro wired into it: found above only when the selector came first).
  std::sort(r.mappings.begin(), r.mappings.end(), [](const RackMapping& a, const RackMapping& b) { return a.wire < b.wire; });
  return r;
}

}  // namespace

bool rackValueSetting(std::string_view name) noexcept {
  if (name == kMacroSetting || name == kRandExcludeSetting) return true;
  return name.size() > kVariationPrefix.size() && name.substr(0, kVariationPrefix.size()) == kVariationPrefix;
}
bool rackGroupSetting(std::string_view name) noexcept {
  return name == kChainOnSetting || name == kChainSoloSetting || name == kKeyUnitSetting;
}

ZoneRange zoneRange(ZoneKind kind, KeyZoneUnit unit) noexcept {
  switch (kind) {
    case ZoneKind::Key: return unit == KeyZoneUnit::Hz ? ZoneRange{1.0, 100000.0} : ZoneRange{-1024.0, 1024.0};
    case ZoneKind::Velocity: return {0.0, kVelocityMax};
    case ZoneKind::Select: return {0.0, kSelectMax};
  }
  return {};
}
Zone fullZone(ZoneKind kind, KeyZoneUnit unit) noexcept {
  const ZoneRange r = zoneRange(kind, unit);
  return Zone{r.min, r.min, r.max, r.max};
}
bool zoneOff(const Zone& z, ZoneKind kind, KeyZoneUnit unit) noexcept {
  const ZoneRange r = zoneRange(kind, unit);
  return z.lo <= r.min && z.fadeLo <= r.min && z.fadeHi >= r.max && z.hi >= r.max;
}
bool zoneValid(const Zone& z, ZoneKind kind, KeyZoneUnit unit) noexcept {
  const ZoneRange r = zoneRange(kind, unit);
  for (const double v : {z.lo, z.fadeLo, z.fadeHi, z.hi})
    if (!std::isfinite(v) || v < r.min || v > r.max) return false;
  return z.lo <= z.fadeLo && z.fadeLo <= z.fadeHi && z.fadeHi <= z.hi;
}
std::string_view zoneGroupName(ZoneKind kind) noexcept {
  return kind == ZoneKind::Key ? kKeyZoneName : kind == ZoneKind::Velocity ? kVelocityZoneName : kSelectZoneName;
}

const RackChain* RackInfo::findChain(DeviceId chain) const noexcept {
  for (const RackChain& c : chains)
    if (c.id == chain) return &c;
  return nullptr;
}
int32_t RackInfo::chainIndex(DeviceId chain) const noexcept {
  for (size_t i = 0; i < chains.size(); ++i)
    if (chains[i].id == chain) return static_cast<int32_t>(i);
  return -1;
}
bool RackInfo::holds(const Project& p, DeviceId d) const {
  if (d == selector) return true;
  for (const RackMacro& m : macros)
    if (m.block == d) return true;
  const Track* t = findTrackPtr(p, track);
  for (const RackChain& c : chains)
    if (c.id == d || (t != nullptr && groupContains(t->devices, c.id, d))) return true;
  return false;
}
std::string RackInfo::macroName(uint32_t k) const {
  if (k < kMacroCount && macros[k].block.valid() && !macros[k].name.empty()) return macros[k].name;
  return "Macro " + std::to_string(k + 1);
}

std::vector<RackInfo> findRacks(const Project& p, const Track& t) {
  const TrackIndex ix(p, t);
  const auto sets = parallelSets(ix);
  std::vector<RackInfo> out;
  out.reserve(sets.size());
  for (const auto& s : sets) out.push_back(makeInfo(p, ix, s, sets));
  return out;
}

std::optional<RackInfo> rackOf(const Project& p, DeviceId id) {
  const DeviceLoc loc = findDevice(p, id);
  if (loc.device == nullptr || loc.track == nullptr) return std::nullopt;
  for (RackInfo& r : findRacks(p, *loc.track))
    if (r.findChain(id) != nullptr || r.selector == id ||
        std::any_of(r.macros.begin(), r.macros.end(), [&](const RackMacro& m) { return m.block == id; }))
      return std::move(r);
  return std::nullopt;
}

std::optional<RackInfo> rackOfChain(const Project& p, const Track& t, DeviceId d) {
  for (RackInfo& r : findRacks(p, t))
    if (r.findChain(d) != nullptr) return std::move(r);
  return std::nullopt;
}

std::vector<const Device*> groupSeries(const Project& p, const Track& t, DeviceId group) {
  const TrackIndex ix(p, t);
  return seriesOf(ix, group, parallelSets(ix));
}

// ---- Blocks -----------------------------------------------------------------------------------------------------

namespace {

struct Builder {
  IdSource& ids;
  BlockSet set;
  DeviceId group;
  DeviceId add(std::string patch, std::string name) {
    const DeviceId id = ids.device();
    auto d = std::make_shared<Device>(*makeDevice(id, std::move(patch), DeviceKind::AudioEffect));
    d->name = std::move(name);
    d->group = group;
    set.devices.push_back(std::move(d));
    return id;
  }
  DeviceId value(std::string name, const dsp::Curve& c, double position) {
    const DeviceId id = add(std::string(kValueDevice), std::move(name));
    auto d = std::make_shared<Device>(*set.devices.back());
    d->params = {DeviceValue{std::string(kValueParam), std::clamp(position, 0.0, 1.0)}};
    d->curves = {ControlCurve{std::string(kValueParam), c}};
    set.devices.back() = std::move(d);
    return id;
  }
  DeviceId constant(std::string name, double v) {
    return value(std::move(name), dsp::curvePreset(dsp::CurvePreset::Linear, v, v), 1.0);
  }
  DeviceId atom(std::string_view atomName, std::string name) {
    return add(std::string(kAtomDevicePrefix) + std::string(atomName), std::move(name));
  }
  DeviceId atomWith(std::string_view atomName, std::string name, std::vector<DeviceValue> settings) {
    const DeviceId id = atom(atomName, std::move(name));
    auto d = std::make_shared<Device>(*set.devices.back());
    std::sort(settings.begin(), settings.end(), [](const DeviceValue& a, const DeviceValue& b) { return a.name < b.name; });
    d->settings = std::move(settings);
    set.devices.back() = std::move(d);
    return id;
  }
  void wire(DeviceId from, std::string_view port, DeviceId to, std::string_view param, bool chain = false) {
    Wire w;
    w.id = ids.wire();
    w.from = from;
    w.port = std::string(port);
    w.to = to;
    w.param = std::string(param);
    w.law = WireLaw::Add;
    w.chain = chain;
    set.wires.push_back(std::move(w));
  }
  void out(DeviceId from, DeviceId to, std::string_view param) { wire(from, "out", to, param); }
  void val(DeviceId from, DeviceId to, std::string_view param) { wire(from, kValueOutput, to, param); }
};

DevicePtr groupOf(DeviceId id, DeviceId parent, std::string name) {
  auto g = std::make_shared<Device>(*makeGroup(id));
  g->name = std::move(name);
  g->group = parent;
  return g;
}

void sortWires(BlockSet& s) {
  std::sort(s.wires.begin(), s.wires.end(), [](const Wire& a, const Wire& b) { return a.id < b.id; });
}

}  // namespace

dsp::Curve chainVolumeCurve() {
  dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Linear, kMinVolumeDb, kMaxVolumeDb);
  c.unit = dsp::CurveUnit::Db;
  c.output = dsp::CurveOutput::DbToGain;
  return c;
}
dsp::Curve chainPanCurve() { return dsp::curvePreset(dsp::CurvePreset::Linear, -1.0, 1.0); }
dsp::Curve zoneCurve(ZoneKind kind, KeyZoneUnit unit) {
  const ZoneRange r = zoneRange(kind, unit);
  if (kind == ZoneKind::Key && unit == KeyZoneUnit::Hz) {
    dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Octave, r.min, r.max);
    c.unit = dsp::CurveUnit::Hz;
    return c;
  }
  return dsp::curvePreset(dsp::CurvePreset::Linear, r.min, r.max);
}
dsp::Curve macroCurve() { return dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 1.0); }
dsp::Curve selectorCurve() {
  dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, kSelectMax);
  c.step = 1.0;
  return c;
}

BlockSet chainBlocks(IdSource& ids, DeviceId parent, std::string name, bool instrument) {
  const DeviceId chain = ids.device(), mixer = ids.device();
  BlockSet out;
  out.devices.push_back(groupOf(chain, parent, std::move(name)));
  out.devices.push_back(groupOf(mixer, chain, std::string(kChainMixerName)));
  Builder b{ids, {}, mixer};
  const dsp::Curve vc = chainVolumeCurve();
  const DeviceId vol = b.value(std::string(kChainVolumeName), vc, dsp::curvePosition(vc, 1.0));
  const DeviceId pan = b.value(std::string(kChainPanName), chainPanCurve(), 0.5);
  dsp::Curve gc = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 1.0);
  gc.step = 1.0;
  const DeviceId gate = b.value(std::string(kChainGateName), gc, 1.0);
  const DeviceId one = b.constant("1", 1.0);
  const DeviceId g = b.atom("mul", "volume · gate"), l1 = b.atom("sub", "1 − pan"), r1 = b.atom("add", "1 + pan"),
                 l = b.atom("min", "left"), r = b.atom("min", "right"), gl = b.atom("mul", "gain L"),
                 gr = b.atom("mul", "gain R"), pk = b.atom("pack", "L | R"), y = b.atom("gain", "chain gain");
  b.val(vol, g, "a");
  b.val(gate, g, "b");
  b.val(one, l1, "a");
  b.val(pan, l1, "b");
  b.val(one, r1, "a");
  b.val(pan, r1, "b");
  b.val(one, l, "a");
  b.out(l1, l, "b");
  b.val(one, r, "a");
  b.out(r1, r, "b");
  b.out(g, gl, "a");
  b.out(l, gl, "b");
  b.out(g, gr, "a");
  b.out(r, gr, "b");
  b.out(gl, pk, "in0");
  b.out(gr, pk, "in1");
  b.wire(mixer, kGroupInPort, y, "x", true);
  b.out(pk, y, "g");
  b.wire(y, kMainPort, mixer, kGroupOutPort, true);
  for (DevicePtr& d : b.set.devices) out.devices.push_back(std::move(d));
  out.wires = std::move(b.set.wires);
  // The chain's series: In → Mixer → Out (an Instrument Rack's empty chain: nothing reaches the mixer yet).
  Builder c{ids, {}, chain};
  if (!instrument) c.wire(chain, kGroupInPort, mixer, kMainPort, true);
  c.wire(mixer, kMainPort, chain, kGroupOutPort, true);
  for (Wire& w : c.set.wires) out.wires.push_back(std::move(w));
  sortWires(out);
  return out;
}

BlockSet zoneBlocks(IdSource& ids, DeviceId chain, ZoneKind kind, KeyZoneUnit unit, const Zone& z) {
  const DeviceId zg = ids.device();
  BlockSet out;
  out.devices.push_back(groupOf(zg, chain, std::string(zoneGroupName(kind))));
  Builder b{ids, {}, zg};
  const dsp::Curve c = zoneCurve(kind, unit);
  const DeviceId lo = b.value(std::string(kZoneLoName), c, dsp::curvePosition(c, z.lo)),
                 fl = b.value(std::string(kZoneFadeLoName), c, dsp::curvePosition(c, z.fadeLo)),
                 fh = b.value(std::string(kZoneFadeHiName), c, dsp::curvePosition(c, z.fadeHi)),
                 hi = b.value(std::string(kZoneHiName), c, dsp::curvePosition(c, z.hi));
  const bool logHz = kind == ZoneKind::Key && unit == KeyZoneUnit::Hz;
  const DeviceId eps = b.constant("ε", logHz ? kZoneEpsLog : kZoneEps), zero = b.constant("0", 0.0), one = b.constant("1", 1.0);
  // v and the bounds in v's domain (ln Hz for Hz key zones).
  DeviceId v;
  std::string_view vPort = "out";
  if (kind == ZoneKind::Key) {
    v = b.atom(unit == KeyZoneUnit::Hz ? "voice.noteHz" : "voice.noteDegree", unit == KeyZoneUnit::Hz ? "note Hz" : "note degree");
    if (logHz) {
      const DeviceId ln = b.atom("log", "ln Hz");
      b.out(v, ln, "x");
      v = ln;
    }
  } else if (kind == ZoneKind::Velocity) {
    const DeviceId vel = b.atom("voice.velocity", "velocity"), k = b.constant("127", kVelocityMax);
    v = b.atom("mul", "velocity · 127");
    b.out(vel, v, "a");
    b.val(k, v, "b");
  } else {
    v = b.value(std::string(kZoneValueName), zoneCurve(kind, unit), 0.0);  // the selector adds into it
    vPort = kValueOutput;
  }
  auto bound = [&](DeviceId x, const char* name) -> std::pair<DeviceId, std::string_view> {
    if (!logHz) return {x, kValueOutput};
    const DeviceId ln = b.atom("log", std::string("ln ") + name);
    b.val(x, ln, "x");
    return {ln, "out"};
  };
  const auto [lo2, loP] = bound(lo, "lo");
  const auto [fl2, flP] = bound(fl, "fade lo");
  const auto [fh2, fhP] = bound(fh, "fade hi");
  const auto [hi2, hiP] = bound(hi, "hi");
  const DeviceId d1 = b.atom("sub", "v − lo"), n1 = b.atom("add", "+ ε"), w1 = b.atom("sub", "fade lo − lo"),
                 dn1 = b.atom("add", "+ ε"), rise = b.atom("div", "rise");
  const DeviceId d2 = b.atom("sub", "hi − v"), n2 = b.atom("add", "+ ε"), w2 = b.atom("sub", "hi − fade hi"),
                 dn2 = b.atom("add", "+ ε"), fall = b.atom("div", "fall");
  const DeviceId m = b.atom("min", "min"), g = b.atom("clip", "zone gain");
  b.wire(v, vPort, d1, "a");
  b.wire(lo2, loP, d1, "b");
  b.out(d1, n1, "a");
  b.val(eps, n1, "b");
  b.wire(fl2, flP, w1, "a");
  b.wire(lo2, loP, w1, "b");
  b.out(w1, dn1, "a");
  b.val(eps, dn1, "b");
  b.out(n1, rise, "a");
  b.out(dn1, rise, "b");
  b.wire(hi2, hiP, d2, "a");
  b.wire(v, vPort, d2, "b");
  b.out(d2, n2, "a");
  b.val(eps, n2, "b");
  b.wire(hi2, hiP, w2, "a");
  b.wire(fh2, fhP, w2, "b");
  b.out(w2, dn2, "a");
  b.val(eps, dn2, "b");
  b.out(n2, fall, "a");
  b.out(dn2, fall, "b");
  b.out(rise, m, "a");
  b.out(fall, m, "b");
  b.out(m, g, "x");
  b.val(zero, g, "lo");
  b.val(one, g, "hi");
  // T4.8: a key / velocity zone's gain starts at its value (a note starts at its gain); a select zone fades in.
  const DeviceId y = kind == ZoneKind::Select ? b.atom("gain", "zone")
                                              : b.atomWith("gain", "zone", {DeviceValue{"start", 1.0}});
  b.wire(zg, kGroupInPort, y, "x", true);
  b.out(g, y, "g");
  b.wire(y, kMainPort, zg, kGroupOutPort, true);
  for (DevicePtr& d : b.set.devices) out.devices.push_back(std::move(d));
  out.wires = std::move(b.set.wires);
  sortWires(out);
  return out;
}

}  // namespace pq::doc
