#pragma once

// The project patch view (T4.13, SPEC §15 2026-10-24): a view mode of the arrangement (View > Arrangement / Project
// Patch, Ctrl+Alt+P) that shows the whole project as one editable circuit — the model is pq/views/project_patch/
// project_patch.h (the lowered ProjectGraph read back onto the document), drawn on T4.2's CircuitCanvas (levels of
// detail, symbol tiles, the cached far-level chunks), edited through the same document commands as the Ableton view.
//
//   bands      one per track in lane order, tinted in the track colour, its name in the title strip; a group is a
//              container around its members; a mixer strip node ends every chain (volume bar, pan, mute and solo
//              buttons; double-click opens it to show each send's level beside its pin).
//   lanes      (T5.x-one-circuit) a MIDI or audio track's source node is a LANE WINDOW: the lane itself — its clips
//              (colours, names, MIDI notes, audio waveforms from the overview cache), bar and beat lines, the playhead —
//              over one time window all windows share (4 bars by default; see laneStart). Wheel over a window scrolls
//              time, Ctrl+wheel zooms it (zoomed out, the windows widen and the chains move right); double-click a clip
//              opens its editor (a MIDI clip's piano roll, an audio clip's Clip View). TX.lane-edit: a window is an
//              editing surface of its track run by the arrangement's own editor and automation (see "lane-window
//              editing" below): select time, Ctrl+Shift+M / the menu make a MIDI clip, clips move / copy / trim / delete /
//              duplicate, audio handles, automation mode (A) draws breakpoints over the clips.
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
//                            param pin or a face's / panel's control: T4.7's modulation wire (ops::connectWire).
//                            TX.mod-all: a wire into a face's or panel's param ends on its control (no pin); TX.cables:
//                            as a cable hanging straight onto the knob or handle where it is now (project_patch.h
//                            Port::anchored, Face::controlAnchor)
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
//   node menu  (TX.node-menu, SPEC §15 2026-11-02 "cables and the node menu"; pq/views/circuit/node_menu.h) Tab opens the
//              node menu at the pointer; the chosen node SPAWNS THERE: on the band, opened group capsule or free rack
//              under the pointer (else the nearest band) as a free block (atoms and typed specs as a free device of that
//              one node; a stock instrument or a rack goes into the chain: the instrument first, a rack where the pointer
//              is along the chain). A block in the way and the new one push each other apart (a short physics animation,
//              nudges: view state), and the band grows to hold blocks at or past its edge. Each placement is one undo step.
//                cable → menu   a cable from an output released on empty space opens the menu, nodes with an input
//                               first; the node arrives wired from that output into its MAIN input (its first signal
//                               input; none: not wired). Hold W and drag out of a param pin or a face's / panel's control:
//                               a BACKWARDS cable (kBackWireKey) — dropped on an output pin it wires that output into the
//                               param, on empty space the menu offers sources first and wires the new node's MAIN output
//                               (its first) into the param.
//                over a cable   Z inserts a VCA at 1 (meta:vca, its default: x1) between the cable's ends; Tab opens the
//                               menu and inserts the chosen node there (a chain cable: as a device of the chain at that
//                               place; a wire: the wire becomes source → main in, main out → target; a node without an
//                               input or output connects only what it has). Notes, routes and sends are not cut.
//                Shift-drag     a block with a main input and output dragged with Shift held over a cable highlights it;
//                               releasing splices the block in (as an insert does; a chain block onto its own chain moves).
//                Ctrl+Delete    dissolves the selected block (it needs a main input and output): it goes, and what fed its
//                               main input is wired to what its main output fed (a chain device: the chain closes up).
//                Ctrl-release   a cable released with Ctrl held connects and keeps a new cable from the same source on the
//                               pointer (no button held): each click wires one more target (Ctrl+click keeps going, a plain
//                               click ends, Esc drops it).
//   cables     (TX.cables) every connection between blocks (notes, chain, modulation and free wires, a group's terminals)
//              is a hanging cable from port to port drawn over the blocks — a glossy tube with a shadow and plugs, coloured
//              by its signal — simulated: it sways when a block moves (a dragged block carries it) and settles; it glows
//              with its signal while the transport plays (a param's modulation sweep, an audio / note path's track
//              level), with light flowing along it. Routes, sends and a mixer's output stay orthogonal traces.
//   resize     (TX.resize) hold S over a device block (a meta face, a panel, a plain block; not an opened one): the
//              cursor shows ⤡ and a drag scales the block uniformly, its top-left kept, its bottom-right corner
//              following the pointer (0.05× … 3×, a detent at 1×; Shift: steps of 25 %); a face or panel and everything
//              on it (controls, text, sockets, the cable ends on its knobs) in proportion, interactive and laid out at
//              that size (a plain block: its box and pin rows; its labels keep the UI font, as at any zoom). Neighbours make
//              room (the layout re-runs per change: blocks after it move right, the band's spine moves down to its
//              main pin, the band grows). Esc cancels the drag. S + double-click or the block menu's Reset Size: its own
//              size. View state like a nudge (saved, not undoable). S does nothing else in this view; while a text
//              field (a face's or panel's type-in) has the focus, S types.
//   drag       (TX.block-drag, the user 2026-11-03) a block dragged (a selection of blocks with it when it is selected; an
//              opened capsule with its contents; a band by its title) is the block itself at the pointer every frame —
//              its panel / face, its cables plugged into its real ports, drawn over the rest (CircuitCanvas::setNodeDrag;
//              no re-layout while it moves); the drop is the edit (a nudge, one re-layout; onto another band: one undo
//              step). Blocks never overlap: after any move, drop, spawn, resize or layout change, overlapping blocks
//              push each other apart (the one just dropped gives way most) and glide until each has its space with a
//              small gap; bands and capsules grow to hold them; then no frames (see "Push-apart" below). A block shrunk
//              below CircuitCanvas::kFaceMiniatureScale is a miniature (no controls: a press grabs it).
//   groups     (TX.groups; doc model.h "Groups") select devices (or meta modules, free devices) of one band and press
//              Ctrl+G: they become a group — one capsule in the chain where they were, its contents placed freely inside
//              between its In and Out terminals, the series kept as wires (so it sounds the same); Ctrl+Shift+G (or the
//              block menu's Ungroup) is the exact inverse. Inside a capsule everything is edited as anywhere else (wires,
//              faces, panels, S-resize, Delete, nudges by dragging a block); a block dragged out of the capsule leaves
//              the group, a free block dropped into one joins it. Double-click the capsule's title strip to close it (a
//              block with a miniature of its contents), double-click the closed block to open it again (view state of
//              this session). Delete on a selected group deletes it with its contents. The Device view shows the group
//              as one device whose panel is this canvas, scoped to the group (ProjectPatchView(ctx, group)).
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
#include "pq/views/circuit/node_menu.h"
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
namespace arrangement {
class EditSurface;
}

