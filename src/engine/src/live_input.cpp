// Live note input (T6.4): pq/engine/live_input.h describes it.
#include "pq/engine/live_input.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace pq::engine {

namespace {
constexpr uint32_t kLiveNoteBase = 0xE0000000u;  // live note ids: kLiveNoteBase | n, n < 2^28
constexpr double kClockGain = 1.0 / 64.0;         // clock filter: the share of a callback's measured error taken
constexpr double kClockJump = 0.010;              // seconds: a larger error re-anchors (a reopened device, a stall)
}  // namespace

// ---- LivePorts ---------------------------------------------------------------------------------------------------

LivePorts::LivePorts(uint32_t ringCapacity) {
  const uint32_t cap = std::bit_ceil(std::max(ringCapacity, 2u));
  for (auto& r : rings_) r = std::make_unique<SpscRing<RawMidi>>(cap);
}

bool LivePorts::push(uint32_t port, const RawMidi& m) noexcept {  // RT-safe (wait-free)
  if (port >= kLiveSources) return false;
  const bool ok = rings_[port]->tryPush(m);
  if (!ok) overflow_[port].fetch_add(1, std::memory_order_release);
  activity_.fetch_add(1, std::memory_order_release);  // after the push: the consumer that sees it sees the message
  return ok;
}

// ---- CaptureRing (T6.9) --------------------------------------------------------------------------------------------

CaptureRing::CaptureRing(uint32_t capacity) {
  const uint32_t cap = std::bit_ceil(std::max(capacity, 2u));
  slots_ = std::make_unique<Slot[]>(cap);
  mask_ = cap - 1;
}

uint64_t CaptureRing::begin(int64_t engineTime, int64_t songSample, float velocity, float bendCents,
                            const tuning::PitchId& id, const RawMidi& raw) noexcept {  // RT-safe
  const uint64_t seq = written_.load(std::memory_order_relaxed);  // the only writer
  begun_.store(seq + 1, std::memory_order_relaxed);  // seqlock: a reader that sees what follows sees this
  std::atomic_thread_fence(std::memory_order_release);
  Slot& s = slots_[seq & mask_];
  s.engineTime = engineTime;
  s.songSample = songSample;
  s.velocity = velocity;
  s.bendCents = bendCents;
  s.id = id;
  s.raw = raw;
  s.release.store(0.0f, std::memory_order_relaxed);
  s.end.store(kCaptureOpen, std::memory_order_relaxed);
  written_.store(seq + 1, std::memory_order_release);
  return seq;
}

void CaptureRing::end(uint64_t seq, int64_t engineTime, float release) noexcept {  // RT-safe
  if (written_.load(std::memory_order_relaxed) - seq > mask_ + 1u) return;  // overwritten by a later note
  Slot& s = slots_[seq & mask_];
  s.release.store(release, std::memory_order_relaxed);
  s.end.store(std::max(engineTime, s.engineTime + 1), std::memory_order_release);
}

void CaptureRing::read(uint64_t from, std::vector<CapturedNote>& out) const {
  const uint64_t cap = uint64_t{mask_} + 1;
  const uint64_t w = written_.load(std::memory_order_acquire);
  const uint64_t lo = std::max(from, w > cap ? w - cap : 0);
  if (lo >= w) return;
  const size_t first = out.size();
  out.reserve(first + static_cast<size_t>(w - lo));
  for (uint64_t q = lo; q < w; ++q) {
    const Slot& s = slots_[q & mask_];
    CapturedNote n;
    n.seq = q;
    n.endTime = s.end.load(std::memory_order_acquire);
    n.release = s.release.load(std::memory_order_relaxed);
    n.engineTime = s.engineTime;
    n.songSample = s.songSample;
    n.velocity = s.velocity;
    n.bendCents = s.bendCents;
    n.id = s.id;
    n.raw = s.raw;
    out.push_back(n);
  }
  // Seqlock-style validation: the writer may have overwritten the oldest slots while they were copied (writing note q
  // overwrites q − cap): only notes from (the notes begun now) − cap on are whole.
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint64_t b = begun_.load(std::memory_order_relaxed);
  const uint64_t keep = b > cap ? b - cap : 0;
  size_t drop = 0;
  while (first + drop < out.size() && out[first + drop].seq < keep) ++drop;
  out.erase(out.begin() + static_cast<std::ptrdiff_t>(first), out.begin() + static_cast<std::ptrdiff_t>(first + drop));
}

