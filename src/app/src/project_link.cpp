// The playback link (T3.17): pq/app/project_link.h describes it.
#include "pq/app/project_link.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#include "pq/compile/compile.h"
#include "pq/document/audibility.h"
#include "pq/document/clip_time.h"
#include "pq/document/meta.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/meta/registry.h"
#include "pq/document/pitch_resolve.h"
#include "pq/engine/tuning_signal.h"
#include "pq/tuning/tuning.h"
#include "pq/ui/theme.h"

#ifdef PQ_SOURCE_DIR
#define PQ_STDLIB_FALLBACK PQ_SOURCE_DIR
#endif

namespace pq::app {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
// M4 review M4: the engine's project param block starts at this many slots and doubles whenever they are all in use
// (ProjectProgram::reserveProjectParams, Engine::growParamRing), up to kMaxProjectParams (64 MB of floats).
constexpr uint32_t kInitialParams = 16384;
constexpr uint32_t kMaxProjectParams = 1u << 24;
constexpr uint32_t kEventSlots = ProjectLink::kEventSlots;

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Error linkError(ErrorCode c, const std::string& m) { return Error{c, "playback: " + m, {}}; }

// ---- Sequence lowering (doc → engine plain data, T3.16's descriptions) -------------------------------------------

bool sameNote(const engine::NoteDesc& a, const engine::NoteDesc& b) {
  return a.start == b.start && a.length == b.length && a.id == b.id && a.hz == b.hz && a.velocity == b.velocity &&
         a.releaseVelocity == b.releaseVelocity && a.chance == b.chance && a.velocityRange == b.velocityRange &&
         a.ratio == b.ratio && a.harmonic == b.harmonic && a.pitch == b.pitch && a.enabled == b.enabled &&
         std::bit_cast<uint32_t>(a.keyDegree) == std::bit_cast<uint32_t>(b.keyDegree);  // NaN: unknown
}

bool sameDesc(const engine::TrackSequenceDesc& a, const engine::TrackSequenceDesc& b) {
  if (a.id != b.id || a.clips.size() != b.clips.size()) return false;
  for (size_t i = 0; i < a.clips.size(); ++i) {
    const engine::ClipDesc& x = a.clips[i];
    const engine::ClipDesc& y = b.clips[i];
    if (x.id != y.id || x.start != y.start || x.length != y.length || x.contentStart != y.contentStart ||
        x.loopEnabled != y.loopEnabled || x.loopStart != y.loopStart || x.loopEnd != y.loopEnd ||
        x.active != y.active || x.notes.size() != y.notes.size())
      return false;
    for (size_t j = 0; j < x.notes.size(); ++j)
      if (!sameNote(x.notes[j], y.notes[j])) return false;
  }
  return true;
}

// Every active MIDI track with its path (main tree; deactivated groups hide their members).
template <class F>
void forEachMidi(const doc::Project& p, F&& f) {
  struct Walk {
    F& f;
    void operator()(const std::vector<doc::TrackPtr>& list, doc::TrackPath path) {
      for (uint32_t i = 0; i < list.size(); ++i) {
        const doc::Track& t = *list[i];
        if (!t.active || path.depth >= doc::kMaxTrackDepth) continue;
        doc::TrackPath here = path;
        here.index[here.depth++] = i;
        if (t.kind == doc::TrackKind::Midi) f(here, t);
        if (t.kind == doc::TrackKind::Group) (*this)(t.children, here);
      }
    }
  } walk{f};
  walk(p.tracks, doc::TrackPath{});
}

float dbFromLinear(float x, bool power) {
  if (!(x > 0.0f)) return -200.0f;
  return (power ? 10.0f : 20.0f) * std::log10(x);
}

}  // namespace

// The node table a main tuning needs (T2.6-fix2): its shape and the discrete part of its definition (periods per
// equave; the root's chain position, i.e. the mode, which the node carries across intervals like the explorer; the
// region hint, which decides exact boundary generators). nullopt: no signal.
std::optional<ProjectLink::TableKey> ProjectLink::tableKey(const doc::TuningDef& d) {
  if (!doc::hasTuningSignal(d)) return std::nullopt;
  TableKey k;
  if (const tuning::Edo* e = d.edo()) {
    k.kind = dsp::TuningTableKind::Edo;
    k.n = static_cast<int32_t>(e->n());
    return k;
  }
  const tuning::MosParams& m = *d.mos();
  k.kind = dsp::TuningTableKind::Mos;
  k.n = m.n;
  k.periodsPerEquave = m.periodsPerEquave;
  const tuning::RegionHint* hint = m.region ? &*m.region : nullptr;
  if (m.n > 1 && m.rotation >= 0 && m.rotation < m.n)
    k.rootChain = tuning::generatorChain(m.generator, m.n, m.period, hint)[static_cast<size_t>(m.rotation)];
  if (hint) {
    k.hasHint = true;
    k.hint = dsp::TuningHint{hint->perPeriodSize, hint->lowCents, hint->highCents};
  }
  return k;
}

// ---- Sequence lowering (doc -> engine plain data, T3.16's descriptions) -------------------------------------------

// Pitches from doc::resolveTrackPitches (T2.7: Ratio anchors within the clip, memoised). A note following one of the
// track's tuning signals (doc::trackTuningSignals: one node each, T2.6-fix3) carries how (PitchSource: the node,
// Degree with its (root's) degree, or Reference) and its static Hz in that tuning's TABLE definition (see the file
// comment of project_link.h); every other note plays its static Hz. Harmonic / JI-fundamental identities need JI
// objects (M7): unresolved, not played.
engine::TrackSequenceDesc ProjectLink::lowerTrack(const doc::Project& p, const doc::TrackPath& path,
                                                  const doc::Track& track) {
  ++stats_.trackLowerings;
  engine::TrackSequenceDesc d;
  d.id = track.id.value;
  const std::vector<doc::TuningId> signals = doc::trackTuningSignals(p, path);
  doc::ResolveOptions ro;
  ro.signalTuning = [this](doc::TuningId id, const doc::TuningDef& def) -> const tuning::Tuning* {
    std::lock_guard lock(mutex_);
    const EntryPtr* e = tuningEntry(id, def);  // held: alive while this (UI) thread lowers
    return e != nullptr && (*e)->built ? &*(*e)->built : nullptr;
  };
  auto pitches = doc::resolveTrackPitches(p, track.id, ro);
  const std::vector<doc::ClipSlot>& clips = doc::playedClips(track);  // T6.1: the audition take's, else the main lane
  for (size_t ci = 0; ci < clips.size(); ++ci) {
    const doc::NoteClip& c = *clips[ci].clip;
    engine::ClipDesc cd;
    cd.id = c.id.value;
    cd.start = c.start;
    cd.length = std::max<Ticks>(c.length, 1);
    cd.contentStart = c.contentStart;
    cd.loopEnabled = c.loop.enabled;
    cd.loopStart = c.loop.start;
    cd.loopEnd = c.loop.end;
    cd.active = c.active;
    if (pitches.ok()) {
      const doc::ClipPitches& cp = pitches.value().clips[ci];
      cd.notes.reserve(c.notes.size());
      size_t ni = 0;
      for (const doc::Note& n : c.notes) {
        const doc::NotePitch& np = cp.notes[ni++];
        if (!np.hz) continue;
        const double hz = *np.hz;
        if (!(hz > 0.0) || !std::isfinite(static_cast<float>(hz))) continue;
        engine::NoteDesc nd;
        if (np.follow != doc::PitchFollow::Static) {
          const uint32_t node = doc::tuningNodeIndex(signals, np.signal);
          if (node != 0) {
            nd.pitch.tuning = np.signal.value;
            nd.pitch.node = static_cast<uint16_t>(node);
            nd.pitch.kind = np.follow == doc::PitchFollow::Degree ? engine::PitchFollowKind::Degree
                                                                   : engine::PitchFollowKind::Reference;
            if (np.follow == doc::PitchFollow::Degree) {
              nd.pitch.degree = np.degree;
              nd.pitch.equave = np.equave;
            }
          }
        }
        nd.start = n.start;
        nd.length = std::max<Ticks>(n.length, 1);
        nd.id = n.id.value;
        nd.hz = static_cast<float>(hz);
        if (np.keyDegree) nd.keyDegree = static_cast<float>(*np.keyDegree);
        nd.velocity = std::clamp(n.velocity, 1.0f, 127.0f);
        nd.releaseVelocity = std::clamp(n.releaseVelocity, 0.0f, 127.0f);
        nd.chance = std::clamp(n.chance, 0.0f, 1.0f);
        nd.velocityRange = std::clamp(n.velocityRange, -127.0f, 127.0f);
        if (n.pitch.kind() == tuning::PitchKind::Ratio && n.pitch.rawDen() != 0)
          nd.ratio = static_cast<float>(static_cast<double>(n.pitch.rawNum()) / static_cast<double>(n.pitch.rawDen()));
        nd.enabled = n.enabled();
        cd.notes.push_back(nd);
      }
    }
    d.clips.push_back(std::move(cd));
  }
  return d;
}

// What a track's lowered sequence depends on besides the track node itself (T2.6-fix3, M3 review H5): per clip the
// definition its notes resolve in — the node table's (an entry; its owner goes to `tables`, so the address stays
// unique while the cache keys on it) for a clip following a tuning signal, else the effective TuningDef — and the
// track's tuning signals (node numbers). A tuning param change replaces the tuning slot but not its table, so tracks
// whose notes follow the node are not re-lowered.
std::vector<const void*> ProjectLink::sequenceKey(const doc::Project& p, const doc::TrackPath& path,
                                                  const doc::Track& t, std::vector<EntryPtr>* tables) const {
  std::vector<const void*> key;
  for (const doc::TuningId id : doc::trackTuningSignals(p, path))
    key.push_back(reinterpret_cast<const void*>(static_cast<uintptr_t>(id.value)));
  key.push_back(nullptr);  // separator
  std::lock_guard lock(mutex_);
  for (const doc::ClipSlot& c : doc::playedClips(t)) {  // T6.1
    const doc::EffectiveTuning e = doc::effectiveTuningRef(p, path, c.clip.get());
    const void* k = e.def;
    if (e.main.valid() && e.def != nullptr && doc::hasTuningSignal(*e.def))
      if (const EntryPtr* entry = tuningEntry(e.main, *e.def)) {
        k = entry->get();
        if (tables) tables->push_back(*entry);
      }
    key.push_back(k);
  }
  return key;
}

// ---- Tuning tables (UI thread builds, the compile path reads) ----------------------------------------------------

const ProjectLink::EntryPtr* ProjectLink::tuningEntry(doc::TuningId id, const doc::TuningDef& def) const {
  const std::optional<TableKey> key = tableKey(def);
  if (!key) return nullptr;
  for (const EntryPtr& e : held_)
    if (e->id == id && e->key == *key) return &e;
  return nullptr;
}

uint32_t ProjectLink::tuningTable(doc::TuningId id, const doc::TuningDef& def) {  // compile path
  std::lock_guard lock(mutex_);
  const EntryPtr* e = tuningEntry(id, def);
  if (e == nullptr) return kTuningTables;
  compileHeld_[(*e)->slot] = *e;  // this compile's images will hold it (ProjectCompileOptions::keepAlive)
  jobHeld_[(*e)->slot] = *e;
  return (*e)->slot;
}

uint32_t ProjectLink::tuningTablesBuilt() const {
  std::lock_guard lock(mutex_);
  uint32_t n = 0;
  for (const auto& w : slots_) n += !w.expired();
  return n;
}

uint32_t ProjectLink::tuningShapesAlive() const {
  std::lock_guard lock(mutex_);
  uint32_t n = 0;
  for (const auto& [k, w] : shapes_) n += !w.expired();
  return n;
}

const dsp::TuningTableView* ProjectLink::tuningView(doc::TuningId id, const doc::TuningDef& def) const {
  std::lock_guard lock(mutex_);
  const EntryPtr* e = tuningEntry(id, def);
  return e != nullptr ? &(*e)->table.view() : nullptr;
}

bool ProjectLink::updateTuningTables(const doc::Project& p, bool fresh) {
  bool built = false;
  std::vector<EntryPtr> next;
  uint32_t exhausted = 0;
  for (const doc::TuningSlotPtr& slot : *p.tunings) {
    const std::optional<TableKey> key = tableKey(slot->def);
    if (!key) continue;
    {
      std::lock_guard lock(mutex_);
      EntryPtr found;
      if (!fresh)  // the held table, else one an image still holds (e.g. after an undo back to it)
        for (const EntryPtr& e : held_)
          if (e->id == slot->id && e->key == *key) found = e;
      if (!found && !fresh)
        for (const auto& w : slots_)
          if (EntryPtr e = w.lock(); e && e->id == slot->id && e->key == *key && e->def == slot->def) found = e;
      if (found) {
        next.push_back(std::move(found));
        continue;
      }
    }
    auto t = doc::buildTuning(slot->def);
    if (!t.ok()) continue;
    auto form = engine::tuningShapeKey(t.value());
    if (!form.ok()) continue;
    const uint64_t shapeKey = (static_cast<uint64_t>(form.value().kind) << 32) | static_cast<uint32_t>(form.value().n);
    std::unique_lock lock(mutex_);
    uint32_t free = kTuningTables;
    // L18: not a slot whose live inputs the last published automation drives (an older image's taps are cleared by
    // the timeline when it takes a newer one, before any unit of that block runs).
    for (uint32_t i = 0; i < kTuningTables && free == kTuningTables; ++i)
      if (slots_[i].expired() && tapped_[i] == 0) free = i;
    if (free == kTuningTables) {  // every slot in use: this tuning plays static pitches
      ++exhausted;
      continue;
    }
    std::shared_ptr<const dsp::TuningShape> shape;
    for (auto& [k, w] : shapes_)
      if (k == shapeKey) shape = w.lock();
    lock.unlock();  // building a shape is O(n³) for a MOS: not under the mutex
    if (!shape) {
      auto made = engine::makeTuningShape(form.value());
      if (!made.ok()) continue;
      shape = std::move(made.value());
    }
    auto table = engine::makeTuningTable(t.value(), shape, &live_[free]);
    if (!table.ok()) continue;
    auto e = std::make_shared<TuningTableEntry>();
    e->id = slot->id;
    e->key = *key;
    e->def = slot->def;
    e->built = std::move(t.value());
    e->slot = free;
    e->table = std::move(table.value());
    lock.lock();
    bool stored = false;
    for (auto& [k, w] : shapes_)
      if (k == shapeKey) {
        if (w.expired()) w = shape;
        stored = true;
      }
    if (!stored) {
      std::erase_if(shapes_, [](const auto& s) { return s.second.expired(); });
      shapes_.emplace_back(shapeKey, shape);
    }
    tuningViews_[free] = &e->table.view();  // before any lowering can name this index
    slots_[free] = e;
    next.push_back(std::move(e));
    built = true;
  }
  {
    std::lock_guard lock(mutex_);
    held_ = std::move(next);  // the tables no longer held die with their last image
  }
  if (exhausted > 0) {
    stats_.tablesExhausted += exhausted;
    const std::string m = "Tuning: " + std::to_string(exhausted) + " tuning(s) play static pitches (all " +
                          std::to_string(kTuningTables) + " tuning node tables are in use)";
    setError(m);
    if (ctx_ != nullptr) ctx_->setStatus(m);
  }
  return built;
}

