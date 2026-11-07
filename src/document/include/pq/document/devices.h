#pragma once

// The block library (T4.1; GU.11, docs/plans/GU.md "One hierarchy").
//
// A block is a group, an atom block, a value block or a native engine (model.h Device). Each plays a FIXED patch: the
// group patches, one patch per atom (built from its signature), the value patch, and a meta type's expansion (the
// meta-module prelude's `meta_<type>` forms). The DEVICE LIBRARY holds them as one graph::PatchLibrary that the lowering
// (lower.h) calls into, and describes each (DeviceInfo: its kind, params with their curves, settings) for the views
// and the edit builders. There is no stock patch and no patch text of an instance's own: the patch language is the
// atom / function library (stdlib/) and the lowering's output, not a second container.
//
// The document itself never needs the library: a Device names its block and stores the values the user set
// (model.h). Not RT code.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/meta.h"
#include "pq/document/model.h"
#include "pq/meta/registry.h"
#include "pq/graph/infer.h"
#include "pq/graph/patch.h"
#include "pq/graph/patch_file.h"

namespace pq::doc {

// One param of a device patch (a `param` declaration), values normalised (graph::Constant::value(): Hz, seconds,
// linear gain for dB, a fraction for %; bools 0/1).
struct DeviceParamInfo {
  std::string name;               // the patch's param name
  std::string label;              // @label("…"), else the name
  graph::Kind kind = graph::Kind::Real;  // Real or Bool
  double defaultValue = 0.0;
  bool hasRange = false;
  double min = 0.0, max = 1.0;    // bools: 0..1
  graph::Unit unit = graph::Unit::None;  // display unit: @unit(…), else the default literal's suffix
  bool exponential = false;       // @curve(exp): a knob maps its travel exponentially
  uint32_t steps = 0;             // @steps(n): n discrete values from min to max (0: continuous)
  dsp::Curve curve;               // GU.1: the declared curve (graph::declaredCurve; a device may store its own)
};

// One structural setting: a `real const` input of the patch, set by a literal at the call site (Q121).
struct DeviceSettingInfo {
  std::string name;
  double defaultValue = 0.0;      // the input's literal default (hasDefault false: no literal default)
  bool hasDefault = false;
};

struct DeviceInfo {
  std::string patch;              // the callee name (Device::patch)
  std::string name;               // display name ("EQ Eight")
  DeviceKind kind = DeviceKind::AudioEffect;
  uint16_t inWidth = 0;           // declared width of the audio input (effects; 0 = inferred)
  uint16_t outWidth = 0;          // declared width of the first output (0 = inferred)
  std::vector<DeviceParamInfo> params;      // declaration order
  std::vector<DeviceSettingInfo> settings;  // declaration order
  const DeviceParamInfo* param(std::string_view name) const noexcept;
  const DeviceSettingInfo* setting(std::string_view name) const noexcept;
};

// Describes a device patch. Instrument: no audio `in`, a voice `out` first (and voice.* inside); AudioEffect: its
// first non-const `in` is audio rate and global, its first `out` global audio. Errors (InvalidArgument) for patches
// that are neither, so a broken library entry is reported when the library loads, not when a track plays it.
Result<DeviceInfo> describeDevice(const graph::Patch& patch, std::string displayName);

// The value a device plays for a param / setting: the one the user set, else the patch's default. GU.1: a param's in
// its units — its curve (the device's own, else the declared one) at its knob position (deviceParamPosition: the stored
// one, else the default's through the declared curve); a rack's macro or selector is not a DeviceParamInfo.
double deviceParamValue(const Device& d, const DeviceParamInfo& p) noexcept;
double deviceParamPosition(const Device& d, const DeviceParamInfo& p) noexcept;
const dsp::Curve& deviceParamCurve(const Device& d, const DeviceParamInfo& p) noexcept;
double deviceSettingValue(const Device& d, const DeviceSettingInfo& s) noexcept;

class DeviceLibrary;
// T4.18: the name of the device param `r` names (r.kind DeviceParam: owner = a device of `p`, target = the hash of the
// name): a `param` of the block (null `lib`: only the device's set values are looked at). "" when there is none (the device is gone, the patch has no such
// param). An automation lane carries it (AutomationLane::paramName).
std::string deviceParamNameOf(const Project& p, const DeviceLibrary* lib, const ParamRef& r);

// TMM.3: the description of a chain meta device of type `t` and `kind` (patch "meta:<id>", the type's name, params and
// settings from its specs); nullopt when the type does not allow that kind.
std::optional<DeviceInfo> metaDeviceInfo(const meta::Type& t, DeviceKind kind);

// TX.groups (model.h "Groups"): the patch every group device plays (groupPatchText(), parsed once; thread-safe) and its
// description (an audio effect "Group": in x, output; out y, input). DeviceLibrary::patchOf / infoOf answer with them.
const graph::Patch& groupPatch();
const DeviceInfo& groupDeviceInfo();
// TX.groups-fix1 (lower.h "Groups"): what a group plays where voices reach it — `GroupVoice` (the same ports with no
// `global` bound: a per-voice signal passes In and Out per voice) and, at the note source, `GroupNotes` (Out → y, and the
// In terminal's note signals: `input` the note's frequency, `phase` its phase; no `x`). The lowering picks one by the
// group's chain stage; the views and the edit builders see groupPatch() (the same terminal names).
const graph::Patch& groupVoicePatch();
const graph::Patch& groupNotesPatch();

// ---- GU.2: atom blocks and value blocks (model.h "Atom blocks and value blocks") ----------------------------------
// Which atoms may be a block of their own: not compiler-internal, not a voice source (they need a voice context), no
// side effect, not an I/O / sample / structure atom, no table resource (the wavetables, shaper, tuning_hz and the GU.1
// curve atom are the meta modules' and the lowering's own). The node menu offers exactly these (circuit/node_catalog).
bool atomBlockAllowed(const graph::AtomSignature& s) noexcept;
// The patch an atom block "atom:<name>" plays, built once per atom from its signature (no text: graph API, cached,
// thread-safe; lives for the process): `in <input> : <kind>[ const]` per fixed input (the variadic one its minimum
// count, numbered `<name>0` ...): an optional input defaults to the atom's default, a const one is a setting; the atom;
// `out <output>` per output (an output whose name an input has is `<name>_out`). Named "Atom_<name>" ('.' -> '_').
// The compiler flattens the call: the block costs exactly the atom. Null: no such atom, or not atomBlockAllowed.
const graph::Patch* atomBlockPatch(std::string_view atomName);
const DeviceInfo* atomBlockInfo(std::string_view atomName);
// GU.9: a voice terminal's block outside a voice context (not per note): the same outputs, each 0. Null: not a voice
// terminal.
const graph::Patch* voicelessAtomPatch(std::string_view atomName);  // its description (AudioEffect, settings: the const ins)
// The patch every value block plays (model.h valuePatchText, parsed once) and its description (one param `value`).
const graph::Patch& valuePatch();
const DeviceInfo& valueDeviceInfo();
// The fixed patch a built-in block plays (a group: groupPatch(); an atom block; a value block), or null for any other
// device. The views and the edit builders see these; the lowering picks a group's variant itself (lower.h "Groups").
const graph::Patch* builtinBlockPatch(const Device& d);

// The blocks' patches as one library. Fixed address: the library and every graph::ProjectGraph lowered with it point
// into it.
class DeviceLibrary {
public:
  DeviceLibrary();
  ~DeviceLibrary();
  DeviceLibrary(const DeviceLibrary&) = delete;
  DeviceLibrary& operator=(const DeviceLibrary&) = delete;

