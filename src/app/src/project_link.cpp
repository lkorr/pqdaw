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
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#include "pq/document/audibility.h"
#include "pq/document/clip_time.h"
#include "pq/document/pitch_resolve.h"
#include "pq/engine/tuning_signal.h"
#include "pq/tuning/tuning.h"

#ifdef PQ_SOURCE_DIR
#define PQ_STDLIB_FALLBACK PQ_SOURCE_DIR
#endif

namespace pq::app {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kProjectParams = 16384;
constexpr uint32_t kEventSlots = 128;

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Error linkError(ErrorCode c, const std::string& m) { return Error{c, "playback: " + m, {}}; }

// ---- Sequence lowering (doc → engine plain data, T3.16's descriptions) -------------------------------------------

// Same table shape (T2.6-fix1): the discrete parameters a tuning node table depends on. Both have a signal.
bool sameTableShape(const doc::TuningDef& a, const doc::TuningDef& b) {
  if (!doc::hasTuningSignal(a) || !doc::hasTuningSignal(b)) return false;
  if (a.edo() && b.edo()) return a.edo()->n() == b.edo()->n();
  const tuning::MosParams* ma = a.mos();
  const tuning::MosParams* mb = b.mos();
  return ma && mb && ma->n == mb->n && ma->periodsPerEquave == mb->periodsPerEquave && ma->rotation == mb->rotation;
}

bool sameNote(const engine::NoteDesc& a, const engine::NoteDesc& b) {
  return a.start == b.start && a.length == b.length && a.id == b.id && a.hz == b.hz && a.velocity == b.velocity &&
         a.releaseVelocity == b.releaseVelocity && a.chance == b.chance && a.velocityRange == b.velocityRange &&
         a.ratio == b.ratio && a.harmonic == b.harmonic && a.pitch.tuning == b.pitch.tuning &&
         a.pitch.degree == b.pitch.degree && a.pitch.equave == b.pitch.equave && a.enabled == b.enabled;
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

// ---- Sequence lowering (doc -> engine plain data, T3.16's descriptions) -------------------------------------------

// Pitches from doc::resolveTrackPitches (T2.7: Ratio anchors within the clip, memoised). A Degree note of the main
// tuning whose signal the track's tuning node follows (doc::trackTuningSignal) carries its identity (PitchSource,
// T2.6) and its static Hz in the tuning TABLE's definition (see the file comment of project_link.h); every other
// note plays its static Hz. Harmonic / JI-fundamental identities need JI objects (M7): unresolved, not played.
engine::TrackSequenceDesc ProjectLink::lowerTrack(const doc::Project& p, const doc::TrackPath& path,
                                                  const doc::Track& track) {
  engine::TrackSequenceDesc d;
  d.id = track.id.value;
  auto pitches = doc::resolveTrackPitches(p, track.id);
  const doc::TuningId signal = doc::trackTuningSignal(p, path);
  const TuningTableEntry* entry = nullptr;
  if (signal.valid()) {
    std::lock_guard lock(mutex_);
    entry = tuningEntry(signal, doc::findTuning(p, signal)->def);  // entries live as long as the link
  }
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
        double hz = *np.hz;
        engine::NoteDesc nd;
        if (signal.valid() && np.signal == signal) {
          nd.pitch = engine::PitchSource{signal.value, np.degree, np.equave};
          if (entry != nullptr && entry->built && !(entry->def == *cp.tuning.def)) {
            const auto th = tuning::resolveHzDouble(n.pitch, *entry->built, {});
            if (th) hz = *th;
          }
        }
        if (!(hz > 0.0) || !std::isfinite(static_cast<float>(hz))) continue;
        nd.start = n.start;
        nd.length = std::max<Ticks>(n.length, 1);
        nd.id = n.id.value;
        nd.hz = static_cast<float>(hz);
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

// ---- Tuning tables (UI thread builds, the compile path reads) ----------------------------------------------------

const ProjectLink::TuningTableEntry* ProjectLink::tuningEntry(doc::TuningId id, const doc::TuningDef& def) const {
  for (const auto& e : tuningEntries_)
    if (e->id == id && sameTableShape(e->def, def)) return e.get();
  return nullptr;
}

uint32_t ProjectLink::tuningTable(doc::TuningId id, const doc::TuningDef& def) const {
  std::lock_guard lock(mutex_);
  for (uint32_t i = 0; i < tuningEntries_.size(); ++i)
    if (tuningEntries_[i]->id == id && sameTableShape(tuningEntries_[i]->def, def)) return i;
  return kTuningTables;
}

uint32_t ProjectLink::tuningTablesBuilt() const {
  std::lock_guard lock(mutex_);
  return static_cast<uint32_t>(tuningEntries_.size());
}

void ProjectLink::updateTuningTables(const doc::Project& p) {
  for (const doc::TuningSlotPtr& slot : *p.tunings) {
    if (!doc::hasTuningSignal(slot->def)) continue;
    {
      std::lock_guard lock(mutex_);
      if (tuningEntry(slot->id, slot->def) != nullptr || tuningEntries_.size() >= kTuningTables) continue;
    }
    auto t = doc::buildTuning(slot->def);
    if (!t.ok()) continue;
    auto table = engine::makeTuningTable(t.value());
    if (!table.ok()) continue;
    auto e = std::make_unique<TuningTableEntry>();
    e->id = slot->id;
    e->def = slot->def;
    e->built = std::move(t.value());
    e->table = std::move(table.value());
    std::lock_guard lock(mutex_);
    tuningViews_[tuningEntries_.size()] = &e->table.view();  // before any lowering can name this index
    tuningEntries_.push_back(std::move(e));
  }
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

Result<std::unique_ptr<InstrumentLibrary>> loadInstrumentLibrary(const std::string& stdlib) {
  const fs::path path = fs::path(stdlib) / "instruments" / "basic_synth.pqp";
  std::ifstream in(path, std::ios::binary);
  if (!in) return linkError(ErrorCode::NotFound, "cannot read the default instrument " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  auto lib = std::make_unique<InstrumentLibrary>();
  lib->text = ss.str();
  auto parsed = graph::parse(lib->text, path.string());
  if (!parsed.ok()) return parsed.error();
  lib->file = std::move(parsed.value());
  lib->library.add(lib->file);
  lib->signatures = graph::signatures(lib->file);
  return lib;
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
  auto lib = loadInstrumentLibrary(cfg_.stdlib.empty() ? stdlibDir() : cfg_.stdlib);
  if (!lib.ok()) return lib.error();
  instruments_ = std::move(lib.value());
  if (Status s = makeEngine(); !s.ok()) return s;
  written_.assign(kProjectParams, std::numeric_limits<float>::quiet_NaN());
  automated_.assign(kProjectParams, 0);
  current_ = initial;
  updateTuningTables(*current_);  // T2.6-fix1: before the first lowering names their indices
  applyParams(*current_, true);
  if (Status s = compileNow(current_); !s.ok()) return s;
  publishTempo(*current_);
  publishSequences(*current_);
  publishAutomation(*current_);
  if (cfg_.compileThread) worker_ = std::thread([this] { workerMain(); });
  return Ok{};
}

Status ProjectLink::makeEngine() {
  engine::EngineConfig ec;
  ec.sampleRate = cfg_.sampleRate;
  ec.blockFrames = cfg_.blockFrames;
  ec.numOut = 2;
  ec.numProjectParams = kProjectParams;
  ec.maxSlots = kEventSlots;
  ec.sleepAfterMs = cfg_.sleepAfterMs;
  ec.timeline.reportEveryFrames = cfg_.blockFrames;  // the playhead within one engine block
  ec.timeline.reportCapacity = 1024;               // > 1 s of reports between two UI polls
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
  compiler_ = std::make_unique<engine::ProjectCompiler>(co);
  return Ok{};
}

Status ProjectLink::setSampleRate(double rate) {
  if (!(rate >= 8000.0 && rate <= 768000.0)) return linkError(ErrorCode::InvalidArgument, "sample rate out of range");
  if (rate == cfg_.sampleRate) return Ok{};
  flush();  // no structural compile pending or running (and none can start: only this thread queues them)
  const views::PlayheadSnapshot ph = playhead();
  const double oldRate = cfg_.sampleRate;
  std::unique_ptr<engine::Engine> oldEngine = std::move(engine_);
  std::unique_ptr<engine::ProjectCompiler> oldCompiler = std::move(compiler_);
  cfg_.sampleRate = rate;
  if (Status s = makeEngine(); !s.ok()) {  // keep the old engine (still valid at its rate)
    cfg_.sampleRate = oldRate;
    engine_ = std::move(oldEngine);
    compiler_ = std::move(oldCompiler);
    return s;
  }
  {
    std::lock_guard lock(mutex_);
    meterBus_.clear();  // the new program's layout is published by compileNow
  }
  meters_.clear();
  written_.assign(kProjectParams, std::numeric_limits<float>::quiet_NaN());
  tempoTimeline_ = nullptr;
  report_ = engine::TransportReport{};
  commandsPosted_ = 0;
  applyParams(*current_, true);
  const Status compiled = compileNow(current_);
  publishTempo(*current_);
  publishSequences(*current_);
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
  if (worker_.joinable()) worker_.join();
}

void ProjectLink::attach(views::ViewContext& ctx) {
  detach();
  ctx_ = &ctx;
  sub_ = ctx.subscribe([this](const doc::StoreEvent& e) { onDocumentChange(e); });
  if (ctx.doc() != current_) {  // follow the context's document
    const doc::ChangeSet none;
    const doc::StoreEvent e{doc::StoreEventKind::Reset, current_, ctx.doc(), none};
    onDocumentChange(e);
  }
}

void ProjectLink::detach() {
  sub_ = views::ViewContext::Subscription();
  ctx_ = nullptr;
}

// ---- Slots ------------------------------------------------------------------------------------------------------

uint32_t ProjectLink::slotForParam(const std::string& key) {
  std::lock_guard lock(mutex_);
  if (auto it = paramSlots_.find(key); it != paramSlots_.end()) return it->second;
  uint32_t s = kNone;
  if (nextParam_ < kProjectParams) {
    s = nextParam_++;
  } else if (!freeParams_.empty()) {
    s = freeParams_.front();
    freeParams_.erase(freeParams_.begin());
  }
  if (s != kNone) paramSlots_.emplace(key, s);
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

void ProjectLink::applyParams(const doc::Project& p, bool forceAll) {
  for (const doc::LoweredParam& lp : doc::lowerParams(p, true)) {
    const uint32_t s = slotForParam(lp.key);
    if (s == kNone || automated_[s]) continue;  // automation owns an automated param
    if (!forceAll && written_[s] == lp.value) continue;
    if (engine_->setParam(s, lp.value)) {
      written_[s] = lp.value;
      ++stats_.paramWrites;
    } else {
      written_[s] = std::numeric_limits<float>::quiet_NaN();  // ring full: retried at the next change
    }
  }
}

void ProjectLink::publishSequences(const doc::Project& p, bool onlyIfChanged) {
  std::vector<engine::TrackSequenceDesc> descs;
  std::unordered_map<uint32_t, SeqCacheEntry> next;
  bool changed = false;
  forEachMidi(p, [&](const doc::TrackPath& path, const doc::Track& t) {
    const uint32_t slot = slotForTrack(t.id);
    if (slot == kNone) return;
    const doc::TuningDef* def = doc::effectiveTuning(p, path);
    SeqCacheEntry e;
    auto it = seqCache_.find(t.id.value);
    if (it != seqCache_.end() && it->second.track == &t && it->second.tuning == def &&
        it->second.tunings == p.tunings.get()) {
      e = std::move(it->second);
    } else {
      e.track = &t;
      e.tuning = def;
      e.tunings = p.tunings.get();
      e.from = current_;
      e.desc = lowerTrack(p, path, t);
      changed |= it == seqCache_.end() || !sameDesc(it->second.desc, e.desc);
    }
    if (descs.size() <= slot) descs.resize(slot + 1);
    descs[slot] = e.desc;
    next.emplace(t.id.value, std::move(e));
  });
  changed |= next.size() != seqCache_.size();
  seqCache_ = std::move(next);
  if (onlyIfChanged && !changed) return;  // e.g. a tuning param that only the tuning nodes follow
  auto img = engine::SequenceImage::build(descs);
  if (!img.ok()) {
    stats_.lastError = img.error().message;
    return;
  }
  if (engine_->timeline().publishSequence(std::move(img.value())).ok()) ++stats_.sequencePublishes;
  engine_->wake();
}

void ProjectLink::publishAutomation(const doc::Project& p) {
  std::vector<engine::AutomationLaneDesc> lanes;
  std::vector<uint8_t> automated(kProjectParams, 0);
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::LanePtr& lane : t.lanes) {
      if (lane->points.empty()) continue;
      const std::string key = doc::paramKey(lane->param);
      if (key.empty()) continue;  // not lowered (mute/solo are gates, tuning params are T2.6)
      const uint32_t s = slotForParam(key);
      if (s == kNone) continue;
      engine::AutomationLaneDesc d;
      d.target = s;
      for (const doc::Breakpoint& b : lane->points) d.points.push_back({b.tick, b.value, b.curve});
      lanes.push_back(std::move(d));
      automated[s] = 1;
    }
  });
  bool released = false;  // a param that lost its lane gets its document value again
  for (uint32_t s = 0; s < kProjectParams; ++s) released |= automated_[s] && !automated[s];
  automated_ = std::move(automated);
  auto img = engine::AutomationImage::build(lanes);
  if (img.ok() && engine_->timeline().publishAutomation(std::move(img.value())).ok()) ++stats_.automationPublishes;
  if (released) applyParams(p, true);
  engine_->wake();
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
    for (auto it = paramSlots_.begin(); it != paramSlots_.end();) {
      const bool tuningKey = it->first.starts_with("tuning:");
      const uint32_t id = static_cast<uint32_t>(std::strtoul(it->first.c_str() + (tuningKey ? 7 : 0), nullptr, 10));
      if (tuningKey ? !std::binary_search(tunings.begin(), tunings.end(), id) : !live(id)) {
        freeParams_.push_back(it->second);
        written_[it->second] = std::numeric_limits<float>::quiet_NaN();
        it = paramSlots_.erase(it);
      } else {
        ++it;
      }
    }
  }
  if (reset || structural) updateTuningTables(p);  // before the sequences resolve in the tables' definitions
  if (reset || structural || c.has(doc::kChangeParam)) applyParams(p, reset);
  if (reset || c.has(doc::kChangeTimeline)) publishTempo(p);
  if (reset || structural || c.has(doc::kChangeSequence)) publishSequences(p);
  else if (c.has(doc::kChangeParam) && !c.tunings.empty()) publishSequences(p, true);  // static pitches only
  if (reset || structural || c.has(doc::kChangeAutomation)) publishAutomation(p);
  if (structural) {
    {
      std::lock_guard lock(mutex_);
      pending_ = true;
      pendingSnap_ = current_;
      pendingSince_ = nowSeconds();
    }
    cv_.notify_all();
  }
}

// ---- Compiling --------------------------------------------------------------------------------------------------

void ProjectLink::workerMain() {
  std::unique_lock lock(mutex_);
  for (;;) {
    cv_.wait(lock, [&] { return quit_ || pending_; });
    if (quit_) return;
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
    cv_.notify_all();
  }
}

void ProjectLink::flush() {
  if (!cfg_.compileThread) {
    doc::Snapshot s;
    {
      std::lock_guard lock(mutex_);
      if (!pending_) return;
      s = std::move(pendingSnap_);
      pending_ = false;
    }
    (void)compileNow(s);
    return;
  }
  std::unique_lock lock(mutex_);
  flushNow_ = true;
  cv_.notify_all();
  cv_.wait(lock, [&] { return quit_ || (!pending_ && !busy_); });
  flushNow_ = false;
}

Status ProjectLink::compileNow(const doc::Snapshot& s) {
  doc::LowerOptions lo;
  lo.library = &instruments_->library;
  lo.externals = instruments_->signatures;
  lo.slotOf = [this](doc::TrackId id) { return slotForTrack(id); };
  lo.tuningTableOf = [this](doc::TuningId id, const doc::TuningDef& def) { return tuningTable(id, def); };
  auto lowered = doc::lower(*s, lo, lowered_ ? &*lowered_ : nullptr);
  auto fail = [&](const Error& e) {
    ++stats_.compileErrors;
    stats_.lastError = e.message;
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
  auto metered = [](engine::ProjectLayout& layout) {
    layout.busMetered.assign(layout.busKey.size(), 0);
    for (size_t b = 0; b < layout.busKey.size(); ++b) layout.busMetered[b] = layout.busKey[b].ends_with(":out") ? 1 : 0;
  };
  // Publishing waits (bounded) until the audio thread has taken earlier images / topologies: a refused publish
  // would destroy the image.
  auto waitFor = [&](auto&& ready) -> Status {
    for (int i = 0; i < 5000; ++i) {
      if (ready()) return Ok{};
      engine_->wake();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return linkError(ErrorCode::Overflow, "the audio thread did not take the previous program within 5 s");
  };
  engine::ProjectProgram* program = engine_->program();
  uint32_t compiledUnits = 0;
  if (program == nullptr) {  // the first program (init): before any audio runs
    auto b = compiler_->build(lp.graph);
    if (!b.ok()) return fail(b.error());
    metered(b.value().layout);
    auto prog = engine::ProjectProgram::create(engine_->projectConfig(), b.value().layout);
    if (!prog.ok()) return fail(prog.error());
    for (uint32_t u = 0; u < b.value().images.size(); ++u)
      if (auto pub = prog.value()->publish(u, std::move(b.value().images[u])); !pub.ok()) return fail(pub.error());
    compiledUnits = static_cast<uint32_t>(b.value().images.size());
    {
      std::lock_guard lock(mutex_);
      meterBus_ = meterMap(b.value().layout);
    }
    engine_->setProgram(std::move(prog.value()));
  } else {
    auto upd = compiler_->update(lp.graph, dirty);
    if (!upd.ok() && upd.error().code == ErrorCode::Unsupported) {  // a topology change
      auto tb = compiler_->rebuild(lp.graph, dirty);
      if (!tb.ok()) return fail(tb.error());
      metered(tb.value().layout);
      compiledUnits = tb.value().compiled;
      if (Status st = waitFor([&] { return !program->topologyPending(); }); !st.ok()) return fail(st.error());
      // The meter map and the published layout change together (read() indexes the published layout).
      std::lock_guard lock(mutex_);
      auto info = program->publishTopology(tb.value().layout, std::move(tb.value().units));
      if (!info.ok()) return fail(info.error());
      meterBus_ = meterMap(tb.value().layout);
      ++stats_.topologyChanges;
    } else if (!upd.ok()) {
      return fail(upd.error());
    } else {
      for (engine::UnitUpdate& u : upd.value()) {
        if (u.unchanged) continue;
        ++compiledUnits;
        if (Status st = waitFor([&] { return program->canPublish(u.unit); }); !st.ok()) return fail(st.error());
        if (auto r = program->publish(u.unit, std::move(u.image)); !r.ok()) return fail(r.error());
        ++stats_.unitPublishes;
      }
    }
  }
  if (compiledUnits > 0) ++stats_.compiles;
  stats_.unitCompiles += compiledUnits;
  lowered_ = std::move(lp);
  engine_->wake();
  return Ok{};
}

// ---- Transport (UI thread) --------------------------------------------------------------------------------------

void ProjectLink::postTransport(const engine::TransportCommand& c) {
  engine::TransportReport r;
  while (engine_->timeline().poll(r)) report_ = r;  // older reports describe the state before this command
  if (engine_->post(c)) ++commandsPosted_;
  ++reportSeq_;
}

void ProjectLink::play(Ticks from) {
  postTransport({engine::TransportCommandKind::Play, false, from, 0, 0});
  optimistic_ = {true, from, 0};
}

void ProjectLink::stop() {
  const views::PlayheadSnapshot now = playhead();
  postTransport({engine::TransportCommandKind::Stop, false, 0, 0, 0});
  optimistic_ = {false, now.tick, 0};
}

void ProjectLink::continuePlayback() {
  const views::PlayheadSnapshot now = playhead();
  postTransport({engine::TransportCommandKind::Continue, false, 0, 0, 0});
  optimistic_ = {true, now.tick, 0};
}

void ProjectLink::locate(Ticks tick) {
  const views::PlayheadSnapshot now = playhead();
  postTransport({engine::TransportCommandKind::Locate, false, tick, 0, 0});
  optimistic_ = {now.playing, tick, 0};
}

void ProjectLink::setLoop(const views::TransportLoop& loop) {
  const views::PlayheadSnapshot now = playhead();
  postTransport({engine::TransportCommandKind::SetLoop, loop.enabled, 0, loop.start, loop.end});
  loop_ = loop;
  loopSent_ = true;
  optimistic_ = now;
}

views::PlayheadSnapshot ProjectLink::playhead() {
  engine::TransportReport r;
  bool fresh = false;
  while (engine_->timeline().poll(r)) {
    report_ = r;
    fresh = true;
  }
  if (fresh) ++reportSeq_;
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

bool ProjectLink::live() const noexcept { return !engine_->asleep() || report_.playing; }

void ProjectLink::read(std::span<const doc::TrackId> ids, std::span<ui::MeterLevels> out) noexcept {
  const size_t n = std::min(ids.size(), out.size());
  engine::ProjectProgram* program = engine_->program();
  std::vector<uint32_t> buses(n, kNone);
  std::vector<engine::BusLevel> levels(n);
  {
    std::lock_guard lock(mutex_);  // the map and the program's published layout change together (compileNow)
    for (size_t i = 0; i < n; ++i)
      if (auto it = meterBus_.find(ids[i].value); it != meterBus_.end()) buses[i] = it->second;
    if (program != nullptr) program->readMeters(buses, levels);
  }
  const double now = nowSeconds();
  const double dt = std::clamp(now - lastMeterRead_, 0.0, 1.0);
  lastMeterRead_ = now;
  constexpr float kFallDbPerSecond = 26.0f;  // PQ Meter-like release of the peak bar
  constexpr double kHoldSeconds = 1.5;
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
      if (l.channels > c && l.peak[c] >= 1.0f) st.clip[c] = true;
      if (now >= st.holdUntil[c]) st.clip[c] = false;
      m.peakDb[c] = st.peakDb[c] < -199.0f ? -200.0f : st.peakDb[c];
      m.rmsDb[c] = rms;
      m.holdDb[c] = st.holdDb[c];
      m.clip[c] = st.clip[c];
    }
    out[i] = m;
  }
}

// ---- Offline ------------------------------------------------------------------------------------------------

Result<engine::RenderStats> ProjectLink::renderOffline(Ticks fromTick, uint64_t frames, engine::AudioSink& sink) {
  flush();
  engine::TransportCommand play{engine::TransportCommandKind::Play, false, fromTick, 0, 0};
  if (!engine_->post(play)) return linkError(ErrorCode::Overflow, "transport ring full");
  ++commandsPosted_;
  auto r = engine_->renderOffline(frames, sink, cfg_.blockFrames);
  engine::TransportCommand stopCmd{engine::TransportCommandKind::Stop, false, 0, 0, 0};
  if (engine_->post(stopCmd)) ++commandsPosted_;
  return r;
}

}  // namespace pq::app
