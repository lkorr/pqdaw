#include "pq/engine/timeline.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>

namespace pq::engine {

namespace {

// T7.1: the Hz of a note `cents` away from its pitch (exactly hz at 0 ¢). One exp2 per breakpoint. RT-safe.
inline float curveHz(float hz, float cents) noexcept {
  return static_cast<float>(static_cast<double>(hz) * std::exp2(static_cast<double>(cents) * (1.0 / 1200.0)));
}

}  // namespace

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
  chaseScratch_.assign(cfg_.maxSoundingPerTrack, 0);
  sources_.resize(cfg_.maxTracks);
  for (uint32_t k = 0; k < cfg_.maxTracks; ++k) sources_[k] = {this, k};
  automation_.prepare(cfg_.maxAutomationLanes, cfg_.maxBlockFrames);
  tickBuf_ = AlignedBuffer<double>(cfg_.maxBlockFrames);
  updateLoopSamples();
  ClipPlayerConfig cp;  // T5.4
  cp.slots = cfg_.clipSlots != 0 ? cfg_.clipSlots : cfg_.maxTracks;
  cp.maxBlockFrames = cfg_.maxBlockFrames;
  cp.sampleRate = cfg_.sampleRate;
  cp.maxVoices = cfg_.maxClipVoices;
  cp.streamBuffers = cfg_.clipStreamBuffers;
  cp.streams = cfg_.streams;
  cp.lookaheadSeconds = cfg_.clipLookaheadSeconds;
  cp.declickSeconds = cfg_.clipDeclickSeconds;
  clips_ = std::make_unique<ClipPlayer>(cp);
  if (cfg_.sampleReclaimer != nullptr) {
    if (auto r = cfg_.sampleReclaimer->registerReader()) {
      sampleReclaimer_ = cfg_.sampleReclaimer;
      sampleReader_ = *r;
    } else {
      sampleReaderFailed_ = true;  // TMM.13-fix1 L4: Engine::create refuses this timeline
    }
  }
}

Timeline::~Timeline() {
  if (auto_ != nullptr)  // audio-rate consumers must not keep pointers into this timeline's buffers
    for (const AutomationImage::Lane& l : auto_->lanes())
      if (l.tap != nullptr) *l.tap = nullptr;
  clips_.reset();  // T5.4: its voices release their streams; they read the audio images below
  delete pubSeq_.exchange(nullptr);
  delete pubAuto_.exchange(nullptr);
  delete pubTempo_.exchange(nullptr);
  delete pubAudio_.exchange(nullptr);
  StretchPool::destroy(pubStretch_.exchange(nullptr));
  for (const auto& [p, deleter] : held_) deleter(p);
  held_.clear();
  reclaimer_.unregisterReader(reader_);
  if (sampleReclaimer_ != nullptr) sampleReclaimer_->unregisterReader(sampleReader_);
}

Status Timeline::publishSequence(std::unique_ptr<SequenceImage> image) {
  if (image == nullptr) return Error{ErrorCode::InvalidArgument, "Timeline: null sequence image", {}};
  if (image->numTracks() > cfg_.maxTracks)
    return Error{ErrorCode::InvalidArgument,
                 "Timeline: " + std::to_string(image->numTracks()) + " tracks (maximum " +
                     std::to_string(cfg_.maxTracks) + ")",
                 {}};
  if (SequenceImage* old = pubSeq_.exchange(image.release(), std::memory_order_seq_cst))
    retireImage(old, [](void* q) noexcept { delete static_cast<SequenceImage*>(q); });
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
  if (AutomationImage* old = pubAuto_.exchange(image.release(), std::memory_order_seq_cst))
    retireImage(old, [](void* q) noexcept { delete static_cast<AutomationImage*>(q); });
  collect();
  return Ok{};
}

Status Timeline::publishTempo(std::unique_ptr<TempoImage> image) {
  if (image == nullptr) return Error{ErrorCode::InvalidArgument, "Timeline: null tempo image", {}};
  if (TempoImage* old = pubTempo_.exchange(image.release(), std::memory_order_seq_cst))
    retireImage(old, [](void* q) noexcept { delete static_cast<TempoImage*>(q); });
  collect();
  return Ok{};
}

Status Timeline::publishAudio(std::unique_ptr<AudioSequenceImage> image) {  // T5.4
  if (image == nullptr) return Error{ErrorCode::InvalidArgument, "Timeline: null audio image", {}};
  if (image->numSlots() > clips_->numSlots())
    return Error{ErrorCode::InvalidArgument,
                 "Timeline: " + std::to_string(image->numSlots()) + " clip slots (maximum " +
                     std::to_string(clips_->numSlots()) + ")",
                 {}};
  // T6.x-fix-warp (M6 review H2, L15): grow the clip player's stretch pool to what this image's Complex clips need,
  // here off the audio thread (≈ 0.5 ms and 0.9 MiB per stereo stretcher at 48 kHz); never past its capacity. Pushed
  // before the image, so the audio thread adopts them no later than the block it takes the image.
  if (const StretchPool* pool = clips_->stretchPool()) {
    const ClipPlayer::StretchDemand d = ClipPlayer::stretchDemand(*image);
    std::lock_guard<std::mutex> lock(stretchMutex_);
    uint32_t room = pool->capacity() > stretchPlanned_[0] + stretchPlanned_[1]
                        ? pool->capacity() - stretchPlanned_[0] - stretchPlanned_[1]
                        : 0u;
    const uint32_t addStereo = std::min(room, d.stereo > stretchPlanned_[0] ? d.stereo - stretchPlanned_[0] : 0u);
    room -= addStereo;
    const uint32_t addMono = std::min(room, d.mono > stretchPlanned_[1] ? d.mono - stretchPlanned_[1] : 0u);
    if (addStereo + addMono > 0) {
      StretchPool::Slot* list = StretchPool::build(addStereo, addMono, cfg_.sampleRate, ClipPlayer::kStretchFrames);
      stretchPlanned_[0] += addStereo;
      stretchPlanned_[1] += addMono;
      StretchPool::push(pubStretch_, list);
    }
  }
  if (AudioSequenceImage* old = pubAudio_.exchange(image.release(), std::memory_order_seq_cst))
    retireImage(old, [](void* q) noexcept { delete static_cast<AudioSequenceImage*>(q); });
  collect();
  return Ok{};
}

