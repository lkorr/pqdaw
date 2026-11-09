#pragma once

// The project patch (T4.13, SPEC §15 2026-10-24 "VISUAL PROGRAMMING VIEW / INTERPRETATION OF PROJECT FILE"): the
// whole project drawn as one circuit, as data. One model, two views: this is not a second file structure, it is the
// lowered ProjectGraph the engine runs (pq/document/lower.h, pq/graph/project.h) read back onto the document entities it
// came from, plus an automatic layout that mirrors the arrangement.
//
//   band       one per track, in lane order (the main tree depth-first, then the returns, then the master): a strip
//              holding the track's chain left → right. Deactivated tracks keep a band (dim, no nodes: they are not in
//              the graph), so every lane of the arrangement has its band.
//   lane       (TL.1, SPEC §15 2026-11-08 "track lanes in the circuit view") every band ENDS IN ITS TRACK'S LANE: a strip
//              across the bottom of the band (Band::laneY / laneH), every band's over the same x range (ProjectPatch::
//              laneX0 … laneX1, so one ruler labels them all), on which the view draws the track's timeline as the
//              arrangement does (clips, notes, waveforms; group / return / master lanes have no clip row). A MIDI or audio
//              track's source is that lane — a model node of kind Lane whose rect IS the strip (hidden in the scene: no
//              block, no pins) with the outputs the track's signal leaves by: a MIDI track's `notes` (wired to the
//              instrument, per voice) and its note outputs (doc::kNoteOutputs: Hz, gate, velocity), an audio track's
//              `out` (its clip player). Each cable from it leaves the lane's TOP EDGE at its own x (doc::Wire::tap,
//              band-local; none: under the block it feeds: Edge::tapX). Group, return, master: a summing node (Σ) whose
//              one input is every Audio wire routed into the track (several wires into one port sum,
//              graph::ProjectWire), on the spine as before.
//   device     one block per device of the track (the call `d<id>` of the track patch, NodeId nodeIdFor(Device, id, 0));
//              deactivated or missing devices stay in place, dim and bypassed. TX.all-free: every block is free; until
//              placed (a nudge), the blocks of the main path sit on the band's spine in signal order and the others on a
//              row under it.
//              Pins: its audio in / out, its exports (the jacks), and the params a modulation wire drives (TX.mod-all:
//              on a face or panel block a wired param whose control is visible has no pin: its wire ends on the control,
//              Port::anchored).
//   mixer      the track's mixer (drawn by the view as its face: header with M / S, pan knob, volume fader with the
//              level meter, one row per send): input `in`, outputs `out` (the routing) and one send pin per lowered
//              send (to a return), each level with its row (pinOffset). TX.mixer-look: the track's output, so the
//              band's TOP-RIGHT block — right of every other block of the band (its path, the row under it, wherever
//              their nudges put them), at the band's top — unless the user placed it (doc::PatchNudge::mixerPlaced:
//              from the band's top-left corner, so it travels with the band and no other block's move shifts it).
//   output     the master's "Out" terminal after its mixer.
//   group      a container around its band and its members' bands; its own chain starts right of its members' mixers.
// Edges: TX.all-free — every document wire is one edge (notes, chain, modulation: EdgeKind), the main path's links
// included; route (a track's `out` → the summing input of its group or the master: graph wire out → in), send (a
// mixer's send pin → a return's summing input: graph wire post / pre → s<id>; a send at −∞ is in the graph too and drawn
// dim); across tracks a wire is also a graph wire m<id> → m<id>.
//
// Layout (world units = logical px at zoom 1, multiples of 4): bands stacked in lane order, a group's members indented
// under it, each band's lane at its bottom (its height: doc::PatchNudge::laneH, else kLaneH; a FOLDED band —
// PatchNudge::folded — is its title strip and its lane, its blocks and their wires hidden: Node::folded); signal left → right; a group's own chain starts right of its members' mixers; returns start right of every
// main track (their sends arrive by buses), the master right of everything. Then the stored nudges (doc::ViewState::
// patchNudges: by track id for a band, by device id for a block), the mixers (TX.mixer-look: placed, or pushed right of
// any block nudged past them) and the wires' orthogonal routes (buses into each
// summing input: every wire into one input shares its vertical channel, as a summing bus).
// TX.resize: a scaled block (the nudge's scale) takes its scaled size in the layout — the blocks after it move right,
// its band's spine moves down to its main pin and the band grows to hold it, the routes reach its scaled pins and
// anchors — so neighbours make room (positions after a scaled block are not multiples of 4).
//
// Pure and UI-free (UI thread; ~O(tracks + devices + wires)). The scene for the canvas is buildProjectScene
// (project_patch_view.h).

