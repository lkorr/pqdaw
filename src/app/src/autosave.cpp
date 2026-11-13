// Autosave (AS.1): see pq/app/autosave.h.
#include "pq/app/autosave.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "pq/app/recording.h"
#include "pq/document/serialize.h"
#include "pq/views/view_context.h"

namespace pq::app {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string utf8(const fs::path& p) {
  const std::u8string u = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
fs::path u8path(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

Status ioError(std::string msg) { return Error{ErrorCode::Io, std::move(msg), {}}; }

// autosave.json, atomically: written to a temporary file, flushed, renamed over the old one.
Status writeMeta(const fs::path& dir, const AutosaveMeta& m) {
  json j = json::object();
  j["originDir"] = m.originDir;
  j["recordingsDir"] = m.recordingsDir;
  j["name"] = m.name;
  j["savedAt"] = m.savedAt;
  j["originMtime"] = m.originMtime;
  const fs::path file = dir / u8path(kAutosaveMetaFileName);
  const fs::path tmp = fs::path(file).concat(L".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return ioError("cannot write " + utf8(tmp));
    out << j.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    out.flush();
    if (!out) return ioError("cannot write " + utf8(tmp));
  }
  if (HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      h != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(h);
    CloseHandle(h);
  }
  if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD err = GetLastError();
    std::error_code ec;
    fs::remove(tmp, ec);
    return ioError("cannot replace " + utf8(file) + " (error " + std::to_string(err) + ")");
  }
  return Ok{};
}

}  // namespace

int64_t fileUnixTime(const fs::path& file) {
  std::error_code ec;
  const fs::file_time_type t = fs::last_write_time(file, ec);
  if (ec) return 0;
  const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
  return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

Result<AutosaveMeta> readAutosaveMeta(const fs::path& autosaveDir) {
  const fs::path file = autosaveDir / u8path(kAutosaveMetaFileName);
  std::ifstream in(file, std::ios::binary);
  if (!in) return Error{ErrorCode::NotFound, "no " + utf8(file), {}};
  std::ostringstream ss;
  ss << in.rdbuf();
  const json j = json::parse(ss.str(), nullptr, false);
  if (j.is_discarded() || !j.is_object()) return Error{ErrorCode::Parse, utf8(file) + ": not a JSON object", {}};
  AutosaveMeta m;
  auto str = [&](const char* k, std::string& out) {
    const auto f = j.find(k);
    if (f == j.end() || !f->is_string()) return false;
    out = f->get<std::string>();
    return true;
  };
  auto num = [&](const char* k, int64_t& out) {
    const auto f = j.find(k);
    if (f == j.end() || !f->is_number_integer()) return false;
    out = f->get<int64_t>();
    return true;
  };
  if (!str("originDir", m.originDir) || !str("recordingsDir", m.recordingsDir) || !str("name", m.name) ||
      !num("savedAt", m.savedAt) || !num("originMtime", m.originMtime))
    return Error{ErrorCode::Parse, utf8(file) + ": a field is missing or has the wrong type", {}};
  return m;
}

Autosaver::Autosaver(fs::path dir, fs::path root, bool threaded, AutosavePolicy policy)
    : dir_(std::move(dir)), root_(std::move(root)), threaded_(threaded), policy_(policy) {}

Autosaver::~Autosaver() {
  if (thread_.joinable()) {
    {
      std::lock_guard lk(mutex_);
      stop_ = true;
    }
    wake_.notify_one();
    thread_.join();  // the worker runs the pending job first
  } else {
    while (pump()) {
    }
  }
}

double Autosaver::tick(views::ViewContext& ctx, double now) {
  if (const uint64_t e = errors_.load(std::memory_order_relaxed); e != errorsShown_) {
    errorsShown_ = e;
    if (const std::string why = lastError(); !why.empty()) ctx.setStatus("Autosave failed: " + why);
  }
  const doc::Snapshot& cur = ctx.doc();
  const doc::Snapshot& saved = ctx.savedSnapshot();
  const bool on = ctx.viewPrefs().autosave;
  // Idle: nothing changed since the last tick and nothing is waiting.
  if (cur == seen_ && saved == savedSeen_ && dirtySince_ < 0 && on == on_) return -1.0;
  on_ = on;
  if (cur != seen_) {
    seen_ = cur;
    lastChange_ = now;
  }
  savedSeen_ = saved;
  if (!on || cur == saved) {  // nothing worth keeping: the saved file holds it (or autosave is off)
    if (exists_) discard();
    dirtySince_ = -1.0;
    return -1.0;
  }
  if (cur == written_) {
    dirtySince_ = -1.0;
    return -1.0;
  }
  if (dirtySince_ < 0) dirtySince_ = now;
  if (ctx.store().inGesture()) return -1.0;  // its commit (or cancel) presents a frame, which ticks again
  const double due = std::min(lastChange_ + policy_.quietSeconds, dirtySince_ + policy_.maxSeconds);
  if (now < due) return due - now;
  AutosaveMeta m;
  m.originDir = ctx.projectDir();
  if (m.originDir.empty()) m.recordingsDir = ctx.unsavedRecordingDir();
  m.name = ctx.projectName();
  submit(cur, ctx.store().view(), std::move(m));
  return -1.0;
}

void Autosaver::submit(doc::Snapshot p, doc::ViewState view, AutosaveMeta meta) {
  written_ = p;
  exists_ = true;
  dirtySince_ = -1.0;
  post(Job{JobKind::Write, std::move(p), std::move(view), std::move(meta)});
}

void Autosaver::discard() {
  written_ = nullptr;
  exists_ = false;
  post(Job{JobKind::Discard, nullptr, {}, {}});
}

void Autosaver::close() {
  written_ = nullptr;
  exists_ = false;
  post(Job{JobKind::Close, nullptr, {}, {}});
  flush();
}

void Autosaver::post(Job j) {
  std::optional<Job> replaced;  // released after the lock (it may hold the last reference to an old snapshot)
  {
    std::lock_guard lk(mutex_);
    replaced = std::exchange(pending_, std::move(j));
    if (threaded_ && !thread_.joinable()) thread_ = std::thread([this] { workerMain(); });
  }
  if (threaded_) wake_.notify_one();
}

bool Autosaver::pump() {
  std::optional<Job> j;
  {
    std::lock_guard lk(mutex_);
    j = std::exchange(pending_, std::nullopt);
  }
  if (!j) return false;
  run(*j);
  return true;
}

void Autosaver::flush() {
  if (!threaded_) {
    while (pump()) {
    }
    return;
  }
  std::unique_lock lk(mutex_);
  idle_.wait(lk, [this] { return !pending_ && !busy_; });
}

uint64_t Autosaver::writes() const {
  std::lock_guard lk(mutex_);
  return writes_;
}

std::string Autosaver::lastError() const {
  std::lock_guard lk(mutex_);
  return lastError_;
}

void Autosaver::workerMain() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  std::unique_lock lk(mutex_);
  for (;;) {
    wake_.wait(lk, [this] { return stop_ || pending_.has_value(); });
    if (pending_) {
      Job j = std::move(*pending_);
      pending_.reset();
      busy_ = true;
      lk.unlock();
      run(j);
      j = Job{};  // the snapshot goes before the lock is taken again
      lk.lock();
      busy_ = false;
      idle_.notify_all();
      continue;
    }
    if (stop_) return;
  }
}

void Autosaver::run(Job& j) {
  Status s = Ok{};
  switch (j.kind) {
    case JobKind::Write: {
      std::error_code ec;
      fs::create_directories(dir_, ec);
      s = doc::saveProject(utf8(dir_), *j.project, j.view);
      if (s) {
        j.meta.savedAt = static_cast<int64_t>(std::time(nullptr));
        j.meta.originMtime =
            j.meta.originDir.empty() ? 0 : fileUnixTime(u8path(j.meta.originDir) / u8path(doc::kProjectFileName));
        s = writeMeta(dir_, j.meta);
      }
      break;
    }
    case JobKind::Discard:
    case JobKind::Close: {
      std::string why;
      (void)removeUnsavedFolder(dir_, root_, &why);
      if (!why.empty()) s = ioError(why);
      if (j.kind == JobKind::Close) {
        std::error_code ec;
        fs::remove(root_, ec);  // only when empty: recordings or another file keep it
      }
      break;
    }
  }
  std::lock_guard lk(mutex_);
  if (s) {
    if (j.kind == JobKind::Write) ++writes_;
    lastError_.clear();
  } else {
    lastError_ = s.error().message;
    errors_.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace pq::app
