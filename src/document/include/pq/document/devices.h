#pragma once

// The device library and catalog (T4.1; QUESTIONS Q?-T4.1-a, item 11).
//
// A device is an instance of a patch (SPEC §7.3: devices are subcircuits). The DEVICE LIBRARY is the set of patches
// devices may call: the stock devices of stdlib/ (BasicSynth, Utility, Saturator, EQ Eight) with everything they
// import, parsed once into one graph::PatchLibrary that the lowering (lower.h) calls into. The CATALOG describes each
// device patch for the views and the edit builders: its kind (instrument / audio effect), its params (range, unit,
// label, curve, steps: the patch's `param` declarations and attributes) and its settings (`real const` inputs:
// structural constants, Q121). Until the M5 browser this list is fixed (stockDevices()); user and project patches
// join the same catalog later.
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
};
// The stock devices in menu order (instruments first).
std::span<const StockDevice> stockDevices() noexcept;

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

  struct Files;

private:
  friend Result<std::unique_ptr<DeviceLibrary>> loadDeviceLibrary(std::string_view stdlibDirUtf8);
  std::unique_ptr<Files> files_;
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
