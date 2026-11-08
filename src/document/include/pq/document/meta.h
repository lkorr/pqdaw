#pragma once

// Meta modules in the document (TMM.3; docs/plans/MM.md "Architecture"; model.h "Meta modules" holds the state,
// pq/meta/type.h the types). Helpers shared by the lowering (lower.h "Meta modules"), the device library, the edit
// builders and the views:
//   keys       the runtime param of a nested instance: key "dev<id>:<path>.<name>", ParamRef::metaParam
// Not RT code.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/main_path.h"
#include "pq/document/model.h"
#include "pq/graph/patch.h"
#include "pq/graph/patch_file.h"
#include "pq/meta/type.h"

namespace pq::doc {

std::string metaParamKey(DeviceId device, std::string_view path, std::string_view param);       // "dev<id>:<path>.<name>"
// The value param `i` of type `t` plays at `path` of `d`: GU.1 — its curve (a chain device's own for path "", else the
// type's: meta::paramCurve) at its knob position (metaParamPosition: the instance's, a chain device's own param for
// path "", else the type's default through its declared curve), in the param's units.
double metaParamValue(const Device& d, std::string_view path, const meta::Type& t, size_t i);
double metaParamPosition(const Device& d, std::string_view path, const meta::Type& t, size_t i);
dsp::Curve metaParamCurve(const Device& d, std::string_view path, const meta::Type& t, size_t i);
// The resolved settings of the instance at `path` (spec order; path "": the device's own settings), defaults filled in.
std::vector<double> metaSettings(const Device& d, std::string_view path, const meta::Type& t);
// TMM.5: why chain meta device `d`'s param `param` cannot be modulated (an automation lane, a modulation wire, a macro
// mapping), or "" when it can: a `bakeable` param of a type without a live path (meta::Type::liveBakeable false) is
// baked into the module's table and only changes with its value; TMM.8: a type with a live path may still refuse with
// some settings (meta::Type::bakedOnly; TX.mod-all: no type refuses now).
std::string metaParamBakedOnly(const Device& d, std::string_view param);
// A new instance of `t`: its version and default data, no values set.
MetaInstance defaultMetaInstance(const meta::Type& t);

// ---- TMM.10-fix1: a chain is wires (SPEC §15 2026-11-01 "notes are a signal"; lower.h "Meta modules") ----
// TX.all-free: along a track's MAIN PATH (main_path.h; or a rack chain's list) each device's main input takes the
// previous active stage's output; the first stage of a MIDI track's path (and of an Instrument Rack chain) takes the
// NOTE SOURCE. What a meta module does follows from what reaches it, never from its type: there are no instrument-only
// or effect-only types. Polyphony follows the wires: a meta module fed by the note source or by a per-voice signal runs
// per voice; a per-voice signal reaching any other device is summed there (a stock device's audio input is global), and
// a stock instrument's voices are summed at its output (Live's instrument: QUESTIONS Q?-TMM.10-fix1-a item 2).
enum class ChainSignal : uint8_t {
  None,    // nothing: a type without a chain input
  Notes,   // the note source (its signal of the input's meaning: meta::Reads)
  Voices,  // a per-voice signal
  Audio,   // global audio
};
struct ChainStage {
  const Device* device = nullptr;
  ChainSignal in = ChainSignal::None;    // what reaches its chain input (a meta device: None when unplugged)
  ChainSignal out = ChainSignal::Audio;  // what it gives the next stage
  int32_t input = -1;                    // a meta device: the input its chain feeds (−1: none); else −1
  // TX.all-free: a block off the main path (not in the flow: in None, input −1; the next stage receives what this one
  // would have received). A meta device then plays its free form with nothing in (its inputs read their defaults, or
  // the wires into them).
  bool offPath = false;
};
// The ACTIVE devices of `list`, in order, with what flows between them. `notes`: the list starts at the note source (a
// MIDI track's main path, an Instrument Rack chain); else at audio. A meta device (of a known type): Notes in → Voices
// out (meta::Form::ChainNotes), Voices → Voices (ChainVoice), Audio → Audio (ChainAudio), unplugged → Audio
// (ChainAudio with nothing in: it runs once, freely). `voices` false (a rack chain, whose devices after its first the
// lowering keeps global) delivers Audio where Voices would arrive. TX.groups-fix1: a group (model.h "Groups") passes on
// what reaches it to its series (IG.1-2: an instrument group sums its voices at Out — Audio out). Any other device
// gives Audio. TX.all-free: with `path`, the devices of `list` not on it are listed (ChainStage::offPath) but take no
// part in the flow (a track: pass flowOrder(t, path), the path first).
std::vector<ChainStage> chainStages(const std::vector<DevicePtr>& list, bool notes, bool voices = true,
                                    const MainPath* path = nullptr);
// TX.all-free: the stages of a track's blocks (its main path's flow, then the blocks off it). IG.1-2: then, for each
// group on the path, its members along its series (In → member → … → Out, each starting at what reaches the group: an
// instrument group's first member takes the notes), its other members off it.
std::vector<ChainStage> trackStages(const Project& p, const Track& t);
std::vector<ChainStage> trackStages(const Project& p, const Track& t, const MainPath& path);  // its path known
// The meta form of a stage (Nested for a device that is not a chain meta device or has no known type).
meta::Form chainForm(const ChainStage& s);
// The input `d`'s chain feeds: its type's meta::Type::chainInput (GU.11: a native engine's main input is always its
// type's); −1: none.
int32_t metaChainInput(const Device& d, const meta::Type& t);
// What would reach the main input of a device inserted at `index` of `list` (kAppend: the end; a rack chain, or a
// track's main path as its blocks in signal order).
ChainSignal chainSignalAt(const std::vector<DevicePtr>& list, bool notes, uint32_t index);

}  // namespace pq::doc