// ---- LiveRouteImage ----------------------------------------------------------------------------------------------

Result<std::unique_ptr<const LiveRouteImage>> LiveRouteImage::build(const LiveRouteDesc& desc) {
  auto img = std::unique_ptr<LiveRouteImage>(new LiveRouteImage());
  img->tables_.reserve(desc.tracks.size());
  img->captures_.reserve(desc.tracks.size());
  for (const LiveTrackDesc& t : desc.tracks) {
    img->captures_.push_back(t.capture);
    if (t.keys.size() != 128) return Error{ErrorCode::InvalidArgument, "live route: a key table must have 128 entries", {}};
    if (!t.keyboard.empty() && t.keyboard.size() != kKeyboardKeys)
      return Error{ErrorCode::InvalidArgument, "live route: a keyboard table has the wrong size", {}};
    if (t.channel > kLiveChannels) return Error{ErrorCode::InvalidArgument, "live route: channel out of range", {}};
    std::vector<LivePitch> table = t.keys;
    table.insert(table.end(), t.keyboard.begin(), t.keyboard.end());
    img->tables_.push_back(std::move(table));
  }
  for (uint32_t s = 0; s < kLiveSources; ++s) {
    const float semis = desc.bendSemitones[s];
    img->bendCents_[s] = (semis > 0.0f && std::isfinite(semis) ? semis : 2.0f) * 100.0f;
    for (uint32_t c = 0; c < kLiveChannels; ++c) {
      Range& r = img->ranges_[s * kLiveChannels + c];
      r.begin = static_cast<uint32_t>(img->targetList_.size());
      for (size_t i = 0; i < desc.tracks.size(); ++i) {
        const LiveTrackDesc& t = desc.tracks[i];
        if ((t.sources & (1u << s)) == 0 || (t.channel != 0 && t.channel != c + 1)) continue;
        const LivePitch* keys = img->tables_[i].data();
        img->targetList_.push_back({t.slot, keys, t.keyboard.empty() ? nullptr : keys + 128, t.capture.get()});
      }
      r.end = static_cast<uint32_t>(img->targetList_.size());
    }
  }
  return std::unique_ptr<const LiveRouteImage>(std::move(img));
}

// ---- LiveInput: UI thread ------------------------------------------------------------------------------------------

LiveInput::LiveInput(const LiveInputConfig& config, std::shared_ptr<LivePorts> ports)
    : cfg_(config),
      ports_(ports ? std::move(ports) : std::make_shared<LivePorts>()),
      record_(std::bit_ceil(std::max(config.recordRing, 2u))) {
  cfg_.maxHeld = std::max(cfg_.maxHeld, 1u);
  cfg_.maxPending = std::max(cfg_.maxPending, 1u);
  held_.resize(cfg_.maxHeld);
  pending_.resize(static_cast<size_t>(cfg_.maxPending) + cfg_.maxHeld);
  ticksPerFrame_ = cfg_.clockHz / cfg_.sampleRate;
  framesPerTick_ = cfg_.sampleRate / cfg_.clockHz;
  bendRamp_ = static_cast<uint32_t>(std::lround(cfg_.bendRampMs * 1e-3 * cfg_.sampleRate));
  staleFrames_ = static_cast<int64_t>(cfg_.staleSeconds * cfg_.sampleRate);
  for (uint32_t s = 0; s < kLiveSources; ++s) seenOverflow_[s] = ports_->overflows(s);
  seenActivity_ = ports_->activity();
}

LiveInput::~LiveInput() = default;

