#pragma once

// The project patch view (T4.13, SPEC §15 2026-10-24): a view mode of the arrangement (View > Arrangement / Project
// Patch, Ctrl+Alt+P) that shows the whole project as one editable circuit — the model is pq/views/project_patch/
// project_patch.h (the lowered ProjectGraph read back onto the document), drawn on T4.2's CircuitCanvas (levels of
// detail, symbol tiles, the cached far-level chunks), edited through the same document commands as the Ableton view.
//
//   bands      one per track in lane order, tinted in the track colour, its name in the title strip; a group is a
//              container around its members; the MIDI source node shows the lane's clips as a strip (song time left →
//              right); a mixer strip node ends every chain (volume bar, pan, mute and solo buttons; double-click opens
//              it to show each send's level beside its pin).
//   zoom       Ctrl+wheel / = / − / F (fit), the wheel pans, middle or Alt drag pans (the canvas's). Semantic zoom:
//              zoomed out, boxes and the bands' names; in, symbols, then titles and pin labels; double-click a band's
//              empty space (or its name) to zoom onto that band, Esc / F back to the whole project; double-click a
//              device to open it to its atoms in place (the stock patch, read-only here: editing a device's patch is
//              the circuit editor's, "Open in Circuit View" in its menu) and again on its frame to close it.
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
// Painting is the canvas's (tiles; the whole project at a far zoom is ≈ 2 quads per 256-px chunk) plus one pass over
// the visible bands. UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/circuit/circuit_scene.h"
#include "pq/views/project_patch/device_cost.h"
#include "pq/views/project_patch/project_patch.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;
class MeterSource;

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
  // A device opened to its atoms in place (read-only), or closed again. View state of this session.
  void setDeviceExpanded(doc::DeviceId device, bool expanded);
  bool deviceExpanded(doc::DeviceId device) const noexcept;
  void setMixerExpanded(doc::TrackId track, bool expanded);
  bool mixerExpanded(doc::TrackId track) const noexcept;
  bool animatingView() const noexcept { return viewAnim_.active; }

  // ---- edits (each one undo step through pq::doc::ops / commands; false and the reason in the status line when it
  // could not be done) ---------------------------------------------------------------------------------------------
  bool connect(doc::DeviceId from, const std::string& port, doc::DeviceId to, const std::string& param);
  // `track`'s output into `target`: a group or the master (its output), a return (its send to 0 dB).
  bool routeTo(doc::TrackId track, doc::TrackId target);
  bool sendOn(doc::TrackId track, doc::TrackId ret);
  bool addDevice(doc::TrackId track, const std::string& patch, uint32_t index = doc::kAppend);
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
  ui::Menu* openParamMenu(doc::DeviceId from, const std::string& port, uint32_t toNode, ui::Vec2 at);

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
  // "Open in Circuit View" on a device block (the circuit editor on the project's device, SPEC §15 2026-10-26; the app
  // or the circuit editor's installer sets it; null: the item is disabled).
  std::function<void(doc::DeviceId)> onOpenDevice;
  // Set by installProjectPatch: hides the other views of the track area (the mixer) when this one is shown.
  std::function<void()> hideOthers;
  // T4.8 hook: racks are devices whose block will open to their chains; until then a rack is a block like any device.

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
  doc::Snapshot shownSnapshot() const noexcept { return built_; }
  Ticks songEnd() const noexcept { return songEnd_; }
  void updateReadout(uint32_t edge);
  void animateViewTo(const circuit::CircuitCanvas::View& v);

private:
  void onDocument(const doc::StoreEvent& e);
  void rebuild();
  bool apply(const doc::ops::EditResult& e, const char* what);
  bool dispatch(const doc::Command& c, const char* what);
  void beginMorph(bool in);
  void finishMorph();
  void captureArrangement();
  circuit::CircuitCanvas::View viewForArrangement() const;
  void pollReadout();

  ViewContext& ctx_;
  ViewContext::Subscription docSub_;
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
  std::vector<std::pair<std::string, std::shared_ptr<const circuit::CircuitScene>>> innerCache_;  // by device patch
  bool valueGesture_ = false;
  Stats stats_;
  Readout readout_;
  uint32_t readoutTimer_ = 0;
  mutable doc::Snapshot tracksOf_;                  // the snapshot bandTracks_ was filled from
  mutable std::vector<const doc::Track*> bandTracks_;  // per band
  DeviceCostSource* costSource_ = nullptr;
  bool showCosts_ = false;
  std::vector<float> costs_;  // per model node (devices; < 0: none)
  uint32_t costTimer_ = 0;

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

}  // namespace project_patch

// Installs the view into ShellSlot::Patch and binds view.togglePatch (called from installDefaultContent, after the
// mixer: the mixer's toggle hides the patch view and the other way round).
project_patch::ProjectPatchView& installProjectPatch(Shell& shell);
project_patch::ProjectPatchView* findProjectPatch(const Shell& shell) noexcept;

}  // namespace pq::views
