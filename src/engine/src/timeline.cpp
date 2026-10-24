#include "pq/engine/timeline.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>

namespace pq::engine {

// ---- TempoImage -----------------------------------------------------------------------------------------------

TempoImage::TempoImage() : pointTick_{0}, ramps_{0} {}

Result<std::unique_ptr<TempoImage>> TempoImage::create(std::span<const TempoPoint> tempo,
                                                       std::span<const TimeSigChange> signatures) {
  auto img = std::make_unique<TempoImage>();
  if (!tempo.empty()) {
    auto map = TempoMap::create(tempo);
    if (!map) return map.error();
    img->tempo_ = std::move(map.value());
    img->pointTick_.clear();
    img->ramps_.clear();
    for (size_t i = 0; i < tempo.size(); ++i) {
      img->pointTick_.push_back(tempo[i].tick);
      img->ramps_.push_back(tempo[i].curve == TempoCurve::Linear && i + 1 < tempo.size() &&
                                    tempo[i + 1].bpm != tempo[i].bpm
                                ? 1
                                : 0);
    }
  }
  if (!signatures.empty()) {
    auto sig = TimeSigMap::create(signatures);
    if (!sig) return sig.error();
    img->sig_ = std::move(sig.value());
  }
  return img;
}

bool TempoImage::constantBetween(double t0, double t1) const noexcept {  // RT-safe
  auto seg = [&](double t) noexcept {
    return std::upper_bound(pointTick_.begin(), pointTick_.end(), t,
                            [](double v, Ticks p) noexcept { return v < static_cast<double>(p); }) -
           pointTick_.begin() - 1;
  };
  const auto i0 = seg(t0);
  return i0 == seg(t1) && (i0 < 0 || ramps_[static_cast<size_t>(i0)] == 0);
}

bool TempoImage::segmentAt(Ticks t, Ticks* from, Ticks* to) const noexcept {  // RT-safe
  const auto it = std::upper_bound(pointTick_.begin(), pointTick_.end(), t);
  if (it == pointTick_.begin()) {  // before the first point: the first tempo, constant
    *from = INT64_MIN;
    *to = pointTick_.front();
    return true;
  }
  const size_t i = static_cast<size_t>(it - pointTick_.begin()) - 1;
  *from = pointTick_[i];
  *to = it == pointTick_.end() ? INT64_MAX : *it;
  return ramps_[i] == 0;
}

// ---- Timeline: construction and publication ---------------------------------------------------------------------

namespace {

TimelineConfig normalised(TimelineConfig c) {
  c.maxBlockFrames = std::max(c.maxBlockFrames, 1u);
  c.maxTracks = std::max(c.maxTracks, 1u);
  c.maxEventsPerTrack = std::max(c.maxEventsPerTrack, 4u);
  c.maxSoundingPerTrack = std::max(c.maxSoundingPerTrack, 1u);
  c.commandCapacity = std::bit_ceil(std::max(c.commandCapacity, 2u));
  c.reportCapacity = std::bit_ceil(std::max(c.reportCapacity, 2u));
  if (!(c.sampleRate >= 1.0)) c.sampleRate = 48000.0;
  return c;
}

}  // namespace

Timeline::Timeline(const TimelineConfig& config)
    : cfg_(normalised(config)), commands_(cfg_.commandCapacity), reports_(cfg_.reportCapacity) {
  reader_ = *reclaimer_.registerReader();
  defaultTempo_ = std::make_unique<TempoImage>();
  tempo_ = defaultTempo_.get();
  const size_t heapSlots = static_cast<size_t>(cfg_.maxTracks) * cfg_.maxSoundingPerTrack;
  heapPool_[0] = AlignedBuffer<Sounding>(heapSlots);
  heapPool_[1] = AlignedBuffer<Sounding>(heapSlots);
  tracks_.resize(cfg_.maxTracks);
  scratchTracks_.resize(cfg_.maxTracks);
  for (uint32_t k = 0; k < cfg_.maxTracks; ++k) {
    tracks_[k].heap = heapPool_[0].data() + static_cast<size_t>(k) * cfg_.maxSoundingPerTrack;
    tracks_[k].cur.nextOn = kNever;
  }
  next_.assign(cfg_.maxTracks, kNever);
  events_ = AlignedBuffer<NoteEvent>(static_cast<size_t>(cfg_.maxTracks) * cfg_.maxEventsPerTrack);
  eventCount_.assign(cfg_.maxTracks, 0);
  matchedScratch_.assign(cfg_.maxTracks, 0);
  sources_.resize(cfg_.maxTracks);
  for (uint32_t k = 0; k < cfg_.maxTracks; ++k) sources_[k] = {this, k};
  automation_.prepare(cfg_.maxAutomationLanes, cfg_.maxBlockFrames);
  tickBuf_ = AlignedBuffer<double>(cfg_.maxBlockFrames);
  updateLoopSamples();
}

Timeline::~Timeline() {
  delete pubSeq_.exchange(nullptr);
  delete pubAuto_.exchange(nullptr);
  delete pubTempo_.exchange(nullptr);
  reclaimer_.unregisterReader(reader_);
}

Status Timeline::publishSequence(std::unique_ptr<SequenceImage> image) {
  if (image == nullptr) return Error{ErrorCode::InvalidArgument, "Timeline: null sequence image", {}};
  if (image->numTracks() > cfg_.maxTracks)
    return Error{ErrorCode::InvalidArgument,
                 "Timeline: " + std::to_string(image->numTracks()) + " tracks (maximum " +
                     std::to_string(cfg_.maxTracks) + ")",
                 {}};
  if (SequenceImage* old = pubSeq_.exchange(image.release(), std::memory_order_acq_rel)) reclaimer_.retire(old);
  collect();
  return Ok{};
}

Status Timeline::publishAutomation(std::unique_ptr<AutomationImage> image) {
  if (image == nullptr) return Error{ErrorCode::InvalidArgument, "Timeline: null automation image", {}};
  if (image->numLanes() > cfg_.maxAutomationLanes)
    return Error{ErrorCode::InvalidArgument,
                 "Timeline: " + std::to_string(image->numLanes()) + " automation lanes (maximum " +
                     std::to_string(cfg_.maxAutomationLanes) + ")",
                 {}};
  if (AutomationImage* old = pubAuto_.exchange(image.release(), std::memory_order_acq_rel)) reclaimer_.retire(old);
  collect();
  return Ok{};
}

Status Timeline::publishTempo(std::unique_ptr<TempoImage> image) {
  if (image == nullptr) return Error{ErrorCode::InvalidArgument, "Timeline: null tempo image", {}};
  if (TempoImage* old = pubTempo_.exchange(image.release(), std::memory_order_acq_rel)) reclaimer_.retire(old);
  collect();
  return Ok{};
}

size_t Timeline::collect() { return reclaimer_.collect(); }

bool Timeline::post(const TransportCommand& command) noexcept { return commands_.tryPush(command); }  // RT-safe
bool Timeline::poll(TransportReport& report) noexcept { return reports_.tryPop(report); }             // RT-safe

// ---- Audio thread: commands and images ---------------------------------------------------------------------------

void Timeline::apply(const TransportCommand& cmd) noexcept {  // RT-safe
  const double sr = cfg_.sampleRate;
  auto locate = [&](Ticks t) noexcept {
    if (playing_) releasePending_ = true;
    pos_ = tempo_->tempo().ticksToSamples(t, sr);
    nextTick_ = t;
    tickLazy_ = false;
    seekPending_ = true;
  };
  switch (cmd.kind) {
    case TransportCommandKind::Play:
      locate(cmd.tick);
      playing_ = true;
      break;
    case TransportCommandKind::Stop:
      if (playing_) releasePending_ = true;
      playing_ = false;
      break;
    case TransportCommandKind::Continue:
      if (!playing_) seekPending_ = true;
      playing_ = true;
      break;
    case TransportCommandKind::Locate: locate(cmd.tick); break;
    case TransportCommandKind::SetLoop:
      loopEnabled_ = cmd.loopEnabled;
      loopStart_ = cmd.loopStart;
      loopEnd_ = cmd.loopEnd;
      updateLoopSamples();
      break;
  }
  snapshotDirty_ = true;
  holdValid_ = false;
  reportPending_ = true;
}

void Timeline::drainCommands() noexcept {  // RT-safe
  TransportCommand c;
  for (uint32_t i = 0; i < cfg_.commandCapacity && commands_.tryPop(c); ++i) {
    apply(c);
    ++commandsPosted_;
  }
}

void Timeline::updateLoopSamples() noexcept {  // RT-safe
  const TempoMap& tm = tempo_->tempo();
  loopStartSample_ = tm.ticksToSamples(loopStart_, cfg_.sampleRate);
  loopEndSample_ = tm.ticksToSamples(loopEnd_, cfg_.sampleRate);
  loopOn_ = loopEnabled_ && loopEnd_ > loopStart_ && loopEndSample_ > loopStartSample_;
}

void Timeline::pickUpImages() noexcept {  // RT-safe
  const TempoImage* t = pubTempo_.load(std::memory_order_acquire);
  if (t == nullptr) t = defaultTempo_.get();
  if (t != tempo_) swapTempo(t);
  const SequenceImage* s = pubSeq_.load(std::memory_order_acquire);
  if (s != seq_) swapSequence(s);
  const AutomationImage* a = pubAuto_.load(std::memory_order_acquire);
  if (a != auto_) {
    auto_ = a;
    automation_.attach(a);
    holdValid_ = false;
    ++stats_.automationSwaps;
  }
}

void Timeline::swapTempo(const TempoImage* next) noexcept {  // RT-safe
  // Keep the musical position: the next tick to play stays, its sample moves.
  const Ticks t = resolveTick();  // under the old map
  tempo_ = next;
  pos_ = next->tempo().ticksToSamples(t, cfg_.sampleRate);
  minNextDirty_ = true;
  updateLoopSamples();
  snapshotDirty_ = true;
  holdValid_ = false;
  lastTickSample_ = INT64_MIN;
  barStartTick_ = barEndTick_ = 0;  // the bar and tempo caches (computeSnapshot) are stale
  bpmFrom_ = bpmTo_ = 0;
  ++stats_.tempoSwaps;
}

void Timeline::swapSequence(const SequenceImage* next) noexcept {  // RT-safe
  const uint32_t newN = next != nullptr ? next->numTracks() : 0u;
  const uint32_t oldN = numTracks_;
  const uint32_t cap = cfg_.maxSoundingPerTrack;
  const Ticks now = resolveTick();
  bool identity = newN == oldN;
  for (uint32_t k = 0; identity && k < newN; ++k) identity = next->tracks()[k].id == tracks_[k].id;

  // Re-validates the sounding notes of `src` (old state) against the new image's track `j`, writing the survivors
  // into `dst.heap` (which may be src.heap itself). Note-offs for the others go to slot `j` at frame 0.
  auto carry = [&](const TrackState& src, TrackState& dst, uint32_t j) noexcept {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < src.heapSize; ++i) {
      Sounding s = src.heap[i];
      Ticks passEnd = 0;
      const SequenceImage::Note* n = next->findOccurrence(j, s.noteId, s.on, &passEnd);
      if (n != nullptr && std::bit_cast<uint32_t>(n->hz) == std::bit_cast<uint32_t>(s.hz)) {
        s.off = std::min(s.on + n->length, passEnd);
        if (s.off > now || !playing_) {
          dst.heap[kept++] = s;
          ++stats_.keptOnSwap;
          continue;
        }
      }
      emitOff(j, s, 0);
      ++stats_.stoppedOnSwap;
    }
    dst.heapSize = kept;
    std::make_heap(dst.heap, dst.heap + kept, [](const Sounding& a, const Sounding& b) noexcept {
      return a.off != b.off ? a.off > b.off : a.order > b.order;
    });
  };

