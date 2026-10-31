#include "pq/document/model.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "pq/tuning/mos_tuning.h"

namespace pq::doc {

namespace {

template <class T>
bool deepEq(const std::shared_ptr<const T>& a, const std::shared_ptr<const T>& b) noexcept {
  if (a == b) return true;
  if (!a || !b) return false;
  return *a == *b;
}

template <class T>
bool deepEqList(const std::vector<std::shared_ptr<const T>>& a, const std::vector<std::shared_ptr<const T>>& b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (!deepEq(a[i], b[i])) return false;
  return true;
}

bool regionEq(const std::optional<tuning::RegionHint>& a, const std::optional<tuning::RegionHint>& b) noexcept {
  if (a.has_value() != b.has_value()) return false;
  if (!a) return true;
  return a->perPeriodSize == b->perPeriodSize && a->lowCents == b->lowCents && a->highCents == b->highCents;
}

bool mosEq(const tuning::MosParams& a, const tuning::MosParams& b) noexcept {
  return a.generator == b.generator && a.n == b.n && a.period == b.period &&
         a.periodsPerEquave == b.periodsPerEquave && a.rotation == b.rotation && a.modmos == b.modmos &&
         a.genSequence == b.genSequence && a.octaveStretch == b.octaveStretch && regionEq(a.region, b.region);
}

const TuningDef* defOf(const Project& p, const TuningAssignment& a) noexcept {
  switch (a.mode) {
    case TuningMode::Main: {
      const TuningSlot* s = findTuning(p, a.main);
      return s ? &s->def : nullptr;
    }
    case TuningMode::Private:
      return a.privateDef.get();
    case TuningMode::Inherit:
      break;
  }
  return nullptr;
}

const std::vector<TrackPtr>& listOf(const Project& p, TrackList l) noexcept {
  return l == TrackList::Returns ? p.returns : p.tracks;
}

bool findIn(const std::vector<TrackPtr>& list, TrackId id, TrackPath& path, uint32_t level) noexcept {
  for (uint32_t i = 0; i < list.size(); ++i) {
    const Track& t = *list[i];
    path.index[level] = i;
    if (t.id == id) {
      path.depth = level + 1;
      return true;
    }
    if (!t.children.empty() && level + 1 < kMaxTrackDepth && findIn(t.children, id, path, level + 1)) return true;
  }
  return false;
}

}  // namespace

bool operator==(const TuningDef& a, const TuningDef& b) noexcept {
  if (a.referenceHz != b.referenceHz || a.system.index() != b.system.index()) return false;
  if (const auto* e = a.edo()) return *e == *b.edo();
  if (const auto* m = a.mos()) return mosEq(*m, *b.mos());
  return *a.scale() == *b.scale();
}

Result<tuning::Tuning> buildTuning(const TuningDef& def) {
  if (const tuning::Edo* e = def.edo()) return tuning::Tuning::make(*e, def.referenceHz);
  if (const tuning::MosParams* m = def.mos()) {
    Result<tuning::Mos> mos = tuning::Mos::make(*m);
    if (!mos) return std::move(mos).error();
    return tuning::Tuning::make(std::move(mos).value(), def.referenceHz);
  }
  const ScaleDef& sd = *def.scale();
  if (sd.scale.descriptionUtf8() != sd.scale.description)
    return Error{ErrorCode::InvalidArgument, "scale description must be valid UTF-8", {}};
  if (sd.mapping)
    if (Status k = tuning::validateKbm(*sd.mapping); !k) return std::move(k).error();
  Result<tuning::Scale> scale = tuning::Scale::make(sd.scale);
  if (!scale) return std::move(scale).error();
  return tuning::Tuning::make(std::move(scale).value(), def.referenceHz);
}

Result<TuningDef> makeScaleDef(tuning::ScalaScale scale, std::optional<tuning::KeyboardMapping> mapping,
                               double referenceHz) {
  scale.description = scale.descriptionUtf8();
  TuningDef d;
  d.referenceHz = referenceHz;
  if (mapping) {
    if (Status k = tuning::validateKbm(*mapping); !k) return std::move(k).error();
    Result<tuning::Scale> s = tuning::Scale::make(scale);
    if (!s) return std::move(s).error();
    Result<tuning::Tuning> t = tuning::makeScalaTuning(*s, *mapping);
    if (!t) return std::move(t).error();
    d.referenceHz = t->referenceHz();
  }
  d.system = ScaleDef{std::move(scale), std::move(mapping)};
  if (Result<tuning::Tuning> t = buildTuning(d); !t) return std::move(t).error();
  return d;
}

bool operator==(const TuningSlot& a, const TuningSlot& b) noexcept {
  return a.id == b.id && a.name == b.name && a.colour == b.colour && a.def == b.def;
}

bool operator==(const TuningAssignment& a, const TuningAssignment& b) noexcept {
  if (a.mode != b.mode) return false;
  switch (a.mode) {
    case TuningMode::Inherit: return true;
    case TuningMode::Main: return a.main == b.main;
    case TuningMode::Private: return deepEq(a.privateDef, b.privateDef);
  }
  return false;
}

bool isTrackParam(ParamKind k) noexcept {
  switch (k) {
    case ParamKind::TrackVolume:
    case ParamKind::TrackPan:
    case ParamKind::TrackMute:
    case ParamKind::TrackSolo:
    case ParamKind::TrackSend:
      return true;
    default:
      return false;
  }
}

// T4.1: device params are not automatable yet (their lanes arrive with device automation).
// T4.7: nor are wire amounts.
bool isAutomatable(ParamKind k) noexcept {
  return k != ParamKind::TrackSolo && k != ParamKind::DeviceParam && k != ParamKind::WireAmount &&
         k != ParamKind::ChainMixer;
}

uint32_t deviceParamHash(std::string_view name) noexcept {
  uint32_t h = 2166136261u;
  for (const char c : name) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
  return h;
}

ParamRef ParamRef::device(DeviceId d, std::string_view param) {
  return {ParamKind::DeviceParam, d.value, deviceParamHash(param)};
}

namespace {
const DeviceValue* findValue(const std::vector<DeviceValue>& v, std::string_view name) noexcept {
  const auto it = std::lower_bound(v.begin(), v.end(), name,
                                   [](const DeviceValue& a, std::string_view n) { return a.name < n; });
  return it != v.end() && it->name == name ? &*it : nullptr;
}
}  // namespace

const DeviceValue* Device::param(std::string_view name) const noexcept { return findValue(params, name); }
const DeviceValue* Device::setting(std::string_view name) const noexcept { return findValue(settings, name); }

DevicePtr makeDevice(DeviceId id, std::string patch, DeviceKind kind) {
  auto d = std::make_shared<Device>();
  d->id = id;
  d->patch = std::move(patch);
  d->kind = kind;
  return d;
}

DevicePtr makeDefaultInstrument(DeviceId id) {
  return makeDevice(id, std::string(kDefaultInstrument), DeviceKind::Instrument);
}

// ---- Racks (T4.8) ----------------------------------------------------------------------------------------------

std::string macroParamName(uint32_t index) { return "macro" + std::to_string(index + 1); }

std::optional<uint32_t> macroIndex(std::string_view param) noexcept {
  if (!param.starts_with("macro") || param.size() < 6 || param.size() > 7) return std::nullopt;
  uint32_t v = 0;
  for (const char c : param.substr(5)) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + static_cast<uint32_t>(c - '0');
  }
  if (param[5] == '0' || v < 1 || v > kMacroCount) return std::nullopt;
  return v - 1;
}

