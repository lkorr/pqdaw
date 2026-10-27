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
#include "pq/views/circuit/symbols.h"

namespace pq::views::circuit {

enum class WireRate : uint8_t { Const, Param, Audio };

struct WireStyle {
  bool boolean = false;     // kind bool (else real)
  WireRate rate = WireRate::Audio;
  bool poly = false;        // per voice: doubled line
  bool modulation = false;  // an audio-rate signal into a parameter: dashed
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
};

struct SceneNode {
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;  // world rect (top-left, size)
  uint32_t spec = 0;                              // into CircuitScene::specs
  uint32_t firstPin = 0, pinCount = 0;            // into CircuitScene::pins (inputs, then outputs)
  graph::NodeIndex node = graph::kNone;
  bool inactive = false;  // T4.15: switched off by a structural constant (drawn dimmed)
  bool hidden = false;    // … and hidden (not drawn, not hit)
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
};

// The scene of `patch` as stored, completing a missing or partial layout (see above). `laidOut` (optional) receives the
// patch with the layout that is drawn (for "tidy" and saving it back to text).
std::shared_ptr<CircuitScene> buildScene(const graph::Patch& patch, const SceneOptions& options = {},
                                         graph::Patch* laidOut = nullptr);

// The derived parts of a scene whose nodes, pins, points, wires (with their nets), segments and labels are filled in:
// junction dots (per net, where three or more directions meet), the bounds and the spatial grid. buildScene() ends
// with it; T4.13's project patch view builds its scene directly and calls it too. `include` (optional): a world rect
// {x0, y0, x1, y1} the bounds must contain as well (what the caller draws around the nodes).
void indexScene(CircuitScene& scene, const float* include = nullptr);

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
