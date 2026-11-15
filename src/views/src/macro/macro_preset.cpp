// Macro-mode presets (T8.1): see pq/views/macro/macro_preset.h.

#include "pq/views/macro/macro_preset.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "pq/tuning/monzo.h"
#include "pq/ui/text.h"

namespace pq::views {

namespace {

using nlohmann::json;

Error presetError(std::string what) { return Error{ErrorCode::Parse, "macro preset: " + std::move(what), {}}; }

struct PadKey {
  std::string_view name;
  ui::Scancode scancode;
  std::string_view label;
};
constexpr PadKey kPadKeys[] = {
    {"Numpad0", ui::Scancode::Kp0, "N0"},       {"Numpad1", ui::Scancode::Kp1, "N1"},
    {"Numpad2", ui::Scancode::Kp2, "N2"},       {"Numpad3", ui::Scancode::Kp3, "N3"},
    {"Numpad4", ui::Scancode::Kp4, "N4"},       {"Numpad5", ui::Scancode::Kp5, "N5"},
    {"Numpad6", ui::Scancode::Kp6, "N6"},       {"Numpad7", ui::Scancode::Kp7, "N7"},
    {"Numpad8", ui::Scancode::Kp8, "N8"},       {"Numpad9", ui::Scancode::Kp9, "N9"},
    {"NumpadDot", ui::Scancode::KpPeriod, "N."}, {"NumpadDiv", ui::Scancode::KpDivide, "N/"},
    {"NumpadMult", ui::Scancode::KpMultiply, "N*"}, {"NumpadSub", ui::Scancode::KpMinus, "N-"},
    {"NumpadAdd", ui::Scancode::KpPlus, "N+"},  {"NumpadEnter", ui::Scancode::KpEnter, "NEnter"},
    {"NumLock", ui::Scancode::NumLock, "NumLk"},
};

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto lo = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    if (lo(a[i]) != lo(b[i])) return false;
  }
  return true;
}

Result<MacroKey> parseKey(std::string_view name) {
  MacroKey k;
  k.name = std::string(name);
  for (const PadKey& p : kPadKeys)
    if (iequals(name, p.name)) {
      k.scancode = p.scancode;
      k.label = std::string(p.label);
      return k;
    }
  auto chord = ui::parseChord(name);
  if (!chord) return presetError("key \"" + std::string(name) + "\": " + chord.error().message);
  if (chord->mods != ui::mod::kNone || ui::isModifierKey(chord->key))
    return presetError("key \"" + std::string(name) + "\": macro keys take no modifiers");
  k.key = chord->key;
  k.label = name.size() == 1 ? std::string(1, static_cast<char>(chord->key)) : ui::formatChord(*chord);
  return k;
}

Result<tuning::Rational> parseRatio(std::string_view text) {
  const size_t slash = text.find('/');
  int64_t n = 0, d = 1;
  const std::string_view a = text.substr(0, slash);
  const auto ra = std::from_chars(a.data(), a.data() + a.size(), n);
  bool ok = ra.ec == std::errc{} && ra.ptr == a.data() + a.size() && !a.empty();
  if (ok && slash != std::string_view::npos) {
    const std::string_view b = text.substr(slash + 1);
    const auto rb = std::from_chars(b.data(), b.data() + b.size(), d);
    ok = rb.ec == std::errc{} && rb.ptr == b.data() + b.size() && !b.empty();
  }
  if (!ok || n <= 0 || d <= 0) return presetError("ratio \"" + std::string(text) + "\" is not a positive n/d");
  const auto r = tuning::Rational::tryMake(n, d);
  if (!r) return presetError("ratio \"" + std::string(text) + "\" is out of range");
  return *r;
}

struct OpName {
  std::string_view name;
  MacroOp op;
};
constexpr OpName kOps[] = {{"place", MacroOp::Place}, {"placeChain", MacroOp::PlaceChain}, {"back", MacroOp::Back},
                           {"left", MacroOp::Left},   {"right", MacroOp::Right},           {"up", MacroOp::Up},
                           {"down", MacroOp::Down},   {"undo", MacroOp::Undo},             {"redo", MacroOp::Redo},
                           {"layer", MacroOp::Layer}, {"lock", MacroOp::Lock}};

bool sameKey(const MacroKey& a, const MacroKey& b) noexcept { return a.key == b.key && a.scancode == b.scancode; }

}  // namespace

const MacroBinding* MacroPreset::find(const ui::KeyEvent& e) const noexcept {
  for (const MacroBinding& b : bindings)
    if (b.key.matches(e)) return &b;
  return nullptr;
}

std::string MacroPreset::keyLabel(MacroOp op, const tuning::Rational& ratio) const {
  for (const MacroBinding& b : bindings)
    if (b.op == op && (op != MacroOp::Interval || b.ratio == ratio)) return b.key.label;
  return {};
}

std::vector<tuning::KeyRatio> MacroPreset::intervalKeys() const {
  std::vector<tuning::KeyRatio> out;
  for (const MacroBinding& b : bindings)
    if (b.op == MacroOp::Interval) out.push_back({b.key.name, b.ratio});
  return out;
}

tuning::Val MacroPreset::valFor(const tuning::Edo& edo) const noexcept {
  if (edo.equaveRatio() == tuning::Rational{2})
    for (const EdoVal& v : vals)
      if (v.edo == edo.n()) return v.val;
  return tuning::patentVal(edo);
}

