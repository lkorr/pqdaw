#pragma once

// The project patch view (T4.13, SPEC §15 2026-10-24): a view mode of the arrangement (View > Arrangement / Project
// Patch, Ctrl+Alt+P) that shows the whole project as one editable circuit — the model is pq/views/project_patch/
// project_patch.h (the lowered ProjectGraph read back onto the document), drawn on T4.2's CircuitCanvas (levels of
// detail, symbol tiles, the cached far-level chunks), edited through the same document commands as the Ableton view.
//
//   bands      one per track in lane order, tinted in the track colour, its name in the title strip; a group is a
//              container around its members; a mixer strip node ends every chain (volume bar, pan, mute and solo
//              buttons; double-click opens it to show each send's level beside its pin).
//   lanes      (T5.x-one-circuit) a MIDI or audio track's source node is a LANE WINDOW: the lane itself, display only —
//              its clips (colours, names, MIDI notes, audio waveforms from the overview cache), bar and beat lines,
//              the playhead — over one time window all windows share (4 bars by default; see laneStart). Wheel over
//              a window scrolls time, Ctrl+wheel zooms it (zoomed out, the windows widen and the chains move right);
//              double-click a clip opens its editor (a MIDI clip's piano roll, an audio clip's Clip View).
//   one view   (T5.x-one-circuit, SPEC §15 2026-10-31: the whole project is one circuit) every "Open in Circuit View"
//              comes here (ViewContext::openProjectCircuit: openCircuit): the device's band opened, framed on the
//              device and focused; zooming out shows the other bands, groups with their chains, returns, the master
//              chain and Out. Double-click inside an opened device edits its circuit (the Device level: the circuit
//              editor, whose Track button comes back here); its title strip closes it again.
//   zoom       Ctrl+wheel / = / − / F (fit), the wheel pans, middle or Alt drag pans (the canvas's). Semantic zoom:
//              zoomed out, boxes and the bands' names; in, symbols, then titles and pin labels; double-click a band's
//              empty space (or its name) to OPEN the band (T5.x-circuit-chain): zoomed onto it with every device of
//              its chain opened to its atoms in place, each in its labelled frame (racks as nested frames with their
//              chains stacked: circuit::buildRackCircuit) — project → track → devices → atoms; Esc / F back to the
//              whole project (the band's devices close again). Double-click a closed device to open it alone; what is
//              drawn is what plays: a device's patch is edited in the circuit editor (a double-click inside its
//              opened frame, or "Edit Device Circuit" in its menu).
//   panels     (TX.panel-faces) a closed stock device is drawn as the panel the Device view shows (the same widgets at
//              the canvas's zoom: devices::PanelFace), edited in place — a knob drag, a toggle, a chooser, a typed value:
//              one undo step each; its title bar and body move / select / open the block; a wire dropped on a control
//              drives that param. Opened, the device shows its atoms. Far out the panels are colour-layer miniatures.
//   edits      (each one undo step, shown at once in the arrangement, mixer and Device view: one document)
//                wire        drag a device's export pin (right side) onto a device → its params (a menu), or onto a
//                            param pin: T4.7's modulation wire (ops::connectWire)
//                re-route    drag a mixer's `out` pin onto a group's or the master's Σ (SetTrackOutput), onto a return's
//                            Σ (its send to 0 dB); drag a send pin onto its return's Σ (send to 0 dB)
//                devices     right-click a band → Add Device ▸ (the device menu), Delete removes the selected blocks,
//                            dragging a block along its band or onto another band moves it (ops::moveDevice; the chain
//                            rule holds), `0` toggles their activators
//                mixer       click M / S on a mixer strip (mute = the activator, solo exclusive, Ctrl additive); drag its
//                            volume bar sideways, its pan dot; an expanded strip's send bars likewise (store gestures)
//                delete      Delete on a selected wire: a modulation wire is removed, a send goes to −∞, a route becomes
//                            Sends Only
//              Ctrl+drag a block or a band's title nudges it (view state, saved with the project, not undoable; the
//              band menu's Reset Layout clears them).
//   morph      toggling animates between the views (kMorphSeconds): each lane keeps its vertical position and unfolds
//              into its band, its clips collapse into the source node's strip, and back.
//   readouts   resting on a wire shows what is known of its signal: a route / send the source track's meter level
//              (MeterSource), a modulation wire its target's live sweep (ModulationSource); chain wires their kind.
//
// CPU: idle = no frames, no timers. The model, lowering and scene are rebuilt only while shown, after a document change
// that can change them (structural, names, colours, a send turned on / off); a mixer drag or a clip edit only repaints.
// A lane-window time zoom that changes their width re-lays out (model + scene, the lowering kept); a scroll, a waveform
// arriving, a clip edit repaint the windows' rects only. While playing, a timer wakes when the playhead has moved about
// one device px in the windows and damages the old and new playhead columns of the visible windows only. The windows'
// MIDI notes come from the arrangement's clip previews (one cache for both views), audio from the waveform overviews.
// Painting is the canvas's (tiles; the whole project at a far zoom is ≈ 2 quads per 256-px chunk) plus one pass over
// the visible bands. UI thread only.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/circuit/chain_circuit.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/circuit/circuit_scene.h"
#include "pq/views/project_patch/device_cost.h"
#include "pq/views/project_patch/project_patch.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;
class MeterSource;
class ArrangementView;
struct PanelTemplate;
namespace devices {
class PanelFace;
}

