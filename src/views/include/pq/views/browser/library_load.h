#pragma once

// Bringing user-library items into the project and saving project items into the library (T5.11; docs/plans/M5.md
// T5.11), shared by the browser (Enter, double-click, hot-swap), the Device view (its menus, drops on a chain) and the
// tuning chips. Every load is one undo step.
//
// Items are named by path: a library file (.pqpreset, .pqdevice, .pqtuning, .scl, .pqclip) or a catalog device
// ("pqdaw-device:<patch>", catalogItemPath: the stock devices and racks the browser lists under Instruments / Audio
// Effects).
//
// UI thread only.

#include <cstdint>
#include <string>
#include <string_view>

#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/library_files.h"
#include "pq/document/model.h"
#include "pq/views/view_context.h"

namespace pq::views {

inline constexpr std::string_view kCatalogScheme = "pqdaw-device:";
std::string catalogItemPath(std::string_view patch);
// The patch of a catalog item path ("" when `path` is not one).
std::string_view catalogPatchOf(std::string_view path) noexcept;

// What a library path holds.
enum class LibraryItem : uint8_t { None, CatalogDevice, Preset, UserDevice, Tuning, Clip };
LibraryItem libraryItemOf(std::string_view path) noexcept;

// Where a device goes: a track's chain at `index` (doc::kAppend: the end), or a rack chain (`chain` valid).
struct DeviceSlot {
  doc::TrackId track;
  uint32_t index = doc::kAppend;
  doc::ChainId chain;
};

// The device's display name: the catalog's ("EQ Eight"), a rack's ("Audio Effect Rack"), else its patch.
std::string deviceDisplayName(const ViewContext& ctx, const doc::Device& d);
std::string deviceDisplayName(const ViewContext& ctx, std::string_view patch);

// A new device of the catalog with its default preset (Library::defaultPreset) when the User Library has one. Returns
// the new device's id.
Result<doc::DeviceId> addCatalogDevice(ViewContext& ctx, std::string_view patch, doc::DeviceKind kind, DeviceSlot at);
// Loads a catalog device, a preset or a user device into a chain. Returns the new device's id.
Result<doc::DeviceId> loadDeviceItem(ViewContext& ctx, std::string_view path, DeviceSlot at);
// Hot-swap: the preset (or user device) at `path` replaces `target` in place (doc::ops::replaceDevice; wires and macro
// mappings onto params the new device still has are kept).
Status hotSwapDevice(ViewContext& ctx, doc::DeviceId target, std::string_view path);
// The params a device would have (the catalog's, a user device's resolved patch, a rack's macros and selector).
std::vector<std::string> deviceParamNames(const ViewContext& ctx, const doc::Device& d);

// A tuning file (.pqtuning, or a Scala .scl at `referenceHz`) as a library tuning (name = the file's stem for .scl).
Result<doc::TuningFile> readTuningItem(std::string_view path, double referenceHz = tuning::kDefaultReferenceHz);
// Loads a tuning: into `target` (hot-swap: its tuning becomes the file's; one undo step), or (no target) as a new main
// tuning. Returns the label of what changed.
Result<std::string> loadTuningItem(ViewContext& ctx, std::string_view path, const TuningTarget* target);

// Loads a clip file at `start` onto `track` (a track of the clip's kind), else onto a new track of its kind after
// `track` (one undo step together). An audio clip's sample joins the project's table (or reuses the entry of the same
// resolved file).
Result<doc::ClipRef> loadClipItem(ViewContext& ctx, std::string_view path, doc::TrackId track, Ticks start);

// Saves (the Device view's menus): the device as a preset / as its device's default preset / (a user device) as a new
// device of the catalog. Returns the written path.
Result<std::string> saveDevicePreset(ViewContext& ctx, doc::DeviceId device, bool asDefault);
Result<std::string> saveAsNewDevice(ViewContext& ctx, doc::DeviceId device);
// A tuning ("Save to Library": the tunings panel, a tuning chip).
Result<std::string> saveTuningToLibrary(ViewContext& ctx, const TuningTarget& target);

// T5.12: an arrangement clip in a Clips drag payload (pq/ui/drag.h: ids = packed clip references).
inline uint64_t packClipRef(const doc::ClipRef& r) noexcept {
  return (static_cast<uint64_t>(r.track.value) << 32) | r.clip.value;
}
inline doc::ClipRef unpackClipRef(uint64_t v) noexcept {
  return doc::ClipRef{doc::TrackId{static_cast<uint32_t>(v >> 32)}, doc::ClipId{static_cast<uint32_t>(v)}};
}

}  // namespace pq::views
