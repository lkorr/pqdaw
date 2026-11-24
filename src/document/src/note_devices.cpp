// Note devices (AP.2): pq/document/note_devices.h.
#include "pq/document/note_devices.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <optional>

#include "pq/document/lower.h"
#include "pq/engine/arp.h"

namespace pq::doc {

namespace {

std::vector<std::string> splitChoices(std::string_view s) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i)
    if (i == s.size() || s[i] == '|') {
      out.emplace_back(s.substr(start, i - start));
      start = i + 1;
    }
  return out;
}

Panel arpeggiatorPanel(DeviceId dev);  // AP.6 (below)

std::vector<NoteDeviceType> makeTypes() {
  std::vector<NoteDeviceType> out;
  NoteDeviceType arp;
  arp.id = "arpeggiator";
  arp.name = "Arpeggiator";
  arp.patchName = "Arpeggiator";
  arp.description = "Plays the held notes one after another: styles, steps, octaves, glides and pulses. A note effect: "
                    "it goes on a MIDI track's notes cable, before the instrument.";
  arp.in = arp.out = ChainSignal::Notes;
  arp.stage = NoteStageKind::Arpeggiator;
  arp.panel = &arpeggiatorPanel;
  for (const engine::ArpParamInfo& r : engine::kArpParamInfo) {
    NoteParamSpec s;
    s.name = std::string(r.name);
    s.label = std::string(r.label);
    s.min = r.min;
    s.max = r.max;
    s.def = r.def;
    s.step = r.step;
    s.isBool = r.isBool;
    s.choices = splitChoices(r.choices);
    s.runtime = r.runtime;
    s.unit = std::string(r.unit);
    s.taper = r.taper;
    arp.params.push_back(std::move(s));
  }
  out.push_back(std::move(arp));
  return out;
}

const std::vector<NoteDeviceType>& types() {
  static const std::vector<NoteDeviceType> t = makeTypes();
  return t;
}

std::string num(double v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

}  // namespace

const NoteParamSpec* NoteDeviceType::param(std::string_view n) const noexcept {
  for (const NoteParamSpec& s : params)
    if (s.name == n) return &s;
  return nullptr;
}

int32_t NoteDeviceType::paramIndex(std::string_view n) const noexcept {
  for (size_t i = 0; i < params.size(); ++i)
    if (params[i].name == n) return static_cast<int32_t>(i);
  return -1;
}

std::span<const NoteDeviceType> noteDeviceTypes() { return types(); }

const NoteDeviceType* findNoteDeviceType(std::string_view id) noexcept {
  for (const NoteDeviceType& t : types())
    if (t.id == id) return &t;
  return nullptr;
}

bool isNoteDevicePatch(std::string_view patch) noexcept {
  return patch.starts_with(kNoteDevicePrefix) && findNoteDeviceType(patch.substr(kNoteDevicePrefix.size())) != nullptr;
}

std::string noteDevicePatch(std::string_view type) { return std::string(kNoteDevicePrefix) + std::string(type); }

bool isNoteDevice(const Device& d) noexcept { return isNoteDevicePatch(d.patch); }

const NoteDeviceType* noteDeviceTypeOf(const Device& d) noexcept {
  if (!d.patch.starts_with(kNoteDevicePrefix)) return nullptr;
  return findNoteDeviceType(std::string_view(d.patch).substr(kNoteDevicePrefix.size()));
}

MainSignals declaredMainSignals(const Device& d) noexcept {
  const NoteDeviceType* t = noteDeviceTypeOf(d);
  return t != nullptr ? MainSignals{t->in, t->out} : MainSignals{};
}

