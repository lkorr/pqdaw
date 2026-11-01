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
// mapping), or "" when it can: a `bakeable` param of a type without a live path (meta::Type::liveBakeable false: the
// Wavetable's fold / warp / sync until TMM.8) is baked into the module's table and only changes with its value.
std::string metaParamBakedOnly(const Device& d, std::string_view param);
// A new instance of `t`: its version and default data, no values set.
MetaInstance defaultMetaInstance(const meta::Type& t);

// ---- TMM.10: voice continuity in a chain (docs/plans/MM.md "Chain and devices") ----
// A MIDI track's META VOICE RUN: its first active device is a chain meta instrument, and the active chain meta devices
// right after it (audio-effect kind, of a type that continues voices: meta::Type::chainVoiceInput) stay per voice —
// each one's voice input is the previous one's voice signal — until the first other device or the chain end, where
// the lowering sums the voices (lower.h "Meta modules"). Inactive devices are skipped (they do not break the run).
// The devices of `t`'s run after its instrument, in chain order (empty: no run, or a run of the instrument alone).
std::vector<const Device*> metaVoiceRun(const Track& t);
// Whether a device inserted at `index` of `t`'s own chain (kAppend: the end) would continue `t`'s meta voice run.
bool continuesMetaVoiceRun(const Track& t, uint32_t index);

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
