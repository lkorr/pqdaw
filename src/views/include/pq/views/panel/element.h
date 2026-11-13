#pragma once

// GU.3 (docs/plans/GU.md "Panels and the panel window"): the ELEMENTS a group's panel is built from (the document's
// pq/document/panel.h PanelElement) and the ELEMENT REGISTRY. An element is vector code in its own world units (its
// rect's size; pq/ui/face_painter.h), drawn by the panel's face (group_panel_face.h) at every zoom:
//
//   static layers  ≤ 2 value-independent layers in white (the host tints them): a knob's track, a slider's groove,
//                  frames and ticks. They depend only on the kind, the style fields and the size (staticKey), so every
//                  element with the same kind + style + size shares ONE coverage tile per layer and scale (the face's
//                  tile cache: rasterised once per zoom, drawn as one quad).
//   live layer     the value (arc, handle, readout), hover and drag highlights: geometry every frame.
//   interaction    pointer events in element-local world units (FacePointer: drags measured in device px, so a control
//                  travels the same per pointer pixel at every zoom; Shift: fine; double-click: reset), keys and text
//                  while it holds the face's key focus (a number being typed into), its cursor and info text.
//   binding        through a PanelTarget (below): what the bound control / setting is (label, curve, choices) and where it
//                  is (its knob position), and the edits — a control dragged in one store gesture (one undo step, param
//                  writes, no compile), a setting committed once at the end of its gesture (one recompile), a curve
//                  edited inside a gesture (SetParamCurve: no compile within its class).
//
// Registry: registerElement(kind, factory, schema) — the generic kinds register themselves (elements.cpp); GU.7's rich
// elements add one factory each. The schema lists a kind's style fields (name, type, default, choices) for GU.5's
// inspector and GU.8's style schemas; an element reads its fields from the document element with those defaults.
//
// UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/panel.h"
#include "pq/dsp/curve.h"
#include "pq/ui/controls.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/input.h"
#include "pq/ui/theme.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views {
class GroupResponses;  // pq/views/panel/group_responses.h (EQ.4)
}

