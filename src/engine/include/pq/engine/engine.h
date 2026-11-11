#pragma once

// The engine (T3.17, ARCHITECTURE §3 "Engine"): one object the app drives a project through, on the device and
// offline, by exactly the same code.
//
//   Timeline (T3.16)        transport, note sequencer, automation renderer; its blockHook runs before every unit
//   ProjectProgram (T1.x-arch1 + T3.17)   the per-track units, per-unit hot swap, topology swaps, bus meters
//   param ring              UI → audio project-param writes (a knob, a fader, a mute: no recompile, SPEC §2)
//   audition ring           UI → audio one-off notes on an instrument slot (the piano roll's click preview), and
//                           releaseAuditions() (the press ended), in order
//   live input (T6.4)       MIDI ports and the computer keyboard → notes of the routed slots (live_input.h), drained
//                           at the start of every block, merged into a slot's events after the timeline's and the
//                           auditions'
//   metronome (T6.6)        the click (pq/engine/metronome.h): scheduled from the timeline's runs in the block hook,
//                           mixed after the project's output (see "Per block" 4)
//   recorder (T6.7)         device inputs → capture rings with song time, and the tracks' monitor gates
//                           (pq/engine/recorder.h); run from the block hook
//   idle gate               see below
//
// ---- Threads ---------------------------------------------------------------------------------------------------
//   setProgram                       non-RT, before the audio thread runs (or while it is offline)
//   program() publish / publishTopology, timeline() publish*         the compile / UI threads (their own rules)
//   post, setParam, audition, wake   ONE producer thread (the UI thread): wait-free SPSC pushes
//   process, callback                the audio thread (or the offline render loop)
//   audioOffline / audioOnline       the device's owner, while the audio thread does not run process(); audioOnline
//                                    also on the audio thread itself, before its first process() (AU.stream's pre-roll)
// Time: process() keeps its own sample count (time()), so the units and the timeline see one monotonic time base
// across device reopens; a device block's frameTime is not used (T3.17-fix1).
//
// ---- Per block (process) ------------------------------------------------------------------------------------
// 1. Drain the param ring into the project param block and the audition ring into the audition slots; the live
//    input drains its rings (anything arriving counts as activity: it wakes a sleeping engine).
// 2. Idle gate: while asleep the block is silence and nothing runs (no units, no timeline). The engine falls asleep
//    after `sleepAfterMs` of quiet blocks — transport stopped, ProjectProgram::quiet() (no fades, nothing queued, no
//    live voice in any unit, and no tail in progress: every unit's sleepable regions asleep, T1.x-silence, so a
//    delay or reverb tail is never frozen mid-way, M3 review M15a), output peak ≤ −120 dBFS — and wakes on any
//    transport command, param write, audition or wake() (a publish). So a stopped project costs one check per device
//    block (SPEC §7.7's per-region sleep is in every image, pq/engine/image_host.h; this is the whole-engine case).
//    Off with EngineConfig::sleepAfterMs = 0 (offline renders).
// 3. ProjectProgram::process: per image block the hook (Timeline::process: transport, automation → project params,
//    note events), then every unit; an instrument unit's events come from slotSource(k) = the timeline's track k
//    plus the audition notes of slot k plus the live notes routed to slot k (T6.4). The gate also waits while live
//    notes are held or queued.
// 4. T6.6: the metronome. The engine's block hook runs the timeline's, then (metronome on, or a count-in running, and
//    not rendering offline) queues the clicks of that image block's runs at beat + ProjectProgram::outputLatency().
//    After the program, queued clicks are added into the main pair (outputs 0 / 1), or — when the owner set
//    setClickPair(true) for this device block and it has numOut + 2 channels — written into channels numOut and
//    numOut + 1 (the click pair: zeroed, then the clicks; the app's audio output adds it into the cue pair). Off and
//    idle: one flag per image block, one per device block. The gate stays awake while a count-in runs or clicks wait.
//    Offline renders (renderOffline) never click.
// 5. T6.7: the recorder. Every device block first hands it the block (its input channels); the block hook, after the
//    timeline's, writes the monitor gates (Recorder::monitor: a changed gate counts as a param write for the idle-unit
//    scan) and captures the image block's song runs while the timeline records (Recorder::capture). The tracks'
//    `track.in` reads the device inputs through ProjectProgram (ProjectProgram::setUnitInputs). The gate stays awake
//    while recording (the transport plays).
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
#include "pq/engine/live_input.h"
#include "pq/engine/metronome.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/engine/recorder.h"
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
  // T6.4: live input. `livePorts` are the producer rings (shared with the app's MIDI service; null: the engine makes
  // its own); live.sampleRate / maxSlots are set from the fields above.
  std::shared_ptr<LivePorts> livePorts;
  LiveInputConfig live;
};