  if (identity) {
    for (uint32_t k = 0; k < newN; ++k) carry(tracks_[k], tracks_[k], k);
  } else {
    // Sounding notes follow their track id into the other heap pool.
    const uint32_t side = heapPoolSide_ ^ 1u;
    for (uint32_t j = 0; j < cfg_.maxTracks; ++j) {
      TrackState& d = scratchTracks_[j];
      d = TrackState{};
      d.heap = heapPool_[side].data() + static_cast<size_t>(j) * cap;
      d.cur.nextOn = kNever;
    }
    std::vector<uint8_t>& matched = matchedScratch_;
    std::fill(matched.begin(), matched.end(), uint8_t{0});
    for (uint32_t j = 0; j < newN; ++j) {
      const uint32_t id = next->tracks()[j].id;
      scratchTracks_[j].id = id;
      for (uint32_t i = 0; i < oldN; ++i) {
        if (matched[i] || tracks_[i].id != id) continue;
        matched[i] = 1;
        scratchTracks_[j].order = tracks_[i].order;
        carry(tracks_[i], scratchTracks_[j], j);
        break;
      }
    }
    for (uint32_t i = 0; i < oldN; ++i) {  // tracks that disappeared: offs in their old slot (ids are unique)
      if (matched[i]) continue;
      for (uint32_t h = 0; h < tracks_[i].heapSize; ++h) {
        emitOff(i, tracks_[i].heap[h], 0);
        ++stats_.orphanedNotes;
      }
    }
    tracks_.swap(scratchTracks_);
    heapPoolSide_ = side;
  }
  seq_ = next;
  numTracks_ = newN;
  for (uint32_t k = newN; k < cfg_.maxTracks; ++k) {
    tracks_[k].heapSize = 0;
    tracks_[k].cur = Cursor{};
    tracks_[k].cur.nextOn = kNever;
    next_[k] = kNever;
  }
  seekAll(now);
  minNextDirty_ = true;
  ++stats_.sequenceSwaps;
}

