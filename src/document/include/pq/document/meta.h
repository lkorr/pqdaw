#pragma once

// Meta modules in the document (TMM.3; docs/plans/MM.md "Architecture"; model.h "Meta modules" holds the state,
// pq/meta/type.h the types). Helpers shared by the lowering (lower.h "Meta modules"), the device library, the edit
// builders and the views:
//   sites      every `name = meta_<type>(…)` call reachable from a device's root patch, by name path
//   keys       the runtime param of a nested instance: key "dev<id>:<path>.<name>", flattened name in the track patch
//              "d<id>__<path, '.' → '__'>__<name>" (the call path the compiler flattens), ParamRef::metaParam
//   renames    which meta-call paths a text edit renamed (NodeIds carried by ParseOptions::previous), so the state key
//              follows the name
// Not RT code.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/model.h"
#include "pq/graph/patch.h"
#include "pq/graph/patch_file.h"
#include "pq/meta/type.h"

namespace pq::doc {

// One meta call in a device's circuit.
struct MetaSite {
  std::string path;                 // "acc1", "opA.acc" (pq/meta/registry.h validPath)
  const meta::Type* type = nullptr;
  std::vector<uint8_t> wired;       // per param of the type: its pin is connected at the call (a wire, not static)
  friend bool operator==(const MetaSite&, const MetaSite&) = default;
};
// Callee name → patch (the library a device's patches call into); null: unknown (not descended into).
using PatchLookup = std::function<const graph::Patch*(std::string_view)>;
// Every NAMED meta call reachable from `root` through named subcircuit calls, depth-first in node order (an unnamed
// call has no path: it plays its default state, and nothing below an unnamed helper call is listed). Bounded by
// graph::kMaxSubcircuitDepth; a cycle is not followed.
std::vector<MetaSite> metaSites(const graph::Patch& root, const PatchLookup& lookup);
// Whether `p` calls a meta module anywhere below it (cheap when nothing does: one pass over its subcircuit nodes per
// patch reached).
bool callsMeta(const graph::Patch& p, const PatchLookup& lookup);

std::string metaParamKey(DeviceId device, std::string_view path, std::string_view param);       // "dev<id>:<path>.<name>"
std::string metaParamFlatName(DeviceId device, std::string_view path, std::string_view param);  // "d<id>__<p>__<name>"
// The value param `i` of type `t` plays at `path` of `d`: the instance's (a chain device's own param for path ""), else
// the type's default.
double metaParamValue(const Device& d, std::string_view path, const meta::Type& t, size_t i);
// The resolved settings of the instance at `path` (spec order; path "": the device's own settings), defaults filled in.
std::vector<double> metaSettings(const Device& d, std::string_view path, const meta::Type& t);
// TMM.5: why chain meta device `d`'s param `param` cannot be modulated (an automation lane, a modulation wire, a macro
// mapping), or "" when it can: a `bakeable` param of a type without a live path (meta::Type::liveBakeable false) is
// baked into the module's table and only changes with its value; TMM.8: a type with a live path may still refuse with
// some settings (meta::Type::bakedOnly: the Wavetable's Shape mode bakes fold / warp / sync).
std::string metaParamBakedOnly(const Device& d, std::string_view param);
// A new instance of `t`: its version and default data, no values set.
MetaInstance defaultMetaInstance(const meta::Type& t);

// ---- TMM.10-fix1: a chain is wires (SPEC §15 2026-11-01 "notes are a signal"; lower.h "Meta modules") ----
// Each device's chain input takes the previous active stage's output; the first stage of a MIDI track's own chain (and
// of an Instrument Rack chain) takes the NOTE SOURCE. What a meta module does follows from what reaches it, never from
// its type: there are no instrument-only or effect-only types. Polyphony follows the wires: a meta module fed by the
// note source or by a per-voice signal runs per voice; a per-voice signal reaching any other device is summed there
// (a stock device's audio input is global), and a stock instrument's voices are summed at its output (Live's
// instrument: QUESTIONS Q?-TMM.10-fix1-a item 2).
enum class ChainSignal : uint8_t {
  None,    // nothing: an unplugged chain input (Device::chainInput "-"), a type without one
  Notes,   // the note source (its signal of the input's meaning: meta::Reads)
  Voices,  // a per-voice signal
  Audio,   // global audio
};
struct ChainStage {
  const Device* device = nullptr;
  ChainSignal in = ChainSignal::None;    // what reaches its chain input (a meta device: None when unplugged)
  ChainSignal out = ChainSignal::Audio;  // what it gives the next stage
  int32_t input = -1;                    // a meta device: the input its chain feeds (−1: none); else −1
};
// The ACTIVE devices of `list`, in order, with what flows between them. `notes`: the list starts at the note source (a
// MIDI track's own chain, an Instrument Rack chain); else at audio. A meta device (of a known type): Notes in → Voices
// out (meta::Form::ChainNotes), Voices → Voices (ChainVoice), Audio → Audio (ChainAudio), unplugged → Audio
// (ChainAudio with nothing in: it runs once, freely). `voices` false (a rack chain, whose devices after its first the
// lowering keeps global) delivers Audio where Voices would arrive. Any other device gives Audio.
std::vector<ChainStage> chainStages(const std::vector<DevicePtr>& list, bool notes, bool voices = true);
// The meta form of a stage (Nested for a device that is not a chain meta device or has no known type).
meta::Form chainForm(const ChainStage& s);
// The input `d`'s chain feeds: Device::chainInput ("" the type's meta::Type::chainInput, "-" none, else that input by
// name); −1: none (or not an input of the type).
int32_t metaChainInput(const Device& d, const meta::Type& t);
// What would reach the chain input of a device inserted at `index` of `list` (kAppend: the end).
ChainSignal chainSignalAt(const std::vector<DevicePtr>& list, bool notes, uint32_t index);
// TMM.10-fix1: Device::chainInput's value for an unplugged chain.
inline constexpr std::string_view kChainUnplugged = "-";

// The renames between two parses of a device text, `after` parsed with ParseOptions::previous = `before`: every NAMED
// subcircuit call reachable from patch `root` (through calls into patches of the same file) whose NodeId path is the
// same in both but whose name path differs, outermost first (a rename of `opA` covers everything below it). The edit
// builders turn them into RenameMetaInstance commands for the paths that hold state.
struct MetaRename {
  std::string from, to;
  friend bool operator==(const MetaRename&, const MetaRename&) = default;
};
std::vector<MetaRename> metaRenames(const graph::PatchFile& before, const graph::PatchFile& after, std::string_view root);

}  // namespace pq::doc