struct EngineStats {
  uint64_t blocks = 0;          // device blocks
  uint64_t asleepBlocks = 0;    // device blocks answered with silence while asleep
  uint64_t wakes = 0;
  uint64_t paramWrites = 0;
  uint64_t auditions = 0;
  uint64_t auditionOffs = 0;    // audition note-offs sent
  uint64_t panics = 0;          // M6 review L16: panic() requests taken
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
  const Timeline& timeline() const noexcept { return *timeline_; }
  const EngineConfig& config() const noexcept { return cfg_; }
  // T6.4: the live input (publish its routes, set its latency, read its record tap on the UI thread; see live_input.h).
  LiveInput& liveInput() noexcept { return *live_; }
  // T6.6: the metronome (its settings from any thread; see metronome.h).
  Metronome& metronome() noexcept { return *metronome_; }
  // T6.6, audio thread (the device's owner, before each device block it runs through the engine): channels numOut,
  // numOut + 1 of the blocks are the click pair (the cue output) instead of nothing. RT-safe.
  void setClickPair(bool on) noexcept { clickPair_ = on; }
  // T6.7: the recorder (capture sets, monitor images and latencies from the UI thread; its segments from the consumer
  // thread; see recorder.h).
  Recorder& recorder() noexcept { return *recorder_; }

  // ---- UI thread (one producer) ----
  // The pushes are wait-free SPSC; each then calls wake() (and so the wake hook, which may take a lock): UI thread.
  bool post(const TransportCommand& c) noexcept;            // transport (wakes the engine)
  bool setParam(uint32_t index, float value) noexcept;      // project param write
  // M4 review M4: the param ring holds at least `capacity` writes from now on (a larger ring is chained behind the
  // current one: the audio thread drains the old ring first, so the writes stay in order). Allocates. The program's
  // param block itself grows with ProjectProgram::reserveProjectParams (any non-RT thread).
  void growParamRing(uint32_t capacity);
  uint32_t paramRingCapacity() const noexcept;
  // A note on instrument slot `slot` for durationFrames frames (velocity 0..127).
  bool audition(uint32_t slot, float hz, float velocity, uint32_t durationFrames) noexcept;
  // AU.voices: every audition requested so far is released now (the piano roll's pointer let go), or at its own end
  // when that comes first; a note always sounds for kAuditionMinMs at least (a click is still heard). In order with
  // audition(): one requested before this call is released by it.
  bool releaseAuditions() noexcept;
  // M6 review L16 (transport.panic): at the next block every slot gets an AllNotesOff (its voices release), every held
  // live note is released and every live source's pitch bend and sustain pedal are reset (LiveInput::requestPanic), and
  // the auditions end. Wakes the engine.
  void panic() noexcept;
  static constexpr double kAuditionMinMs = 50.0;
  // After a publish (and inside post / setParam / audition). Calls the wake hook, if any: never call it from the
  // audio thread.
  void wake() noexcept {
    wake_.fetch_add(1, std::memory_order_release);
    if (auto fn = wakeHook_.load(std::memory_order_acquire)) fn(wakeUser_.load(std::memory_order_relaxed));
    if (auto fn = wakeListener_.load(std::memory_order_acquire)) fn(wakeListenerUser_.load(std::memory_order_relaxed));
  }
  // Non-RT: called on the producer's thread by every wake() (T3.17: the audio output restarts an idle device).
  // fn must be thread-safe and return quickly; null removes it.
  void setWakeHook(void (*fn)(void*), void* user) noexcept {
    wakeUser_.store(user, std::memory_order_relaxed);
    wakeHook_.store(fn, std::memory_order_release);
  }
  // KB.3: a second hook every wake() calls after the wake hook, on the same thread: the app's signal displays (meters,
  // cable glow, faces) re-arm their polls, which stop while the engine sleeps. Same contract as setWakeHook.
  void setWakeListener(void (*fn)(void*), void* user) noexcept {
    wakeListenerUser_.store(user, std::memory_order_relaxed);
    wakeListener_.store(fn, std::memory_order_release);
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
  // next block: T3.17-fix1) / starts calling it again. Called by the device's owner while the audio thread does not
  // run process(); AU.stream: audioOnline may also come from the audio thread itself just before its first process()
  // (AudioOutput's cold-start pre-roll keeps the engine offline until the driver has settled). RT-safe.
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
    int64_t onTime;   // engine sample time of the note-on (AU.voices: the earliest release is kAuditionMinMs after it)
  };
  static constexpr uint32_t kReleaseAuditions = 0xFFFFFFFFu;  // AuditionRequest::slot of a releaseAuditions()
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
  static void onHook(void* self, int64_t sampleTime, uint32_t frames, TransportSnapshot& transport,
                     std::span<float> params) noexcept;  // T6.6: the timeline's hook, then the metronome
  void mixClicks(const AudioBlock& block) noexcept;     // T6.6