Result<MacroPreset> parseMacroPreset(std::string_view text, std::string id) {
  const json root = json::parse(text.begin(), text.end(), nullptr, false, true);
  if (root.is_discarded() || !root.is_object()) return presetError("not a JSON object");
  MacroPreset p;
  p.id = std::move(id);
  p.name = p.id;
  if (const auto it = root.find("name"); it != root.end()) {
    if (!it->is_string()) return presetError("\"name\" must be a string");
    p.name = it->get<std::string>();
  }
  if (const auto it = root.find("primeLimit"); it != root.end()) {
    if (!it->is_number_unsigned() || it->get<uint64_t>() < 2) return presetError("\"primeLimit\" must be an integer >= 2");
    p.primeLimit = it->get<uint64_t>();
  }
  auto add = [&p](MacroBinding b) -> Status {
    for (const MacroBinding& o : p.bindings)
      if (sameKey(o.key, b.key)) return presetError("key \"" + b.key.name + "\" is bound twice");
    p.bindings.push_back(std::move(b));
    return Ok{};
  };
  const auto iv = root.find("intervals");
  if (iv == root.end() || !iv->is_array() || iv->empty()) return presetError("\"intervals\" must be a non-empty array");
  for (const json& e : *iv) {
    if (!e.is_object() || !e.contains("key") || !e["key"].is_string() || !e.contains("ratio") || !e["ratio"].is_string())
      return presetError("an interval needs a \"key\" and a \"ratio\" string");
    auto key = parseKey(e["key"].get<std::string>());
    if (!key) return key.error();
    auto ratio = parseRatio(e["ratio"].get<std::string>());
    if (!ratio) return ratio.error();
    if (Status s = add({std::move(*key), MacroOp::Interval, *ratio}); !s) return s.error();
  }
  if (const auto ks = root.find("keys"); ks != root.end()) {
    if (!ks->is_object()) return presetError("\"keys\" must be an object");
    for (auto it = ks->begin(); it != ks->end(); ++it) {
      const auto op = std::find_if(std::begin(kOps), std::end(kOps), [&](const OpName& o) { return o.name == it.key(); });
      if (op == std::end(kOps)) return presetError("unknown command \"" + it.key() + "\"");
      if (!it->is_string()) return presetError("\"" + it.key() + "\" must be a key name");
      auto key = parseKey(it->get<std::string>());
      if (!key) return key.error();
      if (Status s = add({std::move(*key), op->op, tuning::Rational{1}}); !s) return s.error();
    }
  }
  if (const auto vs = root.find("vals"); vs != root.end()) {
    if (!vs->is_object()) return presetError("\"vals\" must be an object");
    for (auto it = vs->begin(); it != vs->end(); ++it) {
      MacroPreset::EdoVal v;
      const std::string& k = it.key();
      const auto r = std::from_chars(k.data(), k.data() + k.size(), v.edo);
      if (r.ec != std::errc{} || r.ptr != k.data() + k.size() || v.edo == 0)
        return presetError("val \"" + k + "\": the name must be an EDO size");
      if (!it->is_array() || it->size() != static_cast<size_t>(tuning::kValPrimeCount))
        return presetError("val \"" + k + "\" must list the steps of the " + std::to_string(tuning::kValPrimeCount) + " primes 2 … 31");
      for (int i = 0; i < tuning::kValPrimeCount; ++i) {
        const json& s = (*it)[static_cast<size_t>(i)];
        if (!s.is_number_integer()) return presetError("val \"" + k + "\" must hold integers");
        v.val.steps[static_cast<size_t>(i)] = s.get<int32_t>();
      }
      p.vals.push_back(v);
    }
  }
  return p;
}

std::string macroPresetPath(std::string_view id) { return ui::assetsDir() + "/macro/" + std::string(id) + ".json"; }

Result<MacroPreset> loadMacroPreset(std::string_view id) {
  const std::string path = macroPresetPath(id);
  std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  if (!f) return Error{ErrorCode::Io, "macro preset \"" + std::string(id) + "\": cannot open " + path, {}};
  std::ostringstream ss;
  ss << f.rdbuf();
  auto p = parseMacroPreset(ss.str(), std::string(id));
  if (!p) return Error{p.error().code, "\"" + std::string(id) + "\" " + p.error().message, {}};
  return p;
}

std::vector<std::string> listMacroPresets() {
  std::vector<std::string> out;
  const std::string dir = ui::assetsDir() + "/macro";
  std::error_code ec;
  for (std::filesystem::directory_iterator it(std::filesystem::path(std::u8string(dir.begin(), dir.end())), ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->path().extension() != ".json") continue;
    const std::u8string stem = it->path().stem().u8string();
    out.emplace_back(stem.begin(), stem.end());
  }
  std::sort(out.begin(), out.end());
  return out;
}

MacroPreset builtinMacroPreset() {
  MacroPreset p;
  p.id = std::string(kDefaultMacroPreset);
  p.name = "Trainer";
  p.primeLimit = 13;
  auto key = [](std::string_view name) {
    auto k = parseKey(name);
    return k ? std::move(*k) : MacroKey{};
  };
  for (const tuning::KeyRatio& k : tuning::trainerDefaultKeymap()) p.bindings.push_back({key(k.key), MacroOp::Interval, k.ratio});
  const std::pair<std::string_view, MacroOp> rest[] = {
      {"`", MacroOp::Place}, {"Tab", MacroOp::PlaceChain}, {"Backspace", MacroOp::Back}, {"a", MacroOp::Left},
      {"f", MacroOp::Right}, {"s", MacroOp::Up},           {"d", MacroOp::Down},         {"-", MacroOp::Undo},
      {"=", MacroOp::Redo},  {"g", MacroOp::Layer},        {"b", MacroOp::Lock}};
  for (const auto& [name, op] : rest) p.bindings.push_back({key(name), op, tuning::Rational{1}});
  return p;
}

}  // namespace pq::views
