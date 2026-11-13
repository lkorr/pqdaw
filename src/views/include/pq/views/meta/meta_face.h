#pragma once

// Meta-module faces (TMM.4, docs/plans/MM.md "UI", SPEC §15 2026-11-01 "meta modules"): every meta::Type with a face
// has a MetaFace class here, registered by type id. A face is vector code in its own world units (pq/ui/face_painter.h)
// so it draws — and stays interactive — at every zoom of the circuit canvas; the canvas hosts it on the node of a
// meta call (CircuitScene::faces: the block IS the face) and routes pointer events to it in face-local world units.
//
//   static layers   ≤ 3 value-independent layers, one colour each (body, tracks, frame): rasterised once per zoom
//                   into coverage tiles shared by every face of the type, drawn as one quad each
//   live layer      values, hover, the voice dots: geometry each frame (cheap: a few arcs and sprites)
//   jacks           the host's pins on the face edges, drawn by the face (paintJacks: one look for every face)
//   interaction     hitTest → a control (a part ≥ 0), the frame (kFrame: select / move; the block menu's Open Group
//                   opens the expansion) or nothing; drags are relative and measured in device px with a floor, so a control
//                   travels the same distance per pointer pixel at zoom 0.05 as at 3 (Shift: fine); a double-click on a
//                   control resets it; the wheel goes to a face only where it uses it (phase_acc: never)
//   binding         a MetaTarget (the document, or a read-only default state) and an optional VoiceTapReader
//
// VoiceTapReader is the narrow views-side interface the faces consume (TMM.4 builds against it before TMM.2's
// pq/core/voice_tap.h exists): per voice slot the newest sampled phase plus the block's sampled phases since the last
// read; `live()` says whether anything may still be written (the host polls at ≤ 60 Hz only while a visible face's
// reader is live: idle costs nothing). TMM.2's reader adapts to it.
//
// UI thread only.

#include <array>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/model.h"
#include "pq/meta/type.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/input.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"

namespace pq::views::devices {
class SpectrumFeed;  // pq/views/devices/analysis.h (TX.spectrum)
}

namespace pq::dsp {
class FrameTable;
}

