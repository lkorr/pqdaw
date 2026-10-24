#pragma once

// The playback link (T3.17): document snapshots → engine, and the engine → views (transport, playhead, meters).
//
// ---- What a document change does (doc::ChangeSet, T3.4) -----------------------------------------------------------
//   Param        (volume, pan, mute, solo, sends) the param values of the whole snapshot are recomputed
//                (doc::lowerParams, O(tracks): solo changes other tracks' gates) and only the changed ones are
//                written to the engine's project param block (Engine::setParam, a wait-free SPSC push). No compile.
//   Sequence     the note sequences of the touched tracks are lowered again (a per-track cache keyed by the
//                track's snapshot node and its effective tuning), the SequenceImage is rebuilt and published at
//                once (Timeline::publishSequence): a moved note or a velocity change plays from the next block,
//                with no compile.
//   Automation   the AutomationImage is rebuilt and published (targets = project param slots).
//   Timeline     the TempoImage is published (the loop reaches the transport through TransportPort::setLoop).
//   Structural   a debounced (≈ 50 ms, SPEC §7.5) compile on the compile worker thread: doc::lower → the tracks
//                whose generated patch text changed → ProjectCompiler::update (only those tracks' units, hot swap
//                per unit, T1.9) or, when the partition / buses changed (a track added, removed or re-routed),
//                ProjectCompiler::rebuild + ProjectProgram::publishTopology (only new and changed units compile;
//                unchanged units keep running untouched). Param, sequence and automation work of the same change
//                happens at once on the UI thread, as above.
// Every image is the interpreter's (the JIT is not linked into pqdaw yet: T1.x-arch2 / T1b.5 bring the background
// JIT swap; the stock M3 patches are small, so the interpreter's cost is low; QUESTIONS Q96).
//
// ---- Threads ------------------------------------------------------------------------------------------------------
//   UI thread      onDocumentChange (or attach()'s subscription), the TransportPort and MeterSource calls
//   compile worker structural compiles (LinkConfig::compileThread; otherwise flush() compiles on the caller)
//   audio thread   Engine::process through callback() (a device, a NullDevice, or renderOffline)
//
// ---- Ids --------------------------------------------------------------------------------------------------------
// Project param slots and note-event slots are assigned per stable key (param key / track id) and never move while
// the key exists; slots of removed keys are reused only after a later compile (a removed unit may still be fading).

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/document/store.h"
#include "pq/engine/engine.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/sequence.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/transport_port.h"
#include "pq/views/view_context.h"

namespace pq::app {

// The instrument library the lowering calls (stdlib/instruments/basic_synth.pqp, Q15 A).
struct InstrumentLibrary {
  std::string text;
  graph::PatchFile file;
  graph::PatchLibrary library;
  std::vector<graph::SubcircuitRef> signatures;
};
// The stdlib directory: $PQ_STDLIB_DIR, else <exe dir>/stdlib, else the source tree's stdlib/.
std::string stdlibDir();
// Not RT-safe (file I/O, parsing). The result must stay at a fixed address (the library points into it).
Result<std::unique_ptr<InstrumentLibrary>> loadInstrumentLibrary(const std::string& stdlib = stdlibDir());

struct LinkConfig {
  double sampleRate = 48000.0;
  uint32_t blockFrames = 64;      // the engine's image block
  uint32_t maxVoices = 24;        // polyphony per MIDI track (one more voice group of steal headroom)
  bool compileThread = true;      // false: structural compiles run in flush() on the caller's thread (tests)
  double debounceMs = 50.0;       // structural changes: compile this long after the last one
  float sleepAfterMs = 250.0f;    // the engine's idle gate (0: off)
  std::string stdlib;             // empty: stdlibDir()
};

struct LinkStats {
  uint64_t compiles = 0;           // structural compile jobs run (each after its debounce)
  uint64_t unitCompiles = 0;       // units compiled by them
  uint64_t topologyChanges = 0;    // jobs that changed the partition / buses
  uint64_t unitPublishes = 0;
  uint64_t sequencePublishes = 0, automationPublishes = 0, tempoPublishes = 0;
  uint64_t paramWrites = 0;
  uint64_t compileErrors = 0;
  std::string lastError;
};

class ProjectLink final : public views::TransportPort, public views::MeterSource {
public:
  // Builds the engine and compiles `initial` synchronously (the first program is set before any audio runs).
  static Result<std::unique_ptr<ProjectLink>> create(const LinkConfig& config, const doc::Snapshot& initial);
  ~ProjectLink() override;  // stops the worker; the audio device must have stopped calling callback()
  ProjectLink(const ProjectLink&) = delete;
  ProjectLink& operator=(const ProjectLink&) = delete;

  // UI thread: follow a ViewContext (its store's changes; its detail clip for auditions). The context must outlive
  // the link or detach() first.
  void attach(views::ViewContext& ctx);
  void detach();
  // UI thread: a committed document change.
  void onDocumentChange(const doc::StoreEvent& e);
  // Any thread but the audio thread: runs the pending structural compile now (no debounce) and returns when no
  // compile is pending or running. With compileThread = false it compiles on the calling thread.
  void flush();

