// GU.4: the built-in modules (group presets) and the load conversion of the retired meta modules:
// pq/document/modules.h.
#include "pq/document/modules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pq/document/panel.h"
#include "pq/meta/registry.h"
#include "pq/meta/retired.h"

namespace pq::doc {

namespace {

constexpr std::array<ModuleInfo, 2> kModules = {{
    {ModuleKind::PhaseAccumulator, "phase_acc", "Phase Accumulator",
     "A phase ramp 0 … 1 at its frequency (plus the note's Hz in an instrument group), transposed by its pitch, offset by "
     "its phase, scaled by its amplitude: hz + freq → × 2^(pitch/1200) → phasor → + phase → frac → × amp. Feed it into a "
     "Wavetable, or use it as a slow modulation source."},
    {ModuleKind::Vca, "vca", "VCA",
     "One input, one output, one knob: out = in × value. The knob's curve is the VCA's law: fine between −1 and +1 in "
     "the middle of its travel, logarithmic out to ±20000 (edit it with the curve editor)."},
}};

struct Builder {
  IdSource& ids;
  BlockSet set;
  DeviceId group;
  DeviceId add(std::string patch, std::string name, DeviceId id = {}) {
    if (!id.valid()) id = ids.device();
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
  DeviceId atom(std::string_view atomName, std::string name) {
    return add(std::string(kAtomDevicePrefix) + std::string(atomName), std::move(name));
  }
  void wire(DeviceId from, std::string_view port, DeviceId to, std::string_view param) {
    Wire w;
    w.id = ids.wire();
    w.from = from;
    w.port = std::string(port);
    w.to = to;
    w.param = std::string(param);
    w.law = WireLaw::Add;
    set.wires.push_back(std::move(w));
  }
};

DevicePtr groupDevice(DeviceId id, DeviceId inGroup, std::string name, bool active, Panel panel) {
  auto g = std::make_shared<Device>(*makeGroup(id));
  g->name = std::move(name);
  g->group = inGroup;
  g->active = active;
  g->panel = std::make_shared<const Panel>(std::move(panel));
  return g;
}

PanelElement element(uint32_t id, const char* kind, float x, float y, float w, float h, Binding b) {
  PanelElement e;
  e.id = id;
  e.kind = kind;
  e.x = x, e.y = y, e.w = w, e.h = h;
  e.bindings = {std::move(b)};
  return e;
}

dsp::Curve withUnit(dsp::Curve c, dsp::CurveUnit u) {
  c.unit = u;
  return c;
}

}  // namespace

std::span<const ModuleInfo> moduleInfos() noexcept { return kModules; }

const ModuleInfo* findModule(std::string_view key) noexcept {
  for (const ModuleInfo& m : kModules)
    if (m.key == key || m.name == key) return &m;
  return nullptr;
}

dsp::Curve phaseAccumulatorCurve(std::string_view control) {
  // The retired meta module's declared curves (meta::paramCurve of pq/meta/retired.h's specs, written out).
  if (control == kPaFrequency) {
    dsp::Curve c = withUnit(dsp::curvePreset(dsp::CurvePreset::Exp0, 0.0, 20000.0, 14.0), dsp::CurveUnit::Hz);
    c.mode = dsp::CurveMode::Signed;
    return c;
  }
  if (control == kPaPhase) {
    dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 1.0);
    c.mode = dsp::CurveMode::Wrap;
    return c;
  }
  if (control == kPaAmplitude) return dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 4.0);
  if (control == kPaPitch) {
    // GU.5b: the old face's pitch arc scale — ±12 semitones over the middle half of the travel, the rest of ±4 octaves
    // over the outer quarters (straight segments: a wire still adds in cents).
    dsp::Curve c = withUnit(dsp::curvePreset(dsp::CurvePreset::Linear, -4800.0, 4800.0), dsp::CurveUnit::Cents);
    c.segments = 3;
    c.points[0] = {0.0, 0.0};
    c.points[1] = {0.25, 0.375};
    c.points[2] = {0.75, 0.625};
    c.points[3] = {1.0, 1.0};
    c.shapes[0] = c.shapes[1] = c.shapes[2] = {dsp::CurveShape::Linear, 0.0};
    c.preset = dsp::CurvePreset::Custom;
    return c;
  }
  return dsp::curvePreset(dsp::CurvePreset::Linear, 1.0, 1.0);  // kPaReference: a constant 1
}

