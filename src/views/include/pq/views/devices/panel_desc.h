#pragma once

// Panel descriptions (T4.6): how a device's front panel lays out its controls. Stored next to the device as
// `<device>.panel.json` (stdlib/devices/saturator.pqp → saturator.panel.json; QUESTIONS Q?-T4.6-a for the choice of a
// separate JSON file over a section in the .pqp). A device without one gets autoPanel(): one control per param.
//
// Format ("pqdaw-panel 1"; every key below; unknown keys are errors):
//   {
//     "format": "pqdaw-panel 1",
//     "patch": "saturator",                 // the device patch this panel fronts
//     "title": "Saturator",                 // the title bar's text
//     "size": [w, h],                       // the body, logical px (the title bar and jack strip are added)
//     "selectors": [{"name": "band", "values": ["1", …, "8"], "default": 3}],   // optional: view state that
//                                           // controls bind through ("freq{band}" reads freq4 while band = "4")
//     "controls": [
//       {"type": "knob", "param": "drive", "rect": [x, y, w, h]},              // label + arc + value
//       {"type": "box", "param": "bassFreq", "label": "Freq", "rect": […]},     // Live's number box (+ a label)
//       {"type": "toggle", "param": "mono", "text": "Mono", "rect": […]},       // a bool param
//       {"type": "toggle", "const": "color", "text": "Color", "rect": […]},     // a structural setting: values [0, 1]
//       {"type": "chooser", "param": "chan", "items": ["Stereo", "Left", "Right", "Swap"], "rect": […]},
//       {"type": "chooser", "const": "hq", "items": ["Off", "2x", "4x", "8x"], "values": [1, 2, 4, 8], "rect": […]},
//       {"type": "label", "text": "Waveshaper", "rect": […]},
//       {"type": "tabs", "selector": "band", "rect": […]},                       // one button per selector value
//       {"type": "visual", "visual": "transfer" | "eq" | "spectrum", "rect": […], "bind": {"drive": "drive", …}},
//                                           // bind: display role → a param name, "#name" for a structural const;
//                                           // keys starting with "@" are options ("@bands": the band selector,
//                                           // "@range": the EQ display's ± dB range)
//       {"repeat": {"var": "i", "values": ["1", …], "step": [dx, dy]}, "control": {… "{i}" …}}
//     ]
//   }
//   Any control may carry "id" (its widget name; default: the binding or the type and index), "label" (overrides the
//   param's @label), "when": [{"param" | "const" | "selector": name, "in": [values]}] (shown only while every
//   condition holds; params and consts compare numbers, selectors strings) and "info" (info-view body text).
// Bindings may contain "{selector}" (and inside "repeat", "{var}"); parsePanelDesc checks that every binding resolves
// to a param / const of the device for every value of the selectors it uses.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/views/devices/device_model.h"

namespace pq::views::devices {

enum class ControlKind : uint8_t { Knob, Box, Toggle, Chooser, Label, Tabs, Visual };

struct Condition {
  enum class Source : uint8_t { Param, Const, Selector };
  Source source = Source::Param;
  std::string name;               // may be templated
  std::vector<double> values;     // Param / Const
  std::vector<std::string> keys;  // Selector
};

struct ControlDesc {
  ControlKind kind = ControlKind::Knob;
  std::string id;
  std::string param;      // binding to a param (templated), or
  std::string constName;  // binding to a structural const (templated)
  std::string label, text, info;
  std::vector<std::string> items;
  std::vector<double> values;  // chooser / toggle values of a const (default: 0 … n−1; toggle [0, 1])
  std::string selector;        // Tabs
  std::string visual;          // Visual
  std::map<std::string, std::string> bind;  // Visual: role → param / const name (templated)
  float x = 0, y = 0, w = 0, h = 0;          // logical px in the body
  std::vector<Condition> when;
};

struct SelectorDesc {
  std::string name;
  std::vector<std::string> values;
  int32_t def = 0;
};

struct PanelDesc {
  std::string patch, title;
  float width = 0, height = 0;
  std::vector<SelectorDesc> selectors;
  std::vector<ControlDesc> controls;
  bool generated = false;  // autoPanel()
  const SelectorDesc* findSelector(std::string_view name) const noexcept;
};

// Strict reader; errors name the control (`panel: controls[3]: …`). Bindings are checked against `sig`.
Result<PanelDesc> parsePanelDesc(std::string_view json, const DeviceSignature& sig);
Result<PanelDesc> loadPanelDesc(const std::string& path, const DeviceSignature& sig);
// The generic panel of a device without a description: one control per visible param (knobs; bools as toggles;
// stepped params as number boxes) and per structural const (number boxes), flowed into columns.
PanelDesc autoPanel(const DeviceSignature& sig, std::string title = {});
// `<dir>/<stem>.panel.json` for a device file `<dir>/<stem>.pqp`.
std::string panelPathFor(const std::string& devicePath);

// "{name}" placeholders replaced by `values[name]` (unknown names are left as they are).
std::string resolveTemplate(std::string_view s, const std::map<std::string, std::string, std::less<>>& values);

}  // namespace pq::views::devices