bool Timeline::hazardous(const void* p) const noexcept {
  for (const Hazard& h : hazard_)
    if (h.cur.load(std::memory_order_seq_cst) == p || h.cand.load(std::memory_order_seq_cst) == p) return true;
  return false;
}

void Timeline::retireImage(void* old, void (*deleter)(void*) noexcept) {
  if (hazardous(old)) held_.emplace_back(old, deleter);  // the audio thread holds it: later (collect)
  else reclaimer_.retire(old, deleter);
}

size_t Timeline::collect() {
  // Held images the audio thread has let go of (it drops one when it takes a newer one, at a block start): to the
  // reclaimer, which frees them once the audio thread has passed a block (or at once while it is offline).
  for (size_t i = 0; i < held_.size();) {
    if (hazardous(held_[i].first)) {
      ++i;
      continue;
    }
    reclaimer_.retire(held_[i].first, held_[i].second);
    held_[i] = held_.back();
    held_.pop_back();
  }
  return reclaimer_.collect();
}

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
    case TransportCommandKind::Record:  // T6.7: a new session; while playing with `here`, nothing else changes
      if (!recording_) ++recordSession_;
      recording_ = true;
      if (cmd.here && (playing_ || countIn_.active)) break;
      [[fallthrough]];
    case TransportCommandKind::Play:
      if (cmd.kind == TransportCommandKind::Play) recording_ = false;
      if (cmd.countInBars > 0) {
        locate(cmd.tick);
        startCountIn(cmd.tick, cmd.countInBars);
        break;
      }
      countIn_.active = false;
      locate(cmd.tick);
      playing_ = true;
      ++playbackPass_;
      loopWrapsInRun_ = 0;
      break;
    case TransportCommandKind::RecordOff:  // T6.7
      recording_ = false;
      break;
    case TransportCommandKind::SetPunch:  // T6.7
      punchEnabled_ = cmd.punchEnabled;
      punchIn_ = cmd.punchIn;
      punchOut_ = cmd.punchOut;
      updateLoopSamples();
      break;
    case TransportCommandKind::Stop:
      recording_ = false;  // T6.7
      if (playing_) releasePending_ = true;
      playing_ = false;
      countIn_.active = false;  // T6.6: cancels a count-in (the position stays where it would have started)
      break;
    case TransportCommandKind::Continue:
      countIn_.active = false;
      if (!playing_) {
        seekPending_ = true;
        ++playbackPass_;
        loopWrapsInRun_ = 0;
      }
      playing_ = true;
      break;
    case TransportCommandKind::Locate:
      locate(cmd.tick);
      if (playing_) ++playbackPass_;
      loopWrapsInRun_ = 0;
      break;
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

void Timeline::startCountIn(Ticks tick, uint32_t bars) noexcept {  // RT-safe
  // The transport holds at `tick` (located by the caller) for `bars` bars of the signature there, at the tempo there.
  if (playing_) releasePending_ = true;
  playing_ = false;
  const TimeSignature sig = tempo_->signatures().signatureAt(tick);
  const double bpm = tempo_->tempo().bpmAt(tick);
  const double quarters = static_cast<double>(bars) * static_cast<double>(ticksPerBar(sig)) / static_cast<double>(kPpq);
  const double exact = quarters * 60.0 / bpm * cfg_.sampleRate;
  countIn_.active = true;
  countIn_.frames = std::max<SampleTime>(1, std::llround(exact));
  countIn_.beats = bars * static_cast<uint32_t>(sig.numerator);
  countIn_.beatsPerBar = static_cast<uint32_t>(sig.numerator);
  countIn_.beatFrames = exact / static_cast<double>(countIn_.beats);
  countIn_.pos = 0;
  ++stats_.countIns;
}

void Timeline::pushRun(uint32_t frame, uint32_t len, SampleTime pos, TransportRun::Kind kind) noexcept {  // RT-safe
  if (numRuns_ == kMaxRuns) {
    ++stats_.droppedRuns;
    return;
  }
  runs_[numRuns_++] = TransportRun{frame, len, pos, kind};
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
  punch_.in = tm.ticksToSamples(punchIn_, cfg_.sampleRate);  // T6.7
  punch_.out = tm.ticksToSamples(punchOut_, cfg_.sampleRate);
  punch_.punch = punchEnabled_ && punch_.out > punch_.in;
}

// The published image of one kind, protected before it is dereferenced (see Timeline::Hazard): `cur` if nothing
// newer is published or the publisher keeps replacing it (then next block). RT-safe: at most 4 rounds.
template <class T>
const T* Timeline::acquireImage(std::atomic<T*>& pub, Hazard& h, const T* cur, const T* ifNull) noexcept {
  const T* p = pub.load(std::memory_order_seq_cst);
  for (int round = 0; round < 4; ++round) {
    const T* v = p != nullptr ? p : ifNull;
    if (v == cur) return cur;
    h.cand.store(p, std::memory_order_seq_cst);
    const T* q = pub.load(std::memory_order_seq_cst);
    if (q == p) return v;  // protected by cand until it is cur
    p = q;
  }
  h.cand.store(nullptr, std::memory_order_release);
  return cur;
}