namespace project_patch {

// The drawable scene of a project patch: model nodes become scene nodes in model order (an expanded device instead
// contributes its inner patch's nodes after them), model edges become scene wires in edge order (then the wires inside
// expanded devices and the connectors from their frames to their terminals).
struct InnerScene {
  doc::DeviceId device;
  std::shared_ptr<const circuit::CircuitScene> scene;  // the device patch's own scene (circuit::buildScene)
};
struct ProjectScene {
  std::shared_ptr<circuit::CircuitScene> scene;
  std::vector<uint32_t> sceneNode;   // per model node: its scene node (kNone: an expanded device)
  std::vector<uint32_t> modelNode;   // per scene node: the model node (inner nodes: their device's)
  std::vector<uint32_t> modelEdge;   // per scene wire: the model edge (kNone: inside an expanded device)
};
ProjectScene buildProjectScene(const ProjectPatch& patch, std::span<const InnerScene> inner = {});

class ProjectPatchView;

// The canvas of the view: the circuit canvas plus the bands under the scene and the mixer / clip / readout layer over it,
// and the edit gestures.
class ProjectCanvas : public circuit::CircuitCanvas {
public:
  explicit ProjectCanvas(ProjectPatchView& view);
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  void onCaptureLost() override;
  void onInteractionCancelled() override;
  ui::InfoText info() const override;
  // T4.16c: what a press would do — M / S Pointer, the volume / pan / send bars ResizeEW, an output pin (wire)
  // Crosshair, a device block or a band's title Move, panning Move; while dragging: a wire Crosshair (NotAllowed where
  // the drop would be refused), a block Move (NotAllowed off the bands), a bar ResizeEW. Worked out on each move (the
  // same tests as a press), so cursorAt itself costs nothing.
  ui::Cursor cursorAt(ui::Vec2 pos) const override;
  // The status message a wire dropped at `pos` would be refused with (nullptr: it would be made, or nothing happens).
  const char* wireRefusal(ui::Vec2 pos) const;

  // A pin of the scene nearest a device point (within 8 px; hidden pins included), and which side.
  struct PinHit {
    uint32_t node = kNone;  // model node
    uint32_t port = 0;
    bool input = false;
    bool valid() const noexcept { return node != kNone; }
  };
  PinHit pinAt(ui::Vec2 device) const;
  // The model node under a device point (an expanded device's frame counts), kNone: none.
  uint32_t nodeAt(ui::Vec2 device) const;
  // The band under a device point, kNone: none.
  uint32_t bandAt(ui::Vec2 device) const;
  // World rect → device rect.
  ui::Rect deviceRect(float x, float y, float w, float h) const;

  enum class Drag : uint8_t { None, Wire, MaybeMoveDevice, MoveDevice, Nudge, Volume, Pan, Send };
  Drag drag() const noexcept { return drag_; }

protected:
  void paintUnderlay(ui::PaintContext& ctx) override;
  void paintOverlay(ui::PaintContext& ctx) override;

private:
  friend class ProjectPatchView;
  // Mixer strip hit areas (device px) of model node n.
  struct MixerAreas {
    ui::Rect volume, pan, mute, solo;
    std::vector<ui::Rect> sends;  // expanded strips: one bar per send pin
  };
  MixerAreas mixerAreas(uint32_t node) const;
  ui::Cursor pressCursor(ui::Vec2 pos) const;  // T4.16c: the cursor for what a left press at `pos` would start
  void finishWire(ui::Vec2 pos);
  void finishDeviceMove(ui::Vec2 pos);

