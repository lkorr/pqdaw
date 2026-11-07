#pragma once

// The device library and catalog (T4.1; QUESTIONS Q129, item 11).
//
// A device is an instance of a patch (SPEC §7.3: devices are subcircuits). The DEVICE LIBRARY is the set of patches
// devices may call: the stock devices of stdlib/ (BasicSynth, Operator, Utility, Saturator, EQ Eight) with everything they
// import, parsed once into one graph::PatchLibrary that the lowering (lower.h) calls into. The CATALOG describes each
// device patch for the views and the edit builders: its kind (instrument / audio effect), its params (range, unit,
// label, curve, steps: the patch's `param` declarations and attributes) and its settings (`real const` inputs:
// structural constants, Q121). Until the M5 browser this list is fixed (stockDevices()); user and project patches
// join the same catalog later. T4.4-fix1: a device edited in the circuit view plays its own text (a user device,
// model.h UserPatch), resolved here (DeviceLibrary::resolve; see "User devices" below).
//
// The document itself never needs the library: a Device names its patch and stores the values the user set
// (model.h). Not RT code; loadDeviceLibrary does file I/O.

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

class DeviceCatalog {
public:
  void add(DeviceInfo info);
  std::span<const DeviceInfo> devices() const noexcept { return devices_; }
  const DeviceInfo* find(std::string_view patch) const noexcept;

private:
  std::vector<DeviceInfo> devices_;
};

// The value a device plays for a param / setting: the one the user set, else the patch's default. GU.1: a param's in
// its units — its curve (the device's own, else the declared one) at its knob position (deviceParamPosition: the stored
// one, else the default's through the declared curve); a rack's macro or selector is not a DeviceParamInfo.
double deviceParamValue(const Device& d, const DeviceParamInfo& p) noexcept;
double deviceParamPosition(const Device& d, const DeviceParamInfo& p) noexcept;
const dsp::Curve& deviceParamCurve(const Device& d, const DeviceParamInfo& p) noexcept;
double deviceSettingValue(const Device& d, const DeviceSettingInfo& s) noexcept;

class DeviceLibrary;
// T4.18: the name of the device param `r` names (r.kind DeviceParam: owner = a device of `p`, target = the hash of the
// name): a rack's macro or selector, else a `param` of the patch the device plays (`lib`: stock or its user patch;
// null: only the device's set values are looked at). "" when there is none (the device is gone, the patch has no such
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

// A stock device: its file (relative to stdlib/), patch and display name.
struct StockDevice {
  std::string_view file;
  std::string_view patch;
  std::string_view name;
  DeviceKind kind = DeviceKind::AudioEffect;  // what describeDevice finds (the AOT seed's stock chains use it)
  // TX.spectrum: its panel shows the spectrum through it and its measured response (EQ Eight, Auto Filter): the
  // lowering gives it an analysis tap (lower.h "Analysis taps").
  bool analysed = false;
};
// The stock devices in menu order (instruments first).
std::span<const StockDevice> stockDevices() noexcept;
// TX.spectrum: whether a device playing `patch` (Device::patch: a stock name, also for an edited fork) is analysed.
bool analysedDevice(std::string_view patch) noexcept;
// The name path its tap is requested under (LowerOptions::metaSlotOf(device, kAnalysisTapPath, 0)): no meta instance
// path starts with '#'.
inline constexpr std::string_view kAnalysisTapPath = "#analysis";

// ---- User devices (T4.4-fix1; SPEC §15 2026-10-26; model.h UserPatch) ---------------------------------------------
// What a user device (Device::source) plays. Its text is parsed as the circuit editor parses it (lenient layout,
// deferred missing inputs; imports through the library's loader, relative to the stock file, so the stdlib's imports
// resolve) with the NodeIds of UserPatch::ids (the editor's parse), else of the device's previous text, else of the
// stock file — so a fork keeps every state of the stock device that the edit did not touch. Its patches are renamed
// apart from the library's and from every other device's: `<name>__u<device id>_<16 hex digits of its graph hash>`.
// The hash covers what the program depends on (lower.h patchHash: nodes, ids, values, wires; not layout, comments or
// formatting), so a layout or comment edit keeps the names: the track's lowered patch stays shared and its unit does
// not recompile.
// TX.node-menu: a user device whose Device::patch names no stock device (the node menu's free device of one atom, made
// from no stock file) is resolved from its text alone: the same parse, imports relative to the stdlib root, no ids to
// carry but its own previous parse.
// A text that does not parse, has no patch named Device::patch, does not validate or is not a device of its kind is an
// ERROR: the device keeps playing its last good text of this session (`stale`; the circuit editor's rule: nothing that
// fails reaches the audio), or, without one (a loaded project), it is bypassed. Either way `error` says why and the
// status line names it.
struct UserDeviceGraph {
  graph::PatchFile file;        // the text's patches, renamed
  graph::PatchLibrary library;  // file's patches by their new names
  std::string root;             // the device patch's new name
  uint64_t hash = 0;            // of the graph (the suffix of the names)
};
struct ResolvedDevice {
  DeviceId device;
  UserPatchPtr source;                           // the text resolved
  graph::PatchFile parsed;                       // its parse, names as written (the editor opens the device with these ids)
  bool parsedOk = false;                         // `parsed` lowered (else it is the last good parse, or empty)
  std::shared_ptr<const UserDeviceGraph> graph;  // what plays: this text's, the last good one (stale), or null
  bool stale = false;                            // `graph` is an earlier text's: this one has an error
  std::vector<const graph::PatchFile*> extraFiles;  // stdlib files loaded for user imports beyond the stock set
  DeviceInfo info;                               // of `graph`'s root (display name: the stock device's)
  std::string error;                             // "" or why this text does not play
  const graph::Patch* root() const noexcept { return graph ? graph->library.find(graph->root) : nullptr; }
};

