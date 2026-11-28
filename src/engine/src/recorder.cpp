// The recorder (T6.7): pq/engine/recorder.h describes it.
#include "pq/engine/recorder.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace pq::engine {

FloatRing::FloatRing(uint32_t capacityPow2)
    : data_(std::bit_ceil(std::max(capacityPow2, 2u))), mask_(std::bit_ceil(std::max(capacityPow2, 2u)) - 1) {}

bool FloatRing::push(const float* x, uint32_t n) noexcept {  // RT-safe
  if (n > space()) return false;
  const uint64_t head = head_.load(std::memory_order_relaxed);
  const uint32_t at = static_cast<uint32_t>(head) & mask_;
  const uint32_t first = std::min(n, capacity() - at);
  float* d = data_.data();
  if (x != nullptr) {
    std::memcpy(d + at, x, sizeof(float) * first);
    std::memcpy(d, x + first, sizeof(float) * (n - first));
  } else {  // a channel the device does not have: silence
    std::memset(d + at, 0, sizeof(float) * first);
    std::memset(d, 0, sizeof(float) * (n - first));
  }
  head_.store(head + n, std::memory_order_release);
  return true;
}

bool FloatRing::pop(float* out, uint32_t n) noexcept {  // RT-safe
  if (n > available()) return false;
  const uint64_t tail = tail_.load(std::memory_order_relaxed);
  const uint32_t at = static_cast<uint32_t>(tail) & mask_;
  const uint32_t first = std::min(n, capacity() - at);
  const float* d = data_.data();
  std::memcpy(out, d + at, sizeof(float) * first);
  std::memcpy(out + first, d, sizeof(float) * (n - first));
  tail_.store(tail + n, std::memory_order_release);
  return true;
}

CaptureSet::CaptureSet(std::span<const uint16_t> channels, uint32_t ringFrames, uint32_t serial)
    : CaptureSet(channels, {}, ringFrames, serial) {}

CaptureSet::CaptureSet(std::span<const uint16_t> channels, std::span<const uint32_t> sources, uint32_t ringFrames,
                       uint32_t serial)
    : channels_(channels.begin(), channels.end()), sources_(sources.begin(), sources.end()), serial_(serial) {
  const uint32_t cap = std::bit_ceil(std::max(ringFrames, 1024u));
  const size_t n = channels_.size() + 2 * sources_.size();
  rings_.reserve(n);
  for (size_t i = 0; i < n; ++i) rings_.push_back(std::make_unique<FloatRing>(cap));
  offsets_ = std::make_unique<std::atomic<uint32_t>[]>(std::max<size_t>(sources_.size(), 1));
  if (!sources_.empty()) history_ = AlignedBuffer<float>(n * kHistoryFrames);  // RT.2 (zeros)
}

int32_t CaptureSet::indexOf(uint16_t ch) const noexcept {
  for (size_t i = 0; i < channels_.size(); ++i)
    if (channels_[i] == ch) return static_cast<int32_t>(i);
  return -1;
}

int32_t CaptureSet::indexOfSource(uint32_t source, uint16_t c) const noexcept {
  for (size_t j = 0; j < sources_.size(); ++j)
    if (sources_[j] == source && c < 2) return static_cast<int32_t>(channels_.size() + 2 * j + c);
  return -1;
}

Recorder::Recorder() : segments_(kSegmentRing) {}

Recorder::~Recorder() = default;

template <class T>
void Recorder::publish(Published<T>& p, std::shared_ptr<T> next) {  // UI thread
  if (p.current == next) return;
  p.ptr.store(next.get(), std::memory_order_release);
  const uint64_t g = gen_.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (p.current) p.retired.emplace_back(g, std::move(p.current));
  p.current = std::move(next);
}

template <class T>
void Recorder::collectOf(Published<T>& p, uint64_t seen, bool all) {  // UI thread
  std::erase_if(p.retired, [&](const auto& r) { return all || r.first <= seen; });
}

void Recorder::setCapture(std::shared_ptr<CaptureSet> set) { publish(capture_, std::move(set)); }

void Recorder::setMonitors(std::shared_ptr<const MonitorImage> image) { publish(monitors_, std::move(image)); }

void Recorder::collect(bool audioStopped) {
  const uint64_t seen = seen_.load(std::memory_order_acquire);
  collectOf(capture_, seen, audioStopped);
  collectOf(monitors_, seen, audioStopped);
}

void Recorder::beginBlock(const AudioBlock& device) noexcept {  // RT-safe
  // The images loaded from here on are at least this generation's: the ones replaced before it are no longer read.
  seen_.store(gen_.load(std::memory_order_acquire), std::memory_order_release);
  in_ = device.numIn != 0 ? device.in : nullptr;
  numInNow_ = in_ != nullptr ? std::min(device.numIn, kMaxChannels) : 0u;
  blockTime_ = device.frameTime;
  if (numIn_.load(std::memory_order_relaxed) != numInNow_) numIn_.store(numInNow_, std::memory_order_relaxed);
}

