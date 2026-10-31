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

#include "pq/app/device_player.h"
#include "pq/compile/compile.h"
#include "pq/document/audibility.h"
#include "pq/document/clip_time.h"
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
  for (size_t ci = 0; ci < track.clips.size(); ++ci) {
    const doc::NoteClip& c = *track.clips[ci].clip;
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
  for (const doc::ClipSlot& c : t.clips) {
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
  inspector_ = std::make_unique<DeviceInspector>();  // T4.4-fix1
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
    jit_ = std::make_unique<engine::BackgroundCompiler>(bc);
    jitWorker_ = jit_.get();
  }
  current_ = initial;
  conditionalSends_ = doc::conditionalSendsKey(*current_);
  reportDelayedSends(*current_);
  reportWires(*current_);
  updateTuningTables(*current_);  // T2.6-fix1: before the first lowering names their indices
  applyParams(*current_, true);
  if (Status s = compileNow(current_); !s.ok()) return s;
  publishTempo(*current_);
  publishSequences(*current_);
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
  auto eng = engine::Engine::create(ec);
  if (!eng.ok()) return eng.error();
  engine_ = std::move(eng.value());
  engine::ProjectCompileOptions co;
  co.sampleRate = cfg_.sampleRate;
  co.maxBlockFrames = cfg_.blockFrames;
  co.voice.maxVoices = std::max(cfg_.maxVoices, 1u);
  co.visualMode = true;
  resources_.tunings = tuningViews_;  // fixed array: entries are filled in as tables are built
  co.resources = &resources_;
  // T2.6-fix4: every image holds the tuning tables it reads (the ones its compile named: compileHeld_).
  co.keepAlive = [this](dsp::Resource kind, uint32_t index) -> std::shared_ptr<const void> {
    if (kind != dsp::Resource::Tuning) return nullptr;
    std::lock_guard lock(mutex_);
    const auto it = compileHeld_.find(index);
    return it != compileHeld_.end() ? it->second : nullptr;
  };
  // T4.4-fix1: the circuit editor's device inspection (probe taps, cost meter) on the inspected unit's images.
  // T4.13: every other unit's images get its schedule's cost meter (the project patch view's per-device labels); an image
  // has one meter, so the inspected unit is measured by the editor's meter only.
  // M4 review H2, L4: the meter / tap slots an image writes are kept alive by that image (UnitImage::keepAlive).
  co.onBind = [this](uint32_t, const engine::CompiledUnit& cu, engine::ProgramImage& image) -> std::shared_ptr<const void> {
    if (cu.inspect) return inspector_->bind(cu, image);
    return bindCost(image, cu.schedule);
  };
  {
    std::lock_guard lock(mutex_);
    if (inspect_) co.inspect.push_back(*inspect_);
  }
  compiler_ = std::make_unique<engine::ProjectCompiler>(co);
  return Ok{};
}

void ProjectLink::setDeviceInspect(doc::DeviceId device, std::vector<graph::ProbeTarget> targets,
                                   std::vector<uint32_t> ids, bool inspect) {
  const doc::DeviceLoc l = current_ ? doc::findDevice(*current_, device) : doc::DeviceLoc{};
  std::optional<engine::TrackInspect> want;
  if (inspect && l.track != nullptr) {
    engine::TrackInspect ti;
    ti.trackId = l.track->id.value;
    const graph::NodeId call{doc::nodeIdFor(doc::NodeRole::Device, device.value, 0)};
    for (graph::ProbeTarget& t : targets) t.path.insert(t.path.begin(), call);  // paths from the track's patch
    ti.targets = std::move(targets);
    ti.ids = std::move(ids);
    ti.ids.resize(ti.targets.size(), 0);
    want = std::move(ti);
  }
  {
    std::lock_guard lock(mutex_);
    if (!inspect && inspectDevice_ != device) return;  // another device's editor is inspecting
    auto same = [](const engine::TrackInspect& a, const engine::TrackInspect& b) {
      if (a.trackId != b.trackId || a.ids != b.ids || a.targets.size() != b.targets.size()) return false;
      for (size_t i = 0; i < a.targets.size(); ++i)
        if (a.targets[i].path != b.targets[i].path || a.targets[i].node != b.targets[i].node ||
            a.targets[i].output != b.targets[i].output)
          return false;
      return true;
    };
    if (want.has_value() == inspect_.has_value() && (!want || same(*want, *inspect_))) return;
    if (inspect_) inspectDirty_.push_back(inspect_->trackId);
    if (want) inspectDirty_.push_back(want->trackId);
    inspect_ = std::move(want);
    inspectDevice_ = inspect ? device : doc::DeviceId{};
    inspectChanged_ = true;
    // A compile of the snapshot the engine follows (no document change: only the inspected unit recompiles).
    pending_ = true;
    pendingSnap_ = current_;
    pendingSince_ = nowSeconds();
  }
  cv_.notify_all();
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
  }
  meters_.clear();
  written_.assign(written_.size(), std::numeric_limits<float>::quiet_NaN());
  tempoTimeline_ = nullptr;
  report_ = engine::TransportReport{};
  commandsPosted_ = 0;
  applyParams(*current_, true);
  const Status compiled = compileNow(current_);
  publishTempo(*current_);
  publishSequences(*current_);
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
  }
}

