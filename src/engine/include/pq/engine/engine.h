#pragma once

// The engine (T3.17, ARCHITECTURE §3 "Engine"): one object the app drives a project through, on the device and
// offline, by exactly the same code.
//
//   Timeline (T3.16)        transport, note sequencer, automation renderer; its blockHook runs before every unit
//   ProjectProgram (T1.x-arch1 + T3.17)   the per-track units, per-unit hot swap, topology swaps, bus meters
//   param ring              UI → audio project-param writes (a knob, a fader, a mute: no recompile, SPEC §2)
//   audition ring           UI → audio one-off notes on an instrument slot (the piano roll's click preview)
//   idle gate               see below
//
// ---- Threads ---------------------------------------------------------------------------------------------------
//   setProgram                       non-RT, before the audio thread runs (or while it is offline)
//   program() publish / publishTopology, timeline() publish*         the compile / UI threads (their own rules)
//   post, setParam, audition, wake   ONE producer thread (the UI thread): wait-free SPSC pushes
//   process, callback                the audio thread (or the offline render loop)
//   audioOffline / audioOnline       the device's owner, while the audio thread does not run
// Time: process() keeps its own sample count (time()), so the units and the timeline see one monotonic time base
// across device reopens; a device block's frameTime is not used (T3.17-fix1).
//
// ---- Per block (process) ------------------------------------------------------------------------------------
// 1. Drain the param ring into the project param block and the audition ring into the audition slots.
// 2. Idle gate: while asleep the block is silence and nothing runs (no units, no timeline). The engine falls asleep
//    after `sleepAfterMs` of quiet blocks — transport stopped, ProjectProgram::quiet() (no fades, nothing queued, no
//    live voice in any unit), output peak ≤ −120 dBFS — and wakes on any transport command, param write, audition or
//    wake() (a publish). So a stopped project costs one check per device block (SPEC §7.7's per-node sleep is
//    T1.x-silence; this is the whole-engine case). Off with EngineConfig::sleepAfterMs = 0 (offline renders).
// 3. ProjectProgram::process: per image block the hook (Timeline::process: transport, automation → project params,
//    note events), then every unit; an instrument unit's events come from slotSource(k) = the timeline's track k
//    plus the audition notes of slot k.
//
// ---- Offline (SPEC §9.8 "same Program, deterministic") -----------------------------------------------------------
// renderOffline drives process() from a loop at a fixed device block, chunk by chunk into an AudioSink, under
// DenormalGuard and an RtScope per chunk (the same as engine::renderOffline for one image).

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/spsc_ring.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/engine/timeline.h"

namespace pq::engine {

struct EngineConfig {
  double sampleRate = 48000.0;
  uint32_t blockFrames = 64;        // the image block (ProjectConfig::maxBlockFrames, TimelineConfig::maxBlockFrames)
  uint32_t numOut = 2;
  uint32_t numProjectParams = 4096; // project param slots (mixer params, automation targets)
  uint32_t maxSlots = 128;          // instrument event slots = timeline tracks
  uint32_t paramRing = 4096;        // param writes in flight (power of two)
  uint32_t auditionRing = 64;
  uint32_t maxAuditions = 32;       // audition notes at once (beyond it: the oldest is released, the new one dropped)
  float sleepAfterMs = 250.0f;      // idle gate (0: never sleep)
  bool idleUnits = true;            // ProjectConfig::idleUnits
  TimelineConfig timeline;          // sampleRate / maxBlockFrames / maxTracks are set from the fields above
};

struct EngineStats {
  uint64_t blocks = 0;          // device blocks
  uint64_t asleepBlocks = 0;    // device blocks answered with silence while asleep
  uint64_t wakes = 0;
  uint64_t paramWrites = 0;
  uint64_t auditions = 0;
  uint64_t auditionOffs = 0;    // audition note-offs sent
};

class Engine {
public:
  // Non-RT. Errors: bad config.
  static Result<std::unique_ptr<Engine>> create(const EngineConfig& config);
  ~Engine();  // non-RT, after the audio thread stopped
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // ProjectConfig for a ProjectProgram run by this engine (hook, event sources, sizes). For ProjectProgram::create.
  ProjectConfig projectConfig() noexcept;
  // Non-RT, while the audio thread does not call process(): the program to run (built with projectConfig()).
  void setProgram(std::unique_ptr<ProjectProgram> program);
  ProjectProgram* program() noexcept { return program_.get(); }
  Timeline& timeline() noexcept { return *timeline_; }
  const EngineConfig& config() const noexcept { return cfg_; }

