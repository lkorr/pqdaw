#pragma once

// The drawable model of one patch on the circuit canvas (T4.2): every node as a placed symbol with its pins, every
// wire as an orthogonal polyline with its style (kind / rate / poly / modulation), junction dots, labels and a spatial
// grid for culling and hit testing. Built once per patch (or per edit, T4.4) on the UI thread; the canvas only reads it.
//
// Positions come from the patch's stored layout (the `layout { … }` block, T4.3). Nodes without a position are placed
// by graph::completeLayout (an auto-layout of the whole patch with the symbol geometry when nothing is placed); wires
// without routing points are drawn as a Z through the gap (graph::routeUnrouted). Corners are added where a stored
// route does not meet a pin orthogonally (a symbol that changed size), so every wire is drawn orthogonal.
//
// Wire style (SPEC §7.3): the kind and rate of the driving output, from declared types where they are concrete and a
// local forward propagation otherwise (consts are const rate, params param rate, stateful atoms and audio inputs audio
// rate, math follows the highest input rate; bool from comparators and logic). Voice-polyphonic values (voice.*,
// `… voice` inputs and what they feed) are drawn doubled. An audio-rate signal into a subcircuit's param pin is
// modulation: dashed. The scene's styles are plain data, so T4.5 can light bool wires and T4.13 can colour project
// wires the same way.
//
// Inactive parts (T4.15): SceneOptions::inactiveNodes / inactiveWires (graph::analyseStructure: what a structural
// constant switches off) mark nodes and wires inactive. With hideInactive they are HIDDEN: they keep their index (so
// selection, probes and edits address the same nodes) but are in no grid cell, have no segments, labels or junctions,
// and do not count for the bounds, so nothing draws, hits or frames them; with relayoutVisible the visible nodes are
// laid out on their own (graph::autoLayout of the visible subgraph: the hidden branches leave no holes), else they
// keep the stored positions. Without hideInactive they are drawn dimmed where they are.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/layout.h"
#include "pq/graph/patch.h"
#include "pq/graph/structure.h"
#include "pq/views/circuit/orbit.h"
#include "pq/views/circuit/symbols.h"