namespace pq::views {

// TX.spectrogram: one pitch of the tuning the instance's track plays in (a scale degree), what an analysis face draws as
// a guide line.
struct PitchGuide {
  float hz = 0.0f;
  int32_t degree = 0;   // 0 … size − 1
  int32_t equave = 0;   // degree 0 of equave 0 is the tuning's reference pitch
};

// What a face reads and edits: one instance's params (in meta::Type::params order).
class MetaTarget {
public:
  virtual ~MetaTarget() = default;
  virtual double value(size_t param) const = 0;
  virtual bool editable() const noexcept { return true; }
  // A drag: begin, any number of set, then end (one undo step) or cancel (nothing changes).
  virtual void begin(const char* label) = 0;
  virtual void set(size_t param, double value) = 0;
  virtual void end() = 0;
  virtual void cancel() = 0;
  // Back to the type's default (one undo step).
  virtual void reset(size_t param) = 0;
  // TMM.7: settings and data. A setting change is one undo step (structural: it may recompile its unit). data() is the
  // instance's state data (never null for a type with data: the type's default otherwise; the same pointer while the
  // data is unchanged, so a face can cache by it); setData inside begin / end joins that gesture (one undo step for a
  // drag), outside one is its own step. Data never recompiles (the document's MetaData class).
  virtual double setting(size_t i) const;
  virtual void setSetting(size_t /*i*/, double /*value*/) {}
  virtual meta::DataPtr data() const;
  virtual void setData(meta::DataPtr /*data*/) {}
  // A message for the status line (a refused gesture says why).
  virtual void notice(std::string_view /*message*/) {}
  // The type this target edits (null: unknown; the defaults above then read nothing).
  virtual const meta::Type* targetType() const noexcept { return nullptr; }
  // TX.spectrogram: the degrees of the tuning the instance's track plays in, between loHz and hiHz, ascending (none:
  // no track known, the default); guidesKey() changes whenever they may have (the face asks again).
  virtual void pitchGuides(float /*loHz*/, float /*hiHz*/, std::vector<PitchGuide>& out) const { out.clear(); }
  virtual uint64_t guidesKey() const { return 0; }
  // GU.1: param i's own curve (doc::Device::curves of a chain meta device; null: the declared one, meta::paramCurve).
  // The face's controls travel along it (faceTravelToValue / faceValueToTravel).
  virtual const dsp::Curve* curve(size_t /*param*/) const { return nullptr; }
  // T4.18 / TL.6: a knob POSITION (GU.1: what an automation lane holds) shown instead of the stored one: an automated
  // param follows its envelope at the playhead (pq/views/arrangement/automation_follow.h); value() then reads it
  // through the param's curve. nullopt: the stored one again. Returns whether it changed. Default: not supported.
  virtual bool showPosition(size_t /*param*/, std::optional<double> /*position*/) { return false; }
};

// GU.1: a face control's position t in [0, 1] ↔ param i's value on the target's curve (the declared one:
// meta::travelToValue / valueToTravel exactly), and whether the curve is switched off the declared one.
double faceTravelToValue(const MetaTarget& target, const meta::Type& type, size_t param, double t) noexcept;
double faceValueToTravel(const MetaTarget& target, const meta::Type& type, size_t param, double v) noexcept;
bool faceCurveSwitched(const MetaTarget& target, const meta::Type& type, size_t param) noexcept;

// The type's defaults, read-only (a standalone file in the viewer, an instance the host cannot address).
class DefaultTarget final : public MetaTarget {
public:
  explicit DefaultTarget(const meta::Type& t) : type_(&t) {}
  const meta::Type* targetType() const noexcept override { return type_; }
  double value(size_t param) const override { return param < type_->params.size() ? type_->params[param].def : 0.0; }
  bool editable() const noexcept override { return false; }
  void begin(const char*) override {}
  void set(size_t, double) override {}
  void end() override {}
  void cancel() override {}
  void reset(size_t) override {}

private:
  const meta::Type* type_;
};

// One sounding voice as a face shows it.
struct FaceVoice {
  static constexpr uint32_t kMaxSamples = 96;  // ≈ 12 blocks of 8 sampled phases: one displayed frame's reads
  uint32_t slot = 0;     // the voice slot (stable while the voice sounds)
  float phase = 0.0f;    // the newest sampled phase, 0..1
  float position = 0.0f; // TMM.7: the position the writer reported with it (a wavetable's frame position, 0..1)
  float increment = 0.0f; // TMM.16: the read's increment the writer reported (cycles per sample: its mip level)
  // TMM.17: the samples per second the increment is per (the engine rate of the tap; 0: unknown). A phase signal's
  // frequency is then |increment wrapped to ±0.5| · rate (the Phase accumulator's colour and orbit radius).
  float rate = 0.0f;
  uint32_t sampleCount = 0;
  std::array<float, kMaxSamples> samples{};  // the phases sampled since the last read, oldest first
};

// TMM.16: the table the audio side plays for an instance that reads one (the Wavetable), so its face draws what is
// heard: the table published to the instance's slot now (null: none known, e.g. no engine), its slot's publish count
// (changes with every table, preview or full) and the params the instance's live path applies on top of it (per
// meta::Type::params, from the last lowering).
struct HeardTable {
  std::shared_ptr<const dsp::FrameTable> table;
  uint64_t generation = 0;
  std::vector<uint8_t> modulated;
};

class VoiceTapReader {
public:
  virtual ~VoiceTapReader() = default;
  // TMM.16: the instance's published table (see HeardTable); none by default.
  virtual HeardTable heardTable() const { return {}; }
  // The voices sounding now (at most out.size()), newest record per voice slot; returns the count.
  virtual uint32_t read(std::span<FaceVoice> out) = 0;
  // Changes whenever new records arrived.
  virtual uint64_t serial() const noexcept = 0;
  // Whether records may still arrive (a voice sounds, the transport plays): the host keeps polling while true.
  virtual bool live() const noexcept = 0;
};

// A pin of the host on the face's edge (face-local world units).
struct FaceJack {
  ui::Vec2 at{0.0f, 0.0f};
  bool input = true;
  bool connected = false;
  bool visible = true;
  std::string_view name;  // the port (a connected param pin drives that control: drawn as driven)
};

// What a face paints with.
struct FaceEnv {
  const ui::Theme* theme = nullptr;
  float uiScale = 1.0f;
  bool selected = false;
  bool hovered = false;        // the node is the canvas's hovered item
  std::span<const FaceJack> jacks;
};

struct FacePointer {
  enum class Kind : uint8_t { Down, Move, Up, Wheel, Leave } kind = Kind::Move;
  ui::Vec2 local{0.0f, 0.0f};   // face-local world units
  ui::Vec2 device{0.0f, 0.0f};  // device px (drags measure in these)
  float pxPerUnit = 1.0f;       // device px per world unit
  float uiScale = 1.0f;
  ui::Mods mods = ui::mod::kNone;
  uint8_t clicks = 1;
  ui::Vec2 wheel{0.0f, 0.0f};
  // TMM.9: Down / Up: the button (a face gets right presses only where it uses them: onPointer returns false
  // otherwise and the host opens its menu). Move without a drag: the hover (a face may track it; true = repaint).
  ui::PointerButton button = ui::PointerButton::Left;
  // GU.13: the host lets a panel's value controls take the DEVICE-PANEL GESTURES — the wheel turns a knob (a run of notches
  // is one undo step: Face::endWheel), a press gives it the keys (type a value, Delete: default, Up / Down: a step). The
  // Device view's and the panel window's PanelHost set it; the circuit canvas does not (its wheel pans and zooms).
  bool valueGestures = false;
};

// TX.mod-all: where a modulation wire into a param ends on a face — the control that shows the param (the user: "the
// cables actually touch on / go directly to the location of the knob or slider they are modulating"): `at` is the
// control's centre (a knob's), or a slider's handle where it is now, in face-local world units; `radius` how far
// around it the control's body reaches (a knob's rim: the wire stops there and leaves the knob's value visible; 0: it
// ends on the point).
struct ControlAnchor {
  ui::Vec2 at{0.0f, 0.0f};
  float radius = 0.0f;
  // TMM.13-fix2 (MM review H6): half the width of the control's column with its labels (a panel knob's name above and
  // value below): a wire runs down beside it, this far left of `at` plus a gap, and enters the control from its clear
  // side. 0: the control has no labels (the radius).
  float clear = 0.0f;
};

// TX.panel-faces: what a host canvas sees of anything drawn as a block's face in world units — a meta module's vector
// face (MetaFace below) or a stock device's panel (pq/views/devices/panel_face.h: the Device view's panel, its widgets
// laid out at the canvas's zoom). One hosting path (circuit::CircuitCanvas): culling, static-layer tiles, jacks, the
// live layer, pointer routing in face-local world units, keys and text for the face the last press went to, cursor,
// info text, wire drops onto a control (paramAt) and the face's own clock (update / nextWake).
class Face;

// TX.orbit-ports (pq/views/circuit/orbit.h): a face's outline as it draws it, face-local units — a circle (`round`:
// centre, radius) or its whole rect with corners of `radius`. The host's orbiting main ports sit just outside it.
struct FaceShape {
  bool round = false;
  ui::Vec2 centre{0.0f, 0.0f};
  float radius = 10.0f;
};

// TX.panel-faces: what a face with its own clock or popups needs from its host (the canvas).
class FaceHost {
public:
  virtual ~FaceHost() = default;
  // The widget the face is drawn in (its tree's overlay layer, clipboard and resources).
  virtual ui::Widget* hostWidget() noexcept = 0;
  // The face changed outside the host's own calls (a value from the document, a timer it set): the host calls
  // update() soon and repaints it. Must not re-enter the face.
  virtual void faceNeedsUpdate(Face& f) noexcept = 0;
};

class Face {
public:
  static constexpr int32_t kNone = -1;   // outside the face
  static constexpr int32_t kFrame = -2;  // the face but no control: the block (select, move)