// ---- Instruments ------------------------------------------------------------------------------------------------

std::string stdlibDir() {
  if (const char* env = std::getenv("PQ_STDLIB_DIR"); env && *env) return env;
  std::error_code ec;
  {
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
      const fs::path p = fs::path(buf).parent_path() / "stdlib";
      if (fs::is_directory(p, ec)) return p.string();
    }
  }
#ifdef PQ_STDLIB_FALLBACK
  return std::string(PQ_STDLIB_FALLBACK) + "/stdlib";
#else
  return "stdlib";
#endif
}

Result<std::unique_ptr<doc::DeviceLibrary>> loadDeviceLibrary(const std::string& stdlib) {
  return doc::loadDeviceLibrary(stdlib);
}

// ---- Construction -----------------------------------------------------------------------------------------------

ProjectLink::ProjectLink(const LinkConfig& config) : cfg_(config) {}

Result<std::unique_ptr<ProjectLink>> ProjectLink::create(const LinkConfig& config, const doc::Snapshot& initial) {
  std::unique_ptr<ProjectLink> link(new ProjectLink(config));
  if (Status s = link->init(initial); !s.ok()) return s.error();
  return link;
}

Status ProjectLink::init(const doc::Snapshot& initial) {
  if (!initial) return linkError(ErrorCode::InvalidArgument, "no project");
  auto lib = loadDeviceLibrary(cfg_.stdlib.empty() ? stdlibDir() : cfg_.stdlib);
  if (!lib.ok()) return lib.error();
  devices_ = std::move(lib.value());
  {  // T5.4: the sample pool (its disk thread blocks while nothing streams) and the loader thread
    media::PoolConfig pc = cfg_.pool;
    pc.reclaimer = &sampleReclaimer_;
    pool_ = std::make_unique<media::SamplePool>(pc);
    pool_->startDiskThread();
    loader_ = std::thread([this] { loaderMain(); });
  }
  paramCapacity_ = kInitialParams;
  if (Status s = makeEngine(); !s.ok()) return s;
  written_.assign(kInitialParams, std::numeric_limits<float>::quiet_NaN());
  automated_.assign(kInitialParams, 0);
  if (cfg_.jit) {  // T1b.5: before the first compile, which already wants JIT images
    engine::BackgroundCompilerConfig bc;
    bc.backend = cfg_.jitBackend.fn != nullptr ? cfg_.jitBackend : compile::jitUnitBackend(true);
    bc.deliver = &ProjectLink::deliverJit;
    bc.beforeBuild = cfg_.jitBeforeCompile != nullptr ? &ProjectLink::beforeJit : nullptr;
    bc.user = this;
    bc.lowPriority = true;
    bc.threads = cfg_.jitThreads;
    jit_ = std::make_unique<engine::BackgroundCompiler>(bc);
    jitWorker_ = jit_.get();
  }
  current_ = initial;
  {
    std::lock_guard lock(mutex_);
    metaDoc_ = initial;
  }
  // TMM.5: the meta modules' table builder (before the first compile, which already asks for tables).
  tables_ = std::make_unique<meta::TableBuilder>(
      [this](uint32_t id, uint64_t key, std::shared_ptr<const dsp::FrameTable> t) { deliverMetaTable(id, key, std::move(t)); });
  conditionalSends_ = doc::conditionalSendsKey(*current_);
  reportDelayedSends(*current_);
  reportWires(*current_);
  updateTuningTables(*current_);  // T2.6-fix1: before the first lowering names their indices
  applyParams(*current_, true);
  if (Status s = compileNow(current_); !s.ok()) return s;
  publishTempo(*current_);
  publishSequences(*current_);
  publishLiveRoutes(*current_, true);  // T6.4
  publishAudio(*current_);
  publishAutomation(*current_);
  if (cfg_.compileThread) worker_ = std::thread([this] { workerMain(); });
  return Ok{};
}

Status ProjectLink::makeEngine() {
  engine::EngineConfig ec;
  ec.sampleRate = cfg_.sampleRate;
  ec.blockFrames = cfg_.blockFrames;
  ec.numOut = 2;
  {
    std::lock_guard lock(mutex_);
    ec.numProjectParams = paramCapacity_;  // M4: the current capacity (it grows; a new engine starts there)
  }
  // T4.1: every device param is a project param now (BasicSynth alone adds 6 per MIDI track), so writing the whole
  // param set at once (a Reset, applyParams(forceAll)) can exceed the default 4096-write ring; a dropped first write
  // would leave a param at 0 until its next change. The ring holds every slot (16384 × 8 B), and grows with them (M4).
  ec.paramRing = ec.numProjectParams;
  ec.maxSlots = kEventSlots;
  // T3.17-fix3: 512 slots at 128 events per block and 128 sounding notes per track (≈ 25 KB per slot) instead of
  // 128 slots at 512 / 256 (≈ 61 KB): more than enough for a 64-frame block, and room for 512 MIDI tracks.
  ec.timeline.maxEventsPerTrack = 128;
  ec.timeline.maxSoundingPerTrack = 128;
  ec.sleepAfterMs = cfg_.sleepAfterMs;
  ec.idleUnits = cfg_.idleUnits;
  ec.timeline.reportEveryFrames = cfg_.blockFrames;  // the playhead within one engine block
  ec.timeline.reportCapacity = 1024;               // > 1 s of reports between two UI polls
  // T5.4: the clip player streams through the pool; its demotions retire through the link's sample reclaimer.
  ec.timeline.streams = pool_ != nullptr ? &pool_->streams() : nullptr;
  ec.timeline.sampleReclaimer = &sampleReclaimer_;
  ec.timeline.clipLookaheadSeconds = cfg_.clipLookaheadSeconds;
  ec.livePorts = livePorts_;  // T6.4: the MIDI service's rings outlive engines (a rate change makes a new one)
  auto eng = engine::Engine::create(ec);
  if (!eng.ok()) return eng.error();
  engine_ = std::move(eng.value());
  engine::ProjectCompileOptions co;
  co.sampleRate = cfg_.sampleRate;
  co.maxBlockFrames = cfg_.blockFrames;
  co.voice.maxVoices = std::max(cfg_.maxVoices, 1u);
  co.visualMode = true;
  resources_.tunings = tuningViews_;  // fixed array: entries are filled in as tables are built
  resources_.frameTables = frameTableViews_;  // TMM.2: likewise (meta-module slots)
  resources_.voiceTaps = voiceTapViews_;
  resources_.signalTaps = signalTapViews_;  // TX.spectrum
  resources_.curves = curveViews_;          // GU.1: the control curves' slots
  co.resources = &resources_;
  // T2.6-fix4: every image holds the tuning tables it reads (the ones its compile named: compileHeld_). TMM.2: and the
  // meta-module slots it reads.
  co.keepAlive = [this](dsp::Resource kind, uint32_t index) -> std::shared_ptr<const void> {
    if (kind == dsp::Resource::FrameTable || kind == dsp::Resource::VoiceTap || kind == dsp::Resource::SignalTap) {
      std::lock_guard lock(mutex_);
      if (index >= kMetaSlots) return nullptr;
      const MetaSlot& m = metaSlots_[index];
      if (kind == dsp::Resource::FrameTable) return m.table;
      if (kind == dsp::Resource::SignalTap) return m.ring;
      return m.tap;
    }
    if (kind == dsp::Resource::Curve) {  // GU.1
      std::lock_guard lock(mutex_);
      return index < kCurveSlots ? curveSlots_[index].slot : nullptr;
    }
    if (kind != dsp::Resource::Tuning) return nullptr;
    std::lock_guard lock(mutex_);
    const auto it = compileHeld_.find(index);
    return it != compileHeld_.end() ? it->second : nullptr;
  };
  // T4.13: every unit's images get its schedule's cost meter (the project patch view's per-device labels).
  // M4 review H2: the meter an image writes is kept alive by that image (UnitImage::keepAlive).
  co.onBind = [this](uint32_t, const engine::CompiledUnit& cu, engine::ProgramImage& image) -> std::shared_ptr<const void> {
    return bindCost(image, cu.schedule);
  };
  compiler_ = std::make_unique<engine::ProjectCompiler>(co);
  return Ok{};
}

Status ProjectLink::setSampleRate(double rate) {
  if (!(rate >= 8000.0 && rate <= 768000.0)) return linkError(ErrorCode::InvalidArgument, "sample rate out of range");
  if (rate == cfg_.sampleRate) return Ok{};
  flush();  // no structural compile pending or running (and none can start: only this thread queues them)
  const views::PlayheadSnapshot ph = playhead();
  const double oldRate = cfg_.sampleRate;
  std::unique_ptr<engine::Engine> oldEngine;
  std::unique_ptr<engine::ProjectCompiler> oldCompiler;
  {
    // T1b.5: a JIT delivery uses engine_ / compiler_ under compileMutex_; one for the old engine finds no unit in the
    // new compiler (stale) and is dropped.
    std::lock_guard compileLock(compileMutex_);
    oldEngine = std::move(engine_);
    oldCompiler = std::move(compiler_);
    cfg_.sampleRate = rate;
    if (Status s = makeEngine(); !s.ok()) {  // keep the old engine (still valid at its rate)
      cfg_.sampleRate = oldRate;
      engine_ = std::move(oldEngine);
      compiler_ = std::move(oldCompiler);
      return s;
    }
    jitDone_.clear();
    jitFirst_.clear();
    jitLive_.clear();
    jitHeld_.clear();
    deferred_.clear();  // results for the old program (the new one is compiled from scratch below)
    parked_.clear();
    anyDeferred_.store(false, std::memory_order_release);
  }
  {
    std::lock_guard lock(mutex_);
    meterBus_.clear();  // the new program's layout is published by compileNow
    tapBus_.clear();
    ++layoutSerial_;
    // TMM.2: a frame table's levels depend on the rate: every slot gets the default at the new one (TMM.5's builder
    // rebuilds the instances' own tables).
    for (MetaSlot& m : metaSlots_) {
      if (m.table) m.table->publish(metaDefaultTable(), sampleReclaimer_);
      m.wanted = 0;  // TMM.5: rebuilt at the new rate by the compile below
      m.recent.clear();
    }
  }
  meters_.clear();
  written_.assign(written_.size(), std::numeric_limits<float>::quiet_NaN());
  tempoTimeline_ = nullptr;
  report_ = engine::TransportReport{};
  commandsPosted_ = 0;
  applyParams(*current_, true);
  const Status compiled = compileNow(current_, ctx_ != nullptr && ctx_->doc() == current_ ? ctx_->docSerial() : 0);
  publishTempo(*current_);
  publishSequences(*current_);
  publishLiveRoutes(*current_, true);  // T6.4: the new engine has no routes yet
  lastAudio_ = nullptr;
  publishAudio(*current_, true);  // T5.4: the new engine's timeline has no audio image yet
  publishAutomation(*current_);
  if (loopSent_) postTransport({engine::TransportCommandKind::SetLoop, loop_.enabled, 0, loop_.start, loop_.end});
  postTransport({ph.playing ? engine::TransportCommandKind::Play : engine::TransportCommandKind::Locate, false, ph.tick, 0, 0});
  optimistic_ = {ph.playing, ph.tick, 0};
  oldCompiler.reset();
  oldEngine.reset();  // the audio output no longer calls it (precondition)
  return compiled;
}

ProjectLink::~ProjectLink() {
  detach();
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();  // no compile path (and so no wantJit) after this
  tables_.reset();  // TMM.5: no table delivery after this (a running build finishes first)
  jit_.reset();  // then no JIT delivery (a running JIT compile finishes first; a waiting delivery gives up)
  {  // T5.4: no sample load after this
    std::lock_guard lock(loadMutex_);
    loaderQuit_ = true;
  }
  loadCv_.notify_all();
  if (loader_.joinable()) loader_.join();
  engageDisk(false);
  // The engine before the tuning tables: its images point into them, and its timeline clears the automation taps
  // that point at their live inputs (T2.6-fix2). T5.4: and before the sample pool (its images hold samples).
  compiler_.reset();
  engine_.reset();
  audioCache_.clear();
  loaded_.clear();
  sampleReclaimer_.collect();
  pool_.reset();
}

void ProjectLink::attach(views::ViewContext& ctx) {
  detach();
  ctx_ = &ctx;
  // EQ.4b: the response displays read this link's lowerings (ViewContext::setLowering) instead of lowering themselves.
  ctx.setLoweringProvided(true);
  sub_ = ctx.subscribe([this](const doc::StoreEvent& e) { onDocumentChange(e); });
  // "Re-enable Automation" and hand-made overrides (T3.15) change which lanes play (T3.17-fix3, M3 review M4).
  observer_ = ctx.observe([this](uint32_t what) {
    if ((what & views::view_change::kAutomation) != 0 && current_) publishAutomation(*current_);
    if ((what & views::view_change::kSamples) != 0 && current_) {  // T5.4: which samples can play changed
      ++resolution_;
      publishAudio(*current_);
    }
  });
  if (ctx.doc() != current_) {  // follow the context's document
    const doc::ChangeSet none;
    const doc::StoreEvent e{doc::StoreEventKind::Reset, current_, ctx.doc(), none};
    onDocumentChange(e);
  } else {
    std::shared_ptr<const doc::LoweredProject> low;
    {
      std::lock_guard compileLock(compileMutex_);
      low = lowered_;
    }
    if (low) ctx.setLowering(ctx.docSerial(), current_, std::move(low));  // (the context's document, compiled already)
  }
}

void ProjectLink::detach() {
  observer_ = views::ViewContext::Subscription();
  sub_ = views::ViewContext::Subscription();
  if (ctx_ != nullptr) ctx_->setLoweringProvided(false);
  ctx_ = nullptr;
}

// ---- Slots ------------------------------------------------------------------------------------------------------

