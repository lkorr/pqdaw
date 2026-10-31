#pragma once

// Building an Index (T5.9): a rebuild walks the places depth first and, per directory, either copies the old index's
// entries (a directory nothing reported as changed) or lists the directory and merges the listing with the old entries
// by name: a file whose size, mtime and file ID are unchanged keeps its record (no probe); a file that is new by name
// but has an old entry with the same file ID, size and mtime (a rename or move) keeps that record's probe fields; only
// what is left is probed. So revalidating an unchanged tree lists every directory and probes nothing, and a watcher
// batch relists only the directories it touched.
//
// Directory mtimes are not used to skip listings: NTFS updates a directory's mtime when an entry is added, removed or
// renamed, but not when a file inside is rewritten in place, and never for changes deeper down, so a skip would miss
// edits made while pqdaw was closed. A listing returns sizes, mtimes and file IDs of a whole directory in one call
// (FileIdExtdDirectoryInfo), which makes the full listing about as cheap as the stat a skip would need.
//
// Probing can be deferred (T5.x-index-progressive): with ScanRequest::probeBudgetMs set, files that still need a probe
// once the rebuild has run that long are listed without one (kFlagUnprobed), so a first scan of a large folder
// publishes its whole tree after the listing alone; the indexer probes them afterwards (indexer.h).
//
// Not RT code. rebuild() runs on the indexer thread (or a test's thread); it allocates.

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/library/index.h"

namespace pq::library {

// One directory's contents.
struct Listing {
  std::vector<EntryRecord> files;   // indexable files only; probe fields filled when `probed`
  std::vector<std::string> subdirs; // names
  bool probed = false;              // records are complete (a journal replay), nothing to probe
};

// A directory whose contents changed in a rebuild (what the journal records).
struct DirChange {
  std::string placeRoot;  // PlaceSpec::root of its place
  std::string relPath;    // relative to the place root
  Listing listing;        // probed (complete records)
};

class ListingSource {
public:
  virtual ~ListingSource() = default;
  // Lists `relPath` below `place`'s root into `out` (cleared by the caller). False: the directory cannot be listed
  // (missing, access denied, a drive that is gone): the rebuild keeps what the old index had there.
  virtual bool list(uint32_t placeIndex, const PlaceSpec& place, std::string_view relPath, Listing& out) = 0;
};

// Lists directories from disk (Win32: FileIdExtdDirectoryInfo; FindFirstFileExW where the file system lacks it).
class DiskSource final : public ListingSource {
public:
  bool list(uint32_t placeIndex, const PlaceSpec& place, std::string_view relPath, Listing& out) override;

private:
  std::vector<uint8_t> buffer_;
  std::vector<uint64_t> volumes_;  // per place index, 0 = not queried yet
};

// Fills a record's probe fields and category for the file at `absPath` (UTF-8). The default (probeFile) runs
// media::probe with headersOnly on Samples (never decodes or scans: an MP3 without a frame count gets an estimated
// length, kFlagLengthEstimated) and reads the first 4 KiB of presets and user devices; other kinds only get their
// category. Tests inject counting, slow or failing probes. The indexer's probe pass calls it from several threads at
// once (each call on its own record): it must be thread-safe.
using ProbeFn = std::function<void(const std::string& absPath, const PlaceSpec& place, std::string_view relDir,
                                   EntryRecord& r)>;
void probeFile(const std::string& absPath, const PlaceSpec& place, std::string_view relDir, EntryRecord& r);

// A record listed without its probe: kFlagUnprobed, probe fields zero, the category its kind and path give.
void markUnprobed(const PlaceSpec& place, std::string_view relDir, EntryRecord& r);

// Live counters of a running rebuild (written by it, read by other threads; relaxed).
struct ScanProgress {
  std::atomic<uint32_t> place{0};        // the place being listed
  std::atomic<uint32_t> filesListed{0};  // entries in the index being built so far
  std::atomic<uint32_t> dirsListed{0};   // directories in it so far
};

struct ScanRequest {
  std::vector<PlaceSpec> places;
  struct PlaceDirty {
    bool full = false;                     // relist every directory of the place (startup revalidation)
    std::vector<std::string> dirs;         // relative paths to relist (only that directory)
    std::vector<std::string> recursive;    // relative paths to relist with everything below (added / moved-in dirs)
  };
  std::vector<PlaceDirty> dirty;  // per place; a place without an element here (or new to the index) is relisted fully
  // Checked once per directory: when it becomes true the rebuild stops (ScanStats::cancelled; the result is partial and
  // must be discarded). The indexer sets it on stop().
  const std::atomic<bool>* cancel = nullptr;
  // Files needing a probe once the rebuild has run this long (from its start, listing included) are listed unprobed
  // (markUnprobed). Infinity (the default): every file is probed inline.
  double probeBudgetMs = std::numeric_limits<double>::infinity();
  ScanProgress* progress = nullptr;  // optional
};

struct ScanStats {
  uint32_t dirsListed = 0;
  uint32_t dirsCopied = 0;
  uint32_t listFailures = 0;
  uint32_t filesProbed = 0;
  uint32_t filesReused = 0;   // same name, size, mtime
  uint32_t filesMoved = 0;    // matched by file ID (rename / move)
  uint32_t filesDeferred = 0; // listed unprobed (ScanRequest::probeBudgetMs ran out)
  bool changed = false;       // the new index differs from the old one
  bool cancelled = false;
};

struct ScanResult {
  std::shared_ptr<Index> index;
  std::vector<DirChange> changes;  // listed directories whose contents changed (for the journal)
  ScanStats stats;
};

// Builds a new index. `old` may be null (initial scan). Places of `req` not present in `old` are scanned fully.
ScanResult rebuild(const Index* old, const ScanRequest& req, ListingSource& source, const ProbeFn& probe);

// Replays journal records onto an index: each DirChange replaces that directory's files and subdirectory set (a
// subdirectory that is no longer listed is removed with everything below it).
std::shared_ptr<Index> applyChanges(const Index& base, const std::vector<DirChange>& changes);

}  // namespace pq::library
