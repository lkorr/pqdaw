// Panel descriptions (pq/views/devices/panel_desc.h, T4.6).

#include "pq/views/devices/panel_desc.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace pq::views::devices {

namespace {

using json = nlohmann::json;
using Values = std::map<std::string, std::string, std::less<>>;

Error panelError(const std::string& where, const std::string& what) {
  return Error{ErrorCode::InvalidArgument, "panel: " + where + ": " + what, {}};
}

// Every "{name}" in s.
std::vector<std::string> placeholders(std::string_view s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '{') continue;
    const size_t j = s.find('}', i);
    if (j == std::string_view::npos) break;
    out.emplace_back(s.substr(i + 1, j - i - 1));
    i = j;
  }
  return out;
}

void checkKeys(const json& o, std::initializer_list<const char*> allowed, const std::string& where) {
  for (auto it = o.begin(); it != o.end(); ++it) {
    bool ok = false;
    for (const char* a : allowed) ok = ok || it.key() == a;
    if (!ok) throw std::runtime_error(where + ": unknown key \"" + it.key() + "\"");
  }
}

ControlKind kindOf(const std::string& t, const std::string& where) {
  if (t == "knob") return ControlKind::Knob;
  if (t == "box") return ControlKind::Box;
  if (t == "toggle") return ControlKind::Toggle;
  if (t == "chooser") return ControlKind::Chooser;
  if (t == "label") return ControlKind::Label;
  if (t == "tabs") return ControlKind::Tabs;
  if (t == "visual") return ControlKind::Visual;
  throw std::runtime_error(where + ": unknown control type \"" + t + "\"");
}