BlockSet phaseAccumulatorBlocks(IdSource& ids, const PhaseAccumulatorOptions& o, PhaseAccumulatorRoles* roles) {
  PhaseAccumulatorRoles r;
  r.group = o.id.valid() ? o.id : ids.device();
  Builder b{ids, {}, r.group};
  r.freq = b.value(std::string(kPaFrequency), o.freqCurve.value_or(phaseAccumulatorCurve(kPaFrequency)), o.freq);
  r.phase = b.value(std::string(kPaPhase), o.phaseCurve.value_or(phaseAccumulatorCurve(kPaPhase)), o.phase);
  r.amp = b.value(std::string(kPaAmplitude), o.ampCurve.value_or(phaseAccumulatorCurve(kPaAmplitude)), o.amp);
  r.pitch = b.value(std::string(kPaPitch), o.pitchCurve.value_or(phaseAccumulatorCurve(kPaPitch)), o.pitch);
  r.ref = b.value(std::string(kPaReference), phaseAccumulatorCurve(kPaReference), 1.0);
  // The meta module's expansion, atom for atom and operand for operand (its renders: diff 0).
  r.sum = b.atom("add", "hz + freq");
  r.ratio = b.atom("cents_to_hz", "2^(pitch/1200)");
  r.pitched = b.atom("mul", "× ratio");
  r.phasor = b.atom("phasor", "phasor");
  r.addPhase = b.atom("add", "+ phase");
  const bool pm = o.pm || o.feed == PhaseAccumulatorOptions::Feed::Pm;
  if (pm) r.pm = b.atom("add", "+ pm");
  r.frac = b.atom("frac", "frac");
  r.out = b.atom("mul", "× amp");
  const std::string y(kValueOutput);
  if (o.feed == PhaseAccumulatorOptions::Feed::Hz) b.wire(r.group, kGroupInPort, r.sum, "a");
  b.wire(r.freq, y, r.sum, "b");
  b.wire(r.pitch, y, r.ratio, "cents");
  b.wire(r.ref, y, r.ratio, "ref");
  b.wire(r.sum, "out", r.pitched, "a");
  b.wire(r.ratio, "out", r.pitched, "b");
  b.wire(r.pitched, "out", r.phasor, "hz");
  b.wire(r.phasor, "out", r.addPhase, "a");
  b.wire(r.phase, y, r.addPhase, "b");
  if (pm) {
    b.wire(r.addPhase, "out", r.pm, "a");
    if (o.feed == PhaseAccumulatorOptions::Feed::Pm) b.wire(r.group, kGroupInPort, r.pm, "b");
    b.wire(r.pm, "out", r.frac, "x");
  } else {
    b.wire(r.addPhase, "out", r.frac, "x");
  }
  b.wire(r.amp, y, r.out, "a");
  b.wire(r.frac, "out", r.out, "b");
  b.wire(r.out, "out", r.group, kGroupOutPort);
  // The panel (GU.5b: the old face, element for element): the body circle, the four controls as arcs round it —
  // Frequency up the left side, Amplitude over the top, Pitch down the right (0 cents at 3 o'clock), Phase along the
  // bottom — and the phase orbit in the middle (its hub lit by the amplitude; voices on it where the output is tapped).
  Panel panel;
  panel.width = 120.0f;
  panel.height = 120.0f;
  const std::string vp(kValueParam);
  PanelElement body = element(1, "shape", 12, 16, 96, 96, {});
  body.bindings.clear();
  body.setField(styleText("shape", "circle"));
  body.setField(styleText("fill", "body"));
  body.setField(styleText("stroke", "frame"));
  body.setField(styleNumber("thickness", 1.25));
  auto arc = [&](uint32_t id, DeviceId block, double start, double sweep, double from, double to, const char* drag) {
    PanelElement e = element(id, "arc", 0, 0, 120, 120, controlBinding(block, vp));
    e.setField(styleNumber("start", start));
    e.setField(styleNumber("sweep", sweep));
    e.setField(styleNumber("hitFrom", from));
    e.setField(styleNumber("hitTo", to));
    e.setField(styleText("drag", drag));
    return e;
  };
  PanelElement f = arc(2, r.freq, 150.0, 90.0, 140.0, 246.0, "vertical");
  f.setField(styleText("ticks", "100|1000|10000"));
  PanelElement a = arc(3, r.amp, 252.0, 40.0, 246.0, 299.0, "diagonal");
  a.setField(styleText("ticks", "1|2|3"));
  PanelElement p = arc(4, r.pitch, 36.0, -72.0, 315.0, 43.0, "vertical");
  p.setField(styleNumber("origin", 0.5));
  p.setField(styleText("ticks", "-3600|-2400|-1200|0|1200|2400|3600"));
  {
    std::string minor;
    for (int st = -11; st <= 11; ++st)
      if (st != 0) minor += (minor.empty() ? "" : "|") + std::to_string(100 * st);
    p.setField(styleText("minor", minor));
  }
  p.setField(styleNumber("dragPx", 300.0));
  p.setField(styleText("readout", "corner"));
  PanelElement ph = arc(5, r.phase, 130.0, -80.0, 43.0, 140.0, "horizontal");
  ph.setField(styleText("ticks", "0.25|0.5|0.75"));
  PanelElement orbit = element(6, "orbit", 0, 0, 120, 120, signalBinding(r.out, "out"));
  orbit.bindings.push_back(controlBinding(r.amp, vp));
  panel.elements = {std::move(body), std::move(f), std::move(a), std::move(p), std::move(ph), std::move(orbit)};
  BlockSet out;
  out.devices.push_back(groupDevice(r.group, o.inGroup, o.name.empty() ? std::string("Phase Accumulator") : o.name, o.active, std::move(panel)));
  for (DevicePtr& d : b.set.devices) out.devices.push_back(std::move(d));
  out.wires = std::move(b.set.wires);
  std::sort(out.wires.begin(), out.wires.end(), [](const Wire& x, const Wire& y) { return x.id < y.id; });
  if (roles != nullptr) *roles = r;
  return out;
}

