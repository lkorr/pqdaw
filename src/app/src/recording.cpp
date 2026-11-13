// The recording service (T6.7): pq/app/recording.h describes it.
#include "pq/app/recording.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <system_error>

#include <windows.h>

namespace pq::app {

// T6.7b: the guarded delete of an unsaved recordings folder (see the header).
uint64_t removeUnsavedFolder(const std::filesystem::path& target, const std::filesystem::path& root, std::string* why) {
  namespace fs = std::filesystem;
  auto refuse = [why](std::string r) {
    if (why != nullptr) *why = std::move(r);
    return uint64_t{0};
  };
  auto parts = [](const fs::path& p) {
    size_t n = 0;
    for (const fs::path& e : p)
      if (!e.empty() && e != ".") ++n;
    return n;
  };
  if (!root.is_absolute() || !target.is_absolute()) return refuse("not an absolute path");
  fs::path r = root.lexically_normal(), t = target.lexically_normal();
  if (!r.has_filename()) r = r.parent_path();  // "C:/a/b/" → "C:/a/b"
  if (!t.has_filename()) t = t.parent_path();
  if (parts(r.relative_path()) < 2) return refuse("the root is too close to the drive root: " + r.string());
  const fs::path rel = t.lexically_relative(r);
  if (rel.empty() || parts(rel) == 0) return refuse("not inside the unsaved folder");
  for (const fs::path& e : rel)
    if (e == "..") return refuse("not inside the unsaved folder");
  // No link on the way down (a junction or symbolic link could lead anywhere).
  fs::path walk = r;
  for (const fs::path& e : rel) {
    walk /= e;
    const DWORD a = GetFileAttributesW(walk.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return refuse("");  // absent: nothing to delete
    if ((a & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return refuse("a link on the way: " + walk.string());
  }
  std::error_code ec;
  const std::uintmax_t n = fs::remove_all(t, ec);
  if (ec) {
    if (why != nullptr) *why = ec.message();
    return n == static_cast<std::uintmax_t>(-1) ? 0 : n;
  }
  if (why != nullptr) why->clear();
  return n;
}

bool processAlive(uint32_t pid, int64_t started) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (h == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;  // exists, but not ours to ask: leave it alone
  FILETIME created{}, exited{}, kernel{}, user{};
  DWORD code = 0;
  bool alive = GetExitCodeProcess(h, &code) != 0 && code == STILL_ACTIVE;
  if (alive && GetProcessTimes(h, &created, &exited, &kernel, &user) != 0) {
    const uint64_t t = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    const auto createdUnix = static_cast<int64_t>(t / 10000000ull) - 11644473600ll;  // FILETIME: 100 ns since 1601
    alive = started >= createdUnix - 5 && started <= createdUnix + 600;  // the session is named once the link starts
  }
  CloseHandle(h);
  return alive;
}

std::vector<StaleUnsavedSession> findStaleUnsaved(const std::filesystem::path& root, std::string_view own,
                                                  const std::function<bool(uint32_t, int64_t)>& alive) {
  namespace fs = std::filesystem;
  std::vector<StaleUnsavedSession> out;
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec) || it->is_symlink(ec)) continue;
    const std::string name = it->path().filename().string();
    if (name == own) continue;
    unsigned long pid = 0;
    long long started = 0;
    char tail = 0;
    if (std::sscanf(name.c_str(), "%lu-%lld%c", &pid, &started, &tail) != 2) continue;  // not a session folder
    if (alive ? alive(static_cast<uint32_t>(pid), started) : processAlive(static_cast<uint32_t>(pid), started)) continue;
    StaleUnsavedSession s;
    s.dir = it->path();
    s.started = started;
    std::error_code wec;
    for (fs::recursive_directory_iterator w(s.dir, fs::directory_options::none, wec), wend; !wec && w != wend;
         w.increment(wec)) {
      if (!w->is_regular_file(wec)) continue;
      std::string ext = w->path().extension().string();
      for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (ext != ".wav") continue;
      s.wavs.push_back(w->path());
      std::error_code sec;
      if (const std::uintmax_t n = w->file_size(sec); !sec) s.bytes += static_cast<uint64_t>(n);
    }
    // AS.2: the autosave of the document that session was editing (a missing or broken autosave.json: takes only).
    const fs::path autosaveDir = s.dir / fs::path(std::u8string(kAutosaveFolderName.begin(), kAutosaveFolderName.end()));
    if (std::error_code aec; fs::is_regular_file(autosaveDir / "project.pqd", aec))
      if (Result<AutosaveMeta> m = readAutosaveMeta(autosaveDir); m.ok() && autosaveSupersedesOrigin(*m))
        s.autosave = std::move(*m);
    if (s.wavs.empty() && !s.autosave) continue;
    std::sort(s.wavs.begin(), s.wavs.end());
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.started < b.started; });
  return out;
}