namespace project_patch {

class LaneEditSurface;  // TX.lane-edit (src/project_patch/internal.h)

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

  // Lane (TX.lane-edit): a press in a lane window's body went to the arrangement's editor / automation, whose drag runs.
  // TX.node-menu: BackWire (a backwards cable out of a param), MaybeSplice / Splice (a Shift-drag of a block).
  enum class Drag : uint8_t { None, Wire, MaybeMoveDevice, MoveDevice, Nudge, Volume, Pan, Send, Resize, Lane, BackWire, MaybeSplice, Splice };
  Drag drag() const noexcept { return drag_; }
  // TX.lane-edit: the last press was in a lane window (the arrangement's edit chords act there: laneKey).
  bool laneFocused() const noexcept { return laneFocus_; }
  // TX.resize: S is held (and no text field has the focus): a left press on a block resizes it; the block a press at
  // `pos` would resize (a closed device block; kNone: none).
  bool resizeHeld() const noexcept;
  uint32_t resizableAt(ui::Vec2 pos) const;
  static constexpr ui::Key kResizeKey = 's';
  // TX.node-menu: W held (no text field focused): a left press on a param pin or a face's / panel's control starts a
  // backwards cable out of that param.
  static constexpr ui::Key kBackWireKey = 'w';
  bool backWireHeld() const noexcept;
  // TX.node-menu: a cable is held on the pointer after a Ctrl-release (no button held; a click wires the next target).
  bool stickyWire() const noexcept { return sticky_; }
  // The model edge of the cable under a device point (the canvas's wire hit test: TX.cables' curves), kNone: none.
  uint32_t edgeAt(ui::Vec2 device) const;
  // TX.node-menu-fix1: a node can go into model edge `edge` (Z, Tab, a splice): a chain cable or a wire (not notes,
  // routes or sends: a bus, or the chain's own note wire).
  bool cuttableEdge(uint32_t edge) const noexcept;
  // The model edge a Shift-drag would splice into now (kNone: none).
  uint32_t spliceEdge() const noexcept { return spliceEdge_; }
  // TX.block-drag: a dragged block (a selection of blocks, an opened capsule with its contents, a band by its title) is
  // drawn live where the pointer has it (CircuitCanvas::setNodeDrag), its cables on its ports; the model nodes lifted now
  // (empty / false: no block drag), and the blocks a drag moves (the pressed one, or the selection it belongs to).
  bool lifted(uint32_t modelNode) const noexcept { return modelNode < liftModel_.size() && liftModel_[modelNode] != 0; }
  bool bandLifted(uint32_t band) const noexcept { return liftBuilt_ && dragBand_ != kNone && band == dragBand_; }
  const std::vector<doc::DeviceId>& dragDevices() const noexcept { return dragDevices_; }
  // TX.block-drag: below this many device px across, a block is hit as if it were this size (a tiny block can be grabbed)
  // and its pins reach no further than kSmallPinReach of its size (its body is not all pins).
  static constexpr float kMinHitPx = 8.0f;
  static constexpr float kSmallPinReach = 0.3f;

protected:
  void paintUnderlay(ui::PaintContext& ctx) override;
  void paintNodeOverlay(ui::PaintContext& ctx) override;
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
  // TX.block-drag: the dragged blocks drawn at the pointer (the lift set built on the first move and after a rebuild);
  // endLift: the drag is over (the drop's scene places them, or they are back in place).
  void liftDrag();
  void buildLift();
  void endLift();
  void pickDragDevices(uint32_t pressed);  // the pressed block, or the selection it is in
  std::vector<doc::DeviceId> dragDevices_;
  std::vector<uint8_t> liftModel_;
  std::vector<uint32_t> liftScene_;
  bool liftBuilt_ = false;
  bool liftDragging() const noexcept { return drag_ == Drag::MoveDevice || drag_ == Drag::Nudge || drag_ == Drag::Splice; }

