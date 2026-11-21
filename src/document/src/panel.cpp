// GU.3: group panels (pq/document/panel.h).
#include "pq/document/panel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_set>

#include "pq/document/model.h"

namespace pq::doc {

namespace {

Error bad(std::string m) { return Error{ErrorCode::InvalidArgument, std::move(m), {}}; }

bool finite(float v) noexcept { return std::isfinite(v); }

std::string elementName(const PanelElement& e) { return "panel element " + std::to_string(e.id) + " (" + e.kind + ")"; }

}  // namespace

std::string_view bindingKindName(BindingKind k) noexcept {
  static constexpr std::array<std::string_view, kBindingKindCount> kNames = {"none",   "control", "setting", "data",
                                                                             "signal", "panel",   "input"};
  const auto i = static_cast<size_t>(k);
  return i < kNames.size() ? kNames[i] : std::string_view("none");
}

Binding controlBinding(DeviceId device, std::string param, std::string meta) {
  return Binding{BindingKind::Control, device, std::move(meta), std::move(param)};
}
Binding activeBinding(DeviceId device) { return Binding{BindingKind::Setting, device, {}, std::string(kActiveSetting)}; }
Binding settingBinding(DeviceId device, std::string setting, std::string meta) {
  return Binding{BindingKind::Setting, device, std::move(meta), std::move(setting)};
}
Binding signalBinding(DeviceId device, std::string output) { return Binding{BindingKind::Signal, device, {}, std::move(output)}; }
Binding panelBinding(DeviceId group) { return Binding{BindingKind::Panel, group, {}, {}}; }
Binding dataBinding(DeviceId device, std::string meta) { return Binding{BindingKind::Data, device, std::move(meta), {}}; }
Binding inputBinding(DeviceId device, std::string input) { return Binding{BindingKind::Input, device, {}, std::move(input)}; }

EmbedFit embedFit(float w, float h, float childW, float childH) noexcept {
  EmbedFit f;
  if (!(childW > 0.0f) || !(childH > 0.0f) || !(w > 0.0f) || !(h > 0.0f)) return f;
  f.k = std::min(w / childW, h / childH);
  f.x = (w - childW * f.k) * 0.5f;
  f.y = (h - childH * f.k) * 0.5f;
  return f;
}

DeviceId embeddedGroup(const PanelElement& e) noexcept {
  const Binding* b = e.binding();
  return b != nullptr && b->kind == BindingKind::Panel && e.kind == kEmbedKind ? b->device : DeviceId{};
}

StyleField styleNumber(std::string name, double v) {
  StyleField f;
  f.name = std::move(name);
  f.number = v;
  return f;
}
StyleField styleText(std::string name, std::string v) {
  StyleField f;
  f.name = std::move(name);
  f.text = std::move(v);
  f.isText = true;
  return f;
}

const StyleField* PanelElement::field(std::string_view name) const noexcept {
  const auto it = std::lower_bound(style.begin(), style.end(), name,
                                   [](const StyleField& f, std::string_view n) { return f.name < n; });
  return it != style.end() && it->name == name ? &*it : nullptr;
}
double PanelElement::number(std::string_view name, double def) const noexcept {
  const StyleField* f = field(name);
  return f != nullptr && !f->isText ? f->number : def;
}
std::string_view PanelElement::text(std::string_view name, std::string_view def) const noexcept {
  const StyleField* f = field(name);
  return f != nullptr && f->isText ? std::string_view(f->text) : def;
}
void PanelElement::setField(StyleField f) {
  const auto it = std::lower_bound(style.begin(), style.end(), f.name,
                                   [](const StyleField& a, const std::string& n) { return a.name < n; });
  if (it != style.end() && it->name == f.name) *it = std::move(f);
  else style.insert(it, std::move(f));
}

const PanelElement* Panel::find(uint32_t id) const noexcept {
  const int32_t i = indexOf(id);
  return i < 0 ? nullptr : &elements[static_cast<size_t>(i)];
}
int32_t Panel::indexOf(uint32_t id) const noexcept {
  for (size_t i = 0; i < elements.size(); ++i)
    if (elements[i].id == id) return static_cast<int32_t>(i);
  return -1;
}
uint32_t Panel::nextId() const noexcept {
  uint32_t m = 0;
  for (const PanelElement& e : elements) m = std::max(m, e.id);
  return m + 1;
}

bool foldOpenByDefault(const PanelElement& e) noexcept { return e.number("open", 1.0) >= 0.5; }

FoldLayout foldLayout(const Panel& p, const std::function<bool(const PanelElement&)>& open,
                      const std::function<bool(const PanelElement&)>& shown) {
  FoldLayout out;
  out.height = p.height;
  out.width = p.width;
  std::vector<size_t> folds;
  for (size_t i = 0; i < p.elements.size(); ++i)
    if (p.elements[i].kind == kFoldKind) folds.push_back(i);
  if (folds.empty()) return out;
  out.folds = true;
  std::stable_sort(folds.begin(), folds.end(), [&](size_t a, size_t b) { return p.elements[a].y < p.elements[b].y; });
  struct Band {
    uint32_t id;
    float head, top, end;  // the header's top; the section: [the header's bottom, the next fold's top)
    bool closed;           // folded (its section hidden)
    bool gone = false;     // SM.11: hidden itself (its condition fails, or its parent fold is folded / gone)
    int32_t parent = -1;   // the band of its "parent" fold
  };
  std::vector<Band> bands;
  for (size_t k = 0; k < folds.size(); ++k) {
    const PanelElement& f = p.elements[folds[k]];
    const float end = k + 1 < folds.size() ? p.elements[folds[k + 1]].y : std::max(p.height, f.y + f.h);
    bands.push_back({f.id, f.y, f.y + f.h, std::max(f.y + f.h, end), !open(f)});
  }
  const auto bandOf = [&](double id) -> int32_t {
    if (!(id >= 1.0)) return -1;
    for (size_t k = 0; k < bands.size(); ++k)
      if (bands[k].id == static_cast<uint32_t>(id)) return static_cast<int32_t>(k);
    return -1;
  };
  for (size_t k = 0; k < bands.size(); ++k) {
    const PanelElement& f = p.elements[folds[k]];
    bands[k].parent = bandOf(f.number(kFoldParentField, 0.0));
    bands[k].gone = shown && !shown(f);
  }
  // A fold whose parent is folded or gone is gone (parents resolved along the chain; a cycle stops at its length).
  for (size_t pass = 0; pass < bands.size(); ++pass) {
    bool changed = false;
    for (Band& b : bands)
      if (!b.gone && b.parent >= 0 && b.parent != static_cast<int32_t>(&b - bands.data())) {
        const Band& up = bands[static_cast<size_t>(b.parent)];
        if (up.closed || up.gone) b.gone = changed = true;
      }
    if (!changed) break;
  }
  constexpr float kEdge = 0.5f;  // an element starting half a unit above a header's bottom is in its section
  // What the folded bands (a gone one: with its header) take out above v (fit only).
  const auto taken = [&](float v) {
    float d = 0.0f;
    if (p.fit)
      for (const Band& b : bands) {
        if (b.gone) d += std::clamp(v - b.head, 0.0f, b.end - b.head);
        else if (b.closed) d += std::clamp(v - b.top, 0.0f, b.end - b.top);
      }
    return d;
  };
  const size_t n = p.elements.size();
  out.y.resize(n);
  out.h.resize(n);
  out.hidden.assign(n, 0);
  float right = 0.0f, all = 0.0f;  // the shown elements' right edge, every element's
  for (size_t i = 0; i < n; ++i) {
    const PanelElement& e = p.elements[i];
    if (e.kind == kFoldKind) {
      if (const int32_t k = bandOf(e.id); k >= 0) out.hidden[i] = bands[static_cast<size_t>(k)].gone ? 1 : 0;
    } else if (const int32_t own = bandOf(e.number(kFoldSectionField, 0.0)); own >= 0) {  // SM.11: owned by name
      const Band& b = bands[static_cast<size_t>(own)];
      out.hidden[i] = b.closed || b.gone ? 1 : 0;
    } else {
      for (const Band& b : bands) {
        if (e.y >= b.top - kEdge && e.y < b.end) out.hidden[i] = b.closed || b.gone ? 1 : 0;
        else if (e.y >= b.head - kEdge && e.y < b.top - kEdge) out.hidden[i] = b.gone ? 1 : 0;  // beside its header
      }
    }
    const float y0 = e.y - taken(e.y), y1 = e.y + e.h - taken(e.y + e.h);
    out.y[i] = y0;
    out.h[i] = out.hidden[i] ? e.h : std::max(1.0f, y1 - y0);
    if (!out.hidden[i]) right = std::max(right, e.x + e.w);
    all = std::max(all, e.x + e.w);
  }
  out.height = std::max(1.0f, p.height - taken(p.height));
  // Fit: only what hidden elements alone reach is taken off the right (the panel's own margin kept).
  if (p.fit && right < all - 0.5f) out.width = std::clamp(p.width - (all - right), 1.0f, std::max(1.0f, p.width));
  return out;
}

Status checkPanel(const Panel& p) {
  if (!finite(p.width) || !finite(p.height) || p.width < 0.0f || p.height < 0.0f || p.width > kMaxPanelSize ||
      p.height > kMaxPanelSize)
    return bad("a panel's size is 0 … " + std::to_string(static_cast<int>(kMaxPanelSize)));
  if (p.elements.empty()) return Ok{};
  if (!(p.width > 0.0f) || !(p.height > 0.0f)) return bad("a panel with elements has a size");
  if (p.elements.size() > kMaxPanelElements) return bad("a panel holds at most " + std::to_string(kMaxPanelElements) + " elements");
  std::unordered_set<uint32_t> ids;
  for (const PanelElement& e : p.elements) {
    if (e.id == 0) return bad("a panel element's id is ≥ 1");
    if (!ids.insert(e.id).second) return bad("panel element id " + std::to_string(e.id) + " twice");
  }
  for (const PanelElement& e : p.elements) {
    if (e.kind.empty()) return bad("panel element " + std::to_string(e.id) + " has no kind");
    if (!finite(e.x) || !finite(e.y) || !finite(e.w) || !finite(e.h) || !(e.w > 0.0f) || !(e.h > 0.0f) ||
        std::fabs(e.x) > kMaxPanelSize || std::fabs(e.y) > kMaxPanelSize || e.w > kMaxPanelSize || e.h > kMaxPanelSize)
      return bad(elementName(e) + ": its rect is finite, with a width and height > 0");
    for (size_t i = 0; i < e.style.size(); ++i) {
      const StyleField& f = e.style[i];
      if (f.name.empty()) return bad(elementName(e) + ": a style field has no name");
      if (i > 0 && !(e.style[i - 1].name < f.name)) return bad(elementName(e) + ": style fields are sorted by name, one each");
      if (!f.isText && !std::isfinite(f.number)) return bad(elementName(e) + ": style field '" + f.name + "' is not finite");
      if (!f.isText && !f.text.empty()) return bad(elementName(e) + ": style field '" + f.name + "' is a number");
    }
    for (const Binding& b : e.bindings) {
      switch (b.kind) {
        case BindingKind::None:
          if (b.device.valid() || !b.name.empty() || !b.meta.empty()) return bad(elementName(e) + ": an unbound slot names nothing");
          break;
        case BindingKind::Control:
        case BindingKind::Setting:
        case BindingKind::Signal:
          if (!b.device.valid() || b.name.empty())
            return bad(elementName(e) + ": a " + std::string(bindingKindName(b.kind)) + " binding names a block and a " +
                       (b.kind == BindingKind::Signal ? "port" : "name"));
          break;
        case BindingKind::Data:
        case BindingKind::Panel:
          if (!b.device.valid()) return bad(elementName(e) + ": a " + std::string(bindingKindName(b.kind)) + " binding names a block");
          if (!b.name.empty()) return bad(elementName(e) + ": a " + std::string(bindingKindName(b.kind)) + " binding has no name");
          if (b.kind == BindingKind::Panel && !b.meta.empty()) return bad(elementName(e) + ": a panel binding names a group (no meta path)");
          break;
        case BindingKind::Input:  // DY.2 (name "": the main input)
          if (!b.device.valid() || !b.meta.empty()) return bad(elementName(e) + ": an input binding names a block (no meta path)");
          break;
      }
    }
    if (e.when)
      for (size_t t = 0; t < e.when->terms(); ++t) {  // AP.6: every term of a condition of several sources
        const uint32_t src = e.when->termElement(t);
        if (src == e.id) return bad(elementName(e) + ": its condition names itself");
        if (ids.count(src) == 0) return bad(elementName(e) + ": its condition names element " + std::to_string(src) + ", not on the panel");
        if (e.when->termValues(t).empty()) return bad(elementName(e) + ": its condition has no values");
        for (const double v : e.when->termValues(t))
          if (!std::isfinite(v)) return bad(elementName(e) + ": its condition's values are finite");
      }
  }
  return Ok{};
}

bool bindingAllowed(const std::vector<DevicePtr>& list, DeviceId group, const Binding& b) noexcept {
  if (!b.bound()) return true;
  if (b.device == group) return b.kind != BindingKind::Panel;
  if (!groupContains(list, group, b.device)) return false;
  if (b.kind == BindingKind::Panel) {
    for (const DevicePtr& d : list)
      if (d->id == b.device) return isGroupDevice(*d);
    return false;
  }
  return true;
}

std::string bindingsProblem(const std::vector<DevicePtr>& list, DeviceId group, const Panel& panel) {
  for (const PanelElement& e : panel.elements)
    for (size_t s = 0; s < e.bindings.size(); ++s)
      if (!bindingAllowed(list, group, e.bindings[s]))
        return elementName(e) + ": binding " + std::to_string(s) + " (" + std::string(bindingKindName(e.bindings[s].kind)) +
               " of device " + std::to_string(e.bindings[s].device.value) +
               ") is not inside the group (a panel binds what is inside its group, or the group's own settings)";
  return {};
}

PanelPtr prunePanel(const PanelPtr& panel, const std::vector<DevicePtr>& list, DeviceId group) {
  if (!panel) return panel;
  // What is inside the group (one walk of the list), so each binding is a lookup (bindingAllowed's rule).
  std::unordered_set<uint32_t> inside, groups;
  for (const Device* d : groupMembersDeep(list, group)) {
    inside.insert(d->id.value);
    if (isGroupDevice(*d)) groups.insert(d->id.value);
  }
  auto allowed = [&](const Binding& b) {
    if (!b.bound()) return true;
    if (b.device == group) return b.kind != BindingKind::Panel;
    return b.kind == BindingKind::Panel ? groups.count(b.device.value) != 0 : inside.count(b.device.value) != 0;
  };
  auto keep = [&](const PanelElement& e) {
    for (const Binding& b : e.bindings)
      if (!allowed(b)) return false;
    return true;
  };
  if (std::all_of(panel->elements.begin(), panel->elements.end(), keep)) return panel;
  Panel out;
  out.width = panel->width;
  out.height = panel->height;
  out.fit = panel->fit;
  std::unordered_set<uint32_t> kept;
  for (const PanelElement& e : panel->elements)
    if (keep(e)) {
      out.elements.push_back(e);
      kept.insert(e.id);
    }
  for (PanelElement& e : out.elements)
    if (e.when && !remapVisibility(*e.when, [&](uint32_t id) { return kept.count(id) != 0 ? id : 0u; })) e.when.reset();
  if (out.elements.empty()) return nullptr;
  return std::make_shared<const Panel>(std::move(out));
}

bool remapVisibility(Visibility& v, const std::function<uint32_t(uint32_t)>& map) {
  std::vector<VisibilityTerm> terms;
  for (size_t t = 0; t < v.terms(); ++t)
    if (const uint32_t id = map(v.termElement(t)); id != 0) terms.push_back({id, v.termValues(t)});
  if (terms.empty()) return false;
  v.element = terms.front().element;
  v.values = std::move(terms.front().values);
  v.more.assign(std::make_move_iterator(terms.begin() + 1), std::make_move_iterator(terms.end()));
  return true;
}

Panel remapPanel(const Panel& p, std::span<const std::pair<DeviceId, DeviceId>> map) {
  Panel out = p;
  for (PanelElement& e : out.elements)
    for (Binding& b : e.bindings) {
      if (!b.device.valid()) continue;
      for (const auto& [from, to] : map)
        if (from == b.device) {
          b.device = to;
          break;
        }
    }
  return out;
}

ElementTap elementTap(std::string_view kind) noexcept {
  if (kind == "orbit") return ElementTap::Voice;
  if (kind == "scope" || kind == "spectrum" || kind == "spectrogram" || kind == "meter" || kind == "response" || kind == "transfer" ||
      kind == "levelmeter" || kind == "lufsmeter" || kind == "loudnesshistory" ||  // VZ.4
      kind == "stereofield" || kind == "stereoimage" || kind == "phasescope" ||    // VZ.5
      kind == "bandlevels" || kind == "waveform" ||                                   // VZ.6
      kind == "waterfall" || kind == "stereospectrogram")                          // VZ.7
    return ElementTap::Signal;
  return ElementTap::None;
}

std::string tapPath(ElementTap kind, std::string_view port) {
  return std::string(kind == ElementTap::Voice ? "#voice:" : "#signal:") + std::string(port);
}

std::vector<std::pair<DeviceId, std::string>> elementTraces(const PanelElement& e) {
  std::vector<std::pair<DeviceId, std::string>> out;
  if (e.kind != "scope") return out;
  auto signal = [&](size_t slot) {
    const Binding* b = e.binding(slot);
    return b != nullptr && b->kind == BindingKind::Signal && b->device.valid() && !b->name.empty() ? b : nullptr;
  };
  if (signal(0) == nullptr) return out;
  for (size_t slot = 1; slot < 3; ++slot) {
    const Binding* b = signal(slot);
    if (b == nullptr) break;
    out.emplace_back(b->device, b->name);
  }
  return out;
}

std::string traceTapPath(const PanelTap& t) {
  if (t.traces.empty()) return {};
  std::string path = "#trace:" + t.port;
  for (const auto& [d, port] : t.traces) path += "|" + std::to_string(d.value) + ":" + port;
  return path;
}

std::string traceTapPath(const PanelElement& e) {
  const Binding* b = e.binding(0);
  if (b == nullptr) return {};
  return traceTapPath(PanelTap{b->device, b->name, ElementTap::Signal, elementTraces(e)});
}

std::vector<PanelTap> panelTaps(const Panel& p) {
  std::vector<PanelTap> out;
  for (const PanelElement& e : p.elements) {
    const ElementTap k = elementTap(e.kind);
    // MX.2: a trace element (several signals on one time base) is one tap of them all.
    if (std::vector<std::pair<DeviceId, std::string>> traces = elementTraces(e); !traces.empty()) {
      out.push_back(PanelTap{e.binding(0)->device, e.binding(0)->name, ElementTap::Signal, std::move(traces)});
      continue;
    }
    // (EQ.4: a response display's analyser also shows its input spectrum: slot 1)
    for (size_t slot = 0; slot < (e.kind == "response" ? 2u : 1u); ++slot) {
      const Binding* b = e.binding(slot);
      if (k == ElementTap::None || b == nullptr || b->kind != BindingKind::Signal || !b->device.valid() || b->name.empty()) continue;
      out.push_back(PanelTap{b->device, b->name, k});
    }
  }
  std::sort(out.begin(), out.end(), [](const PanelTap& a, const PanelTap& b) {
    if (a.device != b.device) return a.device < b.device;
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.port != b.port) return a.port < b.port;
    return a.traces < b.traces;
  });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool panelBinds(const Panel& p, DeviceId d) noexcept {
  for (const PanelElement& e : p.elements)
    for (const Binding& b : e.bindings)
      if (b.bound() && b.device == d) return true;
  return false;
}

std::span<const SettingSpec> groupSettings() noexcept {
  static constexpr std::array<std::string_view, 3> kSteal = {"Oldest", "Quietest", "None"};
  static const std::array<SettingSpec, 5> kSpecs = {{
      {kGroupVoices, "Voices", 1.0, 128.0, 1.0, static_cast<double>(kDefaultGroupVoices), {}, ""},
      {kGroupMono, "Mono", 0.0, 1.0, 1.0, 0.0, {}, ""},
      {kGroupLegato, "Legato", 0.0, 1.0, 1.0, 0.0, {}, ""},
      {kGroupGlide, "Glide", 0.0, 10.0, 0.0, 0.0, {}, "s"},
      {kGroupSteal, "Steal", 0.0, 2.0, 1.0, 0.0, std::span<const std::string_view>(kSteal), ""},
  }};
  return kSpecs;
}

const SettingSpec& groupOversampleSetting() noexcept {
  // (the latencies: groupOversampleLatency, rounded)
  static constexpr std::array<std::string_view, 4> kFactors = {"1×", "2× · 3.6 smp", "4× · 4.7 smp", "8× · 5.2 smp"};
  static const SettingSpec kSpec{kGroupOversample, "Oversample", 0.0, static_cast<double>(kMaxGroupOversample), 1.0, 0.0,
                                 std::span<const std::string_view>(kFactors), ""};
  return kSpec;
}

const SettingSpec* groupSetting(std::string_view name) noexcept {
  if (name == kGroupOversample) return &groupOversampleSetting();
  for (const SettingSpec& s : groupSettings())
    if (s.name == name) return &s;
  return nullptr;
}

}  // namespace pq::doc
