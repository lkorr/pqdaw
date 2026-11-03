#pragma once

// The node menu's catalog (TX.node-menu, SPEC §15 2026-11-02 "cables and the node menu"): everything that can be placed
// as a node, with what the menu shows and what a cable can connect.
//
//   atoms         every leaf atom of the patch language (pq/graph/atoms.h; its one-line summary). At the project level
//                 only those that make a free device on their own (no voice, I/O, sample, structure or internal atoms):
//                 such an atom becomes a free device of that one atom (specDevice).
//   meta modules  every registered meta-module type with a face (pq/meta/registry.h); the analysers (Spectrum,
//                 Oscilloscope, Spectrogram) under Visualisers
//   devices       the stock instruments and audio effects (doc::stockDevices, the catalog), the two racks
//   user library  the User Library's saved devices, presets and racks (the browser's index: Library::index())
//
// Each entry carries the facts the menu needs and nothing it does not: its signal ports (inputs apart from params,
// outputs), its params with their defaults, whether it has a MAIN input (its first signal input: what a chain or a
// cable into it feeds) and a main output (its first output). Those two facts decide what is "compatible": a cable
// dragged out of an output offers nodes with an input first, a backwards cable out of a param nodes with an output.
// No per-type rule: the facts come from the signatures (atoms, meta types, device patches).
//
// Search: fuzzy over names and aliases (library::fuzzyScore, the browser's: a term must be a subsequence; word starts
// and runs score), and descriptions by substring (a subsequence would match almost any sentence); every term of the
// query must match; names outrank aliases outrank descriptions.
//
// Cost: the catalog is built once per device library and user-library index snapshot (≈ 300 entries: well under a
// millisecond) and shared; a search is one pass over it with a 64-bit character prefilter. UI thread.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/patch.h"

namespace pq::doc {
class DeviceLibrary;
}
namespace pq::views {
class ViewContext;
}

