#include "pq/library/indexer.h"

#include <windows.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include "pq/library/index_file.h"
#include "win_util.h"

namespace pq::library {

using detail::nowMs;
using detail::toUtf8;
using detail::toWide;
using detail::winPath;

namespace {

constexpr DWORD kWatchBufferBytes = 64u << 10;  // the maximum for network shares too
constexpr std::size_t kMaxWatches = MAXIMUM_WAIT_OBJECTS - 1;  // one handle is the command event

// Local fixed (or RAM) drives get a change watch; network and removable drives are rescanned on window focus.
bool watchable(const std::string& root) {
  const std::wstring w = toWide(root);
  wchar_t volume[MAX_PATH];
  if (!GetVolumePathNameW(w.c_str(), volume, MAX_PATH)) return false;
  const UINT type = GetDriveTypeW(volume);
  return type == DRIVE_FIXED || type == DRIVE_RAMDISK;
}

struct Watch {
  uint32_t place = 0;
  HANDLE dir = INVALID_HANDLE_VALUE;
  HANDLE event = nullptr;
  OVERLAPPED ov{};
  std::vector<DWORD> buffer;  // DWORD-aligned, as ReadDirectoryChangesW requires
  bool pending = false;

  bool arm() {
    ov = OVERLAPPED{};
    ov.hEvent = event;
    pending = ReadDirectoryChangesW(dir, buffer.data(), kWatchBufferBytes, TRUE,
                                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                                    nullptr, &ov, nullptr) != 0;
    return pending;
  }
  void close() {
    if (dir != INVALID_HANDLE_VALUE) {
      if (pending) {
        CancelIoEx(dir, &ov);
        DWORD bytes = 0;
        GetOverlappedResult(dir, &ov, &bytes, TRUE);
      }
      CloseHandle(dir);
    }
    if (event) CloseHandle(event);
    dir = INVALID_HANDLE_VALUE;
    event = nullptr;
    pending = false;
  }
};

}  // namespace

struct Indexer::Impl {
  HANDLE command = nullptr;  // auto-reset
  // Commands (guarded by Indexer::mutex_).
  bool stop = false;
  bool flush = false;
  bool rescanUnwatched = false;
  bool rescanAll = false;
  std::optional<std::vector<PlaceSpec>> newPlaces;
  std::atomic<bool> cancel{false};

  // Indexer thread state.
  std::vector<PlaceSpec> places;
  std::vector<uint8_t> watched;  // per place
  std::vector<std::unique_ptr<Watch>> watches;  // heap-stable: a pending read refers to its OVERLAPPED and buffer
  DiskSource disk;
  std::shared_ptr<const Index> current;
  std::vector<std::set<std::string>> pendingDirs, pendingRecursive;
  std::vector<uint8_t> pendingFull;
  bool hasPending = false;
  double coalesceDeadline = 0;
  bool snapshotDirty = false;
  double lastSnapshotMs = -std::numeric_limits<double>::infinity();
  double snapshotBackoffMs = 0;  // after a failed write (M5 review M14)
  uint64_t sequence = 0;
  Journal journal;
  std::filesystem::path pqi, pqj;
};

Indexer::Indexer(IndexerConfig config) : config_(std::move(config)), impl_(std::make_unique<Impl>()) {
  index_ = std::make_shared<Index>();
  impl_->command = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Indexer::~Indexer() {
  stop();
  if (impl_->command) CloseHandle(impl_->command);
}

void Indexer::start() {
  if (thread_.joinable()) return;
  {
    std::lock_guard lock(mutex_);
    impl_->stop = false;
  }
  impl_->cancel.store(false);
  thread_ = std::thread([this] { run(); });
}

void Indexer::stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard lock(mutex_);
    impl_->stop = true;
  }
  impl_->cancel.store(true);
  SetEvent(impl_->command);
  thread_.join();
}

std::shared_ptr<const Index> Indexer::index() const {
  std::lock_guard lock(mutex_);
  return index_;
}

uint64_t Indexer::waitForGeneration(uint64_t after, std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  cv_.wait_for(lock, timeout, [&] { return generation_.load() > after; });
  return generation_.load();
}

bool Indexer::waitIdle(std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, timeout, [&] { return stats_.idle; });
}