void LiveInput::publish(std::unique_ptr<const LiveRouteImage> image) {  // UI thread
  current_.store(image.get(), std::memory_order_release);
  const uint64_t e = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (owned_) retired_.push_back({std::move(owned_), e});
  owned_ = std::move(image);
}

void LiveInput::collect(bool audioStopped) {  // UI thread
  const uint64_t done = done_.load(std::memory_order_acquire);
  std::erase_if(retired_, [&](const Retired& r) { return audioStopped || r.until <= done; });
}

// ---- LiveInput: audio thread ---------------------------------------------------------------------------------------

int64_t LiveInput::engineTimeOf(int64_t stamp) const noexcept {  // RT-safe
  if (!anchored_) return blockStart_;
  return anchorTime_ + static_cast<int64_t>(std::llround((static_cast<double>(stamp) - anchorHost_) * framesPerTick_));
}

void LiveInput::updateClock(const Block& b) noexcept {  // RT-safe
  if (b.hostTime == 0 || (anchored_ && b.hostTime == lastHost_)) return;  // no stamp / the same device callback
  lastHost_ = b.hostTime;
  const int64_t devStart = b.start - b.hostOffset;  // engine time of the callback's first frame
  const auto host = static_cast<double>(b.hostTime);
  if (anchored_) {
    const double predicted = anchorHost_ + static_cast<double>(devStart - anchorTime_) * ticksPerFrame_;
    const double err = host - predicted;
    if (std::fabs(err) <= kClockJump * cfg_.clockHz) {
      anchorTime_ = devStart;
      anchorHost_ = predicted + err * kClockGain;
      return;
    }
  }
  anchored_ = true;  // first stamp, or a jump: re-anchor at this callback
  anchorTime_ = devStart;
  anchorHost_ = host;
}

bool LiveInput::queue(int64_t time, uint32_t slot, const NoteEvent& e, bool off) noexcept {  // RT-safe
  // Room for every held note's note-off is kept: other events only while fewer than maxPending are queued.
  if (numPending_ >= (off ? static_cast<uint32_t>(pending_.size()) : cfg_.maxPending)) return false;
  pending_[numPending_++] = Pending{time, slot, 0, e};
  if (off) ++numPendingOffs_;
  return true;
}

void LiveInput::record(LiveNoteRecord::Kind kind, uint32_t slot, int64_t time, float velocity, const LivePitch* pitch,
                       const RawMidi& m, const Block& b) noexcept {  // RT-safe
  if (!recording_.load(std::memory_order_relaxed)) return;
  LiveNoteRecord r;
  r.kind = kind;
  r.slot = slot;
  r.engineTime = time;
  r.songSample = b.playing ? b.songSample + (time - b.start) : -1;
  r.velocity = velocity;
  const uint32_t src = m.port < kLiveSources ? m.port : 0;
  r.bendCents = bendCents_[src * kLiveChannels + (m.status & 15u)];
  if (pitch != nullptr) r.pitch = *pitch;
  r.raw = m;
  if (!record_.tryPush(r)) ++stats_.recordDrops;
}

void LiveInput::releaseHeld(uint32_t i, float releaseVelocity, int64_t time, const RawMidi* m, const Block& b) noexcept {  // RT-safe
  const Held h = held_[i];
  if (queue(time, h.slot, engine::noteOff(0, h.noteId, releaseVelocity), true)) ++stats_.noteOffs;
  if (m != nullptr) record(LiveNoteRecord::Kind::Off, h.slot, time, releaseVelocity * 127.0f, nullptr, *m, b);
  if (h.capture != nullptr) h.capture->end(h.captureSeq, time, releaseVelocity * 127.0f);  // T6.9
  held_[i] = held_[--numHeld_];
}

