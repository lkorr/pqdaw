#pragma once

// The index on disk (T5.9): `index.pqi` (a snapshot) + `index.pqj` (a journal), by default in
// %LOCALAPPDATA%\pqdaw\library (defaultLibraryDataDir; every caller can pass another directory, and tests always do).
//
// index.pqi: a 64-byte header (magic "PQLIBIDX", format version, snapshot sequence number, counts, payload size, a
// 64-bit hash of the payload), then the places, then every column of Index::Dirs and Index::Entries as raw
// little-endian arrays (each 8-byte aligned) and the string pool. Loading is one read plus one copy per column: no
// parsing per entry. Written as index.pqi.tmp, flushed, then renamed over the old file (MoveFileExW REPLACE_EXISTING |
// WRITE_THROUGH), so a crash leaves either the old or the new snapshot.
//
// index.pqj: a 32-byte header (magic "PQLIBJNL", the sequence number of the snapshot it extends), then records, each
// {u32 magic, u32 payload size, u64 hash of the payload, payload = one DirChange}. Changes are appended right after
// each rebuild, so the index a restart loads includes everything up to a crash; the snapshot is rewritten at most
// every 5 s (indexer.h) and the journal restarts with the new sequence number. On load, records stop at the first
// torn or corrupt one, and a journal whose sequence number is not the snapshot's is ignored (a crash between the
// snapshot's rename and the journal's restart). The startup revalidation corrects anything either file misses.
//
// Not RT code.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "pq/core/result.h"
#include "pq/library/index.h"
#include "pq/library/scan.h"

namespace pq::library {

inline constexpr uint32_t kIndexFormatVersion = 1;

// %LOCALAPPDATA%\pqdaw\library (not created here); empty when LOCALAPPDATA is not set.
std::filesystem::path defaultLibraryDataDir();
// Documents\pqdaw\User Library (Q4; not created here); empty when the Documents folder is unknown.
std::filesystem::path defaultUserLibraryDir();

struct LoadedIndex {
  std::shared_ptr<Index> index;
  uint64_t sequence = 0;
};

Status saveIndex(const Index& index, uint64_t sequence, const std::filesystem::path& file);
// Errors: NotFound (no file), Parse (bad magic / version / sizes / hash).
Result<LoadedIndex> loadIndex(const std::filesystem::path& file);

// Journal records.
std::vector<uint8_t> encodeChange(const DirChange& c);
bool decodeChange(const uint8_t* data, std::size_t size, DirChange& out);

class Journal {
public:
  Journal() = default;
  ~Journal();
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Creates (truncates) the journal for snapshot `sequence`.
  Status reset(const std::filesystem::path& file, uint64_t sequence);
  // Opens an existing journal for appending after `read` validated it up to `validBytes` (a torn tail is cut).
  Status openAppend(const std::filesystem::path& file, uint64_t validBytes);
  Status append(const std::vector<DirChange>& changes);
  void close() noexcept;
  bool isOpen() const noexcept { return handle_ != nullptr; }

  struct Contents {
    std::vector<DirChange> changes;
    uint64_t validBytes = 0;  // header + every intact record
  };
  // The records of a journal that extends snapshot `sequence` (none when the file is missing or its sequence differs).
  static Contents read(const std::filesystem::path& file, uint64_t sequence);

private:
  void* handle_ = nullptr;
};

}  // namespace pq::library