bool isRackParam(std::string_view param) noexcept { return param == kSelectParam || macroIndex(param).has_value(); }

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

const Chain* Rack::findChain(ChainId id) const noexcept {
  for (const ChainPtr& c : chains)
    if (c->id == id) return c.get();
  return nullptr;
}

bool operator==(const Chain& a, const Chain& b) noexcept {
  return a.id == b.id && a.name == b.name && a.volumeDb == b.volumeDb && a.pan == b.pan && a.active == b.active &&
         a.solo == b.solo && a.key == b.key && a.velocity == b.velocity && a.select == b.select &&
         deepEqList(a.devices, b.devices);
}

bool operator==(const Rack& a, const Rack& b) noexcept {
  return deepEqList(a.chains, b.chains) && a.mappings == b.mappings && a.macroNames == b.macroNames &&
         a.visibleMacros == b.visibleMacros && a.variations == b.variations && a.randomizeExclude == b.randomizeExclude &&
         a.keyUnit == b.keyUnit && a.voiceChains == b.voiceChains && a.showChains == b.showChains &&
         a.showMacros == b.showMacros;
}

bool operator==(const Device& a, const Device& b) noexcept {
  return a.id == b.id && a.patch == b.patch && a.kind == b.kind && a.active == b.active && a.params == b.params &&
         a.settings == b.settings && deepEq(a.source, b.source) && deepEq(a.rack, b.rack);
}

