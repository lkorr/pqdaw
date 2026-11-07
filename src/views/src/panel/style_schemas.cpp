// GU.8: style schemas (pq/views/panel/style_schemas.h).
#include "pq/views/panel/style_schemas.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "pq/document/library_files.h"
#include "pq/views/panel/element.h"

namespace pq::views::panel {

namespace {

const StyleSpec* specOf(std::string_view kind, std::string_view field) {
  const ElementSchema* s = elementSchema(kind);
  if (s == nullptr) return nullptr;
  for (const StyleSpec& f : s->style)
    if (f.name == field) return &f;
  return nullptr;
}
bool carried(std::string_view kind, std::string_view field) {
  const StyleSpec* f = specOf(kind, field);
  return f != nullptr && f->type != StyleSpec::Type::Text;
}

StyleSchema schema(std::string kind, std::string name, std::vector<doc::StyleField> fields) {
  std::sort(fields.begin(), fields.end(), [](const doc::StyleField& a, const doc::StyleField& b) { return a.name < b.name; });
  return StyleSchema{std::move(kind), std::move(name), std::move(fields), true};
}

std::string trimmed(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return std::string(s);
}

}  // namespace

std::vector<doc::StyleField> schemaFields(const doc::PanelElement& e) {
  std::vector<doc::StyleField> out;
  const ElementSchema* s = elementSchema(e.kind);
  if (s == nullptr) return out;
  for (const StyleSpec& f : s->style) {
    if (f.type == StyleSpec::Type::Text) continue;
    if (f.type == StyleSpec::Type::Number) out.push_back(doc::styleNumber(std::string(f.name), e.number(f.name, f.number)));
    else out.push_back(doc::styleText(std::string(f.name), std::string(e.text(f.name, f.text))));
  }
  std::sort(out.begin(), out.end(), [](const doc::StyleField& a, const doc::StyleField& b) { return a.name < b.name; });
  return out;
}

bool applyStyleSchema(const StyleSchema& s, doc::PanelElement& e) {
  if (e.kind != s.kind) return false;
  for (const doc::StyleField& f : s.fields)
    if (carried(e.kind, f.name)) e.setField(f);
  return true;
}

StyleSchemas& StyleSchemas::shared() {
  static StyleSchemas s;
  return s;
}

StyleSchemas::StyleSchemas() {
  using doc::styleNumber;
  using doc::styleText;
  builtIn_.push_back(schema("knob", "Classic",
                            {styleText("pointer", "arc"), styleNumber("track", 3.0), styleNumber("sweep", 270.0),
                             styleText("colour", "value"), styleText("readout", "below")}));
  builtIn_.push_back(schema("knob", "Minimal",
                            {styleText("pointer", "dot"), styleNumber("track", 1.5), styleNumber("sweep", 300.0),
                             styleText("colour", "ink"), styleText("readout", "none")}));
  builtIn_.push_back(schema("knob", "Pie",
                            {styleText("pointer", "pie"), styleNumber("track", 3.0), styleNumber("sweep", 360.0),
                             styleText("colour", "signal"), styleText("readout", "inside")}));
  builtIn_.push_back(schema("slider", "Classic",
                            {styleText("orientation", "auto"), styleNumber("track", 4.0), styleText("handle", "bar"),
                             styleText("colour", "value"), styleText("readout", "below")}));
  builtIn_.push_back(schema("slider", "Minimal",
                            {styleText("orientation", "auto"), styleNumber("track", 1.5), styleText("handle", "dot"),
                             styleText("colour", "ink"), styleText("readout", "none")}));
}

std::vector<const StyleSchema*> StyleSchemas::forKind(std::string_view kind) const {
  std::vector<const StyleSchema*> out;
  for (const StyleSchema& s : builtIn_)
    if (s.kind == kind) out.push_back(&s);
  for (const StyleSchema& s : user_)
    if (s.kind == kind) out.push_back(&s);
  return out;
}

const StyleSchema* StyleSchemas::find(std::string_view kind, std::string_view name) const {
  for (const StyleSchema* s : forKind(kind))
    if (s->name == name) return s;
  return nullptr;
}

Result<std::string> StyleSchemas::save(const doc::PanelElement& e, std::string_view name) {
  if (elementSchema(e.kind) == nullptr || schemaFields(e).empty())
    return Error{ErrorCode::InvalidArgument, "a " + e.kind + " element has no style fields to save", {}};
  std::string n = trimmed(name);
  if (n.empty()) n = "Style";
  auto builtInNamed = [&](std::string_view x) {
    return std::any_of(builtIn_.begin(), builtIn_.end(), [&](const StyleSchema& s) { return s.kind == e.kind && s.name == x; });
  };
  if (builtInNamed(n)) {
    const std::string base = n;
    for (int k = 2; builtInNamed(n); ++k) n = base + " " + std::to_string(k);
  }
  StyleSchema s{e.kind, n, schemaFields(e), false};
  const auto it = std::find_if(user_.begin(), user_.end(), [&](const StyleSchema& x) { return x.kind == e.kind && x.name == n; });
  if (it != user_.end()) *it = std::move(s);
  else {
    user_.push_back(std::move(s));
    std::stable_sort(user_.begin(), user_.end(), [](const StyleSchema& a, const StyleSchema& b) {
      return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
    });
  }
  if (Status st = write(); !st) return std::move(st).error();
  return n;
}

Status StyleSchemas::setFile(std::string pathUtf8) {
  file_ = std::move(pathUtf8);
  user_.clear();
  if (file_.empty()) return Ok{};
  auto text = doc::readTextFile(file_);
  if (!text) return text.error().code == ErrorCode::NotFound ? Status(Ok{}) : Status(std::move(text).error());
  const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("schemas") || !j["schemas"].is_array())
    return Error{ErrorCode::Parse, "style schemas: " + file_ + " is not a schema file", {}};
  for (const nlohmann::json& s : j["schemas"]) {
    if (!s.is_object() || !s.value("kind", nlohmann::json()).is_string() || !s.value("name", nlohmann::json()).is_string()) continue;
    StyleSchema x{s["kind"].get<std::string>(), s["name"].get<std::string>(), {}, false};
    if (const auto f = s.find("fields"); f != s.end() && f->is_object())
      for (const auto& [k, v] : f->items()) {
        if (!carried(x.kind, k)) continue;
        if (v.is_number()) x.fields.push_back(doc::styleNumber(k, v.get<double>()));
        else if (v.is_string()) x.fields.push_back(doc::styleText(k, v.get<std::string>()));
      }
    std::sort(x.fields.begin(), x.fields.end(), [](const doc::StyleField& a, const doc::StyleField& b) { return a.name < b.name; });
    if (!x.fields.empty() && find(x.kind, x.name) == nullptr) user_.push_back(std::move(x));
  }
  return Ok{};
}

void StyleSchemas::clearUser() { user_.clear(); }

Status StyleSchemas::write() const {
  if (file_.empty()) return Ok{};
  nlohmann::json list = nlohmann::json::array();
  for (const StyleSchema& s : user_) {
    nlohmann::json f = nlohmann::json::object();
    for (const doc::StyleField& x : s.fields) f[x.name] = x.isText ? nlohmann::json(x.text) : nlohmann::json(x.number);
    list.push_back({{"kind", s.kind}, {"name", s.name}, {"fields", std::move(f)}});
  }
  const nlohmann::json j = {{"version", 1}, {"schemas", std::move(list)}};
  return doc::writeTextFileAtomic(file_, j.dump(2));
}

}  // namespace pq::views::panel
