#pragma once

// GU.3 (SPEC §15 2026-11-06 "built GUIs"; docs/plans/GU.md "Concepts", "Panels and the panel window"): a GROUP'S PANEL —
// its face, built from ELEMENTS. Stored on the group's Device (Device::panel; null or empty: the group is drawn as its
// mini circuit), so a project, the clipboard (doc::BlockSet) and a group preset carry it with the device.
//
//   Panel     its size (world units: logical px at zoom 1) and its elements in paint order (later ones on top).
//   Element   one thing on the panel: a KIND (the views' element registry key: "knob", "slider", "number", "toggle",
//             "button", "menu", "xy", "label", "shape", "tabs", "curve", "modsource"; GU.7 adds rich kinds; MX.4 "jack":
//             a port of the closed block, bound to an Input (a cable into it) or a Signal (a cable out of it) — the
//             project patch puts the block's pin for that port on the jack, always there, wired or not — the document
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
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
// SM.11 (Q?-SM.10-a): a Setting binding named kActiveSetting is the bound block's ACTIVATOR (Device::active, any block:
// a toggle on it switches the block on / off, one undo step, a recompile) — the Sampler's Filter / LFO switches.
inline constexpr std::string_view kActiveSetting = "@active";
Binding activeBinding(DeviceId device);
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

// Shown only while element `element`'s value is one of `values` (a tabs / menu element: the index; a toggle: 0 / 1;
// AP.8b: a number: its value in its units — "from n up" lists n … max).
// A hidden source hides the elements that follow it, unless `values` holds kWhenHidden (EQ.3: shown while the source is
// hidden — the EQ's chain-A columns while its chain tabs are, in Stereo mode).
inline constexpr double kWhenHidden = -1.0;
// AP.6: SEVERAL SOURCES. `more` lists further terms (another element and its values, each read as the main one); the
// element is shown while ANY term holds (`all` false: the arpeggiator's Base for the Harmonics unit or the Harmonic Walk
// style) or while EVERY term holds (`all` true). Saved as the condition's optional "more" / "all"; without `more`: one
// source, as before.
struct VisibilityTerm {
  uint32_t element = 0;
  std::vector<double> values;
  friend bool operator==(const VisibilityTerm&, const VisibilityTerm&) = default;
};
struct Visibility {
  uint32_t element = 0;
  std::vector<double> values;
  std::vector<VisibilityTerm> more;
  bool all = false;
  // Every term, the main one first.
  size_t terms() const noexcept { return 1 + more.size(); }
  uint32_t termElement(size_t i) const noexcept { return i == 0 ? element : more[i - 1].element; }
  const std::vector<double>& termValues(size_t i) const noexcept { return i == 0 ? values : more[i - 1].values; }
  friend bool operator==(const Visibility&, const Visibility&) = default;
};
// A condition after elements went away or were renumbered: each term's element through `map` (0: gone, its term
// dropped; the first term left becomes the main one). False (the caller drops the condition): no term is left.
bool remapVisibility(Visibility& v, const std::function<uint32_t(uint32_t)>& map);

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
  bool fit = false;  // SM.9: the shown height is the open sections' (foldLayout below); saved as the optional "fit"
  bool empty() const noexcept { return elements.empty(); }
  const PanelElement* find(uint32_t id) const noexcept;
  int32_t indexOf(uint32_t id) const noexcept;  // −1: none
  uint32_t nextId() const noexcept;             // 1 + the largest id
  friend bool operator==(const Panel&, const Panel&) = default;
};
using PanelPtr = std::shared_ptr<const Panel>;