std::string noteDevicePatchText(const NoteDeviceType& t) {
  std::string s = "patch " + t.patchName + " {\n  in    notes : audio global\n";
  for (const NoteParamSpec& p : t.params)
    if (!p.runtime) s += "  in    " + p.name + " : real const = " + num(p.def) + "\n";
  for (const NoteParamSpec& p : t.params) {
    if (!p.runtime) continue;
    s += "  param " + p.name + " = ";
    if (p.isBool) {
      s += p.def > 0.5 ? "true" : "false";
    } else {
      const bool pct = p.unit == "%";  // AP.11: percent literals (a fraction × 100)
      auto lit = [&](double v) { return pct ? num(v * 100.0) + "%" : num(v); };
      s += lit(p.def) + " [" + lit(p.min) + ".." + lit(p.max) + "]";
      if (p.step > 0.0) s += " @steps(" + num(std::round((p.max - p.min) / p.step) + 1.0) + ")";
      if (p.taper > 0.0) s += " @curve(exp0, " + num(p.taper) + ")";
    }
    s += " @label(\"" + p.label + "\")";
    if (!p.choices.empty()) {
      std::string items;
      for (const std::string& c : p.choices) items += (items.empty() ? "" : "|") + c;
      s += " @choices(\"" + items + "\")";
    }
    s += "\n";
  }
  s += "  out   out : audio global\n  out = notes\n}\n";
  return s;
}

// ---- AP.6: the Arpeggiator's panel ---------------------------------------------------------------------------------