namespace pq::views::circuit {

enum class WireRate : uint8_t { Const, Param, Audio };

struct WireStyle {
  bool boolean = false;     // kind bool (else real)
  WireRate rate = WireRate::Audio;
  bool poly = false;        // per voice: doubled line
  bool modulation = false;  // an audio-rate signal into a parameter: dashed
  bool notes = false;       // TMM.13-fix2 (M10): the note source's (notes into a chain, its outputs' wires): wireNotes
  bool operator==(const WireStyle&) const = default;
};

struct ScenePin {
  float x = 0.0f, y = 0.0f;  // world position on the node's edge
  float stub = 0.0f;         // towards the body (+x for inputs, −x for outputs)
  graph::NodeIndex node = graph::kNone;
  uint16_t port = 0;
  bool input = true;
  bool visible = true;
  bool connected = false;
  uint32_t label = UINT32_MAX;  // into CircuitScene::strings (labelled symbols), or none
  // TX.orbit-ports (pq/views/circuit/orbit.h): a block's main input / output ORBITS its node's outline (SceneNode::
  // outline): (x, y) is where it settles (at `angle`, as the layout solved it; the cables' rest shapes start there), and
  // the canvas moves it along the outline live (a drag) and animates the slide. `pinned`: the user placed it (the angle
  // is kept). `orbitKey` names the port across rebuilds of the scene (its slide continues from where it was drawn).
  bool orbit = false;
  bool pinned = false;
  float angle = 0.0f;
  uint64_t orbitKey = 0;
};

struct SceneNode {
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;  // world rect (top-left, size)
  uint32_t spec = 0;                              // into CircuitScene::specs
  uint32_t firstPin = 0, pinCount = 0;            // into CircuitScene::pins (inputs, then outputs)
  graph::NodeIndex node = graph::kNone;
  bool inactive = false;  // T4.15: switched off by a structural constant (drawn dimmed)
  bool hidden = false;    // … and hidden (not drawn, not hit)
  // TX.resize: the node drawn `scale` times its own size (the project patch view's S-drag): the rect above is already the
  // scaled one; a face or panel on it is drawn, hit-tested and laid out at scale × the canvas's px per world unit, in
  // face-local units of its own size (local = (world − x, y) / scale). 1 everywhere else.
  float scale = 1.0f;
  // TX.orbit-ports: the block's outline (world units: a circle for a round face, a rounded rect otherwise), which its
  // orbiting pins (ScenePin::orbit) sit just outside of. Unset (invalid) for nodes without orbiting pins.
  Outline outline;
};

struct SceneWire {
  uint32_t firstPoint = 0, pointCount = 0;  // into CircuitScene::points: source pin … target pin (≥ 2, orthogonal)
  WireStyle style;
  uint32_t net = 0;                         // wires driven by the same output share a net
  graph::WireIndex wire = graph::kNone;
  uint32_t fromPin = 0, toPin = 0;          // into CircuitScene::pins
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // bounding box
  bool inactive = false;  // T4.15 (see SceneNode)
  bool hidden = false;    // no points, no segments
  // TX.mod-all: into strings — the param whose control on the face of the target pin's node the wire ends on (the
  // project patch view's modulation wires; UINT32_MAX: none). Its points end on the node's edge; the canvas draws the
  // rest over the face, from there to wherever the face's Face::controlAnchor puts the control when it paints.
  uint32_t control = UINT32_MAX;
  // TX.cables: a hanging cable (the project patch view's connections between blocks), not an orthogonal trace: its
  // points are its rest shape from the source pin to the target (circuit/cables.h cableRest; the segments and the grid
  // hold that curve, the far level draws it), and the canvas draws and hit-tests the simulated rope (CircuitCanvas).
  // `cableKey` names the connection across rebuilds of the scene (a kept cable keeps its motion). A cable with
  // `control` ends on that control (its last point is the control's centre when the scene was built).
  bool cable = false;
  uint64_t cableKey = 0;
  // TX.drag-grow: an end with no pin of its own (the project patch view's chain into / out of an opened capsule's frame
  // edge): the scene node it moves with (the capsule's In / Out terminal at that edge); UINT32_MAX: its pin's node.
  uint32_t fromNode = UINT32_MAX, toNode = UINT32_MAX;
};

// Text drawn at a world position: centred in [x0, x1] at y (the cap-centre line) when it fits, else hidden.
struct SceneLabel {
  float x0 = 0.0f, x1 = 0.0f, y = 0.0f;
  uint32_t text = 0;  // into strings
  enum class Align : uint8_t { Left, Center, Right } align = Align::Center;
  enum class Role : uint8_t { Title, Caption, Pin, Comment } role = Role::Title;
  uint32_t node = UINT32_MAX;  // owning scene node (none for comments)
};

// Uniform grid over the scene's bounds: per cell, the nodes and wire segments touching it.
struct SpatialGrid {
  float cell = 256.0f;
  float x0 = 0.0f, y0 = 0.0f;
  uint32_t cols = 0, rows = 0;
  std::vector<uint32_t> nodeBegin, nodeItems;     // CSR: cell → node indices
  std::vector<uint32_t> segBegin, segItems;       // CSR: cell → segment indices (into CircuitScene::segments)
  void cellRange(float wx0, float wy0, float wx1, float wy1, uint32_t& c0, uint32_t& r0, uint32_t& c1,
                 uint32_t& r1) const noexcept;
};

// One straight piece of a wire (between two consecutive points).
struct SceneSegment {
  uint32_t wire = 0;
  uint32_t point = 0;  // index of its first point in CircuitScene::points
};

// TMM.4: a node drawn as a meta-module face (SymbolSpec::face): its type and the instance's name path relative to the
// scene's patch (the call's binding name; "" for a chain meta device in the project patch view). `owner`: whose state
// it is — 0 for the scene's own patch, else the host's id (the project patch view: the device id).
// TX.panel-faces: or a stock device's panel (`panel`; type null): the host's CircuitCanvas::panelMaker makes the face
// (owner = the device id, path = what the panel fronts: the device's patch, so a rebuilt scene keeps the panel).
struct SceneFace {
  uint32_t node = 0;  // scene node
  const meta::Type* type = nullptr;
  std::string path;
  uint32_t owner = 0;
  bool panel = false;
};

// TMM.6: an `oversample(n) { … }` region, drawn as one wash in the signal colour behind exactly its members: each
// visible member node padded by kRegionPad, every wire between two members as a band, and above the first member (top,
// then left) a title band holding "×n" (and its phase inputs). The layout does not group a region's nodes, so a box
// around them would also frame outer nodes placed between them; the wash marks membership wherever they are.
struct SceneRect {
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};
struct SceneRegion {
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // bounds of its rects (culling, the scene's bounds)
  uint16_t region = 0;                              // graph::Node::region
  uint32_t text = 0;                                // into strings: "×4" or "×4 · phase: ph"
  uint32_t firstRect = 0, rectCount = 0;            // into CircuitScene::regionRects; the first is the title band
};
inline constexpr float kRegionPad = 14.0f;    // world units around a member node (its caption fits inside)
inline constexpr float kRegionBand = 6.0f;    // half the width of a wire band
inline constexpr float kRegionTitle = 16.0f;  // the title band above the first member

struct CircuitScene {
  std::string patchName;
  std::vector<SymbolSpec> specs;  // one per node, in graph node order
  std::vector<SceneNode> nodes;   // one per graph node (index = graph::NodeIndex)
  std::vector<ScenePin> pins;
  std::vector<graph::Point2> points;
  std::vector<SceneWire> wires;   // one per graph wire (index = graph::WireIndex)
  std::vector<SceneSegment> segments;
  std::vector<graph::Point2> junctions;  // where a net branches (≥ 3 directions meet)
  std::vector<uint32_t> junctionWire;    // … a wire of that net (its style colours the dot)
  std::vector<SceneLabel> labels;
  std::vector<std::string> strings;
  std::vector<SceneFace> faces;  // TMM.4, in node order
  std::vector<SceneRegion> regions;  // TMM.6, in region order (regions without a visible node are left out)
  std::vector<SceneRect> regionRects;
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // bounds of everything
  SpatialGrid grid;
  uint32_t autoPlaced = 0;  // nodes completeLayout placed
  uint32_t hiddenNodes = 0, hiddenWires = 0;  // T4.15
  uint32_t visibleNodes() const noexcept { return static_cast<uint32_t>(nodes.size()) - hiddenNodes; }
};

struct SceneOptions {
  graph::AutoLayoutOptions layout;
  // T4.15 (see above): per node / per wire of the patch, 1 = inactive (empty: none).
  std::span<const uint8_t> inactiveNodes, inactiveWires;
  bool hideInactive = true;
  bool relayoutVisible = false;  // with hideInactive: lay out the visible nodes on their own
  // TMM.12: the instance's structural constants (optional): an `oversample(hq)` region whose setting is 1 is no
  // region (no wash), else its title shows the value; without it the title shows the setting's name.
  const graph::StructuralInfo* structure = nullptr;
};

// The scene of `patch` as stored, completing a missing or partial layout (see above). `laidOut` (optional) receives the
// patch with the layout that is drawn (for "tidy" and saving it back to text).
std::shared_ptr<CircuitScene> buildScene(const graph::Patch& patch, const SceneOptions& options = {},
                                         graph::Patch* laidOut = nullptr);

// The derived parts of a scene whose nodes, pins, points, wires (with their nets), segments and labels are filled in:
// junction dots (per net, where three or more directions meet), the bounds and the spatial grid. buildScene() ends
// with it; T4.13's project patch view builds its scene directly and calls it too. `include` (optional): a world rect
// {x0, y0, x1, y1} the bounds must contain as well (what the caller draws around the nodes). `junctionNetsFrom`
// (T5.x-circuit-chain): junctions are worked out only for nets from this one; the scene's junctions so far are kept (a
// composition of scenes copies each part's junctions, translated, instead of searching its nets again: the search is
// quadratic in a net's segments, ≈ 2 ms for a whole chain with Operator).
void indexScene(CircuitScene& scene, const float* include = nullptr, uint32_t junctionNetsFrom = 0);

// The "tidy" command (T4.3): `patch` with every node re-placed and every wire re-routed by graph::autoLayout with this
// symbol library's geometry.
graph::AutoLayoutStats tidy(graph::Patch& patch, const graph::AutoLayoutOptions& options = {});

// Wire styles of every wire of `patch` (the local inference described above).
std::vector<WireStyle> wireStyles(const graph::Patch& patch);
// The same inference per flat output port (Patch::flatOutput; other entries are default): what a node's value is (T4.15
// tooltips). `modulation` is never set.
std::vector<WireStyle> outputStyles(const graph::Patch& patch);

// T4.15: places only the nodes with keep[v] (and routes the wires between them) by graph::autoLayout of that subgraph
// with this symbol library's geometry; the other nodes and wires keep their layout.
graph::AutoLayoutStats layoutSubset(graph::Patch& patch, std::span<const uint8_t> keep, const graph::AutoLayoutOptions& options = {});

}  // namespace pq::views::circuit