DevicePtr makeRack(DeviceId id, DeviceKind kind, std::vector<ChainPtr> chains) {
  auto d = std::make_shared<Device>();
  d->id = id;
  d->patch = std::string(kind == DeviceKind::Instrument ? kInstrumentRack : kAudioEffectRack);
  d->kind = kind;
  auto r = std::make_shared<Rack>();
  r->chains = std::move(chains);
  d->rack = std::move(r);
  return d;
}

ChainPtr makeChain(ChainId id, std::string name, std::vector<DevicePtr> devices) {
  auto c = std::make_shared<Chain>();
  c->id = id;
  c->name = std::move(name);
  c->devices = std::move(devices);
  c->key = fullZone(ZoneKind::Key, KeyZoneUnit::Hz);
  c->velocity = fullZone(ZoneKind::Velocity, KeyZoneUnit::Hz);
  c->select = fullZone(ZoneKind::Select, KeyZoneUnit::Hz);
  return c;
}

std::string macroDisplayName(const Rack& r, uint32_t index) {
  if (index < kMacroCount && !r.macroNames[index].empty()) return r.macroNames[index];
  return "Macro " + std::to_string(index + 1);
}

double rackParamValue(const Device& rack, std::string_view param) noexcept {
  const DeviceValue* v = rack.param(param);
  return v != nullptr ? v->value : 0.0;
}

const std::vector<DevicePtr>& DeviceLoc::list() const noexcept {
  static const std::vector<DevicePtr> kNone;
  if (chain != nullptr) return chain->devices;
  return track != nullptr ? track->devices : kNone;
}

bool deviceContains(const Device& outer, DeviceId inner) noexcept {
  bool found = false;
  forEachDeviceIn(outer, [&](const Device& d) { found = found || d.id == inner; });
  return found;
}

int32_t Track::deviceIndex(DeviceId device) const noexcept {
  for (size_t i = 0; i < devices.size(); ++i)
    if (devices[i]->id == device) return static_cast<int32_t>(i);
  return -1;
}

ParamRange paramRange(ParamKind k) noexcept {
  switch (k) {
    case ParamKind::TrackVolume:
    case ParamKind::TrackSend:
      return {kMinVolumeDb, kMaxVolumeDb};
    case ParamKind::TrackPan: return {-1.0, 1.0};
    case ParamKind::TrackMute:
    case ParamKind::TrackSolo:
      return {0.0, 1.0};
    case ParamKind::TuningReference: return {1.0, 20000.0};
    case ParamKind::TuningEquave: return {1.0, 12000.0};
    case ParamKind::MosGenerator: return {-12000.0, 12000.0};
    case ParamKind::MosPeriod: return {1.0, 12000.0};
    case ParamKind::MosStretch: return {-1199.0, 1200.0};
    case ParamKind::DeviceParam: return {-1e30, 1e30};  // the device patch's own range applies (DeviceCatalog)
    case ParamKind::WireAmount: return {-1.0, 1.0};
    case ParamKind::ChainMixer: return {-1e30, 1e30};  // a chain's mixer / zones (each value its own range)
  }
  return {};
}

bool operator==(const AutomationLane& a, const AutomationLane& b) noexcept {
  return a.id == b.id && a.param == b.param && a.points == b.points;
}