// ---- Audio thread: the cursor ------------------------------------------------------------------------------------

void Timeline::setupPass0(Cursor& c) noexcept {  // RT-safe
  const SequenceImage::Clip& cl = seq_->clips()[c.clip];
  c.pass = 0;
  c.passStart = cl.start;
  c.passContent = cl.content0;
  c.passEnd = cl.firstPassEnd;
  c.note = cl.pass0Begin;
  c.noteEnd = cl.pass0End;
}

void Timeline::computeNextOn(Cursor& c) noexcept {  // RT-safe; O(clips skipped), each pass at most once
  const auto clips = seq_->clips();
  const auto notes = seq_->notes();
  while (c.clip < c.clipEnd) {
    const SequenceImage::Clip& cl = clips[c.clip];
    if (c.note < c.noteEnd) {
      const Ticks t = c.passStart + (notes[c.note].start - c.passContent);
      if (t < c.passEnd) {
        c.nextOn = t;
        return;
      }
    }
    // This pass is over. The next loop pass, if it has notes (otherwise every later pass is empty too).
    if (cl.loopLen > 0 && c.passEnd < cl.end && cl.loopBegin < cl.loopEnd) {
      ++c.pass;
      c.passStart = c.passEnd;
      c.passContent = cl.loopStart;
      c.passEnd = std::min(c.passStart + cl.loopLen, cl.end);
      c.note = cl.loopBegin;
      c.noteEnd = cl.loopEnd;
      continue;
    }
    if (++c.clip < c.clipEnd) setupPass0(c);
  }
  c.nextOn = kNever;
}