  ProjectPatchView& view_;
  Drag drag_ = Drag::None;
  ui::Vec2 press_{0, 0}, last_{0, 0};
  PinHit from_{};
  uint32_t dragNode_ = kNone;      // the device / mixer being dragged
  uint32_t dragBand_ = kNone;      // a band's title being nudged
  uint32_t dragSend_ = 0;          // the send row of a Send drag
  double dragStart_ = 0.0;         // the value at the press (volume fader position, pan, send position)
  ui::Vec2 pointer_{0, 0};         // the last pointer position (the readout is drawn there)
  bool gesture_ = false;
  ui::Cursor hoverCursor_ = ui::Cursor::Arrow;     // T4.16c: pressCursor at the last move
  ui::Cursor wireCursor_ = ui::Cursor::Crosshair;  // T4.16c: during a wire drag
  mutable std::string infoTitle_;                  // TMM.10-fix2: a hovered note source's info (info() returns views)
};

class ProjectPatchView : public ui::Widget {
public:
  explicit ProjectPatchView(ViewContext& ctx);
  ~ProjectPatchView() override;
  ViewContext& context() const noexcept { return ctx_; }

  // ---- shown, and the morph ------------------------------------------------------------------------------------
  // Shown: the view takes the track area (the arrangement is hidden), like the mixer view. With `animate` the lanes
  // morph into the bands (or back) over kMorphSeconds; without, it switches at once (tests, scenes).
  static constexpr double kMorphSeconds = 0.36;
  bool shown() const noexcept;
  void setShown(bool on, bool animate = true);
  void toggle();  // view.togglePatch
  bool morphing() const noexcept { return morph_.active; }
  float morphProgress() const noexcept { return morph_.t; }  // 0 = the arrangement, 1 = the patch
  // While morphing: where band `band` is drawn now (device px), between its arrangement lane (progress 0) and its
  // place on the canvas (progress 1). Empty when not morphing.
  ui::Rect morphBandRect(uint32_t band) const noexcept;

  // ---- the model ----------------------------------------------------------------------------------------------
  // The patch, its lowering and its scene as last built (sync() rebuilds them when the document changed).
  const ProjectPatch& patch() const noexcept { return patch_; }
  const doc::LoweredProject* lowered() const noexcept { return lowered_.get(); }
  const ProjectScene& scene() const noexcept { return scene_; }
  ProjectCanvas& canvas() const noexcept { return *canvas_; }
  void sync();

  // ---- semantic zoom ------------------------------------------------------------------------------------------
  void fitAll(bool animate = true);
  void focusBand(doc::TrackId track, bool animate = true);
  // T5.x-circuit-chain: the band opened (every device of its chain opened to its atoms, then zoomed onto it); a band
  // opened before closes its devices (those the user did not open one by one). closeBand: they close again.
  void openBand(doc::TrackId track, bool animate = true);
  void closeBand();
  doc::TrackId openedBand() const noexcept { return openBand_; }
  // The circuit drawn inside an opened rack's frame (nested boxes), null when the rack is not opened.
  const circuit::ChainCircuit* rackCircuit(doc::DeviceId rack) const noexcept;
  // A device opened to its atoms in place (read-only), or closed again. View state of this session.
  void setDeviceExpanded(doc::DeviceId device, bool expanded);
  bool deviceExpanded(doc::DeviceId device) const noexcept;
  void setMixerExpanded(doc::TrackId track, bool expanded);
  bool mixerExpanded(doc::TrackId track) const noexcept;
  bool animatingView() const noexcept { return viewAnim_.active; }

  // ---- the whole project as one circuit (T5.x-one-circuit) ------------------------------------------------------
  // "Open in Circuit View" (ViewContext::openProjectCircuit): the view shown (at once, without the morph, when it was
  // hidden), `track`'s band opened and framed on `device` (its frame; a device inside an opened rack: its box), or on
  // the band without one. The device is FOCUSED (its frame outlined) until the band closes or another opens.
  bool openCircuit(doc::TrackId track, doc::DeviceId device, bool animate = true);
  doc::DeviceId focusedDevice() const noexcept { return focus_; }
  // Where a device is drawn (world units): its block or opened frame, inside an opened rack its box. False: not drawn.
  bool deviceWorldRect(doc::DeviceId device, LaneRect& out) const;