void Timeline::pickUpImages() noexcept {  // RT-safe
  // Each kind: take the newest published image, swap to it (the swap still reads the old one), then the old one is
  // no longer held (cur) and may be retired.
  const TempoImage* t = acquireImage(pubTempo_, hazard_[2], tempo_, defaultTempo_.get());
  if (t != tempo_) swapTempo(t);
  hazard_[2].cur.store(tempo_, std::memory_order_seq_cst);
  hazard_[2].cand.store(nullptr, std::memory_order_release);
  const SequenceImage* s = acquireImage(pubSeq_, hazard_[0], seq_, static_cast<const SequenceImage*>(nullptr));
  if (s != seq_) swapSequence(s);
  hazard_[0].cur.store(seq_, std::memory_order_seq_cst);
  hazard_[0].cand.store(nullptr, std::memory_order_release);
  const AutomationImage* a = acquireImage(pubAuto_, hazard_[1], auto_, static_cast<const AutomationImage*>(nullptr));
  if (a != auto_) {
    if (auto_ != nullptr)  // the old image's audio-rate consumers read their params again
      for (const AutomationImage::Lane& l : auto_->lanes())
        if (l.tap != nullptr) *l.tap = nullptr;
    auto_ = a;
    automation_.attach(a);
    holdValid_ = false;
    ++stats_.automationSwaps;
  }
  hazard_[1].cur.store(auto_, std::memory_order_seq_cst);
  hazard_[1].cand.store(nullptr, std::memory_order_release);
  // T5.4: an audio image is taken only once the voices of the one before have faded out (ClipPlayer::swapping); the
  // image they read stays protected (hazard 4) until then.
  if (pubStretch_.load(std::memory_order_relaxed) != nullptr)  // T6.x-fix-warp: stretchers for the images to come
    clips_->adoptStretchers(pubStretch_.exchange(nullptr, std::memory_order_acq_rel));
  if (!clips_->swapping()) {
    const AudioSequenceImage* au =
        acquireImage(pubAudio_, hazard_[3], audio_, static_cast<const AudioSequenceImage*>(nullptr));
    if (au != audio_) {
      hazard_[4].cur.store(audio_, std::memory_order_seq_cst);  // before cur moves on: never unprotected
      clips_->setImage(au, pos_, playing_);
      audio_ = au;
      ++stats_.audioSwaps;
    }
  }
  hazard_[3].cur.store(audio_, std::memory_order_seq_cst);
  hazard_[3].cand.store(nullptr, std::memory_order_release);
  hazard_[4].cur.store(clips_->previousImage(), std::memory_order_seq_cst);
}

void Timeline::swapTempo(const TempoImage* next) noexcept {  // RT-safe
  // Keep the musical position: the next tick to play stays, its sample moves.
  const Ticks t = resolveTick();  // under the old map
  tempo_ = next;
  pos_ = next->tempo().ticksToSamples(t, cfg_.sampleRate);
  minNextDirty_ = true;
  updateLoopSamples();
  if (playing_) clips_->releaseAll(0);  // T5.4: the clips' sample positions moved with the map
  clips_->seek(pos_);
  snapshotDirty_ = true;
  holdValid_ = false;
  lastTickSample_ = INT64_MIN;
  barStartTick_ = barEndTick_ = 0;  // the bar and tempo caches (computeSnapshot) are stale
  bpmFrom_ = bpmTo_ = 0;
  ++stats_.tempoSwaps;
  if (playing_) resyncCurvesToTempo();  // T7.1: the running curve ramps get their new lengths
}