  Face() = default;
  virtual ~Face() = default;
  Face(const Face&) = delete;
  Face& operator=(const Face&) = delete;

  // The meta type of a meta module's face (null: another kind of face, e.g. a device panel).
  virtual const meta::Type* metaType() const noexcept { return nullptr; }

  // ---- geometry ----
  virtual ui::Vec2 worldSize() const noexcept = 0;
  // TMM.13-fix2 (MM review "UI and aesthetic" 3): where the host's caption (a circuit's binding name, "acc1") sits,
  // face-local, its text left-aligned at x: above the top-left corner by default; a face with an empty corner (the Phase
  // accumulator's circle) puts it there, so the name reads as the face's, not the node's above it.
  virtual ui::Vec2 captionAt() const noexcept { return {2.0f, -6.0f}; }
  // TMM.13-fix2 (MM review "UI and aesthetic" 8): whether the face draws its own body plate over its whole rect. A
  // Device-view panel draws one behind a face that does not (the Phase accumulator's circle), as the others have.
  virtual bool ownsPlate() const noexcept { return true; }
  // TX.orbit-ports: the outline the host's orbiting ports go round (default: the rect, corners of 10).
  virtual FaceShape shape() const noexcept { return {}; }
  // Where input i of n sits on the left edge / output j of n on the right edge (face-local y), when the host lays
  // its pins out by the face (a nested call's symbol). Default: 24 + 16·i.
  virtual float inputY(size_t i, size_t n) const noexcept;
  virtual float outputY(size_t j, size_t n) const noexcept;