dsp::Curve vcaCurve(double min, double max, double zone) { return dsp::curvePreset(dsp::CurvePreset::Vca, min, max, zone); }

BlockSet vcaBlocks(IdSource& ids, const VcaOptions& o, VcaRoles* roles) {
  VcaRoles r;
  r.group = o.id.valid() ? o.id : ids.device();
  Builder b{ids, {}, r.group};
  r.value = b.value(std::string(kVcaValueName), o.curve.value_or(vcaCurve()), o.value);
  r.mul = b.atom("mul", "×");
  if (o.plugged) b.wire(r.group, kGroupInPort, r.mul, "a");
  b.wire(r.value, kValueOutput, r.mul, "b");
  b.wire(r.mul, "out", r.group, kGroupOutPort);
  // The panel (GU.5b: the old face): the dial — a round 360° knob with its value inside, the curve's points as markers
  // on its track (±1, ±10, ±100: dragging one edits the curve) and its range and curve in a popup under the chevron.
  Panel panel;
  panel.width = 120.0f;
  panel.height = 120.0f;
  panel.elements = {element(1, "dial", 0, 0, 120, 120, controlBinding(r.value, std::string(kValueParam)))};
  BlockSet out;
  out.devices.push_back(groupDevice(r.group, o.inGroup, o.name.empty() ? std::string("VCA") : o.name, o.active, std::move(panel)));
  for (DevicePtr& d : b.set.devices) out.devices.push_back(std::move(d));
  out.wires = std::move(b.set.wires);
  std::sort(out.wires.begin(), out.wires.end(), [](const Wire& x, const Wire& y) { return x.id < y.id; });
  if (roles != nullptr) *roles = r;
  return out;
}

BlockSet moduleBlocks(ModuleKind k) {
  IdSource ids;
  return k == ModuleKind::PhaseAccumulator ? phaseAccumulatorBlocks(ids) : vcaBlocks(ids);
}

// ==== Load conversion =================================================================================================

