// GU.4: the built-in modules (group presets) and the load conversion of the retired meta modules:
// pq/document/modules.h.
#include "pq/document/modules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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

constexpr std::array<ModuleInfo, 12> kModules = {{
    {ModuleKind::PhaseAccumulator, "phase_acc", "Phase Accumulator",
     "A phase ramp 0 … 1 at its frequency (plus the note's Hz in an instrument group), transposed by its pitch, offset by "
     "its phase, scaled by its amplitude: hz + freq → × 2^(pitch/1200) → phasor → + phase → frac → × amp. Feed it into a "
     "Wavetable, or use it as a slow modulation source."},
    {ModuleKind::Vca, "vca", "VCA",
     "One input, one output, one knob: out = in × value. The knob's curve is the VCA's law: fine between −1 and +1 in "
     "the middle of its travel, logarithmic out to ±20000 (edit it with the curve editor)."},
    {ModuleKind::Wavetable, "wavetable", "Wavetable",
     "A wavetable oscillator read by its phase input (feed it a Phase Accumulator): keyframes drawn or built from "
     "harmonics, blended by the position, shaped by fold, warp and sync. Its engine is a native block; its face is a "
     "panel of the harmonic editor, the table display, the draw tools and knobs."},
    {ModuleKind::Envelope, "envelope", "Envelope",
     "Attack, decay, sustain and release with a curve per segment, multiplying its input (a VCA) or, unwired, the "
     "envelope itself. Drag the breakpoints and bends on its display."},
    {ModuleKind::Oscilloscope, "oscilloscope", "Oscilloscope",
     "Shows the signal passing through it as a scope (fixed, pitch-locked or beat-locked timebase); changes nothing."},
    {ModuleKind::Spectrum, "spectrum", "Spectrum", "Shows the spectrum of the signal passing through it; changes nothing."},
    {ModuleKind::Spectrogram, "spectrogram", "Spectrogram",
     "Shows the signal passing through it as a scrolling spectrogram; changes nothing."},
    {ModuleKind::WavetableSynth, "wavetable_synth", "Wavetable Synth",
     "An instrument built from the modules: Phase Accumulator → Wavetable → Envelope per note, with two LFOs (each a "
     "Phase Accumulator into a Wavetable) on the wavetable position and the pitch. Tabs for the oscillator, the "
     "envelope and each LFO; open it to see and rewire the circuit.",
     true},
    // GU.10: the stock devices, rebuilt as group presets.
    {ModuleKind::Utility, "utility", "Utility",
     "Channel mode, phase invert, width or mid/side balance, mono, balance, gain and mute: one 2 x 2 matrix computed at "
     "param rate, applied through smoothed gains."},
    {ModuleKind::Saturator, "saturator", "Saturator",
     "Drive into one of seven curves (Analog Clip, Soft Sine, Bass Shaper, Medium, Hard, Sinoid Fold, Digital Clip), "
     "output gain, soft clip and dry/wet."},
    {ModuleKind::Lfo, "lfo", "LFO",
     "A low-frequency oscillator (sine, triangle, saws, square), free-running or synced to the song; wire its output (or "
     "drag from its jack) onto any control."},
    {ModuleKind::Operator, "operator", "Operator",
     "Four FM operators (Phase Accumulator, Wavetable, Envelope each), D into C into B into A: an instrument group.",
     true},
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
  // (GU.7: the orbit shows the phase each voice reads — frac's output, what the old module tapped — through the tap
  // the lowering puts on it while the orbit is placed.)
  PanelElement orbit = element(6, "orbit", 0, 0, 120, 120, signalBinding(r.frac, "out"));
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

// ---- GU.7: engine modules -----------------------------------------------------------------------------------------

std::string_view moduleEngineType(ModuleKind k) noexcept {
  switch (k) {
    case ModuleKind::Wavetable: return "wavetable";
    case ModuleKind::Envelope: return "envelope";
    case ModuleKind::Oscilloscope: return "oscilloscope";
    case ModuleKind::Spectrum: return "spectrum";
    case ModuleKind::Spectrogram: return "spectrogram";
    case ModuleKind::PhaseAccumulator:
    case ModuleKind::Vca:
    case ModuleKind::WavetableSynth:
    case ModuleKind::Utility:
    case ModuleKind::Saturator:
    case ModuleKind::Lfo:
    case ModuleKind::Operator: break;
  }
  return {};
}

const ModuleInfo* moduleOfEngine(std::string_view typeId) noexcept {
  if (typeId.empty()) return nullptr;
  for (const ModuleInfo& m : kModules)
    if (moduleEngineType(m.kind) == typeId) return &m;
  return nullptr;
}

namespace {

// A generic element drawn as the old face's control: no label, no readout.
PanelElement plainControl(uint32_t id, const char* kind, float x, float y, float w, float h, DeviceId engine, const char* param) {
  PanelElement e = element(id, kind, x, y, w, h, controlBinding(engine, param));
  e.setField(styleText("label", "-"));
  e.setField(styleText("readout", "none"));
  return e;
}
// The bottom element giving the panel the old face's corner (the views: a "shape" with plate 1 is the plate's outline).
PanelElement plate(float w, float h, double radius) {
  PanelElement e = element(1, "shape", 0, 0, w, h, {});
  e.bindings.clear();
  e.setField(styleNumber("plate", 1.0));
  e.setField(styleText("fill", "none"));
  e.setField(styleText("stroke", "none"));
  e.setField(styleNumber("radius", radius));
  return e;
}

}  // namespace

Panel enginePanel(ModuleKind k, DeviceId engine) {
  Panel p;
  const Binding data = dataBinding(engine);
  auto rich = [&](uint32_t id, const char* kind, float x, float y, float w, float h) { return element(id, kind, x, y, w, h, data); };
  switch (k) {
    case ModuleKind::Wavetable: {  // the old face: 204 × 248 (views wavetable_face.cpp)
      p.width = 204.0f;
      p.height = 248.0f;
      p.elements.push_back(plate(p.width, p.height, 12.0));
      p.elements.push_back(rich(2, "drawtools", 8, 13, 188, 21));
      p.elements.push_back(rich(3, "harmonics", 10, 34, 184, 42));
      p.elements.push_back(rich(4, "wavetable", 8, 76, 192, 100));
      PanelElement phase = plainControl(5, "slider", 12, 176, 154, 12, engine, "phase");
      phase.setField(styleText("orientation", "horizontal"));
      p.elements.push_back(std::move(phase));
      constexpr float kKnobX[3] = {38.0f, 89.0f, 140.0f};
      constexpr const char* kKnobs[3] = {"fold", "warp", "sync"};
      for (uint32_t i = 0; i < 3; ++i) {
        PanelElement kn = plainControl(6 + i, "knob", kKnobX[i] - 17.75f, 212.0f - 17.75f, 35.5f, 35.5f, engine, kKnobs[i]);
        kn.setField(styleNumber("track", 3.5));
        p.elements.push_back(std::move(kn));
      }
      p.elements.push_back(rich(9, "warpmode", 20, 225, 168, 16));
      break;
    }
    case ModuleKind::Envelope: {  // 208 × 128 (views envelope_face.cpp)
      p.width = 208.0f;
      p.height = 128.0f;
      p.elements.push_back(plate(p.width, p.height, 12.0));
      p.elements.push_back(rich(2, "envelope", 6, 6, 178, 116));
      PanelElement amt = plainControl(3, "slider", 184, 12, 16, 104, engine, "amount");
      amt.setField(styleText("orientation", "vertical"));
      p.elements.push_back(std::move(amt));
      break;
    }
    case ModuleKind::Oscilloscope:
    case ModuleKind::Spectrum:
    case ModuleKind::Spectrogram: {  // the display fills the panel (260 × 160, 240 × 150, 300 × 170)
      const char* kind = k == ModuleKind::Oscilloscope ? "scope" : k == ModuleKind::Spectrum ? "spectrum" : "spectrogram";
      p.width = k == ModuleKind::Oscilloscope ? 260.0f : k == ModuleKind::Spectrum ? 240.0f : 300.0f;
      p.height = k == ModuleKind::Oscilloscope ? 160.0f : k == ModuleKind::Spectrum ? 150.0f : 170.0f;
      p.elements.push_back(plate(p.width, p.height, 10.0));
      p.elements.push_back(rich(2, kind, 0, 0, p.width, p.height));
      break;
    }
    case ModuleKind::PhaseAccumulator:
    case ModuleKind::Vca:
    case ModuleKind::WavetableSynth:
    case ModuleKind::Utility:
    case ModuleKind::Saturator:
    case ModuleKind::Lfo:
    case ModuleKind::Operator: break;
  }
  return p;
}

BlockSet engineModuleBlocks(IdSource& ids, ModuleKind k, const EngineModuleOptions& o, EngineRoles* roles) {
  const std::string_view type = moduleEngineType(k);
  const meta::Type* t = meta::findType(type);
  EngineRoles r;
  r.group = o.id.valid() ? o.id : ids.device();
  Builder b{ids, {}, r.group};
  r.engine = b.add(std::string(kMetaDevicePrefix) + std::string(type), t != nullptr ? std::string(t->name) : std::string(type));
  if (t != nullptr && t->chainInput >= 0) b.wire(r.group, kGroupInPort, r.engine, t->inputs[static_cast<size_t>(t->chainInput)].name);
  b.wire(r.engine, "out", r.group, kGroupOutPort);
  const ModuleInfo* info = nullptr;
  for (const ModuleInfo& m : kModules)
    if (m.kind == k) info = &m;
  BlockSet out;
  out.devices.push_back(groupDevice(r.group, o.inGroup, o.name.empty() ? std::string(info != nullptr ? info->name : type) : o.name,
                                    o.active, enginePanel(k, r.engine)));
  for (DevicePtr& d : b.set.devices) out.devices.push_back(std::move(d));
  out.wires = std::move(b.set.wires);
  if (roles != nullptr) *roles = r;
  return out;
}

// ---- GU.8: the Wavetable Synth -------------------------------------------------------------------------------------

BlockSet wavetableSynthBlocks(IdSource& ids, DeviceId inGroup, WavetableSynthRoles* roles) {
  WavetableSynthRoles r;
  r.group = ids.device();
  BlockSet out;
  {
    auto g = std::make_shared<Device>(*makeInstrumentGroup(r.group));
    g->name = "Wavetable Synth";
    g->group = inGroup;
    out.devices.push_back(std::move(g));
  }
  auto take = [&](BlockSet set) {
    for (DevicePtr& d : set.devices) out.devices.push_back(std::move(d));
    for (Wire& w : set.wires) out.wires.push_back(std::move(w));
  };
  auto wire = [&](DeviceId from, std::string_view port, DeviceId to, std::string_view param, WireLaw law = WireLaw::Add) {
    Wire w;
    w.id = ids.wire();
    w.from = from;
    w.port = std::string(port);
    w.to = to;
    w.param = std::string(param);
    w.law = law;
    out.wires.push_back(std::move(w));
  };
  // The voice: In → PA → Wavetable → Envelope → Out (module to module through their main ports).
  {
    PhaseAccumulatorOptions o;
    o.inGroup = r.group;
    o.name = "Osc";
    take(phaseAccumulatorBlocks(ids, o, &r.osc));
  }
  take(engineModuleBlocks(ids, ModuleKind::Wavetable, {DeviceId{}, r.group, {}, true}, &r.wavetable));
  {  // the Envelope's amount at 0.3 (−10 dB): a few voices of full-scale sines stay below full scale
    BlockSet env = engineModuleBlocks(ids, ModuleKind::Envelope, {DeviceId{}, r.group, {}, true}, &r.envelope);
    for (DevicePtr& d : env.devices)
      if (d->id == r.envelope.engine) {
        auto e = std::make_shared<Device>(*d);
        e->params = {DeviceValue{"amount", 0.3}};
        d = std::move(e);
      }
    take(std::move(env));
  }
  wire(r.group, kGroupInPort, r.osc.group, kMainPort);
  wire(r.osc.group, kMainPort, r.wavetable.group, kMainPort);
  wire(r.wavetable.group, kMainPort, r.envelope.group, kMainPort);
  wire(r.envelope.group, kMainPort, r.group, kGroupOutPort);
  // The LFOs: a free PA (the rate on its Frequency; it restarts with each note) → a Wavetable (the shape; ×2
  // oversampling, the least there is, is plenty at LFO rates) → × Depth → a Units wire (adds after the target's curve:
  // position, cents).
  constexpr double kRate[2] = {0.5, 5.0};         // Hz
  constexpr double kDepthMax[2] = {1.0, 1200.0};  // position, cents
  constexpr double kDepth[2] = {0.25, 0.0};       // a quarter of the table; vibrato off
  for (int i = 0; i < 2; ++i) {
    PhaseAccumulatorOptions o;
    o.inGroup = r.group;
    o.name = i == 0 ? "LFO 1" : "LFO 2";
    o.feed = PhaseAccumulatorOptions::Feed::None;
    o.freq = dsp::curvePosition(phaseAccumulatorCurve(kPaFrequency), kRate[i], 0.0);
    take(phaseAccumulatorBlocks(ids, o, &r.lfo[i]));
    BlockSet shape = engineModuleBlocks(ids, ModuleKind::Wavetable, {DeviceId{}, r.group, i == 0 ? "LFO 1 Shape" : "LFO 2 Shape", true},
                                        &r.lfoShape[i]);
    for (DevicePtr& d : shape.devices)
      if (d->id == r.lfoShape[i].engine) {
        auto e = std::make_shared<Device>(*d);
        e->settings = {DeviceValue{"os", 1.0}};
        d = std::move(e);
      }
    take(std::move(shape));
    Builder b{ids, {}, r.group};
    dsp::Curve dc = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, kDepthMax[i]);
    if (i == 1) dc = withUnit(dc, dsp::CurveUnit::Cents);
    r.depth[i] = b.value("Depth", dc, kDepth[i] / kDepthMax[i]);
    r.mul[i] = b.atom("mul", "× depth");
    for (DevicePtr& d : b.set.devices) out.devices.push_back(std::move(d));
    wire(r.lfo[i].group, kMainPort, r.lfoShape[i].group, kMainPort);
    wire(r.lfoShape[i].group, kMainPort, r.mul[i], "a");
    wire(r.depth[i], kValueOutput, r.mul[i], "b");
    if (i == 0) wire(r.mul[i], "out", r.wavetable.engine, "position", WireLaw::Units);
    else wire(r.mul[i], "out", r.osc.pitch, kValueParam, WireLaw::Units);
  }
  std::sort(out.wires.begin(), out.wires.end(), [](const Wire& x, const Wire& y) { return x.id < y.id; });
  // The panel: the tabs, the Pitch knob (always shown), each tab's child panels embedded.
  Panel panel;
  panel.width = 316.0f;
  panel.height = 288.0f;
  panel.elements.push_back(plate(panel.width, panel.height, 12.0));
  r.tabs = 2;
  PanelElement tabs = element(r.tabs, "tabs", 8, 8, 232, 18, {});
  tabs.bindings.clear();
  tabs.setField(styleText("items", kSynthTabs));
  panel.elements.push_back(std::move(tabs));
  const std::string vp(kValueParam);
  r.pitchKnob = 3;
  PanelElement pitch = element(r.pitchKnob, "knob", 250, 34, 58, 76, controlBinding(r.osc.pitch, vp));
  pitch.setField(styleText("label", "Pitch"));
  panel.elements.push_back(std::move(pitch));
  auto onTab = [&](PanelElement e, double tab) {
    e.when = Visibility{r.tabs, {tab}};
    panel.elements.push_back(std::move(e));
  };
  uint32_t id = 4;
  onTab(element(id++, "panel", 8, 32, 204, 248, panelBinding(r.wavetable.group)), 0.0);
  onTab(element(id++, "panel", 8, 40, 232, 143, panelBinding(r.envelope.group)), 1.0);
  for (int i = 0; i < 2; ++i) {
    const double tab = 2.0 + i;
    onTab(element(id++, "panel", 8, 40, 104, 104, panelBinding(r.lfo[i].group)), tab);
    onTab(element(id++, "panel", 116, 32, 128, 154, panelBinding(r.lfoShape[i].group)), tab);
    PanelElement depth = element(id++, "knob", 32, 160, 56, 76, controlBinding(r.depth[i], vp));
    depth.setField(styleText("label", "Depth"));
    onTab(std::move(depth), tab);
  }
  {
    auto g = std::make_shared<Device>(*out.devices.front());
    g->panel = std::make_shared<const Panel>(std::move(panel));
    out.devices.front() = std::move(g);
  }
  if (roles != nullptr) *roles = r;
  return out;
}

BlockSet moduleBlocks(ModuleKind k) {
  IdSource ids;
  if (k == ModuleKind::PhaseAccumulator) return phaseAccumulatorBlocks(ids);
  if (k == ModuleKind::Vca) return vcaBlocks(ids);
  if (k == ModuleKind::WavetableSynth) return wavetableSynthBlocks(ids);
  if (k == ModuleKind::Utility) return utilityBlocks(ids);
  if (k == ModuleKind::Saturator) return saturatorBlocks(ids);
  if (k == ModuleKind::Lfo) return lfoBlocks(ids);
  if (k == ModuleKind::Operator) return operatorBlocks(ids);
  return engineModuleBlocks(ids, k);
}

// ---- GU.10: the stock devices as group presets --------------------------------------------------------------------

namespace {

// A signal: a block's output port.
struct Sig {
  DeviceId dev;
  std::string port;
};

// Builds a circuit of atom and value blocks inside one group (Builder above) from expressions: op("mul", {{"a", x},
// {"b", y}}) is one atom block with its inputs wired; k(0.5) a constant (a value block with a flat curve, made once per
// value); val(…) a control.
struct Kit {
  Builder b;
  std::vector<std::pair<double, Sig>> constants;
  Kit(IdSource& ids, DeviceId group) : b{ids, {}, group} {}
  Sig in() const { return {b.group, std::string(kGroupInPort)}; }
  void out(const Sig& s) { b.wire(s.dev, s.port, b.group, kGroupOutPort); }
  // `value` in the curve's units (dB for a gain).
  Sig val(std::string name, const dsp::Curve& c, double value) {
    return {b.value(std::move(name), c, dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)), std::string(kValueOutput)};
  }
  Sig k(double v) {
    for (const auto& [c, s] : constants)
      if (c == v && groupOf(s.dev) == b.group) return s;
    char label[32];
    std::snprintf(label, sizeof label, "%g", v);
    const Sig s{b.value(label, dsp::curvePreset(dsp::CurvePreset::Linear, v, v), 1.0), std::string(kValueOutput)};
    constants.emplace_back(v, s);
    return s;
  }
  DeviceId groupOf(DeviceId d) const {
    for (const DevicePtr& x : b.set.devices)
      if (x->id == d) return x->group;
    return {};
  }
  DeviceId block(std::string_view atom, std::string name, std::initializer_list<std::pair<const char*, Sig>> ins) {
    const DeviceId id = b.atom(atom, std::move(name));
    for (const auto& [port, s] : ins) b.wire(s.dev, s.port, id, port);
    return id;
  }
  Sig op(std::string_view atom, std::string name, std::initializer_list<std::pair<const char*, Sig>> ins,
         const char* outPort = "out") {
    return {block(atom, std::move(name), ins), outPort};
  }
  // A const input of an atom block (a setting).
  void setting(DeviceId id, std::string name, double v) {
    for (DevicePtr& d : b.set.devices)
      if (d->id == id) {
        auto c = std::make_shared<Device>(*d);
        c->settings.push_back(DeviceValue{std::move(name), v});
        std::sort(c->settings.begin(), c->settings.end(), [](const DeviceValue& x, const DeviceValue& y) { return x.name < y.name; });
        d = std::move(c);
      }
  }
  // One of `choices` by the stepped index `index` (0 … n − 1): a chain of two-input muxes (in[clamp(floor(i))]).
  Sig choose(const Sig& index, std::span<const Sig> choices, const std::string& name) {
    Sig r = choices.back();
    for (size_t i = choices.size() - 1; i-- > 0;) {
      const Sig at = i == 0 ? index : op("sub", name + " - " + std::to_string(i), {{"a", index}, {"b", k(static_cast<double>(i))}});
      r = op("mux", name + (i == 0 ? std::string() : " " + std::to_string(i)), {{"index", at}, {"in0", choices[i]}, {"in1", r}});
    }
    return r;
  }
  BlockSet finish(DevicePtr group) {
    BlockSet out;
    out.devices.push_back(std::move(group));
    for (DevicePtr& d : b.set.devices) out.devices.push_back(std::move(d));
    out.wires = std::move(b.set.wires);
    std::sort(out.wires.begin(), out.wires.end(), [](const Wire& x, const Wire& y) { return x.id < y.id; });
    return out;
  }
};

dsp::Curve linear(double min, double max, dsp::CurveUnit unit = dsp::CurveUnit::None) {
  return withUnit(dsp::curvePreset(dsp::CurvePreset::Linear, min, max), unit);
}
dsp::Curve stepped(double count) {  // 0 … count − 1, whole numbers (a menu, a switch)
  dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, count - 1.0);
  c.step = 1.0;
  return c;
}
dsp::Curve octave(double min, double max, dsp::CurveUnit unit = dsp::CurveUnit::None) {
  return withUnit(dsp::curvePreset(dsp::CurvePreset::Octave, min, max), unit);
}
// A gain in dB (the knob's units), the block giving the linear gain.
dsp::Curve gainDb(double min, double max) {
  dsp::Curve c = linear(min, max, dsp::CurveUnit::Db);
  c.output = dsp::CurveOutput::DbToGain;
  return c;
}

