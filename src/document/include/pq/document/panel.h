#pragma once

// GU.3 (SPEC §15 2026-11-06 "built GUIs"; docs/plans/GU.md "Concepts", "Panels and the panel window"): a GROUP'S PANEL —
// its face, built from ELEMENTS. Stored on the group's Device (Device::panel; null or empty: the group is drawn as its
// mini circuit), so a project, the clipboard (doc::BlockSet) and a group preset carry it with the device.
//
//   Panel     its size (world units: logical px at zoom 1) and its elements in paint order (later ones on top).
//   Element   one thing on the panel: a KIND (the views' element registry key: "knob", "slider", "number", "toggle",
//             "button", "menu", "xy", "label", "shape", "tabs", "curve", "modsource"; GU.7 adds rich kinds — the document
//             never interprets it), a RECT in the panel, STYLE fields (named values the kind reads: a knob's "pointer"
//             arc / pie / dot / line, "track" width, "colour" a theme token, "readout" below / above / inside / none,
//             "label"; unknown names are kept: a later kind or GU.8's style schemas may use them), BINDINGS (slot 0 the
//             main one; an XY pad's slot 1 its y) and an optional VISIBILITY CONDITION: shown only while another element
//             of the same panel (a tabs / menu / toggle) has one of the given values (GU.8's tabs).
//   Binding   what an element shows and edits, naming the block by DeviceId (never by name: ids survive renames and are
//             remapped on paste, ops::remapBlocks), plus `meta` — the name path of a native engine's inner instance
//             ("" = the block itself; pq/document/model.h "Meta modules") — and `name`:
//               Control  a param (a knob position; its curve gives the value: model.h ControlCurve)
//               Setting  a structural value (a `real const` input, an instrument group's polyphony: groupSettings())
//               Data     a block's data (a meta instance's DataValue: harmonic tables, envelope breakpoints; GU.7)
//               Signal   an output port (displays, mod-source handles): `name` the port
//               Panel    a whole child panel (`device` a group inside: embedded, GU.6)
//               Input    an input port a cable reaches (DY.2: the `cablesource` element's target): `name` the input,
//                        "" (kMainPort) the block's main input
//
// Rules (docs/plans/GU.md "Decided"):
//   - A panel binds only what is inside its group (any depth) or the group itself (its own settings): SetGroupPanel and
//     the file reader refuse anything else.
//   - After every structural command (apply's final pass: a block deleted, moved to another track, taken out of the
//     group), the elements whose bindings no longer hold are taken off the panel in the same command — one undo step
//     brings both back. A condition naming a removed element is dropped (the element then always shows).
//   - The panel is never lowered: a panel edit is Meta (no compile). What its elements edit is classified as always (a
//     control: Param; a setting: Structural, one recompile; a curve: SetParamCurve's classes).
//
// Element ids are the panel's own (1 … , unique in the panel; ops allocate them), so a pasted panel keeps them.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/ids.h"