void Timeline::seek(uint32_t track, Ticks t) noexcept {  // RT-safe; O(log clips + log notes)
  Cursor& c = tracks_[track].cur;
  c = Cursor{};
  c.nextOn = kNever;
  if (seq_ == nullptr || track >= seq_->numTracks()) return;
  const SequenceImage::Track& tr = seq_->tracks()[track];
  const auto clips = seq_->clips();
  const auto notes = seq_->notes();
  // The first clip ending after t (clips are sorted and disjoint, so their ends are sorted too).
  const auto* cb = clips.data() + tr.clipBegin;
  const auto* ce = clips.data() + tr.clipEnd;
  const auto* it = std::upper_bound(cb, ce, t, [](Ticks v, const SequenceImage::Clip& cl) noexcept { return v < cl.end; });
  c.clip = static_cast<uint32_t>(it - clips.data());
  c.clipEnd = tr.clipEnd;
  if (c.clip >= c.clipEnd) return;
  const SequenceImage::Clip& cl = clips[c.clip];
  auto firstAtOrAfter = [&](uint32_t lo, uint32_t hi, Ticks content) noexcept {
    return static_cast<uint32_t>(std::lower_bound(notes.data() + lo, notes.data() + hi, content,
                                                  [](const SequenceImage::Note& n, Ticks v) noexcept {
                                                    return n.start < v;
                                                  }) -
                                 notes.data());
  };
  setupPass0(c);
  if (t > cl.start) {
    if (cl.loopLen == 0 || t < cl.firstPassEnd) {
      c.note = firstAtOrAfter(cl.pass0Begin, cl.pass0End, cl.content0 + (t - cl.start));
    } else {
      const Ticks o = t - cl.firstPassEnd;
      const Ticks k = o / cl.loopLen;
      c.pass = 1 + k;
      c.passStart = cl.firstPassEnd + k * cl.loopLen;
      c.passContent = cl.loopStart;
      c.passEnd = std::min(c.passStart + cl.loopLen, cl.end);
      c.note = firstAtOrAfter(cl.loopBegin, cl.loopEnd, cl.loopStart + (o - k * cl.loopLen));
      c.noteEnd = cl.loopEnd;
    }
  }
  computeNextOn(c);
}

