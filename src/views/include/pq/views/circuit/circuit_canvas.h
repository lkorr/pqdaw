#pragma once

// The circuit canvas (T4.2, SPEC §7.3): a zoomable, pannable schematic of one CircuitScene — symbols, orthogonal wires
// coloured and thickened by kind and rate (bool thin green-ish, param thin grey, audio thicker in the signal colour,
// poly doubled, modulation into a parameter dashed), junction dots where a net branches (crossings have none), labels,
// selection and hit testing. It is the base of the circuit editor (T4.4: editing, text pane), live probing (T4.5) and
// the project patch view (T4.13); those build on the hooks below rather than on its internals.
//
// View: `View::x/y` is the world point at the canvas's top-left corner and `zoom` the logical px per world unit (device
// px = zoom × UI scale). Ctrl+wheel zooms at the pointer, the wheel pans (Shift: horizontally), the middle button (or
// Alt + left) drags the view, F fits the scene, = / − zoom, Esc clears the selection. Left click selects a node or wire
// (Shift adds, Ctrl toggles), a left drag on empty space selects with a rubber band, a double-click on a subcircuit or
// filter calls onOpen.
//
// Level of detail by zoom: Far (< 0.3): nodes as outlined boxes, wires 1 px in one colour, no dots or text — the whole
// picture cached in world-anchored chunks of 256 device px (two coverage tiles each: wires, nodes), so a pan or a settled
// frame draws ≈ 2 quads per chunk whatever the patch's size; while zooming the previous level's chunks are stretched,
// missing chunks are drawn directly and built in onFrame (≤ 4 per frame); the selection and the hovered item are drawn
// over them; Mid (< 0.75): symbols and
// styled wires with junction dots; Near: also titles, captions and (from zoom 1) pin labels in the UI font. Symbol bodies
// are cached as coverage tiles (pq/ui/tile_atlas.h, two per distinct body: fill and stroke) rasterised once per zoom
// level (pq/ui/raster.h) after a zoom settles, so a settled frame draws each symbol as two quads; while zooming, the
// previous level's tiles are drawn stretched, and without a tile atlas (no GPU) the bodies are drawn directly.
// Culling goes through the scene's spatial grid. Pan and zoom repaint the canvas; a selection or hover change repaints
// only the canvas too. Idle: no frames, no timers.
//
// Meta-module faces (TMM.4, pq/views/meta/meta_face.h): a node of CircuitScene::faces is drawn as its face instead of a
// symbol, at EVERY level of detail (Far included: faces are left out of the Far chunks and drawn over them, so zooming
// out removes nothing). A face's static layers are coverage tiles per exact zoom shared by every face of its type (built
// with the symbol tiles after a zoom settles, stretched meanwhile); its jacks and live layer are geometry each frame.
// Pointer events over a face's control go to the face in face-local world units (a drag at zoom 0.05 works as at 3);
// over its frame they select / move / open the node as for any block. While a bound VoiceTapReader is live and its face
// visible, a ≤ 60 Hz timer polls the readers and damages only the faces' live rects; otherwise nothing runs.
// TX.panel-faces: the same path hosts device panels (SceneFace::panel, made by panelMaker: pq/views/devices/
// panel_face.h) — any views::Face; a face with its own clock wakes the canvas (FaceHost::faceNeedsUpdate, Face::nextWake)
// and is laid out again once a zoom settles (Face::prepare).
//
// Hanging cables (TX.cables, SceneWire::cable; pq/views/circuit/cables.h): a cable wire is drawn over the blocks (after
// the faces and labels) as a glossy tube — width by zoom, a cylindrical shade across it, a specular highlight, a soft
// drop shadow, a plug at each end, coloured by its signal kind — following its simulated rope (CableSim: the scene's
// rest shape until something moves; a block moved, a drag offset or a control's anchor moving makes it swing and
// settle). Frames run only while a cable is awake; idle = none. A cable over a face whose control the pointer is on is
// drawn at kCableFade, and a press there goes to the control unless it is on a cable's end (its plug). Far: the rest
// shapes in the cached chunks, thin and in one colour, no motion, no glow. Glow: the host's cableLevels (0 … 1 per cable)
// polled at ≤ kCableGlowSeconds⁻¹ only while cableSignalLive() (and once after) and only for visible cables; a glowing
// cable is lit around (additive) with light flowing along it from source to target. Silent: no polls, no repaints.
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