std::filesystem::path utf8Path(std::string_view s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string pathText(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string substituteVar(std::string s, const std::string& var, const std::string& value) {
  const std::string key = "{" + var + "}";
  for (size_t p = s.find(key); p != std::string::npos; p = s.find(key, p + value.size())) s.replace(p, key.size(), value);
  return s;
}

json substituteJson(const json& j, const std::string& var, const std::string& value) {
  if (j.is_string()) return substituteVar(j.get<std::string>(), var, value);
  if (j.is_array()) {
    json a = json::array();
    for (const json& e : j) a.push_back(substituteJson(e, var, value));
    return a;
  }
  if (j.is_object()) {
    json o = json::object();
    for (auto it = j.begin(); it != j.end(); ++it) o[substituteVar(it.key(), var, value)] = substituteJson(it.value(), var, value);
    return o;
  }
  return j;
}

ControlDesc readControl(const json& c, const std::string& where) {
  checkKeys(c, {"type", "id", "param", "const", "label", "text", "info", "items", "values", "style", "selector", "visual", "bind", "rect", "when"},
            where);
  ControlDesc d;
  d.kind = kindOf(c.at("type").get<std::string>(), where);
  if (c.contains("id")) d.id = c["id"].get<std::string>();
  if (c.contains("param")) d.param = c["param"].get<std::string>();
  if (c.contains("const")) d.constName = c["const"].get<std::string>();
  if (c.contains("label")) d.label = c["label"].get<std::string>();
  if (c.contains("text")) d.text = c["text"].get<std::string>();
  if (c.contains("info")) d.info = c["info"].get<std::string>();
  if (c.contains("items")) d.items = c["items"].get<std::vector<std::string>>();
  if (c.contains("values")) d.values = c["values"].get<std::vector<double>>();
  if (c.contains("selector")) d.selector = c["selector"].get<std::string>();
  if (c.contains("style")) d.style = c["style"].get<std::string>();
  if (c.contains("visual")) d.visual = c["visual"].get<std::string>();
  if (c.contains("bind"))
    for (auto it = c["bind"].begin(); it != c["bind"].end(); ++it) d.bind[it.key()] = it.value().get<std::string>();
  const auto r = c.at("rect").get<std::vector<float>>();
  if (r.size() != 4 || r[2] <= 0 || r[3] <= 0) throw std::runtime_error(where + ": \"rect\" must be [x, y, w, h] with w, h > 0");
  d.x = r[0];
  d.y = r[1];
  d.w = r[2];
  d.h = r[3];
  if (c.contains("when"))
    for (const json& w : c["when"]) {
      checkKeys(w, {"param", "const", "selector", "in"}, where + ": when");
      Condition cond;
      if (w.contains("param")) {
        cond.source = Condition::Source::Param;
        cond.name = w["param"].get<std::string>();
      } else if (w.contains("const")) {
        cond.source = Condition::Source::Const;
        cond.name = w["const"].get<std::string>();
      } else if (w.contains("selector")) {
        cond.source = Condition::Source::Selector;
        cond.name = w["selector"].get<std::string>();
      } else throw std::runtime_error(where + ": a condition needs \"param\", \"const\" or \"selector\"");
      for (const json& v : w.at("in")) {
        if (cond.source == Condition::Source::Selector) cond.keys.push_back(v.get<std::string>());
        else if (v.is_boolean()) cond.values.push_back(v.get<bool>() ? 1.0 : 0.0);
        else cond.values.push_back(v.get<double>());
      }
      d.when.push_back(std::move(cond));
    }
  switch (d.kind) {
    case ControlKind::Knob:
    case ControlKind::Box:
      if (d.param.empty() && d.constName.empty()) throw std::runtime_error(where + ": a knob or box needs \"param\" or \"const\"");
      if (d.kind == ControlKind::Knob && d.param.empty()) throw std::runtime_error(where + ": a knob needs \"param\"");
      break;
    case ControlKind::Toggle:
      if (d.param.empty() && d.constName.empty()) throw std::runtime_error(where + ": a toggle needs \"param\" or \"const\"");
      if (d.values.empty()) d.values = {0.0, 1.0};
      if (d.values.size() != 2) throw std::runtime_error(where + ": a toggle's \"values\" are [off, on]");
      break;
    case ControlKind::Chooser:
      if (d.param.empty() && d.constName.empty()) throw std::runtime_error(where + ": a chooser needs \"param\" or \"const\"");
      if (d.items.empty()) throw std::runtime_error(where + ": a chooser needs \"items\"");
      if (d.values.empty())
        for (size_t i = 0; i < d.items.size(); ++i) d.values.push_back(static_cast<double>(i));
      if (d.values.size() != d.items.size()) throw std::runtime_error(where + ": \"values\" and \"items\" differ in length");
      if (!d.style.empty() && d.style != "filter") throw std::runtime_error(where + ": unknown chooser style \"" + d.style + "\"");
      break;
    case ControlKind::Tabs:
      if (d.selector.empty()) throw std::runtime_error(where + ": tabs need a \"selector\"");
      break;
    case ControlKind::Visual:
      if (d.visual != "transfer" && d.visual != "eq" && d.visual != "spectrum")
        throw std::runtime_error(where + ": unknown visual \"" + d.visual + "\"");
      break;
    case ControlKind::Label: break;
  }
  return d;
}

// Every combination of the selectors a string uses (≤ a few dozen), as substitution maps.
std::vector<Values> combinations(const PanelDesc& p, const std::vector<std::string>& names) {
  std::vector<Values> out{Values{}};
  for (const std::string& n : names) {
    const SelectorDesc* s = p.findSelector(n);
    if (!s) continue;
    std::vector<Values> next;
    for (const Values& v : out)
      for (const std::string& val : s->values) {
        Values w = v;
        w[n] = val;
        next.push_back(std::move(w));
      }
    out = std::move(next);
  }
  return out;
}

void checkBinding(const PanelDesc& p, const DeviceSignature& sig, const std::string& name, bool isConst, const std::string& where) {
  if (name.empty()) return;
  std::vector<std::string> sels = placeholders(name);
  for (const std::string& s : sels)
    if (!p.findSelector(s)) throw std::runtime_error(where + ": \"{" + s + "}\" is not a selector");
  for (const Values& v : combinations(p, sels)) {
    const std::string resolved = resolveTemplate(name, v);
    const bool ok = isConst ? sig.findConst(resolved) != nullptr : sig.findParam(resolved) != nullptr;
    if (!ok)
      throw std::runtime_error(where + ": \"" + resolved + "\" is not a " + (isConst ? "structural const" : "param") + " of " +
                               sig.patch);
  }
}

}  // namespace

const SelectorDesc* PanelDesc::findSelector(std::string_view name) const noexcept {
  for (const SelectorDesc& s : selectors)
    if (s.name == name) return &s;
  return nullptr;
}

std::string resolveTemplate(std::string_view s, const std::map<std::string, std::string, std::less<>>& values) {
  if (s.find('{') == std::string_view::npos) return std::string(s);
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '{') {
      const size_t j = s.find('}', i);
      if (j != std::string_view::npos) {
        const auto it = values.find(s.substr(i + 1, j - i - 1));
        if (it != values.end()) {
          out += it->second;
          i = j;
          continue;
        }
      }
    }
    out += s[i];
  }
  return out;
}

