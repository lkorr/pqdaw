#pragma once

// The indexer thread (T5.9): keeps the browser index of the places current.
//
// On start() the thread (THREAD_MODE_BACKGROUND_BEGIN: low CPU, I/O and memory priority, out of the way of the audio
// and UI threads) loads index.pqi + its journal and publishes that index at once, arms one overlapped recursive
// ReadDirectoryChangesW per place root on a local fixed drive, then revalidates every place (scan.h: every directory
// listed, unchanged files never probed) and publishes the result. After that it blocks in WaitForMultipleObjects on
// the change handles and its command event, with no timeout: an idle indexer makes no wakeups. A notification starts
// a 100 ms coalescing window; when it closes, the directories the notifications named are relisted (scan.h) and a new
// index is published. A buffer overflow (or a watch that fails) rescans that place. Roots on network or removable
// drives (and places beyond the 62 handles one wait can take) are not watched; rescanUnwatched() (the app calls it on
// window focus) rescans them.
//
// Persistence: after a rebuild that changed something, the change records are appended to the journal; the snapshot
// is rewritten when the last one is ≥ snapshotIntervalMs old (default 5 s), else a timer is armed for then (one
// wakeup). A change of the place list or of a place's online state writes the snapshot at once (journal records only
// describe directories). stop() writes a pending snapshot.
//
// Thread safety: every public member may be called from any thread (non-RT; a mutex guards the published index).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "pq/library/index.h"
#include "pq/library/scan.h"

namespace pq::library {

struct IndexerConfig {
  std::filesystem::path dataDir;  // index.pqi / index.pqj; empty = no persistence
  std::vector<PlaceSpec> places;
  uint32_t coalesceMs = 100;
  uint32_t snapshotIntervalMs = 5000;
  bool backgroundPriority = true;
  ProbeFn probe;  // empty = probeFile
};

struct IndexerStats {
  uint64_t wakeups = 0;          // returns from the thread's wait (each command, notification or timer)
  uint64_t rebuilds = 0;
  uint64_t snapshotsWritten = 0;
  uint64_t snapshotFailures = 0; // snapshot writes that failed (retried after a doubling back-off, 1 s … 60 s)
  uint64_t journalAppends = 0;
  uint64_t overflows = 0;        // change buffers that overflowed (place rescanned)
  double loadMs = 0;             // index.pqi + journal load at start
  double revalidateMs = 0;       // the startup revalidation of every place
  ScanStats lastScan;            // of the most recent rebuild
  uint32_t watchedPlaces = 0;
  bool idle = false;             // blocked with no coalescing window open (a snapshot timer may be armed: it fires
                                 // once, ≤ snapshotIntervalMs after the last change, and then the wait has no timeout)
};

class Indexer {
public:
  explicit Indexer(IndexerConfig config);
  ~Indexer();  // stop()
  Indexer(const Indexer&) = delete;
  Indexer& operator=(const Indexer&) = delete;

  void start();
  void stop();

  // The current index (never null after start(): empty until the first load or scan) and its generation (counts
  // publishes from 1).
  std::shared_ptr<const Index> index() const;
  uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
  // Blocks until generation() > `after` or the timeout passes; returns generation().
  uint64_t waitForGeneration(uint64_t after, std::chrono::milliseconds timeout) const;
  // Blocks until the thread has finished the startup revalidation and is idle (tests).
  bool waitIdle(std::chrono::milliseconds timeout) const;

  // Called on the indexer thread after each publish (the app posts a UI event). Set before start().
  void setOnChange(std::function<void(uint64_t generation)> fn);

  void setPlaces(std::vector<PlaceSpec> places);  // relists added places, drops removed ones, rewatches
  void rescanUnwatched();                         // window focus: rescan places without a watch
  void rescanAll();
  void flush();                                   // write the snapshot now if anything is unsaved

  IndexerStats stats() const;

private:
  struct Impl;
  void run();
  void publish(std::shared_ptr<const Index> index);

  IndexerConfig config_;
  std::unique_ptr<Impl> impl_;
  std::thread thread_;
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::shared_ptr<const Index> index_;
  std::atomic<uint64_t> generation_{0};
  std::function<void(uint64_t)> onChange_;
  IndexerStats stats_;
};

}  // namespace pq::library
