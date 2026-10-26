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
  return std::unique_ptr<Engine>(new Engine(config));
}

Engine::Engine(const EngineConfig& config)
    : cfg_(config),
      params_(std::bit_ceil(std::max(config.paramRing, 2u))),
      auditionIn_(std::bit_ceil(std::max(config.auditionRing, 2u))) {
  TimelineConfig tc = cfg_.timeline;
  tc.sampleRate = cfg_.sampleRate;
  tc.maxBlockFrames = cfg_.blockFrames;
  tc.maxTracks = cfg_.maxSlots;
  timeline_ = std::make_unique<Timeline>(tc);
  for (uint32_t k = 0; k < cfg_.maxSlots; ++k) slots_.push_back({this, k});
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
  pc.hook = timeline_->blockHook();
  pc.numProjectParams = cfg_.numProjectParams;
  pc.idleUnits = cfg_.idleUnits;
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

bool Engine::setParam(uint32_t index, float value) noexcept {  // UI thread (wait-free push + wake hook)
  if (!params_.tryPush(ParamWrite{index, value})) {
    droppedParams_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  wake();
  return true;
}

bool Engine::audition(uint32_t slot, float hz, float velocity, uint32_t durationFrames) noexcept {  // UI thread (wait-free push + wake hook)
  if (slot >= cfg_.maxSlots || !(hz > 0.0f) || !std::isfinite(hz)) return false;
  const bool ok = auditionIn_.tryPush(AuditionRequest{slot, hz, std::clamp(velocity, 1.0f, 127.0f), std::max(durationFrames, 1u)});
  wake();
  return ok;
}

void Engine::collect() {
  timeline_->collect();
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
    std::span<float> pp = program_->projectParams();
    while (params_.tryPop(pw)) {
      if (pw.index < pp.size()) pp[pw.index] = pw.value;
      ++stats_.paramWrites;
      activity = true;
    }
  }
  AuditionRequest ar;
  while (auditionIn_.tryPop(ar)) {
    activity = true;
    ++stats_.auditions;
    if (numAuditions_ == auditions_.size()) {  // full: end the oldest now (its note-off goes out next block)
      auditions_[0].offTime = block.frameTime;
    }
    if (numAuditions_ < auditions_.size()) {
      nextAuditionId_ = (nextAuditionId_ + 1) & 0x0FFFFFFFu;
      auditions_[numAuditions_++] =
          Audition{ar.slot, 0xF0000000u | nextAuditionId_, ar.hz, ar.velocity, block.frameTime + ar.frames, false};
    }
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
  program_->process(block);
  if (sleepFrames_ == 0) return;
  bool quiet = !activity && numAuditions_ == 0 && !timeline_->playing() && program_->quiet();
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
  return detail::runRenderLoop(loop, sink);
}

}  // namespace pq::engine