uint32_t ProjectLink::slotForParam(const std::string& key) {
  std::lock_guard lock(mutex_);
  if (auto it = paramSlots_.find(key); it != paramSlots_.end()) return it->second;
  uint32_t s = kNone;
  if (nextParam_ < paramCapacity_) {
    s = nextParam_++;
  } else if (!freeParams_.empty()) {  // a removed key's slot (the least recently freed)
    s = freeParams_.front();
    freeParams_.erase(freeParams_.begin());
  } else if (paramCapacity_ < kMaxProjectParams) {
    // M4 review M4: every slot in use: double the engine's param block (the audio thread adopts it, values copied;
    // a unit linking the new slots is published after this, so it never reads past the block).
    paramCapacity_ = std::min(paramCapacity_ * 2, kMaxProjectParams);
    if (engine::ProjectProgram* program = engine_ ? engine_->program() : nullptr) program->reserveProjectParams(paramCapacity_);
    s = nextParam_++;
  }
  if (s != kNone) {
    paramSlots_.emplace(key, s);
  } else if (!paramsExhausted_) {
    paramsExhausted_ = true;  // reported by the UI thread (applyParams)
    setError("Playback: more than " + std::to_string(kMaxProjectParams) + " params; the rest play their defaults");
  }
  stats_.paramSlots = static_cast<uint32_t>(paramSlots_.size());
  stats_.paramCapacity = paramCapacity_;
  return s;
}

uint32_t ProjectLink::slotForTrack(doc::TrackId id) {
  std::lock_guard lock(mutex_);
  if (auto it = trackSlots_.find(id.value); it != trackSlots_.end()) return it->second;
  uint32_t s = kNone;
  if (nextSlot_ < kEventSlots) {
    s = nextSlot_++;
  } else if (!freeSlots_.empty()) {
    s = freeSlots_.front();  // the least recently freed: a removed track's unit has long stopped
    freeSlots_.erase(freeSlots_.begin());
  }
  if (s != kNone) trackSlots_.emplace(id.value, s);
  return s;
}

uint32_t ProjectLink::paramSlot(const std::string& key) const {
  std::lock_guard lock(mutex_);
  auto it = paramSlots_.find(key);
  return it == paramSlots_.end() ? kNone : it->second;
}

uint32_t ProjectLink::eventSlot(doc::TrackId track) const {
  std::lock_guard lock(mutex_);
  auto it = trackSlots_.find(track.value);
  return it == trackSlots_.end() ? kNone : it->second;
}

// ---- Document changes (UI thread) -------------------------------------------------------------------------------

void ProjectLink::coverSlot(uint32_t s) {  // UI thread
  if (s < written_.size()) return;
  uint32_t capacity = 0;
  {
    std::lock_guard lock(mutex_);
    capacity = std::max(paramCapacity_, s + 1);
  }
  written_.resize(capacity, std::numeric_limits<float>::quiet_NaN());
  automated_.resize(capacity, 0);
  // M4: a burst of every slot's value (applyParams(forceAll)) must fit the ring, as at 16384.
  if (engine_->paramRingCapacity() < capacity) engine_->growParamRing(capacity);
}

bool ProjectLink::writeParam(uint32_t s, float value) {  // UI thread
  coverSlot(s);
  if (automated_[s] || written_[s] == value) return true;
  if (engine_->setParam(s, value)) {
    written_[s] = value;
    ++stats_.paramWrites;
    return true;
  }
  written_[s] = std::numeric_limits<float>::quiet_NaN();  // ring full: retried at the next change
  return false;
}

void ProjectLink::applyParams(const doc::Project& p, bool forceAll) {
  if (paramsExhausted_ && !paramsReported_ && ctx_ != nullptr) {  // M4: the hard limit (2^24 params)
    paramsReported_ = true;
    ctx_->setStatus(lastError());
  }
  const std::vector<doc::LoweredParam> params = doc::lowerParams(p, true, *devices_);
  drivenKeys_.clear();  // GU.1: the controls whose knob position the patches read (applyDeviceParams)
  for (const doc::LoweredParam& lp : params)
    if (lp.position) drivenKeys_.insert(lp.key.substr(0, lp.key.size() - doc::kPositionKeySuffix.size()));
  // M4: a burst of every value (a Reset, a new engine) must fit the ring before the audio thread drains it (at init it
  // does not run at all), duplicate keys included: room for this whole pass and some.
  if (forceAll && engine_->paramRingCapacity() < params.size() + 1024)
    engine_->growParamRing(static_cast<uint32_t>(params.size() + 1024));
  for (const doc::LoweredParam& lp : params) {
    const uint32_t s = slotForParam(lp.key);
    if (s == kNone) continue;
    coverSlot(s);
    if (automated_[s]) continue;  // automation owns an automated param
    if (!forceAll && written_[s] == lp.value) continue;
    if (engine_->setParam(s, lp.value)) {
      written_[s] = lp.value;
      ++stats_.paramWrites;
    } else {
      written_[s] = std::numeric_limits<float>::quiet_NaN();  // ring full: retried at the next change
    }
  }
}

void ProjectLink::applyDeviceParams(const doc::Project& p, std::span<const doc::ParamRef> params) {
  for (const doc::ParamRef& ref : params) {
    if (ref.kind == doc::ParamKind::WireAmount) {  // T4.7
      const doc::Wire* w = doc::findWire(p, doc::WireId{ref.owner});
      if (w == nullptr) continue;
      const auto value = static_cast<float>(w->amount);
      const uint32_t s = slotForParam("wire" + std::to_string(ref.owner) + ":amount");
      if (s != kNone) (void)writeParam(s, value);
      continue;
    }
    // TMM.3: a param of a nested meta module ("dev<id>:<path>.<name>"): one write.
    if (ref.kind == doc::ParamKind::MetaParam) {
      const doc::DeviceLoc d = doc::findDevice(p, doc::DeviceId{ref.owner});
      if (d.device == nullptr || !d.device->meta) continue;
      for (const doc::MetaEntry& e : *d.device->meta) {
        const meta::Type* t = meta::findType(e.instance.type);
        if (t == nullptr || e.path.empty()) continue;
        for (size_t i = 0; i < t->params.size(); ++i)
          if (doc::ParamRef::metaParam(d.device->id, e.path, t->params[i].name).target == ref.target)
            writeLinked(doc::metaParamKey(d.device->id, e.path, t->params[i].name),
                        static_cast<float>(doc::metaParamValue(*d.device, e.path, *t, i)));
      }
      continue;
    }
    const doc::DeviceLoc d = doc::findDevice(p, doc::DeviceId{ref.owner});
    if (d.device == nullptr) continue;
    const graph::Patch* callee = devices_->patchOf(*d.device);
    if (callee == nullptr) continue;  // bypassed (missing): nothing reads it
    for (graph::NodeIndex v = 0; v < callee->nodeCount(); ++v) {
      if (callee->node(v).kind != graph::NodeKind::Param) continue;
      const std::string& name = callee->nodeName(v);
      if (doc::deviceParamHash(name) != ref.target) continue;
      // GU.1: a driven control's knob position (its curve atom maps it; a curve edit of the same class publishes the
      // new curve into its slot: no compile), else its value — the curve at the position, evaluated here.
      const graph::ParamDecl& decl = callee->paramDecl(v);
      const std::string key = "dev" + std::to_string(ref.owner) + ":" + name;
      if (drivenKeys_.count(key) != 0) {
        const uint32_t s = slotForParam(key + std::string(doc::kPositionKeySuffix));
        if (s != kNone) (void)writeParam(s, static_cast<float>(doc::paramPosition(*d.device, name, decl)));
        publishCurve(d.device->id, name, doc::controlCurve(*d.device, name, decl));
      } else {
        const uint32_t s = slotForParam(key);
        if (s != kNone) (void)writeParam(s, static_cast<float>(doc::paramValue(*d.device, name, decl)));
      }
      break;
    }
  }
}

void ProjectLink::writeLinked(const std::string& key, float value) {
  const uint32_t s = paramSlot(key);  // never allocates: a param the lowered patches do not declare has no slot
  if (s != kNone) (void)writeParam(s, value);
}

void ProjectLink::publishSequences(const doc::Project& p, bool onlyIfChanged) {
  std::vector<engine::TrackSequenceDesc> descs;
  std::unordered_map<uint32_t, SeqCacheEntry> next;
  bool changed = false;
  uint32_t withoutSlot = 0;
  std::string firstWithout;
  forEachMidi(p, [&](const doc::TrackPath& path, const doc::Track& t) {
    const uint32_t slot = slotForTrack(t.id);
    if (slot == kNone) {
      if (withoutSlot++ == 0) firstWithout = t.name;
      return;
    }
    std::vector<EntryPtr> tables;
    std::vector<const void*> key = sequenceKey(p, path, t, &tables);
    SeqCacheEntry e;
    auto it = seqCache_.find(t.id.value);
    if (it != seqCache_.end() && it->second.track == &t && it->second.key == key) {
      e = std::move(it->second);
    } else {
      e.track = &t;
      e.key = std::move(key);
      e.from = current_;
      e.tables = std::move(tables);
      e.desc = lowerTrack(p, path, t);
      changed |= it == seqCache_.end() || !sameDesc(it->second.desc, e.desc);
    }
    if (descs.size() <= slot) descs.resize(slot + 1);
    descs[slot] = e.desc;
    next.emplace(t.id.value, std::move(e));
  });
  changed |= next.size() != seqCache_.size();
  seqCache_ = std::move(next);
  if (withoutSlot != stats_.tracksWithoutSlot) {  // M3 review M5: a visible limit, not a silent track
    stats_.tracksWithoutSlot = withoutSlot;
    if (withoutSlot > 0) {
      const std::string m = "Playback: " + std::to_string(withoutSlot) + " MIDI track(s) from '" + firstWithout +
                            "' on are not played (the engine plays at most " + std::to_string(kEventSlots) +
                            " MIDI tracks)";
      setError(m);
      if (ctx_ != nullptr) ctx_->setStatus(m);
    }
  }
  if (onlyIfChanged && !changed) return;  // e.g. a tuning param that only the tuning nodes follow
  engine::SequenceBuildOptions so;
  so.seed = cfg_.chanceSeed;
  auto img = engine::SequenceImage::build(descs, so);
  if (!img.ok()) {
    setError(img.error().message);
    return;
  }
  if (engine_->timeline().publishSequence(std::move(img.value())).ok()) ++stats_.sequencePublishes;
  engine_->wake();
}

// T4.18: the range of a device-kind lane's param: a device param's from the patch the device plays, else the
// document's.
doc::ParamRange ProjectLink::laneRange(const doc::Project& p, const doc::AutomationLane& lane) const {
  if (lane.param.kind != doc::ParamKind::DeviceParam) return doc::paramRange(lane.param);
  const doc::Device* d = doc::findDevice(p, doc::DeviceId{lane.param.owner}).device;
  if (d == nullptr) return doc::paramRange(lane.param);
  return {0.0, 1.0};  // GU.1: a device param's lane holds knob positions
  if (const doc::DeviceInfo* info = devices_->infoOf(*d))
    if (const doc::DeviceParamInfo* pi = info->param(lane.paramName); pi != nullptr && pi->hasRange)
      return {std::min(pi->min, pi->max), std::max(pi->min, pi->max)};
  return doc::paramRange(lane.param);
}

void ProjectLink::publishAutomation(const doc::Project& p) {
  std::vector<engine::AutomationLaneDesc> lanes;
  std::vector<uint32_t> shift;  // T6.2: per param slot, its unit's compensation
  {
    std::lock_guard lock(mutex_);
    shift = paramShift_;
  }
  shiftsChanged_.store(false, std::memory_order_release);
  std::vector<uint32_t> automatedSlots;
  std::array<uint8_t, kTuningTables> tapped{};  // L18
  std::string inert;  // lanes on a tuning without a signal: nothing follows them (T3.17-fix3, M3 review M3)
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::LanePtr& lane : t.lanes) {
      if (lane->points.empty()) continue;
      const std::string key = doc::paramKey(p, *lane);  // T4.18: a device param's key carries its name (GU.1: "@pos")
      if (key.empty()) continue;  // not lowered (solo is derived into the gates)
      // Overridden by hand (T3.15's "Re-enable Automation"): the document value plays until re-enabled (M4).
      if (ctx_ != nullptr && ctx_->automationOverridden(lane->param)) continue;
      const bool deviceKind = doc::isDeviceParamKind(lane->param.kind);
      if (!doc::isTrackParam(lane->param.kind) && !deviceKind)
        if (const doc::TuningSlot* ts = doc::findTuning(p, doc::TuningId{lane->param.owner});
            ts != nullptr && !doc::hasTuningSignal(ts->def) && inert.empty())
          inert = ts->name;
      const uint32_t s = slotForParam(key);
      if (s == kNone) continue;
      engine::AutomationLaneDesc d;
      d.target = s;
      uint32_t table = kTuningTables;
      d.tap = liveTap(p, lane->param, &table);  // T2.6-fix2: a tuning param drives its node per sample
      if (d.tap != nullptr && table < kTuningTables) tapped[table] = 1;
      d.perSample = false;              // every other consumer reads the param (one value per block)
      d.shiftFrames = s < shift.size() ? shift[s] : 0u;  // T6.2
      // T4.18: device params, macros, chain volumes / pans and wire amounts are read at block rate like the mixer's
      // (their consumers smooth what moves: `gain` stages, the patches' own smoothing). Values are clamped to the
      // param's range (a device's from its patch: the document does not know it).
      const doc::ParamRange range = deviceKind ? laneRange(p, *lane) : doc::paramRange(lane->param);
      for (const doc::Breakpoint& b : lane->points)
        d.points.push_back({b.tick, static_cast<float>(std::clamp(static_cast<double>(b.value), range.min, range.max)), b.curve});
      lanes.push_back(std::move(d));
      automatedSlots.push_back(s);
    }
  });
  for (uint32_t s : automatedSlots) coverSlot(s);
  std::vector<uint8_t> automated(automated_.size(), 0);
  for (uint32_t s : automatedSlots) automated[s] = 1;
  if (!inert.empty() && inert != inertReported_ && ctx_ != nullptr)
    ctx_->setStatus("Automation of tuning '" + inert + "' has no effect: it is not an EDO or MOS tuning (no signal)");
  inertReported_ = inert;
  bool released = false;  // a param that lost its lane gets its document value again
  for (size_t s = 0; s < automated_.size(); ++s) released |= automated_[s] && !automated[s];
  automated_ = std::move(automated);
  auto img = engine::AutomationImage::build(lanes);
  if (img.ok() && engine_->timeline().publishAutomation(std::move(img.value())).ok()) {
    ++stats_.automationPublishes;
    std::lock_guard lock(mutex_);
    tapped_ = tapped;  // L18: the slots the image now published drives
  }
  if (released) applyParams(p, true);
  engine_->wake();
}