void Timeline::seekAll(Ticks t) noexcept {  // RT-safe
  for (uint32_t k = 0; k < numTracks_; ++k) {
    seek(k, t);
    refreshNext(k);
  }
}

// ---- Audio thread: events ---------------------------------------------------------------------------------------

namespace {
struct LaterOff {
  template <class S>
  bool operator()(const S& a, const S& b) const noexcept {
    return a.off != b.off ? a.off > b.off : a.order > b.order;
  }
};
}  // namespace

void Timeline::heapPush(TrackState& t, const Sounding& s) noexcept {  // RT-safe
  t.heap[t.heapSize++] = s;
  std::push_heap(t.heap, t.heap + t.heapSize, LaterOff{});
}

Timeline::Sounding Timeline::heapPop(TrackState& t) noexcept {  // RT-safe
  std::pop_heap(t.heap, t.heap + t.heapSize, LaterOff{});
  return t.heap[--t.heapSize];
}

void Timeline::refreshNext(uint32_t track) noexcept {  // RT-safe
  const TrackState& t = tracks_[track];
  next_[track] = std::min(t.cur.nextOn, t.heapSize != 0 ? t.heap[0].off : kNever);
  minNextDirty_ = true;
}

bool Timeline::pushEvent(uint32_t track, const NoteEvent& e) noexcept {  // RT-safe
  uint32_t& n = eventCount_[track];
  if (n >= cfg_.maxEventsPerTrack) return false;
  events_[static_cast<size_t>(track) * cfg_.maxEventsPerTrack + n] = e;
  ++n;
  anyEvents_ = true;
  return true;
}

void Timeline::emitOff(uint32_t track, const Sounding& s, uint32_t frame) noexcept {  // RT-safe
  if (pushEvent(track, noteOff(frame, s.instance, s.releaseVelocity))) ++stats_.noteOffs;
}

void Timeline::releaseAll(uint32_t frame) noexcept {  // RT-safe
  for (uint32_t k = 0; k < cfg_.maxTracks; ++k) {
    TrackState& t = tracks_[k];
    while (t.heapSize != 0) emitOff(k, heapPop(t), frame);
    next_[k] = t.cur.nextOn;
    minNextDirty_ = true;
  }
}