namespace pq::views::circuit {

// The categories of the catalog (a tab each) in menu order.
enum class NodeCategory : uint8_t { Atoms, MetaModules, Instruments, AudioEffects, MidiEffects, Visualisers, UserLibrary, Count };
// The menu's tabs: All, every category, Recent.
enum class NodeTab : uint8_t { All, Atoms, MetaModules, Instruments, AudioEffects, MidiEffects, Visualisers, UserLibrary, Recent, Count };
std::string_view tabName(NodeTab t) noexcept;
std::string_view categoryName(NodeCategory c) noexcept;
inline NodeTab tabOf(NodeCategory c) noexcept { return static_cast<NodeTab>(static_cast<uint8_t>(c) + 1); }

// What an entry places.
enum class NodeItem : uint8_t {
  Atom,        // id: the atom's name ("tanh"); the circuit editor's spec, a one-atom free device in the project
  Meta,        // id: the type ("vca"): a chain meta device "meta:<id>" / a `meta_<id>` call
  Device,      // id: a stock patch ("utility")
  Rack,        // id: doc::kAudioEffectRack / doc::kInstrumentRack
  LibraryFile, // id: a .pqpreset / .pqdevice path of the User Library
  Spec,        // id: a typed patch-language node spec (not in the catalog: the menu's typed row)
};

struct NodeEntry {
  std::string name;         // shown
  std::string key;          // unique and stable ("atom:tanh", "meta:vca", "device:utility", "file:<path>"): Recent
  NodeItem item = NodeItem::Atom;
  NodeCategory category = NodeCategory::Atoms;
  std::string id;           // see NodeItem
  std::string aliases;      // more words the search matches ("+ plus add")
  std::string description;  // one or two sentences
  std::string chip;         // the kind chip of the preview ("ATOM", "META MODULE", "INSTRUMENT" …)
  std::vector<std::string> inputs, outputs;  // signal ports (params apart)
  struct Param {
    std::string name, value;  // label, default as text ("1 kHz", "0.7")
  };
  std::vector<Param> params;
  bool hasIn = false;   // a main input (what a cable into it, or a chain, feeds)
  bool hasOut = false;  // a main output
  bool instrument = false;  // goes first in a chain (takes the notes), never free
  // Search (folded once).
  std::string foldedName, foldedAliases, foldedDesc;
  uint64_t mask = 0;  // character set of name + aliases + description (a term outside it cannot match)
};

struct NodeCatalog {
  enum class Scope : uint8_t { Project, Circuit };
  Scope scope = Scope::Project;
  std::vector<NodeEntry> entries;  // grouped by category (menu order), by name inside each
  const NodeEntry* find(std::string_view key) const noexcept;
};

// The catalog for `scope`: atoms, meta modules (and, at the project level, the stock devices and racks of `lib`, and
// with a context the User Library's devices and presets: its browser index). Built once per (device library, index
// snapshot) and shared: opening the menu again costs nothing. `lib` may be null (no devices).
std::shared_ptr<const NodeCatalog> nodeCatalog(NodeCatalog::Scope scope, const doc::DeviceLibrary* lib);
std::shared_ptr<const NodeCatalog> nodeCatalog(NodeCatalog::Scope scope, ViewContext& ctx);

// What a menu opened from a cable offers first: nothing in particular, nodes with a main input (a cable out of an
// output), nodes with a main output (a backwards cable out of a param), nodes with both (a node inserted into a cable).
enum class NodeFilter : uint8_t { None, WithInput, WithOutput, WithBoth };
bool compatible(const NodeEntry& e, NodeFilter f) noexcept;

struct NodeHit {
  uint32_t entry = 0;
  int32_t score = 0;
  bool compatible = true;
};
// The entries of `tab` matching `query` (every space-separated term fuzzy in the name or an alias, or a substring of the
// description): compatible ones first, grouped by category (with a query the group holding the best match first, else
// menu order), best score first inside a group, then the shorter name. An empty query lists the tab in catalog order
// (compatible first); Recent lists `recent` (keys, newest first) in that order.
std::vector<NodeHit> searchNodes(const NodeCatalog& c, std::string_view query, NodeTab tab, NodeFilter filter,
                                 std::span<const std::string> recent = {});

// Recent picks of this session (keys, newest first, at most kRecentNodes).
inline constexpr size_t kRecentNodes = 16;
void noteRecentNode(std::string_view key);
std::vector<std::string> recentNodes();
void clearRecentNodes();  // tests

// ---- typed specs (PATCH_LANGUAGE: the circuit editor's "add" field) ---------------------------------------------
// Whether `spec` is one node: a literal (`0.5`, `440Hz`), an atom (`tanh`, `svf` …), an operator (`*`) or an operator
// with a literal operand (`* 2`), a `param` declaration, a meta call (`meta_vca`); in the circuit editor also the
// other declarations (`in x : audio`, `out y`, `export e`) and this file's patches (`callees`).
bool nodeSpecValid(std::string_view spec, NodeCatalog::Scope scope, std::span<const graph::SubcircuitRef> callees = {});
// The circuit editor's form of a spec: `* 2` → the operator (the literal is wired by specOperand).
struct SpecParts {
  std::string node;     // what PatchEditor::addNode / graph::edit::addNode takes
  std::string operand;  // a literal for the operator's second input ("" = none)
};
SpecParts splitSpec(std::string_view spec);

// The project level: a free device made of the one node `spec` describes — its patch text (a user device: model.h
// UserPatch) with one `in` per required input of the node, one `out` per output (a node without a real input gets an
// audio input that drives nothing, so it is still an audio effect; it has no main input), its patch name and the
// device's display name. Declarations other than `param` are refused (they are a patch's ports, not nodes).
struct SpecDevice {
  std::string patch;  // the patch name in `text`
  std::string text;
  std::string name;   // shown on the block
};
Result<SpecDevice> specDevice(std::string_view spec);

// The main ports of a device patch (graph signature `sig` of the patch a device plays, `params` its param names):
// main input = its first input that is not a setting (const), not a param (nor a param's `_add`) and, in `callee` when
// given, drives something; main output = its first output. "" = none.
struct MainPorts {
  std::string in, out;
};
MainPorts mainPortsOf(const graph::SubcircuitRef& sig, const graph::Patch* callee, bool instrument);

}  // namespace pq::views::circuit