  // ---- painting ----
  virtual uint32_t staticLayerCount() const noexcept { return 0; }
  // TX.panel-faces: whether the static layers are drawn at `pxPerUnit` device px per world unit (a device panel draws
  // them only far out, where its layers stand in for its widgets; the live layer then draws the rest). Default: always.
  virtual bool staticAt(float /*pxPerUnit*/) const noexcept { return true; }
  virtual ui::Rgba staticLayerColour(const ui::Theme& /*theme*/, uint32_t /*layer*/) const noexcept { return 0; }
  // In white (the host tints the tile); value-independent.
  virtual void paintStatic(ui::FacePainter& /*p*/, uint32_t /*layer*/) const {}
  // TMM.7: a per-instance key of static layer `layer` (0: value-independent for the whole type, the tile shared by
  // every face of the type). A face whose static content depends on its instance's data (the Wavetable's stacked
  // frames) returns a hash of that content: the host keys the layer's tile by (type, layer, key), and draws the layer
  // directly while the face is dragging (a knob that re-bakes it changes the key every step).
  virtual uint64_t staticKey(uint32_t /*layer*/) const { return 0; }
  virtual void paintLive(ui::FacePainter& p, const FaceEnv& env) = 0;
  // The host's pins, as sockets on the face's edge (every meta face shares the look). The host calls it before
  // paintLive.
  virtual void paintJacks(ui::FacePainter& p, const FaceEnv& env) const;
  // TMM.13-fix2 (MM review L23): how far in from its edge a socket's centre sits (face units; 5 by default, a short lead
  // in); a face whose plot reaches its edges (the analysers) puts its sockets on the edge itself (0), not on the plot.
  virtual float jackInset() const noexcept { return 5.0f; }
  // TX.orbit-ports: a FLOATING PANEL the face has open (the VCA's range and curve): its face-local rect, which may reach
  // outside worldSize (nullopt: none open). The host draws it over everything (after the cables, the lanes, the lifted
  // pass: paintPopup), hit-tests it as part of the face (hitTest / onPointer get its points in face-local units), and
  // repaints when it opens or closes; nothing in the layout moves. It follows the face (its zoom, scale and drag). A
  // face closes it itself (its own control, Esc while it holds the keys, focusLost: a press anywhere else).
  virtual std::optional<ui::Rect> popupRect() const noexcept { return std::nullopt; }
  virtual void paintPopup(ui::FacePainter& /*p*/, const FaceEnv& /*env*/) {}
  // The host has no room around the face (a Device-view panel clips to its body): the floating panel opens over the
  // face itself, inside worldSize.
  virtual void setPopupOverFace(bool /*on*/) {}
  // The face-local rects that change while voices sound (the host damages only these, ≤ 60 Hz).
  virtual void liveRects(std::vector<ui::Rect>& out) const = 0;
  // Whether the live layer moves now (a voice sounds): the host keeps its ≤ 60 Hz repaint going.
  virtual bool animating() const noexcept { return false; }
  // The voice reader the face shows (null: none) and its poll (true: what the face shows changed).
  virtual VoiceTapReader* voices() const noexcept { return nullptr; }
  virtual bool pollVoices() { return false; }
  // DT.1: FRAME TICKS for what follows the transport without a voice reader (a tempo slider's markings follow the tempo
  // at the playhead). The host calls frameTick() on each visible face when it wakes (kTransport, kSignal) and at its
  // ≤ 60 Hz poll while ticking() — a compare per element, true: the live layer changed (the host damages liveRects).
  virtual bool frameTick() { return false; }
  virtual bool ticking() const noexcept { return false; }
  // TX.spectrum: where the face finds the analysis of its instance's signal (a Spectrum module's face: asked at every poll
  // and paint, since the tap appears once the unit is compiled; null: none) and how it asks its host to poll it again
  // (sound arrived while it rested). Faces that show no analysis ignore it (wantsAnalysis false).
  virtual void setAnalysis(std::function<devices::SpectrumFeed*()> /*feedOf*/, std::function<void()> /*wakeHost*/) {}
  virtual bool wantsAnalysis() const noexcept { return false; }