void Indexer::setOnChange(std::function<void(uint64_t)> fn) {
  std::lock_guard lock(mutex_);
  onChange_ = std::move(fn);
}

void Indexer::setPlaces(std::vector<PlaceSpec> places) {
  {
    std::lock_guard lock(mutex_);
    if (!thread_.joinable()) {
      config_.places = std::move(places);
      return;
    }
    impl_->newPlaces = std::move(places);
    stats_.idle = false;
  }
  SetEvent(impl_->command);
}

void Indexer::rescanUnwatched() {
  {
    std::lock_guard lock(mutex_);
    impl_->rescanUnwatched = true;
    stats_.idle = false;
  }
  SetEvent(impl_->command);
}

void Indexer::rescanAll() {
  {
    std::lock_guard lock(mutex_);
    impl_->rescanAll = true;
    stats_.idle = false;
  }
  SetEvent(impl_->command);
}

void Indexer::flush() {
  {
    std::lock_guard lock(mutex_);
    impl_->flush = true;
    stats_.idle = false;
  }
  SetEvent(impl_->command);
}

IndexerStats Indexer::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

void Indexer::publish(std::shared_ptr<const Index> index) {
  std::function<void(uint64_t)> cb;
  uint64_t gen = 0;
  {
    std::lock_guard lock(mutex_);
    index_ = std::move(index);
    gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    cb = onChange_;
  }
  cv_.notify_all();
  if (cb) cb(gen);
}