// Elements: a control bound to a value block (labelled by the block's name unless a style field says otherwise).
PanelElement control(uint32_t id, const char* kind, float x, float y, float w, float h, const Sig& v) {
  return element(id, kind, x, y, w, h, controlBinding(v.dev, std::string(kValueParam)));
}
PanelElement caption(uint32_t id, float x, float y, float w, float h, const char* s) {
  PanelElement e = element(id, "label", x, y, w, h, {});
  e.bindings.clear();
  e.setField(styleText("text", s));
  e.setField(styleText("align", "left"));
  return e;
}
PanelElement menu(PanelElement e, const char* items) {
  e.setField(styleText("items", items));
  e.setField(styleText("label", "-"));
  return e;
}
PanelElement shownWhen(PanelElement e, uint32_t element, std::vector<double> values) {
  e.when = Visibility{element, std::move(values)};
  return e;
}
PanelElement button(PanelElement e, const char* t) {
  e.setField(styleText("look", "button"));
  e.setField(styleText("text", t));
  return e;
}

}  // namespace

BlockSet utilityBlocks(IdSource& ids, const StockModuleOptions& o) {
  const DeviceId group = o.id.valid() ? o.id : ids.device();
  Kit k(ids, group);
  // The controls (the old device's params; the panel's labels are the blocks' names).
  const Sig chan = k.val("Channel Mode", stepped(4), 0.0);
  const Sig invL = k.val("Phase L", stepped(2), 0.0), invR = k.val("Phase R", stepped(2), 0.0);
  const Sig width = k.val("Width", linear(0.0, 4.0, dsp::CurveUnit::Percent), 1.0);
  const Sig msMode = k.val("Mid/Side Mode", stepped(2), 0.0);
  const Sig ms = k.val("Mid/Side", linear(-1.0, 1.0), 0.0);
  const Sig mono = k.val("Mono", stepped(2), 0.0);
  const Sig balance = k.val("Balance", linear(-1.0, 1.0), 0.0);
  const Sig gain = k.val("Gain", linear(-70.0, 35.0, dsp::CurveUnit::Db), 0.0);
  const Sig mute = k.val("Mute", stepped(2), 0.0);
  // The matrix, at param rate (the old patch's Control region, term for term): channel mode, phase, mid / side
  // (width, the Mid/Side balance, mono), balance, gain and mute.
  const Sig one = k.k(1.0), zero = k.k(0.0), half = k.k(0.5);
  // (Switches are reals 0 / 1 and choices go through muxes: in[floor(index)]. An atom block's bool input sums its wires
  // as reals, so the comparisons and selects of the old patch are written as muxes of real indices here.)
  auto mux = [&](const Sig& index, const Sig& in0, const Sig& in1, std::string n) {
    return k.op("mux", std::move(n), {{"index", index}, {"in0", in0}, {"in1", in1}});
  };
  auto mul = [&](const Sig& a, const Sig& b, std::string n) { return k.op("mul", std::move(n), {{"a", a}, {"b", b}}); };
  auto add = [&](const Sig& a, const Sig& b, std::string n) { return k.op("add", std::move(n), {{"a", a}, {"b", b}}); };
  auto sub = [&](const Sig& a, const Sig& b, std::string n) { return k.op("sub", std::move(n), {{"a", a}, {"b", b}}); };
  // cLL = mux(chan, 1, 1, 0, 0), cLR = 1 − cLL, cRL = mux(chan, 0, 1, 0, 1) = chan mod 2, cRR = 1 − cRL
  const Sig cLL = mux(mul(chan, half, "chan / 2"), one, zero, "cLL"), cLR = sub(one, cLL, "cLR");
  const Sig cRL = k.op("mod", "cRL = chan mod 2", {{"a", chan}, {"b", k.k(2.0)}});
  const Sig cRR = sub(one, cRL, "cRR");
  const Sig pL = mux(invL, one, k.k(-1.0), "pL"), pR = mux(invR, one, k.k(-1.0), "pR");
  const Sig m = mux(mono, mux(msMode, one, k.op("min", "min(1, 1 - ms)", {{"a", one}, {"b", sub(one, ms, "1 - ms")}}), "m (M/S)"), one, "m");
  const Sig s = mux(mono, mux(msMode, width, k.op("min", "min(1, 1 + ms)", {{"a", one}, {"b", add(one, ms, "1 + ms")}}), "s (M/S)"), zero, "s");
  const Sig hm = mul(half, m, "0.5 m"), hs = mul(half, s, "0.5 s");
  const Sig aL = mul(pL, cLL, "pL cLL"), bL = mul(pR, cRL, "pR cRL"), aR = mul(pL, cLR, "pL cLR"), bR = mul(pR, cRR, "pR cRR");
  const Sig mL = mul(hm, add(aL, bL, "pL cLL + pR cRL"), "mL"), mR = mul(hm, add(aR, bR, "pL cLR + pR cRR"), "mR");
  const Sig sL = mul(hs, sub(aL, bL, "pL cLL - pR cRL"), "sL"), sR = mul(hs, sub(aR, bR, "pL cLR - pR cRR"), "sR");
  // −70 dB and below is silence: floor(clip(−69 − gain, 0, 1)) is 1 exactly at the range's −70 dB.
  const Sig floorDb = k.op("clip", "at -70 dB", {{"x", sub(k.k(-69.0), gain, "-69 - gain")}, {"lo", zero}, {"hi", one}});
  const Sig muted = k.op("max", "mute or -70 dB", {{"a", mute}, {"b", floorDb}});
  const Sig g = mux(muted, k.op("db_to_gain", "dB to gain", {{"db", gain}}), zero, "g");
  const Sig oL = mul(g, k.op("min", "min(1, 1 - balance)", {{"a", one}, {"b", sub(one, balance, "1 - balance")}}), "oL");
  const Sig oR = mul(g, k.op("min", "min(1, 1 + balance)", {{"a", one}, {"b", add(one, balance, "1 + balance")}}), "oR");
  // Per sample: out L = aLL·L + aLR·R, out R = aRL·L + aRR·R through smoothed gains (2 ms, from their first value).
  const DeviceId chL = k.block("channel", "L", {{"x", k.in()}}), chR = k.block("channel", "R", {{"x", k.in()}});
  k.setting(chL, "index", 0.0);
  k.setting(chR, "index", 1.0);
  const Sig L{chL, "out"}, R{chR, "out"};
  auto smoothed = [&](const Sig& gx, const Sig& gy, const char* n) {
    const DeviceId a = k.block("gain", std::string(n) + " L", {{"x", L}, {"g", gx}});
    k.setting(a, "start", 1.0);
    const DeviceId b = k.block("gain_add", std::string(n) + " + R", {{"acc", Sig{a, "out"}}, {"x", R}, {"g", gy}});
    k.setting(b, "start", 1.0);
    return Sig{b, "out"};
  };
  const Sig outL = smoothed(mul(oL, add(mL, sL, "mL + sL"), "aLL"), mul(oL, add(mR, sR, "mR + sR"), "aLR"), "out L =");
  const Sig outR = smoothed(mul(oR, sub(mL, sL, "mL - sL"), "aRL"), mul(oR, sub(mR, sR, "mR - sR"), "aRR"), "out R =");
  k.out(k.op("pack", "(L, R)", {{"in0", outL}, {"in1", outR}}));
  // The panel: the old face (retired/stdlib/devices/utility.panel.json) at its places, 8 px in from the plate.
  Panel p;
  p.width = 196.0f;
  p.height = 158.0f;
  p.elements.push_back(plate(p.width, p.height, 6.0));
  p.elements.push_back(caption(2, 8, 8, 60, 14, "Channel"));
  p.elements.push_back(menu(control(3, "menu", 8, 22, 60, 18, chan), "Stereo|Left|Right|Swap"));
  p.elements.push_back(button(control(4, "toggle", 8, 48, 28, 18, invL), "\xC3\x98 L"));
  p.elements.push_back(button(control(5, "toggle", 40, 48, 28, 18, invR), "\xC3\x98 R"));
  p.elements.push_back(button(control(6, "toggle", 8, 74, 60, 18, mono), "Mono"));
  p.elements.push_back(button(control(7, "toggle", 8, 96, 60, 18, msMode), "Mid/Side"));
  p.elements.push_back(shownWhen(control(8, "knob", 72, 8, 56, 58, width), 7, {0.0}));
  p.elements.push_back(shownWhen(control(9, "knob", 72, 8, 56, 58, ms), 7, {1.0}));
  p.elements.push_back(control(10, "knob", 72, 92, 56, 58, balance));
  p.elements.push_back(control(11, "knob", 132, 8, 56, 58, gain));
  p.elements.push_back(button(control(12, "toggle", 132, 92, 56, 18, mute), "Mute"));
  return k.finish(groupDevice(group, o.inGroup, o.name.empty() ? std::string("Utility") : o.name, o.active, std::move(p)));
}