void Timeline::playTrack(uint32_t track, uint32_t frame, SampleTime s0, uint32_t len, Ticks t0,
                         Ticks t1) noexcept {  // RT-safe
  TrackState& tr = tracks_[track];
  Cursor& c = tr.cur;
  const auto notes = seq_->notes();
  const TempoMap& tm = tempo_->tempo();
  const double sr = cfg_.sampleRate;
  const uint32_t cap = cfg_.maxEventsPerTrack;
  const uint32_t maxSounding = cfg_.maxSoundingPerTrack;
  auto frameOf = [&](Ticks t) noexcept {
    const SampleTime d = tm.ticksToSamples(std::max(t, t0), sr) - s0;
    return frame + static_cast<uint32_t>(std::clamp<SampleTime>(d, 0, static_cast<SampleTime>(len) - 1));
  };
  // Note-ons looked at (played, skipped or dropped) in this call; past the limit the rest of the range is skipped
  // by a seek, so a pathological pile of notes in one block costs bounded time.
  uint32_t looked = 0;
  const uint32_t lookLimit = 4 * cap;
  for (;;) {
    const Ticks on = c.nextOn;
    const Ticks off = tr.heapSize != 0 ? tr.heap[0].off : kNever;
    if (off <= on) {  // offs first at equal ticks
      if (off >= t1) break;
      emitOff(track, heapPop(tr), frameOf(off));
      continue;
    }
    if (on >= t1) break;
    if (++looked > lookLimit) {
      ++stats_.droppedNoteOns;
      seek(track, t1);  // nextOn ≥ t1 now: only note-offs remain in this range
      continue;
    }
    const SequenceImage::Note& n = notes[c.note];
    const NoteRandom r = noteRandom(seq_->seed(), n.id, c.pass);
    if (!(r.chance < n.chance)) {
      ++stats_.chanceSkips;
    } else if (eventCount_[track] + 2 + tr.heapSize > cap) {  // keep room for every pending note-off
      ++stats_.droppedNoteOns;
    } else {
      const uint32_t f = frameOf(on);
      if (tr.heapSize >= maxSounding) {  // cut the earliest-ending note to make room
        emitOff(track, heapPop(tr), f);
        ++stats_.earlyOffs;
      }
      const float vel = std::clamp(n.velocity + r.velocity * n.velocityRange, 1.0f, 127.0f) * (1.0f / 127.0f);
      NoteEvent e = noteOn(f, nextInstance_, n.hz, vel);
      e.ratio = n.ratio;
      e.harmonic = n.harmonic;
      if (n.pitch.tuning != 0) {  // T2.6: a Degree note of a tuning signal
        e.degree = static_cast<float>(n.pitch.degree);
        e.equave = static_cast<float>(n.pitch.equave);
      }
      pushEvent(track, e);
      ++stats_.noteOns;
      heapPush(tr, Sounding{std::min(on + n.length, c.passEnd), tr.order++, on, nextInstance_, n.id, n.hz,
                            std::clamp(n.releaseVelocity, 0.0f, 127.0f) * (1.0f / 127.0f)});
      if (++nextInstance_ == 0) nextInstance_ = 1;
    }
    ++c.note;
    computeNextOn(c);
  }
  refreshNext(track);
}

// ---- Audio thread: the block ------------------------------------------------------------------------------------

Ticks Timeline::resolveTick() noexcept {  // RT-safe
  if (tickLazy_) {
    nextTick_ = tempo_->tempo().firstTickAtOrAfterSample(pos_, cfg_.sampleRate);
    tickLazy_ = false;
  }
  return nextTick_;
}

Ticks Timeline::position() const noexcept {  // RT-safe
  return tickLazy_ ? tempo_->tempo().firstTickAtOrAfterSample(pos_, cfg_.sampleRate) : nextTick_;
}

void Timeline::refreshMinNext() noexcept {  // RT-safe; O(tracks), only after something changed
  if (!minNextDirty_) return;
  Ticks m = kNever;
  for (uint32_t k = 0; k < numTracks_; ++k) m = std::min(m, next_[k]);
  minNextSample_ = m == kNever ? INT64_MAX : tempo_->tempo().ticksToSamples(m, cfg_.sampleRate);
  minNextDirty_ = false;
}