  const graph::PatchLibrary& library() const noexcept { return library_; }
  // The signatures of every patch in the library (parse externals for a caller's own patch text).
  std::span<const graph::SubcircuitRef> signatures() const noexcept { return signatures_; }

  // The patch `d` plays and its description; null for a device that is not a block (checks.cpp refuses those).
  // TMM.3: a native engine ("meta:<type>") plays its type's expansion: patchOf gives the prelude's chain form of the
  // type (the same params as every expansion of it) and infoOf its description (metaDeviceInfo), null for an unknown
  // type.
  const graph::Patch* patchOf(const Device& d) const;
  const DeviceInfo* infoOf(const Device& d) const;

  // TMM.3: the meta-module prelude the library was loaded with: its `meta_<type>` patches are in library(), their
  // signatures in signatures().
  const std::shared_ptr<const meta::Prelude>& metaPrelude() const noexcept { return prelude_; }

private:
  friend Result<std::unique_ptr<DeviceLibrary>> loadDeviceLibrary(std::string_view stdlibDirUtf8);
  std::shared_ptr<const meta::Prelude> prelude_;   // TMM.3
  std::vector<DeviceInfo> metaInfos_;              // TMM.3: per prelude entry: instrument, effect (kind order)
  graph::PatchLibrary library_;
  std::vector<graph::SubcircuitRef> signatures_;
};

// The library: the group, value and atom block patches and the meta-module prelude. `stdlibDirUtf8` is kept for the
// callers' sake (the app and the tests pass the stdlib root); nothing is read from it.
Result<std::unique_ptr<DeviceLibrary>> loadDeviceLibrary(std::string_view stdlibDirUtf8);

}  // namespace pq::doc