BlockSet saturatorBlocks(IdSource& ids, const StockModuleOptions& o) {
  const DeviceId group = o.id.valid() ? o.id : ids.device();
  Kit k(ids, group);
  const Sig drive = k.val("Drive", gainDb(-36.0, 36.0), 0.0);
  const Sig type = k.val("Type", stepped(7), 0.0);
  const Sig output = k.val("Output", gainDb(-36.0, 36.0), 0.0);
  const Sig softClip = k.val("Soft Clip", stepped(2), 0.0);
  const Sig dryWet = k.val("Dry/Wet", linear(0.0, 1.0, dsp::CurveUnit::Percent), 1.0);
  // u = drive · x (smoothed, from its first value)
  const DeviceId driven = k.block("gain", "x drive", {{"x", k.in()}, {"g", drive}});
  k.setting(driven, "start", 1.0);
  const Sig u{driven, "out"};
  // The curves (the old sat_shape, atom for atom; Waveshaper and Shaper not rebuilt), one nested group each: its In is
  // u, its Out the curve.
  std::vector<Sig> curves;
  auto curve = [&](const char* name, auto&& body) {
    const DeviceId sub = ids.device();
    auto g = std::make_shared<Device>(*makeGroup(sub));
    g->name = name;
    g->group = group;
    k.b.set.devices.push_back(std::move(g));
    k.b.wire(u.dev, u.port, sub, kMainPort);  // (its main input)
    const DeviceId outer = k.b.group;
    k.b.group = sub;
    const Sig y = body(Sig{sub, std::string(kGroupInPort)});
    k.b.wire(y.dev, y.port, sub, kGroupOutPort);
    k.b.group = outer;
    curves.push_back(Sig{sub, std::string(kMainPort)});
  };
  curve("Analog Clip", [&](const Sig& v) {
    const Sig c = k.op("clip", "clip 1.5", {{"x", v}, {"lo", k.k(-1.5)}, {"hi", k.k(1.5)}});
    const Sig c3 = k.op("mul", "c^3", {{"a", k.op("mul", "c^2", {{"a", c}, {"b", c}})}, {"b", c}});
    return k.op("sub", "c - 4/27 c^3", {{"a", c}, {"b", k.op("mul", "4/27 c^3", {{"a", k.k(0.14814814814814814)}, {"b", c3}})}});
  });
  curve("Soft Sine", [&](const Sig& v) {
    const Sig t = k.op("mul", "/ 2pi", {{"a", v}, {"b", k.k(0.15915494309189535)}});
    return k.op("sine", "sine", {{"phase", k.op("clip", "clip 1/4", {{"x", t}, {"lo", k.k(-0.25)}, {"hi", k.k(0.25)}})}});
  });
  curve("Bass Shaper", [&](const Sig& v) {
    const Sig av = k.op("abs", "|v|", {{"x", v}});
    const Sig num = k.op("mul", "v (|v| + 2)", {{"a", v}, {"b", k.op("add", "|v| + 2", {{"a", av}, {"b", k.k(2.0)}})}});
    const Sig den = k.op("add", "v^2 + |v| + 1", {{"a", k.op("add", "v^2 + |v|", {{"a", k.op("mul", "v^2", {{"a", v}, {"b", v}})}, {"b", av}})}, {"b", k.k(1.0)}});
    return k.op("div", "/", {{"a", num}, {"b", den}});
  });
  curve("Medium Curve", [&](const Sig& v) { return k.op("fast_tanh", "tanh", {{"x", v}}); });
  curve("Hard Curve", [&](const Sig& v) {
    const Sig v2 = k.op("mul", "v^2", {{"a", v}, {"b", v}});
    const Sig v4 = k.op("mul", "v^4", {{"a", v2}, {"b", v2}});
    const Sig r = k.op("sqrt", "4th root", {{"x", k.op("sqrt", "sqrt", {{"x", k.op("add", "1 + v^4", {{"a", k.k(1.0)}, {"b", v4}})}})}});
    return k.op("div", "v /", {{"a", v}, {"b", r}});
  });
  curve("Sinoid Fold", [&](const Sig& v) { return k.op("sine", "sine", {{"phase", k.op("mul", "/ 2pi", {{"a", v}, {"b", k.k(0.15915494309189535)}})}}); });
  curve("Digital Clip", [&](const Sig& v) { return k.op("clip", "clip 1", {{"x", v}, {"lo", k.k(-1.0)}, {"hi", k.k(1.0)}}); });
  const Sig c = k.choose(type, curves, "type");
  const DeviceId outGain = k.block("gain", "x output", {{"x", c}, {"g", output}});
  k.setting(outGain, "start", 1.0);
  const Sig gOut{outGain, "out"};
  const Sig wet = k.op("mux", "soft clip?", {{"index", softClip}, {"in0", gOut}, {"in1", k.op("fast_tanh", "tanh", {{"x", gOut}})}});
  // out = x + dryWet·(wet − x), the dry / wet a smoothed gain_add
  const DeviceId mix = k.block("gain_add", "dry/wet", {{"acc", k.in()}, {"x", k.op("sub", "wet - x", {{"a", wet}, {"b", k.in()}})}, {"g", dryWet}});
  k.setting(mix, "start", 1.0);
  k.out(Sig{mix, "out"});
  // The panel: the old face's left half (Drive, Type, Soft Clip, Output, Dry/Wet), 8 px in from the plate.
  Panel p;
  p.width = 172.0f;
  p.height = 144.0f;
  p.elements.push_back(plate(p.width, p.height, 6.0));
  p.elements.push_back(control(2, "knob", 8, 20, 60, 100, drive));
  p.elements.push_back(caption(3, 76, 8, 88, 14, "Type"));
  p.elements.push_back(menu(control(4, "menu", 76, 22, 88, 18, type),
                            "Analog Clip|Soft Sine|Bass Shaper|Medium Curve|Hard Curve|Sinoid Fold|Digital Clip"));
  p.elements.push_back(button(control(5, "toggle", 76, 48, 88, 18, softClip), "Soft Clip"));
  p.elements.push_back(control(6, "knob", 76, 76, 42, 60, output));
  p.elements.push_back(control(7, "knob", 122, 76, 42, 60, dryWet));
  return k.finish(groupDevice(group, o.inGroup, o.name.empty() ? std::string("Saturator") : o.name, o.active, std::move(p)));
}