bool operator==(const NoteClip& a, const NoteClip& b) noexcept {
  return a.id == b.id && a.name == b.name && a.colour == b.colour && a.start == b.start && a.length == b.length &&
         a.contentStart == b.contentStart && a.loop == b.loop && a.signature == b.signature &&
         a.tuning == b.tuning && a.active == b.active && a.fundamental == b.fundamental && a.notes == b.notes;
}

NoteClip withNotes(const NoteClip& c, NoteSeq notes) {
  NoteClip n;
  n.id = c.id;
  n.name = c.name;
  n.colour = c.colour;
  n.start = c.start;
  n.length = c.length;
  n.contentStart = c.contentStart;
  n.loop = c.loop;
  n.signature = c.signature;
  n.tuning = c.tuning;
  n.active = c.active;
  n.fundamental = c.fundamental;
  n.notes = std::move(notes);
  return n;
}

int64_t lcmCapped(int64_t a, int64_t b, int64_t cap) noexcept {  // RT-safe
  if (a < 1 || b < 1) return -1;
  const int64_t x = a / std::gcd(a, b);  // lcm = x · b
  return x > cap / b ? -1 : x * b;
}

std::optional<tuning::Rational> virtualFundamental(const NoteClip& c, int64_t own) {
  if (c.fundamental.lock) return c.fundamental.lock;
  auto isRefRatio = [](const Note& n) {
    return n.pitch.kind() == tuning::PitchKind::Ratio && n.pitch.anchorKind() == tuning::AnchorKind::Reference;
  };
  int64_t D = 0;  // −1: too fine
  auto join = [&D](int64_t d) {
    if (D < 0 || d == 0) return;
    D = d < 0 ? -1 : (D ? lcmCapped(D, d, kMaxFundamentalDenominator) : d);
    if (D > kMaxFundamentalDenominator) D = -1;
  };
  const std::vector<HarmonicStep>& steps = c.fundamental.steps;
  if (own == kComputeOwnDenominators || !steps.empty()) {
    // One pass: own denominators (unless given) and the steps still in force.
    if (own != kComputeOwnDenominators) join(own);
    for (const Note& n : c.notes) {
      if (!isRefRatio(n)) continue;
      const tuning::Rational r = n.pitch.ratio();
      if (own == kComputeOwnDenominators) join(r.den());
      if (steps.empty()) continue;
      const auto it = std::lower_bound(steps.begin(), steps.end(), n.id,
                                       [](const HarmonicStep& s, NoteId id) { return s.note.value < id.value; });
      if (it != steps.end() && it->note == n.id && it->ratio == r) join(it->denominator);
    }
  } else {
    join(own);
  }
  if (D <= 0) return std::nullopt;
  return tuning::Rational(1, D);
}

bool operator==(const Track& a, const Track& b) noexcept {
  if (a.id != b.id || a.kind != b.kind || a.name != b.name || a.colour != b.colour || !(a.tuning == b.tuning) ||
      !(a.mixer == b.mixer) || a.active != b.active)
    return false;
  if (!deepEqList(a.children, b.children) || !deepEqList(a.lanes, b.lanes) || !deepEqList(a.devices, b.devices))
    return false;
  if (a.clips.size() != b.clips.size()) return false;
  for (size_t i = 0; i < a.clips.size(); ++i)
    if (a.clips[i].id != b.clips[i].id || !deepEq(a.clips[i].clip, b.clips[i].clip) ||
        !deepEq(a.clips[i].audio, b.clips[i].audio))
      return false;
  return true;
}

Track makeTrack(TrackId id, TrackKind kind, std::string name) {
  Track t;
  t.id = id;
  t.kind = kind;
  t.name = std::move(name);
  return t;
}

bool operator==(const Timeline& a, const Timeline& b) noexcept {
  if (a.tempo.size() != b.tempo.size() || a.signatures.size() != b.signatures.size()) return false;
  for (size_t i = 0; i < a.tempo.size(); ++i)
    if (a.tempo[i].tick != b.tempo[i].tick || a.tempo[i].bpm != b.tempo[i].bpm || a.tempo[i].curve != b.tempo[i].curve)
      return false;
  for (size_t i = 0; i < a.signatures.size(); ++i)
    if (a.signatures[i].bar != b.signatures[i].bar || !(a.signatures[i].sig == b.signatures[i].sig)) return false;
  return true;  // the maps are functions of the points
}

