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
    case ParamKind::TrackArm:      // T6.1
    case ParamKind::TrackMonitor:  // T6.1
      return true;
    default:
      return false;
  }
}

// T4.18: device params and wire amounts are automatable too (lanes on the device's track).
bool isAutomatable(ParamKind k) noexcept {
  return k != ParamKind::TrackSolo && k != ParamKind::MetaParam && k != ParamKind::TrackArm &&  // TMM.3, T6.1
         k != ParamKind::TrackMonitor;
}  // TMM.3: not MetaParam (MM)

bool isAutomatable(const ParamRef& r) noexcept {
  if (!isAutomatable(r.kind)) return false;
  return true;
}

bool isDeviceParamKind(ParamKind k) noexcept {
  return k == ParamKind::DeviceParam || k == ParamKind::WireAmount;
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

const DeviceValue* Device::param(std::string_view key) const noexcept { return findValue(params, key); }
const DeviceValue* Device::setting(std::string_view key) const noexcept { return findValue(settings, key); }
const dsp::Curve* Device::curve(std::string_view param) const noexcept {  // GU.1
  const auto it = std::lower_bound(curves.begin(), curves.end(), param,
                                   [](const ControlCurve& a, std::string_view n) { return a.param < n; });
  return it != curves.end() && it->param == param ? &it->curve : nullptr;
}

// ---- Meta modules (TMM.3) --------------------------------------------------------------------------------------------
const DeviceValue* MetaInstance::param(std::string_view key) const noexcept { return findValue(params, key); }
const DeviceValue* MetaInstance::setting(std::string_view key) const noexcept { return findValue(settings, key); }

bool operator==(const MetaInstance& a, const MetaInstance& b) noexcept {
  return a.type == b.type && a.version == b.version && a.params == b.params && a.settings == b.settings &&
         meta::dataEqual(a.data, b.data);
}

const MetaInstance* Device::metaAt(std::string_view path) const noexcept {
  if (!meta) return nullptr;
  const auto it = std::lower_bound(meta->begin(), meta->end(), path,
                                   [](const MetaEntry& e, std::string_view p) { return e.path < p; });
  return it != meta->end() && it->path == path ? &it->instance : nullptr;
}

// ---- TX.pan-laws ----

std::string_view panModeName(PanMode m) noexcept {
  switch (m) {
    case PanMode::Balance: return "Balance";
    case PanMode::ConstantPower: return "Constant Power (-3 dB)";
    case PanMode::Linear: return "Linear (-6 dB)";
    case PanMode::Stereo: return "True Stereo";
  }
  return "Balance";
}

std::string_view panModeShort(PanMode m) noexcept {
  switch (m) {
    case PanMode::Balance: return "BAL";
    case PanMode::ConstantPower: return "-3 dB";
    case PanMode::Linear: return "-6 dB";
    case PanMode::Stereo: return "STEREO";
  }
  return "BAL";
}

std::string_view panModeKey(PanMode m) noexcept {
  switch (m) {
    case PanMode::Balance: return "balance";
    case PanMode::ConstantPower: return "constantPower";
    case PanMode::Linear: return "linear";
    case PanMode::Stereo: return "stereo";
  }
  return "balance";
}

std::optional<PanMode> panModeFromKey(std::string_view key) noexcept {
  for (const PanMode m : kPanModes)
    if (panModeKey(m) == key) return m;
  return std::nullopt;
}

PanMatrix panMatrix(PanMode m, double p) noexcept {
  p = std::clamp(std::isnan(p) ? 0.0 : p, -1.0, 1.0);
  constexpr double kQuarterPi = 0.78539816339744830962, kHalfPi = 1.57079632679489661923;
  PanMatrix g;
  switch (m) {
    case PanMode::Balance:
      g.ll = std::min(1.0, 1.0 - p);
      g.rr = std::min(1.0, 1.0 + p);
      break;
    case PanMode::ConstantPower:
      g.ll = std::cos((p + 1.0) * kQuarterPi);
      g.rr = std::sin((p + 1.0) * kQuarterPi);
      break;
    case PanMode::Linear:
      g.ll = 0.5 * (1.0 - p);
      g.rr = 0.5 * (1.0 + p);
      break;
    case PanMode::Stereo: {
      const double a = std::max(p, 0.0) * kHalfPi, b = std::max(-p, 0.0) * kHalfPi;
      g.ll = std::cos(a);  // the left input: what stays left
      g.lr = std::sin(a);  //                 what moves right
      g.rr = std::cos(b);
      g.rl = std::sin(b);
      break;
    }
  }
  return g;
}

bool isMetaDevice(const Device& d) noexcept { return d.patch.starts_with(kMetaDevicePrefix); }

std::string_view metaDeviceType(const Device& d) noexcept {
  return isMetaDevice(d) ? std::string_view(d.patch).substr(kMetaDevicePrefix.size()) : std::string_view();
}

bool isGroupDevice(const Device& d) noexcept { return d.patch == kGroupDevice; }

bool isAtomDevice(const Device& d) noexcept {
  return d.patch.size() > kAtomDevicePrefix.size() && d.patch.starts_with(kAtomDevicePrefix);
}
std::string_view atomDeviceName(const Device& d) noexcept {
  return isAtomDevice(d) ? std::string_view(d.patch).substr(kAtomDevicePrefix.size()) : std::string_view();
}
bool isValueDevice(const Device& d) noexcept { return d.patch == kValueDevice; }

std::string_view valuePatchText() noexcept {
  // GU.2: one control as a signal. The range and law are the control's curve (Device::curves; declared: linear 0 … 1).
  // GU.4: the param declares no range (its declared curve is still linear 0 … 1: graph::declaredCurve's rule for an
  // unranged param), because the engine clamps a runtime param to its declared range: a value block whose curve runs
  // 0 … 20 kHz (a Phase Accumulator's frequency) or ±20000 (a VCA's) writes values its curve gives, beyond [0, 1].
  return "patch Value {\n"
         "  param value = 0\n"
         "  out   y     : real\n"
         "  y = value\n"
         "}\n";
}

std::string_view groupPatchText() noexcept {
  // TX.groups: the In terminal is an output (`input` = x), the Out terminal an input (`output` → y). y is declared stereo
  // (an upper bound: a mono sum stays mono), so the chain after a group never packs an already stereo signal.
  return "patch Group {\n"
         "  in    x      : audio global\n"
         "  in    output : audio global\n"
         "  out   y      : audio global stereo\n"
         "  out   input  : audio global\n"
         "  y = output\n"
         "  input = x\n"
         "}\n";
}

std::string_view groupVoicePatchText() noexcept {
  // TX.groups-fix1: Group with no `global` bound — a per-voice signal passes both terminals per voice.
  return "patch GroupVoice {\n"
         "  in    x      : audio\n"
         "  in    output : audio\n"
         "  out   y      : audio stereo\n"
         "  out   input  : audio\n"
         "  y = output\n"
         "  input = x\n"
         "}\n";
}

std::string_view groupNotesPatchText() noexcept {
  // TX.groups-fix1: at the note source the In terminal carries the note, read by the meaning of the input it reaches
  // (meta::Reads, as a chain input there reads it): `input` the frequency (voice.hz: inside a subcircuit the tuned
  // pitch), `phase` the phase (phasor(voice.hz), 0 at note-on). An unread output costs nothing (the compiler drops it).
  return "patch GroupNotes {\n"
         "  in    output : audio\n"
         "  out   y      : audio stereo\n"
         "  out   input  : audio mono\n"
         "  out   phase  : audio mono\n"
         "  y = output\n"
         "  input = voice.hz\n"
         "  phase = phasor(voice.hz)\n"
         "}\n";
}

std::vector<const Device*> groupMembers(const std::vector<DevicePtr>& list, DeviceId group) {
  std::vector<const Device*> out;
  if (!group.valid()) return out;
  for (const DevicePtr& d : list)
    if (d && d->group == group) out.push_back(d.get());
  return out;
}

namespace {
const Device* inList(const std::vector<DevicePtr>& list, DeviceId id) noexcept {
  for (const DevicePtr& d : list)
    if (d && d->id == id) return d.get();
  return nullptr;
}
}  // namespace

bool groupContains(const std::vector<DevicePtr>& list, DeviceId group, DeviceId d) noexcept {
  if (!group.valid()) return false;
  const Device* at = inList(list, d);
  for (uint32_t step = 0; at != nullptr && at->group.valid() && step < kMaxGroupDepth; ++step) {
    if (at->group == group) return true;
    at = inList(list, at->group);
  }
  return false;
}

std::vector<const Device*> groupMembersDeep(const std::vector<DevicePtr>& list, DeviceId group) {
  std::vector<const Device*> out;
  if (!group.valid()) return out;
  for (const DevicePtr& d : list)
    if (d && d->group.valid() && groupContains(list, group, d->id)) out.push_back(d.get());
  return out;
}

DeviceId outermostGroup(const std::vector<DevicePtr>& list, DeviceId d) noexcept {
  DeviceId out;
  const Device* at = inList(list, d);
  for (uint32_t step = 0; at != nullptr && at->group.valid() && step < kMaxGroupDepth; ++step) {
    out = at->group;
    at = inList(list, at->group);
  }
  return out;
}

WireContainer wireContainer(const Project& p, const Wire& w) noexcept {
  WireContainer out;
  if (w.toMixer()) {
    out.track = w.mixer;
    return out;
  }
  const DeviceLoc to = findDevice(p, w.to);
  if (to.track == nullptr) return out;
  out.track = to.track->id;
  if (w.fromNoteSource()) return out;
  const DeviceLoc from = findDevice(p, w.from);
  if (from.track != to.track) return out;
  auto endGroup = [](const Device& d, bool terminal) { return terminal && isGroupDevice(d) ? d.id : d.group; };
  const DeviceId target = endGroup(*to.device, w.param == kGroupOutPort);
  if (!target.valid()) return out;
  // The innermost group around the source's end that holds the target's end too (the walk outward is bounded as
  // outermostGroup's is).
  const std::vector<DevicePtr>& list = to.track->devices;
  DeviceId g = endGroup(*from.device, w.port == kGroupInPort);
  for (uint32_t step = 0; g.valid() && step <= kMaxGroupDepth; ++step) {
    if (g == target || groupContains(list, g, target)) {
      out.group = g;
      return out;
    }
    const Device* at = inList(list, g);
    if (at == nullptr) break;
    g = at->group;
  }
  return out;
}

bool normaliseMetaKinds(std::vector<DevicePtr>& list, bool instrumentsGo, std::vector<DeviceId>* changed) {
  bool any = false;
  const size_t first = 0;  // (a rack chain: TX.all-free leaves a track's blocks' kinds alone)
  for (size_t i = 0; i < list.size(); ++i) {
    if (!list[i] || !isMetaDevice(*list[i])) continue;
    const DeviceKind want = i == first && instrumentsGo ? DeviceKind::Instrument : DeviceKind::AudioEffect;
    if (list[i]->kind == want) continue;
    auto c = std::make_shared<Device>(*list[i]);
    c->kind = want;
    if (changed != nullptr) changed->push_back(c->id);
    list[i] = std::move(c);
    any = true;
  }
  return any;
}

ParamRef ParamRef::metaParam(DeviceId d, std::string_view path, std::string_view param) {
  std::string key(path);
  key += '.';
  key += param;
  return {ParamKind::MetaParam, d.value, deviceParamHash(key)};
}

DevicePtr makeDevice(DeviceId id, std::string patch, DeviceKind kind) {
  auto d = std::make_shared<Device>();
  d->id = id;
  d->patch = std::move(patch);
  d->kind = kind;
  return d;
}


DevicePtr makeGroup(DeviceId id) { return makeDevice(id, std::string(kGroupDevice), DeviceKind::AudioEffect); }

GroupPolyphony groupPolyphony(const Device& g) noexcept {
  GroupPolyphony p;
  auto val = [&](std::string_view n) -> const DeviceValue* { return g.setting(n); };
  if (const DeviceValue* v = val(kGroupVoices); v != nullptr && std::isfinite(v->value))
    p.voices = static_cast<uint32_t>(std::clamp(std::round(v->value), 1.0, 128.0));
  if (const DeviceValue* v = val(kGroupMono)) p.mono = v->value >= 0.5;
  if (const DeviceValue* v = val(kGroupLegato)) p.legato = v->value >= 0.5;
  if (const DeviceValue* v = val(kGroupGlide); v != nullptr && std::isfinite(v->value)) p.glide = std::max(v->value, 0.0);
  if (const DeviceValue* v = val(kGroupSteal); v != nullptr && std::isfinite(v->value))
    p.steal = static_cast<GroupSteal>(std::clamp(static_cast<int>(std::lround(v->value)), 0, 2));
  return p;
}

uint32_t groupOversampleFactor(const Device& g) noexcept {
  if (!isGroupDevice(g)) return 1;
  const DeviceValue* v = g.setting(kGroupOversample);
  if (v == nullptr || !std::isfinite(v->value)) return 1;
  return 1u << static_cast<uint32_t>(std::clamp(std::lround(v->value), 0l, static_cast<long>(kMaxGroupOversample)));
}

double groupOversampleLatency(uint32_t factor) noexcept {
  // pq/dsp/oversample.h (tests/dsp/test_oversample.cpp measures these): up + down at DC, base-rate samples.
  switch (factor) {
    case 2: return 3.62;
    case 4: return 4.74;
    case 8: return 5.17;
    case 16: return 5.31;
    default: return 0.0;
  }
}

DevicePtr makeInstrumentGroup(DeviceId id) {
  auto d = std::make_shared<Device>(*makeGroup(id));
  // (sorted by name: Device::settings)
  d->settings = {DeviceValue{std::string(kGroupGlide), 0.0}, DeviceValue{std::string(kGroupSteal), 0.0},
                 DeviceValue{std::string(kGroupVoices), static_cast<double>(kDefaultGroupVoices)}};
  return d;
}

namespace {

bool shallowDeviceEq(const Device& a, const Device& b) noexcept {
  return a.id == b.id && a.patch == b.patch && a.kind == b.kind && a.active == b.active && a.params == b.params &&
         a.settings == b.settings && a.name == b.name &&
         deepEq(a.meta, b.meta) &&  // TMM.3 (an empty map is always null: the commands keep it so)
         a.group == b.group &&            // TX.groups
         a.curves == b.curves &&          // GU.1
         deepEq(a.panel, b.panel) &&      // GU.3
         a.defaultPosition == b.defaultPosition;  // TX.reset-default
}

}  // namespace

bool operator==(const Device& a, const Device& b) noexcept { return shallowDeviceEq(a, b); }

const std::vector<DevicePtr>& DeviceLoc::list() const noexcept {
  static const std::vector<DevicePtr> kNone;
  return track != nullptr ? track->devices : kNone;
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
    case ParamKind::MetaParam: return {-1e30, 1e30};  // TMM.3: the meta type's own range applies
    case ParamKind::TrackArm: return {0.0, 1.0};       // T6.1
    case ParamKind::TrackMonitor: return {0.0, 2.0};   // T6.1: MonitorMode
  }
  return {};
}