// The parsed device patches (and what they import) as one library, plus the catalog. Fixed address: the library
// and every graph::ProjectGraph lowered with it point into it.
class DeviceLibrary {
public:
  DeviceLibrary();
  ~DeviceLibrary();
  DeviceLibrary(const DeviceLibrary&) = delete;
  DeviceLibrary& operator=(const DeviceLibrary&) = delete;

  const graph::PatchLibrary& library() const noexcept { return library_; }
  const DeviceCatalog& catalog() const noexcept { return catalog_; }
  // The signatures of every patch in the library (parse externals for a caller's own patch text).
  std::span<const graph::SubcircuitRef> signatures() const noexcept { return signatures_; }
  // The .pqp file a catalog device was loaded from (UTF-8; "" when unknown): the views find its panel description
  // next to it (`<stem>.panel.json`, T4.6).
  std::string pathOf(std::string_view patch) const;
  // Stock devices whose file is not in this stdlib directory (not in the catalog; a project naming one bypasses it).
  std::span<const std::string> missing() const noexcept { return missing_; }

  // ---- T4.4-fix1: user devices (see ResolvedDevice) ----
  // `d` resolved, or null for a stock device. Thread-safe (the UI thread and the compile worker resolve); cached per
  // device by text (the last kResolvedPerDevice texts), so an unchanged device costs a lookup.
  std::shared_ptr<const ResolvedDevice> resolve(const Device& d) const;
  static constexpr size_t kResolvedPerDevice = 16;
  // The text of a stock device's file ("" when unknown) and its parse (the ids the editor opens a stock device with).
  std::string stockText(std::string_view patch) const;
  const graph::PatchFile* stockFile(std::string_view patch) const;
  // The patch `d` plays (the library's, or a user device's own root; `hold` keeps that alive) and its description.
  // Null: nothing plays (a patch not in the library, a user text without a good version).
  // TMM.3: a chain meta device ("meta:<type>") plays its type's expansion: patchOf gives the prelude's chain form of the
  // type (the same params as every expansion of it) and infoOf its description (metaDeviceInfo), null for an unknown
  // type or a kind the type does not allow.
  const graph::Patch* patchOf(const Device& d, std::shared_ptr<const ResolvedDevice>& hold) const;
  const DeviceInfo* infoOf(const Device& d, std::shared_ptr<const ResolvedDevice>& hold) const;

  // ---- TMM.3: meta modules (pq/document/meta.h) ----
  // The meta-module prelude the library was loaded with: its `meta_<type>` patches are in library(), their signatures in
  // signatures(), so stock files and user texts may call them.
  const std::shared_ptr<const meta::Prelude>& metaPrelude() const noexcept { return prelude_; }
  // The nested meta calls of `d`'s circuit (metaSites of `root`, the patch d plays; `user` its resolution when it is a
  // user device), cached per stock patch / per user graph (the cache holds the graph, so a key is never reused). Null:
  // none. Thread-safe.
  std::shared_ptr<const std::vector<MetaSite>> metaSitesOf(const Device& d, const graph::Patch& root,
                                                          const ResolvedDevice* user, const PatchLookup& lookup) const;
  struct MetaCache;

  struct Files;
  struct UserCache;

private:
  friend Result<std::unique_ptr<DeviceLibrary>> loadDeviceLibrary(std::string_view stdlibDirUtf8);
  std::unique_ptr<Files> files_;
  std::unique_ptr<UserCache> users_;
  std::unique_ptr<MetaCache> metaCache_;           // TMM.3
  std::shared_ptr<const meta::Prelude> prelude_;   // TMM.3
  std::vector<DeviceInfo> metaInfos_;              // TMM.3: per prelude entry: instrument, effect (kind order)
  graph::PatchLibrary library_;
  DeviceCatalog catalog_;
  std::vector<graph::SubcircuitRef> signatures_;
  std::vector<std::string> missing_;
  std::vector<std::pair<std::string, std::string>> paths_;  // patch → file
};

// Parses every stock device (stockDevices()) under `stdlibDirUtf8` with its imports. A stock file that does not exist
// is left out (DeviceLibrary::missing(): a reduced stdlib, e.g. a test's probe instrument). Errors: NotFound (no stock
// device at all, or a file without its patch), Io (an unreadable file or import), Parse (diagnostics formatted with
// file:line:col), InvalidArgument (a patch that is not a device).
Result<std::unique_ptr<DeviceLibrary>> loadDeviceLibrary(std::string_view stdlibDirUtf8);

}  // namespace pq::doc