void LiveInput::onNoteOff(uint32_t source, uint32_t channel, uint32_t key, float releaseVelocity, int64_t time,
                          const RawMidi& m, const Block& b) noexcept {  // RT-safe
  const bool pedal = sustain_[source * kLiveChannels + channel] != 0;
  for (uint32_t i = numHeld_; i-- > 0;) {
    Held& h = held_[i];
    if (h.source != source || h.channel != channel || h.key != key || h.sustained) continue;
    if (pedal) h.sustained = true;  // the pedal holds it: released when the pedal goes up
    else releaseHeld(i, releaseVelocity, time, &m, b);
  }
}

void LiveInput::releaseSource(uint32_t source, int64_t time, const Block& b) noexcept {  // RT-safe
  for (uint32_t i = numHeld_; i-- > 0;)
    if (held_[i].source == source) {
      releaseHeld(i, 0.5f, time, nullptr, b);
      ++stats_.overflowReleases;
    }
  for (uint32_t c = 0; c < kLiveChannels; ++c) sustain_[source * kLiveChannels + c] = 0;
}

void LiveInput::onNoteOn(const RawMidi& m, int64_t time, const LiveRouteImage& img, const Block& b) noexcept {  // RT-safe
  const uint32_t src = m.port, ch = m.status & 15u, key = m.d1;
  const float bend = bendCents_[src * kLiveChannels + ch];
  const float bendRatio = bend != 0.0f ? std::exp2(bend * (1.0f / 1200.0f)) : 1.0f;
  for (const LiveRouteImage::Target& t : img.targets(src, ch)) {
    const LivePitch* p = nullptr;
    if (src == kLiveKeyboard) {
      if (t.keyboard != nullptr && key < kKeyboardKeys) p = &t.keyboard[key];
    } else {
      p = &t.keys[key & 127u];
    }
    if (p == nullptr || !p->mapped()) continue;
    if (numHeld_ >= held_.size() || numPending_ >= cfg_.maxPending) {
      ++stats_.droppedOns;
      continue;
    }
    nextNote_ = (nextNote_ + 1) & 0x0FFFFFFFu;
    const uint32_t id = kLiveNoteBase | nextNote_;
    // The fields Timeline::setIdentity sets for the same drawn note (ratio 1, harmonic 0: Degree / Cents identities).
    NoteEvent e = engine::noteOn(0, id, p->hz * bendRatio, static_cast<float>(m.d2) * (1.0f / 127.0f));
    e.degree = p->degree;
    e.equave = p->equave;
    e.tuning = p->tuning;
    e.noteDegree = p->noteDegree;
    (void)queue(time, t.slot, e, false);
    // T6.9: Capture MIDI: always, into the track's ring.
    const uint64_t seq = t.capture != nullptr ? t.capture->begin(time, b.playing ? b.songSample + (time - b.start) : -1,
                                                                 static_cast<float>(m.d2), bend, p->id, m)
                                              : 0;
    held_[numHeld_++] = Held{static_cast<uint8_t>(src), static_cast<uint8_t>(ch), static_cast<uint8_t>(key), false, t.slot,
                             id, p->hz, t.capture, seq};
    ++stats_.noteOns;
    record(LiveNoteRecord::Kind::On, t.slot, time, static_cast<float>(m.d2), p, m, b);
  }
}

void LiveInput::retuneHeld(const LiveRouteImage& img, const Block& b) noexcept {  // RT-safe
  for (uint32_t i = 0; i < numHeld_; ++i) {
    Held& h = held_[i];
    const LivePitch* p = nullptr;
    for (const LiveRouteImage::Target& t : img.targets(h.source, h.channel)) {
      if (t.slot != h.slot) continue;
      if (h.source == kLiveKeyboard) p = t.keyboard != nullptr && h.key < kKeyboardKeys ? &t.keyboard[h.key] : nullptr;
      else p = &t.keys[h.key & 127u];
      break;
    }
    if (p == nullptr || !p->mapped() || p->hz == h.hz) continue;  // unrouted now (its note-off still comes) or unmoved
    const float bend = bendCents_[h.source * kLiveChannels + h.channel];
    const float ratio = bend != 0.0f ? std::exp2(bend * (1.0f / 1200.0f)) : 1.0f;
    NoteEvent e = engine::notePitch(0, h.noteId, p->hz * ratio);
    e.flags = kNoteEventRetune;
    e.degree = p->degree;
    e.equave = p->equave;
    e.tuning = p->tuning;
    e.noteDegree = p->noteDegree;
    if (queue(b.start, h.slot, e, false)) {
      h.hz = p->hz;
      ++stats_.retuned;
    }
  }
}