  ProjectPatchView& view_;
  Drag drag_ = Drag::None;
  ui::Vec2 press_{0, 0}, last_{0, 0};
  PinHit from_{};
  uint32_t dragNode_ = kNone;      // the device / mixer being dragged
  uint32_t dragBand_ = kNone;      // a band's title being nudged
  uint32_t dragSend_ = 0;          // the send row of a Send drag
  // TX.resize: the block being resized (by device: the model is rebuilt per step), its scale and size at the press.
  doc::DeviceId resizeDevice_;
  float resizeK0_ = 1.0f, resizeW0_ = 0.0f, resizeH0_ = 0.0f;
  double dragStart_ = 0.0;         // the value at the press (volume fader position, pan, send position)
  ui::Vec2 pointer_{0, 0};         // the last pointer position (the readout is drawn there)
  bool gesture_ = false;
  bool outside_ = false;           // TX.free-rack: a block drag is out of the canvas (the host shows where it lands)
  void leaveOutside(ui::Vec2 pos);  // … and no longer (the host's marker goes)
  ui::Cursor hoverCursor_ = ui::Cursor::Arrow;     // T4.16c: pressCursor at the last move
  ui::Cursor wireCursor_ = ui::Cursor::Crosshair;  // T4.16c: during a wire drag
  mutable std::string infoTitle_;                  // TMM.10-fix2: a hovered note source's info (info() returns views)
  // TX.lane-edit
  bool laneFocus_ = false;   // the last press was in a lane window
  bool laneHover_ = false;   // the pointer rests over a window body (laneCursor_ is what a press there would do)
  ui::Cursor laneCursor_ = ui::Cursor::Arrow;
  bool lanePointer(const ui::PointerEvent& e);  // the windows' share of onPointer (true: taken)
  // TX.node-menu
  void finishBackWire(ui::Vec2 pos);
  bool pressBackWire(const ui::PointerEvent& e);  // W + a press on a param pin / control (true: a backwards cable)
  void updateSplice(ui::Vec2 pos);
  void endSplice(bool apply, ui::Vec2 pos);
  bool sticky_ = false;                  // a Ctrl-release keeps a cable on the pointer
  bool swallowUp_ = false;               // the release of a held cable's click
  doc::DeviceId fromDevice_;             // the held cable's source (re-resolved after each rebuild)
  std::string fromPort_;
  doc::TrackId fromNotes_;               // … or a note source's output
  doc::DeviceId backDevice_;             // BackWire: the param's device and name, where the cable starts
  std::string backParam_;
  ui::Vec2 backStart_{0, 0};
  uint32_t spliceEdge_ = kNone;          // Splice: the cable under the dragged block
  Selection spliceKeep_;                 // the selection before the Shift-drag (the hovered cable is shown selected)
  // TX.node-menu-fix1: the cable on the pointer (a Wire drag from an output, a held cable, a backwards cable) is a
  // real cable (CircuitCanvas::setLooseCable) from its port to the pointer, snapped onto the port or control a release
  // would connect (its end there, highlighted). Called on the press and every move.
  void updateLoose(ui::Vec2 pos);
  graph::Point2 backWorld_{0, 0};        // BackWire: where the cable starts (the param's pin, or its control's rim)
  bool menuFromDrop_ = false;            // the last release opened the node menu (the loose cable hangs until it closes)
  void rememberSource();                 // fromDevice_ / fromPort_ / fromNotes_ from from_
  bool resolveSource();                  // from_ from them (false: the source is gone)
  // TX.node-menu-fix1: a document change rebuilds the model while a gesture holds model indices (a block or a band
  // being dragged, a cable's source, the cable a splice would cut). The view calls beforeRebuild() with the old model
  // and afterRebuild() with the new one: the indices are found again by identity (kind, device, track); a gesture whose
  // block, band or source went away ends (no stale index is ever read).
  struct NodeKey {
    NodeKind kind = NodeKind::Device;
    doc::DeviceId device;
    doc::TrackId track;
    bool valid = false;
  };
  NodeKey keyOf(uint32_t node) const noexcept;
  uint32_t findKey(const NodeKey& k) const noexcept;
  void beforeRebuild();
  void afterRebuild();
  NodeKey keepNode_, keepFrom_;
  doc::TrackId keepBand_;
  std::string keepFromPort_;
  bool keepFromInput_ = false;
};

