#pragma once

// Project samples (T5.8; SPEC §12; docs/plans/M5.md T5.8 with the user's answer 3 of 2026-10-30): files are REFERENCED
// by default (a sample's path is where the file was when it entered the project: absolute, or project-relative when it
// lies inside the project directory); "Collect All and Save" copies every used file from outside the project into
// `samples/imported/` and saves with the new paths. A plain save copies nothing. On load each sample is resolved
// (resolveSamples); what cannot be found is MISSING: its clips are silent (audio_sequence.h AudioLowerOptions) and the
// status line offers Relink (relinkSamples: a folder searched recursively by name, verified by size and content hash).
// Path updates from a collect or a relink are not undo steps (DocumentStore::relocateSamples).
//
// The document does not decode audio: what a file is (frames, rate, channels) comes from a SampleProbeFn the app plugs
// in (media's probe once T5.1 lands; tests use the WAV reader). The content hash is computed here
// (sampleContentHash), so the stored hash and every check against it use one definition.
//
// Paths are UTF-8 with '/' separators in the document; the file APIs convert to UTF-16. Not RT code; the copy runs on
// a worker thread (runCollect blocks), the rest on the UI thread.

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/model.h"
#include "pq/document/store.h"

namespace pq::doc {

// What a probe says about a file (the fields SampleRef keeps).
struct SampleProbe {
  int64_t frames = 0;
  double rate = 0.0;
  uint32_t channels = 0;
};
using SampleProbeFn = std::function<Result<SampleProbe>(std::string_view pathUtf8)>;

// A file's size and fast content hash: FNV-1a 64 over the size and three 64 KiB windows (start, middle, end), so it
// reads at most 192 KiB whatever the file's length. Errors: NotFound, Io.
struct FileIdentity {
  uint64_t size = 0;
  uint64_t hash = 0;
  friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};
Result<FileIdentity> fileIdentity(std::string_view pathUtf8);

// Path helpers (pure string work; '\\' and '/' accepted, '/' written). A path is absolute with a drive ("C:/…") or a
// UNC prefix ("//server/…").
bool isAbsoluteSamplePath(std::string_view path) noexcept;
// The stored form of an absolute file path: relative to `projectDir` ("samples/x.wav") when inside it, else the
// absolute path with '/' separators. An empty projectDir (untitled project): always absolute.
std::string storedSamplePath(std::string_view absPath, std::string_view projectDir);
// The absolute path a stored path names (a relative one joined to `projectDir`).
std::string absoluteSamplePath(std::string_view stored, std::string_view projectDir);

// A new table entry for an existing file (its id drawn from `ids`; display name = the file name without extension).
Result<SampleRef> makeSampleRef(IdSource& ids, std::string_view absPath, std::string_view projectDir,
                                const SampleProbeFn& probe);

// ---- Resolution and relink ---------------------------------------------------------------------------------------

struct SampleSearch {
  std::string projectDir;            // empty: an untitled project (relative paths cannot resolve)
  std::vector<std::string> folders;  // searched after the project folder: library places, the last relink folder
};
struct SampleResolution {
  SampleId id;
  std::string path;                     // the absolute path that plays; empty = missing
  std::optional<std::string> relocated;  // found elsewhere: the new stored path (DocumentStore::relocateSamples)
};
// Every entry of the table, in table order. A stored path resolves when the file exists there (a relative one under
// projectDir; an absolute one as written); otherwise the project folder and then `folders` are searched recursively
// for a file of the same name and size (and content hash, when the entry has one).
std::vector<SampleResolution> resolveSamples(const Project& p, const SampleSearch& search);
// The ids of the missing entries that a clip plays (unused entries never count).
std::vector<SampleId> missingSamples(const Project& p, std::span<const SampleResolution> resolved);
// Relink: searches `folder` recursively for the given entries (by name; size and hash verified) and returns them with
// their new stored paths, for those found. Every matching entry is relinked in one pass.
std::vector<SampleRef> relinkSamples(const Project& p, std::span<const SampleId> ids, std::string_view folder,
                                     std::string_view projectDir);

// ---- Collect All and Save ------------------------------------------------------------------------------------------

struct CollectItem {
  SampleId id;
  std::string from;    // absolute source
  std::string to;      // absolute destination under <projectDir>/samples/imported/
  std::string stored;  // the new stored path ("samples/imported/<name>")
  uint64_t bytes = 0;
  bool copy = true;    // false: an identical file is already there (same size and hash), or another item copies it
};
struct CollectPlan {
  std::vector<CollectItem> items;
  uint64_t bytes = 0;  // to copy
};
// Every sample a clip plays whose resolved file lies outside `projectDir` → samples/imported/<file name>. A name taken
// by a file with the same size and hash reuses that file; a different one gets " (2)", " (3)" … before the extension.
// Missing samples are skipped. Errors: an untitled project (no directory).
Result<CollectPlan> planCollect(const Project& p, std::string_view projectDir, std::span<const SampleResolution> resolved);

enum class CollectFailPoint : uint8_t { None, MidCopy };  // test hook: a disk error halfway through the bytes
struct CollectOptions {
  std::function<void(uint64_t done, uint64_t total)> progress;  // called from the copying thread
  const std::atomic<bool>* cancel = nullptr;
  CollectFailPoint failAt = CollectFailPoint::None;
};
// Copies the plan's files (each to "<to>.part", flushed, then renamed into place; samples/imported created). Blocking;
// run it on a worker. On an error or cancel the partial file is removed and nothing else changes (files already
// renamed into place stay: they are identical copies the next collect reuses).
Status runCollect(const CollectPlan& plan, const CollectOptions& options = {});
// The table entries with their stored paths after the plan's copies.
std::vector<SampleRef> collectedRefs(const Project& p, const CollectPlan& plan);
// The whole operation, blocking (CLI, tests): resolve, plan, copy, relocate the store's samples, then save
// `project.pqd` last (atomic): a failure before the save leaves the old project file untouched.
Status collectAllAndSave(DocumentStore& store, std::string_view dirUtf8, const SampleSearch& search,
                         const CollectOptions& options = {});

}  // namespace pq::doc
