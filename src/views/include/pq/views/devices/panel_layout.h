#pragma once

// The layout of a LEAF block's Device-view frame (GU.11, docs/plans/GU.md "One hierarchy"): DevicePanel's body. A group
// is drawn as its own panel (pq/document/panel.h elements, pq/views/panel/group_panel_face.h); a leaf — a native engine,
// an atom or value block on a track's main path — has no panel of its own, so its frame's body is generated here: a
// native engine's face (facePanel), else one control per param and setting (autoPanel). There are no panel description
// files (the T4.6 `.panel.json` reader and the stock devices' bespoke displays are in retired/).

#include <cstdint>
#include <string>
#include <vector>

#include "pq/views/devices/device_model.h"

namespace pq::views::devices {

enum class ControlKind : uint8_t { Knob, Box, Toggle, Face };

struct ControlLayout {
  ControlKind kind = ControlKind::Knob;
  std::string id;              // the widget's name
  std::string param;           // bound to a param, or
  std::string constName;       // to a structural setting
  std::string label, text;     // a knob's / box's label ("" = the param's), a toggle's text
  std::vector<double> values;  // a toggle's [off, on]
  std::string meta;            // Face: the meta instance's name path ("" = the native engine itself)
  float x = 0, y = 0, w = 0, h = 0;  // logical px in the body
};

struct PanelLayout {
  std::string patch, title;
  float width = 0, height = 0;
  std::vector<ControlLayout> controls;
  // TMM.10: a native engine's frame (facePanel): its face fills the body and scales to the frame's height (the Device
  // view's), keeping the face's aspect: the body is faceW x faceH at scale 1 (DevicePanel::logicalWidthFor).
  bool fitFace = false;
  float faceW = 0, faceH = 0;
};

// The generic body: one control per visible param (knobs; bools as toggles; stepped params as number boxes) and per
// structural setting (number boxes), flowed into columns.
PanelLayout autoPanel(const DeviceSignature& sig, std::string title = {});
// TMM.10: a native engine's body: one face control (meta path "") of world size faceW x faceH, fitted to the frame's
// height (fitFace).
PanelLayout facePanel(const DeviceSignature& sig, std::string title, float faceW, float faceH);

}  // namespace pq::views::devices
