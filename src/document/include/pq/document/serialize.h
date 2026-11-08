#pragma once

// Project files (SPEC §12, ARCHITECTURE §4, T3.5).
//
// A project is a directory; the document lives in `project.pqd`: JSON, UTF-8 (no BOM written; one is skipped on
// read), with "schemaVersion" as the first member. Everything a snapshot holds is stored, plus the saved view
// state (fold/heights, Q26), with every id preserved, so load(save(p)) == p (deep equality) and the id source
// continues after maxEntityId (DocumentStore::reset).
//
// Exact numbers: ticks and ids are JSON integers; rationals are strings "n/d" (reduced); doubles and floats are
// written in the shortest form that reads back to the same value (std::to_chars), always with a '.' or an exponent
// so -0.0 survives; NaN and infinities are not representable and are rejected (on save and load). Tuning
// definitions are embedded (EDO size + equave, MOS parameters, the Scala scale's pitches — exact ratios as "n/d",
// ratios beyond int64 as their decimal digits (Q23), cents as numbers — and the .kbm mapping).
//
// Reading is strict: unknown or duplicate keys, wrong types, out-of-range values and broken references are errors
// whose message starts with the JSON path of the offending value, e.g.
//   "$.tracks[0].clips[2].notes[517].velocity: must be in (0, 127]"
// and whose location carries the file name and the 1-based line/column. Syntax errors give the path of the
// innermost open container. Lists the model keeps sorted (clips, breakpoints, locators, sends) must be sorted in
// the file; notes may be in any order.
//
// Performance: the writer streams straight into one string and the reader is a specialised pull parser that
// decodes into the model without an intermediate DOM: 100k notes save in ~42–50 ms and load in ~54–65 ms, where
// nlohmann/json alone needs ~114 ms (SAX, no-op handler) or 239–373 ms (DOM) to parse the same 14 MB file
// (bench/RESULTS.md). nlohmann/json is used for schema migrations,
// which are generic DOM transforms of old files (rare, never on the current-version path).
//
// Not RT code.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "pq/core/result.h"
#include "pq/document/model.h"
#include "pq/document/store.h"