Result<PanelDesc> parsePanelDesc(std::string_view text, const DeviceSignature& sig) {
  try {
    const json j = json::parse(text.begin(), text.end(), nullptr, true, true);
    checkKeys(j, {"format", "patch", "title", "size", "selectors", "controls"}, "panel");
    if (j.at("format").get<std::string>() != "pqdaw-panel 1") return panelError("format", "expected \"pqdaw-panel 1\"");
    PanelDesc p;
    p.patch = j.at("patch").get<std::string>();
    if (p.patch != sig.patch) return panelError("patch", "\"" + p.patch + "\" does not match the device patch \"" + sig.patch + "\"");
    p.title = j.at("title").get<std::string>();
    const auto size = j.at("size").get<std::vector<float>>();
    if (size.size() != 2 || size[0] <= 0 || size[1] <= 0) return panelError("size", "expected [w, h]");
    p.width = size[0];
    p.height = size[1];
    if (j.contains("selectors"))
      for (const json& s : j["selectors"]) {
        checkKeys(s, {"name", "values", "default"}, "selectors");
        SelectorDesc d;
        d.name = s.at("name").get<std::string>();
        d.values = s.at("values").get<std::vector<std::string>>();
        if (d.values.empty()) return panelError("selectors", "\"" + d.name + "\" has no values");
        d.def = s.contains("default") ? s["default"].get<int32_t>() : 0;
        d.def = std::clamp(d.def, 0, static_cast<int32_t>(d.values.size()) - 1);
        p.selectors.push_back(std::move(d));
      }
    const json& controls = j.at("controls");
    for (size_t i = 0; i < controls.size(); ++i) {
      const json& c = controls[i];
      const std::string where = "controls[" + std::to_string(i) + "]";
      if (c.contains("repeat")) {
        checkKeys(c, {"repeat", "control"}, where);
        const json& r = c["repeat"];
        checkKeys(r, {"var", "values", "step"}, where + ": repeat");
        const std::string var = r.at("var").get<std::string>();
        const auto vals = r.at("values").get<std::vector<std::string>>();
        const auto step = r.at("step").get<std::vector<float>>();
        if (step.size() != 2) return panelError(where, "\"step\" is [dx, dy]");
        for (size_t k = 0; k < vals.size(); ++k) {
          ControlDesc d = readControl(substituteJson(c.at("control"), var, vals[k]), where + "[" + std::to_string(k) + "]");
          d.x += step[0] * static_cast<float>(k);
          d.y += step[1] * static_cast<float>(k);
          p.controls.push_back(std::move(d));
        }
      } else {
        p.controls.push_back(readControl(c, where));
      }
    }
    // Bindings, ids.
    std::set<std::string> ids;
    for (size_t i = 0; i < p.controls.size(); ++i) {
      ControlDesc& d = p.controls[i];
      const std::string where = "control " + std::to_string(i) + (d.id.empty() ? std::string() : " (" + d.id + ")");
      checkBinding(p, sig, d.param, false, where);
      checkBinding(p, sig, d.constName, true, where);
      for (const auto& [role, name] : d.bind) {
        if (!role.empty() && role[0] == '@') continue;  // an option ("@bands", "@range"), not a binding
        const bool isConst = !name.empty() && name[0] == '#';
        checkBinding(p, sig, isConst ? name.substr(1) : name, isConst, where + ": bind \"" + role + "\"");
      }
      for (const Condition& c : d.when) {
        if (c.source == Condition::Source::Selector) {
          if (!p.findSelector(c.name)) return panelError(where, "unknown selector \"" + c.name + "\"");
        } else checkBinding(p, sig, c.name, c.source == Condition::Source::Const, where + ": when");
      }
      if (d.kind == ControlKind::Tabs && !p.findSelector(d.selector)) return panelError(where, "unknown selector \"" + d.selector + "\"");
      if (d.id.empty()) {  // the binding (or visual) name; "#<index>" when that is taken
        const std::string base = !d.param.empty() ? d.param : !d.constName.empty() ? d.constName : d.kind == ControlKind::Visual ? d.visual : "label";
        d.id = ids.count(base) ? base + "#" + std::to_string(i) : base;
      }
      if (!ids.insert(d.id).second) return panelError(where, "duplicate id \"" + d.id + "\"");
    }
    return p;
  } catch (const std::exception& e) {
    std::string m = e.what();
    if (m.rfind("panel", 0) != 0) m = "panel: " + m;
    return Error{ErrorCode::InvalidArgument, m, {}};
  }
}