#include "pq/graph/patch.h"
#include "pq/ui/widget.h"
#include "pq/views/circuit/circuit_scene.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views::circuit {

struct CanvasHit {
  enum class Kind : uint8_t { None, Node, Pin, Wire };
  Kind kind = Kind::None;
  uint32_t index = UINT32_MAX;  // node, pin or wire index in the scene
  bool operator==(const CanvasHit&) const = default;
};

class CircuitCanvas : public ui::Widget {
public:
  explicit CircuitCanvas(std::string name = "circuit");
  ~CircuitCanvas() override;

  // The scene to show (shared: a canvas never changes it). fit = frame the whole scene at the next layout. keepTiles
  // (T4.4: an edited patch's next scene): symbol body tiles stay (a body's pixels depend only on its bodyKey and the
  // zoom), so an edit rasterises only the bodies that are new; the far-level chunks are rebuilt either way.
  void setScene(std::shared_ptr<const CircuitScene> scene, bool fit = true, bool keepTiles = false);
  const std::shared_ptr<const CircuitScene>& scene() const noexcept;

  struct View {
    double x = 0.0, y = 0.0;  // world point at the canvas's top-left corner
    double zoom = 1.0;        // logical px per world unit
    bool operator==(const View&) const = default;
  };
  static constexpr double kMinZoom = 0.02;
  static constexpr double kMaxZoom = 8.0;
  static constexpr double kFarZoom = 0.3;    // below: Far detail
  static constexpr double kNearZoom = 0.75;  // from here: Near (text)
  static constexpr double kPinLabelZoom = 1.0;
  static constexpr double kZoomSettleSeconds = 0.12;  // tiles are rebuilt this long after the last zoom step
  const View& view() const noexcept;
  void setView(const View& v);
  void fitAll();                                // the whole scene, centred, at most zoom 2
  // M4 review M15: the view a patch opens at: fitAll when that is readable (≥ kNearZoom), else kNearZoom on the
  // outputs (setScene(…, fit = true) does this).
  void fitToRead();
  void zoomAt(ui::Vec2 device, double factor);  // keeps the world point under `device` where it is
  void panBy(float dxDevice, float dyDevice);
  float deviceScale() const noexcept;           // device px per world unit (zoom × UI scale)
  ui::Vec2 toDevice(graph::Point2 world) const noexcept;
  graph::Point2 toWorld(ui::Vec2 device) const noexcept;

  enum class Lod : uint8_t { Far, Mid, Near };
  Lod lod() const noexcept;

  // Topmost pin, else node, else wire under a device point (pins within 5 px, wires within 4 px).
  CanvasHit hitTest(ui::Vec2 device) const noexcept;
  // T4.16c: what the pointer rests on (the last move's hit test, as drawn in the hover colour; not updated during a
  // drag), and whether a middle / Alt drag is panning the view now (subclasses' cursorAt).
  const CanvasHit& hovered() const noexcept;
  bool panning() const noexcept;
  // Alt is held: a left press pans (T4.16-fix1: the cursor shows it before the press).
  bool altHeld() const noexcept;

  struct Selection {
    std::vector<uint32_t> nodes, wires;  // sorted
  };
  const Selection& selection() const noexcept;
  void select(std::span<const uint32_t> nodes, std::span<const uint32_t> wires, bool add = false);
  void clearSelection();
  bool nodeSelected(uint32_t node) const noexcept;
  bool wireSelected(uint32_t wire) const noexcept;

