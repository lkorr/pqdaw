// The recording service (T6.7): pq/app/recording.h describes it.
#include "pq/app/recording.h"

#include <algorithm>
#include <cmath>
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

RecordingService::RecordingService(media::SamplePool* pool) : pool_(pool) {
  if (pool_ != nullptr) pool_->setDiskClient(&RecordingService::diskClient, this);
}

RecordingService::~RecordingService() {
  if (pool_ != nullptr) pool_->setDiskClient(nullptr, nullptr);  // waits for a pump in progress
  if (active_) (void)finish();
}

void RecordingService::bind(engine::Engine* e) {  // UI thread
  std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
    set_ = set;
    channels_ = std::move(channels);
  }
  engine_->recorder().setCapture(std::move(set));
}

Status RecordingService::begin(std::vector<RecordingTrack> tracks, double sampleRate) {  // UI thread
  if (active_) return Error{ErrorCode::InvalidArgument, "recording: a session is already running", {}};
  if (engine_ == nullptr) return Error{ErrorCode::InvalidArgument, "recording: no engine", {}};
  std::lock_guard lock(mutex_);
  (void)pumpLocked();  // segments of a session nobody recorded: discarded (running_ is false)
  writers_.clear();
  errors_.clear();
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
    if (Status st = w->wav.open(t.path, ch, static_cast<uint32_t>(std::lround(sampleRate)),
                                static_cast<uint64_t>(std::lround(sampleRate)));
        !st.ok()) {
      writers_.clear();
      return st;
    }
    for (uint32_t c = 0; c < ch; ++c)
      w->ringIndex.push_back(set_ ? set_->indexOf(static_cast<uint16_t>(t.input.firstChannel + c)) : -1);
    w->track = t;
    writers_.push_back(std::move(w));
  }
  tracks_ = std::move(tracks);
  rate_ = sampleRate;
  running_ = true;
  sessionKnown_ = false;
  passes_.clear();
  fileFrames_ = 0;
  dropped_ = 0;
  peakFill_ = 0;
  lastEngineEnd_ = lastSongEnd_ = INT64_MIN;
  notes_.clear();
  active_ = true;
  ++liveSession_;
  engine_->liveInput().setRecording(!noteSlots_.empty());
  if (pool_ != nullptr) pool_->engage();
  return Ok{};
}

void RecordingService::pollNotes() {  // UI thread
  if (engine_ == nullptr) return;
  engine::LiveNoteRecord r;
  std::lock_guard lock(mutex_);
  while (engine_->liveInput().popRecord(r))
    if (running_ && std::find(noteSlots_.begin(), noteSlots_.end(), r.slot) != noteSlots_.end()) notes_.push_back(r);
}

bool RecordingService::pump() {  // disk thread (or UI)
  std::lock_guard lock(mutex_);
  return pumpLocked();
}

void RecordingService::addRun(const engine::CaptureSegment& s, int64_t fileFrame) {  // mutex_ held
  const bool contiguous = !passes_.empty() && s.engineTime == lastEngineEnd_ && s.songSample == lastSongEnd_;
  if (!contiguous) {
    RecordingPass p;
    p.songStart = p.songEnd = s.songSample;
    p.engineStart = s.engineTime;
    passes_.push_back(p);
  }
  RecordingPass& p = passes_.back();
  p.songEnd = s.songSample + s.frames;
  if (s.audioLen != 0) {
    const int64_t a0 = s.songSample + s.audioFrom;
    if (p.audio1 == p.audio0) {
      p.audio0 = a0;
      p.file0 = fileFrame;
    }
    p.audio1 = a0 + s.audioLen;
  }
  lastEngineEnd_ = s.engineTime + s.frames;
  lastSongEnd_ = s.songSample + s.frames;
}