RecordingService::RecordingService() { startThread(); }

RecordingService::~RecordingService() {
  if (active_) (void)finish();
  stopThread();
}

void RecordingService::startThread() {
  if (thread_.joinable()) return;
  {
    std::lock_guard lk(wakeMutex_);
    stop_ = false;
  }
  thread_ = std::thread([this] { writerMain(); });
}

void RecordingService::stopThread() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard lk(wakeMutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void RecordingService::setWriterThread(bool on) {  // UI thread (tests)
  if (on) startThread();
  else stopThread();
}

void RecordingService::writerMain() {  // the writer thread
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  SetThreadDescription(GetCurrentThread(), L"pqdaw recording");
  // A 2 ms poll needs a high-resolution timer (a condition variable's timeout rounds up to the ~15.6 ms system tick).
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  std::unique_lock lk(wakeMutex_);
  while (!stop_) {
    if (!pumping_.load(std::memory_order_acquire)) {  // idle: asleep until a session begins
      wake_.wait(lk);
      continue;
    }
    lk.unlock();
    (void)pump();
    if (timer != nullptr) {
      LARGE_INTEGER due;
      due.QuadPart = -20000;  // 2 ms, relative, in 100 ns units
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, INFINITE);
      lk.lock();
    } else {
      lk.lock();
      if (!stop_) wake_.wait_for(lk, std::chrono::milliseconds(2));
    }
  }
  lk.unlock();
  if (timer != nullptr) CloseHandle(timer);
}

void RecordingService::bind(engine::Engine* e) {  // UI thread
  std::lock_guard lock(ioMutex_);
  engine_ = e;
  if (engine_ != nullptr && set_) engine_->recorder().setCapture(set_);
}

void RecordingService::prepareChannels(std::vector<uint16_t> channels) {  // UI thread
  wantChannels_ = channels;
  if (channels.empty() || channels == channels_ || active_ || engine_ == nullptr) return;
  const double rate = engine_->config().sampleRate;
  auto set = std::make_shared<engine::CaptureSet>(channels, static_cast<uint32_t>(std::ceil(kRingSeconds * rate)),
                                                  nextSerial_++);
  {
    std::lock_guard lock(ioMutex_);
    set_ = set;
  }
  channels_ = std::move(channels);
  engine_->recorder().setCapture(std::move(set));
}

Status RecordingService::begin(std::vector<RecordingTrack> tracks, double sampleRate) {  // UI thread
  if (active_) return Error{ErrorCode::InvalidArgument, "recording: a session is already running", {}};
  if (engine_ == nullptr) return Error{ErrorCode::InvalidArgument, "recording: no engine", {}};
  std::vector<doc::TrackId> audioTracks;
  {
    std::lock_guard io(ioMutex_);
    (void)pumpLocked();  // segments of a session nobody recorded: discarded (running_ is false)
    writers_.clear();
    noteSlots_.clear();
    for (RecordingTrack& t : tracks) {
      if (!t.audio) {
        noteSlots_.push_back(t.slot);
        continue;
      }
      auto w = std::make_unique<Writer>();
      std::error_code ec;
      std::filesystem::create_directories(t.path.parent_path(), ec);
      const uint32_t ch = std::max<uint32_t>(1, std::min<uint32_t>(t.input.channels, 2));
      w->wav.enableRf64();  // M6 review M8: a take past 4 GiB goes on as RF64
      if (Status st = w->wav.open(t.path, ch, static_cast<uint32_t>(std::lround(sampleRate)),
                                  static_cast<uint64_t>(std::lround(sampleRate)));
          !st.ok()) {
        writers_.clear();
        return st;
      }
      for (uint32_t c = 0; c < ch; ++c)
        w->ringIndex.push_back(set_ ? set_->indexOf(static_cast<uint16_t>(t.input.firstChannel + c)) : -1);
      w->track = t;
      audioTracks.push_back(t.track);
      writers_.push_back(std::move(w));
    }
    tracks_ = std::move(tracks);
    rate_ = sampleRate;
    running_ = true;
    sessionKnown_ = false;
    peakFill_ = 0;
    lastEngineEnd_ = lastSongEnd_ = INT64_MIN;
    newPeaks_.assign(writers_.size(), {});
    std::lock_guard lock(mutex_);
    passes_.clear();
    fileFrames_ = 0;
    dropped_ = 0;
    errors_.clear();
    audioTracks_ = std::move(audioTracks);
    peaks_.assign(audioTracks_.size(), {});
    notes_.clear();
  }
  errorsTaken_ = 0;
  active_ = true;
  ++liveSession_;
  engine_->liveInput().setRecording(!noteSlots_.empty());
  {
    std::lock_guard lk(wakeMutex_);
    pumping_.store(true, std::memory_order_release);
  }
  wake_.notify_all();
  return Ok{};
}