ParamRange paramRange(const ParamRef& r) noexcept {
  return paramRange(r.kind);
}

bool operator==(const AutomationLane& a, const AutomationLane& b) noexcept {
  return a.id == b.id && a.param == b.param && a.points == b.points && a.paramName == b.paramName;
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

namespace {
bool sameSlots(const std::vector<ClipSlot>& a, const std::vector<ClipSlot>& b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].id != b[i].id || a[i].take != b[i].take || !deepEq(a[i].clip, b[i].clip) || !deepEq(a[i].audio, b[i].audio))
      return false;
  return true;
}
}  // namespace

bool operator==(const TakeLane& a, const TakeLane& b) noexcept {
  return a.id == b.id && a.name == b.name && sameSlots(a.clips, b.clips);
}

bool operator==(const Track& a, const Track& b) noexcept {
  if (a.id != b.id || a.kind != b.kind || a.name != b.name || a.colour != b.colour || !(a.tuning == b.tuning) ||
      !(a.mixer == b.mixer) || a.active != b.active || a.noteInput != b.noteInput || a.auditionTake != b.auditionTake)
    return false;
  if (!deepEqList(a.children, b.children) || !deepEqList(a.lanes, b.lanes) || !deepEqList(a.devices, b.devices) ||
      !deepEqList(a.takes, b.takes))
    return false;
  return sameSlots(a.clips, b.clips);
}