// The tuning node input an automation lane on `ref` drives per sample (T2.6-fix2): the live input of the current
// table of its tuning, or null (not a tuning param, or the tuning has no table).
const float** ProjectLink::liveTap(const doc::Project& p, const doc::ParamRef& ref, uint32_t* slotOut) {
  if (doc::isTrackParam(ref.kind) || doc::isDeviceParamKind(ref.kind)) return nullptr;
  const doc::TuningSlot* slot = doc::findTuning(p, doc::TuningId{ref.owner});
  if (slot == nullptr) return nullptr;
  std::lock_guard lock(mutex_);
  const EntryPtr* found = tuningEntry(slot->id, slot->def);
  if (found == nullptr) return nullptr;
  dsp::TuningLiveInputs& live = live_[(*found)->slot];  // the slot's, stable for the link's life (T2.6-fix4)
  if (slotOut != nullptr) *slotOut = (*found)->slot;
  switch (ref.kind) {
    case doc::ParamKind::MosGenerator: return &live.generator;
    case doc::ParamKind::MosPeriod:
    case doc::ParamKind::TuningEquave: return &live.period;
    case doc::ParamKind::MosStretch: return &live.stretch;
    case doc::ParamKind::TuningReference: return &live.reference;
    default: return nullptr;
  }
}

void ProjectLink::publishTempo(const doc::Project& p) {
  if (p.timeline.get() == tempoTimeline_) return;
  tempoTimeline_ = p.timeline.get();
  auto img = engine::TempoImage::create(p.timeline->tempo, p.timeline->signatures);
  if (img.ok() && engine_->timeline().publishTempo(std::move(img.value())).ok()) ++stats_.tempoPublishes;
  engine_->wake();
}

void ProjectLink::onDocumentChange(const doc::StoreEvent& e) {
  if (e.kind == doc::StoreEventKind::GestureCommit) return;  // already applied live
  const doc::Snapshot& next = e.after;
  if (!next || next == current_) return;
  const doc::ChangeSet& c = e.changes;
  const bool reset = e.kind == doc::StoreEventKind::Reset;
  const bool relocated = e.kind == doc::StoreEventKind::SamplesRelocated;  // T5.4: sample paths changed
  const bool structural = reset || c.structural();
  current_ = next;
  const doc::Project& p = *current_;
  {
    std::lock_guard lock(mutex_);
    metaDoc_ = next;
    // TMM.13-fix1 H2: a Reset (New / Open) drops every meta slot at the next compile. Device ids restart per project,
    // so a slot keyed by the old project's (device, path, index) would otherwise be the new instance's.
    if (reset) metaReset_ = true;
  }
  // TMM.5: a MetaData edit, or a param (a baked knob), may change a meta module's table: rebuilt off-thread, no compile.
  // A structural change rebuilds after its compile (compileNow).
  // TMM.13-fix1 M11: a DeviceParam only of a device with meta modules (a stock device's knob builds no table).
  if (!structural && (!c.metaData.empty() || std::any_of(c.params.begin(), c.params.end(), [&](const doc::ParamRef& r) {
        if (r.kind == doc::ParamKind::MetaParam) return true;
        if (r.kind != doc::ParamKind::DeviceParam) return false;
        const doc::Device* d = doc::findDevice(p, doc::DeviceId{r.owner}).device;
        return d != nullptr && (doc::isMetaDevice(*d) || d->meta != nullptr);
      })))
    refreshMetaTables();
  if (structural) {  // free the slots of tracks that are gone (FIFO reuse: much later)
    std::vector<uint32_t> alive;
    doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) { alive.push_back(t.id.value); });
    std::sort(alive.begin(), alive.end());
    auto live = [&](uint32_t id) { return std::binary_search(alive.begin(), alive.end(), id); };
    std::lock_guard lock(mutex_);
    for (auto it = trackSlots_.begin(); it != trackSlots_.end();)
      if (!live(it->first)) {
        freeSlots_.push_back(it->second);
        it = trackSlots_.erase(it);
      } else {
        ++it;
      }
    std::vector<uint32_t> tunings;  // tuning node params ("tuning:<id>:<name>") live while their tuning does
    for (const doc::TuningSlotPtr& t : *p.tunings) tunings.push_back(t->id.value);
    std::sort(tunings.begin(), tunings.end());
    std::vector<uint32_t> devices;  // T4.1: device params ("dev<id>:<name>") live while their device does
    doc::forEachDevice(p, [&](const doc::Device& d, const doc::Track&) { devices.push_back(d.id.value); });
    std::sort(devices.begin(), devices.end());
    std::vector<uint32_t> wires;  // T4.7: wire amounts ("wire<id>:amount") live while their wire does
    for (const doc::Wire& w : doc::wiresOf(p)) wires.push_back(w.id.value);
    for (auto it = paramSlots_.begin(); it != paramSlots_.end();) {
      const bool tuningKey = it->first.starts_with("tuning:");
      const bool deviceKey = it->first.starts_with("dev");
      const bool wireKey = it->first.starts_with("wire");
      const uint32_t id = static_cast<uint32_t>(std::strtoul(
          it->first.c_str() + (tuningKey ? 7 : deviceKey ? 3 : wireKey ? 4 : 0), nullptr, 10));
      const bool keep = tuningKey   ? std::binary_search(tunings.begin(), tunings.end(), id)
                         : deviceKey ? std::binary_search(devices.begin(), devices.end(), id)
                         : wireKey   ? std::binary_search(wires.begin(), wires.end(), id)
                                     : live(id);
      if (!keep) {
        freeParams_.push_back(it->second);
        written_[it->second] = std::numeric_limits<float>::quiet_NaN();
        it = paramSlots_.erase(it);
      } else {
        ++it;
      }
    }
  }
  // Tables before the sequences resolve in their definitions. A param change can need a new table too (a region
  // hint moved with the generator: T2.6-fix2), which then is a compile like a structural change.
  bool newTables = false;
  if (reset || structural || (c.has(doc::kChangeParam) && !c.tunings.empty()))
    newTables = updateTuningTables(p, reset);  // a Reset (New / Open) never keeps an earlier project's tables
  // A send level can change the graph (T3.17-fix3): a return's send to another return, or a send that closes a loop,
  // is lowered only while it is stored (lower.h).
  // T4.1: device params only (a device knob drag): write just those; nothing else depends on them. T4.7: a wire's
  // amount (a badge drag) the same way.
  const bool deviceParamsOnly =
      !reset && !structural && c.paramOnly() && !c.params.empty() &&
      std::all_of(c.params.begin(), c.params.end(), [](const doc::ParamRef& r) {
        return r.kind == doc::ParamKind::DeviceParam || r.kind == doc::ParamKind::WireAmount ||
               r.kind == doc::ParamKind::MetaParam;     // TMM.3
      });
  if (deviceParamsOnly) {
    applyDeviceParams(p, c.params);
    return;
  }
  bool sendsChanged = false;
  if (reset || structural || c.has(doc::kChangeParam)) {
    std::vector<uint64_t> key = doc::conditionalSendsKey(p);
    sendsChanged = key != conditionalSends_;
    conditionalSends_ = std::move(key);
    if (sendsChanged || structural) reportDelayedSends(p);
  }
  if (structural) {
    reportMissingDevices(p);
    reportWires(p);
  }
  const bool compile = structural || newTables || sendsChanged;
  if (reset || structural || c.has(doc::kChangeParam)) applyParams(p, reset);
  if (reset || c.has(doc::kChangeTimeline)) publishTempo(p);
  if (reset || compile || c.has(doc::kChangeSequence)) publishSequences(p);
  else if (c.has(doc::kChangeParam) && !c.tunings.empty()) publishSequences(p, true);  // static pitches only
  // T6.4: arm / monitor (Param), MIDI input and note input (Meta), tunings, slots: the live routes (published only when
  // one changed).
  if (reset || compile || c.has(doc::kChangeParam) || c.has(doc::kChangeMeta)) publishLiveRoutes(p, reset);
  // T5.4: audio clips (edits are runtime data, never a compile); a tuning param can move a Steps transpose, a tempo
  // edit the clips' samples. Published only when something an audio track plays changed.
  if (reset || compile || relocated || c.has(doc::kChangeSequence) || c.has(doc::kChangeTimeline) ||
      (c.has(doc::kChangeParam) && !c.tunings.empty())) {
    if (reset || relocated) ++resolution_;
    publishAudio(p);
  }
  if (reset || compile || c.has(doc::kChangeAutomation)) publishAutomation(p);  // taps follow the tables
  if (compile) {
    {
      std::lock_guard lock(mutex_);
      pending_ = true;
      pendingSnap_ = current_;
      pendingSerial_ = ctx_ != nullptr ? ctx_->docSerial() : 0;
      pendingSince_ = nowSeconds();
    }
    cv_.notify_all();
  }
}

void ProjectLink::reportMissingDevices(const doc::Project& p) {
  std::vector<doc::DeviceId> missing;
  std::string first;
  doc::forEachDevice(p, [&](const doc::Device& d, const doc::Track&) {
    if (!d.active) return;
    if (devices_->patchOf(d) == nullptr) {
      // TMM.17: patchOf, not the library by name: a chain meta device ("meta:<type>") plays its type's expansion, which
      // the library holds under another name (it was reported as missing while it played).
      if (missing.empty()) first = "Device '" + d.patch + "' is not available: it is bypassed";
      missing.push_back(d.id);
    }
  });
  if (missing == missingReported_) return;
  missingReported_ = std::move(missing);
  if (!missingReported_.empty() && ctx_ != nullptr)
    ctx_->setStatus(first +
                    (missingReported_.size() > 1 ? " (and " + std::to_string(missingReported_.size() - 1) + " more)" : ""));
}

void ProjectLink::reportWires(const doc::Project& p) {
  wireStates_ = doc::wireStates(p, *devices_);
  std::vector<doc::WireId> flagged;
  const doc::WireState* first = nullptr;
  uint32_t delayed = 0, refused = 0, invalid = 0;
  for (const doc::WireState& s : wireStates_) {
    if (s.status == doc::WireStatus::Delayed) ++delayed;
    if (s.status == doc::WireStatus::Refused) ++refused;
    if (s.status == doc::WireStatus::Invalid) ++invalid;
    if (s.status != doc::WireStatus::Delayed && s.status != doc::WireStatus::Refused &&
        s.status != doc::WireStatus::Invalid)
      continue;
    flagged.push_back(s.id);
    const bool fresh = std::find(wiresReported_.begin(), wiresReported_.end(), s.id) == wiresReported_.end();
    if (first == nullptr && fresh) first = &s;
  }
  stats_.delayedWires = delayed;
  stats_.refusedWires = refused;
  stats_.invalidWires = invalid;
  wiresReported_ = std::move(flagged);
  if (first == nullptr || ctx_ == nullptr) return;
  // SPEC §15 2026-10-24: a wire that closes a delay-free loop gets an automatic 1-sample delay, shown in the UI.
  const doc::Wire* w = doc::findWire(p, first->id);
  auto name = [&](doc::DeviceId d) {
    const doc::DeviceLoc l = doc::findDevice(p, d);
    const doc::DeviceInfo* info = l.device != nullptr ? devices_->infoOf(*l.device) : nullptr;
    const std::string own = l.device != nullptr ? l.device->name : std::string();
    return (!own.empty() ? own : info != nullptr ? info->name : std::string("device")) +
           (l.track != nullptr ? " (" + l.track->name + ")" : "");
  };
  // TMM.10-fix2: a note source is named by its track.
  auto source = [&](const doc::Wire& x) {
    if (!x.fromNoteSource()) return name(x.from);
    const doc::Track* t = doc::findTrackPtr(p, x.noteSource);
    return (t != nullptr ? t->name : std::string("track")) + " notes";
  };
  const std::string what = w != nullptr ? source(*w) + " " + w->port + " \xE2\x86\x92 " + name(w->to) + " " + w->param : "a wire";
  switch (first->status) {
    case doc::WireStatus::Delayed:
      ctx_->setStatus("Feedback: the wire " + what + " closes a loop and is delayed by 1 sample (z-1)");
      break;
    case doc::WireStatus::Invalid:  // T4.4-fix1: an edit removed (or renamed) a port the wire connects
      ctx_->setStatus("The wire " + what + " is not played: that port is not on the device any more (it plays again if "
                      "the port comes back; Delete removes the wire)");
      break;
    default:
      if (first->reason == doc::WireRefusal::VoiceLoop)  // TMM.13-fix1 H3
        ctx_->setStatus("The wire " + what + " is not played: it would close a feedback loop across a voice sum (a "
                        "single-signal path back into per-note blocks, or through two instrument tracks: not run per sample yet); the "
                        "rest of the track plays");
      else
        ctx_->setStatus("The wire " + what + " is not played: it would close a loop through two instrument tracks");
      break;
  }
}

void ProjectLink::reportDelayedSends(const doc::Project& p) {
  std::vector<doc::DelayedSend> d = doc::delayedSends(p);
  stats_.delayedSends = static_cast<uint32_t>(d.size());
  const bool same = d.size() == delayed_.size() &&
                    std::equal(d.begin(), d.end(), delayed_.begin(), [](const doc::DelayedSend& a, const doc::DelayedSend& b) {
                      return a.from == b.from && a.to == b.to;
                    });
  delayed_ = std::move(d);
  if (same || delayed_.empty() || ctx_ == nullptr) return;
  // SPEC §15 2026-10-24: a send that closes a delay-free loop gets an automatic 1-sample delay, shown in the UI.
  auto name = [&](doc::TrackId id) {
    const doc::Track* t = doc::findTrackPtr(p, id);
    return t != nullptr ? t->name : std::to_string(id.value);
  };
  std::string m = "Feedback: the send " + name(delayed_.front().from) + " \xE2\x86\x92 " + name(delayed_.front().to) +
                  " closes a loop and is delayed by 1 sample";
  if (delayed_.size() > 1) m += " (and " + std::to_string(delayed_.size() - 1) + " more)";
  ctx_->setStatus(m);
}

// ---- Compiling --------------------------------------------------------------------------------------------------