void Recorder::pushPiece(CaptureSet* set, int64_t engine, int64_t song, int64_t n, int64_t off,
                         uint32_t session) noexcept {  // RT-safe
  CaptureSegment s;
  s.kind = CaptureSegment::Audio;
  s.engineTime = engine;
  s.songSample = song;
  s.frames = static_cast<uint32_t>(n);
  s.session = session;
  s.captureSet = set->serial();
  if (segments_.sizeApprox() >= segments_.capacity()) {  // no room for the segment: the piece is lost whole
    bump(stats_.droppedSegments);
    bump(stats_.droppedFrames, s.frames);
    dropped_.fetch_add(s.frames, std::memory_order_relaxed);
    return;
  }
  const std::span<const uint16_t> chans = set->channels();
  const size_t rings = set->rings();
  bool room = true;
  for (size_t i = 0; i < rings && room; ++i) room = set->ring(i).space() >= s.frames;
  if (room && set->sources().empty()) {
    const int64_t at = engine + off;  // captured now: inside this device block
    for (size_t i = 0; i < chans.size(); ++i) (void)set->ring(i).push(input(chans[i], at), s.frames);
    bump(stats_.capturedFrames, s.frames);
  } else if (room) {
    // RT.2: every ring from its history, at the engine time its own offset puts the played frames.
    constexpr int64_t kH = CaptureSet::kHistoryFrames;
    for (size_t i = 0; i < rings; ++i) {
      const int64_t at = engine + ringOffset(*set, i, off);
      const float* h = set->history(i);
      FloatRing& r = set->ring(i);
      if (off - ringOffset(*set, i, off) + blockFrames_ > kH) {  // further back than the history holds: silence
        (void)r.push(nullptr, s.frames);
        continue;
      }
      const auto from = static_cast<uint32_t>(at & (kH - 1));
      const uint32_t first = std::min<uint32_t>(s.frames, static_cast<uint32_t>(kH) - from);
      (void)r.push(h + from, first);
      if (first < s.frames) (void)r.push(h, s.frames - first);
    }
    bump(stats_.capturedFrames, s.frames);
  } else {
    s.dropped = s.frames;
    bump(stats_.droppedFrames, s.frames);
    dropped_.fetch_add(s.frames, std::memory_order_relaxed);
  }
  (void)segments_.tryPush(s);
  bump(stats_.segments);
}

void Recorder::capture(const Timeline& tl, int64_t time, uint32_t frames, uint32_t pdc) noexcept {  // RT-safe
  if (!tl.recording()) return;
  const uint32_t session = tl.recordSession();
  if (session != playedSession_) {  // a new session: nothing played before it is recorded
    playedSession_ = session;
    playedCount_ = 0;
    attributed_ = INT64_MIN;
    // M6 review M7: the device latencies stay what they were when the session started. A WASAPI capture device's
    // measured round trip follows clock drift and re-primes; following it mid-take would jump the placement (a gap or
    // a repeat in the file) where one constant offset for the whole file is what the commit assumes.
    sessionIn_ = inLatency_.load(std::memory_order_relaxed);
    sessionOut_ = outLatency_.load(std::memory_order_relaxed);
    sessionSrcFrozen_ = false;  // RT.2: the sources' offsets are taken at the session's first captureSources
  }
  // The runs: the session's time map, and the played history (contiguous runs merged).
  for (const TransportRun& r : tl.runs()) {
    if (r.kind != TransportRun::Song || r.len == 0) continue;
    CaptureSegment s;
    s.engineTime = time + r.frame;
    s.songSample = r.pos;
    s.frames = r.len;
    s.session = session;
    if (segments_.sizeApprox() >= segments_.capacity()) {
      bump(stats_.droppedSegments);  // the run is lost to the time map
    } else {
      (void)segments_.tryPush(s);
      bump(stats_.segments);
    }
    Played* last = playedCount_ != 0 ? &played_[(playedHead_ + kPlayedRuns - 1) % kPlayedRuns] : nullptr;
    if (last != nullptr && last->engine + last->len == s.engineTime && last->song + last->len == s.songSample) {
      last->len += r.len;
    } else {
      played_[playedHead_] = Played{s.engineTime, s.songSample, r.len};
      playedHead_ = (playedHead_ + 1) % kPlayedRuns;
      playedCount_ = std::min(playedCount_ + 1, kPlayedRuns);  // full: the oldest goes
    }
  }
  // This block's input frames, on what was played `off` frames earlier (M6 review M6: across a wrap too). RT.2: a set
  // with internal sources places after the units (captureSources).
  CaptureSet* set = capture_.ptr.load(std::memory_order_acquire);
  if (set != nullptr && !set->sources().empty()) return;
  const int64_t off = std::max<int64_t>(0, int64_t{sessionIn_} + sessionOut_ + pdc + manual_.load(std::memory_order_relaxed));
  place(tl, set, time, frames, off, session);
}

int64_t Recorder::ringOffset(const CaptureSet& set, size_t i, int64_t off) const noexcept {  // RT-safe
  const size_t nch = set.channels().size();
  if (i < nch) return devOff_;
  (void)off;
  const size_t j = (i - nch) / 2;
  return j < kMaxSources ? int64_t{sessionSrc_[j]} : 0;
}