bool operator==(const Project& a, const Project& b) noexcept {
  if (&a == &b) return true;
  return deepEq(a.timeline, b.timeline) && deepEq(a.locators, b.locators) && a.loop == b.loop &&
         (a.tunings == b.tunings || (a.tunings && b.tunings && deepEqList(*a.tunings, *b.tunings))) &&
         deepEqList(a.tracks, b.tracks) && deepEqList(a.returns, b.returns) && deepEq(a.master, b.master) &&
         (a.wires == b.wires || wiresOf(a) == wiresOf(b)) && (a.samples == b.samples || samplesOf(a) == samplesOf(b));
}

const SampleList& samplesOf(const Project& p) noexcept {
  static const SampleList kNone;
  return p.samples ? *p.samples : kNone;
}

const SampleRef* findSample(const Project& p, SampleId id) noexcept {
  const SampleList& v = samplesOf(p);
  const auto it = std::lower_bound(v.begin(), v.end(), id, [](const SampleRef& a, SampleId b) { return a.id < b; });
  return it != v.end() && it->id == id ? &*it : nullptr;
}

const WireList& wiresOf(const Project& p) noexcept {
  static const WireList kNone;
  return p.wires ? *p.wires : kNone;
}

const Wire* findWire(const Project& p, WireId id) noexcept {
  const WireList& w = wiresOf(p);
  const auto it = std::lower_bound(w.begin(), w.end(), id, [](const Wire& a, WireId b) { return a.id < b; });
  return it != w.end() && it->id == id ? &*it : nullptr;
}

Snapshot makeDefaultProject(IdSource& ids) {
  auto p = std::make_shared<Project>();
  p->timeline = std::make_shared<const Timeline>();
  TuningSlot slot;
  slot.id = ids.tuning();
  slot.name = "12-EDO";
  slot.colour = Colour{0x5E9BD6};
  p->tunings = std::make_shared<const TuningList>(TuningList{std::make_shared<const TuningSlot>(std::move(slot))});
  p->locators = std::make_shared<const LocatorList>();
  p->master = std::make_shared<const Track>(makeTrack(ids.track(), TrackKind::Master, "Master"));
  return p;
}

uint32_t maxEntityId(const Project& p) {
  uint32_t m = 0;
  auto see = [&m](uint32_t v) { m = std::max(m, v); };
  for (const TuningSlotPtr& s : *p.tunings) see(s->id.value);
  for (const Locator& l : *p.locators) see(l.id.value);
  for (const Wire& w : wiresOf(p)) see(w.id.value);
  for (const SampleRef& r : samplesOf(p)) see(r.id.value);
  forEachTrack(p, [&](const Track& t, const Track*) {
    see(t.id.value);
    forEachDeviceIn(t.devices, [&](const Device& d) {
      see(d.id.value);
      if (!d.rack) return;  // T4.8: chains and mappings
      for (const ChainPtr& c : d.rack->chains) see(c->id.value);
      for (const MacroMapping& m : d.rack->mappings) see(m.id.value);
    });
    for (const LanePtr& l : t.lanes) {
      see(l->id.value);
      for (const Breakpoint& b : l->points) see(b.id.value);
    }
    for (const ClipSlot& c : t.clips) {
      see(c.id.value);
      if (c.clip)
        for (const Note& n : c.clip->notes) see(n.id.value);
    }
  });
  return m;
}

std::optional<TrackPath> findTrack(const Project& p, TrackId id) noexcept {
  TrackPath path;
  if (p.master && p.master->id == id) {
    path.list = TrackList::Master;
    path.depth = 1;
    return path;
  }
  path.list = TrackList::Main;
  if (findIn(p.tracks, id, path, 0)) return path;
  path.list = TrackList::Returns;
  if (findIn(p.returns, id, path, 0)) return path;
  return std::nullopt;
}