#include <cstdint>
#include <memory>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/document/store.h"
#include "pq/graph/patch.h"
#include "pq/graph/project.h"
#include "pq/meta/type.h"
#include "pq/views/circuit/orbit.h"
#include "pq/views/circuit/symbols.h"

namespace pq::views::project_patch {

inline constexpr uint32_t kNone = 0xFFFFFFFFu;

// TX.groups: Terminal = a group's In (its output kGroupInPort) or Out (its input kGroupOutPort) terminal inside an opened
// group's capsule (`device` = the group's id).
enum class NodeKind : uint8_t { Lane, Sum, Device, Mixer, Output, Terminal };

struct Port {
  std::string name;   // the graph port (`in`, `out`, an export, a param, `notes`, a note output, "s<id>" for a send)
  std::string label;  // drawn next to the pin ("" = none)
  bool param = false;  // Device input: a param a modulation wire drives (a wire dropped on it targets that param)
  // TX.mod-all (the user: "the cables actually touch on / go directly to the location of the knob or slider they are
  // modulating itself instead of added made up input ports"): a param of a face or panel block whose control is
  // visible has NO pin: its wires end on the control. (ax, ay) is the control's centre (node-local world units) when the
  // layout was made (LayoutOptions::controlAnchor); TX.cables: its cable hangs onto it, and the canvas moves the end to
  // wherever the control is when it paints (Face::controlAnchor). Its invisible scene pin sits at inputEntry (the
  // block's edge nearest the control). Such ports take no pin row.
  bool anchored = false;
  float ax = 0, ay = 0;
  // TMM.13-fix2 (MM review H6): how far left of ax a top / bottom entry runs down (the control's clear column: beside
  // its labels, not through them); the canvas then enters the control from its left.
  float aclear = 0;
  // TX.cable-look: the face's key of the control when it is not the port's own name (a closed group's main input landing
  // on the control its signal is added to inside: "value@<id>"; the canvas follows that control).
  std::string anchorKey;
};

struct Node {
  NodeKind kind = NodeKind::Device;
  uint32_t band = kNone;
  doc::DeviceId device;        // Device
  std::string title;           // device display name, "Notes" / "Audio" (a lane), "Σ", "Mixer", "Out"
  bool lowered = true;         // Device: its call is in the track patch (active, in the library, of its kind)
  bool active = true;          // Device: its activator
  bool missing = false;        // Device: its patch is not in the library
  bool instrument = false;     // Device: an instrument
  bool expanded = false;       // Device: drawn opened to its atoms (LayoutOptions::expanded)
  bool folded = false;         // TL.1: on a folded band (hidden: not drawn, not hit, its wires neither)
  // TX.all-free: off its track's main path (doc::mainPath): placed on the row under the path until a nudge moves it, and
  // listed in the Device view's free rack.
  bool offPath = false;
  // GU.9 (pq/document/racks.h): part of a rack — one of its chains, a macro, its selector (not in the free rack).
  bool rack = false;
  // TX.all-free: the input a wire into its main port (doc::kMainPort) ends on (−1: none, an unplugged meta module).
  int32_t mainIn = 0;
  // TMM.4: a chain meta device ("meta:<type>") whose type has a face: drawn as its face (the block's size), unless
  // opened to its atoms; `meta` is set for every chain meta device (its atoms are generated: opened, read-only).
  const meta::Type* face = nullptr;
  bool meta = false;
  // TX.panel-faces: a closed stock device with a panel (LayoutOptions::panelSize) is drawn as its Device-view panel: the
  // block is the panel's size (taller when its pins need it), its pins on the edges.
  bool panel = false;
  // A closed group's members (any depth): a wire to / from one of them ends on the group's block at a port named
  // "<port>@<device id>" (labelled with the port's name).
  std::vector<doc::DeviceId> inner;
  graph::NodeId call;          // Device (lowered): the call node's NodeId in the track patch
  std::vector<Port> inputs, outputs;
  float x = 0, y = 0, w = 0, h = 0;  // world rect
  // TX.resize: a device block's uniform scale (doc::PatchNudge::scale; 1 for every other node and an opened device): w, h,
  // the pins' rows (pinOffset) and the anchors (Port::ax / ay) are already scaled; its face or panel is drawn at it.
  float scale = 1.0f;
  // TX.groups (doc::Device "Groups"): a group device is a CAPSULE. Opened (`groupOpen`, the default): the block is the
  // capsule's frame around its contents — `members` (free device blocks placed freely inside) and its two terminal nodes
  // `groupIn` / `groupOut` (NodeKind::Terminal) — not a scene node of its own (the canvas draws the frame); its chain
  // pins (`x` in, `y` out) on its edges at `pinTop`, level with the band's spine. Closed: one block (like a rack:
  // `inner` = its members, wires to and from them end on it as "<port>@<device id>") showing a miniature of its contents
  // (`mini`, in the block's unit square). A member's or terminal's `capsule` is its group's node.
  // GU.2: groups nest — a member may be a group (its capsule inside its parent's, or a closed block there); `capsule` is
  // the innermost capsule a node sits in. `instrument` on a group: an instrument group (the outermost group fed notes),
  // drawn as an instrument; `voices` its polyphony (1: mono; 0: the link's default).
  bool group = false;
  bool groupOpen = false;
  // GU.3 (pq/document/panel.h): a closed group whose panel has elements is drawn AS ITS PANEL (the view's panel maker:
  // pq/views/panel/group_panel_face.h GroupPanelFace), at the panel's size; no miniature (`mini` empty). An empty panel:
  // the miniature as before.
  bool groupPanel = false;
  // GU.5b: a round panel (its bottom element's outline, panel::panelShape: the VCA's dial, the Phase Accumulator's
  // body): its centre and radius in the panel's own units (× scale on the block); its ports orbit the circle.
  bool panelRound = false;
  float roundX = 0.0f, roundY = 0.0f, roundR = 0.0f;
  // GU.5b (docs/plans/GU.md 5a, "an opened group is a circuit"): an ATOM BLOCK is drawn as its circuit symbol (the
  // circuit editor's symbol set: ×, +, a phasor, a box with its name …; `symbol` its spec at the block's size, pins at
  // its pin offsets, no title pill), a VALUE BLOCK as a small knob (`valueKnob`; with `groupPanel` set its face is a
  // one-knob panel bound to its `value`, which a wire into it ends on).
  std::shared_ptr<const circuit::SymbolSpec> symbol;
  bool valueKnob = false;
  uint32_t voices = 0;
  uint32_t capsule = kNone;
  std::vector<uint32_t> members;
  uint32_t groupIn = kNone, groupOut = kNone;
  float pinTop = 0.0f;  // Device: the main pin's offset from the top (0: metrics::kPinTop)
  // TX.drag-grow: an opened capsule's frame before it grew to hold its members (world; the canvas grows it again live
  // from where a drag has them).
  float frameX0 = 0, frameY0 = 0, frameX1 = 0, frameY1 = 0;
  struct MiniBox {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // in the block's unit square
    bool terminal = false;
    std::string name;  // TMM.13-fix2 (MM review M9): the member's title, drawn in its box where it fits
  };
  std::vector<MiniBox> mini;
  std::vector<graph::Point2> miniWires;  // pairs of points (unit square): a straight line per wire
  // TX.orbit-ports (pq/views/circuit/orbit.h): a closed device block's main input (inputs[mainIn]) and main output
  // (outputs[0]) orbit its outline (circle for a round face, the rounded rect of its face / panel / body otherwise): `at`
  // is the settled port (the angle that makes its cables shortest, or the pinned one: doc::PatchNudge's pins), where
  // its cables start. Off for every other node and port (their pins keep their places).
  struct Orbit {
    bool on = false;
    bool pinned = false;
    float angle = 0.0f;
    graph::Point2 at{0.0f, 0.0f};
  };
  Orbit orbitIn, orbitOut;
  circuit::Outline outline;  // world units (valid while a port orbits)
  int32_t input(std::string_view name) const noexcept;   // −1: none
  int32_t output(std::string_view name) const noexcept;
};

// TX.all-free: every edge between blocks is a document wire (Edge::wire): Notes — from a MIDI track's notes into a
// block's main input; Chain — an audio path inside a band (main port to main port, from the track's own signal, into
// its mixer: the main path's links among them); Modulation — any other wire. (Chain without a wire: the master's mixer
// into Out.)
enum class EdgeKind : uint8_t { Notes, Chain, Route, Send, Modulation };
// TX.cables (SPEC §15 2026-11-02 "cables and the node menu"): every connection between blocks — notes, chain, modulation
// and free wires, a group's terminals — is a hanging cable straight from its source port to its target port (a param
// whose control shows: onto the control); the bus structure stays orthogonal traces: route / send into a Σ and a
// mixer's output (the master's mixer into Out).
inline bool isCable(EdgeKind k, NodeKind from) noexcept {
  return k != EdgeKind::Route && k != EdgeKind::Send && from != NodeKind::Mixer;
}

struct Edge {
  EdgeKind kind = EdgeKind::Chain;
  uint32_t from = kNone, fromPort = 0;  // node, index into its outputs
  uint32_t to = kNone, toPort = 0;      // node, index into its inputs
  uint32_t graphWire = kNone;           // the ProjectGraph wire it is (route, send, cross-track modulation)
  doc::WireId wire;                     // Modulation: the document wire
  doc::WireStatus status = doc::WireStatus::Lowered;  // Modulation (Inactive / Refused ones are drawn dim)
  bool silent = false;                  // Send: its level is −∞ (lowered, plays nothing)
  bool delayed = false;                 // an automatic 1-sample delay (a delayed send or wire)
  bool pre = false;                     // Send: pre-fader
  bool voice = false;                   // Notes / a voice source: per voice
  // TX.groups: a Modulation edge into an INPUT (not a param: an audio input, a group's Out): a signal path, drawn solid
  // like the chain (a grouped series reads as the audio it is).
  bool input = false;
  bool cable = false;  // TX.cables: isCable (a hanging cable between its two points), else an orthogonal trace
  // A trace (route / send): from pin … to pin (orthogonal). TX.cables: a cable has its two ends — the source pin and the
  // target pin, or an anchored param's control centre (n.x + ax, n.y + ay) — and hangs between them (cables.h).
  std::vector<graph::Point2> points;
  uint32_t net = 0;                     // wires into one summing input share a net (junction dots on the bus)
  // TL.1: from a lane (NodeKind::Lane): where the cable leaves its top edge (world x; doc::Wire::tap, else under the
  // block it feeds), and whether the wire pins it (Wire::tap set).
  float tapX = 0.0f;
  bool tapPinned = false;
};

struct Band {
  doc::TrackId track;
  doc::TrackKind kind = doc::TrackKind::Midi;
  std::string name;
  uint32_t colour = 0x808080;   // 0xRRGGBB
  uint16_t depth = 0;           // group nesting (0 = top level)
  int32_t parent = -1;          // band index of the containing group
  uint32_t lastMember = kNone;  // group: band index of its last descendant (kNone: no members)
  bool active = true;           // the track's activator state (Track::active): lowered
  bool muted = false;
  uint32_t graphTrack = kNone;  // index into the LoweredProject's graph.tracks (kNone: not lowered)
  graph::TrackRole role = graph::TrackRole::Audio;
  uint32_t source = kNone, mixer = kNone, output = kNone;  // node indices (source: the Lane, or the Σ)
  std::vector<uint32_t> devices;                           // node indices (its blocks, list order)
  float y = 0, h = 0;          // world: the band's strip
  float x0 = 0, x1 = 0;        // world: its strip from the indent to the end of its chain
  // TX.drag-grow: its height before it grew down to hold its blocks and its right end before it reached them (the
  // canvas grows it again live from where a drag has them).
  float baseH = 0, baseX1 = 0;
  // TX.drag-grow: its mixer's x before it was placed right of the band's blocks (mixerFollows: not placed by the user).
  float mixerX0 = 0;
  bool mixerFollows = false;
  // Groups: the container around the group's band and its members' bands.
  float cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
  // TL.1: its lane (world: ProjectPatch::laneX0 … laneX1 × laneY … laneY + laneH), at the bottom of its strip; a folded
  // band (its circuit collapsed) keeps its title strip and its lane.
  float laneY = 0, laneH = 0;
  bool folded = false;
};

struct ProjectPatch {
  std::vector<Band> bands;
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // bounds of the bands and nodes
  float laneX0 = 0, laneX1 = 0;          // TL.1: every band's lane spans this x range (one ruler)
  uint32_t master = kNone;               // band
  // TX.groups: the model holds only one group's contents (LayoutOptions::scope: the Device view's group panel): one band
  // (its track) without source, mixer or strip, the capsule and what is inside it. TX.free-rack: or one track's free
  // devices (LayoutOptions::freeTrack: the Device view's free-device rack).
  bool scoped = false;
  uint32_t bandOf(doc::TrackId t) const noexcept;
  uint32_t nodeOf(doc::DeviceId d) const noexcept;  // a device inside a closed group: the group's block
};

// Layout metrics (world units).
namespace metrics {
inline constexpr float kLeft = 16.0f;       // left edge of a top-level band
inline constexpr float kIndent = 24.0f;     // per group level
inline constexpr float kTitle = 20.0f;      // band title strip (name), above the nodes
inline constexpr float kBandPad = 12.0f;    // under the nodes
inline constexpr float kBandGap = 12.0f;    // between bands
inline constexpr float kNodeGap = 40.0f;    // between chain nodes
inline constexpr float kFreeRowGap = 20.0f;  // TX.patching: between the chain and the free devices' row under it
inline constexpr float kBusGap = 56.0f;     // before a summing node (room for its bus)
inline constexpr float kPinTop = 24.0f;     // the main pin's offset from a node's top (the band's spine)
inline constexpr float kPinPitch = 12.0f;   // further pins
// TL.1: a band's lane — its default height, the gap above it (from the circuit) and under it (the band's bottom edge),
// and how far in from the deepest band's indent the lanes start.
inline constexpr float kLaneH = 56.0f;
inline constexpr float kLaneGapAbove = 8.0f, kLaneGapBelow = 4.0f;
inline constexpr float kLaneInset = 8.0f;
inline constexpr float kSumW = 40.0f, kSumH = 32.0f;
// TX.mixer-look: the mixer's face (MixerFace, src/project_patch/mixer_face.cpp): its main section (header with M / S,
// the pan knob, the volume fader and readouts) kMixerH tall, then one row per send from kMixerSendTop (each send's pin
// on the right edge level with its row); expanded (a double-click), wider: longer send faders and their levels.
inline constexpr float kMixerW = 120.0f, kMixerWideW = 168.0f, kMixerH = 72.0f;
inline constexpr float kMixerSendTop = 82.0f;  // the first send row's centre (and its pin)
inline constexpr float kExpandedPad = 16.0f;                // around an expanded device's atoms
inline constexpr float kCapsuleTitle = 20.0f;               // TX.groups: an opened group's title strip
inline constexpr float kCapsulePad = 12.0f;                 // around its contents
inline constexpr float kTerminalW = 36.0f, kTerminalH = 32.0f;  // its In / Out terminals
inline constexpr float kClosedGroupW = 168.0f, kClosedGroupH = 96.0f;  // a closed group's block (its miniature)
inline constexpr float kValueKnobW = 48.0f, kValueKnobH = 56.0f;      // GU.5b: a value block's knob
}  // namespace metrics

// An expanded device (opened to its atoms in place): the size of what is drawn inside its frame, from the view.
struct ExpandedSize {
  doc::DeviceId device;
  float w = 0, h = 0;  // inner bounds (world units); the frame adds kExpandedPad around and the title strip on top
};

struct LayoutOptions {
  std::span<const ExpandedSize> expanded;
  std::span<const doc::TrackId> expandedMixers;  // mixer strips shown expanded (one row per send)
  const std::unordered_map<uint32_t, doc::PatchNudge>* nudges = nullptr;  // ViewState::patchNudges
  // TX.panel-faces: the world size of a device's panel (a stock device that is not edited, not a rack, not a chain meta
  // device: those keep their block / face), {0, 0} for none. Unset: every device is a plain block.
  std::function<graph::Point2(const doc::Device&)> panelSize;
  // TX.mod-all: the centre (node-local world units) of the control that shows `param` on a device's face or panel, or
  // nullopt when none shows it (the param keeps a pin). Asked only for face and panel blocks' wired params. Unset: every
  // wired param has a pin.
  // (x, y) the control's centre, z how far left of it its clear column is (Port::aclear).
  struct Anchor {
    float x = 0, y = 0, clear = 0;
  };
  std::function<std::optional<Anchor>(const doc::Device&, std::string_view param)> controlAnchor;
  // TX.groups: the groups drawn closed (a block with a miniature); every other group is opened (its capsule).
  std::span<const doc::DeviceId> closedGroups;
  // TX.groups: only this group (opened) and its contents (ProjectPatch::scoped; the Device view's group panel).
  doc::DeviceId scope;
  // TX.free-rack: only this track's blocks off its main path (TX.all-free) that are not group members (a group off the
  // path with its contents), and the wires among them (ProjectPatch::scoped; the Device view's free rack). Ignored with
  // `scope`.
  doc::TrackId freeTrack;
};

// TX.mod-all: which edge of its block an anchored param's wire enters by (the one nearest the control; never the
// right edge, which holds the outputs), and where (world units).
enum class EntrySide : uint8_t { Left, Top, Bottom };
// TMM.13-fix2 (MM review H6): the left edge only for a control in the face's first column (its clear column reaches the
// edge); any other is entered down (or up) the gap beside it, never across the controls left of it.
inline EntrySide entrySide(const Node& n, const Port& p) noexcept {
  const float dl = p.ax, dt = p.ay, db = n.h - p.ay;
  if (p.ax - p.aclear <= 2.0f || (p.aclear == 0.0f && dl <= dt && dl <= db)) return EntrySide::Left;
  return dt <= db ? EntrySide::Top : EntrySide::Bottom;
}

// A world rect (x0, y0) … (x1, y1).
struct LaneRect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
// TL.1: band `b`'s lane (world units).
inline LaneRect laneBody(const ProjectPatch& pp, const Band& b) noexcept { return {pp.laneX0, b.laneY, pp.laneX1, b.laneY + b.laneH}; }
// Whether a band's lane has a clip row (a MIDI or an audio track; group / return / master lanes hold automation only).
inline bool hasClipRow(const Band& b) noexcept { return b.kind == doc::TrackKind::Midi || b.kind == doc::TrackKind::Audio; }
// The space a band's lane takes at its bottom (with its gaps).
inline float laneBlock(float laneH) noexcept { return laneH > 0.0f ? metrics::kLaneGapAbove + laneH + metrics::kLaneGapBelow : 0.0f; }
// TL.1: a MIDI lane's PORT CHIPS over the left end of its top edge, one per output of its lane node in order (its notes,
// then doc::kNoteOutputs: Hz, gate, velocity): a cable drawn up from one is of that output. World units.
inline constexpr std::string_view kLanePortLabels[] = {"notes", "Hz", "gate", "vel"};
inline constexpr float kLaneChipH = 12.0f, kLaneChipGap = 2.0f, kLaneChipLeft = 4.0f;
inline constexpr float lanePortChipW(size_t port) noexcept { return 7.0f * static_cast<float>(kLanePortLabels[port].size()) + 10.0f; }
// Chip `port` of band `b`'s lane (empty: not a MIDI lane, no such output, the lane too narrow for it).
inline LaneRect lanePortChip(const ProjectPatch& pp, const Band& b, size_t port) noexcept {
  if (b.kind != doc::TrackKind::Midi || b.laneH <= 0.0f || port >= std::size(kLanePortLabels)) return {};
  float x = pp.laneX0 + kLaneChipLeft;
  for (size_t i = 0; i < port; ++i) x += lanePortChipW(i) + kLaneChipGap;
  if (x + lanePortChipW(port) > pp.laneX1) return {};
  return {x, b.laneY - kLaneChipH - 1.0f, x + lanePortChipW(port), b.laneY - 1.0f};
}

// The y offset of pin `i` (an input, or an output) from its node's top: the main pin (0) at the band's spine, the rest
// a row below each; a Σ's and Out's at their middle. TL.1: a lane's (hidden) pins on its top edge.
inline float pinOffset(const Node& n, size_t i) noexcept {
  if (n.symbol && i < n.symbol->inputs.size()) return n.symbol->inputs[i].y;  // GU.5b (inputs: pinOffset(n, i, false) for outputs)
  switch (n.kind) {
    case NodeKind::Sum: return 16.0f;
    case NodeKind::Output: return 8.0f;
    case NodeKind::Lane: return 0.0f;
    case NodeKind::Device:  // TX.resize; TX.groups: an opened group's chain pins at its inner spine
      return ((n.pinTop > 0.0f ? n.pinTop : metrics::kPinTop) + metrics::kPinPitch * static_cast<float>(i)) * n.scale;
    case NodeKind::Terminal: return metrics::kTerminalH * 0.5f;
    case NodeKind::Mixer:  // TX.mixer-look: in / out under the header; a send's pin level with its row
      return i == 0 ? metrics::kPinTop : metrics::kMixerSendTop + metrics::kPinPitch * static_cast<float>(i - 1);
    default: return metrics::kPinTop + metrics::kPinPitch * static_cast<float>(i);
  }
}

// GU.5b: input `i` (input) or output `i` of `n`: an atom block's symbol puts its inputs and outputs at their own rows.
inline float pinOffset(const Node& n, size_t i, bool input) noexcept {
  if (n.symbol) {
    const std::vector<circuit::PinSpec>& pins = input ? n.symbol->inputs : n.symbol->outputs;
    if (i < pins.size()) return pins[i].y;
  }
  if (n.symbol && !input) return n.h * 0.5f;
  return pinOffset(n, i);
}

// Where input `i`'s wires meet node `n` (world units): its pin on the left edge, or (TX.mod-all) for an anchored param
// the point of the edge nearest its control, level with it (the canvas draws on from there to the control).
inline graph::Point2 inputEntry(const Node& n, size_t i) noexcept {
  if (i < n.inputs.size() && n.inputs[i].anchored) {
    const Port& p = n.inputs[i];
    switch (entrySide(n, p)) {
      case EntrySide::Left: return {n.x, n.y + p.ay};
      case EntrySide::Top: return {n.x + std::max(2.0f, p.ax - p.aclear), n.y};
      case EntrySide::Bottom: return {n.x + std::max(2.0f, p.ax - p.aclear), n.y + n.h};
    }
  }
  return {n.x, n.y + pinOffset(n, i)};
}

// TX.orbit-ports: where output `i` / input `i` of `n` meets its wires — an orbiting main port's settled place (Node::
// Orbit), else its pin on the right edge / inputEntry.
inline graph::Point2 outPortPoint(const Node& n, size_t i) noexcept {
  if (i == 0 && n.orbitOut.on) return n.orbitOut.at;
  if (n.kind == NodeKind::Lane) return {n.x, n.y};  // TL.1: each cable at its own tap (Edge::tapX)
  return {n.x + n.w, n.y + pinOffset(n, i, false)};
}
inline graph::Point2 inPortPoint(const Node& n, size_t i) noexcept {
  if (n.orbitIn.on && n.mainIn >= 0 && i == static_cast<size_t>(n.mainIn)) return n.orbitIn.at;
  return inputEntry(n, i);
}
inline bool portOrbits(const Node& n, size_t i, bool input) noexcept {
  return input ? n.orbitIn.on && n.mainIn >= 0 && i == static_cast<size_t>(n.mainIn) : n.orbitOut.on && i == 0;
}

// The project patch of `project` as lowered into `lowered` (doc::lower with the same project; its library for device
// ports), described with `devices` (the blocks' names; may be null). Never fails: what is not lowered is drawn dim.
ProjectPatch buildProjectPatch(const doc::Project& project, const doc::LoweredProject& lowered,
                               const doc::DeviceLibrary* devices, const LayoutOptions& options = {});

// Checks a patch against the graph it was built from (tests, debug builds): every lowered track has exactly one band
// with its role; every device call `d<id>` of a track patch is a lowered Device node of that band (and every lowered
// Device node is a call); every graph wire is exactly one edge of the matching kind between the matching bands' nodes
// (out → in: route into the destination's Σ; post / pre → s<id>: a send of the source's mixer into the return's Σ;
// m<id>: the modulation edge of wire id), and every other edge is internal (notes, chain, same-track modulation into a
// param pin the track patch drives); groups contain their members. Returns "" when they match, else the first
// difference.
std::string checkAgainstGraph(const ProjectPatch& patch, const doc::Project& project, const doc::LoweredProject& lowered);

}  // namespace pq::views::project_patch