  // Hooks for the views built on the canvas.
  std::function<void()> onSelectionChanged;
  std::function<void(const CanvasHit&)> onHoverChanged;
  std::function<void(uint32_t node)> onOpen;  // double-click on a subcircuit or filter
  // T4.5 (bool wires lit when high): drawn in circuit.wireBoolLit. Repaints only when the flag changes.
  void setWireLit(uint32_t wire, bool lit);
  // T5.x-circuit-chain (M4 review L13): pin stubs drawn only from the Near level (the overviews built on the canvas: the
  // project patch view and the track chain circuit read as boxes and wires when zoomed out). Off by default.
  void setPinStubsNearOnly(bool on);
  bool pinStubsNearOnly() const noexcept;
  // ... and the terminal name pills (M15: below Near, inputs / params / outputs / exports keep their names) only from
  // this zoom (0, the default: at every level). The overviews set kFarZoom: zoomed far out, a whole device's
  // terminals would bury its boundary's title.
  void setTerminalPillMinZoom(double zoom);
  // TMM.13-fix2: a fit that leaves room across puts the contents at the left edge (the margin in), not centred.
  void setFitAlignLeft(bool on) noexcept;

  // ---- TMM.4: meta-module faces ----
  // What a face of the scene edits and shows (the host's document target and voice reader). Unset, or a null target:
  // the type's defaults, read-only. setScene calls it for every face; rebindFaces() again (the host's state moved).
  struct FaceBinding {
    std::unique_ptr<MetaTarget> target;
    VoiceTapReader* voices = nullptr;
    // TX.spectrum: where a face that shows its signal's analysis finds its feed (Face::setAnalysis; empty: none).
    std::function<devices::SpectrumFeed*()> analysis;
  };
  std::function<FaceBinding(const SceneFace&)> faceBinder;
  void rebindFaces();
  // The meta face on scene node `node` (null: none), the meta faces in scene order.
  MetaFace* faceOn(uint32_t node) const noexcept;
  size_t faceCount() const noexcept;
  MetaFace& faceAt(size_t i) const noexcept;
  uint32_t faceNode(size_t i) const noexcept;
  // ---- TX.panel-faces: stock device panels as faces ----
  // Makes the face of a panel entry of the scene (SceneFace::panel): the host's (the project patch view: the device's
  // Device-view panel, bound to the document). Unset or null: the node is drawn as a plain block. A new scene keeps the
  // panel face of the same owner and path. Panels are hosted exactly like meta faces (one path: drawn at every level of
  // detail over the Far chunks, pointer events on their controls routed to them in face-local world units, keys and
  // text for the one the last press went to, their own timers through Face::update / nextWake).
  std::function<std::unique_ptr<Face>(const SceneFace&)> panelMaker;
  size_t panelCount() const noexcept;
  Face& panelAt(size_t i) const noexcept;
  uint32_t panelNode(size_t i) const noexcept;
  Face* panelOn(uint32_t node) const noexcept;
  // A reader may have become live (a note, the transport): poll the faces now and keep animating while one is live.
  void wakeFaces();
  bool animatingFaces() const noexcept;  // the ≤ 60 Hz face timer is armed
  // TMM.7: a key for the face the last press went to (a keyframe's Delete); true: consumed. onKey calls it first;
  // hosts that handle keys before CircuitCanvas::onKey (the project view's Delete) call it before theirs.
  bool faceKey(const ui::KeyEvent& e);
  // Benchmarks (A/B): off draws face nodes as plain blocks and routes nothing to them.
  void setFacesShown(bool on);
  bool facesShown() const noexcept;
  static constexpr double kFaceFrameSeconds = 1.0 / 60.0;
  // TX.block-drag: a face on a node scaled below this (SceneNode::scale: a block shrunk to a miniature) is drawn but has
  // no controls — presses, hover and wire drops there are the block's (select, move, open), so a tiny block can be
  // grabbed. Zoom never changes it (a face far out keeps its controls, as before).
  static constexpr float kFaceMiniatureScale = 0.25f;