namespace pq::doc {

// Schema history: 1 = ticks at PPQ 27,720; 2 = ticks at PPQ 221,760 (T0.5-fix1: migration 1 → 2 multiplies every
// stored tick by 8); 3 = device chains (T4.1: every track has "devices"; migration 2 → 3 gives every MIDI track the
// default instrument it played implicitly, with a new id above the file's largest, in track order); 4 = modulation
// wires (T4.7: an optional top-level "wires" list [{"id", "from", "port", "to", "param", "amount", "reducer"?}];
// migration 3 → 4 changes nothing); 5 = user devices (T4.4-fix1: a device may carry its own patch text as
// "source": [one string per line]; migration 4 → 5 changes nothing: every device of a schema-4 file is a stock one).
// 6 = racks (T4.8: a device may hold "rack": {"chains": [{"id", "name", "devices", "volumeDb"?, "pan"?, "active"?,
// "solo"?, "key"? / "velocity"? / "select"?: [lo, fadeLo, fadeHi, hi]}], "mappings"?: [{"id", "macro" 1…16, "device",
// "param", "min", "max", "curve"?}], "macroNames"?, "visibleMacros"?, "variations"?, "randomizeExclude"?, "keyUnit"?,
// "voiceChains"?, "showChains"?, "showMacros"?}; migration 5 → 6 changes nothing: racks are new).
// 7 = audio (T5.3): "kind": "audio" tracks in the main tree, whose "clips" hold audio clips tagged "kind": "audio" (one
// line each: "sample", "warp" "off" | "rePitch", "contentStartFrame", "seconds", "contentStart", "loop", "sourceBpm",
// "gainDb", "transpose" {"cents" | "steps" | "ratio"}, "detuneCents", "reverse", "fadeIn" / "fadeOut" {"length",
// "curve"}, "crossfadeIn", "tuning", "active"; a warp-Off clip's "length" is re-derived from the tempo map on load), and
// an optional top-level "samples" table [{"id", "path", "name", "hash" (16 hex digits), "size", "frames", "rate",
// "channels"}]; migration 6 → 7 changes nothing: audio is new.
// 8 = device names (T4.17: a device may carry "name", written only when set; migration 7 → 8 changes nothing).
// 9 = device automation (T4.18: a lane's "param" may be {"kind": "deviceParam", "owner", "name"} (a device param, a
// rack's macro or selector), {"kind": "chainMixer", "owner", "name": "volume" | "pan"} or {"kind": "wireAmount",
// "owner"}, on the track holding the device; migration 8 → 9 changes nothing).
// 10 = meta modules (TMM.3, model.h "Meta modules"): a device may carry "meta": {"<path>": {"type", "version",
// "params"?, "settings"?, "data"?}} ("" = a chain meta device itself, whose "patch" is "meta:<type>"); data values are
// numbers, strings, arrays, objects and {"$f32": "<base64 float32 LE>"} sample arrays (pq/meta/data.h); an older state
// version is upgraded by its type on load, a newer one is refused (Unsupported); migration 9 → 10 changes nothing).
// 11 = a chain is wires (TMM.10-fix1, pq/document/meta.h chainStages): a chain meta device may carry "chainInput" ("-"
// unplugged, or an input of its type; written only when not the type's own); a meta device's "kind" is its position's.
// Migration 10 → 11 unplugs a meta "audioEffect" that was the first active device where the note source arrives (it
// heard no note in schema 10) and sets every meta device's kind from its position.
// 12 = wires from the note source (TMM.10-fix2, model.h "Modulation wires"): a wire may carry "notes": <MIDI track id>
// (its source is that track's note source, its "port" a note output: "hz", "gate", "velocity") instead of "from";
// migration 11 → 12 changes nothing.
// 13 = wire laws and free devices (TX.patching, model.h "Modulation wires", Device::free): a wire without "law" adds its
// signal in its target's units and has no "amount"; "law": "travel" (with "amount") is the knob-travel law; a wire's
// "port" may name any output of its device and its "param" any param or input. A device may carry "free": true (out of
// its track's series). Migration 12 → 13 gives every wire "law": "travel" (old projects keep their sound).
// 14 = groups (TX.groups, model.h "Groups"): a device whose "patch" is "Group" is a group; a device may carry "group":
// <group device id> (a member: free, in the same track's list). Migration 13 → 14 changes nothing.
// 15 = chain links (TX.groups-fix1, model.h "Modulation wires"): a wire may carry "chain": true (a link of a grouped
// series: planned and summed before the other wires, as a chain's own link). Migration 14 → 15 changes nothing (a
// schema-14 group's series wires stay plain wires and lower as they did).
// 16 = knob curves (TX.knob-curves: a device's "curves"; schema-15 wires get "law": "units").
// 17 = everything is free (TX.all-free, main_path.h): a wire may carry "mixer": <track id> (a track's mixer input)
// instead of "to", "port" / "param" "" are main ports, and "notes" with "port" "" is any track's own signal; devices no
// longer carry "free". Migration 16 → 17 turns each track's chain into main links (source → … → mixer) at new ids, so
// the project sounds exactly as it did.
// 18 = pan laws (TX.pan-laws, model.h PanMode): a track's "mixer" may carry "panMode": "constantPower" | "linear" |
// "stereo" (absent: "balance"). Migration 17 → 18 gives every MIDI track "constantPower" (its law until then).
// 19 = instrument groups (IG.1-2, pq/document/instrument_groups.h): a wire may carry "single": true (a single-signal
// cable); a group's polyphony is its settings ("voices", "mono", "legato", "glide", "steal"). Migration 18 → 19 is a
// no-op on the JSON; after the decode every file older than 19 has its MIDI tracks' instruments wrapped into instrument
// groups (migrateToInstrumentGroups: sound-identical).
// 20 = knob positions (GU.1, docs/plans/GU.md "Curves"): a device's (and a nested meta module's) "params" are knob
// positions 0 … 1 (a rack's macros and selector keep 0 … 127); a device-param lane's points are positions; a device's
// "curves" are curve objects {param: {"min", "max", "unit"?, "digits"?, "step"?, "mode"?, "output"?, "preset",
// "presetArg"?, "points": [[t, y], …], "shapes": [[shape, bend], …], "detents"?: [interior point index, …] (TX.detent)}}
// (pq/dsp/curve.h). Migration 19 → 20 marks the file
// ("valuesInUnits", "legacyCurves": never written) and parseProject converts the decoded model with
// LoadOptions::devices (knob_positions.h: the nearest-position rule through each param's curve); without the library a
// file with values to convert is refused (Unsupported). Presets older than 20 holding params are refused (no library).
inline constexpr int32_t kSchemaVersion = 21;
inline constexpr std::string_view kProjectFileName = "project.pqd";

// One migration step: rewrites a document of schema version `from` into version `from + 1` in place. The loader
// sets "schemaVersion" itself afterwards. An error aborts the load.
using MigrationFn = Status (*)(nlohmann::json& doc);
struct Migration {
  int32_t from = 0;
  MigrationFn fn = nullptr;
};
// The built-in table: one step per schema version (1 → 2: ticks × 8; 2 → 3: the default instrument as a device;
// 3 → 4: none, wires are new; 4 → 5: none, user device texts are new; 5 → 6: none, racks are new; 6 → 7: none, audio
// tracks, audio clips and the sample table are new; 7 → 8: none, device names are new; 8 → 9: none, device lanes are
// new; 9 → 10: none, meta modules are new).
std::span<const Migration> builtinMigrations() noexcept;

class DeviceLibrary;
struct LoadOptions {
  // Steps applied to files older than kSchemaVersion, in order from the file's version (tests inject their own).
  std::span<const Migration> migrations = builtinMigrations();
  std::string_view fileName;  // labels errors (Error::location.file)
  // GU.1: the device library a file older than schema 20 needs (its values become knob positions through each param's
  // curve). Null: such a file loads only when it holds nothing to convert.
  const DeviceLibrary* devices = nullptr;
};

struct LoadedProject {
  Snapshot project;
  ViewState view;
  int32_t fileSchemaVersion = kSchemaVersion;  // before migration
};

// Text level. serializeProject fails only for values JSON cannot hold (non-finite numbers, invalid UTF-8 in a
// string), with the path of the value. parseProject validates everything the commands guarantee.
Result<std::string> serializeProject(const Project& p, const ViewState& view = {});
Result<LoadedProject> parseProject(std::string_view text, const LoadOptions& options = {});

// Test hook for the atomic save: fail at a given point as a disk error would.
enum class SaveFailPoint : uint8_t { None, WhileWriting, BeforeRename };
struct SaveOptions {
  SaveFailPoint failAt = SaveFailPoint::None;
};

// Directory level. `dirUtf8` is a UTF-8 path (converted to UTF-16 for the Windows API, so non-ASCII paths work).
// Save creates the directory if needed, writes `project.pqd.tmp`, flushes it, and renames it over `project.pqd`
// (MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH): a failure at any point leaves the previous
// file intact and removes the temporary file. View state for ids that no longer exist is not written.
Status saveProject(std::string_view dirUtf8, const Project& p, const ViewState& view = {},
                   const SaveOptions& options = {});
Result<LoadedProject> loadProject(std::string_view dirUtf8, const LoadOptions& options = {});

// DocumentStore convenience: save the current snapshot + view; open = load, then reset the store (history
// cleared, ids continue after the loaded document's largest id) and install the view state.
Status saveProject(std::string_view dirUtf8, const DocumentStore& store, const SaveOptions& options = {});
Status openProject(std::string_view dirUtf8, DocumentStore& store, const LoadOptions& options = {});

}  // namespace pq::doc