class ProjectPatchView : public ui::Widget {
public:
  explicit ProjectPatchView(ViewContext& ctx);
  // TX.groups: an EMBEDDED view of one group's contents (the Device view's group panel): the same model, canvas and
  // gestures, scoped to the group (LayoutOptions::scope: its capsule, terminals, members and the wires among them). It is
  // shown while its parent is; it never morphs, binds no view actions and fits the capsule when it first builds.
  ProjectPatchView(ViewContext& ctx, doc::DeviceId scope);
  // TX.free-rack: an EMBEDDED view of one track's free devices (the Device view's free-device rack,
  // LayoutOptions::freeTrack): embedded like a group panel; setRackTrack picks the track (the Device view's), and the
  // view fits its contents again when the track changes. Inside it a block dragged moves freely (a nudge); a block or a
  // wire dragged out of the canvas is handed to the hooks below (the Device view: into the chain, onto a chain knob).
  struct FreeRack {};
  ProjectPatchView(ViewContext& ctx, FreeRack);
  ~ProjectPatchView() override;
  doc::DeviceId scope() const noexcept { return scope_; }
  bool freeRack() const noexcept { return freeRack_; }
  bool embedded() const noexcept { return scope_.valid() || freeRack_; }
  void setRackTrack(doc::TrackId track);
  doc::TrackId rackTrack() const noexcept { return rackTrack_; }
  // TX.free-rack hooks (set by the host): a block of the canvas dragged out of it (`drop` false while moving: show
  // where it would land, with `device` invalid when it came back in or the drag ended; true: released there — true
  // when the host took it), and a wire dragged out of it from `from`.`port` (the host takes the drag over: the canvas
  // forgets it).
  std::function<bool(doc::DeviceId device, ui::Vec2 pos, bool drop)> onBlockOutside;
  std::function<void(doc::DeviceId from, const std::string& port, ui::Vec2 pos)> onWireOutside;
  // TX.free-rack: where a device's output `port` (its first output when there is no such port) / its input or param
  // `port` (a pin, or a wired param's control) is drawn now (device px, clamped into the canvas; `docked` false when it
  // was clamped). nullopt: the device has no block here.
  std::optional<ui::Vec2> outputPoint(doc::DeviceId device, std::string_view port, bool* docked = nullptr) const;
  std::optional<ui::Vec2> inputPoint(doc::DeviceId device, std::string_view port, bool* docked = nullptr) const;
  // TX.free-rack: what a wire dropped at device point `pos` would drive (an input pin, a face's / panel's control): the
  // device and its param or input; invalid: nothing there.
  struct WireTarget {
    doc::DeviceId device;
    std::string param;
  };
  WireTarget wireTargetAt(ui::Vec2 pos) const;
  // TX.free-rack: the rack's menu (Add Free Device ▸, Zoom to Fit) at `at`.
  ui::Menu* openRackMenu(ui::Vec2 at);
  // The Add Free Device items for `track` (stock effects, meta modules); a pick runs addFreeDevice, or `pick` when set.
  std::vector<ui::MenuItem> addFreeDeviceItems(doc::TrackId track, std::function<void(const std::string& patch)> pick = {});
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