BlockSet lfoBlocks(IdSource& ids, const StockModuleOptions& o) {
  const DeviceId group = o.id.valid() ? o.id : ids.device();
  Kit k(ids, group);
  const Sig shape = k.val("Shape", stepped(5), 0.0);
  const Sig sync = k.val("Sync", stepped(2), 0.0);
  const Sig rate = k.val("Rate", octave(0.01, 40.0, dsp::CurveUnit::Hz), 1.0);
  const Sig beats = k.val("Beats", octave(1.0 / 16.0, 64.0), 1.0);
  const Sig phase = k.val("Phase", linear(0.0, 1.0), 0.0);
  const Sig depth = k.val("Depth", linear(0.0, 1.0, dsp::CurveUnit::Percent), 1.0);
  const Sig offset = k.val("Offset", linear(-1.0, 1.0), 0.0);
  // stdlib/control/lfo.pqp, atom for atom: free = phasor(rate); song = frac(beat / beats); ph = frac((sync ? song :
  // free) + phase); the waves; then offset + depth · wave.
  const Sig free = k.op("phasor", "phasor", {{"hz", rate}});
  const Sig beat = k.op("transport.beat", "beat", {});
  const Sig song = k.op("frac", "song frac", {{"x", k.op("div", "beat / beats", {{"a", beat}, {"b", beats}})}});
  const Sig synced = k.op("mux", "sync?", {{"index", sync}, {"in0", free}, {"in1", song}});
  const Sig ph = k.op("frac", "phase", {{"x", k.op("add", "+ phase", {{"a", synced}, {"b", phase}})}});
  const Sig sine = k.op("sine", "Sine", {{"phase", ph}});
  const Sig q = k.op("frac", "frac", {{"x", k.op("add", "+ 0.75", {{"a", ph}, {"b", k.k(0.75)}})}});
  const Sig tri4 = k.op("mul", "4 |.|", {{"a", k.k(4.0)}, {"b", k.op("abs", "|.|", {{"x", k.op("sub", "- 0.5", {{"a", q}, {"b", k.k(0.5)}})}})}});
  const Sig tri = k.op("sub", "Triangle", {{"a", tri4}, {"b", k.k(1.0)}});
  const Sig h = k.op("frac", "frac", {{"x", k.op("add", "+ 0.5", {{"a", ph}, {"b", k.k(0.5)}})}});
  const Sig saw = k.op("sub", "Saw Up", {{"a", k.op("mul", "2 .", {{"a", k.k(2.0)}, {"b", h}})}, {"b", k.k(1.0)}});
  const Sig down = k.op("neg", "Saw Down", {{"x", saw}});
  const Sig square = k.op("mux", "Square", {{"index", k.op("mul", "2 ph", {{"a", ph}, {"b", k.k(2.0)}})}, {"in0", k.k(1.0)}, {"in1", k.k(-1.0)}});
  const std::array<Sig, 5> waves = {sine, tri, saw, down, square};
  const Sig w = k.choose(shape, waves, "shape");
  const Sig lfo = k.op("add", "offset +", {{"a", offset}, {"b", k.op("mul", "depth x", {{"a", depth}, {"b", w}})}});
  k.out(lfo);
  // The panel: the old face (retired/stdlib/devices/lfo_device.panel.json) at its places, 8 px in, and its jack (a mod
  // source: drag a cable from it onto any control).
  Panel p;
  p.width = 236.0f;
  p.height = 140.0f;
  p.elements.push_back(plate(p.width, p.height, 6.0));
  p.elements.push_back(caption(2, 8, 8, 72, 14, "Shape"));
  p.elements.push_back(menu(control(3, "menu", 8, 22, 72, 18, shape), "Sine|Triangle|Saw Up|Saw Down|Square"));
  p.elements.push_back(button(control(4, "toggle", 8, 48, 72, 18, sync), "Sync"));
  p.elements.push_back(shownWhen(control(5, "knob", 84, 8, 56, 58, rate), 4, {0.0}));
  p.elements.push_back(shownWhen(control(6, "knob", 84, 8, 56, 58, beats), 4, {1.0}));
  p.elements.push_back(control(7, "knob", 84, 74, 56, 58, phase));
  p.elements.push_back(control(8, "knob", 144, 8, 56, 58, depth));
  p.elements.push_back(control(9, "knob", 144, 74, 56, 58, offset));
  p.elements.push_back(element(10, "modsource", 206, 54, 24, 32, signalBinding(lfo.dev, lfo.port)));
  return k.finish(groupDevice(group, o.inGroup, o.name.empty() ? std::string("LFO") : o.name, o.active, std::move(p)));
}

