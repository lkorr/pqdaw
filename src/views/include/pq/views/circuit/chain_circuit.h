#pragma once

// A track's whole device chain as one circuit (T5.x-circuit-chain; the "Track" level of the circuit view, SPEC §7.3):
// every device's own circuit (its patch's scene, circuit::buildScene) laid out left → right in chain order, each inside a
// labelled BOUNDARY, with real wires between them:
//
//   terminals   track mode: the track's input on the left (MIDI: a "Notes" source, poly notes into the instrument; an
//               audio track: "Audio In"; a group / return / master: "Σ In") and its mixer on the right ("Mixer"). Rack
//               mode (a rack's circuit on its own, drawn inside its block by the project patch view): "in" (an Audio
//               Effect Rack) or "notes" (an Instrument Rack) and "out", named as the block's ports so the block's
//               connectors find them.
//   devices     a box around the device's scene, the title strip on top (name, kind); the audio wire from the previous
//               device's `out` terminal ends on this device's audio `in` terminal (the patch's first non-const `in`),
//               its `out` terminal feeds the next. An instrument has no audio input: the notes wire ends on its boundary.
//               A device missing from the library is an empty box (the chain wire passes it).
//   racks       a box holding its chains stacked top → bottom, each chain a box holding its devices (racks nest, any
//               depth: every walk here is iterative); the rack's input splits on a bus into every chain (an Instrument
//               Rack: the notes), every chain's output joins on a bus into a Σ before the rack's output.
//   modulation  T4.7's wires between two devices of the circuit (export terminal → param terminal; a target param with no
//               terminal of its own ends on the target's boundary), dashed, routed in the gutter above the boxes.
//
// Built once per structure change (a document change that is structural or renames something; never per frame). The
// devices' scenes come from a ChainSceneCache keyed by what each device plays (a user device's resolved root, else its
// stock patch), so a rebuild after an edit of one device rebuilds only that device's scene and the composition
// (≈ a copy of every node, pin and wire: O(scene)). Pure, UI-free, UI thread.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/model.h"
#include "pq/views/circuit/circuit_scene.h"

