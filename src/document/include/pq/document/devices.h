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
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/model.h"
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

// The value a device plays for a param / setting: the one the user set, else the patch's default.
double deviceParamValue(const Device& d, const DeviceParamInfo& p) noexcept;
double deviceSettingValue(const Device& d, const DeviceSettingInfo& s) noexcept;

// A stock device: its file (relative to stdlib/), patch and display name.
struct StockDevice {
  std::string_view file;
  std::string_view patch;
  std::string_view name;
  DeviceKind kind = DeviceKind::AudioEffect;  // what describeDevice finds (the AOT seed's stock chains use it)
};
// The stock devices in menu order (instruments first).
std::span<const StockDevice> stockDevices() noexcept;

// ---- User devices (T4.4-fix1; SPEC §15 2026-10-26; model.h UserPatch) ---------------------------------------------
// What a user device (Device::source) plays. Its text is parsed as the circuit editor parses it (lenient layout,
// deferred missing inputs; imports through the library's loader, relative to the stock file, so the stdlib's imports
// resolve) with the NodeIds of UserPatch::ids (the editor's parse), else of the device's previous text, else of the
// stock file — so a fork keeps every state of the stock device that the edit did not touch. Its patches are renamed
// apart from the library's and from every other device's: `<name>__u<device id>_<16 hex digits of its graph hash>`.
// The hash covers what the program depends on (lower.h patchHash: nodes, ids, values, wires; not layout, comments or
// formatting), so a layout or comment edit keeps the names: the track's lowered patch stays shared and its unit does
// not recompile.
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
  const graph::Patch* patchOf(const Device& d, std::shared_ptr<const ResolvedDevice>& hold) const;
  const DeviceInfo* infoOf(const Device& d, std::shared_ptr<const ResolvedDevice>& hold) const;

  struct Files;
  struct UserCache;

private:
  friend Result<std::unique_ptr<DeviceLibrary>> loadDeviceLibrary(std::string_view stdlibDirUtf8);
  std::unique_ptr<Files> files_;
  std::unique_ptr<UserCache> users_;
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