namespace {

using engine::ArpParamId;

// Builds a panel: element ids in order, each element's rect in panel units (logical px at 1:1).
struct PanelKit {
  Panel p;
  DeviceId dev;
  uint32_t next = 1;
  static std::string name(ArpParamId id) { return std::string(engine::arpParamInfo(id).name); }
  PanelElement& add(const char* kind, float x, float y, float w, float h, std::vector<Binding> b = {}) {
    PanelElement e;
    e.id = next++;
    e.kind = kind;
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.bindings = std::move(b);
    p.elements.push_back(std::move(e));
    return p.elements.back();
  }
  // A control on param `id` (a Control binding; a structural one: a Setting binding).
  Binding bind(ArpParamId id) const {
    return engine::arpParamInfo(id).runtime ? controlBinding(dev, name(id)) : settingBinding(dev, name(id));
  }
  PanelElement& ctl(const char* kind, float x, float y, float w, float h, ArpParamId id, const char* label = nullptr) {
    PanelElement& e = add(kind, x, y, w, h, {bind(id)});
    if (label != nullptr) e.setField(styleText("label", label));
    return e;
  }
  // A number box without a label in a labelled row: its box where the labelled ones' boxes are.
  PanelElement& bare(float x, float y, float w, float h, ArpParamId id) { return ctl("number", x, y + 10, w, h - 10, id, "-"); }
  PanelElement& button(float x, float y, float w, float h, ArpParamId id, const char* text) {
    PanelElement& e = ctl("toggle", x, y, w, h, id);
    e.setField(styleText("look", "button"));
    e.setField(styleText("text", text));
    return e;
  }
  PanelElement& caption(float x, float y, float w, const char* text, double size = 7.5) {
    PanelElement& e = add("label", x, y, w, 10);
    e.setField(styleText("text", text));
    e.setField(styleText("align", "left"));
    e.setField(styleNumber("size", size));
    return e;
  }
  // A caption over chips (tabs) on param `id`: the chips' element.
  PanelElement& chips(float x, float y, float w, ArpParamId id, const char* label, const char* items) {
    caption(x, y, w, label);
    PanelElement& e = ctl("tabs", x, y + 12, w, 16, id);
    e.setField(styleText("items", items));
    return e;
  }
  void backdrop(float x, float y, float w, float h) {
    PanelElement& e = add("shape", x, y, w, h);
    e.setField(styleText("fill", "track"));
    e.setField(styleText("stroke", "none"));
    e.setField(styleNumber("opacity", 0.35));
    e.setField(styleNumber("radius", 5.0));
  }
  PanelElement& fold(float y, const char* title, bool open = false) {
    PanelElement& e = add(kFoldKind.data(), 6, y, p.width - 12, 16);
    e.setField(styleText("title", title));
    e.setField(styleNumber("open", open ? 1.0 : 0.0));
    return e;
  }
  // The elements added since `from` shown only while element `src` has one of `values` (unless they have a condition).
  void when(size_t from, uint32_t src, const std::vector<double>& values) {
    for (size_t i = from; i < p.elements.size(); ++i)
      if (!p.elements[i].when) p.elements[i].when = Visibility{src, values};
  }
};

// AR.2: the glide params a drag on a note sets, in the slot order of the arpbend / arppattern elements (after slot 0,
// the device).
constexpr ArpParamId kGlideParams[] = {ArpParamId::IntoOffset, ArpParamId::IntoPrev, ArpParamId::IntoTime,
                                       ArpParamId::IntoCurve,  ArpParamId::IntoShape, ArpParamId::OutOffset,
                                       ArpParamId::OutNext,    ArpParamId::OutTime,   ArpParamId::OutCurve,
                                       ArpParamId::OutShape};

Panel arpeggiatorPanel(DeviceId dev) {
  PanelKit k;
  k.dev = dev;
  k.p.width = 960.0f;  // AP.10: the pattern view and the bend view take the room
  k.p.fit = true;
  using A = ArpParamId;
  // ---- the top band: Arp | Pitch | Pattern (AR.4) ----
  constexpr float kTop = 200.0f;
  k.backdrop(4, 4, 362, kTop - 8);
  k.backdrop(370, 4, 202, kTop - 8);
  k.backdrop(576, 4, 380, kTop - 8);
  // Arp, left column: Style; Gate, Groove; Seed, Offset; Repeats, Hold.
  const uint32_t style = k.ctl("menu", 10, 8, 104, 28, A::Style, "Style").id;
  k.ctl("knob", 10, 44, 48, 58, A::Gate);
  k.ctl("knob", 62, 44, 48, 58, A::Groove).setField(styleNumber("origin", 0.5));  // bipolar: 0 in the middle
  k.ctl("number", 10, 112, 48, 26, A::Seed, "Seed");
  k.ctl("number", 62, 112, 48, 26, A::Offset, "Offset");
  k.ctl("number", 10, 146, 48, 26, A::Repeats, "Repeats").setField(styleText("zero", "\xE2\x88\x9E"));
  k.button(62, 156, 48, 16, A::Hold, "Hold");
  // The rate: Sync, then ONE control — synced: the note value's menu (1/1 … 1/128) and the feel chips (Straight /
  // Dotted / Triplet: the tuplet); its "n:m" chip shows the four numbers themselves instead (any num / den, any
  // tuplet; shown anyway while the rate is none of the menu's). Free: ms.
  const uint32_t sync = k.button(120, 20, 34, 16, A::Synced, "Sync").id;
  uint32_t rate = 0;
  {
    PanelElement& r = k.add("rate", 158, 8, 80, 28, {k.bind(A::RateNum), k.bind(A::RateDen), k.bind(A::TupletN), k.bind(A::TupletM)});
    r.setField(styleText("label", "Rate"));
    r.when = Visibility{sync, {1.0}};
    rate = r.id;
  }
  // (shown while synced AND the rate control's selection is `custom`)
  auto rateWhen = [&](size_t from, double custom) {
    for (size_t i = from; i < k.p.elements.size(); ++i) {
      Visibility v{sync, {1.0}};
      v.more.push_back(VisibilityTerm{rate, {custom}});
      v.all = true;
      k.p.elements[i].when = std::move(v);
    }
  };
  size_t at = k.p.elements.size();
  k.caption(244, 8, 60, "Feel");
  {
    PanelElement& pr = k.add("presets", 244, 20, 116, 16, {k.bind(A::TupletN), k.bind(A::TupletM)});
    pr.setField(styleText("items", "Straight|Dotted|Triplet"));
    pr.setField(styleText("values", "1 1|2 3|3 2"));
  }
  rateWhen(at, 0.0);
  at = k.p.elements.size();
  k.ctl("number", 244, 8, 24, 28, A::RateNum, "Note");
  k.caption(270, 22, 6, "/", 9.0);
  k.bare(276, 8, 30, 28, A::RateDen);
  k.ctl("number", 312, 8, 21, 28, A::TupletN, "Tuplet");
  k.caption(334.5f, 22, 5, ":", 9.0);
  k.bare(339, 8, 21, 28, A::TupletM);
  rateWhen(at, 1.0);
  at = k.p.elements.size();
  k.ctl("number", 158, 8, 72, 28, A::RateMs, "Rate ms");
  k.when(at, sync, {0.0});
  // Retrigger and (Beat) its rate.
  const uint32_t retrig = k.chips(120, 46, 114, A::Retrigger, "Retrigger", "Off|Note|Beat").id;
  at = k.p.elements.size();
  k.caption(244, 46, 60, "Every");
  k.bare(244, 46, 26, 28, A::RetriggerNum);
  k.caption(272, 60, 6, "/", 9.0);
  k.bare(278, 46, 30, 28, A::RetriggerDen);
  k.when(at, retrig, {2.0});
  // Velocity: its source; the decay (to Target over Decay, restarting with the pattern's retrigger or not); Fixed.
  const uint32_t vsrc = k.chips(120, 84, 114, A::VelocitySource, "Velocity", "Played|Fixed|Lane").id;
  k.ctl("knob", 120, 118, 48, 58, A::VelocityDecayMs, "Decay");
  k.ctl("knob", 172, 118, 48, 58, A::VelocityTarget, "Target");
  at = k.p.elements.size();
  k.ctl("knob", 224, 118, 48, 58, A::FixedVelocity, "Fixed");
  k.when(at, vsrc, {1.0});
  // (AP.8b / AR.4, review L12: not the pattern's Retrigger — a switch, no button glow)
  k.caption(244, 84, 80, "Decay restarts");
  k.ctl("toggle", 244, 97, 100, 14, A::VelocityRetrigger).setField(styleText("text", "On retrigger"));
  // Pitch: Unit; Distance, Steps, Shift / Rotate; Ratio p / q (Ratio) and Base (Harmonics); the walk (Harmonic Walk).
  // The row at y 136 … 194 is the Layers' (AR.3: a count and up to three level knobs).
  const uint32_t unit = k.chips(376, 8, 190, A::Unit, "Pitch Unit", "Degrees|Ratio|Harmonics").id;
  k.ctl("number", 376, 40, 50, 28, A::Distance, "Distance");
  k.ctl("number", 430, 40, 40, 28, A::Steps, "Steps");
  k.chips(476, 40, 90, A::Transpose, "Mode", "Shift|Rotate");
  at = k.p.elements.size();
  k.ctl("number", 376, 72, 40, 28, A::RatioP, "Ratio");
  k.caption(419, 86, 8, "/", 9.0);
  k.bare(426, 72, 40, 28, A::RatioQ);
  k.when(at, unit, {1.0});
  {  // Base: the Harmonics unit OR the Harmonic Walk style (a walk of an ungrouped note starts at it)
    PanelElement& base = k.ctl("number", 476, 72, 50, 28, A::HarmonicBase, "Base");
    base.when = Visibility{unit, {2.0}};
    base.when->more.push_back(VisibilityTerm{style, {static_cast<double>(engine::ArpStyle::HarmonicWalk)}});
  }
  at = k.p.elements.size();
  k.ctl("number", 376, 104, 44, 28, A::WalkRange);
  k.button(424, 116, 46, 16, A::WalkUnder, "Under");
  k.ctl("menu", 474, 104, 92, 28, A::WalkOrder);
  k.when(at, style, {static_cast<double>(engine::ArpStyle::HarmonicWalk)});
  {  // AR.3: Layers (both modes): extra copies at (p/q)^L of every note; a level knob only while Layers reaches it
     // (AP.8b L11: a number is a visibility source by its value: the values from n up).
    const uint32_t layers = k.ctl("number", 376, 152, 40, 28, A::Layers, "Layers").id;
    for (int i = 0; i < engine::kArpMaxLayers; ++i) {
      std::vector<double> from;
      for (int n = i + 1; n <= engine::kArpMaxLayers; ++n) from.push_back(static_cast<double>(n));
      const std::string label = "Level " + std::to_string(i + 1);
      k.ctl("knob", 422.0f + 48.0f * static_cast<float>(i), 136, 48, 58, static_cast<A>(static_cast<int>(A::LayerLevel1) + i),
            label.c_str())
          .when = Visibility{layers, std::move(from)};
    }
  }
  // AP.10: the resolved pattern — the whole cycle over the held notes (generated ones dotted, their bends drawn), the
  // playing step lit.
  k.caption(582, 8, 120, "Pattern");
  {  // (AR.2: with the glide params — a drag on a note's head / tail edits them, as in the bend view)
    std::vector<Binding> b = {dataBinding(dev)};
    for (const A id : kGlideParams) b.push_back(k.bind(id));
    k.add("arppattern", 582, 22, 368, kTop - 34, std::move(b));
  }
  // ---- Glide (fold, open; AR.2: the bend view IS the editor — drag the note's head / tail / curves; per side two
  // knobs that follow the drags and its shape chips) ----
  float y = kTop;
  k.fold(y, "Glide", true);
  y += 20.0f;
  auto side = [&](float x, A off, A time, A shape, const char* title) {
    k.caption(x, y, 104, title);
    k.ctl("knob", x, y + 12, 48, 58, off, "Amount");
    k.ctl("knob", x + 54, y + 12, 48, 58, time, "Time");
    k.chips(x, y + 76, 104, shape, "Shape", "Curve|S|Spring");
  };
  side(10, A::IntoOffset, A::IntoTime, A::IntoShape, "Into (head)");
  side(846, A::OutOffset, A::OutTime, A::OutShape, "Out (tail)");
  {  // The bend view: one note large between its neighbours; its drags write the params above (one undo step each).
    std::vector<Binding> b = {dataBinding(dev)};
    for (const A id : kGlideParams) b.push_back(k.bind(id));
    b.push_back(k.bind(A::Gate));
    k.add("arpbend", 124, y, 712, 150, std::move(b));
  }
  y += 158.0f;
  // ---- Pulse (fold; AR.3: the pulse view IS the editor — a row per held note, a dot per pulse of the Cycle; click
  // a dot to mute it, drag a row for its division, Shift+drag for its shift, its menu for Euclid / Rotate / Invert) ----
  k.fold(y, "Pulse");
  y += 20.0f;
  const uint32_t pulse = k.button(10, y + 12, 40, 16, A::Pulse, "On").id;
  at = k.p.elements.size();
  k.button(56, y + 12, 40, 16, A::PulseAuto, "Auto");
  k.ctl("number", 10, y + 36, 28, 28, A::PulseCycleNum, "Cycle");
  k.caption(40, y + 50, 8, "/", 9.0);
  k.bare(46, y + 36, 28, 28, A::PulseCycleDen);
  k.caption(78, y + 50, 24, "bars");
  {
    std::vector<Binding> b = {dataBinding(dev), k.bind(A::PulseAuto), k.bind(A::PulseCycleNum), k.bind(A::PulseCycleDen)};
    for (int32_t r = 0; r < engine::kArpPulseRows; ++r)
      for (const A first : {A::PulseDiv1, A::PulseShift1, A::PulseMaskLo1, A::PulseMaskHi1})
        b.push_back(k.bind(engine::arpPulseRowId(first, r)));
    k.add("arppulse", 110, y, 840, 148, std::move(b));
  }
  k.when(at, pulse, {1.0});
  y += 156.0f;
  // ---- Step Lanes (fold; AR.4) ----
  // One `steplane` per lane, the panel's width: its name column (the switch, the menu, the readout) and 32 steps. A
  // lane that is ON is tall (Transpose: a grid, a row per step of the unit); one that is OFF is its 14 px name strip —
  // the element spans a conditional `band` fold (shown while the lane's switch is on) that the fit takes out, and a
  // plain band between two lanes keeps the next lane's strip out of that section.
  const uint32_t lanesFold = k.fold(y, "Step Lanes").id;
  y += 20.0f;
  struct Lane {
    engine::ArpLane lane;
    A on, len;
    const char* name;
    float height;
    const char* look;
    const char* copy;  // the lanes its values make sense in (the same 0 … 1 kind)
  };
  constexpr float kLaneStrip = 14.0f;  // (the views' StepLane::kStrip)
  constexpr Lane kLanes[] = {
      {engine::ArpLane::Velocity, A::VelocityLane, A::VelocityLength, "Velocity", 44, "bars",
       "Gate=gateStep,gateLen|Glide=glideStep,glideLen|Chance=chanceStep,chanceLen"},
      {engine::ArpLane::Gate, A::GateLane, A::GateLength, "Gate", 44, "bars",
       "Velocity=velStep,velLen|Glide=glideStep,glideLen|Chance=chanceStep,chanceLen"},
      {engine::ArpLane::Glide, A::GlideLane, A::GlideLength, "Glide", 44, "bars",
       "Velocity=velStep,velLen|Gate=gateStep,gateLen|Chance=chanceStep,chanceLen"},
      {engine::ArpLane::Transpose, A::TransposeLane, A::TransposeLength, "Transpose", 96, "grid", ""},
      {engine::ArpLane::Chance, A::ChanceLane, A::ChanceLength, "Chance", 44, "bars",
       "Velocity=velStep,velLen|Gate=gateStep,gateLen|Glide=glideStep,glideLen"},
      {engine::ArpLane::SkipTie, A::SkipTieLane, A::SkipTieLength, "Skip / Tie", 26, "cells", ""},
  };
  auto band = [&](float top, std::optional<Visibility> when) {
    PanelElement& f = k.add(kFoldKind.data(), 10, top, k.p.width - 20, 1);
    f.setField(styleText("look", "band"));
    f.setField(styleNumber(std::string(kFoldParentField), static_cast<double>(lanesFold)));
    f.when = std::move(when);
  };
  bool first = true;
  for (const Lane& l : kLanes) {
    if (!first) {
      band(y, std::nullopt);
      y += 3.0f;
    }
    first = false;
    std::vector<Binding> b = {k.bind(l.on), k.bind(l.len), k.bind(A::Seed), k.bind(A::Unit)};
    for (int32_t i = 0; i < engine::kArpMaxLaneSteps; ++i) b.push_back(k.bind(engine::arpLaneStepId(l.lane, i)));
    PanelElement& e = k.add("steplane", 10, y, k.p.width - 20, l.height, std::move(b));
    e.setField(styleNumber("lane", static_cast<double>(l.lane)));
    e.setField(styleText("look", l.look));
    e.setField(styleText("name", l.name));
    if (l.copy[0] != 0) e.setField(styleText("copy", l.copy));
    if (l.lane == engine::ArpLane::Gate) e.setField(styleNumber("mark", 0.5));  // × 1
    const uint32_t id = e.id;
    band(y + kLaneStrip, Visibility{id, {1.0}});
    y += l.height;
  }
  band(y, std::nullopt);
  y += 1.0f;
  k.p.height = y + 4.0f;
  return std::move(k.p);
}

}  // namespace