  // ---- lane windows (T5.x-one-circuit) --------------------------------------------------------------------------
  // Every lane window shows [laneStart, laneStart + laneSpan). When the view is shown the window starts at the bar of
  // the playhead (playing) or of the arrangement's left edge, kLaneBars bars long by default (pq::kPpq and the time
  // signature there); while playing it pages on when the playhead runs out of it. The windows' width follows the span:
  // kSourceW up to the default span, wider in proportion up to kSourceMaxW (4 × the default span), denser beyond.
  static constexpr int64_t kLaneBars = 4, kLaneMaxBars = 64;
  static constexpr double kLaneZoomStep = 1.25;   // span factor per Ctrl+wheel notch
  static constexpr double kLaneScrollStep = 0.125;  // span fraction per wheel notch
  Ticks laneStart() const noexcept { return laneStart_; }
  Ticks laneSpan() const noexcept { return laneSpan_; }
  Ticks laneDefaultSpan() const noexcept { return laneUnit_; }
  float laneWidth() const noexcept;
  // Clamped: start ≥ 0, one beat ≤ span ≤ kLaneMaxBars bars. A width change re-lays out; otherwise the windows repaint.
  void setLaneTime(Ticks start, Ticks span);
  void scrollLanes(double notches);                  // + = later
  void zoomLanes(double notches, float anchor = 0.0f);  // + = zoom in (a shorter span); anchor: 0..1 across the window
  // What band `b`'s window shows: its clips meeting the time window, each with its world x span inside the window.
  struct LaneClip {
    doc::ClipId clip;
    float x0 = 0, x1 = 0;
    bool audio = false;
  };
  std::vector<LaneClip> laneClips(uint32_t band) const;
  uint32_t laneAt(ui::Vec2 device) const;            // the band whose window is under a device point (kNone: none)
  doc::ClipRef laneClipAt(ui::Vec2 device) const;    // the clip under a device point in a window (invalid: none)
  // A clip's editor: the detail view shown in Clip mode on it (a MIDI clip: the piano roll; audio: the Clip View), the
  // clip selected in the arrangement, then onOpenClip (installProjectPatch: the editor takes the keyboard).
  void openClip(const doc::ClipRef& clip);
  std::function<void(const doc::ClipRef&)> onOpenClip;
  // The playhead the windows draw (read while playing; empty when stopped).
  const std::optional<Ticks>& lanePlayhead() const noexcept { return playhead_; }

  // ---- edits (each one undo step through pq::doc::ops / commands; false and the reason in the status line when it
  // could not be done) ---------------------------------------------------------------------------------------------
  bool connect(doc::DeviceId from, const std::string& port, doc::DeviceId to, const std::string& param);
  // TMM.10-fix2: MIDI track `track`'s note source, its note output `port` (doc::kNoteOutputs), into `to`.`param`.
  bool connectNotes(doc::TrackId track, const std::string& port, doc::DeviceId to, const std::string& param);
  // `track`'s output into `target`: a group or the master (its output), a return (its send to 0 dB).
  bool routeTo(doc::TrackId track, doc::TrackId target);
  bool sendOn(doc::TrackId track, doc::TrackId ret);
  bool addDevice(doc::TrackId track, const std::string& patch, uint32_t index = doc::kAppend);
  // TX.patching: a device added to `track` out of its series (free: placed under the chain, it plays only through
  // wires; one undo step), and taking devices out of the series or putting them back (doc::ops::setDevicesFree).
  bool addFreeDevice(doc::TrackId track, const std::string& patch);
  bool setDevicesFree(std::span<const doc::DeviceId> devices, bool free);
  // A free device back into its track's series at list position `index` (one undo step).
  bool putIntoChain(doc::DeviceId device, uint32_t index);
  bool moveDevice(doc::DeviceId device, doc::TrackId toTrack, uint32_t index);
  bool toggleDevicesActive(std::span<const doc::DeviceId> devices);
  // The canvas selection: devices removed, modulation wires removed, sends to −∞, routes to Sends Only; one step.
  bool deleteSelection();
  bool toggleMute(doc::TrackId track);
  bool solo(doc::TrackId track, bool additive);
  // Value gestures (one store gesture each: begin … set* … end).
  void beginValueGesture(const char* label);
  void setVolume(doc::TrackId track, float db);
  void setPan(doc::TrackId track, float pan);
  void setSend(doc::TrackId track, doc::TrackId ret, float db);
  void endValueGesture();
  void cancelValueGesture();
  // Nudges (view state; saved, not undoable): world units added to a band (track id) or a block (device id).
  void nudge(uint32_t entity, float dx, float dy);
  void resetLayout();