namespace pq::views::panel {

// What a binding is, resolved against the document (and the device library) by the target.
struct BoundInfo {
  bool valid = false;     // resolves now (the block exists, the param / setting is the patch's)
  bool setting = false;   // a structural value: committed once per gesture
  bool editable = true;
  std::string label;      // the param's @label / the setting's label
  dsp::Curve curve;       // knob position → value (a setting: linear over its range with its step)
  std::vector<std::string> choices;  // a choice (value = index); empty: none
  bool isSwitch = false;  // a bool param / a 0 … 1 step-1 setting
  int32_t steps = 0;      // discrete positions (≥ 2), 0: continuous
  bool curveSwitched = false;  // GU.13: a control switched off its declared curve (Knob Curve): the knob shows its glyph
  // Changes whenever the target resolved the binding again (its block changed): what an element caches from it (its
  // label, a readout's text) is keyed on it.
  uint64_t generation = 0;
};

// What elements read and edit. A gesture is begin, any number of setPosition / setCurve, then end (one undo step) or
// cancel (nothing changes). A setting's positions inside a gesture are a preview (position() returns it); end() commits
// it as one command.
class PanelTarget {
public:
  virtual ~PanelTarget() = default;
  // (The reference stays valid for the target's life; its content follows the document.)
  virtual const BoundInfo& info(const doc::Binding& b) const = 0;
  // The knob position in [0, 1] now (a setting: its value's share of its range; a preview while its gesture runs).
  virtual double position(const doc::Binding& b) const = 0;
  // The value at `position` in the binding's units as text ("1.00 kHz", "Quietest", "On").
  virtual std::string format(const doc::Binding& b, double position) const = 0;
  // Typed text → a knob position (nearest the current one on a curve that turns back); nullopt: not a value.
  virtual std::optional<double> parse(const doc::Binding& b, std::string_view text) const = 0;
  virtual void begin(const doc::Binding& b, const char* label) = 0;
  virtual void setPosition(const doc::Binding& b, double position) = 0;
  virtual void setCurve(const doc::Binding& b, const dsp::Curve& c) = 0;
  virtual void end() = 0;
  virtual void cancel() = 0;
  // One step of its own: a click on a toggle / menu / tab, a typed value.
  virtual void setOnce(const doc::Binding& b, double position, const char* label) = 0;
  virtual void reset(const doc::Binding& b) = 0;  // back to the default (one step)
  // Changes whenever anything a binding resolves to may have (a new document): cached resolutions are redone.
  virtual uint64_t revision() const = 0;
  virtual void notice(std::string_view /*message*/) {}
  // The elements were rebuilt (their bindings are new objects): what a target caches per binding goes.
  virtual void bindingsChanged() {}
  // The host widget (menus open from it) — null: no menus (tests without a tree).
  virtual ui::Widget* menuHost() const noexcept { return nullptr; }
  // GU.5: whether a wire drives the bound control (a modulation cable reaches it): the element shows a MODULATION RING.
  // Cheap per call (the target indexes the project's wires once per wire list).
  virtual bool driven(const doc::Binding& /*b*/) const { return false; }
  // GU.5: a mod source dragged onto a control — the real cable from output `from` (a Signal binding) into control `to`
  // (a Control binding): one undo step. False (and a notice saying why) when it cannot be made.
  virtual bool connect(const doc::Binding& /*from*/, const doc::Binding& /*to*/) { return false; }
  // GU.5b: the voices of a signal (a Signal binding: what each sounding voice's tap read; the phase orbit's dots), null
  // when nothing taps it (GU.7 makes the lowering tap a placed display's signal; until then only hosts that have a
  // reader, e.g. tests, show voices).
  virtual VoiceTapReader* voices(const doc::Binding& /*b*/) { return nullptr; }
  // GU.6: the panel of the group a Panel binding names (an embedded child panel), null: none (or no document).
  virtual doc::PanelPtr childPanel(const doc::Binding& /*b*/) const { return nullptr; }
  // GU.7: the NATIVE ENGINE FACE a rich element shows a section of (engine_elements.cpp): for a Data binding the face of
  // the engine block it names (its params, settings and data edited through the document, its voices and analysis
  // from the app), for a Signal binding a face of meta type `typeId` (an analyser) reading the lowering's tap of that
  // signal, its display state its own (not saved). Shared by every element of the target bound alike, so a face's state
  // is one (a keyframe selected on the table display is the one the harmonic editor edits). `host` wakes the face's
  // host when an analysis feed gets sound. Null: none (no document, a test target, an unknown block or type).
  virtual std::shared_ptr<MetaFace> engineFace(const doc::Binding& /*b*/, std::string_view /*typeId*/, FaceHost* /*host*/) {
    return nullptr;
  }
  // GU.7: the analysis feed a meter reads: a Signal binding's tap (the lowering's), or a Data binding's analyser
  // engine's own tap. Null: none yet.
  virtual devices::SpectrumFeed* signalFeed(const doc::Binding& /*b*/) { return nullptr; }
  // ---- GU.13: what a host shows on the controls (the Device view: pq/views/device_chain/device_chain.h) ----
  // The LIVE MODULATION RING of a control (the range its wires sweep, what they swept since the last poll, where the
  // modulation puts it), drawn by a knob instead of the static driven() ring; null: none set. setModulation returns
  // whether it changed (the host repaints).
  virtual const ui::ModulationRing* modulation(const doc::Binding& /*b*/) const { return nullptr; }
  virtual bool setModulation(const doc::Binding& /*b*/, const ui::ModulationRing& /*ring*/) { return false; }
  // A position SHOWN instead of the stored one (an automated control follows its envelope at the playhead while the
  // transport plays); nullopt: the stored one again. Returns whether it changed.
  virtual bool showPosition(const doc::Binding& /*b*/, std::optional<double> /*position*/) { return false; }
  // DT.1: the project's tempo where the playhead is (while playing; else at the start marker), in BPM — what a tempo
  // slider's note markings are placed by. Cheap (one tempo-map lookup). 120 without a document.
  virtual double tempoBpm() const { return 120.0; }
  virtual bool transportPlaying() const { return false; }  // DT.1: whether the transport plays (cheap)
  // DT.4: the engine's sample rate (a tempo slider shows short times in samples); 48 kHz without one.
  virtual double sampleRate() const { return 48000.0; }
  // ---- EQ.3: a STRUCTURAL EDIT an element asks for (the EQ's band ops: doc::ops::addBand, setEqMode …) ----
  // `build` makes it from the document as it is now and the store's id source; the target dispatches it through its
  // host's history (one undo step). False — with a notice saying why — when the builder refuses (its message: "A chain
  // holds at most 32 bands"), the store refuses, or there is no document; an empty edit is true and records nothing.
  using EditBuilder = std::function<doc::ops::EditResult(const doc::Project&, doc::IdSource&)>;
  virtual bool run(const EditBuilder& /*build*/) { return false; }
  // ---- EQ.4: the response displays (response_elements.cpp) ----
  // Groups' analytic responses (prepared once per structural revision, their controls re-read per change), null: none
  // (no document: the displays draw their grid and "no curve").
  virtual GroupResponses* responses() { return nullptr; }
  // The SELECTED GROUP of the panel (a band picked on a response display: its strip column lights up, Delete removes
  // it) and the displays' VIEW STATE (zoom, toggles) by element id and key: kept by the target, so they survive the
  // panel's rebuilds (a band added rebuilds the elements); not saved, not undoable.
  doc::DeviceId selectedGroup() const noexcept { return selected_; }
  void selectGroup(doc::DeviceId g) noexcept { selected_ = g; }
  double viewValue(uint32_t element, std::string_view key, double def) const;
  void setViewValue(uint32_t element, std::string_view key, double v);

private:
  doc::DeviceId selected_;
  std::vector<std::pair<std::string, double>> view_;
};

// What an element paints with.
struct ElementEnv {
  const ui::Theme* theme = nullptr;
  PanelTarget* target = nullptr;
  bool hovered = false;
  bool focused = false;  // holds the face's key focus
  float uiScale = 1.0f;  // DT.3: device px per logical px (the painter's scale / uiScale: logical px per panel unit)
};

// One style field of a kind (registry schema).
struct StyleSpec {
  enum class Type : uint8_t { Number, Choice, Token, Text };
  std::string_view name;
  Type type = Type::Number;
  double number = 0.0;          // Number default
  std::string_view text;        // Choice / Token / Text default
  std::span<const std::string_view> choices;  // Choice
  double min = 0.0, max = 0.0;  // Number (min == max: unbounded)
};
struct ElementSchema {
  std::string_view kind, label;
  std::span<const StyleSpec> style;
  std::span<const std::string_view> slots;  // binding slots ("value"; an XY pad: "x", "y")
  float defaultW = 48.0f, defaultH = 48.0f;
};

// The theme colour a token names ("value", "signal", "track", "handle", "ink", "frame", "tick", "hover", "glow",
// "body", "accent" = value); unknown: `fallback`.
ui::Rgba themeToken(const ui::Theme& th, std::string_view token, ui::Rgba fallback) noexcept;

class Element {
public:
  explicit Element(doc::PanelElement desc);
  virtual ~Element() = default;
  Element(const Element&) = delete;
  Element& operator=(const Element&) = delete;