Panel noteDevicePanel(const Device& d) {
  const NoteDeviceType* t = noteDeviceTypeOf(d);
  return t != nullptr && t->panel != nullptr ? t->panel(d.id) : Panel{};
}

uint32_t noteRun(const MainPath& path) noexcept {
  uint32_t n = 0;
  while (n < path.blocks.size() && isNoteDevice(*path.blocks[n])) ++n;
  return n;
}

NoteDissolve dissolveNoteDevices(const Project& p) {
  NoteDissolve out;
  std::vector<DeviceId> notes;
  forEachDevice(p, [&](const Device& d, const Track&) {
    if (isNoteDevice(d)) notes.push_back(d.id);
  });
  if (notes.empty()) return out;
  out.any = true;
  WireList w = wiresOf(p);
  std::vector<std::pair<WireId, WireId>> alias;
  auto runtimeParam = [&](DeviceId n, const std::string& name) {
    const Device* d = findDevice(p, n).device;
    const NoteDeviceType* t = d != nullptr ? noteDeviceTypeOf(*d) : nullptr;
    const NoteParamSpec* s = t != nullptr ? t->param(name) : nullptr;
    return s != nullptr && s->runtime && !s->isBool;
  };
  for (const DeviceId n : notes) {
    const Wire* in = nullptr;  // the oldest wire into its main input (the list is by id)
    for (const Wire& x : w)
      if (!x.toMixer() && x.to == n && x.param == kMainPort) {
        in = &x;
        break;
      }
    const std::optional<Wire> from = in != nullptr ? std::optional<Wire>(*in) : std::nullopt;
    WireList next;
    next.reserve(w.size());
    WireId joinedFirst;
    for (const Wire& x : w) {
      const bool into = !x.toMixer() && x.to == n;
      const bool outOf = !x.fromNoteSource() && x.from == n;
      if (!into && !outOf) {
        next.push_back(x);
        continue;
      }
      if (outOf && !into && x.port == kMainPort && from) {  // the joined wire: from where its input's wire starts
        Wire j = x;
        j.from = from->from;
        j.noteSource = from->noteSource;
        j.port = from->port;
        j.tap = from->tap;
        if (!joinedFirst.valid()) joinedFirst = j.id;
        next.push_back(std::move(j));
        continue;
      }
      if (from && x.id == from->id) continue;  // (aliased below)
      if (into && !outOf && x.param != kMainPort && runtimeParam(n, x.param)) {  // AP.2b: a cable into a param
        out.params.push_back(x.id);
        next.push_back(x);
        continue;
      }
      out.dropped.push_back(x.id);
    }
    if (from) {
      if (joinedFirst.valid()) alias.emplace_back(from->id, joinedFirst);
      else out.dropped.push_back(from->id);  // nothing goes on from it
    }
    w = std::move(next);
  }
  // A chain of note devices: an alias of an alias plays as the last one.
  for (auto& [a, b] : alias)
    for (size_t guard = 0; guard <= alias.size(); ++guard) {
      const auto it = std::find_if(alias.begin(), alias.end(), [&](const auto& x) { return x.first == b; });
      if (it == alias.end()) break;
      b = it->second;
    }
  // A dropped wire stays dropped even when it was aliased first (then dissolved away by a later device).
  std::sort(out.dropped.begin(), out.dropped.end());
  out.dropped.erase(std::unique(out.dropped.begin(), out.dropped.end()), out.dropped.end());
  for (const auto& [a, b] : alias)
    if (std::binary_search(out.dropped.begin(), out.dropped.end(), b)) out.dropped.push_back(a);
    else out.aliases.emplace_back(a, b);
  std::sort(out.dropped.begin(), out.dropped.end());
  out.dropped.erase(std::unique(out.dropped.begin(), out.dropped.end()), out.dropped.end());
  std::sort(out.aliases.begin(), out.aliases.end());
  std::sort(out.params.begin(), out.params.end());
  if (!w.empty()) out.wires = std::make_shared<const WireList>(std::move(w));
  return out;
}