const Track& trackAt(const Project& p, const TrackPath& path) noexcept {
  if (path.list == TrackList::Master) return *p.master;
  const Track* t = listOf(p, path.list)[path.index[0]].get();
  for (uint32_t l = 1; l < path.depth; ++l) t = t->children[path.index[l]].get();
  return *t;
}

const Track* findTrackPtr(const Project& p, TrackId id) noexcept {
  const auto path = findTrack(p, id);
  return path ? &trackAt(p, *path) : nullptr;
}

const Track* parentOf(const Project& p, const TrackPath& path) noexcept {
  if (path.list != TrackList::Main || path.depth <= 1) return nullptr;
  return &trackAt(p, path.parent());
}

const TuningSlot* findTuning(const Project& p, TuningId id) noexcept {
  for (const TuningSlotPtr& s : *p.tunings)
    if (s->id == id) return s.get();
  return nullptr;
}

int32_t findClipIndex(const Track& t, ClipId id) noexcept {
  for (size_t i = 0; i < t.clips.size(); ++i)
    if (t.clips[i].id == id) return static_cast<int32_t>(i);
  return -1;
}

namespace {
// Searches `list` (held by `chain` of `rack`, or the track's own chain) and the racks in it.
bool findIn(const std::vector<DevicePtr>& list, DeviceId id, const Chain* chain, const Device* rack, DeviceLoc& out) noexcept {
  for (size_t i = 0; i < list.size(); ++i) {
    const Device& d = *list[i];
    if (d.id == id) {
      out.device = &d;
      out.index = static_cast<uint32_t>(i);
      out.chain = chain;
      out.rack = rack;
      return true;
    }
    if (d.rack)
      for (const ChainPtr& c : d.rack->chains)
        if (findIn(c->devices, id, c.get(), &d, out)) return true;
  }
  return false;
}
}  // namespace

DeviceLoc findDevice(const Project& p, DeviceId id) noexcept {
  DeviceLoc out;
  if (!id.valid()) return out;
  forEachTrack(p, [&](const Track& t, const Track*) {
    if (out.track) return;
    if (findIn(t.devices, id, nullptr, nullptr, out)) out.track = &t;
  });
  return out;
}

ChainLoc findChain(const Project& p, ChainId id) noexcept {
  ChainLoc out;
  if (!id.valid()) return out;
  forEachDevice(p, [&](const Device& d, const Track& t) {
    if (out.chain || !d.rack) return;
    for (size_t i = 0; i < d.rack->chains.size(); ++i)
      if (d.rack->chains[i]->id == id) out = ChainLoc{&t, &d, d.rack->chains[i].get(), static_cast<uint32_t>(i)};
  });
  return out;
}

MappingLoc findMapping(const Project& p, MappingId id) noexcept {
  MappingLoc out;
  if (!id.valid()) return out;
  forEachDevice(p, [&](const Device& d, const Track& t) {
    if (out.mapping || !d.rack) return;
    for (const MacroMapping& m : d.rack->mappings)
      if (m.id == id) out = MappingLoc{&t, &d, &m};
  });
  return out;
}

MappingLoc mappingOf(const Project& p, DeviceId device, std::string_view param) noexcept {
  MappingLoc out;
  forEachDevice(p, [&](const Device& d, const Track& t) {
    if (out.mapping || !d.rack) return;
    for (const MacroMapping& m : d.rack->mappings)
      if (m.device == device && m.param == param) out = MappingLoc{&t, &d, &m};
  });
  return out;
}

const TuningDef* effectiveTuning(const Project& p, const TrackPath& path, const NoteClip* clip) noexcept {
  if (clip && clip->tuning.mode != TuningMode::Inherit) return defOf(p, clip->tuning);
  TrackPath at = path;
  while (at.depth > 0) {
    const Track& t = trackAt(p, at);
    if (t.tuning.mode != TuningMode::Inherit) return defOf(p, t.tuning);
    if (at.list != TrackList::Main) break;
    at = at.parent();
  }
  return p.tunings->empty() ? nullptr : &p.tunings->front()->def;
}

}  // namespace pq::doc