  // ---- TX.cables ----
  static constexpr double kCableGlowSeconds = 1.0 / 30.0;
  static constexpr float kCableFade = 0.3f;
  // Off (tests, goldens): cables jump to their rest shapes at once; on (default): they swing and settle.
  void setCablesAnimated(bool on);
  bool cablesAnimated() const noexcept;
  bool cablesMoving() const noexcept;  // a cable swings (the canvas runs frames until it settles)
  // The cable of scene wire `wire` as it hangs now (world units, source → target; empty: not a cable), its middle.
  std::span<const graph::Point2> cablePoints(uint32_t wire) const noexcept;
  graph::Point2 cableMidpoint(uint32_t wire) const noexcept;
  // TX.block-drag: blocks being dragged while the scene still has them where they were. Scene nodes `nodes` (sorted) are
  // LIFTED: drawn `offset` (world units) from their place in a second pass over everything else (their bodies, faces,
  // panels, labels, the wires between them, the host's under- and overlay parts of them: paintUnderlay /
  // paintNodeOverlay with lifting() true), hit-tested there, and their cables plugged into their ports there (they
  // swing). A trace with one end lifted is drawn as a Z between its ends; far out the chunks under them are drawn
  // directly without them. Per move of a drag only the offset changes (no allocation). A new scene drops the lift (the
  // host lifts the new scene's nodes again if the drag goes on); clearNodeDrag: back in place (a cancelled drag).
  void setNodeDrag(std::span<const uint32_t> nodes, graph::Point2 offset);
  void clearNodeDrag();
  std::span<const uint32_t> draggedNodes() const noexcept;
  graph::Point2 nodeDragOffset() const noexcept;
  bool nodeLifted(uint32_t node) const noexcept;
  // The lifted pass is being painted (paintUnderlay / paintNodeOverlay and what they call draw only the lifted parts).
  bool lifting() const noexcept;
  // TX.node-menu-fix1: the LOOSE cable — a new connection being drawn out (a drag from an output, a backwards cable,
  // a cable kept on the pointer after a Ctrl-release): a real cable from world point `from` (its port) to `to` (the
  // pointer, or the port / control it snaps to), drawn as every cable (tube, plugs, `style`'s colour) over the blocks
  // and simulated: it trails behind the pointer (kLooseCableCarry) and swings. `snapped`: over a valid target (drawn
  // with the selection halo). Call it on every move; frames run only while it moves. dropLooseCable(): released — the
  // new scene's cable from the same port whose other end is nearest its free end takes its motion over (no pop to the
  // rest shape); if none does, it goes at the next frame. clearLooseCable(): gone at once (a cancelled drag).
  void setLooseCable(graph::Point2 from, graph::Point2 to, const WireStyle& style, bool snapped);
  void dropLooseCable();
  void clearLooseCable();
  bool looseCable() const noexcept;
  bool looseCableSnapped() const noexcept;
  bool looseCableMoving() const noexcept;
  std::span<const graph::Point2> looseCablePoints() const noexcept;
  // The glow: the host fills `levels` (0 = silent … 1) for the listed scene wires from its live sources; whether any
  // may be non-zero now (the transport plays, a source is active). wakeCables(): a source may have become live — poll
  // now, and keep polling while it is live and once after.
  std::function<void(std::span<const uint32_t> wires, std::span<float> levels)> cableLevels;
  std::function<bool()> cableSignalLive;
  void wakeCables();
  float cableGlow(uint32_t wire) const noexcept;
  bool cableGlowPolling() const noexcept;  // the ≤ 30 Hz glow timer is armed

