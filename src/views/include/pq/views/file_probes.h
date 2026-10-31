#pragma once

// File probes for drops (T5.7): what a dragged file is, learnt while it hovers, off the UI thread.
//
// A drop target asks for a payload's paths on the first dragOver (request()); a folder and the file kind are decided at
// once (one attribute read, the extension), audio files are probed (media::probe: headers only) and hashed (the
// document's content hash, ≤ 192 KiB read) by a small pool of worker threads, in parallel, so a 100-file drop is probed
// in the time of a few files. The first time an audio file is seen it is also handed to the sample preloader (the app:
// the playback link loads it into its sample pool, so a resident file plays as soon as Play is pressed after the drop).
// Results come back through the context's UI poster (setUiPoster): the drop target repaints its ghosts at their probed
// lengths. Without a poster (tests, scenes) the workers still run; callers poll ready() or block in wait().
//
// Not RT code. UI thread, except the workers.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pq::views {

class ViewContext;

struct ProbedFile {
  enum class Kind : uint8_t {
    Missing,   // no such file or folder
    Folder,
    Audio,     // a file the media probe reads (by extension; Failed if its headers don't parse)
    Midi,      // .mid / .midi: import arrives with M11
    Preset,    // .pqpreset / .pqdevice: dropped on a device chain (T5.11)
    Tuning,    // .pqtuning / .scl
    Project,   // .pqd
    Other,
  };
  enum class State : uint8_t { Pending, Ready, Failed };
  std::string path;     // as requested
  std::string absPath;  // the absolute path as the document resolves it (doc::absoluteSamplePath form)
  Kind kind = Kind::Other;
  State state = State::Pending;
  std::string error;    // Failed: why
  // Audio, once Ready (the fields of a doc::SampleRef).
  int64_t frames = 0;
  double rate = 0;
  uint32_t channels = 0;
  uint64_t size = 0;
  uint64_t contentHash = 0;
  double bpm = 0;  // acid chunk or file name (0: unknown)
  std::string displayName;  // the file name without its extension
  double seconds() const noexcept { return rate > 0 ? static_cast<double>(frames) / rate : 0.0; }
};

// The kind of a path by its extension (no file access; a folder is not recognised here).
ProbedFile::Kind fileKindByName(std::string_view path) noexcept;

class FileProbes {
public:
  explicit FileProbes(ViewContext& ctx);
  ~FileProbes();  // joins the workers
  FileProbes(const FileProbes&) = delete;
  FileProbes& operator=(const FileProbes&) = delete;

  // Starts probing the paths not seen yet (and preloads new audio files); returns a snapshot of each, in order.
  std::vector<ProbedFile> request(std::span<const std::string> paths);
  // Snapshots without starting anything (Pending for unknown paths).
  std::vector<ProbedFile> snapshot(std::span<const std::string> paths) const;
  // Every path has a result (Ready or Failed; folders and non-audio kinds are immediate).
  bool ready(std::span<const std::string> paths) const;
  // Blocks until ready() or the timeout; returns ready().
  bool wait(std::span<const std::string> paths, std::chrono::milliseconds timeout) const;
  // Called on the UI thread (through the context's poster) whenever results arrived. One listener (the active target).
  void setListener(std::function<void()> fn) { listener_ = std::move(fn); }
  // Forgets every result (the next request probes again: the files may have changed).
  void clear();

  uint32_t workers() const noexcept { return static_cast<uint32_t>(threads_.size()); }

private:
  void worker();
  void startWorkers();

  ViewContext& ctx_;
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;      // work for the workers
  mutable std::condition_variable doneCv_;  // a result arrived
  std::unordered_map<std::string, ProbedFile> files_;
  std::deque<std::string> queue_;
  std::vector<std::thread> threads_;
  bool quit_ = false;
  bool postPending_ = false;  // a UI notification is queued (results coalesce into one)
  std::function<void()> listener_;
  std::shared_ptr<char> life_ = std::make_shared<char>();
};

}  // namespace pq::views