  // ---- interaction ----
  virtual int32_t hitTest(ui::Vec2 local, float pxPerUnit) const noexcept = 0;
  // true: consumed (a press on a control captures until Up). Down / Move / Up / Wheel / Leave.
  virtual bool onPointer(const FacePointer& e) = 0;
  virtual bool dragging() const noexcept = 0;
  // TMM.7: a key while this face holds the canvas's key focus (the last press went to it); true: consumed.
  virtual bool onKey(const ui::KeyEvent& /*e*/) { return false; }
  // TX.panel-faces: text while it holds the key focus and wantsText() (a panel's number box being typed into).
  virtual bool onText(const ui::TextEvent& /*e*/) { return false; }
  virtual bool wantsText() const noexcept { return false; }
  // The key focus went elsewhere (a press outside the face): a type-in commits.
  virtual void focusLost() {}
  virtual void cancelDrag() = 0;  // capture lost / Esc: the drag's edits are undone
  // GU.13: a run of wheel notches on a control ended (the host's timer after the last notch, or any press): one undo step.
  virtual void endWheel() {}
  virtual ui::Cursor cursor(int32_t part) const noexcept;
  // The info view's text for a part (title, body).
  virtual void describe(int32_t part, std::string& title, std::string& body) const;
  // TX.patching / TX.panel-faces: the param whose control is under face-local point `local` ("": none): a wire dropped
  // there drives that param.
  virtual std::string paramAt(ui::Vec2 /*local*/) const { return {}; }
  // TX.mod-all: where a wire into param `param` ends (ControlAnchor); nullopt: no visible control shows it now (the host
  // keeps a pin on the edge for it). Value- and state-dependent (a slider's handle, an envelope's breakpoint, the
  // panel's selected band): the host asks again whenever it draws the wire.
  virtual std::optional<ControlAnchor> controlAnchor(std::string_view /*param*/) const { return std::nullopt; }
  // TX.mod-all: where the param's control is placed when it shows, whatever the face's state now (a panel's control on
  // another tab or band, or one a setting hides: its place in the panel's description); the host lays the wire's
  // route out to it. nullopt: the face has no control for it at all (the host keeps a pin). Default: controlAnchor.
  virtual std::optional<ControlAnchor> controlHome(std::string_view param) const { return controlAnchor(param); }
  int32_t hoveredPart() const noexcept { return hovered_; }
  // Returns whether it changed (the host repaints the face).
  virtual bool setHoveredPart(int32_t part) noexcept;

  // ---- the face's own clock (TX.panel-faces) ----
  // The host it is drawn in (null: detached).
  virtual void attach(FaceHost* /*host*/) {}
  // Runs what is due at `now` (the host tree's clock); true when the face must repaint.
  virtual bool update(double /*now*/) { return false; }
  // When update() must run next (+inf: nothing pending; ≤ the host's now: at the next frame).
  virtual double nextWake() const noexcept;
  // The host's zoom settled at `pxPerUnit` device px per world unit: rebuild what depends on the scale (a panel's layout
  // and fonts); true when the face must repaint. needsPrepare: whether prepare would rebuild (the host asks for a frame).
  virtual bool needsPrepare(float /*pxPerUnit*/) const noexcept { return false; }
  virtual bool prepare(float /*pxPerUnit*/) { return false; }

protected:
  int32_t hovered_ = kNone;
};

class MetaFace : public Face {
public:
  explicit MetaFace(const meta::Type& t) : type_(&t) {}

