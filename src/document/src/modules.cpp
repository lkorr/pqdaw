// GU.4: the built-in modules (group presets): pq/document/modules.h.
#include "pq/document/modules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pq/document/panel.h"
#include "pq/meta/noise.h"
#include "pq/meta/registry.h"

namespace pq::doc {

namespace {

constexpr std::array<ModuleInfo, 16> kModules = {{
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
    // EQ.3: the filter / EQ (docs/plans/EQ.md).
    {ModuleKind::EqBand, "eq_band", "EQ Band",
     "One filter band: a state-variable filter (bell, cuts, shelves, band pass, notch, all pass) with frequency, Q, gain "
     "and up to 64 stages. Open it to rebuild the circuit; an EQ's chains are made of these."},
    {ModuleKind::Eq, "eq", "EQ",
     "A parametric EQ: bands in series (four to start), stereo, left/right or mid/side with a second chain, and an "
     "output gain."},
    // DT.1 (docs/plans/DT.md).
    {ModuleKind::DelayTap, "delay_tap", "Delay Tap",
     "Delays its input by a time (0 … 5 s, free in ms or synced to a note value at the tempo), nothing else: no dry, no "
     "feedback. Feed its output back into its input through a VCA for echoes."},
    // NZ.1: every kind of randomness (docs/plans/NZ.md).
    {ModuleKind::Noise, "noise", "Noise",
     "Noise and random modulation in one: a value drawn per step at a rate (its top: every sample, plain noise), on the "
     "song's grid, per note or by a clock into its In; coloured (brown, pink, white, blue, violet: a random walk when "
     "slow), sparse (density), stepped, glided (smooth), in a range. On play it repeats every playback."},
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
    d->defaultPosition = d->params.front().value;  // TX.reset-default: the module's default
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
  // The old Phase accumulator meta module's declared curves, written out.
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
  if (control == kPaAmplitude) {
    // TX.detent: ×1 at the centre of the travel, a detent: 0 … 1 over the first half, 1 … 4 over the second, both
    // straight (a piecewise-linear curve: a wire still adds in amplitude). The point's value is exactly 1 (0.25 · 4).
    dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 4.0);
    c.segments = 2;
    c.points[0] = {0.0, 0.0};
    c.points[1] = {0.5, 0.25, true};
    c.points[2] = {1.0, 1.0};
    c.shapes[0] = c.shapes[1] = {dsp::CurveShape::Linear, 0.0};
    c.preset = dsp::CurvePreset::Custom;
    return c;
  }
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
    (void)dsp::setCurveDetent(c, 0.5);  // TX.detent: 0 cents (the middle segment split there: still straight)
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
  b.wire(r.addPhase, "out", r.frac, "x");
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
    case ModuleKind::Noise: return "noise";
    case ModuleKind::PhaseAccumulator:
    case ModuleKind::Vca:
    case ModuleKind::WavetableSynth:
    case ModuleKind::Utility:
    case ModuleKind::Saturator:
    case ModuleKind::Lfo:
    case ModuleKind::Operator:
    case ModuleKind::EqBand:
    case ModuleKind::Eq:
    case ModuleKind::DelayTap: break;
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
    case ModuleKind::Noise: {  // NZ.1: 236 × 248 — the display, the clock / distribution chips, eight knobs, the
                               // restart / voices chips (pq/document/modules.h "Noise")
      p.width = 236.0f;
      p.height = 248.0f;
      p.elements.push_back(plate(p.width, p.height, 10.0));
      p.elements.push_back(rich(2, "noise", 8, 8, 220, 44));
      auto caption = [&](uint32_t id, float x, float y, float w, const char* text) {
        PanelElement e = element(id, "label", x, y, w, 10, {});
        e.bindings.clear();
        e.setField(styleText("text", text));
        e.setField(styleText("align", "left"));
        p.elements.push_back(std::move(e));
      };
      auto chips = [&](uint32_t id, float x, float y, float w, const char* setting, const char* items) {
        PanelElement e = element(id, "tabs", x, y, w, 16, settingBinding(engine, setting));
        e.setField(styleText("items", items));
        p.elements.push_back(std::move(e));
      };
      auto knob = [&](uint32_t id, float x, float y, const char* param) {
        p.elements.push_back(element(id, "knob", x, y, 52, 58, controlBinding(engine, param)));
      };
      caption(3, 8, 56, 112, "Clock");
      chips(4, 8, 68, 112, "clock", "Rate|Sync|Note");
      caption(5, 124, 56, 104, "Distribution");
      chips(6, 124, 68, 104, "dist", "Uniform|Gauss");
      PanelElement rate = element(7, "knob", 8, 90, 52, 58, controlBinding(engine, "rate"));
      rate.when = Visibility{4, {0.0}};  // Rate clock
      p.elements.push_back(std::move(rate));
      PanelElement sync = element(8, "menu", 8, 112, 52, 18, controlBinding(engine, "sync"));  // Sync clock: a note value
      sync.setField(styleText("items", meta::kNoiseSyncItems));
      sync.when = Visibility{4, {1.0}};
      p.elements.push_back(std::move(sync));
      caption(20, 8, 98, 52, "Sync");  // above the menu, where the Rate knob's name is
      p.elements.back().setField(styleText("align", "centre"));
      p.elements.back().setField(styleNumber("size", 7.5));  // a knob's name's size
      p.elements.back().when = Visibility{4, {1.0}};
      knob(9, 64, 90, "smooth");
      knob(10, 120, 90, "colour");
      knob(11, 176, 90, "density");
      knob(12, 8, 150, "steps");
      knob(13, 64, 150, "min");
      knob(14, 120, 150, "max");
      knob(15, 176, 150, "seed");
      caption(16, 8, 212, 120, "Restart");
      chips(17, 8, 224, 120, "restart", "Play|Free|Note");
      caption(18, 132, 212, 96, "Voices");
      chips(19, 132, 224, 96, "voices", "Voice|Shared");
      break;
    }
    case ModuleKind::PhaseAccumulator:
    case ModuleKind::Vca:
    case ModuleKind::WavetableSynth:
    case ModuleKind::Utility:
    case ModuleKind::Saturator:
    case ModuleKind::Lfo:
    case ModuleKind::Operator:
    case ModuleKind::EqBand:
    case ModuleKind::Eq:
    case ModuleKind::DelayTap: break;
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
  if (k == ModuleKind::EqBand) return eqBandBlocks(ids);
  if (k == ModuleKind::Eq) return eqBlocks(ids);
  if (k == ModuleKind::DelayTap) return delayTapBlocks(ids);
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

// TX.detent: `c` with a detent where it has `value` (in its units: dB for a gain), its neutral value.
dsp::Curve detent(dsp::Curve c, double value) {
  (void)dsp::setCurveDetent(c, dsp::curvePosition(c, dsp::curveOutput(c, value), 0.5));
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
  const Sig width = k.val("Width", detent(linear(0.0, 4.0, dsp::CurveUnit::Percent), 1.0), 1.0);
  const Sig msMode = k.val("Mid/Side Mode", stepped(2), 0.0);
  const Sig ms = k.val("Mid/Side", detent(linear(-1.0, 1.0), 0.0), 0.0);
  const Sig mono = k.val("Mono", stepped(2), 0.0);
  const Sig balance = k.val("Balance", detent(linear(-1.0, 1.0), 0.0), 0.0);
  const Sig gain = k.val("Gain", detent(linear(-70.0, 35.0, dsp::CurveUnit::Db), 0.0), 0.0);
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
  const Sig drive = k.val("Drive", detent(gainDb(-36.0, 36.0), 0.0), 0.0);
  const Sig type = k.val("Type", stepped(7), 0.0);
  const Sig output = k.val("Output", detent(gainDb(-36.0, 36.0), 0.0), 0.0);
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
  const Sig offset = k.val("Offset", detent(linear(-1.0, 1.0), 0.0), 0.0);
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
  DeviceId coarse, pa, pitch, phase, wavetable, wavetableGroup, envelope, envelopeGroup;
};
BlockSet voiceChain(IdSource& ids, DeviceId group, bool coarse, double level, VoiceRoles& r, std::string_view suffix = {},
                    bool toOut = true) {
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
  if (!sfx.empty()) po.name = "Phase Accumulator" + sfx;
  BlockSet pa = phaseAccumulatorBlocks(ids, po, &pr);
  r.pa = pr.group;
  r.pitch = pr.pitch;
  r.phase = pr.phase;
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
  for (size_t i = 0; i < 4; ++i) append(out, voiceChain(ids, group, true, i == 0 ? 1.0 : 0.0, roles[i], kOps[i], i == 0));
  Builder b{ids, {}, group};
  // GU.f7: a plain per-voice cable into the Phase control (GU.f5: a modulation tap never ends a voice).
  for (size_t i = 3; i > 0; --i) b.wire(roles[i].envelopeGroup, kMainPort, roles[i - 1].phase, std::string(kValueParam));
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

// ---- EQ.3: the band and the EQ (modules.h; docs/plans/EQ.md "Structure") ------------------------------------------

namespace {

double settingOf(const Device& d, std::string_view name) noexcept {
  const DeviceValue* v = d.setting(name);
  return v != nullptr ? v->value : 0.0;
}

// The panel's tags (a style text "eq"): what layEqStrip finds or replaces.
constexpr const char* kEqTag = "eq";
constexpr std::string_view kTagStrip = "strip", kTagChains = "chains", kTagDisplay = "display";

// EQ.4b: a band's Q box reads "Q 0.71", its Stages box "×16" (the number element's format fields).
PanelElement qBox(PanelElement e) {
  e.setField(styleNumber("decimals", 2.0));
  e.setField(styleText("prefix", "Q "));
  return e;
}
PanelElement stagesBox(PanelElement e) {
  e.setField(styleNumber("decimals", 0.0));
  e.setField(styleText("prefix", "\xC3\x97"));
  return e;
}

PanelElement tagged(PanelElement e, std::string_view tag) {
  e.setField(styleText(kEqTag, std::string(tag)));
  return e;
}

// The panel's geometry (world units): the response display (EQ.4d: its magnitude, phase and delay areas stacked in it),
// the controls on the left and the band strip.
constexpr float kResponseY = 8.0f, kResponseH = 198.0f;
constexpr float kStripX = 88.0f, kColumnStep = 60.0f, kColumnW = 56.0f, kStripTop = 220.0f, kEqHeight = kStripTop + 164.0f;
constexpr uint32_t kMinColumns = 6;

void addRole(Kit& k, const Sig& v, std::string_view setting, double role) { k.setting(v.dev, std::string(setting), role); }

BandInfo bandOfSet(const BlockSet& b) {
  BandInfo info;
  info.group = b.devices.front()->id;
  for (const DevicePtr& dv : b.devices) {
    switch (bandRole(*dv)) {
      case BandRole::Frequency: info.frequency = dv->id; break;
      case BandRole::Q: info.q = dv->id; break;
      case BandRole::Gain: info.gain = dv->id; break;
      case BandRole::Stages: info.stages = dv->id; break;
      case BandRole::Type: info.type = dv->id; break;
      case BandRole::None: break;
    }
  }
  return info;
}

}  // namespace

BandRole bandRole(const Device& d) noexcept {
  const double v = settingOf(d, kBandRoleSetting);
  return v >= 1.0 && v <= 5.0 && v == std::floor(v) ? static_cast<BandRole>(static_cast<uint8_t>(v)) : BandRole::None;
}
EqRole eqRole(const Device& d) noexcept {
  const double v = settingOf(d, kEqRoleSetting);
  return v >= 1.0 && v <= 3.0 && v == std::floor(v) ? static_cast<EqRole>(static_cast<uint8_t>(v)) : EqRole::None;
}
uint32_t bandChainOf(const Device& d) noexcept {
  if (!isGroupDevice(d)) return 0;
  const double v = settingOf(d, kBandChainSetting);
  return v == 1.0 ? 1u : v == 2.0 ? 2u : 0u;
}
bool bandTypeHasGain(uint32_t type) noexcept { return type == 4 || type == 5 || type == 6; }
bool bandTypeHasStages(uint32_t type) noexcept { return type != 3 && type != 4 && type <= 7; }

dsp::Curve bandFrequencyCurve() {
  // One Exp segment from 0 of bend ln(1 + 20 kHz / 10 Hz): f = 20 kHz · (2001^t − 1) / 2000 = 10 Hz · (2001^t − 1), so
  // the knob travel is the display's x = log(1 + f / 10 Hz) / log(2001) exactly.
  return withUnit(dsp::curvePreset(dsp::CurvePreset::Exponential, 0.0, kBandMaxHz, std::log(1.0 + kBandMaxHz / kBandAxisHz)),
                  dsp::CurveUnit::Hz);
}

double valueBlockUnits(const Device& v) noexcept {
  const dsp::Curve* c = v.curve(kValueParam);
  const DeviceValue* pos = v.param(kValueParam);
  if (c == nullptr) return pos != nullptr ? pos->value : 0.0;
  const double y = dsp::evalCurve(*c, pos != nullptr ? pos->value : 0.0);
  if (c->output == dsp::CurveOutput::DbToGain) return y > 0.0 ? 20.0 * std::log10(y) : c->min;
  return y;
}

std::optional<BandInfo> bandInfo(const Project& p, DeviceId band) {
  const DeviceLoc loc = findDevice(p, band);
  if (loc.device == nullptr || !isGroupDevice(*loc.device)) return std::nullopt;
  BandInfo b;
  b.group = band;
  for (const DevicePtr& d : loc.list()) {
    if (d->group != band || !isValueDevice(*d)) continue;
    switch (bandRole(*d)) {
      case BandRole::Frequency: b.frequency = d->id; break;
      case BandRole::Q: b.q = d->id; break;
      case BandRole::Gain: b.gain = d->id; break;
      case BandRole::Stages: b.stages = d->id; break;
      case BandRole::Type: b.type = d->id; break;
      case BandRole::None: break;
    }
  }
  if (!b.frequency.valid()) return std::nullopt;
  return b;
}

std::optional<EqInfo> eqInfo(const Project& p, DeviceId eq) {
  const DeviceLoc loc = findDevice(p, eq);
  if (loc.device == nullptr || !isGroupDevice(*loc.device)) return std::nullopt;
  EqInfo e;
  e.group = eq;
  const std::vector<DevicePtr>& list = loc.list();
  for (const DevicePtr& d : list) {
    if (d->group != eq) continue;
    if (isValueDevice(*d)) {
      switch (eqRole(*d)) {
        case EqRole::Mode: e.mode = d->id; break;
        case EqRole::Output: e.output = d->id; break;
        case EqRole::EditChain: e.editChain = d->id; break;
        case EqRole::None: break;
      }
    } else if (const uint32_t c = bandChainOf(*d); c != 0 && !e.chain[c - 1].valid()) {
      e.chain[c - 1] = d->id;
    }
  }
  if (!e.chain[0].valid() || !e.chain[1].valid()) return std::nullopt;
  const WireList& wires = wiresOf(p);
  for (uint32_t c = 0; c < 2; ++c) {
    const DeviceId chain = e.chain[c];
    std::vector<BandInfo> members;
    for (const DevicePtr& d : list)
      if (d->group == chain && isGroupDevice(*d))
        if (std::optional<BandInfo> b = bandInfo(p, d->id)) members.push_back(*b);
    // The series: In → band → … (main ports), each band once.
    std::vector<BandInfo>& out = e.bands[c];
    auto taken = [&](DeviceId g) { return std::any_of(out.begin(), out.end(), [&](const BandInfo& x) { return x.group == g; }); };
    DeviceId at = chain;
    std::string_view port = kGroupInPort;
    for (size_t hop = 0; hop <= members.size(); ++hop) {
      const BandInfo* next = nullptr;
      for (const Wire& w : wires) {
        if (w.from != at || w.port != port || w.toMixer() || w.fromNoteSource() || w.param != kMainPort) continue;
        for (const BandInfo& b : members)
          if (b.group == w.to && !taken(b.group)) next = &b;
        if (next != nullptr) break;
      }
      if (next == nullptr) break;
      out.push_back(*next);
      at = next->group;
      port = kMainPort;
    }
    for (const BandInfo& b : members)  // (bands off the series: after it)
      if (!taken(b.group)) out.push_back(b);
  }
  return e;
}

std::optional<std::pair<EqInfo, uint32_t>> eqOfBand(const Project& p, DeviceId band) {
  const DeviceLoc loc = findDevice(p, band);
  if (loc.device == nullptr || !loc.device->group.valid()) return std::nullopt;
  const DeviceLoc chain = findDevice(p, loc.device->group);
  if (chain.device == nullptr || bandChainOf(*chain.device) == 0 || !chain.device->group.valid()) return std::nullopt;
  std::optional<EqInfo> e = eqInfo(p, chain.device->group);
  if (!e) return std::nullopt;
  for (uint32_t c = 0; c < 2; ++c)
    if (e->chain[c] == chain.device->id) return std::make_pair(std::move(*e), c);
  return std::nullopt;
}

BlockSet eqBandBlocks(IdSource& ids, const EqBandOptions& o) {
  const DeviceId group = o.id.valid() ? o.id : ids.device();
  Kit k(ids, group);
  const uint32_t type = std::min<uint32_t>(o.type, 7);
  dsp::Curve stagesCurve = linear(1.0, 64.0);
  stagesCurve.step = 1.0;
  const Sig freq = k.val("Frequency", bandFrequencyCurve(), o.freqHz);
  const Sig q = k.val("Q", detent(octave(0.1, 40.0), std::numbers::sqrt2 / 2.0), o.q);  // TX.reset-default: Butterworth
  const Sig gain = k.val("Gain", detent(gainDb(-24.0, 24.0), 0.0), o.gainDb);
  const Sig stages = k.val("Stages", stagesCurve, static_cast<double>(std::clamp<uint32_t>(o.stages, 1, 64)));
  const Sig ty = k.val("Type", stepped(8.0), static_cast<double>(type));
  addRole(k, freq, kBandRoleSetting, static_cast<double>(BandRole::Frequency));
  addRole(k, q, kBandRoleSetting, static_cast<double>(BandRole::Q));
  addRole(k, gain, kBandRoleSetting, static_cast<double>(BandRole::Gain));
  addRole(k, stages, kBandRoleSetting, static_cast<double>(BandRole::Stages));
  addRole(k, ty, kBandRoleSetting, static_cast<double>(BandRole::Type));
  const DeviceId svf = k.block("svf", "svf", {{"x", k.in()}, {"cutoff", freq}, {"q", q}, {"gain", gain}, {"mode", ty}, {"stages", stages}});
  k.out(Sig{svf, "out"});
  // Its own panel (the band opened or placed alone): EQ.4b: its curve (the response display with the analyser on its
  // output, its input dimmed; EQ.4d: its phase and delay areas under the magnitude), then the strip's column laid flat.
  Panel p;
  p.width = 240.0f;
  p.height = 228.0f;
  p.elements.push_back(plate(p.width, p.height, 6.0));
  PanelElement display = element(7, "response", 8, 8, p.width - 16.0f, 138, signalBinding(group, "y"));
  display.bindings.push_back(signalBinding(group, std::string(kGroupInPort)));
  p.elements.push_back(std::move(display));
  constexpr float kRow = 154.0f;
  p.elements.push_back(menu(control(2, "menu", 8, kRow, 96, 18, ty), kBandTypeItems));
  p.elements.push_back(control(3, "number", 110, kRow, 60, 18, freq));
  p.elements.push_back(qBox(control(4, "number", 174, kRow, 58, 18, q)));
  p.elements.push_back(shownWhen(control(5, "knob", 8, kRow + 22, 58, 44, gain), 2, {4.0, 5.0, 6.0}));
  p.elements.push_back(shownWhen(stagesBox(control(6, "number", 110, kRow + 22, 60, 18, stages)), 2, {0.0, 1.0, 2.0, 5.0, 6.0, 7.0}));
  // TX.group-oversample: a band placed alone oversamples itself (inside an EQ, the EQ's setting covers it).
  p.elements.push_back(menu(element(8, "menu", 174, kRow + 22, 58, 18, settingBinding(group, std::string(kGroupOversample))), kOversampleItems));
  p.elements.push_back(caption(9, 174, kRow + 42, 58, 12, "Oversample"));
  return k.finish(groupDevice(group, o.inGroup, o.name.empty() ? std::string("Band") : o.name, o.active, std::move(p)));
}

Panel layEqStrip(const Panel& panel, const std::vector<BandInfo> (&bands)[2]) {
  Panel p;
  p.height = panel.height;
  uint32_t chains = 0;  // the chain tabs every column follows (0: none)
  for (const PanelElement& e : panel.elements) {
    if (e.text(kEqTag, "") == kTagStrip) continue;
    if (e.text(kEqTag, "") == kTagChains) chains = e.id;
    p.elements.push_back(e);
  }
  const size_t columns = std::max<size_t>({kMinColumns, bands[0].size(), bands[1].size()});
  p.width = kStripX + kColumnStep * static_cast<float>(columns) + 4.0f;
  uint32_t id = panel.nextId();
  for (uint32_t c = 0; c < 2; ++c)
    for (size_t i = 0; i < bands[c].size(); ++i) {
      const BandInfo& b = bands[c][i];
      const float x = kStripX + kColumnStep * static_cast<float>(i);
      auto bound = [](DeviceId v) { return Sig{v, std::string(kValueOutput)}; };
      const std::vector<double> chainValues = c == 0 ? std::vector<double>{0.0, kWhenHidden} : std::vector<double>{1.0};
      // The column's plate: bound to the band (a Panel binding on a shape: it lights up while the band is selected on
      // the response display), shown with its chain.
      PanelElement plate = element(id++, "shape", x - 2.0f, kStripTop - 4.0f, kColumnW + 4.0f, 160.0f, panelBinding(b.group));
      plate.setField(styleText("fill", "track"));
      plate.setField(styleNumber("opacity", 0.45));
      plate.setField(styleText("stroke", "none"));
      plate.setField(styleNumber("radius", 4.0));
      plate.setField(styleNumber("band", static_cast<double>(i)));
      if (chains != 0) plate.when = Visibility{chains, chainValues};
      p.elements.push_back(tagged(std::move(plate), kTagStrip));
      const uint32_t typeMenu = id;
      if (b.type.valid()) {
        // The type as its glyph and short code (LP, PK …) in the band's colour (`band`: its index in the chain).
        PanelElement m = menu(control(id++, "bandtype", x, kStripTop, kColumnW, 18, bound(b.type)), kBandTypeItems);
        m.setField(styleNumber("band", static_cast<double>(i)));
        m = tagged(std::move(m), kTagStrip);
        if (chains != 0) m.when = Visibility{chains, chainValues};
        p.elements.push_back(std::move(m));
      }
      // The rest follow the Type menu (shown with it for these types), or the tabs when the band has no Type block.
      auto follow = [&](PanelElement e, std::vector<double> types) {
        if (b.type.valid()) e.when = Visibility{typeMenu, std::move(types)};
        else if (chains != 0) e.when = Visibility{chains, chainValues};
        return tagged(std::move(e), kTagStrip);
      };
      const std::vector<double> all{0, 1, 2, 3, 4, 5, 6, 7};
      PanelElement f = control(id++, "number", x, kStripTop + 22, kColumnW, 18, bound(b.frequency));
      f.setField(styleText("label", "-"));
      p.elements.push_back(follow(std::move(f), all));
      if (b.q.valid()) {
        PanelElement q = qBox(control(id++, "number", x, kStripTop + 44, kColumnW, 18, bound(b.q)));
        q.setField(styleText("label", "-"));
        p.elements.push_back(follow(std::move(q), all));
      }
      if (b.gain.valid()) {
        PanelElement g = control(id++, "slider", x, kStripTop + 66, kColumnW, 64, bound(b.gain));
        g.setField(styleText("orientation", "vertical"));
        p.elements.push_back(follow(std::move(g), b.type.valid() ? std::vector<double>{4, 5, 6} : all));
      }
      if (b.stages.valid()) {
        PanelElement s = stagesBox(control(id++, "number", x, kStripTop + 134, kColumnW, 18, bound(b.stages)));
        s.setField(styleText("label", "-"));
        p.elements.push_back(follow(std::move(s), b.type.valid() ? std::vector<double>{0, 1, 2, 5, 6, 7} : all));
      }
    }
  for (PanelElement& e : p.elements) {
    if (e.number("plate", 0.0) != 0.0) {
      e.w = p.width;
      e.h = p.height;
    }
    if (e.text(kEqTag, "") == kTagDisplay) e.w = p.width - 16.0f;
  }
  return p;
}

BlockSet eqBlocks(IdSource& ids, const StockModuleOptions& o) {
  const DeviceId group = o.id.valid() ? o.id : ids.device();
  Kit k(ids, group);
  const Sig mode = k.val("Mode", stepped(3.0), 0.0);
  const Sig output = k.val("Output", detent(gainDb(-24.0, 24.0), 0.0), 0.0);
  const Sig edit = k.val("Edit Chain", stepped(2.0), 0.0);
  addRole(k, mode, kEqRoleSetting, static_cast<double>(EqRole::Mode));
  addRole(k, output, kEqRoleSetting, static_cast<double>(EqRole::Output));
  addRole(k, edit, kEqRoleSetting, static_cast<double>(EqRole::EditChain));
  // The chains: groups of bands in series.
  auto chainGroup = [&](const char* name, uint32_t which) {
    const DeviceId c = ids.device();
    auto g = std::make_shared<Device>(*makeGroup(c));
    g->name = name;
    g->group = group;
    g->settings = {DeviceValue{std::string(kBandChainSetting), static_cast<double>(which)}};
    k.b.set.devices.push_back(std::move(g));
    return c;
  };
  const DeviceId chainA = chainGroup("Chain A", 1), chainB = chainGroup("Chain B", 2);
  std::vector<BandInfo> bands[2];
  struct Default {
    uint32_t type;
    double hz;
  };
  constexpr Default kDefaults[] = {{5, 100.0}, {4, 400.0}, {4, 2500.0}, {6, 8000.0}};
  DeviceId at = chainA;
  std::string_view port = kGroupInPort;
  uint32_t n = 0;
  for (const Default& d : kDefaults) {
    EqBandOptions bo;
    bo.inGroup = chainA;
    bo.type = d.type;
    bo.freqHz = d.hz;
    bo.name = "Band " + std::to_string(++n);
    BlockSet b = eqBandBlocks(ids, bo);
    const BandInfo info = bandOfSet(b);
    bands[0].push_back(info);
    for (DevicePtr& dv : b.devices) k.b.set.devices.push_back(std::move(dv));
    for (Wire& w : b.wires) k.b.set.wires.push_back(std::move(w));
    k.b.wire(at, port, info.group, kMainPort);
    at = info.group;
    port = kMainPort;
  }
  k.b.wire(at, port, chainA, kGroupOutPort);
  k.b.wire(chainB, kGroupInPort, chainB, kGroupOutPort);  // (empty: In → Out)
  // The matrix (literal muxes on Mode: lower.h "Structural value blocks"). Stereo folds every op of it and of chain B
  // away; A's input is chosen before type inference, so A runs mono in L/R and M/S.
  const DeviceId chL = k.block("channel", "L", {{"x", k.in()}}), chR = k.block("channel", "R", {{"x", k.in()}});
  k.setting(chL, "index", 0.0);
  k.setting(chR, "index", 1.0);
  const Sig L{chL, "out"}, R{chR, "out"}, half = k.k(0.5);
  const Sig m1 = k.op("sub", "Mode - 1", {{"a", mode}, {"b", k.k(1.0)}});
  const Sig mid = k.op("mul", "M = (L + R) / 2", {{"a", k.op("add", "L + R", {{"a", L}, {"b", R}})}, {"b", half}});
  const Sig side = k.op("mul", "S = (L - R) / 2", {{"a", k.op("sub", "L - R", {{"a", L}, {"b", R}})}, {"b", half}});
  const Sig x = k.op("mux", "L or M", {{"index", m1}, {"in0", L}, {"in1", mid}});
  const Sig inA = k.op("mux", "into A", {{"index", mode}, {"in0", k.in()}, {"in1", x}});
  const Sig inB = k.op("mux", "into B", {{"index", m1}, {"in0", R}, {"in1", side}});
  k.b.wire(inA.dev, inA.port, chainA, kMainPort);
  k.b.wire(inB.dev, inB.port, chainB, kMainPort);
  const Sig outA{chainA, std::string(kMainPort)}, outB{chainB, std::string(kMainPort)};
  const DeviceId chA0 = k.block("channel", "A", {{"x", outA}});
  k.setting(chA0, "index", 0.0);
  const Sig a0{chA0, "out"};
  const Sig lr = k.op("pack", "(A, B)", {{"in0", a0}, {"in1", outB}});
  const Sig ms = k.op("pack", "(A + B, A - B)", {{"in0", k.op("add", "A + B", {{"a", a0}, {"b", outB}})},
                                                  {"in1", k.op("sub", "A - B", {{"a", a0}, {"b", outB}})}});
  const Sig y = k.op("mux", "L/R or M/S", {{"index", m1}, {"in0", lr}, {"in1", ms}});
  const Sig z = k.op("mux", "out", {{"index", mode}, {"in0", outA}, {"in1", y}});
  const DeviceId outGain = k.block("gain", "x output", {{"x", z}, {"g", output}});
  k.setting(outGain, "start", 1.0);
  k.out(Sig{outGain, "out"});
  // The panel: the response display (the analyser on the EQ's output, its input dimmed; its magnitude, phase and delay
  // areas), the controls on the left, the band strip.
  Panel p;
  p.width = kStripX + kColumnStep * kMinColumns + 4.0f;
  p.height = kEqHeight;
  p.elements.push_back(plate(p.width, p.height, 6.0));
  PanelElement display = element(2, "response", 8, kResponseY, p.width - 16.0f, kResponseH, signalBinding(group, "y"));
  display.bindings.push_back(signalBinding(group, std::string(kGroupInPort)));
  p.elements.push_back(tagged(std::move(display), kTagDisplay));
  p.elements.push_back(caption(3, 8, kStripTop - 2, 72, 12, "Mode"));
  p.elements.push_back(menu(control(4, "menu", 8, kStripTop + 12, 72, 18, mode), kEqModeItems));
  // The chain tabs: one bound to Edit Chain that the columns follow (shown when Mode ≠ Stereo; while it is hidden chain
  // A's columns show: kWhenHidden), under the two the user sees (the same block: L | R, M | S).
  PanelElement chains = shownWhen(control(5, "tabs", 8, kStripTop + 34, 72, 16, edit), 4, {1.0, 2.0});
  chains.setField(styleText("items", "A|B"));
  p.elements.push_back(tagged(std::move(chains), kTagChains));
  PanelElement lrTabs = shownWhen(control(6, "tabs", 8, kStripTop + 34, 72, 16, edit), 4, {1.0});
  lrTabs.setField(styleText("items", "L|R"));
  p.elements.push_back(std::move(lrTabs));
  PanelElement msTabs = shownWhen(control(7, "tabs", 8, kStripTop + 34, 72, 16, edit), 4, {2.0});
  msTabs.setField(styleText("items", "M|S"));
  p.elements.push_back(std::move(msTabs));
  p.elements.push_back(control(8, "knob", 8, kStripTop + 56, 72, 64, output));
  // TX.group-oversample: the EQ's oversampling (its group's setting: every band shares one up / down conversion).
  p.elements.push_back(caption(9, 8, kStripTop + 124, 72, 12, "Oversample"));
  p.elements.push_back(menu(element(10, "menu", 8, kStripTop + 138, 72, 18, settingBinding(group, std::string(kGroupOversample))),
                            kOversampleItems));
  p = layEqStrip(p, bands);
  return k.finish(groupDevice(group, o.inGroup, o.name.empty() ? std::string("EQ") : o.name, o.active, std::move(p)));
}

// ---- DT.1: the Delay Tap --------------------------------------------------------------------------------------------

namespace {

using NK = NoteValueKind;
// Straight s, triplet 2/3 s, dotted 3/2 s for s = 1/64 … 1/1 note (1/16 … 4 beats), in ascending duration: T(n) < S(n) <
// T(n+1) < D(n) < S(n+1) (4/3 s < 3/2 s). DT.3: each named as the reduced fraction of a whole note it lasts ("1/12", not
// "1/8T"; "3/16", not "1/8.").
constexpr std::array<NoteValue, 21> kNoteValues = {{
    {1.0 / 24.0, "1/96", NK::Triplet}, {1.0 / 16.0, "1/64", NK::Straight}, {1.0 / 12.0, "1/48", NK::Triplet},
    {3.0 / 32.0, "3/128", NK::Dotted}, {1.0 / 8.0, "1/32", NK::Straight},  {1.0 / 6.0, "1/24", NK::Triplet},
    {3.0 / 16.0, "3/64", NK::Dotted},  {1.0 / 4.0, "1/16", NK::Straight},  {1.0 / 3.0, "1/12", NK::Triplet},
    {3.0 / 8.0, "3/32", NK::Dotted},   {1.0 / 2.0, "1/8", NK::Straight},   {2.0 / 3.0, "1/6", NK::Triplet},
    {3.0 / 4.0, "3/16", NK::Dotted},   {1.0, "1/4", NK::Straight},         {4.0 / 3.0, "1/3", NK::Triplet},
    {3.0 / 2.0, "3/8", NK::Dotted},    {2.0, "1/2", NK::Straight},         {8.0 / 3.0, "2/3", NK::Triplet},
    {3.0, "3/4", NK::Dotted},          {4.0, "1/1", NK::Straight},         {6.0, "3/2", NK::Dotted},
}};

}  // namespace

std::span<const NoteValue> noteValues() noexcept { return kNoteValues; }

double noteValueSeconds(uint32_t index, double bpm) noexcept {
  const NoteValue& v = kNoteValues[std::min<size_t>(index, kNoteValues.size() - 1)];
  const double s = v.beats * 60.0 / std::max(bpm, 1e-6);
  return std::min(s, kDelayTapMaxSeconds);
}

dsp::Curve delayTimeCurve() {
  // ms(x) = 62.5 · (e^(k·x) − 1), k = 2·ln 9 = ln 81: Exp0's bend is arg · ln 2, so arg = log2(81).
  dsp::Curve c = withUnit(dsp::curvePreset(dsp::CurvePreset::Exp0, 0.0, kDelayTapMaxSeconds, std::log2(81.0)), dsp::CurveUnit::Seconds);
  return c;
}

DelayRole delayRole(const Device& d) noexcept {
  const double v = settingOf(d, kDelayRoleSetting);
  return v >= 1.0 && v <= 3.0 && v == std::floor(v) ? static_cast<DelayRole>(static_cast<uint8_t>(v)) : DelayRole::None;
}

BlockSet delayTapBlocks(IdSource& ids, const DelayTapOptions& o, DelayTapRoles* roles) {
  DelayTapRoles r;
  r.group = o.id.valid() ? o.id : ids.device();
  Kit k(ids, r.group);
  const std::string y(kValueOutput);
  const Sig time{k.b.value("Time", delayTimeCurve(), o.time), y};
  const double syncCount = static_cast<double>(kNoteValues.size());
  const Sig sync{k.b.value("Sync", stepped(syncCount), std::min<double>(o.sync, syncCount - 1.0) / (syncCount - 1.0)), y};
  const Sig mode{k.b.value("Mode", stepped(2.0), o.mode != 0 ? 1.0 : 0.0), y};
  addRole(k, time, kDelayRoleSetting, static_cast<double>(DelayRole::Time));
  addRole(k, sync, kDelayRoleSetting, static_cast<double>(DelayRole::Sync));
  addRole(k, mode, kDelayRoleSetting, static_cast<double>(DelayRole::Mode));
  r.time = time.dev, r.sync = sync.dev, r.mode = mode.dev;
  // The note value's beats, ARITHMETIC (not a mux chain: a stepped block reaching a mux index would lower as a literal
  // choice — lower.h "Structural value blocks" — so a sync step or a mode switch would recompile the unit instead of
  // gliding): a nested group "Note Value" (its In the Sync index i, its Out the beats). The ascending list is periodic
  // from i = 1: j = i − 1, g = floor(j / 3), r = j − 3g gives S(g), T(g+1), D(g) for r = 0, 1, 2, i.e. 2^g / 16 beats ×
  // f(r), f = 1 + r·(5 − r)/12 (1, 4/3, 3/2); its two ends corrected: i = 0 is 1/64T (not D(−1)), i = 20 is 1/1. (not
  // T(7)). Param rate: one Control-region evaluation per block in which Sync changes (tests/document/test_delay_tap.cpp
  // checks it against noteValues()).
  Sig beats;
  {
    const DeviceId sub = ids.device();
    auto g = std::make_shared<Device>(*makeGroup(sub));
    g->name = "Note Value";
    g->group = r.group;
    k.b.set.devices.push_back(std::move(g));
    k.b.wire(sync.dev, sync.port, sub, kMainPort);
    const DeviceId outer = k.b.group;
    k.b.group = sub;
    const Sig i{sub, std::string(kGroupInPort)};
    auto op2 = [&](const char* atom, const char* name, const Sig& a, const Sig& b) { return k.op(atom, name, {{"a", a}, {"b", b}}); };
    const Sig j = op2("sub", "j = i - 1", i, k.k(1.0));
    const Sig gi = k.op("floor", "g", {{"x", op2("div", "j / 3", j, k.k(3.0))}});
    const Sig rem = op2("sub", "r = j - 3g", j, op2("mul", "3g", gi, k.k(3.0)));
    const Sig straight = op2("mul", "2^g / 16", op2("pow", "2^g", k.k(2.0), gi), k.k(1.0 / 16.0));
    const Sig f = op2("add", "f(r)", k.k(1.0), op2("mul", "r (5 - r) / 12", rem, op2("mul", "(5 - r) / 12", op2("sub", "5 - r", k.k(5.0), rem), k.k(1.0 / 12.0))));
    const Sig base = op2("mul", "S(g) f(r)", straight, f);
    auto edge = [&](const Sig& x, const char* name) {  // clip(x, 0, 1): 1 at the end, 0 elsewhere (i whole)
      return k.op("clip", name, {{"x", x}, {"lo", k.k(0.0)}, {"hi", k.k(1.0)}});
    };
    const Sig top = op2("mul", "+ 2/3 at 1/1.", edge(op2("sub", "i - 19", i, k.k(19.0)), "i = 20"), k.k(6.0 - 16.0 / 3.0));
    const Sig bottom = op2("mul", "- 1/192 at 1/64T", edge(op2("sub", "1 - i", k.k(1.0), i), "i = 0"), k.k(1.0 / 24.0 - 3.0 / 64.0));
    const Sig b = op2("add", "beats", op2("add", "+ top", base, top), bottom);
    k.b.wire(b.dev, b.port, sub, kGroupOutPort);
    k.b.group = outer;
    beats = Sig{sub, std::string(kMainPort)};
  }
  // Sync seconds = min(beats · 60 / bpm, 5); the delay = lerp(time, sync, Mode) (arithmetic for the same reason; Mode
  // is 0 or 1). Param rate: a Control region, both computed per block in which an input changes (a few ops).
  const Sig bpm = k.op("transport.bpm", "tempo", {});
  const Sig synced = k.op("min", "≤ 5 s", {{"a", k.op("div", "· 60 / bpm", {{"a", k.op("mul", "beats · 60", {{"a", beats}, {"b", k.k(60.0)}})}, {"b", bpm}})},
                                           {"b", k.k(kDelayTapMaxSeconds)}});
  const Sig seconds = k.op("lerp", "ms / sync", {{"a", time}, {"b", synced}, {"t", mode}});
  r.delay = k.block("delay_fade", "delay", {{"x", k.in()}, {"time", seconds}});
  k.setting(r.delay, "max", kDelayTapMaxSeconds);
  k.setting(r.delay, "fade", kDelayTapFadeSeconds);
  k.out(Sig{r.delay, "out"});
  // The panel: the plate and one tempo slider (its tempo markings above, the readout, the ms / ♪ and magnet buttons below).
  Panel p;
  p.width = 168.0f;
  p.height = 64.0f;
  p.elements.push_back(plate(p.width, p.height, 8.0));
  PanelElement s = element(2, "temposlider", 0, 0, p.width, p.height, controlBinding(time.dev, std::string(kValueParam)));
  s.bindings.push_back(controlBinding(sync.dev, std::string(kValueParam)));
  s.bindings.push_back(controlBinding(mode.dev, std::string(kValueParam)));
  p.elements.push_back(std::move(s));
  if (roles != nullptr) *roles = r;
  return k.finish(groupDevice(r.group, o.inGroup, o.name.empty() ? std::string("Delay Tap") : o.name, o.active, std::move(p)));
}

}  // namespace pq::doc