  EngineConfig cfg_;
  std::unique_ptr<Timeline> timeline_;
  std::unique_ptr<ProjectProgram> program_;
  std::unique_ptr<LiveInput> live_;  // T6.4
  std::unique_ptr<Metronome> metronome_;  // T6.6
  std::unique_ptr<Recorder> recorder_;    // T6.7
  uint64_t hookWrites_ = 0;               // T6.7: the hook's param-write counter (the timeline's + monitor writes)
  uint64_t monitorWrites_ = 0;
  BlockHook timelineHook_{};              // T6.6: the timeline's own hook (onHook calls it)
  bool clickPair_ = false;                // T6.6: audio thread
  bool offline_ = false;                  // T6.6: renderOffline is running (no clicks)
  // Param writes (UI → audio). M4: a chain of rings; the producer pushes to the newest, the audio thread drains each
  // ring before following its `next`. collect() frees the rings the audio thread has left.
  struct ParamRing {
    explicit ParamRing(uint32_t capacity) : ring(capacity) {}
    SpscRing<ParamWrite> ring;
    std::atomic<ParamRing*> next{nullptr};
  };
  std::vector<std::unique_ptr<ParamRing>> paramRings_;  // UI thread (oldest first)
  ParamRing* paramProducer_ = nullptr;                  // UI thread
  ParamRing* paramConsumer_ = nullptr;                  // audio thread
  std::atomic<ParamRing*> paramConsumed_{nullptr};      // the ring the audio thread reads (for collect)
  SpscRing<AuditionRequest> auditionIn_;
  std::vector<SlotSource> slots_;
  std::atomic<uint32_t> wake_{0};
  std::atomic<void (*)(void*)> wakeHook_{nullptr};
  std::atomic<void*> wakeUser_{nullptr};
  std::atomic<void (*)(void*)> wakeListener_{nullptr};  // KB.3
  std::atomic<void*> wakeListenerUser_{nullptr};
  std::atomic<bool> asleep_{false};
  std::atomic<uint64_t> droppedParams_{0};
  static constexpr uint8_t kAudioUnknown = 0, kAudioRunning = 1, kAudioStopped = 2;
  std::atomic<uint8_t> audio_{kAudioUnknown};
  // Audio thread.
  std::vector<Audition> auditions_;  // capacity maxAuditions
  uint32_t numAuditions_ = 0;
  uint32_t nextAuditionId_ = 0;
  uint32_t seenWake_ = 0;
  std::atomic<uint32_t> panicReq_{0};  // L16: requests (UI) / the one taken (audio thread)
  uint32_t panicSeen_ = 0;
  bool panicNow_ = false;              // this block sends AllNotesOff to every slot
  uint64_t quietFrames_ = 0;
  uint64_t sleepFrames_ = 0;
  int64_t nextTime_ = 0;  // engine time of the next block (its own count of frames: monotonic)
  EngineStats stats_;
};

}  // namespace pq::engine
