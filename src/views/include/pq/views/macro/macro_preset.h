#pragma once

// Macro-mode presets (T8.1, SPEC §6.2): which key does what. Data in assets/macro/<id>.json:
//
//   {
//     "name": "Trainer",
//     "primeLimit": 13,                                 // fewest-keypress labels use the interval keys up to this limit
//     "intervals": [ {"key": "1", "ratio": "2/1"}, … ],  // a key multiplies the composition by its ratio
//     "keys": { "place": "`", "placeChain": "Tab", "back": "Backspace", "left": "a", "right": "f", "up": "s",
//               "down": "d", "undo": "-", "redo": "=", "layer": "g", "lock": "b" },
//     "vals": { "31": [31, 49, 72, 87, 107, 115, 127, 132, 140, 151, 154] }   // optional: per EDO, the steps of the
//   }                                                                          // primes 2 … 31 (default: the patent val)
//
// Key names: one printable character (letters case-insensitive), the names ui::parseChord knows (Tab, Backspace,
// Enter, Space, …), and the keypad by AutoHotkey's names (Numpad0 … Numpad9, NumpadDot, NumpadDiv, NumpadMult,
// NumpadSub, NumpadAdd, NumpadEnter, NumLock): keypad keys are matched by scancode, so they work whatever Num Lock
// says. No modifiers: macro mode takes its keys only without Ctrl / Alt / Super.
//
// Presets shipped: trainer.json (the JI trainer's keyMappings.js = the SPEC table; the default) and ahk-numpad.json
// (the AHK macro's midi_config.ini [Preset_Default] slots on the keypad).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/tuning/edo.h"
#include "pq/tuning/fewest_keypress.h"
#include "pq/tuning/rational.h"
#include "pq/ui/input.h"

namespace pq::views {

enum class MacroOp : uint8_t { Interval, Place, PlaceChain, Back, Left, Right, Up, Down, Undo, Redo, Layer, Lock };

struct MacroKey {
  ui::Key key = ui::key::kUnknown;                   // matched when scancode is Unknown
  ui::Scancode scancode = ui::Scancode::Unknown;     // keypad keys
  std::string name;                                  // as written in the preset ("1", "Tab", "Numpad8")
  std::string label;                                 // short, for labels ("1", "Tab", "N8")
  bool matches(const ui::KeyEvent& e) const noexcept {
    return scancode != ui::Scancode::Unknown ? e.scancode == scancode : (key != ui::key::kUnknown && e.key == key);
  }
};

struct MacroBinding {
  MacroKey key;
  MacroOp op = MacroOp::Interval;
  tuning::Rational ratio{1};  // Interval
};

struct MacroPreset {
  std::string id;    // the file's stem ("trainer")
  std::string name;  // shown in the preferences
  std::vector<MacroBinding> bindings;  // intervals first, in file order, then the other keys
  uint64_t primeLimit = 13;  // the trainer's default (prLabelPrimeLimit)
  struct EdoVal {
    uint32_t edo = 0;
    tuning::Val val;
  };
  std::vector<EdoVal> vals;

  const MacroBinding* find(const ui::KeyEvent& e) const noexcept;
  // The key bound to `op` (its label; empty: unbound). For Interval: the first key of `ratio`.
  std::string keyLabel(MacroOp op, const tuning::Rational& ratio = tuning::Rational{1}) const;
  // The interval keys in file order, as the fewest-keypress search takes them (labels as key names).
  std::vector<tuning::KeyRatio> intervalKeys() const;
  // The val the tempered layer maps intervals with in `edo`: the preset's override, else the patent val.
  tuning::Val valFor(const tuning::Edo& edo) const noexcept;
};

Result<MacroPreset> parseMacroPreset(std::string_view json, std::string id);
// assetsDir()/macro/<id>.json
std::string macroPresetPath(std::string_view id);
Result<MacroPreset> loadMacroPreset(std::string_view id);
// The ids of the presets in assetsDir()/macro, sorted ("ahk-numpad", "trainer").
std::vector<std::string> listMacroPresets();
// The SPEC §6.2 table, compiled in: what macro mode runs on when the chosen preset cannot be loaded (the status line
// says why). Equal to assets/macro/trainer.json (a test compares them).
MacroPreset builtinMacroPreset();
inline constexpr std::string_view kDefaultMacroPreset = "trainer";

}  // namespace pq::views