  // ---- lane-window editing (TX.lane-edit) ------------------------------------------------------------------------
  // A window body is an editing surface of its track: the ARRANGEMENT's own editor and automation run there (one
  // selection, one clipboard, one undo history; src/arrangement/edit_surface.h) — a drag selects time, the clip gestures
  // are the arrangement's (title strip: move, Alt: copy, edges: trim, audio handles: fades / gain), the right-click menu
  // is the arrangement's (Insert MIDI Clip = Ctrl+Shift+M …) plus Automation Mode and the parameter drawn over the clips;
  // in automation mode (A) the chosen parameter's envelope is drawn and edited over the clips as in the arrangement. While
  // the last press was in a window, the arrangement's edit chords (its keymap scope) act there (laneKey).
  uint32_t laneBodyAt(ui::Vec2 device) const;    // the band whose window body is under a device point (kNone: none)
  bool lanePointer(const ui::PointerEvent& e);   // a press / drag / release in a window (true: taken)
  bool laneDragging() const noexcept;            // a clip, time-selection, handle or breakpoint drag runs in a window
  void laneHover(ui::Vec2 device, ui::Mods mods);  // the part a press would grab, highlighted (and laneCursor's)
  void clearLaneHover();
  ui::Cursor laneCursor(ui::Vec2 device, ui::Mods mods) const;
  bool laneKey(const ui::KeyEvent& e);           // an arrangement edit chord on the windows (true: it ran)
  void laneAbort();                              // Esc / capture lost: the window's drag ends without an edit
  arrangement::EditSurface* laneSurface() const noexcept;  // null: no arrangement next to the view (tests)

  // ---- edits (each one undo step through pq::doc::ops / commands; false and the reason in the status line when it
  // could not be done) ---------------------------------------------------------------------------------------------
  bool connect(doc::DeviceId from, const std::string& port, doc::DeviceId to, const std::string& param);
  // TMM.10-fix2: MIDI track `track`'s note source, its note output `port` (doc::kNoteOutputs), into `to`.`param`.
  bool connectNotes(doc::TrackId track, const std::string& port, doc::DeviceId to, const std::string& param);
  // `track`'s output into `target`: a group or the master (its output), a return (its send to 0 dB).
  bool routeTo(doc::TrackId track, doc::TrackId target);
  bool sendOn(doc::TrackId track, doc::TrackId ret);
  // TX.all-free: a cable with these ends (doc::ops::connect; doc::canonicalWire writes two main ports as kMainPort): a
  // device's port or a track's source → a device's param / input or a track's mixer.
  bool connectEnds(doc::Wire ends);
  // A device into `track`'s main path at slot `index` (doc::ops::addDevice; kOffPath: unwired).
  bool addDevice(doc::TrackId track, const std::string& patch, uint32_t index = doc::kAppend);
  // TX.all-free: a block added to `track` unwired (placed under its path; it plays once a cable reaches it).
  bool addFreeDevice(doc::TrackId track, const std::string& patch);
  // TX.all-free: the blocks off their main paths (doc::ops::takeOffPath).
  bool takeOffPath(std::span<const doc::DeviceId> devices);
  bool moveDevice(doc::DeviceId device, doc::TrackId toTrack, uint32_t index);
  // TX.all-free: a block dropped on another band: to that track, off its path, placed at world point `at`.
  bool moveToTrack(doc::DeviceId device, doc::TrackId toTrack, graph::Point2 at);
  // TX.block-drag: a selection of blocks dropped on another band: all to that track (one undo step), each centred at
  // its `at`.
  bool moveToTrack(std::span<const doc::DeviceId> devices, doc::TrackId toTrack, std::span<const graph::Point2> at);
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
  // TX.block-drag: several at once (one re-layout); moved blocks are the lighter ones when they push others apart.
  void nudge(uint32_t entity, float dx, float dy);
  void nudge(std::span<const uint32_t> entities, float dx, float dy);
  // Reset Layout: every nudge's position (TX.resize: blocks keep their sizes).
  void resetLayout();
  // TX.resize (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"): a device block's uniform scale — its face,
  // panel or plain block and everything on it in proportion (doc::PatchNudge::scale: view state like a nudge, saved with
  // the project, not undoable). setBlockScale clamps (clampBlockScale) and re-lays out (neighbours make room); false:
  // unchanged. 1 = its own size.
  float blockScale(doc::DeviceId device) const noexcept;
  bool setBlockScale(doc::DeviceId device, float scale);
  // doc::kMinPatchScale … kMaxPatchScale; `snap`: to kBlockScaleStep (Shift while dragging); else whole percent, with a
  // detent at 1 (within kBlockScaleDetent).
  static float clampBlockScale(float scale, bool snap) noexcept;
  static constexpr float kBlockScaleStep = 0.25f;
  static constexpr float kBlockScaleDetent = 0.04f;

