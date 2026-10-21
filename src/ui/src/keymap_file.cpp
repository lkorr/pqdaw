// Keymap files: strict JSON reader and binding into a Keymap.

#include "pq/ui/keymap_file.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "pq/ui/text.h"

namespace pq::ui {

namespace {

using Json = nlohmann::json;

Error keymapError(std::string msg) { return Error{ErrorCode::Parse, "keymap: " + std::move(msg), {}}; }

bool validStatus(std::string_view s) noexcept {
  if (s == "implemented") return true;
  constexpr std::string_view kPrefix = "deferred:M";
  if (s.rfind(kPrefix, 0) != 0 || s.size() == kPrefix.size()) return false;
  for (char c : s.substr(kPrefix.size()))
    if (c < '0' || c > '9') return false;
  return true;
}

}  // namespace

Result<KeymapFile> parseKeymapFile(std::string_view text) {
  Json j;
  try {
    j = Json::parse(text.begin(), text.end());
  } catch (const Json::exception& e) {  // parsing boundary: exceptions stop here
    return keymapError(std::string("invalid JSON: ") + e.what());
  }
  if (!j.is_object()) return keymapError("the file must be a JSON object");
  for (auto it = j.begin(); it != j.end(); ++it)
    if (it.key() != "name" && it.key() != "bindings") return keymapError("unknown key \"" + it.key() + "\"");
  if (!j.contains("name") || !j["name"].is_string()) return keymapError("\"name\" must be a string");
  if (!j.contains("bindings") || !j["bindings"].is_array()) return keymapError("\"bindings\" must be an array");
  KeymapFile out;
  out.name = j["name"].get<std::string>();
  std::set<std::pair<std::string, uint64_t>> seen;  // (scope, mods << 32 | key)
  size_t index = 0;
  for (const Json& b : j["bindings"]) {
    const std::string where = "bindings[" + std::to_string(index++) + "]";
    if (!b.is_object()) return keymapError(where + " must be an object");
    for (auto it = b.begin(); it != b.end(); ++it)
      if (it.key() != "scope" && it.key() != "keys" && it.key() != "action" && it.key() != "status" && it.key() != "task")
        return keymapError(where + ": unknown key \"" + it.key() + "\"");
    for (const char* k : {"scope", "keys", "action", "status"})
      if (!b.contains(k) || !b[k].is_string() || b[k].get<std::string>().empty())
        return keymapError(where + ": \"" + k + "\" must be a non-empty string");
    KeymapEntry e;
    e.scope = b["scope"].get<std::string>();
    e.keys = b["keys"].get<std::string>();
    e.action = b["action"].get<std::string>();
    e.status = b["status"].get<std::string>();
    if (b.contains("task")) {
      if (!b["task"].is_string()) return keymapError(where + ": \"task\" must be a string");
      e.task = b["task"].get<std::string>();
    }
    auto chord = parseChord(e.keys);
    if (!chord) return keymapError(where + ": chord \"" + e.keys + "\": " + chord.error().message);
    e.chord = *chord;
    if (isModifierKey(e.chord.key)) return keymapError(where + ": chord \"" + e.keys + "\" has no key");
    if (!validStatus(e.status))
      return keymapError(where + ": status \"" + e.status + "\" must be \"implemented\" or \"deferred:M<n>\"");
    const uint64_t k = (uint64_t(e.chord.mods) << 32) | e.chord.key;
    if (!seen.insert({e.scope, k}).second)
      return keymapError(where + ": chord \"" + formatChord(e.chord) + "\" is bound twice in scope \"" + e.scope + "\"");
    out.entries.push_back(std::move(e));
  }
  return out;
}

Result<KeymapFile> loadKeymapFile(const std::string& path) {
  std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  if (!f) return Error{ErrorCode::Io, path + ": cannot open keymap file", {}};
  std::ostringstream ss;
  ss << f.rdbuf();
  auto k = parseKeymapFile(ss.str());
  if (!k) return Error{k.error().code, path + ": " + k.error().message, {}};
  return k;
}

std::string defaultKeymapPath() { return assetsDir() + "/keymaps/ableton.json"; }

Status applyKeymapFile(const KeymapFile& file, const ActionRegistry& actions, Keymap& keymap) {
  for (const KeymapEntry& e : file.entries)
    if (!e.deferred() && actions.find(e.action) == kNoAction)
      return keymapError("action \"" + e.action + "\" (" + e.keys + " in " + e.scope + ") is marked implemented but is not registered");
  for (const KeymapEntry& e : file.entries) {
    if (e.deferred()) continue;
    if (Status s = keymap.bind(keymap.scope(e.scope), e.chord, e.action); !s) return s;
  }
  return Ok{};
}

}  // namespace pq::ui
