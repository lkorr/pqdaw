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
    : channels_(channels.begin(), channels.end()), serial_(serial) {
  const uint32_t cap = std::bit_ceil(std::max(ringFrames, 1024u));
  rings_.reserve(channels_.size());
  for (size_t i = 0; i < channels_.size(); ++i) rings_.push_back(std::make_unique<FloatRing>(cap));
}

int32_t CaptureSet::indexOf(uint16_t ch) const noexcept {
  for (size_t i = 0; i < channels_.size(); ++i)
    if (channels_[i] == ch) return static_cast<int32_t>(i);
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

void Recorder::capture(const Timeline& tl, int64_t time, uint32_t frames, uint32_t pdc) noexcept {  // RT-safe
  (void)frames;
  if (!tl.recording()) return;
  CaptureSet* set = capture_.ptr.load(std::memory_order_acquire);
  const int64_t off = latencyOffset(pdc);
  const RecordWindow w = tl.recordWindow();
  const int64_t lo = w.punch ? w.in + off : INT64_MIN;
  const int64_t hi = w.punch ? w.out + off : INT64_MAX;
  for (const TransportRun& r : tl.runs()) {
    if (r.kind != TransportRun::Song || r.len == 0) continue;
    CaptureSegment s;
    s.engineTime = time + r.frame;
    s.songSample = r.pos;
    s.frames = r.len;
    s.session = tl.recordSession();
    const int64_t len = r.len;
    const int64_t a = std::clamp<int64_t>(lo == INT64_MIN ? 0 : lo - r.pos, 0, len);
    const int64_t b = std::clamp<int64_t>(hi == INT64_MAX ? len : hi - r.pos, a, len);
    s.audioFrom = static_cast<uint32_t>(a);
    if (set != nullptr && b > a) {
      s.audioLen = static_cast<uint32_t>(b - a);
      s.captureSet = set->serial();
    }
    if (segments_.sizeApprox() >= segments_.capacity()) {  // no room for the segment: the run is lost whole
      ++stats_.droppedSegments;
      if (s.audioLen != 0) {
        stats_.droppedFrames += s.audioLen;
        dropped_.fetch_add(s.audioLen, std::memory_order_relaxed);
      }
      continue;
    }
    if (s.audioLen != 0) {
      const std::span<const uint16_t> chans = set->channels();
      bool room = true;
      for (size_t i = 0; i < chans.size() && room; ++i) room = set->ring(i).space() >= s.audioLen;
      if (room) {
        const int64_t at = s.engineTime + s.audioFrom;
        for (size_t i = 0; i < chans.size(); ++i) (void)set->ring(i).push(input(chans[i], at), s.audioLen);
        stats_.capturedFrames += s.audioLen;
      } else {
        s.dropped = s.audioLen;
        stats_.droppedFrames += s.audioLen;
        dropped_.fetch_add(s.audioLen, std::memory_order_relaxed);
      }
    }
    (void)segments_.tryPush(s);
    ++stats_.segments;
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
      ++stats_.monitorWrites;
    }
  }
  return changed;
}

}  // namespace pq::engine