void ProjectLink::workerMain() {
  std::unique_lock lock(mutex_);
  // After a compile the worker keeps collecting the program's retired images for a while (the audio thread retires
  // them a fade later: T3.17-fix1, M3 review L13), and retries deferred publishes while there are any.
  uint32_t collects = 0;
  for (;;) {
    const bool deferred = anyDeferred_.load(std::memory_order_acquire);
    if (!pending_ && !quit_ && (deferred || collects > 0)) {
      cv_.wait_for(lock, std::chrono::milliseconds(deferred ? 20 : 100));
      if (quit_) return;
      if (!pending_) {
        lock.unlock();
        {
          std::lock_guard compileLock(compileMutex_);
          bool drained = true;
          if (anyDeferred_.load(std::memory_order_acquire)) (void)drainDeferred(drained);
          if (engine::ProjectProgram* program = engine_->program()) program->collect();
        }
        lock.lock();
        if (collects > 0) --collects;
        continue;
      }
    }
    // M4 review M1: a parked JIT image (or a held topology whose image arrived) wakes the worker too.
    cv_.wait(lock, [&] { return quit_ || pending_ || anyDeferred_.load(std::memory_order_acquire); });
    if (quit_) return;
    if (!pending_) continue;  // deferred work only: the branch above publishes it
    // Debounce: compile once nothing structural happened for debounceMs (flush() skips the wait).
    while (!quit_ && !flushNow_) {
      const double due = pendingSince_ + cfg_.debounceMs * 1e-3;
      const double now = nowSeconds();
      if (now >= due) break;
      cv_.wait_for(lock, std::chrono::duration<double>(due - now));
    }
    if (quit_) return;
    doc::Snapshot s = std::move(pendingSnap_);
    const uint64_t serial = pendingSerial_;
    pending_ = false;
    flushNow_ = false;
    busy_ = true;
    lock.unlock();
    (void)compileNow(s, serial);
    lock.lock();
    busy_ = false;
    collects = 20;  // about 2 s of collects
    cv_.notify_all();
  }
}

void ProjectLink::flush() {
  if (!cfg_.compileThread) {
    doc::Snapshot s;
    uint64_t serial = 0;
    {
      std::lock_guard lock(mutex_);
      if (pending_) {
        s = std::move(pendingSnap_);
        serial = pendingSerial_;
        pending_ = false;
      }
    }
    if (s) {
      (void)compileNow(s, serial);
    } else if (anyDeferred_.load(std::memory_order_acquire)) {
      std::lock_guard compileLock(compileMutex_);
      bool drained = true;
      (void)drainDeferred(drained);
    }
    takeLatencyShifts();  // T6.2 (compiled on the calling thread: the UI thread)
    return;
  }
  std::unique_lock lock(mutex_);
  flushNow_ = true;
  cv_.notify_all();
  cv_.wait(lock, [&] { return quit_ || (!pending_ && !busy_); });
  flushNow_ = false;
}

void ProjectLink::handLowering(const doc::Snapshot& s, uint64_t serial, const std::shared_ptr<const doc::LoweredProject>& lowered) {
  if (!cfg_.compileThread) {  // compiled on the UI thread (flush, init, a rate change)
    if (ctx_ != nullptr) ctx_->setLowering(serial, s, lowered);
    return;
  }
  std::function<void(std::function<void()>)> post;
  {
    std::lock_guard lock(loadMutex_);
    post = post_;
  }
  if (post) post([alive = std::weak_ptr<int>(alive_), this, s, serial, lowered] {
    if (alive.lock() && ctx_ != nullptr) ctx_->setLowering(serial, s, lowered);
  });
}

void ProjectLink::setError(std::string message) {
  std::lock_guard lock(errorMutex_);
  lastError_ = std::move(message);
}

std::string ProjectLink::lastError() const {
  std::lock_guard lock(errorMutex_);
  return lastError_;
}

void ProjectLink::collect() {
  engine_->collect();
  sampleReclaimer_.collect();  // T5.4: residencies a demotion replaced
}

void ProjectLink::collectIdle() {
  const double now = nowSeconds();
  if (now - lastCollect_ < 0.2) return;
  lastCollect_ = now;
  collect();
  takeLoadedSamples();  // T5.4: loads finished without a UI poster
  reportDisk();
}