void Timeline::swapSequence(const SequenceImage* next) noexcept {  // RT-safe
  const uint32_t newN = next != nullptr ? next->numTracks() : 0u;
  const uint32_t oldN = numTracks_;
  const uint32_t cap = cfg_.maxSoundingPerTrack;
  const Ticks now = resolveTick();
  bool identity = newN == oldN;
  for (uint32_t k = 0; identity && k < newN; ++k) identity = next->tracks()[k].id == tracks_[k].id;

  // Re-validates the sounding notes of `src` (old state) against the new image's track `j`, writing the survivors
  // into `dst.heap` (which may be src.heap itself). Note-offs for the others go to slot `j` at frame 0. A survivor
  // whose pitch changed (a re-resolved note: a tuning edit, a re-pitch) is retuned at frame 0 by a Pitch event that
  // also carries its new identity, instead of being cut (T2.6-fix3, M3 review H5).
  // T7.1: a kept note keeps its curve cursor when its curves did not change; otherwise (or when it is retuned and has
  // a pitch curve) it is re-synchronised at frame 0 to the new image's curve.
  // `oldTrack`: src's index in the old image (seq_ until the end of this function), which every sounding note is in:
  // it started from it, or the last swap kept it because it was there. The identity it sounds with is that note's.
  auto carry = [&](const TrackState& src, uint32_t oldTrack, TrackState& dst, uint32_t j) noexcept {
    uint32_t kept = 0;
    Ticks nextCurve = kNever;
    for (uint32_t i = 0; i < src.heapSize; ++i) {
      Sounding s = src.heap[i];
      Ticks passEnd = 0;
      const SequenceImage::Note* n = next->findOccurrence(j, s.noteId, s.on, &passEnd);
      if (n != nullptr) {
        s.off = offTick(s.on, std::min(s.on + n->length, passEnd));
        if (s.off > now || !playing_) {
          const SequenceImage::Note* o = seq_->findOccurrence(oldTrack, s.noteId, s.on, nullptr);
          const bool retune =
              o == nullptr || std::bit_cast<uint32_t>(n->hz) != std::bit_cast<uint32_t>(s.hz) || !(n->pitch == o->pitch) ||
              std::bit_cast<uint32_t>(n->ratio) != std::bit_cast<uint32_t>(o->ratio) ||
              std::bit_cast<uint32_t>(n->harmonic) != std::bit_cast<uint32_t>(o->harmonic) ||
              n->unisonCount != o->unisonCount || n->unisonIndex != o->unisonIndex;
          const SequenceImage::NoteCurves nc = next->curvesOf(*n);
          const bool hadPitch = s.pitchCount != 0, hasPitch = nc.pitchCount != 0;
          const bool hadGain = s.gainCount != 0, hasGain = nc.gainCount != 0;
          const bool curvesChanged =
              s.pitchCount != nc.pitchCount || s.gainCount != nc.gainCount ||
              ((hadPitch || hadGain) && static_cast<uint64_t>(seq_->curves()[s.curve].tick) != next->curveHash(nc));
          s.curve = nc.begin;
          s.pitchCount = nc.pitchCount;
          s.gainCount = nc.gainCount;
          if (retune) {  // at the curve's value at the block's first sample
            const float cents =
                hasPitch ? SequenceImage::curveValueAt(next->pitchCurve(*n),
                                                       tempo_->tempo().samplesToTicks(pos_, cfg_.sampleRate) -
                                                           static_cast<double>(s.shift),
                                                       0.0f)
                         : 0.0f;
            NoteEvent e = notePitch(0, s.instance, cents != 0.0f ? curveHz(n->hz, cents) : n->hz);
            e.flags = kNoteEventRetune;
            setIdentity(e, *n);
            if (pushEvent(j, e)) ++stats_.retunedOnSwap;
            s.hz = n->hz;
          }
          const CurveOut out{next, j, 0, src.heapSize, pos_};
          const bool syncPitch = retune ? hasPitch : curvesChanged && (hadPitch || hasPitch);
          const bool syncGain = curvesChanged && (hadGain || hasGain);
          // After a retune only the ramp is missing (the retune stepped); else a step onto the new curve first.
          if (syncPitch) curveSync(out, s, CurveKind::Pitch, retune ? CurveSync::Resume : CurveSync::Step, now);
          if (syncGain) curveSync(out, s, CurveKind::Gain, CurveSync::Step, now);
          if (syncPitch || syncGain) ++stats_.curveResyncs;
          if (hasPitch || hasGain) nextCurve = std::min(nextCurve, nextCurveTick(*next, s));
          dst.heap[kept++] = s;
          ++stats_.keptOnSwap;
          continue;
        }
      }
      emitOff(j, s, 0);
      ++stats_.stoppedOnSwap;
    }
    dst.heapSize = kept;
    dst.nextCurve = nextCurve;
    std::make_heap(dst.heap, dst.heap + kept, [](const Sounding& a, const Sounding& b) noexcept {
      return a.off != b.off ? a.off > b.off : a.order > b.order;
    });
  };

  if (identity) {
    for (uint32_t k = 0; k < newN; ++k) carry(tracks_[k], k, tracks_[k], k);
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
        carry(tracks_[i], i, scratchTracks_[j], j);
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
  seqCurves_ = next != nullptr && next->hasCurves();
  numTracks_ = newN;
  for (uint32_t k = newN; k < cfg_.maxTracks; ++k) {
    tracks_[k].heapSize = 0;
    tracks_[k].cur = Cursor{};
    tracks_[k].cur.nextOn = kNever;
    tracks_[k].nextCurve = kNever;
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

// T7.3 (Q278; timeline.h "Chase"): the notes of the pass under `t` that started before it and still sound there
// start at `frame` (song sample `sample`), each a fresh note-on with its pitch and gain read from its curves at t.
// Out of line and only called where the playhead jumped. O(log clips + (found + 1) · log notes) per track.
__declspec(noinline) void Timeline::chase(uint32_t track, Ticks t, uint32_t frame, SampleTime sample) noexcept {  // RT-safe
  const SequenceImage::Track& tr = seq_->tracks()[track];
  const auto clips = seq_->clips();
  const auto* cb = clips.data() + tr.clipBegin;
  const auto* ce = clips.data() + tr.clipEnd;
  const auto* it = std::upper_bound(cb, ce, t, [](Ticks v, const SequenceImage::Clip& cl) noexcept { return v < cl.end; });
  if (it == ce || it->start >= t) return;  // no clip under t, or it starts here: nothing began earlier
  const SequenceImage::Clip& cl = *it;
  int64_t pass = 0;
  Ticks passStart = cl.start, passContent = cl.content0, passEnd = cl.firstPassEnd;
  uint32_t lo = cl.pass0Begin, hi = cl.pass0End;
  if (cl.loopLen != 0 && t >= cl.firstPassEnd) {
    const Ticks k = (t - cl.firstPassEnd) / cl.loopLen;
    pass = 1 + k;
    passStart = cl.firstPassEnd + k * cl.loopLen;
    passContent = cl.loopStart;
    passEnd = std::min(passStart + cl.loopLen, cl.end);
    lo = cl.loopBegin;
    hi = cl.loopEnd;
  }
  TrackState& ts = tracks_[track];
  const uint32_t found = seq_->covering(cl, lo, hi, passContent + (t - passStart), chaseScratch_.data(),
                                        static_cast<uint32_t>(chaseScratch_.size()));
  const auto notes = seq_->notes();
  const TempoMap& tm = tempo_->tempo();
  for (uint32_t i = 0; i < found; ++i) {
    const SequenceImage::Note& n = notes[chaseScratch_[i]];
    const Ticks on = passStart + (n.start - passContent);
    const Ticks off = std::min(on + n.length, passEnd);
    if (off <= t || tm.ticksToSamples(off, cfg_.sampleRate) <= sample) continue;  // over within this sample
    const NoteRandom r = noteRandom(seq_->seed() ^ (playbackPass_ * 0x9E3779B97F4A7C15ull), n.id, pass);
    if (!(r.chance < n.chance)) {
      ++stats_.chanceSkips;
      continue;
    }
    if (eventCount_[track] + 2 + ts.heapSize > cfg_.maxEventsPerTrack || ts.heapSize >= cfg_.maxSoundingPerTrack) {
      ++stats_.droppedNoteOns;
      continue;
    }
    const float vel = std::clamp(n.velocity + r.velocity * n.velocityRange, 1.0f, 127.0f) * (1.0f / 127.0f);
    NoteEvent e = noteOn(frame, nextInstance_, n.hz, vel);
    setIdentity(e, n);
    const float release = std::clamp(n.releaseVelocity, 0.0f, 127.0f) * (1.0f / 127.0f);
    const SequenceImage::NoteCurves nc = seq_->curvesOf(n);
    const Sounding s{off, ts.order++, on, on - n.start, nextInstance_, n.id, n.hz, release, nc.begin,
                     nc.pitchCount, nc.gainCount, 0, 0};
    if (!nc.any()) {
      pushEvent(track, e);
      heapPush(ts, s);
    } else {
      startCurvedNote(track, e, s, frame, sample, t);
    }
    ++stats_.noteOns;
    ++stats_.chasedNotes;
    if (++nextInstance_ >= 0xE0000000u) nextInstance_ = 1;
  }
  refreshNext(track);
}

void Timeline::chaseAll(Ticks t, uint32_t frame) noexcept {  // RT-safe
  if (seq_ == nullptr) return;
  for (uint32_t k = 0; k < numTracks_; ++k) chase(k, t, frame, pos_);
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
  next_[track] = std::min(std::min(t.cur.nextOn, t.heapSize != 0 ? t.heap[0].off : kNever), t.nextCurve);
  minNextDirty_ = true;
}

// ---- Audio thread: note curves (T7.1) ----------------------------------------------------------------------------

Ticks Timeline::nextCurveTick(const SequenceImage& image, const Sounding& s) const noexcept {  // RT-safe
  const CurvePoint* p = image.curves().data() + s.curve + 1;
  Ticks t = kNever;
  if (s.pitchNext < s.pitchCount) t = p[s.pitchNext].tick + s.shift;
  if (s.gainNext < s.gainCount) t = std::min(t, p[s.pitchCount + s.gainNext].tick + s.shift);
  return t;
}

float Timeline::curveLocate(const SequenceImage& image, Sounding& s, CurveKind kind, Ticks at,
                            SampleTime sample) const noexcept {  // RT-safe
  const bool pitch = kind == CurveKind::Pitch;
  const std::span<const CurvePoint> curve{image.curves().data() + s.curve + 1 + (pitch ? 0u : s.pitchCount),
                                          pitch ? s.pitchCount : s.gainCount};
  const float empty = pitch ? 0.0f : 1.0f;
  uint32_t after = 0;
  float v = SequenceImage::curveValue(curve, at - s.shift, empty, &after);  // O(log points)
  (pitch ? s.pitchNext : s.gainNext) = static_cast<uint16_t>(after);
  if (sample != INT64_MIN && !curve.empty())
    v = SequenceImage::curveValueAt(
        curve, tempo_->tempo().samplesToTicks(sample, cfg_.sampleRate) - static_cast<double>(s.shift), empty);
  return v;
}

bool Timeline::pushCurveEvent(const CurveOut& o, const NoteEvent& e) noexcept {  // RT-safe
  // Room stays for every pending note-off (as for note-ons).
  if (eventCount_[o.track] + o.pendingOffs >= cfg_.maxEventsPerTrack || !pushEvent(o.track, e)) {
    ++stats_.droppedCurveEvents;
    return false;
  }
  ++stats_.curveEvents;
  return true;
}

void Timeline::curveRamp(const CurveOut& o, const Sounding& s, CurveKind kind, float value) noexcept {  // RT-safe
  const bool pitch = kind == CurveKind::Pitch;
  const uint32_t next = pitch ? s.pitchNext : s.gainNext;
  if (next >= (pitch ? s.pitchCount : s.gainCount)) return;
  const CurvePoint& p = o.image->curves()[s.curve + 1 + (pitch ? 0u : s.pitchCount) + next];
  if (std::bit_cast<uint32_t>(p.value) == std::bit_cast<uint32_t>(value)) return;  // flat: nothing moves
  const SampleTime d = tempo_->tempo().ticksToSamples(p.tick + s.shift, cfg_.sampleRate) - o.sample;
  const auto ramp = static_cast<uint32_t>(std::clamp<SampleTime>(d, 0, 0xFFFFFFFFll));
  pushCurveEvent(o, pitch ? notePitch(o.frame, s.instance, curveHz(s.hz, p.value), ramp)
                          : noteGain(o.frame, s.instance, p.value, ramp));
}

void Timeline::curveSync(const CurveOut& o, Sounding& s, CurveKind kind, CurveSync mode, Ticks at) noexcept {  // RT-safe
  const bool pitch = kind == CurveKind::Pitch;
  bool step = mode == CurveSync::Step;
  uint32_t arriving = 0;
  if (mode == CurveSync::Breakpoint)  // the value the running ramp ends on
    arriving = std::bit_cast<uint32_t>(
        o.image->curves()[s.curve + 1 + (pitch ? s.pitchNext : static_cast<uint32_t>(s.pitchCount) + s.gainNext)].value);
  const float value = curveLocate(*o.image, s, kind, at, mode == CurveSync::Breakpoint ? INT64_MIN : o.sample);
  if (mode == CurveSync::Breakpoint) step = std::bit_cast<uint32_t>(value) != arriving;  // a jump
  if (step) pushCurveEvent(o, pitch ? notePitch(o.frame, s.instance, curveHz(s.hz, value)) : noteGain(o.frame, s.instance, value));
  curveRamp(o, s, kind, value);
}

// Out of line (as playCurves): the sequencer's loop over notes without curves stays as small as it was.
__declspec(noinline) void Timeline::startCurvedNote(uint32_t track, NoteEvent e, Sounding s, uint32_t frame,
                                                    SampleTime sample, Ticks at) noexcept {  // RT-safe
  TrackState& tr = tracks_[track];
  const float cents = curveLocate(*seq_, s, CurveKind::Pitch, at), gain = curveLocate(*seq_, s, CurveKind::Gain, at);
  if (cents != 0.0f) e.hz = curveHz(s.hz, cents);
  e.gain = gain;
  pushEvent(track, e);
  const CurveOut o{seq_, track, frame, tr.heapSize + 1, sample};  // + 1: this note's own off
  curveRamp(o, s, CurveKind::Pitch, cents);
  curveRamp(o, s, CurveKind::Gain, gain);
  tr.nextCurve = std::min(tr.nextCurve, nextCurveTick(*seq_, s));
  heapPush(tr, s);
}

__declspec(noinline) void Timeline::playCurves(uint32_t track, Ticks at, uint32_t frame, SampleTime sample,
                                               CurveSync mode) noexcept {  // RT-safe
  TrackState& tr = tracks_[track];
  const CurvePoint* curves = seq_->curves().data();
  const CurveOut o{seq_, track, frame, tr.heapSize, sample};
  Ticks mn = kNever;
  for (uint32_t i = 0; i < tr.heapSize; ++i) {
    Sounding& s = tr.heap[i];  // the cursor is not part of the heap order
    if ((s.pitchCount | s.gainCount) == 0) continue;
    const CurvePoint* p = curves + s.curve + 1;
    if (s.pitchNext < s.pitchCount && p[s.pitchNext].tick + s.shift <= at) curveSync(o, s, CurveKind::Pitch, mode, at);
    if (s.gainNext < s.gainCount && p[s.pitchCount + s.gainNext].tick + s.shift <= at) curveSync(o, s, CurveKind::Gain, mode, at);
    mn = std::min(mn, nextCurveTick(*seq_, s));
  }
  tr.nextCurve = mn;
}

void Timeline::resyncCurvesToTempo() noexcept {  // RT-safe
  const Ticks now = resolveTick();
  for (uint32_t k = 0; k < numTracks_; ++k) {
    TrackState& tr = tracks_[k];
    if (tr.nextCurve == kNever) continue;
    for (uint32_t i = 0; i < tr.heapSize; ++i) {
      Sounding& s = tr.heap[i];
      if ((s.pitchCount | s.gainCount) == 0) continue;
      const CurveOut o{seq_, k, 0, tr.heapSize, pos_};
      curveSync(o, s, CurveKind::Pitch, CurveSync::Resume, now);
      curveSync(o, s, CurveKind::Gain, CurveSync::Resume, now);
      ++stats_.curveResyncs;
    }
    refreshNext(k);
  }
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

void Timeline::setIdentity(NoteEvent& e, const SequenceImage::Note& n) noexcept {  // RT-safe
  e.ratio = n.ratio;
  e.harmonic = n.harmonic;
  e.noteDegree = n.keyDegree;
  e.unisonCount = n.unisonCount;  // T7.3
  e.unisonIndex = n.unisonIndex;
  if (n.pitch.tuning != 0) {  // T2.6: a note following a tuning signal (T2.6-fix3: which node, and how)
    e.tuning = static_cast<float>(n.pitch.node);
    if (n.pitch.kind == PitchFollowKind::Degree) {
      e.degree = static_cast<float>(n.pitch.degree);
      e.equave = static_cast<float>(n.pitch.equave);
    }
  }
}

Ticks Timeline::offTick(Ticks on, Ticks off) const noexcept {  // RT-safe
  const TempoMap& tm = tempo_->tempo();
  const double sr = cfg_.sampleRate;
  if (tm.ticksToSamples(off, sr) > tm.ticksToSamples(on, sr)) return off;  // the common case: one conversion each
  return std::max(off, tm.firstTickAtOrAfterSample(tm.ticksToSamples(on, sr) + 1, sr));
}

void Timeline::releaseAll(uint32_t frame) noexcept {  // RT-safe
  clips_->releaseAll(frame);  // T5.4: sounding clips fade out from `frame`
  for (uint32_t k = 0; k < cfg_.maxTracks; ++k) {
    TrackState& t = tracks_[k];
    while (t.heapSize != 0) emitOff(k, heapPop(t), frame);
    t.nextCurve = kNever;
    next_[k] = t.cur.nextOn;
    minNextDirty_ = true;
  }
}

void Timeline::playTrack(uint32_t track, uint32_t frame, SampleTime s0, uint32_t len, Ticks t0,
                         Ticks t1) noexcept {  // RT-safe
  if (seqCurves_) playTrackImpl<true>(track, frame, s0, len, t0, t1);
  else playTrackImpl<false>(track, frame, s0, len, t0, t1);
}

template <bool Curves>
void Timeline::playTrackImpl(uint32_t track, uint32_t frame, SampleTime s0, uint32_t len, Ticks t0,
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
  bool collapsed = false;  // T7.1: the breakpoint limit was hit (see "Note curves" in timeline.h)
  for (;;) {
    const Ticks on = c.nextOn;
    const Ticks off = tr.heapSize != 0 ? tr.heap[0].off : kNever;
    // T7.1: the track's next breakpoint (an image without curves has none: nextCurve is kNever).
    const Ticks cv = Curves && !collapsed ? tr.nextCurve : kNever;
    if (off <= on && off <= cv) {  // offs first at equal ticks
      if (off >= t1) break;
      emitOff(track, heapPop(tr), frameOf(off));
      continue;
    }
    if constexpr (Curves) {
      if (cv <= on) {  // then the breakpoints of sounding notes, then ons
        if (cv >= t1) break;
        if (++looked > lookLimit) {  // too many in one block: one step per note at the block's last tick (below)
          ++stats_.curveCollapses;
          collapsed = true;
          continue;
        }
        const uint32_t f = frameOf(cv);
        playCurves(track, cv, f, s0 + (f - frame), CurveSync::Breakpoint);
        continue;
      }
    }
    if (on >= t1) break;
    if (++looked > lookLimit) {
      ++stats_.droppedNoteOns;
      seek(track, t1);  // nextOn ≥ t1 now: only note-offs remain in this range
      continue;
    }
    const SequenceImage::Note& n = notes[c.note];
    const NoteRandom r = noteRandom(seq_->seed() ^ (playbackPass_ * 0x9E3779B97F4A7C15ull), n.id, c.pass);
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
      setIdentity(e, n);
      const Ticks offAt = offTick(on, std::min(on + n.length, c.passEnd));
      const float release = std::clamp(n.releaseVelocity, 0.0f, 127.0f) * (1.0f / 127.0f);
      // T7.1: a note with curves takes its own (out-of-line) path; this one stays what it was before curves existed.
      SequenceImage::NoteCurves nc;
      if constexpr (Curves) nc = seq_->curvesOf(n);
      if (!nc.any()) [[likely]] {
        pushEvent(track, e);
        heapPush(tr, Sounding{offAt, tr.order++, on, on - n.start, nextInstance_, n.id, n.hz, release, 0, 0, 0, 0, 0});
      } else {
        startCurvedNote(track, e,
                        Sounding{offAt, tr.order++, on, on - n.start, nextInstance_, n.id, n.hz, release, nc.begin,
                                 nc.pitchCount, nc.gainCount, 0, 0},
                        f, s0 + (f - frame), on);
      }
      ++stats_.noteOns;
      if (++nextInstance_ >= 0xE0000000u) nextInstance_ = 1;  // T6.4: live notes (0xE…) and auditions (0xF…) above
    }
    ++c.note;
    computeNextOn(c);
  }
  if (collapsed) {  // after every other event of the range: this is its last frame
    const uint32_t f = frameOf(t1 - 1);
    playCurves(track, t1 - 1, f, s0 + (f - frame), CurveSync::Step);
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

void Timeline::playBlock(uint32_t from, uint32_t frames) noexcept {  // RT-safe
  const TempoMap& tm = tempo_->tempo();
  const double sr = cfg_.sampleRate;
  const bool notes = seq_ != nullptr && numTracks_ != 0;
  for (uint32_t f = from; f < frames;) {
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
    clips_->play(f, pos_, len);  // T5.4
    // T6.5b: the last look-ahead before a loop wrap primes the Complex voices the loop start needs (ClipPlayer::cue).
    if (loopOn_ && pos_ < loopEndSample_ && loopEndSample_ - s1 <= clips_->cueLead()) clips_->cue(loopStartSample_, len);
    renderAutomation(f, pos_, len, f == 0 && len == frames);
    pushRun(f, len, pos_, TransportRun::Song);  // T6.6: the metronome's beats
    f += len;
    if (wrap) {
      ++stats_.loopWraps;
      ++playbackPass_;
      ++loopWrapsInRun_;
      pos_ = loopStartSample_;
      nextTick_ = loopStart_;
      tickLazy_ = false;
      if (f < frames) {
        releaseAll(f);
        seekAll(loopStart_);
        chaseAll(loopStart_, f);  // T7.3
        clips_->seek(pos_);
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
  automation_.render(tickSpan(s0, len, tA, tB), frame, len, wholeBlock);
  // T6.2 / T6.2b: the lanes of compensated targets, at the song samples `shift` frames earlier (per sample: their taps
  // too). Two tick conversions per shifted lane per run.
  // M6 review M1: after a loop wrap, the frames whose shifted sample lies before the loop start take it from the end
  // of the pass the delay lines replay then (a run splits at most once per pass it reaches back).
  const AutomationImage& img = *automation_.image();
  const SampleTime loopLen = loopEndSample_ - loopStartSample_;
  for (const uint32_t l : img.shiftedLanes()) {
    if (l >= automation_.numLanes()) break;
    const auto shift = static_cast<SampleTime>(img.lanes()[l].shift);
    if (loopWrapsInRun_ == 0 || !loopOn_ || s0 - shift >= loopStartSample_) {
      const SampleTime s = s0 - shift;
      automation_.renderLane(l, tickSpan(s, len, tm.samplesToTicks(s, sr), tm.samplesToTicks(s + len, sr)), frame, len,
                             wholeBlock);
      continue;
    }
    for (uint32_t i = 0; i < len;) {
      SampleTime s = s0 + i - shift;
      for (uint64_t back = 0; s < loopStartSample_ && back < loopWrapsInRun_; ++back) s += loopLen;
      // Up to the end of that pass (s < loopEnd after a step back), or to the loop start of this one.
      const SampleTime room = s0 + i - shift < loopStartSample_ ? std::min(loopEndSample_ - s, loopStartSample_ - (s0 + i - shift))
                                                               : static_cast<SampleTime>(len - i);
      const auto n = static_cast<uint32_t>(std::min<SampleTime>(room, len - i));
      automation_.renderLane(l, tickSpan(s, n, tm.samplesToTicks(s, sr), tm.samplesToTicks(s + n, sr)), frame + i, n,
                             wholeBlock && n == len);
      i += n;
    }
  }
}

TickSpan Timeline::tickSpan(SampleTime s0, uint32_t len, double tA, double tB) noexcept {  // RT-safe
  const TempoMap& tm = tempo_->tempo();
  const double sr = cfg_.sampleRate;
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
  return ts;
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
  clips_->beginBlock(frames);
  numRuns_ = 0;
  drainCommands();
  pickUpImages();
  if (releasePending_) {
    releaseAll(0);
    releasePending_ = false;
  }
  if (seekPending_) {
    seekAll(resolveTick());
    if (playing_) chaseAll(resolveTick(), 0);  // T7.3: the notes under the new position
    clips_->seek(pos_);
    seekPending_ = false;
  }
  if (playing_ || snapshotDirty_) {
    computeSnapshot();
    snapshotDirty_ = false;
  }
  if (reportPending_ || ((playing_ || countIn_.active) && framesSinceReport_ >= cfg_.reportEveryFrames)) {
    framesSinceReport_ = 0;
    TransportReport r;
    r.block = stats_.blocks;
    r.tick = resolveTick();
    r.songSample = pos_;
    r.bpm = snapshot_.bpm;
    r.loopWraps = stats_.loopWraps;
    r.playing = playing_;
    r.commands = commandsPosted_;
    if (countIn_.active) {  // T6.6
      r.countingIn = true;
      r.countInBeats = countIn_.beats;
      r.countInBeatsPerBar = countIn_.beatsPerBar;
      r.countInBeat = std::min(countIn_.beats - 1,
                               static_cast<uint32_t>(static_cast<double>(countIn_.pos) / countIn_.beatFrames));
    }
    r.recording = recording_;  // T6.7
    r.recordSession = recordSession_;
    if (!reports_.tryPush(r)) ++stats_.droppedReports;
    reportPending_ = false;
  }
  if (frames != 0) {
    uint32_t from = 0;
    if (countIn_.active) {  // T6.6: count frames in; the transport starts at the frame where the bars end
      const SampleTime left = countIn_.frames - countIn_.pos;
      const uint32_t n = left >= static_cast<SampleTime>(frames) ? frames : static_cast<uint32_t>(left);
      pushRun(0, n, countIn_.pos, TransportRun::CountIn);
      clips_->cue(pos_, n);  // T6.5b: the Complex voices the start needs prime during the count-in
      countIn_.pos += n;
      if (countIn_.pos >= countIn_.frames) {
        countIn_.active = false;
        clips_->seek(pos_);  // T6.5b: adopts the cue
        playing_ = true;
        ++playbackPass_;
        loopWrapsInRun_ = 0;
        snapshotDirty_ = true;
        reportPending_ = true;
        from = n;
        if (from < frames) chaseAll(resolveTick(), from);  // T7.3: the notes under the start
        else seekPending_ = true;                          // the start is the next block's frame 0
        if (from > 0) {  // the frames before the start hold, as while stopped
          if (clips_->liveVoices() != 0) clips_->idle(from);
          if (!holdValid_ && automation_.numLanes() != 0)
            automation_.renderHold(static_cast<double>(resolveTick()), 0, from, false);
        }
      }
    }
    if (playing_) {
      playBlock(from, frames);
      holdValid_ = false;
    } else {
      if (clips_->liveVoices() != 0) clips_->idle(frames);  // T5.4: fades after a stop
      clips_->cue(pos_, frames);  // T6.5b: Play from here starts the Complex voices primed (ClipPlayer::cue)
      if (!holdValid_) {
        if (automation_.numLanes() != 0) automation_.renderHold(static_cast<double>(resolveTick()), 0, frames, true);
        holdValid_ = true;  // renderHold(wholeBlock) filled whole buffers: nothing to do until something changes
      }
    }
  }
  clips_->endBlock();
  framesSinceReport_ += frames;
  ++stats_.blocks;
  reclaimer_.quiescent(reader_);
  if (sampleReclaimer_ != nullptr) sampleReclaimer_->quiescent(sampleReader_);
}

void Timeline::writeAutomatedParams(std::span<float> params) noexcept {  // RT-safe
  const uint32_t n = automation_.numLanes();
  const auto lanes = n != 0 ? automation_.image()->lanes() : std::span<const AutomationImage::Lane>{};
  bool changed = false;
  for (uint32_t l = 0; l < n; ++l) {
    const uint32_t t = automation_.target(l);
    if (t < params.size()) {  // M4 review L1: only a changed value counts as a write (a held lane changes nothing)
      const float v = automation_.buffer(l)[0];  // (a shifted lane's buffer is rendered shifted: renderAutomation)
      changed |= std::bit_cast<uint32_t>(params[t]) != std::bit_cast<uint32_t>(v);
      params[t] = v;
    }
    if (lanes[l].tap != nullptr) *lanes[l].tap = automation_.constant(l) ? nullptr : automation_.buffer(l);
  }
  if (changed) ++paramWrites_;
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
