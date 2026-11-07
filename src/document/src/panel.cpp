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
  static constexpr std::array<std::string_view, kBindingKindCount> kNames = {"none", "control", "setting",
                                                                             "data", "signal",  "panel"};
  const auto i = static_cast<size_t>(k);
  return i < kNames.size() ? kNames[i] : std::string_view("none");
}

Binding controlBinding(DeviceId device, std::string param, std::string meta) {
  return Binding{BindingKind::Control, device, std::move(meta), std::move(param)};
}
Binding settingBinding(DeviceId device, std::string setting, std::string meta) {
  return Binding{BindingKind::Setting, device, std::move(meta), std::move(setting)};
}
Binding signalBinding(DeviceId device, std::string output) { return Binding{BindingKind::Signal, device, {}, std::move(output)}; }
Binding panelBinding(DeviceId group) { return Binding{BindingKind::Panel, group, {}, {}}; }
Binding dataBinding(DeviceId device, std::string meta) { return Binding{BindingKind::Data, device, std::move(meta), {}}; }

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
  return b != nullptr && b->kind == BindingKind::Panel ? b->device : DeviceId{};
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
      }
    }
    if (e.when) {
      if (e.when->element == e.id) return bad(elementName(e) + ": its condition names itself");
      if (ids.count(e.when->element) == 0)
        return bad(elementName(e) + ": its condition names element " + std::to_string(e.when->element) + ", not on the panel");
      if (e.when->values.empty()) return bad(elementName(e) + ": its condition has no values");
      for (const double v : e.when->values)
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
  std::unordered_set<uint32_t> kept;
  for (const PanelElement& e : panel->elements)
    if (keep(e)) {
      out.elements.push_back(e);
      kept.insert(e.id);
    }
  for (PanelElement& e : out.elements)
    if (e.when && kept.count(e.when->element) == 0) e.when.reset();
  if (out.elements.empty()) return nullptr;
  return std::make_shared<const Panel>(std::move(out));
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
  if (kind == "scope" || kind == "spectrum" || kind == "spectrogram" || kind == "meter") return ElementTap::Signal;
  return ElementTap::None;
}

std::string tapPath(ElementTap kind, std::string_view port) {
  return std::string(kind == ElementTap::Voice ? "#voice:" : "#signal:") + std::string(port);
}

std::vector<PanelTap> panelTaps(const Panel& p) {
  std::vector<PanelTap> out;
  for (const PanelElement& e : p.elements) {
    const ElementTap k = elementTap(e.kind);
    const Binding* b = e.binding(0);
    if (k == ElementTap::None || b == nullptr || b->kind != BindingKind::Signal || !b->device.valid() || b->name.empty()) continue;
    out.push_back(PanelTap{b->device, b->name, k});
  }
  std::sort(out.begin(), out.end(), [](const PanelTap& a, const PanelTap& b) {
    if (a.device != b.device) return a.device < b.device;
    if (a.kind != b.kind) return a.kind < b.kind;
    return a.port < b.port;
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

const SettingSpec* groupSetting(std::string_view name) noexcept {
  for (const SettingSpec& s : groupSettings())
    if (s.name == name) return &s;
  return nullptr;
}

}  // namespace pq::doc