  // The selected devices / tracks (from the canvas selection), for tests and the menus.
  std::vector<doc::DeviceId> selectedDevices() const;

  // Menus: the band's (add device, focus, reset layout), a block's (delete, (de)activate, open, expand), a wire's.
  ui::Menu* openBandMenu(uint32_t band, ui::Vec2 at);
  ui::Menu* openNodeMenu(uint32_t node, ui::Vec2 at);
  ui::Menu* openWireMenu(uint32_t edge, ui::Vec2 at);
  // The params of block `toNode` a wire from `from`.`port` (or, `notes` valid, from that track's note output `port`)
  // can drive; choosing one connects it.
  ui::Menu* openParamMenu(doc::DeviceId from, const std::string& port, uint32_t toNode, ui::Vec2 at, doc::TrackId notes = {});

  // ---- sources and hooks --------------------------------------------------------------------------------------
  void setMeterSource(MeterSource* source);
  MeterSource* meterSource() const noexcept { return meters_; }
  // Per-device CPU cost labels (T4.13; Q134 item 8): the app's ProjectLink. Shown on request (the band menu's Show CPU
  // Cost): metering on, polled at kCostPollSeconds while shown and live, each device labelled with its ns per sample
  // over its block; off: metering off, nothing polled.
  static constexpr double kCostPollSeconds = 0.25;
  void setCostSource(DeviceCostSource* source);
  DeviceCostSource* costSource() const noexcept { return costSource_; }
  void setShowCosts(bool on);
  bool showCosts() const noexcept { return showCosts_; }
  // The label of model node n (its ns per sample, smoothed), or < 0 for none.
  float deviceCost(uint32_t node) const noexcept { return node < costs_.size() ? costs_[node] : -1.0f; }
  void pollCosts();
  // A device block's "Edit Device Circuit": the project-device circuit editor (T4.4-fix1,
  // circuit::openDeviceInCircuitView; SPEC §15 2026-10-26): its edits are document commands, editing a stock device
  // forks it into a user device. False: no device library, or the device is gone.
  // Edit Device Circuit: the device's own editor (above). T5.x-one-circuit: Open in Circuit View is openCircuit here.
  bool editDeviceCircuit(doc::DeviceId device);
  // Set by installProjectPatch: hides the other views of the track area (the mixer) when this one is shown.
  std::function<void()> hideOthers;
  // T4.8 hook: racks are devices whose block will open to their chains; until then a rack is a block like any device.

  // TMM.4: the voice reader of a meta instance's face ((device, path); "" = a chain meta device), or null (no dots).
  // The app binds TMM.2's voice taps here; scenes and tests bind synthetic voices. Changing it: rebindFaces().
  std::function<VoiceTapReader*(doc::DeviceId device, std::string_view path)> faceVoices;
  void rebindFaces();
  // TMM.5: a reader may have become live (the host saw new voice records: a note while nothing animated). The view
  // also wakes its faces while the transport plays (each playhead refresh, when no face timer runs).
  void wakeFaces();

  // ---- TX.panel-faces: closed stock devices drawn as their Device-view panels ----------------------------------
  // On (default): a closed stock device (not edited, not a rack, not a chain meta device, not missing) is drawn as the
  // panel the Device view shows (devices::PanelFace: the same widgets at the canvas's zoom, interactive in place, one
  // undo step per gesture); off: plain blocks (benchmarks' A/B). Rebuilds the model.
  void setPanelsShown(bool on);
  bool panelsShown() const noexcept { return panelsShown_; }
  // The panel face of a device's block (null: the device is not drawn as a panel now).
  devices::PanelFace* panelOf(doc::DeviceId device) const noexcept;

  // The hovered wire's readout (what the overlay draws; tests).
  struct Readout {
    uint32_t edge = kNone;
    std::string title, value;
  };
  const Readout& readout() const noexcept { return readout_; }

