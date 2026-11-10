// The engine (T3.17): pq/engine/engine.h describes it.
#include "pq/engine/engine.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>

#include "pq/core/aligned_buffer.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "render_loop.h"

namespace pq::engine {

namespace {
// Output at or below this is silence for the idle gate (−120 dBFS).
constexpr float kSleepPeak = 1e-6f;
}  // namespace

Result<std::unique_ptr<Engine>> Engine::create(const EngineConfig& config) {
  if (!(config.sampleRate >= 1000.0 && config.sampleRate <= 768000.0))
    return Error{ErrorCode::InvalidArgument, "engine: bad sample rate", {}};
  if (config.blockFrames == 0 || config.blockFrames > 4096)
    return Error{ErrorCode::InvalidArgument, "engine: block size must be 1..4096 frames", {}};
  if (config.numOut == 0 || config.numOut > kMaxChannels)
    return Error{ErrorCode::InvalidArgument, "engine: bad channel count", {}};
  if (config.maxSlots == 0) return Error{ErrorCode::InvalidArgument, "engine: no event slots", {}};
  std::unique_ptr<Engine> e(new Engine(config));
  // TMM.13-fix1 L4: a timeline that could not register with the sample reclaimer would read frame-table entries and
  // sample residencies without QSBR protection (freed under it): refused, loudly.
  if (!e->timeline_->sampleReaderOk())
    return Error{ErrorCode::Overflow, "engine: the sample reclaimer has no reader slot left (too many engines share it)", {}};
  return e;
}

Engine::Engine(const EngineConfig& config)
    : cfg_(config),
      auditionIn_(std::bit_ceil(std::max(config.auditionRing, 2u))) {
  TimelineConfig tc = cfg_.timeline;
  tc.sampleRate = cfg_.sampleRate;
  tc.maxBlockFrames = cfg_.blockFrames;
  tc.maxTracks = cfg_.maxSlots;
  timeline_ = std::make_unique<Timeline>(tc);
  LiveInputConfig lc = cfg_.live;
  lc.sampleRate = cfg_.sampleRate;
  lc.maxSlots = cfg_.maxSlots;
  live_ = std::make_unique<LiveInput>(lc, cfg_.livePorts);
  metronome_ = std::make_unique<Metronome>(cfg_.sampleRate);
  timelineHook_ = timeline_->blockHook();
  for (uint32_t k = 0; k < cfg_.maxSlots; ++k) slots_.push_back({this, k});
  paramRings_.push_back(std::make_unique<ParamRing>(std::bit_ceil(std::max(cfg_.paramRing, 2u))));
  paramProducer_ = paramConsumer_ = paramRings_.back().get();
  paramConsumed_.store(paramConsumer_, std::memory_order_relaxed);
  auditions_.resize(std::max(cfg_.maxAuditions, 1u));
  sleepFrames_ = cfg_.sleepAfterMs > 0.0f
                     ? static_cast<uint64_t>(std::ceil(static_cast<double>(cfg_.sleepAfterMs) * 1e-3 * cfg_.sampleRate))
                     : 0;
}

Engine::~Engine() {
  program_.reset();
  timeline_.reset();
}

ProjectConfig Engine::projectConfig() noexcept {
  ProjectConfig pc;
  pc.sampleRate = cfg_.sampleRate;
  pc.maxBlockFrames = cfg_.blockFrames;
  pc.numOut = cfg_.numOut;
  pc.hook = {&Engine::onHook, this, timelineHook_.paramWrites};  // T6.6: the timeline's hook + the metronome
  pc.numProjectParams = cfg_.numProjectParams;
  pc.idleUnits = cfg_.idleUnits;
  pc.clipIn = timeline_->clipPlayer().channels();  // T5.4
  pc.clipActive = timeline_->clipPlayer().active();
  pc.numClipSlots = timeline_->clipPlayer().numSlots();
  pc.eventSources.reserve(slots_.size());
  for (SlotSource& s : slots_) pc.eventSources.push_back({&Engine::onSlot, &s});
  return pc;
}

void Engine::setProgram(std::unique_ptr<ProjectProgram> program) { program_ = std::move(program); }

bool Engine::post(const TransportCommand& c) noexcept {  // UI thread (wait-free push + wake hook)
  const bool ok = timeline_->post(c);
  wake();
  return ok;
}

bool Engine::releaseAuditions() noexcept {  // UI thread (wait-free push + wake hook)
  const bool ok = auditionIn_.tryPush(AuditionRequest{kReleaseAuditions, 0.0f, 0.0f, 0});
  wake();
  return ok;
}

bool Engine::setParam(uint32_t index, float value) noexcept {  // UI thread (wait-free push + wake hook)
  if (!paramProducer_->ring.tryPush(ParamWrite{index, value})) {
    droppedParams_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  wake();
  return true;
}

void Engine::growParamRing(uint32_t capacity) {  // UI thread (the producer)
  if (capacity <= paramProducer_->ring.capacity()) return;
  auto r = std::make_unique<ParamRing>(std::bit_ceil(std::min(capacity, 1u << 31)));
  paramProducer_->next.store(r.get(), std::memory_order_release);  // after every push into the old ring
  paramProducer_ = r.get();
  paramRings_.push_back(std::move(r));
}

uint32_t Engine::paramRingCapacity() const noexcept { return paramProducer_->ring.capacity(); }

bool Engine::audition(uint32_t slot, float hz, float velocity, uint32_t durationFrames) noexcept {  // UI thread (wait-free push + wake hook)
  if (slot >= cfg_.maxSlots || !(hz > 0.0f) || !std::isfinite(hz)) return false;
  const bool ok = auditionIn_.tryPush(AuditionRequest{slot, hz, std::clamp(velocity, 1.0f, 127.0f), std::max(durationFrames, 1u)});
  wake();
  return ok;
}

void Engine::collect() {
  // M4: rings the audio thread has left (it reads paramConsumed_ and every ring after it).
  const ParamRing* reading = paramConsumed_.load(std::memory_order_acquire);
  while (paramRings_.size() > 1 && paramRings_.front().get() != reading) paramRings_.erase(paramRings_.begin());
  timeline_->collect();
  live_->collect(audioStopped());
  if (program_) program_->collect();
}

void Engine::audioOffline() noexcept {  // RT-safe
  // T3.17-fix1 (M3 review M15b): the device stopped (a reconfigure, a lost device, an idle suspend). A sounding
  // audition ends now: its note-off goes out with the next block that runs, whenever that is; one whose note-on was
  // never sent is dropped. The audio thread is not running, so its state may be touched here.
  for (uint32_t i = 0; i < numAuditions_;) {
    Audition& a = auditions_[i];
    if (!a.started) {
      a = auditions_[--numAuditions_];
      continue;
    }
    a.offTime = std::min(a.offTime, nextTime_);
    ++i;
  }
  audio_.store(kAudioStopped, std::memory_order_release);
  timeline_->audioOffline();
  if (program_) program_->audioOffline();
}

void Engine::audioOnline() noexcept {  // RT-safe
  timeline_->audioOnline();
  if (program_) program_->audioOnline();
  audio_.store(kAudioRunning, std::memory_order_release);
}

void Engine::onSlot(void* slot, int64_t sampleTime, uint32_t frames, EventBuffer& out) noexcept {  // RT-safe
  const SlotSource& s = *static_cast<const SlotSource*>(slot);
  Engine& e = *s.engine;
  const EventSource tl = e.timeline_->eventSource(s.slot);
  tl.fn(tl.user, sampleTime, frames, out);
  // Auditions of this slot: the note-on at the first block after the request, the note-off at its time.
  const int64_t end = sampleTime + frames;
  for (uint32_t i = 0; i < e.numAuditions_;) {
    Audition& a = e.auditions_[i];
    if (a.slot != s.slot) {
      ++i;
      continue;
    }
    if (!a.started) {
      out.push(noteOn(0, a.id, a.hz, a.velocity * (1.0f / 127.0f)));
      a.started = true;
    }
    if (a.offTime < end) {
      out.push(noteOff(static_cast<uint32_t>(std::max<int64_t>(0, a.offTime - sampleTime)), a.id));
      a = e.auditions_[--e.numAuditions_];  // unordered removal
      ++e.stats_.auditionOffs;
      continue;
    }
    ++i;
  }
  e.live_->emit(s.slot, sampleTime, frames, out);  // T6.4: live notes routed to this slot
}

void Engine::onHook(void* self, int64_t sampleTime, uint32_t frames, TransportSnapshot& transport,
                    std::span<float> params) noexcept {  // RT-safe
  Engine& e = *static_cast<Engine*>(self);
  e.timelineHook_.fn(e.timelineHook_.user, sampleTime, frames, transport, params);
  const std::span<const TransportRun> runs = e.timeline_->runs();  // empty while stopped
  if (runs.empty() || e.offline_) return;
  const bool song = e.metronome_->enabled();
  if (!song && runs.front().kind != TransportRun::CountIn) return;  // off, no count-in: nothing to click
  e.metronome_->schedule(sampleTime, runs, e.timeline_->tempoImage(), e.timeline_->countIn(), e.cfg_.sampleRate,
                         e.program_ ? e.program_->outputLatency() : 0u, song);
}

void Engine::mixClicks(const AudioBlock& block) noexcept {  // RT-safe
  const uint32_t main = std::min(cfg_.numOut, block.numOut);
  if (clickPair_ && block.numOut >= main + 2) {  // the cue pair: always written whole while it is routed here
    float* pair[2] = {block.out[main], block.out[main + 1]};
    for (float* ch : pair) std::memset(ch, 0, sizeof(float) * block.frames);
    if (metronome_->pending()) metronome_->render(block.frameTime, block.frames, pair, 2);
    return;
  }
  if (metronome_->pending()) metronome_->render(block.frameTime, block.frames, block.out, main);
}

void Engine::process(const AudioBlock& device) noexcept {  // RT-safe
  ++stats_.blocks;
  // The engine's own time base (T3.17-fix1, M3 review M15b): monotonic whatever the device does (a reopened device
  // starts its frame count at 0 again), so audition note-offs and the units' sample time never go back.
  AudioBlock block = device;
  block.frameTime = nextTime_;
  nextTime_ += block.frames;
  bool activity = false;
  ParamWrite pw;
  if (program_) {
    std::span<float> pp;
    auto drain = [&](ParamRing& r) noexcept {
      while (r.ring.tryPop(pw)) {
        if (pp.empty()) pp = program_->projectParams();  // only in blocks with writes (L1: it counts as a change)
        if (pw.index >= pp.size()) {  // M4: a slot of a block reserved since this block started
          program_->adoptProjectParams();
          pp = program_->projectParams();
        }
        if (pw.index < pp.size()) pp[pw.index] = pw.value;
        ++stats_.paramWrites;
        activity = true;
      }
    };
    for (;;) {  // M4: a grown ring is chained behind the old one; the old one's writes come first
      drain(*paramConsumer_);
      ParamRing* next = paramConsumer_->next.load(std::memory_order_acquire);
      if (next == nullptr) break;
      drain(*paramConsumer_);  // what was pushed before `next` was set is visible now
      paramConsumer_ = next;
      paramConsumed_.store(next, std::memory_order_release);
    }
  }
  AuditionRequest ar;
  while (auditionIn_.tryPop(ar)) {
    activity = true;
    if (ar.slot == kReleaseAuditions) {  // AU.voices: the note-offs go out at once (≥ kAuditionMinMs after the onsets)
      const auto minFrames = static_cast<int64_t>(kAuditionMinMs * 1e-3 * cfg_.sampleRate);
      for (uint32_t i = 0; i < numAuditions_; ++i)
        auditions_[i].offTime = std::min(auditions_[i].offTime, std::max(block.frameTime, auditions_[i].onTime + minFrames));
      continue;
    }
    ++stats_.auditions;
    if (numAuditions_ == auditions_.size()) {  // full: end the oldest now (its note-off goes out next block)
      auditions_[0].offTime = block.frameTime;
    }
    if (numAuditions_ < auditions_.size()) {
      nextAuditionId_ = (nextAuditionId_ + 1) & 0x0FFFFFFFu;
      auditions_[numAuditions_++] =
          Audition{ar.slot, 0xF0000000u | nextAuditionId_, ar.hz, ar.velocity, block.frameTime + ar.frames, false, block.frameTime};
    }
  }
  {  // T6.4: live input (MIDI ports, the computer keyboard)
    LiveInput::Block lb;
    lb.start = block.frameTime;
    lb.frames = block.frames;
    lb.hostTime = block.hostTime;
    lb.hostOffset = block.hostOffset;
    lb.playing = timeline_->playing();
    lb.songSample = timeline_->songSample();
    if (live_->process(lb)) activity = true;
  }
  const uint32_t w = wake_.load(std::memory_order_acquire);
  if (w != seenWake_) {
    seenWake_ = w;
    activity = true;
  }
  if (program_ == nullptr) {
    for (uint32_t c = 0; c < block.numOut; ++c) std::memset(block.out[c], 0, sizeof(float) * block.frames);
    return;
  }
  if (asleep_.load(std::memory_order_relaxed)) {
    if (!activity) {
      ++stats_.asleepBlocks;
      for (uint32_t c = 0; c < block.numOut; ++c) std::memset(block.out[c], 0, sizeof(float) * block.frames);
      return;
    }
    asleep_.store(false, std::memory_order_relaxed);
    ++stats_.wakes;
  }
  {
    AudioBlock main = block;  // the program plays the main channels; the click pair (if any) is the engine's
    main.numOut = std::min(block.numOut, cfg_.numOut);
    program_->process(main);
  }
  if (clickPair_ || metronome_->pending()) mixClicks(block);  // T6.6
  if (sleepFrames_ == 0) return;
  bool quiet = !activity && numAuditions_ == 0 && live_->idle() && !timeline_->playing() && !timeline_->countingIn() &&
               !metronome_->pending() && program_->quiet();
  if (quiet)
    for (uint32_t c = 0; c < block.numOut && quiet; ++c)
      for (uint32_t f = 0; f < block.frames; ++f)
        if (std::fabs(block.out[c][f]) > kSleepPeak) {
          quiet = false;
          break;
        }
  quietFrames_ = quiet ? quietFrames_ + block.frames : 0;
  if (quietFrames_ >= sleepFrames_) {
    asleep_.store(true, std::memory_order_relaxed);
    quietFrames_ = 0;
  }
}

Result<RenderStats> Engine::renderOffline(uint64_t frames, AudioSink& sink, uint32_t blockFrames, uint32_t chunkFrames) {
  if (program_ == nullptr) return Error{ErrorCode::InvalidArgument, "engine: renderOffline without a program", {}};
  detail::RenderLoop loop;  // the one offline loop (render_loop.h), as engine::renderOffline
  loop.callback = callback();
  loop.numChannels = cfg_.numOut;
  loop.sampleRate = cfg_.sampleRate;
  loop.frames = frames;
  loop.blockFrames = blockFrames != 0 ? blockFrames : cfg_.blockFrames;
  loop.chunkFrames = chunkFrames;
  loop.startFrame = nextTime_;  // informational: process() keeps its own time base (engine time continues)
  loop.between = [](void* self, int64_t) { static_cast<Engine*>(self)->collect(); };
  loop.user = this;
  offline_ = true;  // T6.6: an offline render never clicks
  metronome_->clear();
  auto r = detail::runRenderLoop(loop, sink);
  offline_ = false;
  return r;
}

}  // namespace pq::engine