  // The selected devices / tracks (from the canvas selection, and a selected group's capsule), for tests and the menus.
  std::vector<doc::DeviceId> selectedDevices() const;

  // ---- TX.node-menu (see the file comment; each one undo step; false + the reason in the status line when refused) -----
  // What a menu choice places: a catalog entry (circuit::NodeItem and its id) or a typed spec (NodeItem::Spec).
  struct NodeChoice {
    circuit::NodeItem item = circuit::NodeItem::Spec;
    std::string id;
    std::string name;
  };
  static NodeChoice choiceOf(const circuit::NodeEntry& e);
  static NodeChoice specChoice(std::string spec);
  // How a spawned node arrives wired: from an output (a device's port, or a MIDI track's note output) into its main
  // input, or its main output into a param (or input) of a device.
  struct SpawnLink {
    enum class Kind : uint8_t { None, FromOutput, IntoParam };
    Kind kind = Kind::None;
    doc::DeviceId device;  // FromOutput: the source (invalid with `notes`); IntoParam: the target
    std::string port;      // FromOutput: the output; IntoParam: the param or input
    doc::TrackId notes;    // FromOutput: a note source's track
  };
  // Places `c` at world point `at` (see "node menu" above), wired by `link`. The new device (invalid: refused).
  doc::DeviceId spawnNode(const NodeChoice& c, graph::Point2 at, const SpawnLink& link = {});
  // Over a cable (a model edge): `c` between its ends; Z: a VCA at 1.
  bool insertOnEdge(uint32_t edge, const NodeChoice& c);
  bool insertVca(uint32_t edge);
  // Shift-splice: `device` (a main input and output) into the cable.
  bool spliceIntoEdge(doc::DeviceId device, uint32_t edge);
  // Ctrl+Delete: `device` goes, its main input's sources wired to its main output's targets.
  bool dissolve(doc::DeviceId device);
  // The menus: spawn at device point `at` (wired by `link`; its filter from the link), or insert on `edge`.
  circuit::NodeMenu* openSpawnMenu(ui::Vec2 at, const SpawnLink& link = {});
  // TX.node-menu-fix1: while a node menu is open, where its choice goes (world units; drawn as a marker), else nullopt.
  const std::optional<graph::Point2>& spawnMark() const noexcept { return spawnMark_; }
  circuit::NodeMenu* openInsertMenu(uint32_t edge, ui::Vec2 at);
  // The main input / output of a device as it is now ("" = none), from its patch's signature (catalog facts).
  circuit::MainPorts mainPorts(doc::DeviceId device) const;
  // Push-apart — TX.block-drag: BLOCKS NEVER OVERLAP, one rule for every block. After every re-layout (a move, a drop, a
  // spawn, a resize, a nudge, a layout change: each rebuild of the model; not while a block is being dragged or resized,
  // whose release settles), blocks of one level (a band's blocks outside capsules; an opened capsule's members) that
  // overlap (closer than kPushDetect: 0, their rects share area; touching is not overlapping) are separated to kPushGap (solved at once along each pair's axis of least overlap, shared by mass:
  // a block's area, a just-moved / spawned block a quarter of it so it gives way most, a just-resized one four times;
  // the band's source, mixer, Out and a capsule's terminals stand, its title strip is a wall) and each glides there as a
  // critically damped spring (≈ 0.3 s; nudges, view state), one re-layout per frame while it runs. A capsule or band
  // that grows into others is settled by the next round (at most kPushCascade rounds after one change). Then no frames.
  // A push no gesture caused (a project opened with overlapping blocks) moves them without marking the project changed.
  // Off (tests, scenes): at once. Every device block moves, including chain blocks, opened capsules and opened devices
  // (TX.all-free: every block is free; Q205's "only free blocks" is gone).
  bool pushing() const noexcept { return !push_.bodies.empty(); }
  void setPushAnimated(bool on) noexcept { pushAnimated_ = on; }
  static constexpr float kPushGap = 12.0f;     // world units between blocks pushed apart
  static constexpr float kPushDetect = 0.0f;   // closer than this is an overlap (well under the gap: no re-push)
  static constexpr uint32_t kPushCascade = 4;
  uint64_t pushFrames() const noexcept { return push_.frames; }
  uint64_t pushes() const noexcept { return push_.count; }  // pushes started (tests)
  // The rule's check now (a gesture that deferred it ends: an S-resize's release); true: a push started.
  bool settleOverlaps();
  // A push still gliding lands at once (a block drag starts: it moves the blocks from where they end).
  void landPush();
  // The blocks closer than kPushDetect at one level now (world units; tests): pairs of device ids.
  std::vector<std::pair<doc::DeviceId, doc::DeviceId>> overlappingBlocks() const;