// T6.9: a new image: each held note's ring is the one the image routes its (source, channel) to its slot with, if that
// is the ring its note-on went to; else none (its track stopped listening: the ring may be freed with the old image).
void LiveInput::recaptureHeld(const LiveRouteImage* img) noexcept {  // RT-safe
  for (uint32_t i = 0; i < numHeld_; ++i) {
    Held& h = held_[i];
    if (h.capture == nullptr) continue;
    CaptureRing* keep = nullptr;
    if (img != nullptr)
      for (const LiveRouteImage::Target& t : img->targets(h.source, h.channel))
        if (t.slot == h.slot && t.capture == h.capture) {
          keep = h.capture;
          break;
        }
    h.capture = keep;
  }
}

void LiveInput::bend(uint32_t source, uint32_t channel, float cents, int64_t time) noexcept {  // RT-safe
  float& cur = bendCents_[source * kLiveChannels + channel];
  if (cur == cents) return;
  cur = cents;
  const float ratio = std::exp2(cents * (1.0f / 1200.0f));
  for (uint32_t i = 0; i < numHeld_; ++i) {
    const Held& h = held_[i];
    if (h.source != source || h.channel != channel) continue;
    if (queue(time, h.slot, engine::notePitch(0, h.noteId, h.hz * ratio, bendRamp_), false)) ++stats_.bends;
  }
}

void LiveInput::handle(const RawMidi& m, int64_t time, bool stale, const LiveRouteImage* img, const Block& b) noexcept {  // RT-safe
  if (m.port >= kLiveSources) return;
  const uint32_t src = m.port, ch = m.status & 15u;
  switch (m.status & 0xF0u) {
    case 0x90:
      if (m.d2 != 0) {
        // A key struck again while held (or sustained) ends its earlier note first.
        for (uint32_t i = numHeld_; i-- > 0;)
          if (held_[i].source == src && held_[i].channel == ch && held_[i].key == m.d1) releaseHeld(i, 0.5f, time, &m, b);
        if (stale || img == nullptr) {
          if (stale) ++stats_.droppedOns;
          return;
        }
        onNoteOn(m, time, *img, b);
        return;
      }
      onNoteOff(src, ch, m.d1, 64.0f / 127.0f, time, m, b);  // note-on with velocity 0 = note-off
      return;
    case 0x80:
      onNoteOff(src, ch, m.d1, static_cast<float>(m.d2) * (1.0f / 127.0f), time, m, b);
      return;
    case 0xE0: {
      const int32_t v = (static_cast<int32_t>(m.d2 & 127u) << 7) | (m.d1 & 127u);
      const float range = img != nullptr ? img->bendCents(src) : 200.0f;
      bend(src, ch, static_cast<float>(v - 8192) * (1.0f / 8192.0f) * range, time);
      return;
    }
    case 0xB0:
      if (m.d1 == 64) {  // sustain pedal
        const bool down = m.d2 >= 64;
        sustain_[src * kLiveChannels + ch] = down ? 1 : 0;
        if (!down)
          for (uint32_t i = numHeld_; i-- > 0;)
            if (held_[i].source == src && held_[i].channel == ch && held_[i].sustained) releaseHeld(i, 0.5f, time, &m, b);
      } else if (m.d1 == 120 || m.d1 == 123) {  // All Sound Off / All Notes Off: every held note of the channel
        sustain_[src * kLiveChannels + ch] = 0;
        for (uint32_t i = numHeld_; i-- > 0;)
          if (held_[i].source == src && held_[i].channel == ch) releaseHeld(i, 0.5f, time, &m, b);
      }
      return;
    default:
      return;  // aftertouch, program change: not used yet
  }
}