namespace {

// A meta engine's param position for `value` (its declared curve).
double enginePosition(std::string_view type, std::string_view param, double value) {
  const meta::Type* t = meta::findType(type);
  const int32_t i = t != nullptr ? t->paramIndex(param) : -1;
  if (i < 0) return 0.0;
  return dsp::curvePosition(meta::paramCurve(t->params[static_cast<size_t>(i)]), value, 0.0);
}
void setParam(BlockSet& set, DeviceId id, std::string name, double position) {
  for (DevicePtr& d : set.devices)
    if (d->id == id) {
      auto c = std::make_shared<Device>(*d);
      std::erase_if(c->params, [&](const DeviceValue& v) { return v.name == name; });
      c->params.push_back(DeviceValue{std::move(name), position});
      std::sort(c->params.begin(), c->params.end(), [](const DeviceValue& x, const DeviceValue& y) { return x.name < y.name; });
      d = std::move(c);
    }
}
void append(BlockSet& to, BlockSet&& from) {
  for (DevicePtr& d : from.devices) to.devices.push_back(std::move(d));
  for (Wire& w : from.wires) to.wires.push_back(std::move(w));
}
void sortWires(BlockSet& s) {
  std::sort(s.wires.begin(), s.wires.end(), [](const Wire& x, const Wire& y) { return x.id < y.id; });
}

// One voice: In → [× Coarse] → Phase Accumulator → Wavetable (its default table, a sine) → Envelope (Amount = `level`)
// → Out, inside group `group` (an operator of Operator, or the default instrument).
struct VoiceRoles {
  DeviceId coarse, pa, pitch, phase, pm, wavetable, wavetableGroup, envelope, envelopeGroup;
};
BlockSet voiceChain(IdSource& ids, DeviceId group, bool coarse, double level, VoiceRoles& r, std::string_view suffix = {},
                    bool toOut = true, bool pm = false) {
  Kit k(ids, group);
  const std::string sfx = suffix.empty() ? std::string() : " " + std::string(suffix);
  Sig feed = k.in();
  if (coarse) {
    dsp::Curve cv = dsp::curvePreset(dsp::CurvePreset::Linear, 1.0, 16.0);
    cv.step = 1.0;
    cv.digits = 0;
    const Sig c = k.val("Coarse" + sfx, cv, 1.0);
    r.coarse = c.dev;
    feed = k.op("mul", "x coarse" + sfx, {{"a", feed}, {"b", c}});
  }
  PhaseAccumulatorRoles pr;
  PhaseAccumulatorOptions po;
  po.inGroup = group;
  po.pm = pm;  // (the Phase Accumulator's phase-modulation add: its `b` takes the modulator)
  if (!sfx.empty()) po.name = "Phase Accumulator" + sfx;
  BlockSet pa = phaseAccumulatorBlocks(ids, po, &pr);
  r.pa = pr.group;
  r.pitch = pr.pitch;
  r.phase = pr.phase;
  r.pm = pr.pm;
  EngineRoles wr, er;
  EngineModuleOptions eo;
  eo.inGroup = group;
  if (!sfx.empty()) eo.name = "Wavetable" + sfx;
  BlockSet wt = engineModuleBlocks(ids, ModuleKind::Wavetable, eo, &wr);
  if (!sfx.empty()) eo.name = "Envelope" + sfx;
  BlockSet env = engineModuleBlocks(ids, ModuleKind::Envelope, eo, &er);
  r.wavetable = wr.engine;
  r.wavetableGroup = wr.group;
  r.envelope = er.engine;
  r.envelopeGroup = er.group;
  setParam(env, er.engine, "amount", enginePosition("envelope", "amount", level));
  k.b.wire(feed.dev, feed.port, pr.group, kMainPort);  // (groups: their main input and output)
  k.b.wire(pr.group, kMainPort, wr.group, kMainPort);
  k.b.wire(wr.group, kMainPort, er.group, kMainPort);
  if (toOut) k.b.wire(er.group, kMainPort, group, kGroupOutPort);
  BlockSet out;
  for (DevicePtr& d : k.b.set.devices) out.devices.push_back(std::move(d));
  for (Wire& w : k.b.set.wires) out.wires.push_back(std::move(w));
  append(out, std::move(pa));
  append(out, std::move(wt));
  append(out, std::move(env));
  return out;
}

DevicePtr instrumentGroup(DeviceId id, const StockModuleOptions& o, std::string_view name) {
  auto g = std::make_shared<Device>(*makeInstrumentGroup(id));
  g->name = o.name.empty() ? std::string(name) : o.name;
  g->group = o.inGroup;
  g->active = o.active;
  return g;
}

}  // namespace