  struct Stats {
    uint64_t rebuilds = 0;       // lower + model + scene
    uint64_t repaints = 0;       // document changes that only repainted
    double lastRebuildMs = 0.0;  // the last rebuild's wall time (lower + model + scene)
    uint64_t morphFrames = 0;
    double lastMorphPaintMs = 0.0;
    // T5.x-one-circuit: lane-window time zooms that re-laid out (model + scene, no lowering) and the last one's wall
    // time; playhead reads while playing; lane windows painted (with their clips).
    uint64_t relayouts = 0;
    double lastRelayoutMs = 0.0;
    uint64_t playheadReads = 0;
    uint64_t lanesPainted = 0;
  };
  const Stats& stats() const noexcept { return stats_; }

  // ---- widget ---------------------------------------------------------------------------------------------------
  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;
  void onFrame(double now) override;
  void onTimer(uint32_t id) override;

  // Internal (the canvas).
  // The track of band `b` in the current document (null: gone). O(1) after the first call per document version (a
  // per-snapshot table: painting never walks the track tree per band).
  const doc::Track* bandTrack(uint32_t b) const noexcept;
  // T5.x-one-circuit: the tuning band `b`'s track plays in (Inherit resolved; the same table as bandTrack), for its
  // lane window's note previews; and the arrangement next to this view (its clip previews and audio layouts), or null.
  const doc::TuningDef* bandTuning(uint32_t b) const noexcept;
  ArrangementView* arrangement() const noexcept;
  doc::Snapshot shownSnapshot() const noexcept { return built_; }
  Ticks songEnd() const noexcept { return songEnd_; }
  Stats& mutableStats() noexcept { return stats_; }  // the canvas counts what it paints
  void updateReadout(uint32_t edge);
  void animateViewTo(const circuit::CircuitCanvas::View& v);
  // TX.panel-faces: the face of a panel entry of the scene (CircuitCanvas::panelMaker).
  std::unique_ptr<Face> makePanel(const circuit::SceneFace& f);

private:
  void onDocument(const doc::StoreEvent& e);
  void rebuild();
  void relayout();  // T5.x-one-circuit: the lane windows' width changed: model + scene again, the lowering kept
  void initLaneTime();
  void refreshTransport();
  void readPlayhead();
  void schedulePlayhead();
  void damageLaneColumn(Ticks t);
  void damageLanes();
  void frameDevice(doc::DeviceId device, bool animate);
  void expandBand(doc::TrackId track);  // opens the band's devices (closing another opened band), without zooming
  bool apply(const doc::ops::EditResult& e, const char* what);
  bool dispatch(const doc::Command& c, const char* what);
  void beginMorph(bool in);
  void finishMorph();
  void captureArrangement();
  circuit::CircuitCanvas::View viewForArrangement() const;
  void pollReadout();
  // TX.panel-faces: what a device's panel is (cached per patch), its size (world units; {0, 0}: none), and the shown
  // panels brought up to date with the document (values, activation, name; the block's size after a rebuild).
  const PanelTemplate* panelTemplate(const doc::Device& d);
  graph::Point2 panelSize(const doc::Device& d);
  void syncPanels();
  // TX.mod-all: the centre (block-local world units) of the control showing `param` on device `d`'s face or panel
  // (LayoutOptions::controlAnchor): the shown face's (its values, its panel's layout and selection now), else a
  // prototype's of the type / patch (the defaults' layout); nullopt: no visible control.
  std::optional<graph::Point2> controlAnchorOf(const doc::Device& d, std::string_view param);