namespace {

// A group's chain ports by name (groupPatchText: `in x`, `out y`; edit_groups chainPortsOf, what Ctrl+G's links name).
constexpr std::string_view kGroupChainIn = "x";
constexpr std::string_view kGroupChainOut = "y";

bool retired(const Device& d) noexcept {
  return isMetaDevice(d) && meta::findType(metaDeviceType(d)) == nullptr && meta::findRetiredType(metaDeviceType(d)) != nullptr;
}

// Whether a retired device sits inside rack `d` at any depth (forEachDeviceIn: iterative, any nesting depth).
bool rackHoldsRetired(const Device& d) noexcept {
  if (!d.isRack()) return false;
  bool any = false;
  forEachDeviceIn(d, [&](const Device& x) { any = any || (&x != &d && retired(x)); });
  return any;
}

// A retired param's value in units (its stored knob position through its declared curve; absent: its default).
double unitsOf(const Device& d, const meta::Type& t, std::string_view name) {
  const int32_t i = t.paramIndex(name);
  if (i < 0) return 0.0;
  const meta::ParamSpec& s = t.params[static_cast<size_t>(i)];
  if (const DeviceValue* v = d.param(name)) return dsp::evalCurve(meta::paramCurve(s), v->value);
  return s.def;
}
double positionOf(const Device& d, const meta::Type& t, std::string_view name) {
  const int32_t i = t.paramIndex(name);
  if (const DeviceValue* v = d.param(name)) return v->value;
  return i < 0 ? 0.0 : dsp::curvePosition(meta::paramCurve(t.params[static_cast<size_t>(i)]), t.params[static_cast<size_t>(i)].def, 0.0);
}

// The old VCA's law as a curve (markers aside) and whether it was its Bend curve.
dsp::Curve oldVcaCurve(const Device& d, const meta::Type& t, bool& bendCurve) {
  const double lo = unitsOf(d, t, "min"), max = unitsOf(d, t, "max");
  const DeviceValue* cs = d.setting("curve");
  bendCurve = cs != nullptr && std::lround(cs->value) == 1;
  if (!bendCurve) return vcaCurve(lo, max, unitsOf(d, t, "zone"));
  const double hi = std::max(max, lo);
  const double k = meta::kRetiredVcaBendK * std::clamp(unitsOf(d, t, "bend"), -1.0, 1.0);
  if (std::fabs(k) < meta::kRetiredVcaLinearK) return dsp::curvePreset(dsp::CurvePreset::Linear, lo, hi);
  // lo + (hi − lo)·(e^(K·p) − 1)/(e^K − 1): one Exp segment of bend K (the Exponential / Logarithmic presets' form).
  return dsp::curvePreset(k > 0.0 ? dsp::CurvePreset::Exponential : dsp::CurvePreset::Logarithmic, lo, hi, std::fabs(k));
}

// Where an old device's ends go.
struct Target {
  DeviceId device;
  std::string port;  // a param / input (into), an output (out of)
  bool drop = false;
  bool units = false;  // an Add wire becomes a Units wire (the VCA's Bend curve)
};
struct Converted {
  std::vector<DevicePtr> devices;  // the replacement (the group first) in list order
  WireList internal;
  std::unordered_map<std::string, Target> into;  // by the old param / input name ("" = the main input)
  Target out;                                    // the old outputs ("" and "out")
  std::unordered_map<std::string, DeviceId> controls;  // old param → its value block (lanes)
};

bool wiredInto(const Project& p, DeviceId d, std::string_view param) {
  for (const Wire& w : wiresOf(p))
    if (w.to == d && !w.toMixer() && w.param == param) return true;
  return false;
}

Converted convertPa(const Project& p, const Device& d, IdSource& ids, BlockSet* presetOut = nullptr) {
  const meta::Type& t = *meta::findRetiredType(meta::kRetiredPhaseAcc);
  PhaseAccumulatorOptions o;
  o.id = d.id;
  o.inGroup = d.group;
  o.name = d.name;
  o.active = d.active;
  o.freq = positionOf(d, t, "freq");
  o.phase = positionOf(d, t, "phase");
  o.amp = positionOf(d, t, "amp");
  o.pitch = positionOf(d, t, "pitch");
  if (const dsp::Curve* c = d.curve("freq")) o.freqCurve = *c;
  if (const dsp::Curve* c = d.curve("phase")) o.phaseCurve = *c;
  if (const dsp::Curve* c = d.curve("amp")) o.ampCurve = *c;
  if (const dsp::Curve* c = d.curve("pitch")) o.pitchCurve = *c;
  // The chain input (the main input): the type's own (hz), another input by name, or unplugged.
  const std::string feed = d.chainInput.empty() ? std::string("hz") : d.chainInput;
  o.feed = feed == "hz" ? PhaseAccumulatorOptions::Feed::Hz : feed == "pm" ? PhaseAccumulatorOptions::Feed::Pm : PhaseAccumulatorOptions::Feed::None;
  o.pm = wiredInto(p, d.id, "pm");
  PhaseAccumulatorRoles r;
  BlockSet set = phaseAccumulatorBlocks(ids, o, &r);
  Converted c;
  c.devices = set.devices;
  c.internal = set.wires;
  const std::string vp(kValueParam);
  c.into[""] = {r.group, ""};
  // (A wire into the chain input by its name goes to the group's by its name, `x`: Ctrl+G's chain links name the ports.)
  c.into["hz"] = o.feed == PhaseAccumulatorOptions::Feed::Hz ? Target{r.group, std::string(kGroupChainIn)} : Target{r.sum, "a"};
  c.into["pm"] = o.feed == PhaseAccumulatorOptions::Feed::Pm ? Target{r.group, std::string(kGroupChainIn)} : Target{r.pm, "b"};
  c.into["freq"] = {r.freq, vp};
  c.into["phase"] = {r.phase, vp};
  c.into["amp"] = {r.amp, vp};
  c.into["pitch"] = {r.pitch, vp};
  c.controls = {{"freq", r.freq}, {"phase", r.phase}, {"amp", r.amp}, {"pitch", r.pitch}};
  c.out = {r.group, std::string(kGroupChainOut)};
  if (presetOut != nullptr) *presetOut = std::move(set);
  return c;
}

Converted convertVca(const Project* p, const Device& d, IdSource& ids, BlockSet* presetOut = nullptr) {
  const meta::Type& t = *meta::findRetiredType(meta::kRetiredVca);
  bool bend = false;
  const dsp::Curve curve = oldVcaCurve(d, t, bend);
  const double pos = positionOf(d, t, "value");
  // Whether anything fed `in` (its own wire, or the main input while it is the chain input): else the module was a
  // constant source of its value (in read its default 1) — a Value block.
  const bool plugged = d.chainInput != "-";
  bool fed = presetOut != nullptr;  // a preset: as the module (its wires come with its use)
  if (p != nullptr)
    for (const Wire& w : wiresOf(*p))
      if (w.to == d.id && !w.toMixer() && (w.param == "in" || (plugged && w.param.empty()))) fed = true;
  Converted c;
  const std::string vp(kValueParam);
  if (!fed) {
    auto v = std::make_shared<Device>(*makeDevice(d.id, std::string(kValueDevice), DeviceKind::AudioEffect));
    v->name = d.name.empty() ? std::string("VCA") : d.name;
    v->group = d.group;
    v->active = d.active;
    v->params = {DeviceValue{vp, std::clamp(pos, 0.0, 1.0)}};
    v->curves = {ControlCurve{vp, curve}};
    c.devices = {v};
    c.into["value"] = {d.id, vp, false, bend};
    c.into[""] = {d.id, ""};  // (an unplugged VCA on a path: the link stays; a value block has no main input)
    c.into["in"] = {d.id, "", true};
    c.controls = {{"value", d.id}};
    c.out = {d.id, "y"};
    if (presetOut != nullptr) *presetOut = BlockSet{c.devices, {}};
    return c;
  }
  VcaOptions o;
  o.id = d.id;
  o.inGroup = d.group;
  o.name = d.name;
  o.active = d.active;
  o.value = pos;
  o.curve = curve;
  o.plugged = plugged;
  VcaRoles r;
  BlockSet set = vcaBlocks(ids, o, &r);
  c.devices = set.devices;
  c.internal = set.wires;
  c.into[""] = {r.group, ""};
  c.into["in"] = plugged ? Target{r.group, std::string(kGroupChainIn)} : Target{r.mul, "a"};
  c.into["value"] = {r.value, vp, false, bend};
  c.controls = {{"value", r.value}};
  c.out = {r.group, std::string(kGroupChainOut)};
  if (presetOut != nullptr) *presetOut = std::move(set);
  return c;
}

void rebuildTracks(std::vector<TrackPtr>& list, const std::unordered_map<uint32_t, std::vector<DevicePtr>>& devices,
                   const std::unordered_map<uint32_t, std::vector<LanePtr>>& lanes) {
  for (TrackPtr& tp : list) {
    std::vector<TrackPtr> children = tp->children;
    rebuildTracks(children, devices, lanes);
    const auto d = devices.find(tp->id.value);
    const auto l = lanes.find(tp->id.value);
    if (d == devices.end() && l == lanes.end() && children == tp->children) continue;
    auto t = std::make_shared<Track>(*tp);
    t->children = std::move(children);
    if (d != devices.end()) t->devices = d->second;
    if (l != lanes.end()) t->lanes = l->second;
    tp = std::move(t);
  }
}

}  // namespace