Project withoutNoteDevices(const Project& p, const NoteDissolve& how) {
  if (!how.any) return p;
  Project q = p;
  std::function<TrackPtr(const TrackPtr&)> walk = [&](const TrackPtr& tp) -> TrackPtr {
    bool has = false;
    for (const DevicePtr& d : tp->devices) has = has || isNoteDevice(*d);
    bool kids = false;
    std::vector<TrackPtr> children = tp->children;
    for (TrackPtr& ch : children) {
      TrackPtr n = walk(ch);
      kids = kids || n != ch;
      ch = std::move(n);
    }
    if (!has && !kids) return tp;
    auto nt = std::make_shared<Track>(*tp);
    if (has) std::erase_if(nt->devices, [](const DevicePtr& d) { return isNoteDevice(*d); });
    nt->children = std::move(children);
    return nt;
  };
  for (TrackPtr& t : q.tracks) t = walk(t);
  for (TrackPtr& t : q.returns) t = walk(t);
  if (q.master) q.master = walk(q.master);
  q.wires = how.wires;
  return q;
}

double noteParamValue(const NoteParamSpec& s, double position) noexcept {
  double p = std::isfinite(position) ? std::clamp(position, 0.0, 1.0) : 0.0;
  if (s.taper > 0.0 && !s.isBool) p = engine::arpTaper(static_cast<float>(s.taper), static_cast<float>(p));  // AP.11
  double v = s.min + p * (s.max - s.min);
  if (s.isBool) return p > 0.5 ? 1.0 : 0.0;
  if (s.step > 0.0) v = s.min + std::round((v - s.min) / s.step) * s.step;
  return std::clamp(v, std::min(s.min, s.max), std::max(s.min, s.max));
}

double noteParamPosition(const NoteParamSpec& s, double value) noexcept {
  if (s.isBool) return value > 0.5 ? 1.0 : 0.0;
  const double span = s.max - s.min;
  if (!(std::abs(span) > 0.0) || !std::isfinite(value)) return 0.0;
  const double f = std::clamp((value - s.min) / span, 0.0, 1.0);
  return s.taper > 0.0 ? std::clamp(engine::arpUntaper(s.taper, f), 0.0, 1.0) : f;  // AP.11
}

double noteDeviceValue(const Device& d, const NoteParamSpec& s) noexcept {
  if (!s.runtime) {
    const DeviceValue* v = d.setting(s.name);
    return v != nullptr ? v->value : s.def;
  }
  const DeviceValue* v = d.param(s.name);
  return v != nullptr ? noteParamValue(s, v->value) : s.def;
}

}  // namespace pq::doc
