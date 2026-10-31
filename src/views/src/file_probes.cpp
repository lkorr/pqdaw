// T5.7: file probes for drops (pq/views/file_probes.h).

#include "pq/views/file_probes.h"

#include <algorithm>
#include <filesystem>

#include "pq/document/samples.h"
#include "pq/library/index.h"
#include "pq/media/hash.h"
#include "pq/media/probe.h"
#include "pq/views/view_context.h"

namespace pq::views {

namespace fs = std::filesystem;

namespace {

fs::path pathOf(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8Of(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

}  // namespace

ProbedFile::Kind fileKindByName(std::string_view path) noexcept {
  const size_t slash = path.find_last_of("/\\");
  const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const auto k = library::indexedKind(name);
  if (!k) return ProbedFile::Kind::Other;
  switch (*k) {
    case library::ItemKind::Sample: return ProbedFile::Kind::Audio;
    case library::ItemKind::Preset:
    case library::ItemKind::Device: return ProbedFile::Kind::Preset;
    case library::ItemKind::Tuning: return ProbedFile::Kind::Tuning;
    case library::ItemKind::Project: return ProbedFile::Kind::Project;
    case library::ItemKind::Clip: {
      // .pqclip is a library clip (T5.11); .mid / .midi are MIDI files.
      const size_t dot = name.find_last_of('.');
      std::string ext(dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1));
      for (char& c : ext) c = library::foldChar(c);
      return ext == "mid" || ext == "midi" ? ProbedFile::Kind::Midi : ProbedFile::Kind::Preset;
    }
    case library::ItemKind::Patch:
    case library::ItemKind::Count: break;
  }
  return ProbedFile::Kind::Other;
}

FileProbes::FileProbes(ViewContext& ctx) : ctx_(ctx) {}

FileProbes::~FileProbes() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  for (std::thread& t : threads_) t.join();
}

void FileProbes::startWorkers() {
  if (!threads_.empty()) return;
  const uint32_t hw = std::max(2u, std::thread::hardware_concurrency());
  const uint32_t n = std::clamp(hw / 2, 2u, 8u);
  for (uint32_t i = 0; i < n; ++i) threads_.emplace_back([this] { worker(); });
}

std::vector<ProbedFile> FileProbes::request(std::span<const std::string> paths) {
  std::vector<ProbedFile> out;
  out.reserve(paths.size());
  std::vector<std::string> preload;
  bool queued = false;
  {
    std::lock_guard lock(mutex_);
    for (const std::string& p : paths) {
      auto it = files_.find(p);
      if (it == files_.end()) {
        ProbedFile f;
        f.path = p;
        std::error_code ec;
        const fs::path fp = pathOf(p);
        const fs::file_status st = fs::status(fp, ec);
        f.absPath = doc::absoluteSamplePath(doc::storedSamplePath(p, ctx_.projectDir()), ctx_.projectDir());
        f.displayName = utf8Of(fp.stem());
        if (ec || !fs::exists(st)) {
          f.kind = ProbedFile::Kind::Missing;
          f.state = ProbedFile::State::Failed;
          f.error = "not found";
        } else if (fs::is_directory(st)) {
          f.kind = ProbedFile::Kind::Folder;
          f.state = ProbedFile::State::Ready;
        } else {
          f.kind = fileKindByName(p);
          if (f.kind == ProbedFile::Kind::Audio) {
            queue_.push_back(p);
            queued = true;
            preload.push_back(f.absPath);
          } else {
            f.state = ProbedFile::State::Ready;
          }
        }
        it = files_.emplace(p, std::move(f)).first;
      }
      out.push_back(it->second);
    }
  }
  if (queued) {
    startWorkers();
    cv_.notify_all();
  }
  if (const auto& pre = ctx_.samplePreloader())
    for (const std::string& abs : preload) pre(abs);
  return out;
}

std::vector<ProbedFile> FileProbes::snapshot(std::span<const std::string> paths) const {
  std::vector<ProbedFile> out;
  out.reserve(paths.size());
  std::lock_guard lock(mutex_);
  for (const std::string& p : paths) {
    const auto it = files_.find(p);
    if (it != files_.end()) {
      out.push_back(it->second);
    } else {
      ProbedFile f;
      f.path = p;
      out.push_back(std::move(f));
    }
  }
  return out;
}

bool FileProbes::ready(std::span<const std::string> paths) const {
  std::lock_guard lock(mutex_);
  for (const std::string& p : paths) {
    const auto it = files_.find(p);
    if (it == files_.end() || it->second.state == ProbedFile::State::Pending) return false;
  }
  return true;
}

bool FileProbes::wait(std::span<const std::string> paths, std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  return doneCv_.wait_for(lock, timeout, [&] {
    for (const std::string& p : paths) {
      const auto it = files_.find(p);
      if (it == files_.end() || it->second.state == ProbedFile::State::Pending) return false;
    }
    return true;
  });
}

void FileProbes::clear() {
  std::lock_guard lock(mutex_);
  queue_.clear();
  std::erase_if(files_, [](const auto& kv) { return kv.second.state != ProbedFile::State::Pending; });
}

void FileProbes::worker() {
  for (;;) {
    std::string path;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
      if (quit_) return;
      path = std::move(queue_.front());
      queue_.pop_front();
    }
    ProbedFile r;
    const fs::path fp = pathOf(path);
    auto info = media::probe(fp);
    Result<uint64_t> hash = info ? media::contentHash(fp) : Result<uint64_t>(Error{ErrorCode::Io, "", {}});
    bool post = false;
    {
      std::lock_guard lock(mutex_);
      const auto it = files_.find(path);
      if (it == files_.end()) continue;  // cleared meanwhile
      ProbedFile& f = it->second;
      if (!info) {
        f.state = ProbedFile::State::Failed;
        f.error = info.error().message;
      } else if (!hash) {
        f.state = ProbedFile::State::Failed;
        f.error = hash.error().message;
      } else if (info->frames == 0 || info->sampleRate == 0 || info->channels == 0) {
        f.state = ProbedFile::State::Failed;
        f.error = "no audio in the file";
      } else {
        f.frames = static_cast<int64_t>(info->frames);
        f.rate = static_cast<double>(info->sampleRate);
        f.channels = info->channels;
        f.size = info->identity.size;
        if (f.size == 0) {
          std::error_code ec;
          f.size = static_cast<uint64_t>(fs::file_size(fp, ec));
        }
        f.contentHash = *hash;
        f.bpm = info->bpm;
        f.state = ProbedFile::State::Ready;
      }
      if (!postPending_ && ctx_.uiPoster()) {
        postPending_ = true;
        post = true;
      }
    }
    doneCv_.notify_all();
    if (post)
      ctx_.uiPoster()([this, alive = std::weak_ptr<char>(life_)] {
        if (!alive.lock()) return;
        {
          std::lock_guard lock(mutex_);
          postPending_ = false;
        }
        if (listener_) listener_();
      });
  }
}

}  // namespace pq::views