void Recorder::captureSources(const Timeline& tl, int64_t time, uint32_t frames, uint32_t pdc) noexcept {  // RT-safe
  if (!tl.recording()) return;
  CaptureSet* set = capture_.ptr.load(std::memory_order_acquire);
  if (set == nullptr || set->sources().empty()) return;
  const std::span<const uint16_t> chans = set->channels();
  const std::span<const uint32_t> srcs = set->sources();
  if (!sessionSrcFrozen_) {  // M6 review M7's rule for the sources too: the offsets of the session's start
    for (size_t j = 0; j < srcs.size() && j < kMaxSources; ++j) sessionSrc_[j] = set->sourceOffset(j);
    sessionSrcFrozen_ = true;
  }
  // Every ring's frames of this image block into its history (engine time modulo its size).
  constexpr uint32_t kH = CaptureSet::kHistoryFrames;
  auto keep = [&](size_t ring, const float* x) noexcept {
    float* h = set->history(ring);
    const auto at = static_cast<uint32_t>(time & (kH - 1));
    const uint32_t first = std::min(frames, kH - at);
    if (x != nullptr) {
      std::memcpy(h + at, x, sizeof(float) * first);
      std::memcpy(h, x + first, sizeof(float) * (frames - first));
    } else {
      std::memset(h + at, 0, sizeof(float) * first);
      std::memset(h, 0, sizeof(float) * (frames - first));
    }
  };
  for (size_t i = 0; i < chans.size(); ++i) keep(i, input(chans[i], time));
  for (size_t j = 0; j < srcs.size(); ++j)
    for (uint16_t c = 0; c < 2; ++c)
      keep(chans.size() + 2 * j + c, sourceFn_ != nullptr ? sourceFn_(sourceUser_, srcs[j], c) : nullptr);
  // One session offset: the largest of the rings' (each ring is read back at its own).
  devOff_ = std::max<int64_t>(0, int64_t{sessionIn_} + sessionOut_ + pdc + manual_.load(std::memory_order_relaxed));
  blockFrames_ = frames;
  int64_t off = chans.empty() ? 0 : devOff_;
  for (size_t j = 0; j < srcs.size() && j < kMaxSources; ++j) off = std::max<int64_t>(off, sessionSrc_[j]);
  place(tl, set, time, frames, off, tl.recordSession());
}

void Recorder::place(const Timeline& tl, CaptureSet* set, int64_t time, uint32_t frames, int64_t off,
                     uint32_t session) noexcept {  // RT-safe
  const int64_t from = std::max(time - off, attributed_);
  const int64_t to = time + static_cast<int64_t>(frames) - off;
  attributed_ = std::max(attributed_, to);
  if (set == nullptr || from >= to) return;
  const RecordWindow w = tl.recordWindow();
  const int64_t lo = w.punch ? w.in : INT64_MIN;
  const int64_t hi = w.punch ? w.out : INT64_MAX;
  // Oldest first (played order). Runs that ended before `from` are never needed again.
  while (playedCount_ != 0) {
    const Played& p = played_[(playedHead_ + kPlayedRuns - playedCount_) % kPlayedRuns];
    if (p.engine + p.len > from) break;
    --playedCount_;
  }
  for (uint32_t k = playedCount_; k > 0; --k) {
    const Played& p = played_[(playedHead_ + kPlayedRuns - k) % kPlayedRuns];
    if (p.engine >= to) break;
    const int64_t e0 = std::max(from, p.engine), e1 = std::min(to, p.engine + p.len);
    if (e0 >= e1) continue;
    const int64_t s0 = p.song + (e0 - p.engine);
    const int64_t a = std::max(s0, lo), b = std::min(s0 + (e1 - e0), hi);
    if (a >= b) continue;
    pushPiece(set, e0 + (a - s0), a, b - a, off, session);
  }
}

bool Recorder::monitor(const Timeline& tl, std::span<float> params) noexcept {  // RT-safe
  const MonitorImage* m = monitors_.ptr.load(std::memory_order_acquire);
  monitoring_ = false;
  if (m == nullptr || m->entries.empty()) return false;
  const bool idleTransport = !tl.playing() || tl.recording();
  const ClipPlayer& cp = tl.clipPlayer();
  const uint8_t* active = cp.active();
  bool changed = false;
  for (const MonitorEntry& e : m->entries) {
    if (e.param >= params.size()) continue;
    float v = 0.0f;
    switch (e.mode) {
      case MonitorEntry::In: v = 1.0f; break;
      case MonitorEntry::Off: v = 0.0f; break;
      case MonitorEntry::Auto: {
        const bool clip = e.clipSlot < cp.numSlots() && active != nullptr && active[e.clipSlot] != 0;
        v = e.armed && (idleTransport || !clip) ? 1.0f : 0.0f;
        break;
      }
    }
    monitoring_ = monitoring_ || v != 0.0f;
    if (params[e.param] != v) {
      params[e.param] = v;
      changed = true;
      bump(stats_.monitorWrites);
    }
  }
  return changed;
}

}  // namespace pq::engine