namespace pq::views::circuit {

inline constexpr uint32_t kNoBox = 0xFFFFFFFFu;

// Layout metrics (world units = logical px at zoom 1, multiples of 4).
namespace chain_metrics {
inline constexpr float kTitle = 24.0f;      // a box's title strip
inline constexpr float kPad = 16.0f;        // around a device's scene inside its box
inline constexpr float kGap = 56.0f;        // between consecutive boxes of a chain (the audio wire's jog)
inline constexpr float kChainPad = 12.0f;   // inside a rack's chain box
inline constexpr float kChainGap = 16.0f;   // between a rack's chains
inline constexpr float kRackSplit = 32.0f;  // a rack's split bus (left of its chains)
inline constexpr float kRackSum = 80.0f;    // its summing bus and Σ (right of its chains)
inline constexpr float kSumW = 40.0f, kSumH = 32.0f;
inline constexpr float kEmptyW = 160.0f, kEmptyH = 72.0f;  // a missing device, an empty rack
inline constexpr float kModLane = 12.0f;    // between modulation wires in the gutter above the boxes
}  // namespace chain_metrics

enum class ChainBoxKind : uint8_t { Device, Rack, Chain };

struct ChainBox {
  ChainBoxKind kind = ChainBoxKind::Device;
  doc::DeviceId device;          // Device, Rack
  doc::ChainId chain;            // Chain
  uint32_t parent = kNoBox;      // the chain holding a device, the rack holding a chain (kNoBox: the top level)
  uint16_t depth = 0;            // rack nesting: 0 = the track's chain (or the rack of a rack circuit)
  std::string title;             // the device's name (Device::name, else the catalog's), the chain's name
  std::string kindLabel;         // "Instrument", "Audio Effect", "Instrument Rack", "Audio Effect Rack", "Chain"
  bool instrument = false;
  bool active = true;            // the device's / chain's activator
  bool missing = false;          // Device: its patch is not in the library (an empty box)
  bool edited = false;           // Device: a user device (its own patch text)
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // world rect: the boundary
  uint32_t firstNode = 0, nodeCount = 0;  // Device: its nodes in the scene; Rack: its Σ (nodeCount 1)
  uint32_t inNode = kNoBox, outNode = kNoBox;  // Device: the scene nodes of its audio in / out terminals (kNoBox: none)
  std::vector<uint32_t> children;  // Rack: its chains, top → bottom; Chain: its devices, left → right
  bool contains(float x, float y) const noexcept { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

// A wire of the circuit that is not inside one device's scene.
struct ChainLink {
  enum class Kind : uint8_t { Audio, Notes, Modulation };
  Kind kind = Kind::Audio;
  uint32_t from = kNoBox, to = kNoBox;  // boxes (kNoBox: a terminal)
  doc::WireId wire;                     // Modulation: the document wire
  uint32_t sceneWire = 0;
};

struct ChainCircuit {
  doc::TrackId track;
  doc::DeviceId rack;            // a rack's circuit (rack mode); invalid in track mode
  std::string trackName;
  uint32_t trackColour = 0x808080;
  std::shared_ptr<CircuitScene> scene;
  std::vector<ChainBox> boxes;   // a parent before its children
  std::vector<uint32_t> roots;   // top-level boxes, in chain order
  std::vector<ChainLink> links;
  std::vector<uint32_t> boxOfNode;  // per scene node: its innermost box (kNoBox: a terminal)
  uint32_t inputNode = kNoBox, outputNode = kNoBox;  // the terminals' scene nodes
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the boxes and terminals

  uint32_t boxOf(doc::DeviceId d) const noexcept;       // Device / Rack box of a device (kNoBox: none)
  uint32_t boxAt(float x, float y) const noexcept;      // the innermost box containing a world point
  uint32_t deviceBoxAt(float x, float y) const noexcept;  // … that is a Device or a Rack (a chain's padding: its rack)
};

// T4.15 / TMM.12: the scene of a device's patch as it plays — what its structural settings switch off hidden and the
// visible rest laid out on its own (unless the patch's stored layout places it), as the Device-level editor shows it,
// so a device opened in the one circuit shows only what plays (Operator's operators that are off, the Meta Operator's
// other algorithms; an `oversample(hq)` region only while hq > 1).
std::shared_ptr<const CircuitScene> buildDeviceScene(const graph::Patch& callee, const std::vector<doc::DeviceValue>& settings);
// The key a device's scene is cached under: what it plays (`what`) and its settings.
std::string deviceSceneKey(const std::string& what, const std::vector<doc::DeviceValue>& settings);

// The scenes of device patches, by what they play. Bounded (oldest dropped first); UI thread.
class ChainSceneCache {
public:
  static constexpr size_t kMax = 48;
  // The scene of `callee` with these settings (buildDeviceScene) stored under `key` (built on first use).
  std::shared_ptr<const CircuitScene> sceneOf(const std::string& key, const graph::Patch& callee,
                                              const std::vector<doc::DeviceValue>& settings = {});
  void clear() noexcept { entries_.clear(); }
  size_t size() const noexcept { return entries_.size(); }
  uint64_t builds() const noexcept { return builds_; }

private:
  std::vector<std::pair<std::string, std::shared_ptr<const CircuitScene>>> entries_;
  uint64_t builds_ = 0;
};

struct ChainOptions {
  const doc::DeviceLibrary* devices = nullptr;  // patches, names (null: every device is an empty box)
  ChainSceneCache* cache = nullptr;             // null: scenes built afresh
};

// The circuit of `track`'s chain (empty, with only its terminals, when the track has no devices; boxes empty when the
// track does not exist).
ChainCircuit buildTrackCircuit(const doc::Project& project, doc::TrackId track, const ChainOptions& options);
// The circuit of one rack (rack mode, see above). Empty when `rack` is not a rack.
ChainCircuit buildRackCircuit(const doc::Project& project, doc::DeviceId rack, const ChainOptions& options);

}  // namespace pq::views::circuit