// ---- SM.9: fold sections ------------------------------------------------------------------------------------------
// A `fold` element (kFoldKind: a chevron and a title) is a SECTION HEADER. Folds are ordered by their top; a fold's
// section is the band from its bottom edge to the next fold's top (the last: to the panel's bottom), and it owns the
// elements whose top lies in that band (elements above the first fold, or beside a header in its row, belong to none).
// Folded, its elements are hidden. Whether a fold is open is VIEW state (ViewState::panelWindows[group].folds: saved,
// not undoable; every host of the panel shows the same), else its style "open" (default 1).
// Panel::fit: the shown height is the sum of the open sections: each folded section's band is taken out, so what lies
// below it moves up by the band's height and an element spanning it (a background shape) shrinks by it; SM.11: what
// only hidden elements reach on the right is taken off the shown width (the panel's right margin kept). Without fit the
// panel keeps its size and a
// folded section's place stays empty.
// SM.11: NESTED and CONDITIONAL folds, and sections owned by name —
//   - a fold's number field "parent" (a fold's id): while that fold is folded or gone this one is GONE — its header and
//     its section are hidden and (fit) taken out with the header; so is a fold whose visibility condition fails (the
//     `shown` callback: the host evaluates `when`). Elements beside a header (in its row) show with the header.
//   - an element's number field "section" (a fold's id) puts it in that fold's section wherever it lies (hidden while
//     the fold is folded or gone): the Sampler's mode row beside its oval belongs to the Expand chevron.
//   - AR.4: a fold of look "band" draws nothing and takes no press: a layout band alone. With a condition, an element
//     that SPANS its section shrinks by it while the condition fails (the arpeggiator's step lanes: a lane switched
//     off is its name strip; the condition's source is the lane's own element, its selection the switch).
inline constexpr std::string_view kFoldKind = "fold";
inline constexpr std::string_view kFoldParentField = "parent";
inline constexpr std::string_view kFoldSectionField = "section";
bool foldOpenByDefault(const PanelElement& e) noexcept;
struct FoldLayout {
  bool folds = false;           // the panel has a fold element (else y / h / hidden are empty and height = p.height)
  float height = 0.0f;          // the shown height
  float width = 0.0f;           // SM.11: the shown width (fit: the shown elements' right edge; else p.width)
  std::vector<float> y, h;      // per element (paint order): its shown top and height
  std::vector<uint8_t> hidden;  // per element: in a folded section
};
// `open(e)`: whether fold element e is open now (its view state, else foldOpenByDefault).
// `shown(e)` (optional): whether fold element e's visibility condition holds now (SM.11; none: every fold shows).
FoldLayout foldLayout(const Panel& p, const std::function<bool(const PanelElement&)>& open,
                      const std::function<bool(const PanelElement&)>& shown = {});

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
//           the input spectrum), "transfer" (DY.3:
//           its group's In: the live level's dot)
// The tap's slot is LowerOptions::metaSlotOf(device, tapPath(kind, port), 0), listed in LoweredProject::metaResources
// (type "", VoiceTap / SignalTap). Several displays of one output share one tap.
// MX.2 TRACE TAPS: a "scope" element whose slots 0 and 1 (and 2) are all Signals shows several signals on ONE time base
// (the Math module's A, B and Out): it gets one signal_tap(transport.beat, s0[0], s1[0], s2[0]) — channel 0 of each
// signal (s1 again when there are two), one ring, so the frames of all traces are the same samples and the face triggers
// and times every trace from s0. Requested under slot 0's device as traceTapPath(e) ("#trace:<port0>|<device1>:<port1>
// …"); the outputs are those a single tap may take (the first two of their blocks). One trace tap per first signal: a
// second element starting at the same output with other traces gets none. Unwatched it costs what one Oscilloscope's
// tap does (one relaxed load per block).
enum class ElementTap : uint8_t { None, Voice, Signal };
ElementTap elementTap(std::string_view kind) noexcept;
// The name path a tap is requested under: "#voice:<port>" / "#signal:<port>" (no meta path starts with '#').
std::string tapPath(ElementTap kind, std::string_view port);
struct PanelTap {
  DeviceId device;
  std::string port;
  ElementTap kind = ElementTap::None;
  std::vector<std::pair<DeviceId, std::string>> traces;  // MX.2: a trace tap's further signals (slot 1, 2); else empty
  friend bool operator==(const PanelTap&, const PanelTap&) = default;
};
// MX.2: the further signals of a trace element (slots 1, 2 while slot 0 is a Signal too; empty: not a trace element),
// and the path its tap is requested under (slot 0's device): "" when it is not one.
std::vector<std::pair<DeviceId, std::string>> elementTraces(const PanelElement& e);
std::string traceTapPath(const PanelTap& t);
std::string traceTapPath(const PanelElement& e);
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