  // ---- TX.groups (doc::ops::groupInCircuit / ungroupInCircuit; one undo step each; false + the reason in the status
  // line when refused) -----------------------------------------------------------------------------------------------
  bool groupDevices(std::span<const doc::DeviceId> devices);
  bool ungroup(doc::DeviceId group);
  bool groupSelection();    // Ctrl+G: the selected devices
  bool ungroupSelection();  // Ctrl+Shift+G: the selected groups (or the groups of the selected members)
  // A device into a group (invalid: out of its group; a device of the series leaves it first): one undo step.
  bool setDeviceGroup(doc::DeviceId device, doc::DeviceId group);
  // An opened group's capsule (its contents placed inside) or a closed block (a miniature): view state of the session.
  void setGroupOpen(doc::DeviceId group, bool open);
  bool groupOpen(doc::DeviceId group) const noexcept;
  // An opened capsule is no scene node (its contents are): pressing its frame selects the group here (cleared by any
  // other press); its frame is outlined while selected.
  void selectGroup(doc::DeviceId group);
  doc::DeviceId selectedGroup() const noexcept { return selectedGroup_; }

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
  // TX.cables: what the canvas's cables glow with — per scene wire 0 … 1 (a wire into a param: its modulation sweep; an
  // audio or note path: its track's peak level), and whether a source may be live (the meters, the modulation taps).
  void cableLevels(std::span<const uint32_t> wires, std::span<float> levels);
  bool cableSignalLive() const;

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
    uint64_t rescales = 0;  // TX.resize: block scale changes (each a re-layout)
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
  friend class LaneEditSurface;
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
  std::optional<LayoutOptions::Anchor> controlAnchorOf(const doc::Device& d, std::string_view param);

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
  // TX.lane-edit: main view only, made and registered with the arrangement next to the view on first use; laneArr_ is
  // that arrangement while the surface is registered (its Surfaces unregister it when the arrangement goes first).
  mutable std::unique_ptr<LaneEditSurface> laneEdit_;
  mutable ArrangementView* laneArr_ = nullptr;
  LaneEditSurface* laneEditReady() const;  // null: embedded, or no arrangement
  ArrangementView* laneArrangement() const;  // the arrangement laneEdit_ is registered with (null: none)
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
  // TX.groups
  doc::DeviceId scope_;
  // TX.free-rack
  bool freeRack_ = false;
  doc::TrackId rackTrack_;
  ui::IRect fitRect_{};                    // the canvas's rect and view at the last fit (an untouched view refits)
  circuit::CircuitCanvas::View fitView_{};
  void fitEmbedded();
  std::vector<doc::DeviceId> closedGroups_;
  doc::DeviceId selectedGroup_;
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