  // T3.18: another sample rate (the audio preferences, a driver that changed its clock). Every compiled image has
  // the rate folded in (`sr` constants, durations in samples, the timeline's tick ↔ sample map, the smoothing
  // coefficients), so the engine and the compiler are rebuilt at `rate` and the current document is compiled again
  // synchronously; slots, sequences, automation, tempo, params and the transport (playing / position / loop) carry
  // over. UI thread, and no device may call callback() during the call (the audio output is closed): the old
  // engine is destroyed here. Afterwards engine() is the new engine (the audio output must be rebound to it).
  // Errors: a rate outside [8 kHz, 768 kHz], a compile error (the engine then plays silence until the next compile).
  Status setSampleRate(double rate);

  engine::AudioCallback callback() noexcept { return engine_->callback(); }
  engine::Engine& engine() noexcept { return *engine_; }
  const LinkStats& stats() const noexcept { return stats_; }  // read after flush() (written by the compile path)
  const LinkConfig& config() const noexcept { return cfg_; }
  // Offline render of the current state from `fromTick` (transport playing) for `frames` frames into `sink`; must not
  // run while a device drives the engine. Flushes pending compiles first.
  Result<engine::RenderStats> renderOffline(Ticks fromTick, uint64_t frames, engine::AudioSink& sink);

  static constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
  // Slot lookups (tests, tools). kNoSlot when unknown.
  uint32_t paramSlot(const std::string& key) const;
  uint32_t eventSlot(doc::TrackId track) const;

  // ---- views::TransportPort (UI thread) ----
  void play(Ticks from) override;
  void stop() override;
  void continuePlayback() override;
  void locate(Ticks tick) override;
  void setLoop(const views::TransportLoop& loop) override;
  views::PlayheadSnapshot playhead() override;
  void auditionHz(float hz, float velocity, float durationMs) override;

  // ---- views::MeterSource (UI thread) ----
  bool live() const noexcept override;
  void read(std::span<const doc::TrackId> ids, std::span<ui::MeterLevels> out) noexcept override;

private:
  struct SeqCacheEntry {
    const doc::Track* track = nullptr;
    const doc::TuningDef* tuning = nullptr;
    doc::Snapshot from;  // keeps `track` and `tuning` alive, so neither address can be reused by a later node
    engine::TrackSequenceDesc desc;
  };
  struct MeterState {
    float peakDb[2] = {-200.0f, -200.0f};
    float holdDb[2] = {-200.0f, -200.0f};
    double holdUntil[2] = {0.0, 0.0};
    bool clip[2] = {false, false};
  };

  explicit ProjectLink(const LinkConfig& config);
  Status init(const doc::Snapshot& initial);
  Status makeEngine();  // engine_ + compiler_ at cfg_.sampleRate
  // UI thread.
  void applyParams(const doc::Project& p, bool forceAll);
  void publishSequences(const doc::Project& p);
  void publishAutomation(const doc::Project& p);
  void publishTempo(const doc::Project& p);
  void postTransport(const engine::TransportCommand& c);
  // Compile path (worker or flush()).
  Status compileNow(const doc::Snapshot& s);
  void workerMain();
  uint32_t slotForParam(const std::string& key);  // allocates (mutex_)
  uint32_t slotForTrack(doc::TrackId id);         // allocates (mutex_)

  LinkConfig cfg_;
  std::unique_ptr<InstrumentLibrary> instruments_;
  std::unique_ptr<engine::Engine> engine_;
  std::unique_ptr<engine::ProjectCompiler> compiler_;
  std::optional<doc::LoweredProject> lowered_;   // compile path
  doc::Snapshot current_;                        // UI thread: the snapshot the engine follows

  // Slots (mutex_).
  mutable std::mutex mutex_;
  std::unordered_map<std::string, uint32_t> paramSlots_;
  std::vector<uint32_t> freeParams_;
  std::vector<uint32_t> releasedParams_;         // freed at the next compile
  std::unordered_map<uint32_t, uint32_t> trackSlots_;  // track id → event slot
  std::vector<uint32_t> freeSlots_;               // FIFO: the least recently freed first
  std::unordered_map<uint32_t, uint32_t> meterBus_;    // track id → bus index of its `out` in the published layout
  uint32_t nextParam_ = 0, nextSlot_ = 0;

  // UI thread state.
  std::vector<float> written_;                   // per param slot: the value last written (NaN: never)
  std::vector<uint8_t> automated_;               // per param slot: an automation lane drives it
  std::unordered_map<uint32_t, SeqCacheEntry> seqCache_;  // track id
  const void* tempoTimeline_ = nullptr;
  views::ViewContext* ctx_ = nullptr;
  views::ViewContext::Subscription sub_;
  uint64_t commandsPosted_ = 0;
  engine::TransportReport report_;
  views::PlayheadSnapshot optimistic_;
  views::TransportLoop loop_{};  // the last loop sent (re-sent to a rebuilt engine)
  bool loopSent_ = false;
  uint64_t reportSeq_ = 0;
  std::unordered_map<uint32_t, MeterState> meters_;
  double lastMeterRead_ = 0.0;

  // Compile worker (cv_ with mutex_).
  std::condition_variable cv_;
  std::thread worker_;
  bool quit_ = false, pending_ = false, busy_ = false, flushNow_ = false;
  doc::Snapshot pendingSnap_;
  double pendingSince_ = 0.0;   // seconds (steady clock) of the last structural change
  LinkStats stats_;
};

}  // namespace pq::app
