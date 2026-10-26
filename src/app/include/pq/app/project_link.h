#pragma once

// The playback link (T3.17): document snapshots → engine, and the engine → views (transport, playhead, meters).
//
// ---- What a document change does (doc::ChangeSet, T3.4) -----------------------------------------------------------
//   Param        (volume, pan, mute, solo, sends, tuning params) the param values of the whole snapshot are
//                recomputed (doc::lowerParams, O(tracks): solo changes other tracks' gates) and only the changed ones
//                are written to the engine's project param block (Engine::setParam, a wait-free SPSC push). No
//                compile. A tuning param (generator, period, stretch, reference, EDO equave) reaches the tuning
//                nodes of the tracks that follow it the same way (T2.6-fix1), so every note following it (Degree
//                notes, Ratio notes on them, reference-relative Ratio / Cents notes: T2.6-fix3), held ones too, moves
//                at the next block; only tracks whose notes keep static pitches in that tuning are lowered again,
//                and republished only if one of them changed (held notes then retuned by Pitch events, not cut).
//   Sequence     the note sequences of the touched tracks are lowered again (a per-track cache keyed by the
//                track's snapshot node and the definitions its clips resolve in: sequenceKey), the SequenceImage is
//                rebuilt and published at
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
// T1b.5, the interpreter → JIT live swap: every compile publishes interpreted images (an edit is audible at once,
// QUESTIONS Q86), then asks the background compiler (pq/engine/background_compile.h: ONE worker thread at
// below-normal priority, newest wants replace the queue) for the JIT image of every unit schedule that has none yet.
// A finished JIT image is published into its unit if the unit still runs that schedule (ProjectCompiler::unitOf;
// else it is stale and dropped), with an Instant swap: interpreter and JIT images are bit-identical by default
// (T1b.1-fix1) and the swap transfers every state word, so the output is sample-exact through it (tested). The JIT
// publish does not wake a sleeping engine: the packet waits for the next block that runs.
//
// ---- Threads ------------------------------------------------------------------------------------------------------
//   UI thread      onDocumentChange (or attach()'s subscription), the TransportPort and MeterSource calls
//   compile worker structural compiles (LinkConfig::compileThread; otherwise flush() compiles on the caller)
//   audio thread   Engine::process through callback() (a device, a NullDevice, or renderOffline)
//
// ---- Ids --------------------------------------------------------------------------------------------------------
// Project param slots and note-event slots are assigned per stable key (param key / track id) and never move while
// the key exists; slots of removed keys are reused only after a later compile (a removed unit may still be fading).
//
/// ---- Tuning tables (T2.6-fix1, T2.6-fix4) ---------------------------------------------------------------------------
// Every main tuning with a signal (doc::hasTuningSignal) gets a tuning node table (engine::makeTuningTable) in
// Resources::tunings, built on the UI thread when a snapshot first shows that tuning with that TableKey (EDO n; MOS
// n, periods per equave, the mode as the root's chain position, the region hint: T2.6-fix2). A table is a shared
// SHAPE (the interval table of a chain length: one per (kind, n) while any table uses it) plus its definition.
// Ownership (T2.6-fix4, M3 review M8): the link holds the tables of the current document (`held_`; a Reset — File >
// New / Open — drops them all, so a new project never plays an older project's table, whatever its tuning ids), the
// running compile holds the ones its lowering named, and every unit image holds the ones it reads
// (ProjectCompileOptions::keepAlive → UnitImage::keepAlive, freed with the image by the program's epoch reclaimer). A
// table nobody holds is freed and its slot of the kTuningTables (256) Resources::tunings slots is reused. With every
// slot in use a tuning's node gets no table (static pitches) and the link reports it (LinkStats::tablesExhausted, the
// status line). A Param change that needs a new table (a region hint moved with the generator) compiles like a
// structural change. Automation lanes on a tuning's params drive the node per sample through the slot's
// TuningLiveInputs (AutomationLaneDesc::tap; T2.6-fix2, M3 review M6); every other lane is rendered one value per
// block (AutomationLaneDesc::perSample false: its consumers read params). A
// continuous change (a param) never rebuilds a table, so the table's definition values can lag the document's: the
// static NoteDesc::hz of a note following the node is resolved in the TABLE's definition, which is what the
// node passes through bit for bit while its inputs equal those values (pq/dsp/tuning_table.h), and the node
// computes the pitch from the params otherwise. Unmodulated projects therefore render bit-identically to static
// pitches; after a live edit the pitch is computed from the float32 param (a float32 generator is within half an ulp,
// ~3e-5 ¢ at 700 ¢, so degree k is within k times that: ~7e-4 ¢ at k = 24; QUESTIONS Q105.4).
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/devices.h"
#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/document/store.h"
#include "pq/dsp/tuning_table.h"
#include "pq/engine/background_compile.h"
#include "pq/engine/engine.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/sequence.h"
#include "pq/tuning/tuning.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/project_patch/device_cost.h"
#include "pq/views/transport_port.h"
#include "pq/views/view_context.h"