void Indexer::run() {
  Impl& m = *impl_;
  if (config_.backgroundPriority) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
  const ProbeFn probe = config_.probe ? config_.probe : ProbeFn(probeFile);
  {
    std::lock_guard lock(mutex_);
    m.places = config_.places;
    stats_ = IndexerStats{};
  }
  const bool persist = !config_.dataDir.empty();
  if (persist) {
    m.pqi = config_.dataDir / L"index.pqi";
    m.pqj = config_.dataDir / L"index.pqj";
  }

  // ---- Load ----
  const double t0 = nowMs();
  if (persist) {
    auto loaded = loadIndex(m.pqi);
    if (!loaded && loaded.error().code == ErrorCode::Parse) {
      // M5 review H8: a corrupt snapshot is deleted (with its journal), so it is not met again on every launch; the
      // revalidation below rebuilds the index from the places.
      std::error_code ec;
      std::filesystem::remove(m.pqi, ec);
      std::filesystem::remove(m.pqj, ec);
    }
    if (loaded) {
      std::shared_ptr<Index> idx = loaded->index;
      m.sequence = loaded->sequence;
      Journal::Contents j = Journal::read(m.pqj, m.sequence);
      if (!j.changes.empty()) idx = applyChanges(*idx, j.changes);
      if (j.validBytes > 0) (void)m.journal.openAppend(m.pqj, j.validBytes);
      m.current = idx;
      // A journal replay is not in the snapshot yet.
      m.snapshotDirty = !j.changes.empty();
    }
  }
  const double loadMs = nowMs() - t0;
  if (m.current) publish(m.current);
  {
    std::lock_guard lock(mutex_);
    stats_.loadMs = loadMs;
  }

  auto writeSnapshot = [&] {
    if (!persist || !m.current) return;
    if (saveIndex(*m.current, m.sequence + 1, m.pqi)) {
      ++m.sequence;
      (void)m.journal.reset(m.pqj, m.sequence);
      m.lastSnapshotMs = nowMs();
      m.snapshotDirty = false;
      m.snapshotBackoffMs = 0;
      std::lock_guard lock(mutex_);
      ++stats_.snapshotsWritten;
    } else {
      // M5 review M14: a write that fails (full disk, read-only or locked data folder) used to leave the timer due at
      // once, so the thread re-serialised the whole index in a loop at a full core. Retry after a doubling back-off.
      m.snapshotBackoffMs = std::clamp(m.snapshotBackoffMs * 2.0, 1000.0, 60000.0);
      m.lastSnapshotMs = nowMs() + m.snapshotBackoffMs - static_cast<double>(config_.snapshotIntervalMs);
      std::lock_guard lock(mutex_);
      ++stats_.snapshotFailures;
    }
  };

  // `structural`: the place list or a place's online state changed, which journal records (directory listings
  // replayed onto the snapshot's places) cannot express: the snapshot is written now (rare: place edits).
  auto persistChanges = [&](const std::vector<DirChange>& changes, bool structural) {
    if (!persist) return;
    m.snapshotDirty = true;
    const bool elapsed = nowMs() - m.lastSnapshotMs >= config_.snapshotIntervalMs;
    if (m.snapshotBackoffMs == 0 ? structural || changes.empty() || elapsed : elapsed) {
      writeSnapshot();
      return;
    }
    if (!m.journal.isOpen()) (void)m.journal.reset(m.pqj, m.sequence);
    if (m.journal.isOpen() && m.journal.append(changes)) {
      std::lock_guard lock(mutex_);
      ++stats_.journalAppends;
    }
  };

  // Rebuilds with the given request; publishes and persists when something changed.
  auto rebuildWith = [&](ScanRequest& req) -> bool {
    req.places = m.places;
    req.cancel = &m.cancel;
    ScanResult r = rebuild(m.current.get(), req, m.disk, probe);
    {
      std::lock_guard lock(mutex_);
      ++stats_.rebuilds;
      stats_.lastScan = r.stats;
    }
    if (r.stats.cancelled) return false;
    if (r.stats.changed || !m.current) {
      const bool structural = !m.current || m.current->places != r.index->places ||
                              m.current->placeOnline != r.index->placeOnline;
      m.current = r.index;
      publish(m.current);
      persistChanges(r.changes, structural);
    }
    return true;
  };

  auto closeWatches = [&] {
    for (auto& w : m.watches) w->close();
    m.watches.clear();
  };
  auto openWatches = [&] {
    closeWatches();
    m.watched.assign(m.places.size(), 0);
    for (uint32_t p = 0; p < m.places.size() && m.watches.size() < kMaxWatches; ++p) {
      if (!watchable(m.places[p].root)) continue;
      auto wp = std::make_unique<Watch>();
      Watch& w = *wp;
      w.place = p;
      w.dir = CreateFileW(winPath(m.places[p].root).c_str(), FILE_LIST_DIRECTORY,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
      if (w.dir == INVALID_HANDLE_VALUE) continue;
      w.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      w.buffer.resize(kWatchBufferBytes / sizeof(DWORD));
      if (!w.event || !w.arm()) {
        w.close();
        continue;
      }
      m.watched[p] = 1;
      m.watches.push_back(std::move(wp));
    }
    std::lock_guard lock(mutex_);
    stats_.watchedPlaces = static_cast<uint32_t>(m.watches.size());
  };
  auto resetPending = [&] {
    m.pendingDirs.assign(m.places.size(), {});
    m.pendingRecursive.assign(m.places.size(), {});
    m.pendingFull.assign(m.places.size(), 0);
    m.hasPending = false;
  };
  auto fullRescan = [&](auto&& include) {
    ScanRequest req;
    req.dirty.resize(m.places.size());
    for (uint32_t p = 0; p < m.places.size(); ++p) req.dirty[p].full = include(p);
    return rebuildWith(req);
  };

  // ---- Watch, then revalidate (changes during the revalidation queue up in the watch buffers) ----
  openWatches();
  resetPending();
  {
    const double r0 = nowMs();
    fullRescan([](uint32_t) { return true; });
    std::lock_guard lock(mutex_);
    stats_.revalidateMs = nowMs() - r0;
  }

  // ---- Event loop ----
  std::vector<HANDLE> handles;
  for (;;) {
    double deadline = std::numeric_limits<double>::infinity();
    if (m.hasPending) deadline = std::min(deadline, m.coalesceDeadline);
    if (m.snapshotDirty && persist) deadline = std::min(deadline, m.lastSnapshotMs + config_.snapshotIntervalMs);
    DWORD timeout = INFINITE;
    if (deadline != std::numeric_limits<double>::infinity())
      timeout = static_cast<DWORD>(std::clamp(deadline - nowMs() + 0.999, 0.0, 3600000.0));

    handles.clear();
    handles.push_back(m.command);
    for (const auto& w : m.watches) handles.push_back(w->event);
    if (!m.hasPending) {  // a pending snapshot timer (one wakeup, then none) still counts as idle
      {
        std::lock_guard lock(mutex_);
        stats_.idle = !m.newPlaces && !m.flush && !m.rescanAll && !m.rescanUnwatched && !m.stop;
      }
      cv_.notify_all();
    }
    const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout);
    {
      std::lock_guard lock(mutex_);
      ++stats_.wakeups;
      stats_.idle = false;
    }

    if (r == WAIT_OBJECT_0) {
      bool stop = false, flush = false, rescanUnwatched = false, rescanAll = false;
      std::optional<std::vector<PlaceSpec>> places;
      {
        std::lock_guard lock(mutex_);
        stop = m.stop;
        flush = std::exchange(m.flush, false);
        rescanUnwatched = std::exchange(m.rescanUnwatched, false);
        rescanAll = std::exchange(m.rescanAll, false);
        places = std::exchange(m.newPlaces, std::nullopt);
      }
      if (stop) break;
      if (places) {
        // New places: rewatch everything and revalidate (places that stay keep their entries; listing is cheap).
        m.places = std::move(*places);
        openWatches();
        resetPending();
        fullRescan([](uint32_t) { return true; });
      } else if (rescanAll) {
        resetPending();
        fullRescan([](uint32_t) { return true; });
      } else if (rescanUnwatched) {
        fullRescan([&](uint32_t p) { return p >= m.watched.size() || !m.watched[p]; });
      }
      if (flush && m.snapshotDirty) writeSnapshot();
    } else if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + handles.size()) {
      Watch& w = *m.watches[r - WAIT_OBJECT_0 - 1];
      DWORD bytes = 0;
      const BOOL ok = GetOverlappedResult(w.dir, &w.ov, &bytes, FALSE);
      w.pending = false;
      const uint32_t p = w.place;
      if (!ok || bytes == 0) {
        // Overflow (bytes == 0) or a failed watch: rescan the place.
        m.pendingFull[p] = 1;
        std::lock_guard lock(mutex_);
        ++stats_.overflows;
      } else {
        const auto* base = reinterpret_cast<const uint8_t*>(w.buffer.data());
        std::size_t at = 0;
        for (;;) {
          const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + at);
          const std::string rel = toUtf8(std::wstring_view(fni->FileName, fni->FileNameLength / sizeof(wchar_t)));
          const std::size_t sep = rel.rfind('\\');
          m.pendingDirs[p].insert(sep == std::string::npos ? std::string() : rel.substr(0, sep));
          if (fni->Action == FILE_ACTION_ADDED || fni->Action == FILE_ACTION_RENAMED_NEW_NAME)
            m.pendingRecursive[p].insert(rel);
          if (fni->NextEntryOffset == 0) break;
          at += fni->NextEntryOffset;
        }
      }
      if (!ok) {
        // The root went away or the handle broke: stop watching it (rescanUnwatched picks it up).
        w.close();
        m.watched[p] = 0;
        m.watches.erase(m.watches.begin() + (r - WAIT_OBJECT_0 - 1));
        std::lock_guard lock(mutex_);
        stats_.watchedPlaces = static_cast<uint32_t>(m.watches.size());
      } else if (!w.arm()) {
        m.pendingFull[p] = 1;
      }
      if (!m.hasPending) {
        m.hasPending = true;
        m.coalesceDeadline = nowMs() + config_.coalesceMs;
      }
    }

    const double now = nowMs();
    if (m.hasPending && now >= m.coalesceDeadline) {
      ScanRequest req;
      req.dirty.resize(m.places.size());
      for (uint32_t p = 0; p < m.places.size(); ++p) {
        req.dirty[p].full = m.pendingFull[p] != 0;
        req.dirty[p].dirs.assign(m.pendingDirs[p].begin(), m.pendingDirs[p].end());
        req.dirty[p].recursive.assign(m.pendingRecursive[p].begin(), m.pendingRecursive[p].end());
      }
      resetPending();
      if (!rebuildWith(req)) break;  // cancelled: stopping
    }
    if (persist && m.snapshotDirty && now >= m.lastSnapshotMs + config_.snapshotIntervalMs) writeSnapshot();
  }

  closeWatches();
  if (m.snapshotDirty) writeSnapshot();
  m.journal.close();
}

}  // namespace pq::library
