#pragma once

// Keymap files (T3.2): the default bindings are data in assets/keymaps/ableton.json (SPEC §6.1, Live 12's
// shortcuts → pqdaw action ids; docs/HOTKEYS.md lists them with their Live manual section and status).
//
//   { "name": "...",
//     "bindings": [ { "scope": "global", "keys": "Ctrl+D", "action": "edit.duplicate",
//                     "status": "deferred:M3", "task": "T3.9" }, ... ] }
//
// status is "implemented" (the action must be registered when the keymap is applied) or "deferred:M<n>" (the
// milestone that implements it; the chord is reserved but not bound yet). `task` names the TASKS.md task (free
// text, optional). Reading is strict: every chord must parse (pq/ui/input.h parseChord), a chord may appear
// once per scope, and unknown keys are errors.

#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/ui/actions.h"
#include "pq/ui/input.h"

namespace pq::ui {

struct KeymapEntry {
  std::string scope;
  std::string keys;  // as written
  Chord chord;
  std::string action;
  std::string status;  // "implemented" or "deferred:M<n>"
  std::string task;
  bool deferred() const noexcept { return status.rfind("deferred:", 0) == 0; }
};

struct KeymapFile {
  std::string name;
  std::vector<KeymapEntry> entries;
};

Result<KeymapFile> parseKeymapFile(std::string_view json);
Result<KeymapFile> loadKeymapFile(const std::string& path);
std::string defaultKeymapPath();  // assetsDir()/keymaps/ableton.json

// Every "implemented" entry's action must be registered in `actions` (else an error naming it); those are bound
// into `keymap`. Deferred entries are skipped (their chords do nothing until their milestone lands).
Status applyKeymapFile(const KeymapFile& file, const ActionRegistry& actions, Keymap& keymap);

}  // namespace pq::ui