BlockSet operatorBlocks(IdSource& ids, const StockModuleOptions& o) {
  const DeviceId group = o.id.valid() ? o.id : ids.device();
  BlockSet out;
  out.devices.push_back(instrumentGroup(group, o, "Operator"));
  // Four operators A … D, each a row of blocks in the group: In (the note's Hz) → × Coarse → Phase Accumulator (its Pitch
  // = the old Fine) → Wavetable (its default table: a sine) → Envelope (its Amount = the operator's level). The FM matrix
  // is wires: the old device's algorithm 1, D → C → B → A, each modulator's Envelope into the next one's Phase
  // Accumulator Phase (phase modulation in cycles: the Phase control wraps); only A's Envelope reaches Out (and ends the
  // voices). B, C and D start at level 0 (the old default: A alone). (Operators as nested groups would read better, but
  // a cable into a nested group on the path to Out keeps the lowering from finding the Envelope that ends the voices:
  // QUESTIONS Q?-GU.10-a.)
  constexpr std::array<const char*, 4> kOps = {"A", "B", "C", "D"};
  std::array<VoiceRoles, 4> roles{};
  for (size_t i = 0; i < 4; ++i) append(out, voiceChain(ids, group, true, i == 0 ? 1.0 : 0.0, roles[i], kOps[i], i == 0, i < 3));
  Builder b{ids, {}, group};
  for (size_t i = 3; i > 0; --i) b.wire(roles[i].envelopeGroup, kMainPort, roles[i - 1].pm, "b");
  for (Wire& w : b.set.wires) out.wires.push_back(std::move(w));
  sortWires(out);
  // The panel: the old face's oscillator rows (A … D: Coarse, Fine, Level) and its envelope display under the A … D
  // tabs (each operator's Envelope panel embedded, shown under its tab).
  Panel p;
  p.width = 470.0f;
  p.height = 172.0f;
  p.elements.push_back(plate(p.width, p.height, 6.0));
  uint32_t id = 2;
  constexpr std::array<std::pair<const char*, float>, 3> kHeads = {{{"Coarse", 34.0f}, {"Fine", 88.0f}, {"Level", 142.0f}}};
  for (const auto& [h, x] : kHeads) {
    PanelElement e = caption(id++, x, 8, 50, 12, h);
    e.setField(styleText("align", "centre"));
    p.elements.push_back(std::move(e));
  }
  for (size_t i = 0; i < 4; ++i) {
    const float y = 26.0f + 36.0f * static_cast<float>(i);
    PanelElement name = caption(id++, 10, y + 4, 18, 16, kOps[i]);
    name.setField(styleNumber("size", 11.0));
    p.elements.push_back(std::move(name));
    const std::array<Binding, 3> bound = {controlBinding(roles[i].coarse, std::string(kValueParam)),
                                          controlBinding(roles[i].pitch, std::string(kValueParam)),
                                          controlBinding(roles[i].envelope, "amount")};
    for (size_t c = 0; c < 3; ++c) {
      PanelElement e = element(id++, "number", kHeads[c].second, y + 2, 50, 20, bound[c]);
      e.setField(styleText("label", "-"));
      p.elements.push_back(std::move(e));
    }
  }
  const uint32_t tabs = id++;
  PanelElement t = element(tabs, "tabs", 204, 8, 258, 16, {});
  t.bindings.clear();
  t.setField(styleText("items", "A|B|C|D"));
  p.elements.push_back(std::move(t));
  for (size_t i = 0; i < 4; ++i)
    p.elements.push_back(shownWhen(element(id++, "panel", 204, 30, 258, 136, panelBinding(roles[i].envelopeGroup)), tabs,
                                   {static_cast<double>(i)}));
  auto g = std::make_shared<Device>(*out.devices.front());
  g->panel = std::make_shared<const Panel>(std::move(p));
  out.devices.front() = std::move(g);
  return out;
}