  ViewContext& ctx_;
  ViewContext::Subscription docSub_;
  ViewContext::Subscription viewSub_;
  ProjectCanvas* canvas_ = nullptr;
  MeterSource* meters_ = nullptr;
  doc::Snapshot built_;               // the snapshot the model was built from
  std::shared_ptr<doc::LoweredProject> lowered_;
  const doc::DeviceLibrary* loweredWith_ = nullptr;
  ProjectPatch patch_;
  ProjectScene scene_;
  bool stale_ = true;
  bool fitted_ = false;
  Ticks songEnd_ = 0;
  std::vector<doc::DeviceId> expanded_;
  std::vector<doc::TrackId> expandedMixers_;
  // Expanded devices' scenes by the patch they play (a user device's resolved root name: one per text); bounded.
  static constexpr size_t kInnerCacheMax = 32;
  std::vector<std::pair<std::string, std::shared_ptr<const circuit::CircuitScene>>> innerCache_;
  // T5.x-circuit-chain: opened racks' circuits (rebuilt with the model), their devices' scenes; the opened band and
  // the devices it opened.
  std::vector<std::pair<doc::DeviceId, std::shared_ptr<const circuit::ChainCircuit>>> rackCircuits_;
  circuit::ChainSceneCache chainCache_;
  doc::TrackId openBand_;
  std::vector<doc::DeviceId> bandExpanded_;
  doc::DeviceId focus_;           // T5.x-one-circuit: the device "Open in Circuit View" framed
  // T5.x-one-circuit: the lane windows' time window, its default span, the playhead the windows show.
  Ticks laneStart_ = 0, laneSpan_ = 0, laneUnit_ = 0;
  bool laneInit_ = false;
  std::optional<Ticks> playhead_;
  uint32_t playheadTimer_ = 0;
  bool valueGesture_ = false;
  Stats stats_;
  Readout readout_;
  uint32_t readoutTimer_ = 0;
  mutable doc::Snapshot tracksOf_;                  // the snapshot bandTracks_ was filled from
  mutable std::vector<const doc::Track*> bandTracks_;  // per band
  mutable std::vector<const doc::TuningDef*> bandTunings_;  // per band (T5.x-one-circuit)
  DeviceCostSource* costSource_ = nullptr;
  bool showCosts_ = false;
  std::vector<float> costs_;  // per model node (devices; < 0: none)
  uint32_t costTimer_ = 0;
  // TX.panel-faces
  bool panelsShown_ = true;
  std::map<std::string, std::unique_ptr<PanelTemplate>, std::less<>> panelTemplates_;  // by patch
  const doc::DeviceLibrary* panelTemplatesFrom_ = nullptr;
  std::unordered_map<uint32_t, doc::DevicePtr> panelSynced_;  // per device: what its panel shows (unchanged: skipped)
  // TX.mod-all: prototype faces (by meta type) and panels (by patch) for controlAnchorOf before a block is shown.
  std::unordered_map<const meta::Type*, std::unique_ptr<MetaFace>> anchorFaces_;
  std::map<std::string, std::unique_ptr<Face>, std::less<>> anchorPanels_;

  struct Morph {
    bool active = false;
    bool in = true;        // arrangement → patch
    double start = -1.0;   // frame time of the first frame (−1: not started)
    float resumeFrom = 0.0f;  // progress already made (a reversed morph continues from where it was)
    float t = 1.0f;
    struct BandGeom {
      ui::Rect lane, band;  // device px: the arrangement lane, the band on the canvas
    };
    std::vector<BandGeom> bands;  // per model band
    struct ClipGeom {
      ui::Rect lane;   // where it is in the arrangement
      uint32_t band = kNone;
      float u0 = 0, u1 = 0;  // its span in the source node's strip (0..1)
      uint32_t colour = 0;
    };
    std::vector<ClipGeom> clips;
    struct NodeGeom {
      ui::Rect end;  // device px on the canvas
      uint32_t band = kNone;
      uint32_t node = kNone;
    };
    std::vector<NodeGeom> nodes;
  } morph_;