  const meta::Type& type() const noexcept { return *type_; }
  const meta::Type* metaType() const noexcept override { return type_; }
  void bind(std::unique_ptr<MetaTarget> target, VoiceTapReader* voices);
  MetaTarget& target() const noexcept { return *target_; }
  bool hasTarget() const noexcept { return target_ != nullptr; }  // TX.knob-curves
  VoiceTapReader* voices() const noexcept override { return voices_; }

  bool animating() const noexcept override { return voiceCount_ > 0; }
  // Reads the voice reader into the face (the host calls it at most once per displayed frame, never from paint, so a
  // partial repaint of several rects draws one state); true when what the face shows changed (new records, or a face's
  // own animation still running: voicesPolled).
  bool pollVoices() override;
  static constexpr uint32_t kMaxVoices = 16;
  std::span<const FaceVoice> sounding() const noexcept { return {voiceBuf_.data(), voiceCount_}; }

  void describe(int32_t part, std::string& title, std::string& body) const override;
  // TX.knob-curves: the jacks, then a small glyph of the curve beside each control whose knob curve is switched off its
  // declared one (paramRect's top-right corner).
  void paintJacks(ui::FacePainter& p, const FaceEnv& env) const override;
  // TMM.10: the face-local rect of the control that edits param `param` (meta::Type::params index), where a host
  // marks it (the Device view's automation LEDs, cable drops, Show Automation); nullopt: no such control.
  virtual std::optional<ui::Rect> paramRect(size_t /*param*/) const noexcept { return std::nullopt; }
  // TX.panel-faces: the name of the param whose paramRect holds `local`.
  std::string paramAt(ui::Vec2 local) const override;
  // TX.mod-all: by default the centre of the param's paramRect, its radius half the rect's smaller side (faces with
  // knobs and handles say exactly where they are).
  std::optional<ControlAnchor> controlAnchor(std::string_view param) const override;

  // The clock faces animate by (seconds; steady). Tests replace it (null: the steady clock again).
  static void setClock(double (*clock)());
  static double clockNow();

protected:
  // Called by pollVoices after each poll (`changed`: what it read changed what is shown); returns whether the face must
  // repaint (a face with its own animation, such as voice fades, returns true while it runs).
  virtual bool voicesPolled(bool changed) { return changed; }
  bool justBound_ = true;  // set by bind(): the next poll is the first one (voices sounding already appear, unfaded)
  std::array<FaceVoice, kMaxVoices> voiceBuf_{};
  uint32_t voiceCount_ = 0;
  uint64_t voiceSerial_ = 0;

private:
  const meta::Type* type_;
  std::unique_ptr<MetaTarget> target_;
  VoiceTapReader* voices_ = nullptr;
};

// Paints a whole face without a host's tiles (static layers drawn directly in their colours, then jacks and the live
// layer): goldens and benchmarks outside a canvas.
void paintFace(MetaFace& f, ui::FacePainter& p, const FaceEnv& env);

// ---- registry ----
// The face of type `t` (null: the type has no face; it is drawn as a plain subcircuit block).
std::unique_ptr<MetaFace> makeFace(const meta::Type& t);
bool hasFace(const meta::Type& t) noexcept;
// worldSize of the type's face ({0, 0}: none), without making one.
ui::Vec2 faceSize(const meta::Type& t) noexcept;
// TMM.13-fix2: captionAt of the type's face ({2, −6} without one), without making one each time.
ui::Vec2 faceCaptionAt(const meta::Type& t) noexcept;
// TX.orbit-ports: shape() of the type's face (the default without one), without making one each time.
FaceShape faceShape(const meta::Type& t) noexcept;
// Registers a face factory for a type id (built-in faces register themselves; tests may add one).
using FaceFactory = std::unique_ptr<MetaFace> (*)(const meta::Type& t);
bool registerFace(std::string_view typeId, FaceFactory factory);

}  // namespace pq::views
