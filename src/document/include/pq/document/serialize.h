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
// stored tick by 8).
inline constexpr int32_t kSchemaVersion = 2;
inline constexpr std::string_view kProjectFileName = "project.pqd";

// One migration step: rewrites a document of schema version `from` into version `from + 1` in place. The loader
// sets "schemaVersion" itself afterwards. An error aborts the load.
using MigrationFn = Status (*)(nlohmann::json& doc);
struct Migration {
  int32_t from = 0;
  MigrationFn fn = nullptr;
};
// The built-in table: one step per schema version (1 → 2: ticks × 8).
std::span<const Migration> builtinMigrations() noexcept;

struct LoadOptions {
  // Steps applied to files older than kSchemaVersion, in order from the file's version (tests inject their own).
  std::span<const Migration> migrations = builtinMigrations();
  std::string_view fileName;  // labels errors (Error::location.file)
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
