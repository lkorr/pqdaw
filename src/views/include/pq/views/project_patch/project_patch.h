#pragma once

// The project patch (T4.13, SPEC §15 2026-10-24 "VISUAL PROGRAMMING VIEW / INTERPRETATION OF PROJECT FILE"): the
// whole project drawn as one circuit, as data. One model, two views: this is not a second file structure, it is the
// lowered ProjectGraph the engine runs (pq/document/lower.h, pq/graph/project.h) read back onto the document entities it
// came from, plus an automatic layout that mirrors the arrangement.
//
//   band       one per track, in lane order (the main tree depth-first, then the returns, then the master): a strip
//              holding the track's chain left → right. Deactivated tracks keep a band (dim, no nodes: they are not in
//              the graph), so every lane of the arrangement has its band.
//   source     MIDI track: a "MIDI" node playing the lane's clips (the view draws a strip of them inside it), its
//              output wired to the instrument when one is lowered (a "notes" wire, per voice). Group, return, master:
//              a summing node (Σ) whose one input is every Audio wire routed into the track (several wires into one
//              port sum, graph::ProjectWire).
//   device     one block per device of the chain (the call `d<id>` of the track patch, NodeId nodeIdFor(Device, id, 0));
//              deactivated or missing devices stay in place, dim and bypassed (the chain wire passes under them).
//              Pins: its audio in / out, its exports (the jacks), and the params a modulation wire drives.
//   mixer      the compact mixer strip at the end of each chain (volume, pan, mute / solo drawn by the view): input
//              `in`, outputs `out` (the routing) and one send pin per lowered send (to a return).
//   output     the master's "Out" terminal after its mixer.
//   group      a container around its band and its members' bands; its own chain starts right of its members' mixers.
// Edges: notes (source → instrument), chain (left → right through the lowered devices into the mixer), route (a
// track's `out` → the summing input of its group or the master: graph wire out → in), send (a mixer's send pin → a
// return's summing input: graph wire post / pre → s<id>; a send at −∞ is in the graph too and drawn dim), modulation
// (a device export → a device param: T4.7's wires; across tracks also a graph wire m<id> → m<id>).
//
// Layout (world units = logical px at zoom 1, multiples of 4): bands stacked in lane order, a group's members indented
// under it; signal left → right; a group's own chain starts right of its members' mixers; returns start right of every
// main track (their sends arrive by buses), the master right of everything. Then the stored nudges (doc::ViewState::
// patchNudges: by track id for a band, by device id for a block) and the wires' orthogonal routes (buses into each
// summing input: every wire into one input shares its vertical channel, as a summing bus).
//
// Pure and UI-free (UI thread; ~O(tracks + devices + wires)). The scene for the canvas is buildProjectScene
// (project_patch_view.h).

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/document/store.h"
#include "pq/graph/patch.h"
#include "pq/graph/project.h"

namespace pq::views::project_patch {

inline constexpr uint32_t kNone = 0xFFFFFFFFu;

enum class NodeKind : uint8_t { Source, Sum, Device, Mixer, Output };

struct Port {
  std::string name;   // the graph port (`in`, `out`, an export, a param, `notes`, a send's return id as "s<id>")
  std::string label;  // drawn next to the pin ("" = none)
};

struct Node {
  NodeKind kind = NodeKind::Device;
  uint32_t band = kNone;
  doc::DeviceId device;        // Device
  std::string title;           // device display name, "MIDI", "Σ", "Mixer", "Out"
  bool lowered = true;         // Device: its call is in the track patch (active, in the library, of its kind)
  bool active = true;          // Device: its activator
  bool missing = false;        // Device: its patch is not in the library
  bool edited = false;         // Device: a user device (its own patch text, T4.4-fix1); the title says "(edited)"
  bool instrument = false;     // Device: an instrument
  bool expanded = false;       // Device: drawn opened to its atoms (LayoutOptions::expanded)
  graph::NodeId call;          // Device (lowered): the call node's NodeId in the track patch
  std::vector<Port> inputs, outputs;
  float x = 0, y = 0, w = 0, h = 0;  // world rect
  int32_t input(std::string_view name) const noexcept;   // −1: none
  int32_t output(std::string_view name) const noexcept;
};

enum class EdgeKind : uint8_t { Notes, Chain, Route, Send, Modulation };

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
  std::vector<graph::Point2> points;    // route: from pin … to pin (orthogonal)
  uint32_t net = 0;                     // wires into one summing input share a net (junction dots on the bus)
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
  uint32_t source = kNone, mixer = kNone, output = kNone;  // node indices
  std::vector<uint32_t> devices;                           // node indices, chain order
  float y = 0, h = 0;          // world: the band's strip
  float x0 = 0, x1 = 0;        // world: its strip from the indent to the end of its chain
  // Groups: the container around the group's band and its members' bands.
  float cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
};

struct ProjectPatch {
  std::vector<Band> bands;
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // bounds of the bands and nodes
  uint32_t master = kNone;               // band
  uint32_t bandOf(doc::TrackId t) const noexcept;
  uint32_t nodeOf(doc::DeviceId d) const noexcept;
};

// Layout metrics (world units).
namespace metrics {
inline constexpr float kLeft = 16.0f;       // left edge of a top-level band
inline constexpr float kIndent = 24.0f;     // per group level
inline constexpr float kTitle = 20.0f;      // band title strip (name), above the nodes
inline constexpr float kBandPad = 12.0f;    // under the nodes
inline constexpr float kBandGap = 12.0f;    // between bands
inline constexpr float kNodeGap = 40.0f;    // between chain nodes
inline constexpr float kBusGap = 56.0f;     // before a summing node (room for its bus)
inline constexpr float kPinTop = 24.0f;     // the main pin's offset from a node's top (the band's spine)
inline constexpr float kPinPitch = 12.0f;   // further pins
inline constexpr float kSourceW = 128.0f, kSourceH = 48.0f;  // MIDI node (the clip strip inside)
inline constexpr float kSumW = 40.0f, kSumH = 32.0f;
inline constexpr float kMixerW = 88.0f, kMixerH = 60.0f;     // compact (M / S, pan, volume, values); expanded: wider
inline constexpr float kExpandedPad = 16.0f;                // around an expanded device's atoms
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
};

// The project patch of `project` as lowered into `lowered` (doc::lower with the same project; its library for device
// ports), described with `catalog` (display names; may be null). Never fails: what is not lowered is drawn dim.
ProjectPatch buildProjectPatch(const doc::Project& project, const doc::LoweredProject& lowered,
                               const doc::DeviceCatalog* catalog, const LayoutOptions& options = {});

// Checks a patch against the graph it was built from (tests, debug builds): every lowered track has exactly one band
// with its role; every device call `d<id>` of a track patch is a lowered Device node of that band (and every lowered
// Device node is a call); every graph wire is exactly one edge of the matching kind between the matching bands' nodes
// (out → in: route into the destination's Σ; post / pre → s<id>: a send of the source's mixer into the return's Σ;
// m<id>: the modulation edge of wire id), and every other edge is internal (notes, chain, same-track modulation into a
// param pin the track patch drives); groups contain their members. Returns "" when they match, else the first
// difference.
std::string checkAgainstGraph(const ProjectPatch& patch, const doc::Project& project, const doc::LoweredProject& lowered);

}  // namespace pq::views::project_patch