void RecordingService::pollNotes() {  // UI thread
  if (engine_ == nullptr) return;
  engine::LiveNoteRecord r;
  std::lock_guard lock(mutex_);
  while (engine_->liveInput().popRecord(r))
    if (active_ && std::find(noteSlots_.begin(), noteSlots_.end(), r.slot) != noteSlots_.end()) notes_.push_back(r);
}

std::vector<std::string> RecordingService::takeErrors() {  // UI thread
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  for (; errorsTaken_ < errors_.size(); ++errorsTaken_) out.push_back(errors_[errorsTaken_]);
  return out;
}

bool RecordingService::pump() {  // writer thread (or UI)
  std::lock_guard io(ioMutex_);
  return pumpLocked();
}

void RecordingService::addRun(const engine::CaptureSegment& s) {  // ioMutex_ held
  const bool contiguous = !passes_.empty() && s.engineTime == lastEngineEnd_ && s.songSample == lastSongEnd_;
  {
    std::lock_guard lock(mutex_);
    if (!contiguous) {
      RecordingPass p;
      p.songStart = p.songEnd = s.songSample;
      p.engineStart = s.engineTime;
      passes_.push_back(p);
    }
    passes_.back().songEnd = s.songSample + s.frames;
  }
  lastEngineEnd_ = s.engineTime + s.frames;
  lastSongEnd_ = s.songSample + s.frames;
}

void RecordingService::writeFrames(bool fromRings, uint32_t stride, uint32_t from, uint32_t n) {  // ioMutex_ held
  if (n == 0) return;
  if (zeros_.size() < n) zeros_.assign(n, 0.0f);
  const size_t nch = set_ ? set_->channels().size() : 0;
  for (size_t i = 0; i < writers_.size(); ++i) {
    Writer& w = *writers_[i];
    const float* chans[2] = {zeros_.data(), zeros_.data()};
    for (size_t c = 0; c < w.ringIndex.size() && c < 2; ++c)
      if (fromRings && w.ringIndex[c] >= 0 && static_cast<size_t>(w.ringIndex[c]) < nch)
        chans[c] = scratch_.data() + static_cast<size_t>(w.ringIndex[c]) * stride + from;
    if (w.wav.isOpen())
      if (Status st = w.wav.write(chans, n); !st.ok()) {
        {
          std::lock_guard lock(mutex_);
          errors_.push_back(st.error().message);  // M8: the UI reports it at its next poll
        }
        (void)w.wav.close();  // the file keeps what was written (its header says so)
      }
  }
  // The live waveform: min / max over the written channels per kPeakFrames frames.
  for (uint32_t f = 0; f < n;) {
    const uint32_t m = std::min(n - f, kPeakFrames - peakFill_);
    if (fromRings)
      for (auto& w : writers_)
        for (int32_t idx : w->ringIndex) {
          if (idx < 0 || static_cast<size_t>(idx) >= nch) continue;
          const float* x = scratch_.data() + static_cast<size_t>(idx) * stride + from + f;
          const auto [lo, hi] = std::minmax_element(x, x + m);
          w->peakMin = std::min(w->peakMin, *lo);
          w->peakMax = std::max(w->peakMax, *hi);
        }
    peakFill_ += m;
    f += m;
    if (peakFill_ == kPeakFrames) {
      for (size_t i = 0; i < writers_.size(); ++i) {
        newPeaks_[i].push_back(writers_[i]->peakMin);
        newPeaks_[i].push_back(writers_[i]->peakMax);
        writers_[i]->peakMin = writers_[i]->peakMax = 0.0f;
      }
      peakFill_ = 0;
    }
  }
}