Status ProjectLink::drainDeferred(bool& drained) {
  drained = false;
  engine::ProjectProgram* program = engine_->program();
  if (program == nullptr) {
    deferred_.clear();
    anyDeferred_.store(false, std::memory_order_release);
    drained = true;
    return Ok{};
  }
  // Waits (bounded) while the audio thread runs. When the output is stopped (Engine::audioStopped) it wakes the
  // engine once (a suspended stream restarts on a wake, AudioOutput, within a few milliseconds) and gives up after
  // 30 ms: the result stays queued for the next drain.
  // TMM.13-fix1 L19: bounded by a deadline (5 s), not 5000 × sleep_for(1 ms): with Windows' default timer resolution a
  // 1-ms sleep lasts ≈ 15.6 ms, so the count waited ≈ 80 s.
  auto waitFor = [&](auto&& ready) -> bool {
    double stoppedSince = -1.0;
    for (const double deadline = nowSeconds() + 5.0; nowSeconds() < deadline;) {
      if (ready()) return true;
      if (engine_->audioStopped()) {
        const double now = nowSeconds();
        if (stoppedSince < 0.0) stoppedSince = now;
        else if (now - stoppedSince > 0.03) return false;
      } else {
        stoppedSince = -1.0;
      }
      engine_->wake();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  auto fail = [&](const Error& e) {
    deferred_.clear();
    anyDeferred_.store(false, std::memory_order_release);
    ++stats_.compileErrors;
    setError(e.message);
    std::fprintf(stderr, "pqdaw: %s\n", e.message.c_str());
    return Status(e);
  };
  while (!deferred_.empty()) {
    Deferred& d = deferred_.front();
    if (d.topology) {
      if (!d.waiting.empty()) {  // M4 review M3: a continued heavy unit's JIT image is on its way (deliverJit)
        anyDeferred_.store(true, std::memory_order_release);
        return Ok{};
      }
      if (!waitFor([&] { return !program->topologyPending(); })) {
        anyDeferred_.store(true, std::memory_order_release);
        return Ok{};
      }
      // The meter map and the published layout change together (read() indexes the published layout).
      std::lock_guard lock(mutex_);
      auto info = program->publishTopology(d.topo.layout, std::move(d.topo.units));
      if (!info.ok()) return fail(info.error());
      meterBus_ = std::move(d.meterMap);
      tapBus_ = std::move(d.tapMap);
      pendingLatency_.reset();  // T6.2: made for the layout this one replaces (d.latency follows)
      ++layoutSerial_;
      ++stats_.topologyChanges;
    } else {
      while (!d.units.empty()) {
        engine::UnitUpdate& u = d.units.front();
        if (!waitFor([&] { return program->canPublish(u.unit); })) {
          anyDeferred_.store(true, std::memory_order_release);
          return Ok{};
        }
        if (auto r = program->publish(u.unit, std::move(u.image)); !r.ok()) return fail(r.error());
        ++stats_.unitPublishes;
        d.units.erase(d.units.begin());
      }
    }
    if (d.latency) {  // T6.2: the plan of this result, for the layout just published
      if (Status st = applyLatency(*d.latency); !st.ok()) return fail(st.error());
      d.latency.reset();
    }
    deferred_.pop_front();
  }
  // M4 review M1: parked JIT images, now that the program's units are the compiler's again (never waits: one that still
  // has no room stays parked for the next drain).
  for (size_t i = 0; i < parked_.size();) {
    ParkedJit& pj = parked_[i];
    const JitPublish r = publishJit(pj.schedule, pj.first, pj.refine, pj.image);
    if (r == JitPublish::NoRoom) {
      ++i;
      continue;
    }
    if (r == JitPublish::Stale) {
      std::lock_guard lock(jitStatsMutex_);
      ++(pj.refine ? jitStats_.refineStale : jitStats_.stale);
    }
    parked_.erase(parked_.begin() + static_cast<std::ptrdiff_t>(i));
  }
  if (pendingLatency_) {  // T6.2: a plan that found no room earlier
    const LatencyJob job = std::move(*pendingLatency_);
    pendingLatency_.reset();
    (void)applyLatency(job);
  }
  anyDeferred_.store(!parked_.empty() || pendingLatency_.has_value(), std::memory_order_release);
  drained = true;  // every compile result is in (parked JIT images are optional: the interpreted ones play)
  engine_->wake();
  return Ok{};
}

// ---- Meta-module resources (TMM.2) -------------------------------------------------------------------------------

VoiceTapConfig ProjectLink::metaTapConfig() const {
  VoiceTapConfig c;
  c.groups = engine::voiceGroupsFor(std::max(cfg_.maxVoices, 1u));
  c.depth = 64;
  c.stride = std::max(cfg_.blockFrames / kVoiceTapSamples, 1u);  // one record per engine block
  return c;
}

std::shared_ptr<const dsp::FrameTable> ProjectLink::metaDefaultTable() {  // mutex_ held
  if (metaDefault_ && metaDefaultRate_ == cfg_.sampleRate) return metaDefault_;
  const AlignedBuffer<float> cycle = dsp::basicCycle(dsp::BasicCycle::Sine);
  const dsp::Keyframe key{0.0f, cycle.span()};
  dsp::FrameTableSpec spec;
  spec.keyframes = std::span<const dsp::Keyframe>(&key, 1);
  spec.denseFrames = 1;
  spec.sampleRate = cfg_.sampleRate;
  dsp::FrameTableBuilder b;
  auto t = b.build(spec);
  if (!t.ok()) return metaDefault_;  // cannot happen at a valid rate
  metaDefault_ = std::make_shared<const dsp::FrameTable>(std::move(t.value()));
  metaDefaultRate_ = cfg_.sampleRate;
  return metaDefault_;
}

void ProjectLink::resetMetaSlotsLocked() {
  for (const auto& [key, id] : curveIds_) {  // GU.1: the curve slots too (device ids restart per project)
    curveSlots_[id].live = false;
    curveFree_.push_back(id);
  }
  curveIds_.clear();
  for (const auto& [key, id] : metaIds_) {
    metaSlots_[id].live = false;  // the next syncMetaSlots makes a new object for the id (images hold the old one)
    metaFree_.push_back(id);
    if (tables_) tables_->forget(id);  // and drops its queued job (L18): nothing built for A reaches B's instance
  }
  metaIds_.clear();
  metaGen_.fetch_add(1, std::memory_order_release);
}

uint32_t ProjectLink::metaSlotFor(doc::DeviceId device, std::string_view path, uint32_t index) {
  std::lock_guard lock(mutex_);
  MetaKey key{device.value, std::string(path), index};
  auto it = metaIds_.find(key);
  if (it == metaIds_.end()) {
    uint32_t id = 0;
    if (!metaFree_.empty()) {
      id = metaFree_.front();
      metaFree_.pop_front();
    } else if (metaNext_ < kMetaSlots) {
      id = metaNext_++;
    }
    if (id == 0) return 0;  // every slot in use: the expansion reads the engine's default (and writes no tap)
    it = metaIds_.emplace(std::move(key), id).first;
  }
  metaTouched_.push_back(it->second);
  return it->second;
}

void ProjectLink::syncMetaSlots(const std::vector<doc::LoweredProject::MetaResource>& resources) {
  std::lock_guard lock(mutex_);
  for (const doc::LoweredProject::MetaResource& r : resources) {
    if (r.slot == 0 || r.slot >= kMetaSlots) continue;
    MetaSlot& m = metaSlots_[r.slot];
    if (!m.live || m.kind != r.request.kind) {  // a new id, or a reused one: a new object (the old one lives on in the
      m = MetaSlot{};                           // images that hold it)
      m.kind = r.request.kind;
      if (m.kind == meta::ResourceKind::FrameTable) {
        metaFresh_.push_back(r.slot);  // TMM.8-fix: its table is built before the image that reads it is published
        m.table = std::make_shared<dsp::FrameTableSlot>(metaDefaultTable(), metaTapConfig());
        m.tap = m.table->sharedTap();
      } else if (m.kind == meta::ResourceKind::SignalTap) {  // TX.spectrum: written per engine block
        m.ring = std::make_shared<TapRing>(dsp::kSignalTapChannels, kAnalysisTapFrames,
                                           std::clamp<uint32_t>(cfg_.blockFrames, 1u, kAnalysisTapFrames / 4));
        tapsFresh_ = true;
      } else {
        m.tap = std::make_shared<VoiceTap>(metaTapConfig());
      }
      m.live = true;
    }
    m.type = meta::findType(r.type);  // TMM.5: what its table is built from
    m.device = r.device;
    m.path = r.path;
    m.index = r.request.index;
    m.modulated = r.modulated;
    frameTableViews_[r.slot] = m.table.get();
    voiceTapViews_[r.slot] = m.kind == meta::ResourceKind::VoiceTap ? m.tap.get() : nullptr;
    signalTapViews_[r.slot] = m.kind == meta::ResourceKind::SignalTap ? m.ring.get() : nullptr;
  }
  // Ids this lowering did not name are free again. Their objects (and view entries) stay until the id is given out
  // again: an image built later from an older schedule (a background JIT compile) may still name the id, and it holds
  // what it reads through keepAlive.
  std::sort(metaTouched_.begin(), metaTouched_.end());
  for (auto it = metaIds_.begin(); it != metaIds_.end();) {
    if (std::binary_search(metaTouched_.begin(), metaTouched_.end(), it->second)) {
      ++it;
      continue;
    }
    metaSlots_[it->second].live = false;
    metaFree_.push_back(it->second);
    if (tables_) tables_->forget(it->second);  // TMM.16: a later instance on this id never previews over its table
    it = metaIds_.erase(it);
  }
  metaGen_.fetch_add(1, std::memory_order_release);  // TMM.13-fix1 M12: after every change above
}

// TMM.8-fix: an instance's slot whatever its table variant (the index's upper bits): the one the newest lowering named.
uint32_t ProjectLink::metaIdLocked(doc::DeviceId device, std::string_view path, uint32_t index) const {
  for (const auto& [key, id] : metaIds_)
    if (std::get<0>(key) == device.value && std::get<1>(key) == path && (std::get<2>(key) & 0xFFFFu) == index &&
        metaSlots_[id].live)
      return id;
  return 0;
}

uint32_t ProjectLink::curveSlotFor(doc::DeviceId device, std::string_view param) {  // GU.1
  std::lock_guard lock(mutex_);
  auto key = std::pair<uint32_t, std::string>{device.value, std::string(param)};
  auto it = curveIds_.find(key);
  if (it == curveIds_.end()) {
    uint32_t id = 0;
    if (!curveFree_.empty()) {
      id = curveFree_.front();
      curveFree_.pop_front();
    } else if (curveNext_ < kCurveSlots) {
      id = curveNext_++;
    }
    if (id == 0) return 0;  // every slot in use: the atom is the identity on the position (and the status says so)
    it = curveIds_.emplace(std::move(key), id).first;
  }
  curveTouched_.push_back(it->second);
  return it->second;
}

void ProjectLink::syncCurveSlots(const std::vector<doc::CurveSite>& sites) {  // GU.1
  std::lock_guard lock(mutex_);
  for (const doc::CurveSite& c : sites) {
    if (c.slot == 0 || c.slot >= kCurveSlots) {
      if (c.slot == 0) setError("Playback: more driven controls than curve slots (" + std::to_string(kCurveSlots) + ")");
      continue;
    }
    CurveEntry& e = curveSlots_[c.slot];
    const dsp::CurveData data = dsp::compileCurve(c.curve);
    if (!e.live) {  // a new id, or a reused one: a new object (the old one lives on in the images that hold it)
      e.slot = std::make_shared<dsp::CurveSlot>(data);
      e.live = true;
    } else {
      e.slot->publish(data, sampleReclaimer_);
    }
    curveViews_[c.slot] = e.slot.get();
  }
  std::sort(curveTouched_.begin(), curveTouched_.end());
  for (auto it = curveIds_.begin(); it != curveIds_.end();) {
    if (std::binary_search(curveTouched_.begin(), curveTouched_.end(), it->second)) {
      ++it;
      continue;
    }
    curveSlots_[it->second].live = false;
    curveFree_.push_back(it->second);
    it = curveIds_.erase(it);
  }
}

void ProjectLink::publishCurve(doc::DeviceId device, std::string_view param, const dsp::Curve& curve) {  // GU.1
  std::lock_guard lock(mutex_);
  const auto it = curveIds_.find(std::pair<uint32_t, std::string>{device.value, std::string(param)});
  if (it == curveIds_.end() || !curveSlots_[it->second].live) return;
  if (curveSlots_[it->second].slot->publish(dsp::compileCurve(curve), sampleReclaimer_)) sampleReclaimer_.collect();
}

uint32_t ProjectLink::curveSlot(doc::DeviceId device, std::string_view param) const {
  std::lock_guard lock(mutex_);
  const auto it = curveIds_.find(std::pair<uint32_t, std::string>{device.value, std::string(param)});
  return it != curveIds_.end() && curveSlots_[it->second].live ? it->second : 0;
}

std::shared_ptr<const dsp::CurveSlot> ProjectLink::curveSlotObject(uint32_t id) const {
  std::lock_guard lock(mutex_);
  return id < kCurveSlots ? curveSlots_[id].slot : nullptr;
}

uint32_t ProjectLink::metaSlot(doc::DeviceId device, std::string_view path, uint32_t index) const {
  std::lock_guard lock(mutex_);
  return metaIdLocked(device, path, index);
}

std::shared_ptr<dsp::FrameTableSlot> ProjectLink::metaFrameTable(doc::DeviceId device, std::string_view path,
                                                                uint32_t index) const {
  std::lock_guard lock(mutex_);
  const uint32_t id = metaIdLocked(device, path, index);
  return id != 0 ? metaSlots_[id].table : nullptr;
}

std::shared_ptr<dsp::FrameTableSlot> ProjectLink::metaFrameTable(doc::DeviceId device, std::string_view path,
                                                                uint32_t index, std::vector<uint8_t>& modulated) const {
  std::lock_guard lock(mutex_);
  const uint32_t id = metaIdLocked(device, path, index);
  modulated = id != 0 ? metaSlots_[id].modulated : std::vector<uint8_t>{};
  return id != 0 ? metaSlots_[id].table : nullptr;
}

std::shared_ptr<VoiceTap> ProjectLink::metaVoiceTap(doc::DeviceId device, std::string_view path, uint32_t index) const {
  std::lock_guard lock(mutex_);
  const uint32_t id = metaIdLocked(device, path, index);
  return id != 0 ? metaSlots_[id].tap : nullptr;
}

std::shared_ptr<TapRing> ProjectLink::metaSignalTap(doc::DeviceId device, std::string_view path, uint32_t index) const {
  std::lock_guard lock(mutex_);
  // A tap has no variants: its key is exact (one map lookup; the displays ask every frame).
  const auto it = metaIds_.find(MetaKey{device.value, std::string(path), index});
  if (it == metaIds_.end() || !metaSlots_[it->second].live) return nullptr;
  return metaSlots_[it->second].ring;
}

std::shared_ptr<TapRing> ProjectLink::deviceAnalysisTap(doc::DeviceId device) const {
  return metaSignalTap(device, "", 0);  // a Spectrum native engine: its own request 0
}

Status ProjectLink::publishFrameTable(doc::DeviceId device, std::string_view path, uint32_t index,
                                      std::shared_ptr<const dsp::FrameTable> table) {
  if (table == nullptr) return linkError(ErrorCode::InvalidArgument, "publishFrameTable: no table");
  const std::shared_ptr<dsp::FrameTableSlot> slot = metaFrameTable(device, path, index);
  if (slot == nullptr) return linkError(ErrorCode::NotFound, "publishFrameTable: no frame-table slot for that instance");
  if (table->layout().sampleRate != cfg_.sampleRate)
    return linkError(ErrorCode::InvalidArgument, "publishFrameTable: the table was built for another sample rate");
  slot->publish(std::move(table), sampleReclaimer_);
  sampleReclaimer_.collect();
  return Ok{};
}

void ProjectLink::refreshMetaTables() {
  if (!tables_) return;
  struct Pending {
    uint32_t id;
    meta::TableJob job;
  };
  std::vector<Pending> jobs;
  // TMM.13-fix1 M11: data values are hashed WITHOUT mutex_ (≈ 2 ms per 256-keyframe table; the UI thread takes mutex_
  // every frame for its taps): the first pass collects the values not hashed yet, the second builds the jobs.
  auto dataOf = [](const MetaSlot& m, const doc::Project& p) -> meta::DataPtr {
    const doc::Device* d = doc::findDevice(p, m.device).device;
    if (d == nullptr) return nullptr;
    const doc::MetaInstance* inst = d->metaAt(m.path);
    const bool own = inst != nullptr && inst->type == m.type->id && inst->data != nullptr;
    return own ? inst->data : m.type->defaultData != nullptr ? m.type->defaultData() : nullptr;
  };
  auto tableSlot = [](const MetaSlot& m) {
    return m.live && m.kind == meta::ResourceKind::FrameTable && m.table && m.type != nullptr && m.type->frameTable != nullptr;
  };
  std::vector<std::pair<meta::DataPtr, uint64_t>> hashed;
  metaRefreshes_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    if (!metaDoc_) return;
    for (const auto& [key, id] : metaIds_) {
      const MetaSlot& m = metaSlots_[id];
      if (!tableSlot(m)) continue;
      meta::DataPtr data = dataOf(m, *metaDoc_);
      if (data != nullptr && data != m.data &&
          std::none_of(hashed.begin(), hashed.end(), [&](const auto& h) { return h.first == data; }))
        hashed.emplace_back(std::move(data), 0);
    }
  }
  for (auto& [data, hash] : hashed) hash = meta::dataHash(*data);
  metaHashes_.fetch_add(hashed.size(), std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    if (!metaDoc_) return;
    const doc::Project& p = *metaDoc_;
    std::vector<double> params;
    for (const auto& [key, id] : metaIds_) {
      MetaSlot& m = metaSlots_[id];
      if (!tableSlot(m)) continue;
      const doc::Device* d = doc::findDevice(p, m.device).device;
      if (d == nullptr) continue;  // gone in the newest snapshot; the next compile frees the slot
      meta::DataPtr data = dataOf(m, p);
      if (data != m.data) {  // immutable values: hash each one once
        const auto h = std::find_if(hashed.begin(), hashed.end(), [&](const auto& x) { return x.first == data; });
        // (A value the first pass did not see: a newer snapshot arrived between the passes; hashed here, rarely.)
        if (data != nullptr && h == hashed.end()) metaHashesLocked_.fetch_add(1, std::memory_order_relaxed);
        m.dataHash = data == nullptr ? 0 : h != hashed.end() ? h->second : meta::dataHash(*data);
        m.data = data;
      }
      params.resize(m.type->params.size());
      for (size_t i = 0; i < params.size(); ++i) params[i] = doc::metaParamValue(*d, m.path, *m.type, i);
      const std::vector<double> settings = doc::metaSettings(*d, m.path, *m.type);
      meta::TableSource src;
      src.data = std::move(data);
      src.dataHash = m.dataHash;
      src.params = params;
      src.settings = settings;
      src.sampleRate = cfg_.sampleRate;
      src.modulated = m.modulated;
      meta::TableJob job;
      if (!m.type->frameTable(src, m.index, job).ok() || job.key == m.wanted) continue;
      m.wanted = job.key;
      if (m.recent.size() < MetaSlot::kRecentKeys) m.recent.push_back(job.key);
      else m.recent[m.recentHead++ % MetaSlot::kRecentKeys] = job.key;
      jobs.push_back({id, std::move(job)});
    }
  }
  for (Pending& j : jobs) tables_->submit(j.id, std::move(j.job));
}

void ProjectLink::deliverMetaTable(uint32_t id, uint64_t key, std::shared_ptr<const dsp::FrameTable> table) {
  std::shared_ptr<dsp::FrameTableSlot> slot;
  {
    std::lock_guard lock(mutex_);
    if (id == 0 || id >= kMetaSlots) return;
    const MetaSlot& m = metaSlots_[id];
    // A table built for another instance (a reused id) or another rate is dropped; one for an earlier state of this
    // slot is published (TMM.16, MetaSlot::recent: tables arrive in submission order, so it is the newest built).
    // (The rate is compared with the defaults' rate, which setSampleRate updates under mutex_; cfg_ is the UI's.)
    if (!m.live || !m.table || table->layout().sampleRate != metaDefaultRate_) return;
    if (key != m.wanted && std::find(m.recent.begin(), m.recent.end(), key) == m.recent.end()) return;
    slot = m.table;
  }
  slot->publish(std::move(table), sampleReclaimer_);
  sampleReclaimer_.collect();
}

bool ProjectLink::waitForMetaTables(double timeoutSeconds) { return tables_ == nullptr || tables_->waitIdle(timeoutSeconds); }

meta::TableBuilder::Stats ProjectLink::metaTableStats() const {
  return tables_ != nullptr ? tables_->stats() : meta::TableBuilder::Stats{};
}

uint32_t ProjectLink::metaSlotsInUse() const {
  std::lock_guard lock(mutex_);
  return static_cast<uint32_t>(metaIds_.size());
}

Status ProjectLink::compileNow(const doc::Snapshot& s, uint64_t serial) {
  std::lock_guard compileLock(compileMutex_);  // vs the JIT worker's deliveries (compiler_, program publishes)
  {
    // The tables this compile names (tuningTable) go to compileHeld_ (its images hold them through keepAlive) and to
    // jobHeld_; when it ends, compileHeld_ keeps only this compile's, so a later bind() of a schedule that still runs
    // (an image compiled in the background) finds its tables, and older ones are released.
    std::lock_guard lock(mutex_);
    jobHeld_.clear();
  }
  struct EndJob {
    ProjectLink& link;
    bool ok = false;
    ~EndJob() {  // a failed compile keeps the earlier compile's tables too (its schedules still run)
      std::lock_guard lock(link.mutex_);
      if (ok) link.compileHeld_ = std::move(link.jobHeld_);
      else link.compileHeld_.merge(link.jobHeld_);
      link.jobHeld_.clear();
    }
  } endJob{*this};
  doc::LowerOptions lo;
  lo.library = &devices_->library();
  lo.externals = devices_->signatures();
  lo.devices = devices_.get();  // T4.4-fix1: user devices
  lo.slotOf = [this](doc::TrackId id) { return slotForTrack(id); };
  // T5.3: an audio track's clip-player slot shares the event-slot allocator (stable per track, freed with it); T5.4:
  // publishAudio fills that slot of the engine's audio sequence image.
  lo.clipSlotOf = lo.slotOf;
  lo.tuningTableOf = [this](doc::TuningId id, const doc::TuningDef& def) { return tuningTable(id, def); };
  // TMM.2: the meta-module resource slots (created / kept / released by syncMetaSlots below).
  {
    std::lock_guard lock(mutex_);
    if (std::exchange(metaReset_, false)) resetMetaSlotsLocked();  // TMM.13-fix1 H2: a new document
  }
  metaTouched_.clear();
  lo.metaSlotOf = [this](doc::DeviceId d, std::string_view path, uint32_t index) { return metaSlotFor(d, path, index); };
  curveTouched_.clear();  // GU.1: the control curves' slots (syncCurveSlots below)
  lo.curveSlotOf = [this](doc::DeviceId d, std::string_view param) { return curveSlotFor(d, param); };
  auto lowered = doc::lower(*s, lo, lowered_.get());
  auto fail = [&](const Error& e) {
    ++stats_.compileErrors;
    setError(e.message);
    std::fprintf(stderr, "pqdaw: %s\n", e.message.c_str());
    return Status(e);
  };
  if (!lowered.ok()) return fail(lowered.error());
  // EQ.4b: shared (no copy) with the response displays, which get it now: before the build and the publish, so a
  // display draws the new structure as soon as it is lowered, whatever the build costs (or if it fails).
  const std::shared_ptr<const doc::LoweredProject> shared = std::make_shared<const doc::LoweredProject>(std::move(lowered.value()));
  const doc::LoweredProject& lp = *shared;
  if (serial != 0) handLowering(s, serial, shared);
  metaFresh_.clear();
  syncMetaSlots(lp.metaResources);  // before any image is built: resourceFor reads the slots by id
  syncCurveSlots(lp.curves);         // GU.1: likewise
  if (std::exchange(tapsFresh_, false) && onAnalysisTaps) {  // TX.spectrum: displays waiting for a tap look again
    std::function<void(std::function<void()>)> post;
    {
      std::lock_guard lock(loadMutex_);
      post = post_;
    }
    if (post) post(onAnalysisTaps);
  }
  refreshMetaTables();              // TMM.5: new instances, settings (the warp mode), the rate: their tables
  // TMM.8-fix: a new frame-table slot (a new instance, or a table variant: a control switching between the baked and
  // the live path, Shape mode) gets its table before the image that reads it plays, so the switch never plays the
  // other path's table (the old image keeps its own slot until the swap). The compile path waits, never the audio
  // thread. TMM.13-fix1 H4: only for those slots, built before other jobs (a drag on another instance neither delays
  // the compile nor is forced into full builds), and only for their first table: a baked instance's preview (its own
  // state at 8 dense frames, ≈ 20 – 40 ms) lets the image play; its full table follows as a click-free swap. So
  // opening a project with n baked Wavetables blocks for about n previews, not n full bakes. Bounded at 2 s.
  if (!metaFresh_.empty() && tables_) (void)tables_->waitFor(metaFresh_, 2.0);
  std::vector<engine::ProjectParamLink> links;
  for (const doc::LoweredParam& p : lp.params) {
    const uint32_t slot = slotForParam(p.key);
    if (slot != kNone) links.push_back({p.track, p.name, slot});
  }
  compiler_->setParamLinks(std::move(links));
  // Dirty tracks: a patch that is not shared with the previous lowering (new text or a new track).
  std::vector<uint32_t> dirty;
  for (uint32_t i = 0; i < lp.patches.size(); ++i) {
    bool same = false;
    if (lowered_)
      for (uint32_t j = 0; j < lowered_->patches.size() && !same; ++j)
        same = lowered_->trackIds[j] == lp.trackIds[i] && lowered_->patches[j] == lp.patches[i];
    if (!same) dirty.push_back(i);
  }
  auto meterMap = [&](const engine::ProjectLayout& layout) {
    std::unordered_map<uint32_t, uint32_t> m;
    for (uint32_t b = 0; b < layout.busKey.size(); ++b) {
      const std::string& k = layout.busKey[b];
      if (k.size() > 4 && k.ends_with(":out")) m[static_cast<uint32_t>(std::strtoul(k.c_str(), nullptr, 10))] = b;
    }
    return m;
  };
  // T4.7: the modulation taps ("<track id>:tap_d<device id>_<param>") → "<device id>:<param>".
  auto tapMap = [&](const engine::ProjectLayout& layout) {
    std::unordered_map<std::string, uint32_t> m;
    for (const doc::ModulationTap& t : lp.taps) {
      const std::string key = std::to_string(lp.trackIds[t.track].value) + ":" + t.port;
      for (uint32_t b = 0; b < layout.busKey.size(); ++b)
        if (layout.busKey[b] == key) m[std::to_string(t.device.value) + ":" + t.param] = b;
    }
    return m;
  };
  auto metered = [](engine::ProjectLayout& layout) {
    layout.busMetered.assign(layout.busKey.size(), 0);
    for (size_t b = 0; b < layout.busKey.size(); ++b)
      layout.busMetered[b] = layout.busKey[b].ends_with(":out") || layout.busKey[b].find(":tap_d") != std::string::npos ? 1 : 0;
  };
  // Publishing: in order, after whatever earlier compiles left deferred (drainDeferred waits, bounded, for the
  // audio thread to take earlier images / topologies, because a refused publish would destroy the image; when the
  // output is stopped it queues instead of waiting: T3.17-fix1, M3 review L11).
  engine::ProjectProgram* program = engine_->program();
  uint32_t compiledUnits = 0;
  if (program == nullptr) {  // the first program (init): before any audio runs
    auto b = compiler_->build(lp.graph);
    if (!b.ok()) return fail(b.error());
    metered(b.value().layout);
    auto prog = engine::ProjectProgram::create(engine_->projectConfig(), b.value().layout);
    if (!prog.ok()) return fail(prog.error());
    {
      std::lock_guard lock(mutex_);  // M4: slots allocated before the program existed (init's applyParams)
      prog.value()->reserveProjectParams(paramCapacity_);
    }
    for (uint32_t u = 0; u < b.value().images.size(); ++u)
      if (auto pub = prog.value()->publish(u, std::move(b.value().images[u])); !pub.ok()) return fail(pub.error());
    compiledUnits = static_cast<uint32_t>(b.value().images.size());
    {  // T6.2: the first plan (a new program: nothing sent before)
      LatencyJob job = planLatency();
      lastPlan_.reset();
      pendingLatency_.reset();
      if (!job.plan.none()) {
        if (Status st = prog.value()->setCompensation(job.plan); !st.ok()) return fail(st.error());
        lastPlan_ = job.plan;
      }
      std::lock_guard lock(mutex_);
      if (paramShift_ != job.shift) shiftsChanged_.store(true, std::memory_order_release);
      paramShift_ = std::move(job.shift);
    }
    {
      std::lock_guard lock(mutex_);
      meterBus_ = meterMap(b.value().layout);
      tapBus_ = tapMap(b.value().layout);
      ++layoutSerial_;
    }
    engine_->setProgram(std::move(prog.value()));
  } else {
    Deferred d;
    std::vector<std::shared_ptr<const graph::Schedule>> before;  // each unit's schedule before this compile
    std::vector<std::vector<uint32_t>> beforeMembers;            // … and its member tracks (M3: rebuilds)
    for (uint32_t u = 0; u < compiler_->numUnits(); ++u) {
      before.push_back(compiler_->unit(u).schedule);
      beforeMembers.push_back(compiler_->unit(u).memberIds);
    }
    auto upd = compiler_->update(lp.graph, dirty);
    if (!upd.ok() && upd.error().code == ErrorCode::Unsupported) {  // a topology change
      auto tb = compiler_->rebuild(lp.graph, dirty);
      if (!tb.ok()) return fail(tb.error());
      metered(tb.value().layout);
      compiledUnits = tb.value().compiled;
      d.topology = true;
      d.meterMap = meterMap(tb.value().layout);
      d.tapMap = tapMap(tb.value().layout);
      d.topo = std::move(tb.value());
      // M4 review M3: a rebuilt unit that continues an earlier one (the same member tracks) which plays a JIT image
      // waits for its own JIT image, like a unit update (holdForJit), instead of publishing an interpreted image that
      // would cost more than interpretBudget (Operator's `voices` setting at 64 voices: about a core).
      for (uint32_t u = 0; u < d.topo.units.size() && u < compiler_->numUnits(); ++u) {
        if (!d.topo.units[u].hasImage) continue;
        const std::vector<uint32_t>& members = compiler_->unit(u).memberIds;
        std::shared_ptr<const graph::Schedule> previous;
        for (size_t o = 0; o < beforeMembers.size() && !previous; ++o)
          if (beforeMembers[o] == members) previous = before[o];
        if (holdForJit(u, previous)) d.waiting.push_back(compiler_->unit(u).schedule);
      }
      if (!d.waiting.empty()) ++stats_.heldTopologies;
    } else if (!upd.ok()) {
      return fail(upd.error());
    } else {
      for (engine::UnitUpdate& u : upd.value()) {
        if (u.unchanged) continue;
        ++compiledUnits;
        if (holdForJit(u.unit, u.unit < before.size() ? before[u.unit] : nullptr)) continue;
        d.units.push_back(std::move(u));
      }
    }
    if (d.topology || !d.units.empty()) {
      d.latency = planLatency();  // T6.2
      const bool queued = !deferred_.empty();
      // M3: a topology still waiting for JIT images is published interpreted now: this result supersedes it (its
      // schedules may no longer be the compiler's, so their JIT images might never come).
      for (Deferred& q : deferred_) {
        for (const auto& sc : q.waiting) std::erase(jitHeld_, sc);
        q.waiting.clear();
      }
      deferred_.push_back(std::move(d));
      bool drained = true;
      if (Status st = drainDeferred(drained); !st.ok()) return st;
      if (!drained || queued) ++stats_.deferredPublishes;
    }
  }
  if (compiledUnits > 0) ++stats_.compiles;
  stats_.unitCompiles += compiledUnits;
  lowered_ = shared;
  endJob.ok = true;
  engine_->wake();
  wantJit();
  return Ok{};
}

// ---- The interpreter → JIT live swap (T1b.5) --------------------------------------------------------------------

// compileMutex_ held, after compiler_->update(): whether unit u's new schedule waits for its JIT image while the unit
// keeps playing the JIT image of `previous` (LinkConfig::interpretBudget).
bool ProjectLink::holdForJit(uint32_t u, const std::shared_ptr<const graph::Schedule>& previous) {
  if (jit_ == nullptr || cfg_.interpretBudget <= 0.0f || previous == nullptr || !deferred_.empty()) return false;
  auto in = [&](const auto& list) { return std::find(list.begin(), list.end(), previous) != list.end(); };
  if (!in(jitLive_) && !in(jitHeld_)) return false;  // it plays an interpreted image: nothing to keep
  const std::shared_ptr<const graph::Schedule>& sc = compiler_->unit(u).schedule;
  const double ns = engine::estimateInterpretedNsPerFrame(*sc, compiler_->imageVoiceGroups(u));
  if (ns * cfg_.sampleRate * 1e-9 <= cfg_.interpretBudget) return false;
  jitHeld_.push_back(sc);
  ++stats_.heldForJit;
  return true;
}

void ProjectLink::wantJit() {
  if (jit_ == nullptr) return;
  std::vector<std::shared_ptr<const graph::Schedule>> done, first;
  std::vector<engine::BackgroundJob> jobs, refinements;
  for (uint32_t u = 0; u < compiler_->numUnits(); ++u) {
    const std::shared_ptr<const graph::Schedule>& sc = compiler_->unit(u).schedule;
    if (std::find(jitDone_.begin(), jitDone_.end(), sc) != jitDone_.end()) {
      done.push_back(sc);  // its JIT image is running (or the JIT failed on it): nothing to do
      continue;
    }
    engine::BackgroundJob j;
    j.schedule = sc;
    j.maxBlockFrames = compiler_->options().maxBlockFrames;
    j.maxVoiceGroups = compiler_->imageVoiceGroups(u);
    j.resources = compiler_->options().resources;
    j.tag = compiler_->unit(u).key;
    if (std::find(jitFirst_.begin(), jitFirst_.end(), sc) != jitFirst_.end()) {  // Q111 C: its final image, after
      first.push_back(sc);                                                       // every unit's first one
      j.refine = true;
      refinements.push_back(std::move(j));
      continue;
    }
    jobs.push_back(std::move(j));
  }
  jitDone_ = std::move(done);  // schedules no unit runs any more are forgotten (and freed)
  jitFirst_ = std::move(first);
  auto running = [&](std::vector<std::shared_ptr<const graph::Schedule>>& list) {
    std::erase_if(list, [&](const std::shared_ptr<const graph::Schedule>& sc) {
      for (uint32_t u = 0; u < compiler_->numUnits(); ++u)
        if (compiler_->unit(u).schedule == sc) return false;
      return true;
    });
  };
  running(jitLive_);
  running(jitHeld_);
  for (engine::BackgroundJob& j : refinements) jobs.push_back(std::move(j));
  {
    std::lock_guard lock(jitStatsMutex_);
    jitStats_.wanted += jobs.size();
  }
  jit_->want(std::move(jobs));
}

void ProjectLink::beforeJit(void* self, const engine::BackgroundJob&) {
  const ProjectLink& link = *static_cast<ProjectLink*>(self);
  link.cfg_.jitBeforeCompile(link.cfg_.jitUser);
}

// compileMutex_ held. Never waits: NoRoom when the unit cannot take an image now (deferred results first, a pending
// topology, a full queue — and T3.17-fix2: only while the queue keeps room for the compile path's next image).
ProjectLink::JitPublish ProjectLink::publishJit(const std::shared_ptr<const graph::Schedule>& sc, bool first, bool refine,
                                                std::unique_ptr<engine::ProgramImage>& image) {
  auto count = [&](auto&& f) {
    std::lock_guard lock(jitStatsMutex_);
    f(jitStats_);
  };
  engine::ProjectProgram* program = engine_->program();
  const uint32_t u = program != nullptr ? compiler_->unitOf(sc.get()) : engine::kNoUnit;
  if (u == engine::kNoUnit) return JitPublish::Stale;  // the unit was recompiled or removed meanwhile
  if (!deferred_.empty() || !program->canPublish(u) || program->queued(u) > 1) return JitPublish::NoRoom;
  auto bound = compiler_->bind(u, std::move(image));
  // Bit-identical to the interpreted image (no contraction, T1b.1-fix1), and a first JIT image to its final one
  // (Q111 C: voice groups and pairs compute the same bits): an instant swap is sample-exact. A held edit
  // (interpretBudget) replaces an earlier schedule's image: the default policy (crossfaded while audible).
  const bool held = std::find(jitHeld_.begin(), jitHeld_.end(), sc) != jitHeld_.end();
  const engine::SwapPolicy policy = held ? engine::SwapPolicy{} : engine::SwapPolicy{engine::SwapMode::Instant};
  if (!bound.ok() || !program->publish(u, std::move(bound.value()), policy).ok()) {
    count([&](JitStats& st) { ++(refine ? st.refineErrors : st.errors); });
    return JitPublish::Failed;
  }
  std::erase(jitHeld_, sc);
  if (std::find(jitLive_.begin(), jitLive_.end(), sc) == jitLive_.end()) jitLive_.push_back(sc);
  std::erase(jitFirst_, sc);
  (first ? jitFirst_ : jitDone_).push_back(sc);
  count([&](JitStats& st) {
    if (refine) ++st.refined;
    else ++st.published, st.firstImages += first ? 1 : 0;
  });
  return JitPublish::Published;
}

void ProjectLink::park(ParkedJit p) {  // compileMutex_ held
  for (ParkedJit& q : parked_)
    if (q.schedule == p.schedule) {  // a newer image of the same schedule (the refined one) replaces it
      q = std::move(p);
      return;
    }
  parked_.push_back(std::move(p));
  ++stats_.parkedJit;
  anyDeferred_.store(true, std::memory_order_release);
  notifyWorker();
}

void ProjectLink::notifyWorker() {
  { std::lock_guard lock(mutex_); }  // the worker reads anyDeferred_ under mutex_ before it waits: no lost wake-up
  cv_.notify_all();
}

void ProjectLink::deliverJit(void* self, const engine::BackgroundJob& job, Result<std::unique_ptr<engine::ProgramImage>> img) {
  ProjectLink& link = *static_cast<ProjectLink*>(self);
  auto count = [&](auto&& f) {
    std::lock_guard lock(link.jitStatsMutex_);
    f(link.jitStats_);
  };
  // M4 review M3: the held topology change whose continued unit runs this schedule (compileMutex_ held). A waiting
  // topology is always the last deferred result (a later compile releases it).
  auto waitingTopology = [&]() -> Deferred* {
    if (link.deferred_.empty()) return nullptr;
    Deferred& d = link.deferred_.back();
    return d.topology && std::find(d.waiting.begin(), d.waiting.end(), job.schedule) != d.waiting.end() ? &d : nullptr;
  };
  if (!img.ok()) {
    std::fprintf(stderr, "pqdaw: JIT: %s (the unit stays %s)\n", img.error().message.c_str(),
                 job.refine ? "on its first JIT image" : "interpreted");
    count([&](JitStats& st) { ++(job.refine ? st.refineErrors : st.errors); });
    std::lock_guard lock(link.compileMutex_);
    std::erase(link.jitFirst_, job.schedule);
    link.jitDone_.push_back(job.schedule);  // not retried
    if (Deferred* d = waitingTopology()) {  // M3: the topology goes out with the interpreted image
      std::erase(d->waiting, job.schedule);
      std::erase(link.jitHeld_, job.schedule);
      link.anyDeferred_.store(true, std::memory_order_release);
      link.notifyWorker();
      return;
    }
    if (std::find(link.jitHeld_.begin(), link.jitHeld_.end(), job.schedule) == link.jitHeld_.end()) return;
    // A held edit (interpretBudget) whose JIT failed: play it interpreted after all, as an ordinary edit would. M4
    // review M2: through deferred_ (in order with every other compile result, published once the unit has room, the
    // result checked there: a refused publish is a compile error), never dropped.
    std::erase(link.jitHeld_, job.schedule);
    const uint32_t u = link.engine_->program() != nullptr ? link.compiler_->unitOf(job.schedule.get()) : engine::kNoUnit;
    if (u == engine::kNoUnit) return;  // recompiled meanwhile: the newer image is on its way
    engine::InterpretOptions io;
    io.maxBlockFrames = job.maxBlockFrames;
    io.maxVoiceGroups = job.maxVoiceGroups;
    io.resources = job.resources;
    auto interp = engine::buildInterpreted(*job.schedule, io);
    Result<engine::UnitImage> bound = interp.ok() ? link.compiler_->bind(u, std::move(interp.value()))
                                                  : Result<engine::UnitImage>(interp.error());
    if (!bound.ok()) {
      ++link.stats_.compileErrors;
      link.setError(bound.error().message);
      std::fprintf(stderr, "pqdaw: %s\n", bound.error().message.c_str());
      return;
    }
    Deferred d;
    engine::UnitUpdate upd;
    upd.unit = u;
    upd.image = std::move(bound.value());
    d.units.push_back(std::move(upd));
    link.deferred_.push_back(std::move(d));
    link.anyDeferred_.store(true, std::memory_order_release);
    link.notifyWorker();
    return;
  }
  if (!job.refine) count([&](JitStats& st) { ++st.built; });
  {
    std::lock_guard lock(link.compileMutex_);
    if (Deferred* d = waitingTopology()) {  // M3: into the held topology, which is published with it
      const uint32_t u = link.compiler_->unitOf(job.schedule.get());
      if (u < d->topo.units.size() && d->topo.units[u].hasImage) {
        auto bound = link.compiler_->bind(u, std::move(img.value()));
        if (bound.ok()) {
          d->topo.units[u].image = std::move(bound.value());
          std::erase(link.jitHeld_, job.schedule);
          link.jitLive_.push_back(job.schedule);
          std::erase(link.jitFirst_, job.schedule);
          (job.first ? link.jitFirst_ : link.jitDone_).push_back(job.schedule);
          count([&](JitStats& st) {
            if (job.refine) ++st.refined;
            else ++st.published, st.firstImages += job.first ? 1 : 0;
          });
        } else {
          count([&](JitStats& st) { ++(job.refine ? st.refineErrors : st.errors); });
        }
      }
      std::erase(d->waiting, job.schedule);
      link.anyDeferred_.store(true, std::memory_order_release);
      link.notifyWorker();
      return;
    }
  }
  // Publish into the unit that still runs this schedule. While the audio thread runs, a full unit queue soon has room
  // again: retry without holding the lock (the compile path may need it, and on a test's own thread the engine only
  // runs once that path returns), for at most 5 s. M4 review M1: when the output is stopped or earlier compile results
  // wait (and after the 5 s), the image is parked for drainDeferred instead of dropped.
  // TMM.13-fix1 L19: the 5 s by a deadline (5000 × sleep_for(1 ms) is ≈ 80 s at Windows' default timer resolution).
  const double deadline = nowSeconds() + 5.0;
  for (;;) {
    {
      std::lock_guard lock(link.compileMutex_);
      const JitPublish r = link.publishJit(job.schedule, job.first, job.refine, img.value());
      if (r == JitPublish::Published || r == JitPublish::Failed) return;
      if (r == JitPublish::Stale || link.jitWorker_->stopping()) break;
      if (!link.deferred_.empty() || link.engine_->audioStopped() || nowSeconds() >= deadline) {
        link.park(ParkedJit{job.schedule, job.first, job.refine, std::move(img.value())});
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  count([&](JitStats& st) { ++(job.refine ? st.refineStale : st.stale); });
}

bool ProjectLink::waitForJit(double timeoutSeconds) {
  flush();
  const bool idle = jit_ == nullptr || jit_->waitIdle(timeoutSeconds);
  if (anyDeferred_.load(std::memory_order_acquire)) {  // M4 review M1, M3: parked images, a held topology
    std::lock_guard compileLock(compileMutex_);
    bool drained = true;
    (void)drainDeferred(drained);
  }
  return idle;
}

JitStats ProjectLink::jitStats() const {
  std::lock_guard lock(jitStatsMutex_);
  JitStats st = jitStats_;
  if (jit_ != nullptr) st.totalCompileMs = jit_->stats().totalBuildMs;
  return st;
}

// ---- Transport (UI thread) --------------------------------------------------------------------------------------

void ProjectLink::postTransport(const engine::TransportCommand& c) {
  engine::TransportReport r;
  while (engine_->timeline().poll(r)) report_ = r;  // older reports describe the state before this command
  if (engine_->post(c)) ++commandsPosted_;
  ++reportSeq_;
}

void ProjectLink::play(Ticks from) {
  if (!playhead().playing) prefetchAt(from);  // T5.4: usually done already by the locate that set `from`
  engageDisk(true);
  postTransport({engine::TransportCommandKind::Play, false, from, 0, 0});
  optimistic_ = {true, from, 0};
}

void ProjectLink::stop() {
  const views::PlayheadSnapshot now = playhead();
  postTransport({engine::TransportCommandKind::Stop, false, 0, 0, 0});
  optimistic_ = {false, now.tick, 0};
  engageDisk(false);  // T5.4: the disk thread blocks again once no stream is claimed
  prefetchAt(now.tick);
}

void ProjectLink::continuePlayback() {
  const views::PlayheadSnapshot now = playhead();
  if (!now.playing) prefetchAt(now.tick);
  engageDisk(true);
  postTransport({engine::TransportCommandKind::Continue, false, 0, 0, 0});
  optimistic_ = {true, now.tick, 0};
}

void ProjectLink::locate(Ticks tick) {
  const views::PlayheadSnapshot now = playhead();
  if (!now.playing) prefetchAt(tick);  // T5.4: Play then starts with the streams filled
  postTransport({engine::TransportCommandKind::Locate, false, tick, 0, 0});
  optimistic_ = {now.playing, tick, 0};
}

void ProjectLink::setLoop(const views::TransportLoop& loop) {
  const views::PlayheadSnapshot now = playhead();
  postTransport({engine::TransportCommandKind::SetLoop, loop.enabled, 0, loop.start, loop.end});
  const bool loopStartMoved = loop.enabled && (!loopSent_ || !loop_.enabled || loop_.start != loop.start);
  loop_ = loop;
  loopSent_ = true;
  optimistic_ = now;
  if (loopStartMoved && !now.playing) {  // T5.4: the loop start's streams too
    prefetchedAt_ = INT64_MIN;
    prefetchAt(now.tick);
  }
}

views::PlayheadSnapshot ProjectLink::playhead() {
  engine::TransportReport r;
  bool fresh = false;
  while (engine_->timeline().poll(r)) {
    report_ = r;
    fresh = true;
  }
  if (fresh) ++reportSeq_;
  collectIdle();
  takeLatencyShifts();  // T6.2
  views::PlayheadSnapshot s;
  if (report_.commands >= commandsPosted_) {
    s.playing = report_.playing;
    s.tick = report_.tick;
  } else {  // the audio thread has not taken the last command yet: show what it will do
    s.playing = optimistic_.playing;
    s.tick = optimistic_.tick;
  }
  s.sequence = reportSeq_;
  return s;
}

void ProjectLink::auditionHz(float hz, float velocity, float durationMs) {
  uint32_t slot = kNone;
  if (ctx_ != nullptr) {
    const doc::TrackId t = ctx_->detailClip().track;
    if (t.value != 0) slot = eventSlot(t);
  }
  if (slot == kNone)  // no clip in focus: the first MIDI track
    forEachMidi(*current_, [&](const doc::TrackPath&, const doc::Track& t) {
      if (slot == kNone) slot = eventSlot(t.id);
    });
  if (slot == kNone) return;
  const auto frames = static_cast<uint32_t>(std::clamp(durationMs, 1.0f, 10000.0f) * 1e-3 * cfg_.sampleRate);
  engine_->audition(slot, hz, velocity, frames);
}

void ProjectLink::releaseAuditions() { engine_->releaseAuditions(); }

// ---- Meters (UI thread) -----------------------------------------------------------------------------------------

bool ProjectLink::live() const noexcept { return !engine_->asleep() || metersLit_; }  // asleep ⇒ stopped

void ProjectLink::readLevels(int who, std::span<const uint32_t> buses, std::span<engine::BusLevel> out) {
  engine::ProjectProgram* program = engine_->program();
  std::vector<uint32_t> all;
  std::vector<engine::BusLevel> levels;
  {
    std::lock_guard lock(mutex_);  // the maps and the program's published layout change together (compileNow)
    if (layoutSerial_ != levelsSerial_) {  // other bus indices: what is pending belongs to the old layout
      pendingLevels_[0].clear();
      pendingLevels_[1].clear();
      levelsSerial_ = layoutSerial_;
    }
    // Every metered bus at once: the program restarts all of them at each read.
    for (const auto& [id, b] : meterBus_) all.push_back(b);
    for (const auto& [key, b] : tapBus_) all.push_back(b);
    levels.resize(all.size());
    if (program != nullptr) program->readMeters(all, levels);
  }
  const double now = nowSeconds();
  const double dt = std::clamp(now - lastLevelRead_, 1e-3, 1.0);
  lastLevelRead_ = now;
  for (size_t i = 0; i < all.size(); ++i)
    for (std::unordered_map<uint32_t, LevelAcc>& pending : pendingLevels_) {
      LevelAcc& a = pending[all[i]];
      a.channels = std::max(a.channels, levels[i].channels);
      for (int c = 0; c < 2; ++c) {
        a.peak[c] = std::max(a.peak[c], levels[i].peak[c]);
        a.msTime[c] += static_cast<double>(levels[i].meanSquare[c]) * dt;
      }
      a.seconds += dt;
    }
  for (size_t i = 0; i < buses.size() && i < out.size(); ++i) {
    engine::BusLevel l;
    const auto it = pendingLevels_[who].find(buses[i]);
    if (buses[i] != kNone && it != pendingLevels_[who].end()) {
      l.channels = it->second.channels;
      for (int c = 0; c < 2; ++c) {
        l.peak[c] = it->second.peak[c];
        l.meanSquare[c] = it->second.seconds > 0.0 ? static_cast<float>(it->second.msTime[c] / it->second.seconds) : 0.0f;
      }
      pendingLevels_[who].erase(it);
    }
    out[i] = l;
  }
}

bool ProjectLink::modulationLive() const noexcept { return !engine_->asleep(); }

void ProjectLink::readModulation(std::span<const views::ModTarget> targets, std::span<views::ModSweep> out) noexcept {
  const size_t n = std::min(targets.size(), out.size());
  std::vector<uint32_t> buses(n, kNone);
  {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < n; ++i)
      if (auto it = tapBus_.find(std::to_string(targets[i].device.value) + ":" + targets[i].param); it != tapBus_.end())
        buses[i] = it->second;
  }
  std::vector<engine::BusLevel> levels(n);
  readLevels(1, buses, levels);
  const bool asleep = engine_->asleep();
  for (size_t i = 0; i < n; ++i) {
    views::ModSweep s;
    s.known = buses[i] != kNone;
    if (s.known && !asleep && levels[i].channels >= 2) {  // tap = (up, down) ≥ 0: the peaks are the sweep
      s.up = levels[i].peak[0];
      s.down = levels[i].peak[1];
    }
    out[i] = s;
  }
}

void ProjectLink::read(std::span<const doc::TrackId> ids, std::span<ui::MeterLevels> out) noexcept {
  const size_t n = std::min(ids.size(), out.size());
  std::vector<uint32_t> buses(n, kNone);
  std::vector<engine::BusLevel> levels(n);
  {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < n; ++i)
      if (auto it = meterBus_.find(ids[i].value); it != meterBus_.end()) buses[i] = it->second;
  }
  readLevels(0, buses, levels);
  // Asleep, nothing runs and every bus is silent; the program's published levels are those of the last block it ran
  // (it restarts the accumulation only when it runs again), which froze the meters lit (T3.17-fix1, M3 review L13).
  if (engine_->asleep())
    for (engine::BusLevel& l : levels) l.peak[0] = l.peak[1] = l.meanSquare[0] = l.meanSquare[1] = 0.0f;
  const double now = nowSeconds();
  const double dt = std::clamp(now - lastMeterRead_, 0.0, 1.0);
  lastMeterRead_ = now;
  constexpr float kFallDbPerSecond = 26.0f;  // PQ Meter-like release of the peak bar
  constexpr double kHoldSeconds = 1.5;
  bool lit = false;
  for (size_t i = 0; i < n; ++i) {
    ui::MeterLevels m;
    MeterState& st = meters_[ids[i].value];
    const engine::BusLevel& l = levels[i];
    m.channels = 2;
    for (int c = 0; c < 2; ++c) {
      const float pk = l.channels > c ? dbFromLinear(l.peak[c], false) : -200.0f;
      const float rms = l.channels > c ? dbFromLinear(l.meanSquare[c], true) : -200.0f;
      st.peakDb[c] = std::max(pk, st.peakDb[c] - kFallDbPerSecond * static_cast<float>(dt));
      if (pk >= st.holdDb[c] || now >= st.holdUntil[c]) {
        st.holdDb[c] = pk;
        st.holdUntil[c] = now + kHoldSeconds;
      }
      if (l.channels > c && l.peak[c] >= 1.0f) st.clip[c] = true;  // latched until resetClip (a click), as in Live
      m.peakDb[c] = st.peakDb[c] < -199.0f ? -200.0f : st.peakDb[c];
      m.rmsDb[c] = rms;
      m.holdDb[c] = st.holdDb[c];
      m.clip[c] = st.clip[c];
      constexpr float kFloor = ui::signal_theme::kMeterFloorDb;  // nothing below it is drawn
      // M4 review M5: a latched clip lamp is not "lit": it is static once drawn (its reset is a click, which redraws),
      // so it must not keep the 30 Hz meter poll (live()) running on an idle UI.
      lit = lit || m.peakDb[c] > kFloor || m.rmsDb[c] > kFloor || m.holdDb[c] > kFloor;
    }
    out[i] = m;
  }
  metersLit_ = lit;
  collectIdle();
}

void ProjectLink::resetClip(doc::TrackId id) noexcept {
  if (auto it = meters_.find(id.value); it != meters_.end()) it->second.clip[0] = it->second.clip[1] = false;
}

// ---- Offline ------------------------------------------------------------------------------------------------


}  // namespace pq::app