  struct Stats {
    uint32_t nodesPainted = 0, nodesFromTiles = 0, nodesStretched = 0, nodesDirect = 0;
    // TMM.4: faces painted, their static layers from tiles / stretched / drawn directly, face primitives emitted.
    uint32_t facesPainted = 0, faceLayersFromTiles = 0, faceLayersStretched = 0, faceLayersDirect = 0, facePrimitives = 0;
    uint32_t panelsPainted = 0;  // TX.panel-faces: device panels painted (not counted in facesPainted)
    uint32_t wiresPainted = 0, segmentsPainted = 0, junctionsPainted = 0, labelsPainted = 0;
    uint32_t cablesPainted = 0, cablesGlowing = 0;  // TX.cables (near / mid)
    uint64_t cableGlowPolls = 0;
    // Far level: chunks drawn from their tiles, from the previous zoom level's tiles (stretched), or directly.
    uint32_t chunksFromTiles = 0, chunksStretched = 0, chunksDirect = 0;
    uint64_t tilesBuilt = 0, tileFailures = 0, paints = 0;
  };
  const Stats& stats() const noexcept;
  bool zooming() const noexcept;
  bool tilesPending() const noexcept;  // a visible body has no tile at this zoom yet (built in onFrame)
  void dropTiles();                    // benchmarks: forget every tile

  void paint(ui::PaintContext& ctx) override;
  void arrange(ui::LayoutPass& pass) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  bool onText(const ui::TextEvent& e) override;  // TX.panel-faces: the focused face's type-in
  void onFrame(double now) override;
  void onTimer(uint32_t id) override;
  void onCaptureLost() override;
  void onInteractionCancelled() override;
  ui::InfoText info() const override;
  // T4.16c: Move while panning; otherwise inherited (the read-only viewer: Arrow). The hovered pin is ringed, the
  // hovered node and wire drawn in the hover colour.
  ui::Cursor cursorAt(ui::Vec2 pos) const override;

protected:
  // TMM.4: a pointer event for the faces (subclasses call it first in onPointer; the canvas's own onPointer does): a
  // press on a face's control and the drag it starts, the hover of controls, a wheel a face uses. True: consumed.
  bool facePointer(const ui::PointerEvent& e);
  // The cursor over a face's control (Inherit: none there) and its info-view text (false: no face part under the
  // pointer).
  ui::Cursor faceCursor(ui::Vec2 pos) const;
  bool faceInfo(ui::InfoText& out) const;
  // TX.patching: the param whose control is under device point `pos` on a face (Face::paramAt: a meta face's control,
  // a panel's knob), with the face's scene node; node UINT32_MAX / param "" when none (a cable drop target).
  struct FaceParamHit {
    uint32_t node = UINT32_MAX;
    std::string param;
  };
  FaceParamHit faceParamAt(ui::Vec2 pos) const;
  // TX.node-menu-fix1: where a cable onto param `param` of the face on scene node `node` ends now (world units: the
  // control's rim toward `toward`, as a cable's end onto it); nullopt: no such control shown.
  std::optional<graph::Point2> faceControlPoint(uint32_t node, std::string_view param, graph::Point2 toward) const;
  bool faceDragging() const noexcept;
  // Drawn over the scene, inside the canvas (editing previews, probes): last, once.
  virtual void paintOverlay(ui::PaintContext& /*ctx*/) {}
  // TX.block-drag: what the host draws over its nodes (after the scene's cables): once per pass — lifting() false for
  // the scene (leave lifted nodes' parts out: nodeLifted), true for the lifted nodes only (the view is shifted by their
  // offset then, so toDevice places them where they are drawn).
  virtual void paintNodeOverlay(ui::PaintContext& /*ctx*/) {}
  // T4.13: drawn under the scene (after the background, before wires and symbols; at every level of detail, so not
  // cached in the Far chunks): the project patch view's bands and group containers.
  virtual void paintUnderlay(ui::PaintContext& /*ctx*/) {}

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pq::views::circuit