void Timeline::playBlock(uint32_t frames) noexcept {  // RT-safe
  const TempoMap& tm = tempo_->tempo();
  const double sr = cfg_.sampleRate;
  const bool notes = seq_ != nullptr && numTracks_ != 0;
  for (uint32_t f = 0; f < frames;) {
    uint32_t len = frames - f;
    const bool wrap = loopOn_ && pos_ < loopEndSample_ && pos_ + len >= loopEndSample_;
    if (wrap) len = static_cast<uint32_t>(loopEndSample_ - pos_);
    const SampleTime s1 = pos_ + len;
    bool ticked = false;
    Ticks t1 = 0;
    if (notes) {
      // The earliest pending event of any track, as a sample: t < firstTickAtOrAfterSample(s1) ⟺ ticksToSamples(t)
      // < s1, so a run without events costs one compare, whatever the number of tracks (and no tick conversion).
      refreshMinNext();
      if (minNextSample_ < s1) {
        const Ticks t0 = resolveTick();
        t1 = std::max(tm.firstTickAtOrAfterSample(s1, sr), t0);
        ticked = true;
        if (t1 > t0) {
          const Ticks* nx = next_.data();
          for (uint32_t k = 0; k < numTracks_; ++k)
            if (nx[k] < t1) playTrack(k, f, pos_, len, t0, t1);  // idle tracks: one compare each
        }
      }
    }
    renderAutomation(f, pos_, len, f == 0 && len == frames);
    f += len;
    if (wrap) {
      ++stats_.loopWraps;
      pos_ = loopStartSample_;
      nextTick_ = loopStart_;
      tickLazy_ = false;
      if (f < frames) {
        releaseAll(f);
        seekAll(loopStart_);
      } else {  // the wrap sample is the next block's frame 0
        releasePending_ = true;
        seekPending_ = true;
      }
    } else {
      pos_ = s1;
      nextTick_ = t1;
      tickLazy_ = !ticked;  // firstTickAtOrAfterSample(pos_), computed when needed
    }
  }
}

void Timeline::renderAutomation(uint32_t frame, SampleTime s0, uint32_t len, bool wholeBlock) noexcept {  // RT-safe
  if (automation_.numLanes() == 0) return;
  const TempoMap& tm = tempo_->tempo();
  const double sr = cfg_.sampleRate;
  const double tA = s0 == lastTickSample_ ? lastTickValue_ : tm.samplesToTicks(s0, sr);
  const double tB = tm.samplesToTicks(s0 + len, sr);
  lastTickSample_ = s0 + len;
  lastTickValue_ = tB;
  TickSpan ts;
  ts.first = tA;
  if (tempo_->constantBetween(tA, tB)) {
    ts.step = (tB - tA) / static_cast<double>(len);
    exactFrom_ = s0;
    ts.exact = [](const void* user, double* out, uint32_t n) noexcept {  // RT-safe
      const Timeline& tl = *static_cast<const Timeline*>(user);
      for (uint32_t i = 0; i < n; ++i) out[i] = tl.tempo_->tempo().samplesToTicks(tl.exactFrom_ + i, tl.cfg_.sampleRate);
    };
    ts.user = this;
  } else {
    for (uint32_t i = 0; i < len; ++i) tickBuf_[i] = tm.samplesToTicks(s0 + i, sr);
    ts.perSample = tickBuf_.data();
  }
  automation_.render(ts, frame, len, wholeBlock);
}