bool holdsRetiredModules(const Project& p) noexcept {
  bool any = false;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const DevicePtr& d : t.devices)
      if (retired(*d) || rackHoldsRetired(*d)) any = true;
  });
  return any;
}

Status convertRetiredModules(Project& p) {
  if (!holdsRetiredModules(p)) return Ok{};
  IdSource ids;
  ids.reserveThrough(maxEntityId(p));
  std::unordered_map<uint32_t, Converted> made;  // by old device id
  std::unordered_map<uint32_t, std::vector<DevicePtr>> newDevices;
  std::unordered_map<uint32_t, std::vector<LanePtr>> newLanes;
  Status failed = Ok{};
  forEachTrack(p, [&](const Track& t, const Track*) {
    bool changed = false;
    std::vector<DevicePtr> list;
    for (const DevicePtr& d : t.devices) {
      if (rackHoldsRetired(*d) && failed) {
        failed = Error{ErrorCode::Unsupported,
                       "track '" + t.name + "': a Phase Accumulator or VCA inside a rack chain cannot become its group preset "
                       "(GU.4: a group lives in a track's circuit, not in a rack chain); take it out of the rack in an "
                       "older pqdaw first",
                       {}};
      }
      if (!retired(*d)) {
        list.push_back(d);
        continue;
      }
      Converted c = metaDeviceType(*d) == meta::kRetiredPhaseAcc ? convertPa(p, *d, ids) : convertVca(&p, *d, ids);
      for (const DevicePtr& x : c.devices) list.push_back(x);
      made.emplace(d->id.value, std::move(c));
      changed = true;
    }
    if (!changed) return;
    newDevices[t.id.value] = std::move(list);
    // Lanes of the old controls follow onto their value blocks; lanes of the curve's params go.
    std::vector<LanePtr> lanes;
    bool lanesChanged = false;
    for (const LanePtr& l : t.lanes) {
      const auto m = l->param.kind == ParamKind::DeviceParam ? made.find(l->param.owner) : made.end();
      if (m == made.end()) {
        lanes.push_back(l);
        continue;
      }
      lanesChanged = true;
      const auto ctl = m->second.controls.find(l->paramName);
      if (ctl == m->second.controls.end()) continue;  // a VCA's min / max / zone / bend / marker: the curve holds it now
      auto n = std::make_shared<AutomationLane>(*l);
      n->param = ParamRef::device(ctl->second, kValueParam);
      n->paramName = std::string(kValueParam);
      lanes.push_back(std::move(n));
    }
    if (lanesChanged) newLanes[t.id.value] = std::move(lanes);
  });
  if (!failed) return failed;
  // Wires: ends on the old devices follow their targets; the modules' own wires join.
  WireList wires;
  for (const Wire& w0 : wiresOf(p)) {
    Wire w = w0;
    bool drop = false;
    if (!w.fromNoteSource()) {
      if (const auto m = made.find(w.from.value); m != made.end()) {
        w.from = m->second.out.device;
        if (!w.port.empty()) w.port = m->second.out.port;  // the main output stays the main output; `out` by its name
      }
    }
    if (!w.toMixer()) {
      if (const auto m = made.find(w.to.value); m != made.end()) {
        const auto it = m->second.into.find(w.param);
        if (it == m->second.into.end() || it->second.drop || !it->second.device.valid()) {
          drop = true;
        } else {
          w.to = it->second.device;
          w.param = it->second.port;
          if (it->second.units && w.law == WireLaw::Add) w.law = WireLaw::Units;
        }
      }
    }
    if (!drop) wires.push_back(std::move(w));
  }
  for (auto& [id, c] : made)
    for (const Wire& w : c.internal) wires.push_back(w);
  std::sort(wires.begin(), wires.end(), [](const Wire& a, const Wire& b) { return a.id < b.id; });
  rebuildTracks(p.tracks, newDevices, newLanes);
  rebuildTracks(p.returns, newDevices, newLanes);
  if (p.master) {
    std::vector<TrackPtr> m{p.master};
    rebuildTracks(m, newDevices, newLanes);
    p.master = m.front();
  }
  p.wires = wires.empty() ? nullptr : std::make_shared<const WireList>(std::move(wires));
  return Ok{};
}

std::optional<BlockSet> convertRetiredPreset(const Device& d) {
  if (!retired(d)) return std::nullopt;
  IdSource ids;
  ids.reserveThrough(d.id.value);
  BlockSet set;
  if (metaDeviceType(d) == meta::kRetiredPhaseAcc) {
    Project empty;
    (void)convertPa(empty, d, ids, &set);
  } else {
    (void)convertVca(nullptr, d, ids, &set);
  }
  // A preset's top-level block is in no group.
  if (!set.devices.empty() && set.devices.front()->group.valid()) {
    auto g = std::make_shared<Device>(*set.devices.front());
    g->group = {};
    set.devices.front() = std::move(g);
  }
  return set;
}

}  // namespace pq::doc