  const doc::PanelElement& desc() const noexcept { return desc_; }
  uint32_t id() const noexcept { return desc_.id; }
  ui::Rect rect() const noexcept { return {desc_.x, desc_.y, desc_.x + desc_.w, desc_.y + desc_.h}; }
  ui::Vec2 size() const noexcept { return {desc_.w, desc_.h}; }
  const doc::Binding* binding(size_t slot = 0) const noexcept { return desc_.binding(slot); }

  // ---- painting (element-local world units: (0, 0) … size()) ----
  virtual uint32_t staticLayerCount() const noexcept { return 0; }
  virtual ui::Rgba staticLayerColour(const ui::Theme& /*th*/, uint32_t /*layer*/) const noexcept { return 0; }
  virtual void paintStatic(ui::FacePainter& /*p*/, uint32_t /*layer*/) const {}
  // Kind + style + size: the static layers' shared tile key (computed once).
  uint64_t staticKey() const noexcept { return staticKey_; }
  virtual void paintLive(ui::FacePainter& p, const ElementEnv& env) = 0;
  // GU.13: a key of what the live layer shows from the target (its bindings' positions, resolutions, rings): a document
  // change repaints only the elements whose key changed (the old device panel's rule: a value change damages only its
  // control). kAnyChange: it cannot tell (an engine's face, an embedded panel, a display): repainted on every change.
  static constexpr uint64_t kAnyChange = ~0ull;
  virtual uint64_t liveKey(const PanelTarget& t) const;