bool RecordingService::pumpLocked() {  // mutex_ held
  if (engine_ == nullptr) return false;
  engine::Recorder& rec = engine_->recorder();
  engine::CaptureSegment s;
  while (rec.popSegment(s)) {
    const uint32_t n = s.audioLen;
    const bool inRings = n != 0 && s.dropped == 0 && set_ && s.captureSet == set_->serial();
    const size_t nch = set_ ? set_->channels().size() : 0;
    if (inRings) {  // take the frames off the rings whatever happens to them
      if (scratch_.size() < nch * n) scratch_.resize(nch * n);
      for (size_t c = 0; c < nch; ++c) (void)set_->ring(c).pop(scratch_.data() + c * n, n);
    }
    if (!running_) continue;
    if (!sessionKnown_) {
      session_ = s.session;
      sessionKnown_ = true;
    }
    if (s.session != session_) continue;
    const int64_t fileFrame = fileFrames_;
    if (n != 0) {
      if (zeros_.size() < n) zeros_.assign(n, 0.0f);
      if (!inRings) dropped_ += n;
      for (auto& w : writers_) {
        const float* chans[2] = {zeros_.data(), zeros_.data()};
        for (size_t c = 0; c < w->ringIndex.size() && c < 2; ++c)
          if (inRings && w->ringIndex[c] >= 0) chans[c] = scratch_.data() + static_cast<size_t>(w->ringIndex[c]) * n;
        if (w->wav.isOpen())
          if (Status st = w->wav.write(chans, n); !st.ok()) {
            errors_.push_back(st.error().message);
            (void)w->wav.close();
          }
      }
      // The live waveform: min / max over the written channels per kPeakFrames frames.
      for (uint32_t f = 0; f < n;) {
        const uint32_t m = std::min(n - f, kPeakFrames - peakFill_);
        if (inRings)
          for (auto& w : writers_)
            for (int32_t idx : w->ringIndex) {
              if (idx < 0) continue;
              const float* x = scratch_.data() + static_cast<size_t>(idx) * n + f;
              const auto [lo, hi] = std::minmax_element(x, x + m);
              w->peakMin = std::min(w->peakMin, *lo);
              w->peakMax = std::max(w->peakMax, *hi);
            }
        peakFill_ += m;
        f += m;
        if (peakFill_ == kPeakFrames) {
          for (auto& w : writers_) {
            w->peaks.push_back(w->peakMin);
            w->peaks.push_back(w->peakMax);
            w->peakMin = w->peakMax = 0.0f;
          }
          peakFill_ = 0;
        }
      }
      fileFrames_ += n;
    }
    addRun(s, fileFrame);
  }
  return running_;
}

RecordingResult RecordingService::finish() {  // UI thread
  RecordingResult r;
  if (!active_) return r;
  pollNotes();
  if (engine_ != nullptr) engine_->liveInput().setRecording(false);
  {
    std::lock_guard lock(mutex_);
    (void)pumpLocked();
    for (auto& w : writers_)
      if (Status st = w->wav.close(); !st.ok()) errors_.push_back(st.error().message);
    r.tracks = std::move(tracks_);
    tracks_.clear();
    writers_.clear();
    running_ = false;
    r.session = session_;
    r.sampleRate = rate_;
    r.passes = passes_;
    r.fileFrames = fileFrames_;
    r.notes = std::move(notes_);
    notes_.clear();
    r.droppedFrames = dropped_;
    r.errors = errors_;
  }
  active_ = false;
  if (pool_ != nullptr) pool_->disengage();
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
  out.audioTracks.resize(writers_.size());
  out.peaks.resize(writers_.size());
  for (size_t i = 0; i < writers_.size(); ++i) {
    out.audioTracks[i] = writers_[i]->track.track;
    const std::vector<float>& src = writers_[i]->peaks;
    std::vector<float>& dst = out.peaks[i];
    if (dst.size() < src.size()) dst.insert(dst.end(), src.begin() + static_cast<ptrdiff_t>(dst.size()), src.end());
  }
  if (out.notes.size() < notes_.size()) out.notes.insert(out.notes.end(), notes_.begin() + static_cast<ptrdiff_t>(out.notes.size()), notes_.end());
}

}  // namespace pq::app