const TakeLane* Track::takeLane(LaneId lane) const noexcept {
  const int32_t i = takeIndex(lane);
  return i < 0 ? nullptr : takes[static_cast<size_t>(i)].get();
}

int32_t Track::takeIndex(LaneId lane) const noexcept {
  if (!lane.valid()) return -1;
  for (size_t i = 0; i < takes.size(); ++i)
    if (takes[i]->id == lane) return static_cast<int32_t>(i);
  return -1;
}

const std::vector<ClipSlot>& playedClips(const Track& t) noexcept {
  if (const TakeLane* l = t.takeLane(t.auditionTake)) return l->clips;
  return t.clips;
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
         a.defaultTuning == b.defaultTuning &&
         deepEqList(a.tracks, b.tracks) && deepEqList(a.returns, b.returns) && deepEq(a.master, b.master) &&
         (a.wires == b.wires || wiresOf(a) == wiresOf(b)) && (a.samples == b.samples || samplesOf(a) == samplesOf(b)) &&
         a.punch == b.punch && a.record == b.record;
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

const NoteOutput* findNoteOutput(std::string_view port) noexcept {
  for (const NoteOutput& o : kNoteOutputs)
    if (o.port == port) return &o;
  return nullptr;
}

const Wire* findWire(const Project& p, WireId id) noexcept {
  const WireList& w = wiresOf(p);
  const auto it = std::lower_bound(w.begin(), w.end(), id, [](const Wire& a, WireId b) { return a.id < b; });
  return it != w.end() && it->id == id ? &*it : nullptr;
}

const Track* laneHome(const Project& p, const ParamRef& r) noexcept {
  switch (r.kind) {
    case ParamKind::DeviceParam: return findDevice(p, DeviceId{r.owner}).track;
    case ParamKind::WireAmount: {
      const Wire* w = findWire(p, WireId{r.owner});
      return w != nullptr ? findDevice(p, w->to).track : nullptr;
    }
    default: return nullptr;
  }
}

Snapshot makeDefaultProject(IdSource& ids, bool wireMaster) {
  auto p = std::make_shared<Project>();
  p->timeline = std::make_shared<const Timeline>();
  TuningSlot slot;
  slot.id = ids.tuning();
  slot.name = "12-EDO";
  slot.colour = Colour{0x5E9BD6};
  p->defaultTuning = slot.id;
  p->tunings = std::make_shared<const TuningList>(TuningList{std::make_shared<const TuningSlot>(std::move(slot))});
  p->locators = std::make_shared<const LocatorList>();
  p->master = std::make_shared<const Track>(makeTrack(ids.track(), TrackKind::Master, "Master"));
  // TX.all-free: the master's input → its mixer (its main path; main_path.h).
  if (!wireMaster) return p;
  Wire w;
  w.id = ids.wire();
  w.noteSource = p->master->id;
  w.mixer = p->master->id;
  p->wires = std::make_shared<const WireList>(WireList{std::move(w)});
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
    });
    for (const LanePtr& l : t.lanes) {
      see(l->id.value);
      for (const Breakpoint& b : l->points) see(b.id.value);
    }
    auto seeClips = [&](const std::vector<ClipSlot>& clips) {
      for (const ClipSlot& c : clips) {
        see(c.id.value);
        if (c.clip)
          for (const Note& n : c.clip->notes) see(n.id.value);
      }
    };
    seeClips(t.clips);
    for (const TakeLanePtr& l : t.takes) {  // T6.1
      see(l->id.value);
      seeClips(l->clips);
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

const TuningSlot* defaultTuningOf(const Project& p) noexcept { return findTuning(p, p.defaultTuning); }

int32_t findClipIndex(const Track& t, ClipId id) noexcept {
  for (size_t i = 0; i < t.clips.size(); ++i)
    if (t.clips[i].id == id) return static_cast<int32_t>(i);
  return -1;
}

namespace {
bool findIn(const std::vector<DevicePtr>& list, DeviceId id, DeviceLoc& out) {
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i]->id == id) {
      out.device = list[i].get();
      out.index = static_cast<uint32_t>(i);
      return true;
    }
  return false;
}
}  // namespace

DeviceLoc findDevice(const Project& p, DeviceId id) noexcept {
  DeviceLoc out;
  if (!id.valid()) return out;
  forEachTrack(p, [&](const Track& t, const Track*) {
    if (out.track) return;
    if (findIn(t.devices, id, out)) out.track = &t;
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
  const TuningSlot* d = defaultTuningOf(p);
  return d ? &d->def : nullptr;
}

}  // namespace pq::doc