void ProjectLink::detach() {
  observer_ = views::ViewContext::Subscription();
  sub_ = views::ViewContext::Subscription();
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
    // T4.8: a rack chain's mixer or zone: the derived params of that chain (only those the lowered patch has).
    if (ref.kind == doc::ParamKind::ChainMixer) {
      for (const doc::LoweredParam& lp : doc::chainParamValues(p, doc::ChainId{ref.owner})) writeLinked(lp.key, lp.value);
      continue;
    }
    const doc::DeviceLoc d = doc::findDevice(p, doc::DeviceId{ref.owner});
    if (d.device == nullptr) continue;
    if (d.device->rack) {  // T4.8: a macro or the chain selector: one param write (when the patch reads it)
      for (uint32_t i = 0; i <= doc::kMacroCount; ++i) {
        const std::string name = i == doc::kMacroCount ? std::string(doc::kSelectParam) : doc::macroParamName(i);
        if (doc::deviceParamHash(name) != ref.target) continue;
        writeLinked("dev" + std::to_string(ref.owner) + ":" + name, static_cast<float>(doc::rackParamValue(*d.device, name)));
        break;
      }
      continue;
    }
    std::shared_ptr<const doc::ResolvedDevice> hold;  // T4.4-fix1: a user device plays its own patch
    const graph::Patch* callee = devices_->patchOf(*d.device, hold);
    if (callee == nullptr) continue;  // bypassed (missing): nothing reads it
    for (graph::NodeIndex v = 0; v < callee->nodeCount(); ++v) {
      if (callee->node(v).kind != graph::NodeKind::Param) continue;
      const std::string& name = callee->nodeName(v);
      if (doc::deviceParamHash(name) != ref.target) continue;
      const doc::DeviceValue* set = d.device->param(name);
      const auto value = static_cast<float>(set != nullptr ? set->value : callee->paramDecl(v).defaultValue.value());
      const uint32_t s = slotForParam("dev" + std::to_string(ref.owner) + ":" + name);
      if (s != kNone) (void)writeParam(s, value);
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

// T4.18: the range of a device-kind lane's param: a device param's from the patch the device plays (a rack's macros
// and selector 0 … 127), else the document's.
doc::ParamRange ProjectLink::laneRange(const doc::Project& p, const doc::AutomationLane& lane) const {
  if (lane.param.kind != doc::ParamKind::DeviceParam) return doc::paramRange(lane.param);
  const doc::Device* d = doc::findDevice(p, doc::DeviceId{lane.param.owner}).device;
  if (d == nullptr) return doc::paramRange(lane.param);
  if (d->rack) return {0.0, doc::kMacroMax};
  std::shared_ptr<const doc::ResolvedDevice> hold;
  if (const doc::DeviceInfo* info = devices_->infoOf(*d, hold))
    if (const doc::DeviceParamInfo* pi = info->param(lane.paramName); pi != nullptr && pi->hasRange)
      return {std::min(pi->min, pi->max), std::max(pi->min, pi->max)};
  return doc::paramRange(lane.param);
}

void ProjectLink::publishAutomation(const doc::Project& p) {
  std::vector<engine::AutomationLaneDesc> lanes;
  std::vector<uint32_t> automatedSlots;
  std::array<uint8_t, kTuningTables> tapped{};  // L18
  std::string inert;  // lanes on a tuning without a signal: nothing follows them (T3.17-fix3, M3 review M3)
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::LanePtr& lane : t.lanes) {
      if (lane->points.empty()) continue;
      const std::string key = doc::paramKey(*lane);  // T4.18: a device param's key carries its name
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
    std::vector<uint32_t> chains;   // T4.8: rack chain params ("chain<id>:<name>") while their chain does
    doc::forEachDevice(p, [&](const doc::Device& d, const doc::Track&) {
      devices.push_back(d.id.value);
      if (d.rack)
        for (const doc::ChainPtr& c : d.rack->chains) chains.push_back(c->id.value);
    });
    std::sort(devices.begin(), devices.end());
    std::sort(chains.begin(), chains.end());
    std::vector<uint32_t> wires;  // T4.7: wire amounts ("wire<id>:amount") live while their wire does
    for (const doc::Wire& w : doc::wiresOf(p)) wires.push_back(w.id.value);
    for (auto it = paramSlots_.begin(); it != paramSlots_.end();) {
      const bool tuningKey = it->first.starts_with("tuning:");
      const bool deviceKey = it->first.starts_with("dev");
      const bool wireKey = it->first.starts_with("wire");
      const bool chainKey = it->first.starts_with("chain");
      const uint32_t id = static_cast<uint32_t>(std::strtoul(
          it->first.c_str() + (tuningKey ? 7 : deviceKey ? 3 : wireKey ? 4 : chainKey ? 5 : 0), nullptr, 10));
      const bool keep = tuningKey   ? std::binary_search(tunings.begin(), tunings.end(), id)
                         : deviceKey ? std::binary_search(devices.begin(), devices.end(), id)
                         : wireKey   ? std::binary_search(wires.begin(), wires.end(), id)
                         : chainKey  ? std::binary_search(chains.begin(), chains.end(), id)
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
               r.kind == doc::ParamKind::ChainMixer;  // T4.8
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
      pendingSince_ = nowSeconds();
    }
    cv_.notify_all();
  }
}

void ProjectLink::reportMissingDevices(const doc::Project& p) {
  std::vector<doc::DeviceId> missing;
  std::string first;
  doc::forEachDevice(p, [&](const doc::Device& d, const doc::Track& t) {  // T4.8: inside racks too (racks have no patch)
    if (!d.active || d.rack) return;
    if (d.edited()) {  // T4.4-fix1: a user device whose text does not play
      const auto r = devices_->resolve(d);
      if (r && r->error.empty()) return;
      if (missing.empty()) {
        const doc::DeviceInfo* info = devices_->catalog().find(d.patch);
        const std::string why = r ? r->error.substr(0, r->error.find('\n')) : std::string("no device library");
        first = "The edited " + (info ? info->name : d.patch) + " (" + t.name + ") does not play its text (" + why +
                (r && r->stale ? "): its last good version plays" : "): it is bypassed");
      }
      missing.push_back(d.id);
    } else if (devices_->library().find(d.patch) == nullptr) {
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
    const doc::DeviceInfo* info = l.device != nullptr ? devices_->catalog().find(l.device->patch) : nullptr;
    return (info != nullptr ? info->name : std::string("device")) + (l.device != nullptr && l.device->edited() ? " (edited)" : "") +
           (l.track != nullptr ? " (" + l.track->name + ")" : "");
  };
  const std::string what = w != nullptr ? name(w->from) + " " + w->port + " \xE2\x86\x92 " + name(w->to) + " " + w->param : "a wire";
  switch (first->status) {
    case doc::WireStatus::Delayed:
      ctx_->setStatus("Feedback: the wire " + what + " closes a loop and is delayed by 1 sample (z-1)");
      break;
    case doc::WireStatus::Invalid:  // T4.4-fix1: an edit removed (or renamed) a port the wire connects
      ctx_->setStatus("The wire " + what + " is not played: that port is not on the device any more (it plays again if "
                      "the port comes back; Delete removes the wire)");
      break;
    default:
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
    pending_ = false;
    flushNow_ = false;
    busy_ = true;
    lock.unlock();
    (void)compileNow(s);
    lock.lock();
    busy_ = false;
    collects = 20;  // about 2 s of collects
    cv_.notify_all();
  }
}

void ProjectLink::flush() {
  if (!cfg_.compileThread) {
    doc::Snapshot s;
    {
      std::lock_guard lock(mutex_);
      if (pending_) {
        s = std::move(pendingSnap_);
        pending_ = false;
      }
    }
    if (s) {
      (void)compileNow(s);
    } else if (anyDeferred_.load(std::memory_order_acquire)) {
      std::lock_guard compileLock(compileMutex_);
      bool drained = true;
      (void)drainDeferred(drained);
    }
    return;
  }
  std::unique_lock lock(mutex_);
  flushNow_ = true;
  cv_.notify_all();
  cv_.wait(lock, [&] { return quit_ || (!pending_ && !busy_); });
  flushNow_ = false;
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
  auto waitFor = [&](auto&& ready) -> bool {
    double stoppedSince = -1.0;
    for (int i = 0; i < 5000; ++i) {
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
  anyDeferred_.store(!parked_.empty(), std::memory_order_release);
  drained = true;  // every compile result is in (parked JIT images are optional: the interpreted ones play)
  engine_->wake();
  return Ok{};
}

Status ProjectLink::compileNow(const doc::Snapshot& s) {
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
  auto lowered = doc::lower(*s, lo, lowered_ ? &*lowered_ : nullptr);
  auto fail = [&](const Error& e) {
    ++stats_.compileErrors;
    setError(e.message);
    std::fprintf(stderr, "pqdaw: %s\n", e.message.c_str());
    return Status(e);
  };
  if (!lowered.ok()) return fail(lowered.error());
  doc::LoweredProject& lp = lowered.value();
  std::vector<engine::ProjectParamLink> links;
  for (const doc::LoweredParam& p : lp.params) {
    const uint32_t slot = slotForParam(p.key);
    if (slot != kNone) links.push_back({p.track, p.name, slot});
  }
  compiler_->setParamLinks(std::move(links));
  // T4.4-fix1: an inspection change (the circuit editor's probes on a device) recompiles the units of those tracks.
  std::vector<uint32_t> inspectTracks;
  {
    std::lock_guard lock(mutex_);
    if (inspectChanged_) {
      std::vector<engine::TrackInspect> ins;
      if (inspect_) ins.push_back(*inspect_);
      compiler_->setInspect(std::move(ins));
      inspectTracks = std::move(inspectDirty_);
      inspectDirty_.clear();
      inspectChanged_ = false;
    }
  }
  // Dirty tracks: a patch that is not shared with the previous lowering (new text or a new track).
  std::vector<uint32_t> dirty;
  for (uint32_t i = 0; i < lp.patches.size(); ++i) {
    bool same = false;
    if (lowered_)
      for (uint32_t j = 0; j < lowered_->patches.size() && !same; ++j)
        same = lowered_->trackIds[j] == lp.trackIds[i] && lowered_->patches[j] == lp.patches[i];
    if (same && std::find(inspectTracks.begin(), inspectTracks.end(), lp.trackIds[i].value) != inspectTracks.end())
      same = false;
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
  lowered_ = std::move(lp);
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
  for (int attempt = 0; attempt < 5000; ++attempt) {
    {
      std::lock_guard lock(link.compileMutex_);
      const JitPublish r = link.publishJit(job.schedule, job.first, job.refine, img.value());
      if (r == JitPublish::Published || r == JitPublish::Failed) return;
      if (r == JitPublish::Stale || link.jitWorker_->stopping()) break;
      if (!link.deferred_.empty() || link.engine_->audioStopped() || attempt + 1 == 5000) {
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