namespace pq::app {

class DeviceInspector;  // pq/app/device_player.h (T4.4-fix1)

// The stdlib directory: $PQ_STDLIB_DIR, else <exe dir>/stdlib, else the source tree's stdlib/.
std::string stdlibDir();
// T4.1: the device library the lowering calls (doc::loadDeviceLibrary: the stock devices, BasicSynth first) and its
// catalog (the views' device menu and panels). Not RT-safe (file I/O, parsing); fixed address (the library points
// into it).
Result<std::unique_ptr<doc::DeviceLibrary>> loadDeviceLibrary(const std::string& stdlib = stdlibDir());

struct LinkConfig {
  double sampleRate = 48000.0;
  uint32_t blockFrames = 64;      // the engine's image block
  uint32_t maxVoices = 24;        // polyphony per MIDI track (one more voice group of steal headroom)
  bool compileThread = true;      // false: structural compiles run in flush() on the caller's thread (tests)
  double debounceMs = 50.0;       // structural changes: compile this long after the last one
  float sleepAfterMs = 250.0f;    // the engine's idle gate (0: off)
  std::string stdlib;             // empty: stdlibDir()
  // T3.17-fix3 (M3 review M16): the chance / velocity-range seed (engine::SequenceBuildOptions). Each playback and
  // each arrangement loop repeat draws anew from it (the timeline's playback pass); a given seed renders the same
  // way every time (offline: `--render --seed`). The window sets a random one per session.
  uint64_t chanceSeed = 0;
  // Skip idle units (pq/engine/project_program.h; bit-exact). false: run every unit (measurements, tests).
  bool idleUnits = true;
  // T1b.5: swap every unit to its JIT image once the background compile has it (false: interpreter images only).
  bool jit = true;
  // T1b.x-perf (the T4.12-fix1 follow-up): an edit of a unit that plays a JIT image keeps that image playing, instead
  // of publishing the interpreted one, when the interpreted image would cost more than this fraction of a core
  // (engine::estimateInterpretedNsPerFrame at the unit's full polyphony; e.g. Operator with a Newton filter at 64
  // voices): the edit becomes audible when its JIT image arrives (a cache hit: about a millisecond; a compile: tens to
  // hundreds of ms), crossfaded like any edit. A failed JIT publishes the interpreted image then. Topology changes
  // always publish interpreted images. ≤ 0: never hold.
  float interpretBudget = 0.25f;
  // T1b.4: what builds the JIT images (fn null: compile::jitUnitBackend(true), the process compiler and its cache, with
  // T1b.x-perf's quick first images: QUESTIONS Q111 option C); tests and the AOT seed pass
  // compile::jitUnitBackend(&theirCompiler[, quickFirst]).
  engine::UnitBackend jitBackend;
  // Test hook (the "simulated slow compile"): called on the JIT worker before each compile; may block.
  void (*jitBeforeCompile)(void* user) = nullptr;
  void* jitUser = nullptr;
};

// T1b.5 counters (written by the JIT worker; jitStats() returns a consistent copy).
struct JitStats {
  uint64_t wanted = 0;      // unit schedules queued for a JIT image
  uint64_t built = 0;       // JIT images compiled
  uint64_t published = 0;   // ... and published into their unit
  uint64_t stale = 0;       // ... dropped: the unit had moved on to another schedule (or room never came)
  uint64_t errors = 0;      // JIT compiles that failed (the unit stays interpreted)
  // T1b.x-perf (Q111 option C): of the published images, the quick first ones (voice groups, no pair functions);
  // each is followed by a refinement build: the final image, published with an Instant swap (`refined`), dropped when
  // its unit moved on (`refineStale`) or failed (`refineErrors`: the first image keeps playing). The counters above
  // count first images and final images built at once (a cache hit, nothing to pair); refinements only these three.
  uint64_t firstImages = 0;
  uint64_t refined = 0, refineStale = 0, refineErrors = 0;
  double totalCompileMs = 0.0;
};

struct LinkStats {
  uint64_t compiles = 0;           // structural compile jobs run (each after its debounce)
  uint64_t unitCompiles = 0;       // units compiled by them
  uint64_t topologyChanges = 0;    // jobs that changed the partition / buses
  uint64_t unitPublishes = 0;
  uint64_t sequencePublishes = 0, automationPublishes = 0, tempoPublishes = 0;
  uint64_t trackLowerings = 0;     // tracks whose note sequence was lowered again (the per-track cache missed)
  uint64_t tablesExhausted = 0;    // tunings left without a node table (every Resources::tunings slot in use)
  uint64_t paramWrites = 0;
  uint64_t compileErrors = 0;
  // T3.17-fix1 (M3 review L11): compile results that could not be handed to the program at once because the audio
  // thread was not running (the output stopped) and an earlier image / topology was still waiting for it; they are
  // published in order once it runs again (deferredPublishes counts the compiles that waited this way).
  uint64_t deferredPublishes = 0;
  // T3.17-fix3 (M3 review M5, M2): MIDI tracks beyond the engine's event slots (not played; the status line says
  // so), and sends lowered with an automatic 1-sample delay (they close a loop), at the last change.
  uint32_t tracksWithoutSlot = 0;
  uint32_t delayedSends = 0;
  // T4.7: wires lowered with an automatic 1-sample delay (they close a loop) and wires refused (a cycle through two
  // instrument tracks), at the last structural change.
  uint32_t delayedWires = 0;
  uint32_t refusedWires = 0;
  uint64_t heldForJit = 0;  // T1b.x-perf: unit edits that kept the JIT image playing (LinkConfig::interpretBudget)
  // T4.4-fix1: wires left out because a port they connect is not on the device (an edit of a user device removed it).
  uint32_t invalidWires = 0;
};

class ProjectLink final : public views::TransportPort,
                          public views::MeterSource,
                          public views::ModulationSource,
                          public views::project_patch::DeviceCostSource {
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
  // Any thread but the audio thread: flush(), then waits until the background JIT has compiled (and published or
  // dropped) every image wanted so far; false on timeout. A JIT publish waits for room in the unit's queue, which an
  // audio thread (or the caller driving the engine) makes: with a stopped engine and a full queue it times out.
  bool waitForJit(double timeoutSeconds = 60.0);
  JitStats jitStats() const;

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
  // The last error any path reported (compile, sequence build, tuning tables). Any thread (T3.17-fix1, M3 review
  // L12: the compile worker and the UI thread both write it).
  std::string lastError() const;
  // Frees retired engine images (timeline and program). UI thread. The link also calls it by itself while the UI
  // polls the playhead or the meters (at most every 200 ms), and the compile worker collects the program's retired
  // images after a compile (T3.17-fix1, M3 review L13), so nothing waits for the next publish to be freed.
  void collect();
  const LinkConfig& config() const noexcept { return cfg_; }
  // T4.1: the device library the link lowers with (the window gives its catalog to the views: one load per process).
  const doc::DeviceLibrary& devices() const noexcept { return *devices_; }
  // Offline render of the current state from `fromTick` (transport playing) for `frames` frames into `sink`; must not
  // run while a device drives the engine. Flushes pending compiles first.
  Result<engine::RenderStats> renderOffline(Ticks fromTick, uint64_t frames, engine::AudioSink& sink);

  static constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
  // Note-event slots: MIDI tracks the engine can play at once (T3.17-fix3, M3 review M5: was 128, silently). A track
  // beyond it is lowered without notes and the status line names the limit.
  static constexpr uint32_t kEventSlots = 512;
  // Slot lookups (tests, tools). kNoSlot when unknown.
  uint32_t paramSlot(const std::string& key) const;
  uint32_t eventSlot(doc::TrackId track) const;
  // Tuning tables (see the file comment): the index into Resources::tunings of main tuning `id` in `def`'s shape
  // (the compile path: the running compile then holds that table), or kTuningTables (a null entry: no table); the
  // tables alive (held by the document, a compile or an image), and the shapes alive.
  static constexpr uint32_t kTuningTables = 256;
  uint32_t tuningTable(doc::TuningId id, const doc::TuningDef& def);
  uint32_t tuningTablesBuilt() const;
  uint32_t tuningShapesAlive() const;
  // The table main tuning `id` in `def`'s shape plays through, or null (tests, tools).
  const dsp::TuningTableView* tuningView(doc::TuningId id, const doc::TuningDef& def) const;

  // What a tuning node table depends on besides the tuning's continuous values (T2.6-fix2): the shape (kind, n),
  // periods per equave, the mode as the root's chain position, the region hint.
  struct TableKey {
    dsp::TuningTableKind kind = dsp::TuningTableKind::Edo;
    int32_t n = 0;
    int32_t periodsPerEquave = 1;
    int32_t rootChain = 0;
    bool hasHint = false;
    dsp::TuningHint hint{};
    friend bool operator==(const TableKey& a, const TableKey& b) noexcept {
      return a.kind == b.kind && a.n == b.n && a.periodsPerEquave == b.periodsPerEquave &&
             a.rootChain == b.rootChain && a.hasHint == b.hasHint &&
             (!a.hasHint || (a.hint.perPeriodSize == b.hint.perPeriodSize && a.hint.lowCents == b.hint.lowCents &&
                             a.hint.highCents == b.hint.highCents));
    }
  };
  static std::optional<TableKey> tableKey(const doc::TuningDef& def);  // nullopt: no tuning signal

  // ---- views::TransportPort (UI thread) ----
  void play(Ticks from) override;
  void stop() override;
  void continuePlayback() override;
  void locate(Ticks tick) override;
  void setLoop(const views::TransportLoop& loop) override;
  views::PlayheadSnapshot playhead() override;
  void auditionHz(float hz, float velocity, float durationMs) override;

  // ---- views::MeterSource (UI thread) ----
  // True while the engine runs (it never sleeps while playing), and afterwards until every meter the views read has decayed
  // to the floor (T3.17-fix1, M3 review L13: the meters' last poll after a stop shows them falling, not frozen lit).
  bool live() const noexcept override;
  void read(std::span<const doc::TrackId> ids, std::span<ui::MeterLevels> out) noexcept override;

  // ---- views::ModulationSource (UI thread; T4.7) ----
  // The modulated params' tap buses (doc::LoweredProject::taps: metered on the audio thread like a track's out bus).
  // Both consumers read the program's meters through one path (readLevels), so neither steals the other's peaks.
  bool modulationLive() const noexcept override;
  void readModulation(std::span<const views::ModTarget> targets, std::span<views::ModSweep> out) noexcept override;

  // ---- views::project_patch::DeviceCostSource (UI thread; T4.13, src/app/src/project_link_cost.cpp) ----
  // One engine::CostMeter per unit schedule, bound to each of its images (interpreter, JIT) as it is published; off
  // (the default) the images only read the meter's flag per block. A reading is split among each member track's nodes
  // by T4.5's attribution (views::circuit::attributeCost): a device call `d<id>` gets its whole instance. A compile in
  // progress (compileMutex_ busy) makes a read return the last values instead of waiting.
  void setCostMetering(bool on) override;
  bool costLive() const noexcept override;
  void readDeviceCosts(std::span<const doc::DeviceId> devices, std::span<float> nsPerFrame) override;
  // The wires as the last lowering placed them (doc::wireStates; UI thread, after a structural change).
  std::span<const doc::WireState> wireStates() const noexcept { return wireStates_; }

  // ---- T4.4-fix1: the circuit editor on a device of the project (pq/app/device_player.h) ----
  // UI thread. `inspect` true: the unit holding `device`'s track is compiled with the editor's probes (`targets`: paths
  // from the device's patch, the ids the editor reads them by; may be empty) and its instance tree, its images bound to
  // the inspector's taps and cost meter; false: back to the plain unit. A change recompiles that unit only (debounced
  // like a structural edit; flush() runs it now); the audio is bit-identical with or without probes. One device at a
  // time (the last call wins; a call for another device than the inspected one with `inspect` false is ignored).
  void setDeviceInspect(doc::DeviceId device, std::vector<graph::ProbeTarget> targets, std::vector<uint32_t> ids,
                        bool inspect);
  DeviceInspector& inspector() noexcept { return *inspector_; }

private:
  struct TuningTableEntry {
    doc::TuningId id;
    TableKey key;
    doc::TuningDef def;                    // the definition the table was built from
    std::optional<tuning::Tuning> built;   // `def` built (UI thread: the static Hz of the node's Degree notes)
    uint32_t slot = 0;                     // its index into Resources::tunings
    dsp::TuningTable table;                // its view points at live_[slot]
  };
  using EntryPtr = std::shared_ptr<const TuningTableEntry>;
  struct SeqCacheEntry {
    const doc::Track* track = nullptr;
    std::vector<const void*> key;  // sequenceKey: the definitions (or node tables) its clips resolve in
    doc::Snapshot from;  // keeps `track` and the keyed definitions alive, so no address can be reused by a later node
    std::vector<EntryPtr> tables;  // … and the keyed tables
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
  // T4.1: a change of device params only (a knob drag): writes just those, O(changed params).
  void applyDeviceParams(const doc::Project& p, std::span<const doc::ParamRef> params);
  // onlyIfChanged: publish only when a track's description changed (after a tuning param change).
  void publishSequences(const doc::Project& p, bool onlyIfChanged = false);
  engine::TrackSequenceDesc lowerTrack(const doc::Project& p, const doc::TrackPath& path, const doc::Track& t);
  std::vector<const void*> sequenceKey(const doc::Project& p, const doc::TrackPath& path, const doc::Track& t,
                                       std::vector<EntryPtr>* tables = nullptr) const;
  // UI thread: the tables of the snapshot's signal tunings become the held set (`fresh`: a Reset, build new ones even
  // for an unchanged key); true when one was built (its index is new: a compile).
  bool updateTuningTables(const doc::Project& p, bool fresh = false);
  const EntryPtr* tuningEntry(doc::TuningId id, const doc::TuningDef& def) const;  // held; caller holds mutex_
  const float** liveTap(const doc::Project& p, const doc::ParamRef& ref);  // UI thread (T2.6-fix2)
  void publishAutomation(const doc::Project& p);
  void reportDelayedSends(const doc::Project& p);
  void reportMissingDevices(const doc::Project& p);  // UI thread (T4.1): the status line names a bypassed device  // UI thread: the status line names a newly delayed send
  void reportWires(const doc::Project& p);  // UI thread (T4.7): a newly delayed or refused wire, in the status line
  // T4.7: the program's bus meters for consumer `who` (0: track meters, 1: modulation taps): one program read of
  // every metered bus, merged into both consumers' pending levels; `out` gets this consumer's since its last read.
  void readLevels(int who, std::span<const uint32_t> buses, std::span<engine::BusLevel> out);
  void publishTempo(const doc::Project& p);
  void postTransport(const engine::TransportCommand& c);
  // Compile path (worker or flush()).
  Status compileNow(const doc::Snapshot& s);
  // A compile's result for the program, published in order (compileMutex_).
  struct Deferred {
    bool topology = false;
    engine::TopologyBuild topo;                      // topology
    std::unordered_map<uint32_t, uint32_t> meterMap;  // topology: track id → bus of its `out`
    std::unordered_map<std::string, uint32_t> tapMap;  // topology (T4.7): "<device id>:<param>" → its tap bus
    std::vector<engine::UnitUpdate> units;           // otherwise: the changed units' images, in order
  };
  // Publishes deferred_ front to back (compileMutex_ held). While the audio thread runs it waits (bounded) for the
  // room a publish needs; when it does not run (Engine::audioStopped), it stops at the first publish that needs it
  // and leaves the rest queued: ok() with `drained` false. An error drops the queue (a compile then rebuilds).
  Status drainDeferred(bool& drained);
  void setError(std::string message);
  void collectIdle();  // UI thread: collect() at most every 200 ms
  bool holdForJit(uint32_t unit, const std::shared_ptr<const graph::Schedule>& previous);  // compileMutex_ held
  void wantJit();  // compile path, compileMutex_ held: the schedules without a JIT image → the background compiler
  static void deliverJit(void* self, const engine::BackgroundJob& job, Result<std::unique_ptr<engine::ProgramImage>> img);
  static void beforeJit(void* self, const engine::BackgroundJob& job);
  void workerMain();
  uint32_t slotForParam(const std::string& key);  // allocates (mutex_)
  uint32_t slotForTrack(doc::TrackId id);         // allocates (mutex_)

  // T4.13 (project_link_cost.cpp): the cost meters. Declared before engine_ so they outlive every image bound to them.
  struct CostState;
  struct CostStateDeleter {
    void operator()(CostState* s) const noexcept;
  };
  static CostState* newCostState();
  void bindCost(engine::ProgramImage& image, const std::shared_ptr<const graph::Schedule>& schedule);  // compileMutex_
  LinkConfig cfg_;
  std::unique_ptr<CostState, CostStateDeleter> cost_{newCostState()};
  std::unique_ptr<doc::DeviceLibrary> devices_;
  std::unique_ptr<DeviceInspector> inspector_;   // T4.4-fix1 (see the end of the class)
  std::vector<doc::DeviceId> missingReported_;   // UI thread: devices the status line named as missing
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
  std::unordered_map<std::string, uint32_t> tapBus_;  // T4.7: "<device id>:<param>" → its tap bus (published layout)
  uint64_t layoutSerial_ = 0;                         // bumped with every new bus map (readLevels drops its pending)
  uint32_t nextParam_ = 0, nextSlot_ = 0;
  // Tuning tables (mutex_; see the file comment). tuningViews_[i] is written when slot i gets a table, before any
  // lowering can name it, and resources_ (read by the compile path) spans the whole fixed array; live_[i] is the
  // audio thread's (automation taps), stable for the link's life.
  std::vector<EntryPtr> held_;                                        // the current document's
  std::unordered_map<uint32_t, EntryPtr> compileHeld_;               // the last compile's (+ the running one's)
  std::unordered_map<uint32_t, EntryPtr> jobHeld_;                   // the running compile's
  std::array<std::weak_ptr<const TuningTableEntry>, kTuningTables> slots_;
  std::vector<std::pair<uint64_t, std::weak_ptr<const dsp::TuningShape>>> shapes_;  // (kind, n) → shape
  std::array<const dsp::TuningTableView*, kTuningTables> tuningViews_{};
  std::array<dsp::TuningLiveInputs, kTuningTables> live_{};
  engine::Resources resources_;

  // UI thread state.
  std::vector<float> written_;                   // per param slot: the value last written (NaN: never)
  std::vector<uint8_t> automated_;               // per param slot: an automation lane drives it
  std::unordered_map<uint32_t, SeqCacheEntry> seqCache_;  // track id
  const void* tempoTimeline_ = nullptr;
  views::ViewContext* ctx_ = nullptr;
  views::ViewContext::Subscription sub_;
  views::ViewContext::Subscription observer_;  // the context's automation overrides (T3.17-fix3, M3 review M4)
  std::vector<uint64_t> conditionalSends_;     // doc::conditionalSendsKey of current_
  std::vector<doc::DelayedSend> delayed_;      // doc::delayedSends of current_ (reported when it changes)
  std::string inertReported_;                  // the tuning whose inert lanes the status line last named
  uint64_t commandsPosted_ = 0;
  engine::TransportReport report_;
  views::PlayheadSnapshot optimistic_;
  views::TransportLoop loop_{};  // the last loop sent (re-sent to a rebuilt engine)
  bool loopSent_ = false;
  uint64_t reportSeq_ = 0;
  std::unordered_map<uint32_t, MeterState> meters_;
  struct LevelAcc {
    float peak[2] = {0.0f, 0.0f};
    double msTime[2] = {0.0, 0.0};  // Σ mean square × seconds
    double seconds = 0.0;
    uint8_t channels = 0;
  };
  std::unordered_map<uint32_t, LevelAcc> pendingLevels_[2];  // per consumer, by bus (UI thread)
  uint64_t levelsSerial_ = 0;
  double lastLevelRead_ = 0.0;
  std::vector<doc::WireState> wireStates_;  // T4.7 (UI thread)
  std::vector<doc::WireId> wiresReported_;  // delayed / refused wires the status line last named
  double lastMeterRead_ = 0.0;
  bool metersLit_ = false;  // a meter read last time is above its floor (live() keeps the views polling)
  double lastCollect_ = 0.0;

  // Compile worker (cv_ with mutex_).
  std::condition_variable cv_;
  std::thread worker_;
  bool quit_ = false, pending_ = false, busy_ = false, flushNow_ = false;
  doc::Snapshot pendingSnap_;
  double pendingSince_ = 0.0;   // seconds (steady clock) of the last structural change
  LinkStats stats_;
  mutable std::mutex errorMutex_;
  std::string lastError_;

  // T1b.5. compileMutex_ serialises the compile path's use of compiler_ / program publishes with JIT deliveries.
  std::mutex compileMutex_;
  std::deque<Deferred> deferred_;                 // compileMutex_; see drainDeferred
  std::atomic<bool> anyDeferred_{false};          // deferred_ is not empty (read by the worker without the lock)
  std::vector<std::shared_ptr<const graph::Schedule>> jitDone_;  // schedules with a JIT image (or a failed JIT)
  std::vector<std::shared_ptr<const graph::Schedule>> jitFirst_;  // ... with a quick first JIT image only (Q111 C)
  // Schedules whose unit plays a JIT image: published ones, and held ones (interpretBudget: the unit still plays an
  // earlier schedule's JIT image until this one's arrives; published then with a crossfade, not Instant).
  std::vector<std::shared_ptr<const graph::Schedule>> jitLive_, jitHeld_;
  mutable std::mutex jitStatsMutex_;
  JitStats jitStats_;
  std::unique_ptr<engine::BackgroundCompiler> jit_;  // destroyed first (~ProjectLink): no delivery after it
  engine::BackgroundCompiler* jitWorker_ = nullptr;  // jit_, for its own worker's deliveries (never reassigned)

  // T4.4-fix1: device inspection. inspect_ / inspectDirty_ under mutex_ (UI thread → compile path); the compiler's
  // copy is set by the compile path (compileMutex_). (inspector_ is declared before the engine: it outlives every
  // image bound to it and the JIT worker that binds them.)
  std::optional<engine::TrackInspect> inspect_;
  doc::DeviceId inspectDevice_;
  std::vector<uint32_t> inspectDirty_;  // track ids whose units must recompile for an inspection change
  bool inspectChanged_ = false;
};

}  // namespace pq::app