bool LiveInput::process(const Block& b) noexcept {  // RT-safe
  const uint64_t epoch = epoch_.load(std::memory_order_acquire);
  const LiveRouteImage* img = current_.load(std::memory_order_acquire);
  blockStart_ = b.start;
  now_.store(b.start + static_cast<int64_t>(b.frames), std::memory_order_relaxed);  // T6.9
  // Events emitted, or due before this block and never asked for (no unit ran their slot), are done.
  if (numPending_ != 0) {
    uint32_t o = 0, offs = 0;
    for (uint32_t i = 0; i < numPending_; ++i) {
      const Pending& p = pending_[i];
      if (p.done != 0 || p.time < b.start) continue;
      if (p.e.kind == NoteEventKind::NoteOff) ++offs;
      pending_[o++] = p;
    }
    numPending_ = o;
    numPendingOffs_ = offs;
  }
  updateClock(b);
  // TU.1: a new route image (a tuning edit that needs a new table: an EDO size, a MOS shape) gives a held key a new
  // pitch: the note moves there at the block start (a Pitch event that also carries the new identity, as the
  // timeline retunes a sounding drawn note on a sequence swap), keeping its bend. Continuous edits never get here:
  // they keep the image (the tuning node retunes from its params).
  bool retuned = false;
  if (img != seenRoute_) {
    seenRoute_ = img;
    if (numHeld_ != 0) recaptureHeld(img);  // T6.9: before the old image (and maybe a ring) can be freed
    if (img != nullptr && numHeld_ != 0) {
      const uint32_t before = numPending_;
      retuneHeld(*img, b);
      retuned = numPending_ != before;
    }
  }
  const uint32_t latency = latency_.load(std::memory_order_relaxed);
  const int64_t latest = b.start + latency + b.frames;  // a stamp from the future plays no later than this
  bool any = false;
  // Nothing pushed since the last drain: one load, no ring touched (the idle cost of live input per block).
  const uint64_t activity = ports_->activity();
  if (activity == seenActivity_) {
    done_.store(epoch, std::memory_order_release);
    return retuned;
  }
  seenActivity_ = activity;
  for (uint32_t s = 0; s < kLiveSources; ++s) {
    if (const uint64_t o = ports_->overflows(s); o != seenOverflow_[s]) {  // messages were lost: no stuck notes
      seenOverflow_[s] = o;
      releaseSource(s, b.start, b);
      any = true;
    }
    SpscRing<RawMidi>& ring = ports_->ring(s);
    RawMidi m;
    // Bounded: at most one ring's worth per block (a producer pushing meanwhile waits for the next block).
    for (uint32_t n = ring.capacity(); n > 0 && ring.tryPop(m); --n) {
      any = true;
      ++stats_.messages;
      int64_t t = b.start;
      bool stale = false;
      if (anchored_ && m.stamp != 0) {
        t = engineTimeOf(m.stamp) + latency;
        stale = t < b.start - staleFrames_;
        if (t < b.start) {
          ++stats_.lateEvents;
          t = b.start;
        }
        t = std::min(t, latest);
      }
      handle(m, t, stale, img, b);
    }
  }
  done_.store(epoch, std::memory_order_release);
  return any;
}

void LiveInput::emit(uint32_t slot, int64_t sampleTime, uint32_t frames, EventBuffer& out) noexcept {  // RT-safe
  if (numPending_ == 0) return;
  const int64_t end = sampleTime + frames;
  for (uint32_t i = 0; i < numPending_; ++i) {
    Pending& p = pending_[i];
    if (p.done != 0 || p.slot != slot || p.time >= end) continue;
    NoteEvent e = p.e;
    e.frame = static_cast<uint32_t>(std::clamp<int64_t>(p.time - sampleTime, 0, static_cast<int64_t>(frames) - 1));
    out.push(e);
    p.done = 1;
  }
}

}  // namespace pq::engine