  struct ViewAnim {
    bool active = false;
    double start = -1.0;
    circuit::CircuitCanvas::View from, to;
  } viewAnim_;
  static constexpr double kViewAnimSeconds = 0.22;
};

// Projects for scenes, tests and benchmarks, built through the edit operations (the store's history grows):
//   the user's sketch: a group of "Keys" (Operator → Saturator) and "Pad" (the default synth → EQ Eight) with its own
//   processing (EQ Eight, Utility), a "Mod" track whose LFO modulates the group's Utility width and the keys' Saturator
//   drive, a return "Space" fed by both tracks' sends, a Saturator on the master; clips on Keys and Pad.
//   a wide project: `tracks` MIDI tracks (an effect or two each, a clip each) in groups of eight, two returns, sends.
// Both need the stock device library for the lowering to draw the devices (the document does not).
Status buildSketchProject(doc::DocumentStore& store);
Status buildWideProject(doc::DocumentStore& store, uint32_t tracks);
// TMM.4: Phase accumulator faces in chains — "Lead" (a face → Saturator), "Bell" (220 Hz, phase 0.25, amp 2 → Auto
// Filter), "Sub" (4.5 Hz, phase 0.5, amp 0.5 → EQ Eight) and "Keys" (Operator → Saturator: plain blocks beside them).
Status buildMetaFacesProject(doc::DocumentStore& store);
// The instrument of the first `count` MIDI tracks replaced by a chain Phase accumulator (benchmarks: a project plus faces).
Status addPhaseAccFaces(doc::DocumentStore& store, uint32_t count);
// TMM.7: a three-keyframe table for scenes, tests and benchmarks: a saw at 0, a square at 0.5 (spectral morph between
// them), a bright vocal-like cycle (odd and formant partials) at 1 (crossfade gap), 64 dense frames.
meta::DataPtr demoWavetableData();
// TMM.7: Wavetable faces — "Wave" (a chain Wavetable with demoWavetableData, warp 0.3 bend, position 0.35 → Saturator)
// and "Pair" (the default synth forked: `acc1 = meta_phase_acc(voice.hz)` wired into `wt1 = meta_wavetable(acc1)`
// under an envelope; wt1 holds demoWavetableData at position 0.7). Returns the Pair instrument in `pair` when given.
Status buildMetaWavetableProject(doc::DocumentStore& store, doc::DeviceId* pair = nullptr);
// TMM.10: meta modules as chain devices — "Meta Lead" (Phase accumulator → Wavetable with demoWavetableData at
// position 0.35, warp 0.3 → Auto Filter: a meta voice run) and "Meta Bass" (Phase accumulator +1.5 Hz → a saw
// Wavetable, nothing after it). The ids, when given.
struct MetaChainIds {
  doc::TrackId lead;
  doc::DeviceId acc, wt;
  doc::TrackId bass;
  doc::DeviceId bassAcc, bassWt;
};
Status buildMetaChainProject(doc::DocumentStore& store, MetaChainIds* ids = nullptr);
// The instrument of the first `count` MIDI tracks replaced by a chain Wavetable holding `data` (benchmarks).
Status addWavetableFaces(doc::DocumentStore& store, uint32_t count, const meta::DataPtr& data);
// TMM.11: the Envelope — "Pluck" (Phase accumulator → Wavetable → Envelope: a meta voice run whose voices end by the
// envelope; attack 12 ms bent up, decay 0.9 s bent down, sustain 0.35, release 1.2 s bent toward a line) and "Shape"
// (the default synth forked: acc1 → wt1 → env1 = meta_envelope(wt1) as a VCA whose `done` ends the voice). The ids.
struct MetaEnvelopeIds {
  doc::TrackId pluck;
  doc::DeviceId acc, wt, env;
  doc::TrackId shape;
  doc::DeviceId shapeInst;
};
Status buildMetaEnvelopeProject(doc::DocumentStore& store, MetaEnvelopeIds* ids = nullptr);
// TMM.12: the Meta Operator — "FM Keys" plays it at algorithm 1 (D → C → B → A) with exact ratios A 1, B 2, C 3/2,
// D 7/4 (+1.5 Hz on D's accumulator), modulator envelopes shorter and quieter (amounts 0.35 / 0.25 / 0.15), B reading
// demoWavetableData at position 0.4 and D a saw; a four-note chord. The ids.
struct MetaOperatorIds {
  doc::TrackId keys;
  doc::DeviceId op;
};
Status buildMetaOperatorProject(doc::DocumentStore& store, MetaOperatorIds* ids = nullptr);
// TMM.14: the VCA — "Keys" (the default synth, then a chain VCA at ×0.60 on the default bipolar-log curve) and "Trem"
// (the synth forked: acc1 → wt1 → vca1 = meta_vca(wt1 · env) on a Bend curve 0 … 2, bend 0.5, knob at 0.75). The ids.
struct MetaVcaIds {
  doc::TrackId keys;
  doc::DeviceId vca;
  doc::TrackId trem;
  doc::DeviceId tremInst;
};
Status buildMetaVcaProject(doc::DocumentStore& store, MetaVcaIds* ids = nullptr);

}  // namespace project_patch

// Installs the view into ShellSlot::Patch and binds view.togglePatch (called from installDefaultContent, after the
// mixer: the mixer's toggle hides the patch view and the other way round).
project_patch::ProjectPatchView& installProjectPatch(Shell& shell);
project_patch::ProjectPatchView* findProjectPatch(const Shell& shell) noexcept;

}  // namespace pq::views