  // TX.node-menu: push-apart. Bodies are device blocks (by id) with their world rect at the start, offset, velocity.
  struct Push {
    struct Body {
      doc::DeviceId device;
      float x = 0, y = 0, w = 0, h = 0;  // world rect when the push began (before the offset)
      float dx = 0, dy = 0, vx = 0, vy = 0;
      float tx = 0, ty = 0;              // the offset it ends at (solved when the push begins)
      float minY = 0;                    // its band's title bottom: never pushed above it
      float mass = 1.0f;                 // TX.block-drag: its share of a separation (0: it stands)
      uint32_t level = 0;                // TX.block-drag: bodies meet only within one level (a band's, a capsule's)
      bool movable = true;
      doc::PatchNudge base;              // its nudge when the push began
    };
    std::vector<Body> bodies;
    double last = -1.0;  // frame time of the last step
    uint64_t frames = 0;
    uint64_t count = 0;
    bool user = false;  // a gesture's (a drop, a spawn, a resize): the project is changed (view state)
  } push_;
  bool pushAnimated_ = true;
  // TX.block-drag: blocks just moved (a drop, a spawn, a nudge) and resized (by device id), what the next check weighs;
  // the rounds of pushes since the last user change (kPushCascade).
  std::vector<uint32_t> moved_, resized_;
  uint32_t cascade_ = 0;
  bool settleHeld_ = false;  // no check in this re-layout (a push's own; several blocks placed one by one)
  // The bodies of every level of the model, by level (TX.block-drag); with `onlyOverlapping` the levels that overlap.
  void collectBodies(std::vector<Push::Body>& out, bool onlyOverlapping) const;
  bool blockGestureRunning() const noexcept;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);  // the node menu's callbacks check it (the view may go first)
  std::optional<graph::Point2> spawnMark_;  // TX.node-menu-fix1
  bool startPush();  // the rule's check and solve (true: a push started)
  bool stepPush(double seconds);  // false: settled
  void applyPush();
  void finishPushNow();
  // Moves a free block to world point `at`: its centre there, or (TX.node-menu-fix1) with `inPort` its input pin there
  // (a cable dropped from an output: the cable reaches the block where it was dropped), with `outPort` its output pin.
  void placeAt(doc::DeviceId device, graph::Point2 at, std::string_view inPort = {}, std::string_view outPort = {});
  bool dispatchSteps(const char* label, std::vector<doc::Command> steps);
  bool insertImpl(uint32_t edge, const NodeChoice& c, bool plain);  // plain: Z's VCA (no default preset)

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
// TX.spectrum: the analysis displays — "Mix" (an audio track: EQ Eight with three bands moved, then a Spectrum meta
// device). The ids.
struct MetaSpectrumIds {
  doc::TrackId keys;
  doc::DeviceId eq, spectrum;
};
Status buildSpectrumProject(doc::DocumentStore& store, MetaSpectrumIds* ids = nullptr);
// TX.groups: the user's example — "Keys" plays the default synth → Phase accumulator (110 Hz) → Wavetable (demoWavetable
// Data, position 0.35) → Phase accumulator (3 Hz), with a free LFO (0.5 Hz) wired onto the Wavetable's position; "Bass"
// the default synth → Saturator → Utility. Nothing grouped yet (ops::groupInCircuit needs the device library). The ids.
struct GroupProjectIds {
  doc::TrackId keys;
  doc::DeviceId pa1, wt, pa2, lfo;
  doc::TrackId bass;
  doc::DeviceId sat, util;
};
Status buildGroupProject(doc::DocumentStore& store, GroupProjectIds* ids = nullptr);
// TX.free-rack: "Lead" plays the default synth → Saturator → Auto Filter → Utility; `lfos` free LFOs (0.25 Hz apart) are
// each wired onto a knob of the chain (Saturator drive, Auto Filter frequency, Utility width, Auto Filter resonance,
// Saturator dry / wet, Utility balance, Auto Filter drive, Saturator output, then again), and a free Phase accumulator
// (55 Hz) feeds the Auto Filter's sidechain input, its frequency moved by the first LFO. The ids.
struct FreeRackIds {
  doc::TrackId lead;
  doc::DeviceId sat, filter, util;
  std::vector<doc::DeviceId> lfos;
  doc::DeviceId acc;
};
Status buildFreeRackProject(doc::DocumentStore& store, uint32_t lfos = 3, FreeRackIds* ids = nullptr);

}  // namespace project_patch

// Installs the view into ShellSlot::Patch and binds view.togglePatch (called from installDefaultContent, after the
// mixer: the mixer's toggle hides the patch view and the other way round).
project_patch::ProjectPatchView& installProjectPatch(Shell& shell);
project_patch::ProjectPatchView* findProjectPatch(const Shell& shell) noexcept;

}  // namespace pq::views