void RecordingService::addAudio(const engine::CaptureSegment& s, bool inRings) {  // ioMutex_ held
  // The pass that played these frames (passes_ is written only under ioMutex_: read here without mutex_).
  size_t k = passes_.size();
  while (k > 0 && passes_[k - 1].engineStart > s.engineTime) --k;
  if (k == 0) return;  // played before the session's first run: not recorded
  const RecordingPass& p = passes_[k - 1];
  if (s.engineTime >= p.engineStart + (p.songEnd - p.songStart)) return;  // not inside that pass
  for (size_t j = k; j < passes_.size(); ++j)
    if (passes_[j].audio1 > passes_[j].audio0) return;  // a later pass has frames already (never: played order)
  const bool has = p.audio1 > p.audio0;
  int64_t skip = 0, gap = 0;
  if (has && s.songSample < p.audio1) skip = std::min<int64_t>(s.frames, p.audio1 - s.songSample);  // placed already
  if (has && s.songSample > p.audio1) gap = s.songSample - p.audio1;  // the offset shrank: silence keeps the timing
  for (int64_t g = gap; g > 0;) {
    const auto m = static_cast<uint32_t>(std::min<int64_t>(g, 1 << 16));
    writeFrames(false, 0, 0, m);
    g -= m;
  }
  const auto n = static_cast<uint32_t>(s.frames - skip);
  writeFrames(inRings, s.frames, static_cast<uint32_t>(skip), n);
  std::lock_guard lock(mutex_);
  RecordingPass& q = passes_[k - 1];
  if (!has) {
    q.audio0 = s.songSample;
    q.file0 = fileFrames_;
  }
  if (n != 0) q.audio1 = s.songSample + s.frames;
  fileFrames_ += gap + n;
  dropped_ += static_cast<uint64_t>(gap) + (inRings ? 0u : n);
}

bool RecordingService::pumpLocked() {  // ioMutex_ held
  if (engine_ == nullptr) return false;
  engine::Recorder& rec = engine_->recorder();
  engine::CaptureSegment s;
  auto inSession = [&] {
    if (!running_) return false;
    if (!sessionKnown_) {
      session_ = s.session;
      sessionKnown_ = true;
    }
    return s.session == session_;
  };
  while (rec.popSegment(s)) {
    if (s.kind == engine::CaptureSegment::Run) {
      if (inSession()) addRun(s);
      continue;
    }
    const uint32_t n = s.frames;
    const bool inRings = n != 0 && s.dropped == 0 && set_ && s.captureSet == set_->serial();
    if (inRings) {  // take the frames off the rings whatever happens to them
      const size_t nch = set_->channels().size();
      if (scratch_.size() < nch * n) scratch_.resize(nch * n);
      for (size_t c = 0; c < nch; ++c) (void)set_->ring(c).pop(scratch_.data() + c * n, n);
    }
    if (n != 0 && inSession()) addAudio(s, inRings);
  }
  bool any = false;
  for (const auto& v : newPeaks_) any = any || !v.empty();
  if (any) {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < newPeaks_.size() && i < peaks_.size(); ++i) {
      peaks_[i].insert(peaks_[i].end(), newPeaks_[i].begin(), newPeaks_[i].end());
      newPeaks_[i].clear();
    }
  }
  return running_;
}

RecordingResult RecordingService::finish() {  // UI thread
  RecordingResult r;
  if (!active_) return r;
  pollNotes();
  if (engine_ != nullptr) engine_->liveInput().setRecording(false);
  pumping_.store(false, std::memory_order_release);
  {
    std::lock_guard io(ioMutex_);
    (void)pumpLocked();
    for (auto& w : writers_) {
      for (RecordingTrack& t : tracks_)
        if (t.audio && t.path == w->track.path) t.frames = static_cast<int64_t>(w->wav.framesWritten());
      if (Status st = w->wav.close(); !st.ok()) {
        std::lock_guard lock(mutex_);
        errors_.push_back(st.error().message);
      }
    }
    r.tracks = std::move(tracks_);
    tracks_.clear();
    writers_.clear();
    running_ = false;
    r.session = session_;
    r.sampleRate = rate_;
    std::lock_guard lock(mutex_);
    r.passes = passes_;
    r.fileFrames = fileFrames_;
    r.notes = std::move(notes_);
    notes_.clear();
    r.droppedFrames = dropped_;
    for (size_t i = errorsTaken_; i < errors_.size(); ++i) r.errors.push_back(errors_[i]);
    errorsTaken_ = errors_.size();
  }
  active_ = false;
  if (wantChannels_ != channels_) prepareChannels(wantChannels_);  // a change that waited for the session's end
  return r;
}

void RecordingService::live(Live& out) const {  // UI thread
  std::lock_guard lock(mutex_);
  if (out.session != liveSession_) {
    out = Live{};
    out.session = liveSession_;
  }
  out.passes = passes_;
  out.fileFrames = fileFrames_;
  out.audioTracks = audioTracks_;
  out.peaks.resize(peaks_.size());
  for (size_t i = 0; i < peaks_.size(); ++i) {
    const std::vector<float>& src = peaks_[i];
    std::vector<float>& dst = out.peaks[i];
    if (dst.size() < src.size()) dst.insert(dst.end(), src.begin() + static_cast<ptrdiff_t>(dst.size()), src.end());
  }
  if (out.notes.size() < notes_.size()) out.notes.insert(out.notes.end(), notes_.begin() + static_cast<ptrdiff_t>(out.notes.size()), notes_.end());
}

}  // namespace pq::app