namespace pq::doc {

struct Device;
using DevicePtr = std::shared_ptr<const Device>;

enum class BindingKind : uint8_t { None, Control, Setting, Data, Signal, Panel, Input };
inline constexpr uint32_t kBindingKindCount = 7;
// "none", "control", "setting", "data", "signal", "panel", "input"
std::string_view bindingKindName(BindingKind k) noexcept;

struct Binding {
  BindingKind kind = BindingKind::None;
  DeviceId device;
  std::string meta;  // a native engine's inner instance ("" = the block itself)
  std::string name;  // Control: the param; Setting: the setting; Signal: the output; Data / Panel: ""
  bool bound() const noexcept { return kind != BindingKind::None; }
  friend bool operator==(const Binding&, const Binding&) = default;
};
Binding controlBinding(DeviceId device, std::string param, std::string meta = {});
Binding settingBinding(DeviceId device, std::string setting, std::string meta = {});
Binding signalBinding(DeviceId device, std::string output);
Binding panelBinding(DeviceId group);  // GU.6: a child group's whole panel (embedded)
Binding dataBinding(DeviceId device, std::string meta = {});  // GU.7: a native engine's data (and its face's state)
Binding inputBinding(DeviceId device, std::string input = {});  // DY.2: an input a cable reaches ("": the main input)

// A style field: a number or a text (a theme token, an enum's name, a label).
struct StyleField {
  std::string name;
  std::string text;     // when isText
  double number = 0.0;  // else
  bool isText = false;
  friend bool operator==(const StyleField&, const StyleField&) = default;
};
StyleField styleNumber(std::string name, double v);
StyleField styleText(std::string name, std::string v);

// Shown only while element `element`'s value is one of `values` (a tabs / menu element: the index; a toggle: 0 / 1).
// A hidden source hides the elements that follow it, unless `values` holds kWhenHidden (EQ.3: shown while the source is
// hidden — the EQ's chain-A columns while its chain tabs are, in Stereo mode).
inline constexpr double kWhenHidden = -1.0;
struct Visibility {
  uint32_t element = 0;
  std::vector<double> values;
  friend bool operator==(const Visibility&, const Visibility&) = default;
};

struct PanelElement {
  uint32_t id = 0;  // ≥ 1, unique in its panel
  std::string kind;
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
  std::vector<Binding> bindings;    // slot 0: the main one (may be empty: an unbound label, shape, tabs)
  std::vector<StyleField> style;    // sorted by name, unique
  std::optional<Visibility> when;   // none: always shown
  const Binding* binding(size_t slot = 0) const noexcept {
    return slot < bindings.size() && bindings[slot].bound() ? &bindings[slot] : nullptr;
  }
  const StyleField* field(std::string_view name) const noexcept;
  double number(std::string_view name, double def) const noexcept;  // a number field, else `def`
  std::string_view text(std::string_view name, std::string_view def) const noexcept;  // a text field, else `def`
  // Sets (or replaces) a field, keeping them sorted.
  void setField(StyleField f);
  friend bool operator==(const PanelElement&, const PanelElement&) = default;
};

inline constexpr uint32_t kMaxPanelElements = 4096;
inline constexpr float kMaxPanelSize = 8192.0f;

struct Panel {
  float width = 0.0f, height = 0.0f;
  std::vector<PanelElement> elements;  // paint order
  bool empty() const noexcept { return elements.empty(); }
  const PanelElement* find(uint32_t id) const noexcept;
  int32_t indexOf(uint32_t id) const noexcept;  // −1: none
  uint32_t nextId() const noexcept;             // 1 + the largest id
  friend bool operator==(const Panel&, const Panel&) = default;
};
using PanelPtr = std::shared_ptr<const Panel>;

// The panel's own rules: size in (0, kMaxPanelSize] when it has elements, at most kMaxPanelElements, ids ≥ 1 and unique,
// a kind for each, finite rects with w, h > 0, style fields sorted / unique / named / finite, bindings well formed (a
// kind's fields: Control / Setting / Signal a device and a name, Data / Panel a device), a condition naming another
// element of the panel. Bindings' devices are not looked up (bindingsProblem does that).
Status checkPanel(const Panel& p);
// Whether `b` may be on group `group`'s panel in `list` (its track's own blocks): its device is the group itself (its
// settings; a Panel binding never) or a device inside it at any depth (a Panel binding: a group inside it).
bool bindingAllowed(const std::vector<DevicePtr>& list, DeviceId group, const Binding& b) noexcept;
// "" when every binding of `panel` is allowed on group `group` of `list`, else what is wrong (element and binding).
std::string bindingsProblem(const std::vector<DevicePtr>& list, DeviceId group, const Panel& panel);
// `panel` without the elements whose bindings fail `keep` (conditions naming a removed element dropped); the same
// pointer when nothing goes, null when nothing is left.
PanelPtr prunePanel(const PanelPtr& panel, const std::vector<DevicePtr>& list, DeviceId group);
// A copy with every binding's device mapped through `map` (old → new; an id not in it is kept as it is).
Panel remapPanel(const Panel& p, std::span<const std::pair<DeviceId, DeviceId>> map);
// ---- GU.6: embedded child panels ---------------------------------------------------------------------------------
// A child group left whole on its parent's panel is an element of kind kEmbedKind bound (slot 0) to the child by a Panel
// binding: the child's panel drawn live inside the element's rect (its own elements, the same controls). The child's
// panel is fitted into the rect with its aspect kept and centred: a child-panel point c is at (x + k·c.x, y + k·c.y)
// in the element (embedFit). Breaking it apart (ops::breakApartPanelElements) puts copies of the child's elements at
// those places, so nothing moves on screen.
inline constexpr std::string_view kEmbedKind = "panel";
struct EmbedFit {
  float k = 1.0f, x = 0.0f, y = 0.0f;
};
// The fit of a `childW` × `childH` panel into a `w` × `h` element (k = 1 when either size is not positive).
EmbedFit embedFit(float w, float h, float childW, float childH) noexcept;
// The group an embed element shows (its Panel binding), invalid: `e` is not one. (EQ.4: a Panel binding on another kind
// names a group without embedding it: the EQ's band columns, a `shape` that lights up while its band is selected.)
DeviceId embeddedGroup(const PanelElement& e) noexcept;

// Whether any element of `p` binds device `d`.
bool panelBinds(const Panel& p, DeviceId d) noexcept;

// ---- GU.7: displays tapped by the lowering (docs/plans/GU.md "Phases" 7) -------------------------------------------
// A DISPLAY element whose main binding (slot 0) is a Signal — a block's output port — gets a tap from the lowering
// while it is placed: the panel's taps are part of the track patch, so placing / removing one (or rebinding it) is a
// structural edit (SetGroupPanel: one recompile), and a display that is not placed costs nothing. The tap kind follows
// from the element's kind (the document's one fact about kinds):
//   Voice   "orbit"                                  voice_tap(x): per sounding voice its newest value and the values
//                                                     sampled since the last read (the phase orbit's dots)
//   Signal  "scope", "spectrum", "spectrogram",       signal_tap(transport.beat, note, x[0], x[1]): the analysis feed's
//           "meter", "response" (EQ.4: slot 1 too,    four channels (note: the lowest voice's Hz in a voice context, else 0)
//           the input spectrum)
// The tap's slot is LowerOptions::metaSlotOf(device, tapPath(kind, port), 0), listed in LoweredProject::metaResources
// (type "", VoiceTap / SignalTap). Several displays of one output share one tap.
enum class ElementTap : uint8_t { None, Voice, Signal };
ElementTap elementTap(std::string_view kind) noexcept;
// The name path a tap is requested under: "#voice:<port>" / "#signal:<port>" (no meta path starts with '#').
std::string tapPath(ElementTap kind, std::string_view port);
struct PanelTap {
  DeviceId device;
  std::string port;
  ElementTap kind = ElementTap::None;
  friend bool operator==(const PanelTap&, const PanelTap&) = default;
};
// The taps `p`'s displays need, sorted (device, kind, port), unique.
std::vector<PanelTap> panelTaps(const Panel& p);

// ---- Settings that are not a patch's `real const` inputs (GU.3: an instrument group's polyphony) ------------------
// What a panel element (and the group's inspector) needs to show and edit one: its range, step, default, and for a
// choice its labels (index = value). The views resolve a Setting binding on a group through these; on any other block
// through the patch's `real const` inputs.
struct SettingSpec {
  std::string_view name, label;
  double min = 0.0, max = 1.0, step = 0.0, def = 0.0;
  std::span<const std::string_view> choices;  // a choice (value = index); empty: a number (or a switch: 0 … 1, step 1)
  std::string_view unit;                       // "s", "" …
  bool isSwitch() const noexcept { return choices.empty() && min == 0.0 && max == 1.0 && step == 1.0; }
};
// The polyphony settings of an instrument group (model.h "Instrument groups"), in inspector order: voices, mono, legato,
// glide, steal.
std::span<const SettingSpec> groupSettings() noexcept;
// TX.group-oversample: every group's `oversample` (model.h "Oversampled groups"): a choice 1× / 2× / 4× / 8×, each with
// its converters' latency at DC in base-rate samples (groupOversampleLatency; the project's PDC compensates it).
const SettingSpec& groupOversampleSetting() noexcept;
// A group setting by name: the polyphony's and `oversample` (null: none).
const SettingSpec* groupSetting(std::string_view name) noexcept;

}  // namespace pq::doc