Result<PanelDesc> loadPanelDesc(const std::string& path, const DeviceSignature& sig) {
  std::ifstream f(utf8Path(path), std::ios::binary);
  if (!f) return Error{ErrorCode::NotFound, "panel: cannot read " + path, {}};
  std::stringstream ss;
  ss << f.rdbuf();
  auto r = parsePanelDesc(ss.str(), sig);
  if (!r) return Error{r.error().code, path + ": " + r.error().message, {}};
  return r;
}

std::string panelPathFor(const std::string& devicePath) {
  std::filesystem::path p = utf8Path(devicePath);
  p.replace_extension(".panel.json");
  return pathText(p);
}

PanelDesc autoPanel(const DeviceSignature& sig, std::string title) {
  PanelDesc p;
  p.generated = true;
  p.patch = sig.patch;
  p.title = title.empty() ? sig.patch : std::move(title);
  // Columns 64 px wide; a knob takes a 56-px slot, a toggle or a box a 28-px one; three knob slots per column.
  constexpr float kCol = 64.0f, kKnobH = 56.0f, kSmallH = 28.0f, kColH = 3 * kKnobH, kPad = 6.0f;
  float x = kPad, y = 0.0f;
  auto place = [&](ControlDesc d, float h) {
    if (y + h > kColH) {
      x += kCol;
      y = 0.0f;
    }
    d.x = x;
    d.y = y;
    d.w = kCol - 8.0f;
    d.h = h - (h == kSmallH ? 6.0f : 0.0f);
    y += h;
    p.controls.push_back(std::move(d));
  };
  for (const ParamInfo& prm : sig.params) {
    if (prm.hidden) continue;
    ControlDesc d;
    d.param = prm.name;
    d.id = prm.name;
    if (prm.isBool) {
      d.kind = ControlKind::Toggle;
      d.text = prm.label;
      d.values = {0.0, 1.0};
      place(std::move(d), kSmallH);
    } else if (prm.steps > 1) {
      d.kind = ControlKind::Box;
      place(std::move(d), kKnobH);
    } else {
      d.kind = ControlKind::Knob;
      place(std::move(d), kKnobH);
    }
  }
  for (const ConstInfo& c : sig.consts) {
    ControlDesc d;
    d.kind = ControlKind::Box;
    d.constName = c.name;
    d.id = c.name;
    d.label = c.name;
    place(std::move(d), kKnobH);
  }
  p.width = x + kCol + kPad - 8.0f;
  p.height = kColH;
  return p;
}

}  // namespace pq::views::devices