void Timeline::computeSnapshot() noexcept {  // RT-safe
  const TempoMap& tm = tempo_->tempo();
  const TimeSigMap& sm = tempo_->signatures();
  const double sr = cfg_.sampleRate;
  // Playing: the exact time of frame 0 (within half a sample of a located tick); stopped: the held tick.
  const double exact = playing_ ? tm.samplesToTicks(pos_, sr) : static_cast<double>(resolveTick());
  const Ticks t = static_cast<Ticks>(std::floor(exact));
  TransportSnapshot& s = snapshot_;
  s.playing = playing_;
  if (t >= bpmFrom_ && t < bpmTo_) {  // tempo cache: a lookup only on leaving a constant segment, or while ramping
    s.bpm = bpmConst_;
  } else {
    s.bpm = tm.bpmAt(t);
    Ticks from = 0, to = 0;
    if (tempo_->segmentAt(t, &from, &to)) {
      bpmFrom_ = from;
      bpmTo_ = to;
      bpmConst_ = s.bpm;
    } else {
      bpmFrom_ = bpmTo_ = 0;
    }
  }
  s.beat = exact / static_cast<double>(kPpq);
  s.beatsPerFrame = playing_ ? s.bpm / (60.0 * sr) : 0.0;
  if (t < barStartTick_ || t >= barEndTick_) {  // bar cache: a bar lookup only when the playhead leaves the bar
    const BarBeatTick bbt = sm.ticksToBarBeat(t);
    barStartTick_ = sm.barToTicks(bbt.bar);
    barEndTick_ = sm.barToTicks(bbt.bar + 1);
    beatsPerBar_ = static_cast<double>(barEndTick_ - barStartTick_) / static_cast<double>(kPpq);
  }
  s.beatsPerBar = beatsPerBar_;
  s.barStartBeat = static_cast<double>(barStartTick_) / static_cast<double>(kPpq);
}

void Timeline::process(int64_t sampleTime, uint32_t frames) noexcept {  // RT-safe
  if (sampleTime == lastSampleTime_) return;  // this block was processed already (another hook)
  lastSampleTime_ = sampleTime;
  frames = std::min(frames, cfg_.maxBlockFrames);
  if (anyEvents_) {
    std::memset(eventCount_.data(), 0, sizeof(uint32_t) * eventCount_.size());
    anyEvents_ = false;
  }
  drainCommands();
  pickUpImages();
  if (releasePending_) {
    releaseAll(0);
    releasePending_ = false;
  }
  if (seekPending_) {
    seekAll(resolveTick());
    seekPending_ = false;
  }
  if (playing_ || snapshotDirty_) {
    computeSnapshot();
    snapshotDirty_ = false;
  }
  if (reportPending_ || (playing_ && framesSinceReport_ >= cfg_.reportEveryFrames)) {
    framesSinceReport_ = 0;
    TransportReport r;
    r.block = stats_.blocks;
    r.tick = resolveTick();
    r.songSample = pos_;
    r.bpm = snapshot_.bpm;
    r.loopWraps = stats_.loopWraps;
    r.playing = playing_;
    r.commands = commandsPosted_;
    if (!reports_.tryPush(r)) ++stats_.droppedReports;
    reportPending_ = false;
  }
  if (frames != 0) {
    if (playing_) {
      playBlock(frames);
      holdValid_ = false;
    } else if (!holdValid_) {
      if (automation_.numLanes() != 0) automation_.renderHold(static_cast<double>(resolveTick()), 0, frames, true);
      holdValid_ = true;  // renderHold(wholeBlock) filled whole buffers: nothing to do until something changes
    }
  }
  framesSinceReport_ += frames;
  ++stats_.blocks;
  reclaimer_.quiescent(reader_);
}

void Timeline::writeAutomatedParams(std::span<float> params) const noexcept {  // RT-safe
  const uint32_t n = automation_.numLanes();
  for (uint32_t l = 0; l < n; ++l) {
    const uint32_t t = automation_.target(l);
    if (t < params.size()) params[t] = automation_.buffer(l)[0];
  }
}

void Timeline::onBlockHook(void* self, int64_t sampleTime, uint32_t frames, TransportSnapshot& transport,
                           std::span<float> params) noexcept {  // RT-safe
  Timeline& tl = *static_cast<Timeline*>(self);
  tl.process(sampleTime, frames);
  transport = tl.snapshot_;
  tl.writeAutomatedParams(params);
}

void Timeline::onEventSource(void* slot, int64_t sampleTime, uint32_t frames, EventBuffer& out) noexcept {  // RT-safe
  const SourceSlot& s = *static_cast<const SourceSlot*>(slot);
  s.timeline->process(sampleTime, frames);
  const EventList l = s.timeline->events(s.track);
  for (uint32_t i = 0; i < l.count; ++i) out.push(l.events[i]);
}

}  // namespace pq::engine