  // ---- UI thread (one producer) ----
  // The pushes are wait-free SPSC; each then calls wake() (and so the wake hook, which may take a lock): UI thread.
  bool post(const TransportCommand& c) noexcept;            // transport (wakes the engine)
  bool setParam(uint32_t index, float value) noexcept;      // project param write
  // A note on instrument slot `slot` for durationFrames frames (velocity 0..127).
  bool audition(uint32_t slot, float hz, float velocity, uint32_t durationFrames) noexcept;
  // After a publish (and inside post / setParam / audition). Calls the wake hook, if any: never call it from the
  // audio thread.
  void wake() noexcept {
    wake_.fetch_add(1, std::memory_order_release);
    if (auto fn = wakeHook_.load(std::memory_order_acquire)) fn(wakeUser_.load(std::memory_order_relaxed));
  }
  // Non-RT: called on the producer's thread by every wake() (T3.17: the audio output restarts an idle device).
  // fn must be thread-safe and return quickly; null removes it.
  void setWakeHook(void (*fn)(void*), void* user) noexcept {
    wakeUser_.store(user, std::memory_order_relaxed);
    wakeHook_.store(fn, std::memory_order_release);
  }
  bool asleep() const noexcept { return asleep_.load(std::memory_order_relaxed); }
  uint64_t droppedParams() const noexcept { return droppedParams_.load(std::memory_order_relaxed); }  // ring full
  // Non-RT: frees retired timeline images and program packets.
  void collect();

  // ---- Audio thread ----
  void process(const AudioBlock& block) noexcept;  // RT-safe
  AudioCallback callback() noexcept { return {&Engine::onBlock, this}; }
  const EngineStats& stats() const noexcept { return stats_; }
  // The device stopped calling process() (lets collect() free while nothing runs; sounding auditions end with the
  // next block: T3.17-fix1) / starts calling it again. Called by the device's owner, not by the audio thread, while
  // the audio thread does not run. RT-safe.
  void audioOffline() noexcept;
  void audioOnline() noexcept;
  // Any thread: audioOffline() was the last of the two calls (the owner stopped the device and nothing runs
  // process() now). False for an engine whose owner never called either (tests and offline renders drive process()
  // themselves). The compile path uses it to stop waiting for an audio thread that is not there (T3.17-fix1, L11).
  bool audioStopped() const noexcept { return audio_.load(std::memory_order_acquire) == kAudioStopped; }
  // Audio thread (or after it): the engine time of the next block. Monotonic: process() ignores the device's
  // AudioBlock::frameTime (a reopened device restarts it at 0) and counts the frames it was given.
  int64_t time() const noexcept { return nextTime_; }

  // ---- Offline ----
  // Renders `frames` frames at device blocks of `blockFrames` (a multiple of the image block is cheapest) into
  // `sink`, numOut channels. Must not run concurrently with a device. Not RT-safe as a whole (the sink does I/O).
  Result<RenderStats> renderOffline(uint64_t frames, AudioSink& sink, uint32_t blockFrames = 0,
                                    uint32_t chunkFrames = 8192);

private:
  struct Audition {
    uint32_t slot, id;
    float hz, velocity;
    int64_t offTime;  // engine sample time of the note-off
    bool started;
  };
  struct AuditionRequest {
    uint32_t slot;
    float hz, velocity;
    uint32_t frames;
  };
  struct ParamWrite {
    uint32_t index;
    float value;
  };
  struct SlotSource {
    Engine* engine;
    uint32_t slot;
  };
  explicit Engine(const EngineConfig& config);
  static void onBlock(void* self, const AudioBlock& b) noexcept { static_cast<Engine*>(self)->process(b); }
  static void onSlot(void* slot, int64_t sampleTime, uint32_t frames, EventBuffer& out) noexcept;

  EngineConfig cfg_;
  std::unique_ptr<Timeline> timeline_;
  std::unique_ptr<ProjectProgram> program_;
  SpscRing<ParamWrite> params_;
  SpscRing<AuditionRequest> auditionIn_;
  std::vector<SlotSource> slots_;
  std::atomic<uint32_t> wake_{0};
  std::atomic<void (*)(void*)> wakeHook_{nullptr};
  std::atomic<void*> wakeUser_{nullptr};
  std::atomic<bool> asleep_{false};
  std::atomic<uint64_t> droppedParams_{0};
  static constexpr uint8_t kAudioUnknown = 0, kAudioRunning = 1, kAudioStopped = 2;
  std::atomic<uint8_t> audio_{kAudioUnknown};
  // Audio thread.
  std::vector<Audition> auditions_;  // capacity maxAuditions
  uint32_t numAuditions_ = 0;
  uint32_t nextAuditionId_ = 0;
  uint32_t seenWake_ = 0;
  uint64_t quietFrames_ = 0;
  uint64_t sleepFrames_ = 0;
  int64_t nextTime_ = 0;  // engine time of the next block (its own count of frames: monotonic)
  EngineStats stats_;
};

}  // namespace pq::engine