BlockSet defaultInstrumentBlocks(IdSource& ids, const StockModuleOptions& o) {
  const DeviceId group = o.id.valid() ? o.id : ids.device();
  BlockSet out;
  out.devices.push_back(instrumentGroup(group, o, kDefaultInstrumentName));
  // In (the note's Hz) → Phase Accumulator → Wavetable (a sine) → Envelope (level 0.25: a few voices on a few tracks
  // stay below 0 dBFS, as Basic Synth's 0.2 did) → Out. The Envelope ends the voices.
  VoiceRoles r;
  append(out, voiceChain(ids, group, false, 0.25, r));
  sortWires(out);
  return out;
}

// ==== Load conversion =================================================================================================

namespace {

// A group's chain ports by name (groupPatchText: `in x`, `out y`; edit_groups chainPortsOf, what Ctrl+G's links name).
constexpr std::string_view kGroupChainIn = "x";
constexpr std::string_view kGroupChainOut = "y";

bool retired(const Device& d) noexcept {
  return isMetaDevice(d) && meta::findType(metaDeviceType(d)) == nullptr && meta::findRetiredType(metaDeviceType(d)) != nullptr;
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
  if (d.curve("pitch") == nullptr) {
    // The module's Pitch curve is not the retired type's (GU.5b's arc scale): keep the pitch in cents, not the position.
    const int32_t i = t.paramIndex("pitch");
    if (i >= 0) {
      const double cents = dsp::evalCurve(meta::paramCurve(t.params[static_cast<size_t>(i)]), o.pitch);
      o.pitch = dsp::curvePosition(phaseAccumulatorCurve(kPaPitch), cents, o.pitch);
    }
  }
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
      if (retired(*d)) any = true;
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
  forEachTrack(p, [&](const Track& t, const Track*) {
    bool changed = false;
    std::vector<DevicePtr> list;
    for (const DevicePtr& d : t.devices) {
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