  // ---- interaction ----
  // Where a press is the element's (default: its whole rect). DT.3: an element whose rect holds more than its control
  // (labels, readouts, markings) hits only the control, so a press on the rest moves the block (GroupPanelFace kFrame).
  virtual bool hit(ui::Vec2 local) const noexcept;
  // DT.3: > 0: the element WIDENS with its panel (GroupPanelFace::stretchX / stretchTo: a drag of the block's edge), at
  // least this wide; 0 (default): its width is fixed.
  virtual float stretchMinWidth() const noexcept { return 0.0f; }
  virtual bool interactive() const noexcept { return false; }
  virtual bool onPointer(const FacePointer& /*e*/, PanelTarget& /*t*/) { return false; }
  virtual bool dragging() const noexcept { return false; }
  virtual void cancelDrag(PanelTarget& /*t*/) {}
  virtual bool onKey(const ui::KeyEvent& /*e*/, PanelTarget& /*t*/) { return false; }
  virtual bool onText(const ui::TextEvent& /*e*/, PanelTarget& /*t*/) { return false; }
  virtual bool wantsText() const noexcept { return false; }
  virtual void focusLost(PanelTarget& /*t*/) {}
  virtual ui::Cursor cursor() const noexcept { return interactive() ? ui::Cursor::Pointer : ui::Cursor::Inherit; }
  virtual void describe(const PanelTarget& t, std::string& title, std::string& body) const;
  // Its value as a visibility source (a tabs / menu element: the index; a toggle: 0 / 1); nullopt: it is none.
  virtual std::optional<double> selection(const PanelTarget& /*t*/) const { return std::nullopt; }
  // An unbound selection's own state (tabs: kept by the face across rebuilds of the panel).
  virtual void setLocalSelection(double /*v*/) {}
  // Where a wire into the bound param ends (element-local), nullopt: nowhere particular.
  virtual std::optional<ControlAnchor> anchor() const { return std::nullopt; }
  // GU.5: a mod source's handle (element-local) while it is bound to an output: a drag from it onto a control of the
  // panel makes the cable (GroupPanelFace draws the cable on the pointer meanwhile). nullopt: not a cable source.
  virtual std::optional<ui::Vec2> cableHandle() const { return std::nullopt; }
  // GU.5b: the element's own outline (element-local) when it can be a panel's plate: the BOTTOM element of a panel
  // that has one (a circle shape, the dial) makes the panel that shape — no rectangular plate, the block's orbiting
  // ports go round it (GroupPanelFace::shape). nullopt: none.
  virtual std::optional<FaceShape> outline() const noexcept { return std::nullopt; }
  // GU.5b: a FLOATING POPUP the element has open (element-local; it may reach outside the panel: Face::popupRect),
  // drawn over everything by the host and hit-tested as part of the element (its onPointer gets those points too).
  // The element closes it itself (its own control, Esc, focusLost).
  virtual std::optional<ui::Rect> popupRect() const noexcept { return std::nullopt; }
  virtual void paintPopup(ui::FacePainter& /*p*/, const ElementEnv& /*env*/) {}
  virtual void setPopupOverFace(bool /*on*/) {}  // the host has no room around the panel (Face::setPopupOverFace)
  // GU.5b: voices (Face::pollVoices / animating): read the bound signal's reader once per displayed frame (true: what
  // the element shows changed); whether the element still moves.
  virtual bool pollVoices(PanelTarget& /*t*/) { return false; }
  virtual bool animating() const noexcept { return false; }
  // DT.1: Face::frameTick / ticking for one element (a tempo slider: the playhead's tempo compared; ticking while the
  // transport plays).
  virtual bool frameTick(PanelTarget& /*t*/) { return false; }
  virtual bool ticking() const noexcept { return false; }
  virtual VoiceTapReader* voiceReader(PanelTarget& /*t*/) const { return nullptr; }
  // GU.5b: the face-local rects (element-local) its live layer changes while voices sound (default: its rect).
  virtual void liveRects(std::vector<ui::Rect>& out) const { out.push_back({0.0f, 0.0f, desc_.w, desc_.h}); }
  // GU.6: elements that hold a panel of their own (an embedded child panel, kind doc::kEmbedKind):
  //   bind          called by the face once the element is made (and when the face gets a host): its target (lives as
  //                 long as the face) and host (its tree's tile atlas).
  //   needsPrepare  / prepare: static tiles of what it holds, at the face's scale (the face asks every element).
  //   controlAt     the Control binding under `local` (default: its own binding): a wire dropped there, P.
  //   anchorFor     where control `want` sits in it (element-local; default: its own anchor when it is bound to `want`).
  virtual void bind(PanelTarget& /*t*/, FaceHost* /*host*/) {}
  virtual bool needsPrepare(float /*pxPerUnit*/) const noexcept { return false; }
  virtual bool prepare(float /*pxPerUnit*/) { return false; }
  virtual const doc::Binding* controlAt(ui::Vec2 /*local*/) const { return binding(); }
  virtual std::optional<ControlAnchor> anchorFor(const doc::Binding& want, bool shownOnly) const;
  // GU.13: every CONTROL the element shows (a Control binding) with its rect (element-local): its own binding, an engine
  // section's face controls, an embedded panel's controls (shown ones only). The Device view's drop targets, automation
  // LEDs and modulation rings visit them.
  using ControlVisitor = std::function<void(const doc::Binding& b, const ui::Rect& r)>;
  virtual void visitControls(const ControlVisitor& fn) const;
  // GU.13: the run of wheel notches the element was turned by ended (one undo step).
  virtual void endWheel(PanelTarget& /*t*/) {}

protected:
  // Style fields with the schema's defaults.
  double num(std::string_view name, double def) const noexcept { return desc_.number(name, def); }
  std::string_view txt(std::string_view name, std::string_view def) const noexcept { return desc_.text(name, def); }
  ui::Rgba colour(const ui::Theme& th, std::string_view field, std::string_view def, ui::Rgba fallback) const noexcept {
    return themeToken(th, txt(field, def), fallback);
  }
  // The label shown: the "label" field ("-": none), else the binding's label (cached by the binding's generation).
  const std::string& labelText(const PanelTarget& t, size_t slot = 0) const;
  // The text of the binding's value at `position` (cached by position and generation: a repaint that changes nothing
  // formats nothing).
  const std::string& readoutText(const PanelTarget& t, double position, size_t slot = 0) const;
  doc::PanelElement desc_;

private:
  uint64_t staticKey_ = 0;
  mutable std::string label_, readout_;
  mutable uint64_t labelGen_ = ~0ull, readoutGen_ = ~0ull;
  mutable double readoutPos_ = -1.0;
};

using ElementFactory = std::unique_ptr<Element> (*)(const doc::PanelElement& desc);
// Registers kind `schema.kind` (false: taken). The generic kinds are registered by registerGenericElements(), which
// makeElement / elementSchema call once.
bool registerElement(const ElementSchema& schema, ElementFactory factory);
// An element of `desc.kind`; an unknown kind becomes a placeholder (a dashed frame with the kind's name), so a panel
// from a newer build still opens.
std::unique_ptr<Element> makeElement(const doc::PanelElement& desc);
const ElementSchema* elementSchema(std::string_view kind);
std::vector<std::string_view> elementKinds();  // registered kinds, registration order

// ---- a small library of drawing helpers shared by elements (element-local units) ----
// A rounded rect's outline (the painter fills rounded rects only).
void outlineRect(ui::FacePainter& p, const ui::Rect& r, float radius, float thickness, ui::Rgba col);

}  // namespace pq::views::panel
