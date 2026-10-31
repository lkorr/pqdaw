#include "pq/views/view_context.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <thread>

#include "pq/document/serialize.h"
#include "pq/views/browser/library.h"
#include "pq/views/file_probes.h"
#include "pq/views/waveforms.h"

namespace pq::views {

namespace {

bool isSep(char c) noexcept { return c == '/' || c == '\\'; }

bool iequalsSuffix(std::string_view s, std::string_view suffix) noexcept {
  if (s.size() < suffix.size()) return false;
  const std::string_view tail = s.substr(s.size() - suffix.size());
  for (size_t i = 0; i < suffix.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(tail[i])) != std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
  return true;
}

std::string_view trimSeparators(std::string_view p) noexcept {
  // Keep a lone root ("/", "C:\") intact.
  while (p.size() > 1 && isSep(p.back()) && !(p.size() == 3 && p[1] == ':')) p.remove_suffix(1);
  return p;
}

TransportLoop loopOf(const doc::Project& p) noexcept { return {p.loop.start, p.loop.end, p.loop.enabled}; }

}  // namespace

std::string projectDirFromPickedPath(std::string_view picked) {
  std::string_view p = trimSeparators(picked);
  if (p.empty()) return {};
  size_t slash = p.size();
  while (slash > 0 && !isSep(p[slash - 1])) --slash;
  const std::string_view leaf = p.substr(slash);
  if (iequalsSuffix(leaf, doc::kProjectFileName) && leaf.size() == doc::kProjectFileName.size())
    return std::string(trimSeparators(p.substr(0, slash)));  // ".../X/project.pqd" → ".../X"
  if (iequalsSuffix(leaf, ".pqd") && leaf.size() > 4) return std::string(p.substr(0, p.size() - 4));  // "Song.pqd" → "Song"
  return std::string(p);
}

std::string projectNameOf(std::string_view dir) {
  const std::string_view p = trimSeparators(dir);
  if (p.empty()) return "Untitled";
  size_t slash = p.size();
  while (slash > 0 && !isSep(p[slash - 1])) --slash;
  const std::string_view leaf = p.substr(slash);
  return leaf.empty() ? std::string(p) : std::string(leaf);
}

// --- construction ---------------------------------------------------------------------------------------

ViewContext::ViewContext(TransportPort& transport) : transport_(transport) {
  waveforms_ = std::make_unique<Waveforms>(*this);
  probes_ = std::make_unique<FileProbes>(*this);
  saved_ = store_.current();
  storeListener_ = store_.subscribe([this](const doc::StoreEvent& e) { onStoreEvent(e); });
  // Scopes of the default keymap, interned now so views can set them before the keymap file is applied.
  for (std::string_view s : {"arrangement", "pianoroll"}) (void)scope(s);
  registerGlobalActions();
  declareViewActions(kPianoRollActions);
  declareViewActions(kMosWheelActions);
  declareViewActions(kMixerActions);
  declareViewActions(kPrefsActions);
  declareViewActions(kTuningsActions);
  declareViewActions(kCircuitActions);
  declareViewActions(kDeviceActions);
  declareViewActions(kDeviceChainActions);
  declareViewActions(kWiringActions);
  declareViewActions(kPatchActions);
  declareViewActions(kBrowserActions);
  syncLoop();
  library_ = std::make_unique<Library>(*this);  // last: it observes the context
}

// T5.8: a Collect All and Save in progress (the copy on its worker thread).
struct ViewContext::CollectJob {
  doc::CollectPlan plan;
  std::string dir;
  doc::CollectOptions options;
  Status result = Ok{};
  std::thread thread;
  ~CollectJob() {
    if (thread.joinable()) thread.join();
  }
};

ViewContext::~ViewContext() {
  library_.reset();        // T5.10: unsubscribes from this context first
  collectWorker_.reset();  // joins a running copy
  waveforms_.reset();      // joins its workers
  probes_.reset();         // T5.7: joins the probe workers
  store_.unsubscribe(storeListener_);
}

ViewContext::Subscription ViewContext::subscribe(doc::Listener listener) {
  const doc::ListenerId id = store_.subscribe(std::move(listener));
  return Subscription([this, id] { store_.unsubscribe(id); });
}

ViewContext::Subscription ViewContext::observe(std::function<void(uint32_t)> fn) {
  const uint32_t id = nextObserver_++;
  observers_.emplace_back(id, std::move(fn));
  return Subscription([this, id] {
    for (auto& o : observers_)
      if (o.first == id) o.second = nullptr;  // compacted in notify(); safe to call from inside a callback
  });
}

void ViewContext::notify(uint32_t bits) {
  // Index loop: an observer may subscribe others (appended) or unsubscribe (nulled) while we iterate, or notify
  // again (nested); entries are only removed by the outermost call.
  ++notifyDepth_;
  for (size_t i = 0; i < observers_.size(); ++i)
    if (observers_[i].second) {
      auto fn = observers_[i].second;  // keep the callable alive if it unsubscribes itself
      fn(bits);
    }
  if (--notifyDepth_ == 0) std::erase_if(observers_, [](const auto& o) { return !o.second; });
}

namespace {

// Whether any lane automates `r` (a track parameter's lane lives on its own track).
bool automated(const doc::Project& p, const doc::ParamRef& r) {
  if (!doc::isAutomatable(r.kind)) return false;  // T4.1: device params (a knob drag walks no lanes)
  if (doc::isTrackParam(r.kind)) {
    const doc::Track* t = doc::findTrackPtr(p, doc::TrackId{r.owner});
    return t && std::any_of(t->lanes.begin(), t->lanes.end(), [&](const doc::LanePtr& l) { return l->param == r; });
  }
  bool found = false;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::LanePtr& l : t.lanes) found = found || l->param == r;
  });
  return found;
}

}  // namespace

void ViewContext::setDeviceLibrary(const doc::DeviceLibrary* lib) {
  if (devices_ == lib) return;
  devices_ = lib;
  notify(view_change::kDevices);
}

void ViewContext::setModulationSource(ModulationSource* source) {
  if (modulation_ == source) return;
  modulation_ = source;
  notify(view_change::kModulation);
}

void ViewContext::onStoreEvent(const doc::StoreEvent& e) {
  if (e.kind == doc::StoreEventKind::Reset || !(e.before->loop == e.after->loop)) syncLoop();
  // T5.8: the sample table changed (a drop, undo / redo of one): resolve it again (cheap: a file check per entry).
  if (e.kind != doc::StoreEventKind::SamplesRelocated && e.kind != doc::StoreEventKind::Reset &&
      e.after->samples != resolvedFor_)
    refreshSamples(false);  // inside a notification: resolve only (a relocation would notify again)
  // T3.15: a hand-made change of an automated parameter overrides its automation (Live); undo / redo do not.
  bool overridesChanged = false;
  if (e.kind == doc::StoreEventKind::Reset) {
    overridesChanged = !overrides_.empty();
    overrides_.clear();
  } else {
    if (e.kind == doc::StoreEventKind::Dispatch && e.changes.has(doc::kChangeParam))
      for (const doc::ParamRef& r : e.changes.params) {
        const auto it = std::lower_bound(overrides_.begin(), overrides_.end(), r);
        if ((it == overrides_.end() || !(*it == r)) && automated(*e.after, r)) {
          overrides_.insert(it, r);
          overridesChanged = true;
        }
      }
    if (!overrides_.empty() && e.changes.has(doc::kChangeStructural)) {
      const size_t n = overrides_.size();
      std::erase_if(overrides_, [&](const doc::ParamRef& r) { return !automated(*e.after, r); });
      overridesChanged = overridesChanged || overrides_.size() != n;
    }
  }
  if (overridesChanged) notify(view_change::kAutomation);
  const bool d = dirty();
  if (d != lastDirty_) {
    lastDirty_ = d;
    notify(view_change::kProject);
  }
}

void ViewContext::syncLoop() {
  const TransportLoop l = loopOf(*store_.current());
  if (loopSentValid_ && l == loopSent_) return;
  loopSent_ = l;
  loopSentValid_ = true;
  transport_.setLoop(l);
}

// --- project I/O ----------------------------------------------------------------------------------------

void ViewContext::viewStateChanged() {
  viewStateDirty_ = true;
  if (!lastDirty_) {
    lastDirty_ = true;
    notify(view_change::kProject);
  }
}

void ViewContext::afterLoad() {
  saved_ = store_.current();
  viewStateDirty_ = false;
  clearTuningPreview();
  if (detailClip_.clip.valid() || detailClip_.track.valid()) {
    detailClip_ = {};
    notify(view_change::kDetailClip);
  }
  lastDirty_ = false;
  if (transport_.playhead().playing) transport_.stop();
  startMarker_ = 0;
  transport_.locate(0);
  syncLoop();
  waveforms_->clear();  // T5.5: another project (its own cache folder)
  refreshSamples();
  notify(view_change::kProject | view_change::kTransport);
}

void ViewContext::newProject() {
  store_.reset(doc::makeDefaultProject(store_.ids()));
  store_.view() = {};
  projectDir_.clear();
  afterLoad();
  setStatus("New project");
}

Status ViewContext::openProject(std::string_view dirUtf8) {
  if (Status s = doc::openProject(dirUtf8, store_); !s) {
    setStatus("Could not open " + std::string(dirUtf8) + ": " + s.error().message);
    return s;
  }
  projectDir_ = std::string(dirUtf8);
  afterLoad();
  if (missing_.empty()) setStatus("Opened " + projectName());  // else the status line says which samples are missing
  return Ok{};
}

Status ViewContext::saveProject() {
  if (projectDir_.empty())
    return Error{ErrorCode::InvalidArgument, "the project has no location yet (Save As)", {}};
  return saveProjectAs(std::string(projectDir_));
}

Status ViewContext::saveProjectAs(std::string_view dirUtf8) {
  if (dirUtf8.empty()) return Error{ErrorCode::InvalidArgument, "save: empty project directory", {}};
  if (projectDir_ != dirUtf8 && !projectDir_.empty()) {
    // T5.8: a project-relative sample stays where it is: saved elsewhere, the project references it by its absolute path
    // (Collect All and Save copies it into the new project).
    std::vector<doc::SampleRef> abs;
    for (const doc::SampleRef& r : doc::samplesOf(*store_.current()))
      if (!doc::isAbsoluteSamplePath(r.path)) {
        doc::SampleRef n = r;
        n.path = doc::absoluteSamplePath(r.path, projectDir_);
        abs.push_back(std::move(n));
      }
    store_.relocateSamples(abs);
  }
  if (Status s = doc::saveProject(dirUtf8, store_); !s) {
    setStatus("Could not save " + std::string(dirUtf8) + ": " + s.error().message);
    return s;
  }
  const bool renamed = projectDir_ != dirUtf8;
  projectDir_ = std::string(dirUtf8);
  saved_ = store_.current();
  viewStateDirty_ = false;
  const bool wasDirty = lastDirty_;
  lastDirty_ = false;
  if (renamed || wasDirty) notify(view_change::kProject);
  if (renamed) refreshSamples();
  setStatus("Saved " + projectName());
  return Ok{};
}

// --- project samples (T5.8) -------------------------------------------------------------------------------

void ViewContext::setSampleSearchFolders(std::vector<std::string> folders) {
  sampleFolders_ = std::move(folders);
  refreshSamples();
}

void ViewContext::refreshSamples(bool relocate) {
  const doc::Project& p = *store_.current();
  sampleRes_ = doc::resolveSamples(p, doc::SampleSearch{projectDir_, sampleFolders_});
  std::vector<doc::SampleRef> moved;
  for (const doc::SampleResolution& r : sampleRes_)
    if (r.relocated)
      if (const doc::SampleRef* old = doc::findSample(p, r.id)) {
        doc::SampleRef n = *old;
        n.path = *r.relocated;
        moved.push_back(std::move(n));
      }
  if (relocate && !moved.empty()) {
    const bool clean = store_.current() == saved_;
    store_.relocateSamples(moved);  // not an undo step; a project that was clean stays clean
    if (clean) saved_ = store_.current();
  }
  resolvedFor_ = store_.current()->samples;
  resolvedDir_ = projectDir_;
  const size_t before = missing_.size();
  missing_ = doc::missingSamples(*store_.current(), sampleRes_);
  if (!missing_.empty())
    setStatus(std::to_string(missing_.size()) + (missing_.size() == 1 ? " sample missing" : " samples missing") +
              " \xE2\x80\x94 Relink\xE2\x80\xA6");
  else if (before != 0)
    setStatus("All samples found");
  waveforms_->samplesResolved();  // T5.5: overviews of files that changed are dropped
  notify(view_change::kSamples);  // T5.4: the playback link follows which samples can play
}

bool ViewContext::sampleAvailable(doc::SampleId id) const noexcept {
  for (const doc::SampleResolution& r : sampleRes_)
    if (r.id == id) return !r.path.empty();
  return false;
}

std::string ViewContext::samplePath(doc::SampleId id) const {
  for (const doc::SampleResolution& r : sampleRes_)
    if (r.id == id) return r.path;
  return {};
}

uint32_t ViewContext::relinkMissingSamples(std::string_view folder) {
  if (folder.empty()) return 0;
  const size_t wanted = missing_.size();
  const std::vector<doc::SampleRef> found = doc::relinkSamples(*store_.current(), missing_, folder, projectDir_);
  if (std::find(sampleFolders_.begin(), sampleFolders_.end(), folder) == sampleFolders_.end())
    sampleFolders_.emplace_back(folder);
  store_.relocateSamples(found);  // not an undo step
  refreshSamples();
  setStatus("Relinked " + std::to_string(found.size()) + " of " + std::to_string(wanted) +
            (wanted == 1 ? " missing sample" : " missing samples") +
            (missing_.empty() ? std::string() : " (" + std::to_string(missing_.size()) + " still missing)"));
  return static_cast<uint32_t>(found.size());
}

Status ViewContext::collectAllAndSave(const doc::CollectOptions& options) {
  if (collectWorker_) return Error{ErrorCode::InvalidArgument, "Collect All and Save is already running", {}};
  if (projectDir_.empty()) return Error{ErrorCode::InvalidArgument, "the project has no location yet (Save As)", {}};
  Result<doc::CollectPlan> plan = doc::planCollect(*store_.current(), projectDir_, sampleRes_);
  if (!plan) {
    setStatus("Collect All and Save: " + plan.error().message);
    return std::move(plan).error();
  }
  auto job = std::make_unique<CollectJob>();
  job->plan = std::move(*plan);
  job->dir = projectDir_;
  job->options = options;
  if (!post_ || job->plan.bytes == 0) {  // nothing to copy, or no UI thread to come back to: here
    job->result = doc::runCollect(job->plan, job->options);
    Status st = job->result;
    collectWorker_ = std::move(job);
    finishCollect();
    return st;
  }
  // The copy on a worker; progress to the status line at most every 50 ms; the end back on the UI thread.
  CollectJob* raw = job.get();
  const std::weak_ptr<char> life = life_;
  const UiPoster post = post_;
  const doc::CollectOptions user = options;
  raw->options.progress = [life, post, this, user, last = std::chrono::steady_clock::time_point{}](
                              uint64_t done, uint64_t total) mutable {
    if (user.progress) user.progress(done, total);
    const auto now = std::chrono::steady_clock::now();
    if (done < total && now - last < std::chrono::milliseconds(50)) return;
    last = now;
    post([life, this, done, total] {
      if (life.expired() || !collectWorker_) return;
      const int pct = total ? static_cast<int>(100.0 * static_cast<double>(done) / static_cast<double>(total)) : 100;
      setStatus("Collecting samples\xE2\x80\xA6 " + std::to_string(pct) + " %");
    });
  };
  collectWorker_ = std::move(job);
  raw->thread = std::thread([raw, life, post, this] {
    raw->result = doc::runCollect(raw->plan, raw->options);
    post([life, this] {
      if (!life.expired()) finishCollect();
    });
  });
  setStatus("Collecting samples\xE2\x80\xA6");
  return Ok{};
}

void ViewContext::waitForCollect() {
  if (!collectWorker_) return;
  if (collectWorker_->thread.joinable()) collectWorker_->thread.join();
  finishCollect();
}

void ViewContext::finishCollect() {
  if (!collectWorker_) return;
  if (collectWorker_->thread.joinable()) collectWorker_->thread.join();
  std::unique_ptr<CollectJob> job = std::move(collectWorker_);
  if (!job->result) {
    setStatus("Collect All and Save failed: " + job->result.error().message);
    return;
  }
  store_.relocateSamples(doc::collectedRefs(*store_.current(), job->plan));
  if (Status s = saveProjectAs(job->dir); !s) return;  // the status line has the error
  size_t copied = 0;
  for (const doc::CollectItem& it : job->plan.items) copied += it.copy ? 1 : 0;
  refreshSamples();
  setStatus("Collected " + std::to_string(copied) + (copied == 1 ? " sample" : " samples") + " and saved " +
            projectName());
}

// --- dialogs --------------------------------------------------------------------------------------------

void ViewContext::guardUnsaved(std::function<void()> proceed) {
  if (!dirty() || !dialogs_) {
    proceed();
    return;
  }
  switch (dialogs_->askSaveChanges(projectName())) {
    case SaveChoice::Cancel: return;
    case SaveChoice::Discard: proceed(); return;
    case SaveChoice::Save: requestSave(std::move(proceed)); return;
  }
}

void ViewContext::requestNew() {
  guardUnsaved([this] { newProject(); });
}

void ViewContext::requestOpen() {
  if (!dialogs_) {
    setStatus("Open: no file dialog available");
    return;
  }
  guardUnsaved([this] {
    dialogs_->chooseProjectToOpen([this](std::string picked) {
      if (picked.empty()) return;
      (void)openProject(projectDirFromPickedPath(picked));
    });
  });
}

void ViewContext::requestSave(std::function<void()> then) {
  if (projectDir_.empty()) {
    requestSaveAs(std::move(then));
    return;
  }
  if (saveProject().ok() && then) then();
}

void ViewContext::requestSaveAs(std::function<void()> then) {
  if (!dialogs_) {
    setStatus("Save As: no file dialog available");
    return;
  }
  dialogs_->chooseProjectToSave(projectName(), [this, then = std::move(then)](std::string picked) {
    if (picked.empty()) return;
    if (saveProjectAs(projectDirFromPickedPath(picked)).ok() && then) then();
  });
}

bool ViewContext::confirmQuit(std::function<void()> quitAfterSave) {
  if (!dirty() || !dialogs_) return true;
  switch (dialogs_->askSaveChanges(projectName())) {
    case SaveChoice::Cancel: return false;
    case SaveChoice::Discard: return true;
    case SaveChoice::Save:
      if (!projectDir_.empty()) return saveProject().ok();
      requestSaveAs(std::move(quitAfterSave));
      return false;
  }
  return false;
}

// --- undo -----------------------------------------------------------------------------------------------

bool ViewContext::undo() {
  const std::string label(store_.history().undoLabel());
  if (!store_.undo()) return false;
  setStatus("Undo " + label);
  return true;
}

bool ViewContext::redo() {
  const std::string label(store_.history().redoLabel());
  if (!store_.redo()) return false;
  setStatus("Redo " + label);
  return true;
}

// --- transport ------------------------------------------------------------------------------------------

void ViewContext::togglePlay() {
  if (transport_.playhead().playing)
    transport_.stop();
  else
    transport_.play(startMarker_);
  notify(view_change::kTransport);
}

void ViewContext::continuePlayback() {
  if (transport_.playhead().playing) return;
  transport_.continuePlayback();
  notify(view_change::kTransport);
}

void ViewContext::stopButton() {
  if (transport_.playhead().playing) {
    transport_.stop();
  } else {
    startMarker_ = 0;
    transport_.locate(0);
  }
  notify(view_change::kTransport);
}

void ViewContext::setStartMarker(Ticks t) {
  if (t == startMarker_) return;
  startMarker_ = t;
  notify(view_change::kTransport);
}

// --- grid -----------------------------------------------------------------------------------------------

void ViewContext::setGrid(const GridSpec& g) {
  if (g == grid_) return;
  grid_ = g;
  notify(view_change::kGrid);
}

void ViewContext::setResolvedGrid(std::optional<GridDivision> d) {
  if (d == resolvedGrid_) return;
  resolvedGrid_ = d;
  notify(view_change::kGrid);
}

std::string ViewContext::gridLabel() const {
  std::string s;
  char buf[32];
  if (grid_.off) {
    s = "Off";
  } else if (resolvedGrid_) {
    s.assign(buf, formatGridDivision(*resolvedGrid_, buf));
  } else if (grid_.mode == GridMode::Fixed) {
    GridDivision d;
    if (grid_.fixedExp >= 0)
      d.denominator = 1 << grid_.fixedExp;
    else
      d.bars = 1 << -grid_.fixedExp;
    d.tuplet = grid_.tuplet;
    s.assign(buf, formatGridDivision(d, buf));
  } else {
    s = "Adaptive (";
    s += adaptiveLevelName(grid_.level);
    s += ')';
    if (grid_.tuplet != 1) {
      std::snprintf(buf, sizeof buf, " %u-tuplet", static_cast<unsigned>(grid_.tuplet));
      s += buf;
    }
  }
  if (!grid_.off && !grid_.snap) s += " \xC2\xB7 Snap off";
  return s;
}

// --- tools and the clip view -----------------------------------------------------------------------------

void ViewContext::setDrawMode(bool on) {
  if (on == drawMode_) return;
  drawMode_ = on;
  setStatus(on ? "Draw Mode on" : "Draw Mode off");
  notify(view_change::kTool);
}

void ViewContext::setDetailClip(const doc::ClipRef& clip) {
  if (clip.track == detailClip_.track && clip.clip == detailClip_.clip) return;
  detailClip_ = clip;
  if (detailAuto_ && clip.clip.valid() && !detailVisible_) {
    detailAuto_ = false;
    detailVisible_ = true;
    notify(view_change::kDetailClip | view_change::kLayout);
    return;
  }
  notify(view_change::kDetailClip);
}

void ViewContext::setSelectedTrack(doc::TrackId t) {
  if (t == selectedTrack_) return;
  selectedTrack_ = t;
  notify(view_change::kSelection);
}

// --- tuning preview (T3.14) ------------------------------------------------------------------------------

void ViewContext::setTuningPreview(const TuningTarget& target, doc::TuningDef def) {
  preview_ = TuningPreview{target, std::move(def)};
  notify(view_change::kTuningPreview);
}

void ViewContext::clearTuningPreview() {
  if (!preview_) return;
  preview_.reset();
  notify(view_change::kTuningPreview);
}

const doc::TuningDef* ViewContext::previewedTuning(const doc::Project& p, const doc::TrackPath& path, const doc::NoteClip* clip,
                                                   doc::ClipId clipId) const noexcept {
  if (!preview_) return doc::effectiveTuning(p, path, clip);
  // The same walk as doc::effectiveTuning, answering with the preview where the walk reaches the target.
  const TuningTarget& t = preview_->target;
  const doc::TuningDef* pv = &preview_->def;
  auto resolve = [&](const doc::TuningAssignment& a) -> const doc::TuningDef* {
    if (a.mode == doc::TuningMode::Main) {
      if (t.kind == TuningTarget::Kind::Main && a.main == t.main) return pv;
      const doc::TuningSlot* slot = doc::findTuning(p, a.main);
      return slot ? &slot->def : nullptr;
    }
    return a.privateDef.get();
  };
  if (clip) {
    if (t.kind == TuningTarget::Kind::Clip && clipId == t.clip.clip) return pv;
    if (clip->tuning.mode != doc::TuningMode::Inherit) return resolve(clip->tuning);
  }
  doc::TrackPath at = path;
  while (at.depth > 0) {
    const doc::Track& tr = doc::trackAt(p, at);
    if (t.kind == TuningTarget::Kind::Track && tr.id == t.track) return pv;
    if (tr.tuning.mode != doc::TuningMode::Inherit) return resolve(tr.tuning);
    if (at.list != doc::TrackList::Main) break;
    at = at.parent();
  }
  if (p.tunings->empty()) return nullptr;
  const doc::TuningSlot& first = *p.tunings->front();
  return (t.kind == TuningTarget::Kind::Main && first.id == t.main) ? pv : &first.def;
}

// --- layout ---------------------------------------------------------------------------------------------

void ViewContext::setDetailVisible(bool v) {
  detailAuto_ = false;
  if (v == detailVisible_) return;
  detailVisible_ = v;
  notify(view_change::kLayout);
}

void ViewContext::setDetailMode(DetailMode m) {
  detailAuto_ = false;
  if (m == detailMode_ && detailVisible_) return;
  detailMode_ = m;
  detailVisible_ = true;
  notify(view_change::kLayout);
}

void ViewContext::toggleClipDevice() {
  detailAuto_ = false;
  if (!detailVisible_) {
    detailVisible_ = true;
    notify(view_change::kLayout);
    return;
  }
  setDetailMode(detailMode_ == DetailMode::Clip ? DetailMode::Device : DetailMode::Clip);
}

void ViewContext::setBrowserVisible(bool v) {
  if (v == browserVisible_) return;
  browserVisible_ = v;
  notify(view_change::kLayout);
}

void ViewContext::setTuningsVisible(bool v) {
  if (v == tuningsVisible_) return;
  tuningsVisible_ = v;
  notify(view_change::kLayout);
}

void ViewContext::setStatus(std::string text) {
  if (text == status_) return;
  status_ = std::move(text);
  notify(view_change::kStatus);
}

// --- automation override (T3.15) ---------------------------------------------------------------------------

bool ViewContext::automationOverridden(const doc::ParamRef& p) const noexcept {
  return std::binary_search(overrides_.begin(), overrides_.end(), p);
}

void ViewContext::overrideAutomation(const doc::ParamRef& p) {
  const auto it = std::lower_bound(overrides_.begin(), overrides_.end(), p);
  if (it != overrides_.end() && *it == p) return;
  overrides_.insert(it, p);
  notify(view_change::kAutomation);
}

void ViewContext::reenableAutomation() {
  if (overrides_.empty()) return;
  overrides_.clear();
  notify(view_change::kAutomation);
}

// --- actions --------------------------------------------------------------------------------------------

void ViewContext::declareViewActions(std::span<const std::string_view> ids) {
  viewActions_.reserve(viewActions_.size() + ids.size());
  for (std::string_view id : ids) {
    const size_t i = viewActions_.size();
    viewActions_.push_back({std::string(id), {}, {}});
    // Steps repeat while the key is held (Live nudges notes on auto-repeat).
    ui::ActionOptions opt;
    opt.repeatable = id.ends_with("Up") || id.ends_with("Down");
    [[maybe_unused]] auto r = actions_.add(
        std::string(id),
        [this, i] {
          if (viewActions_[i].handler) viewActions_[i].handler();
        },
        [this, i] { return viewActions_[i].handler && (!viewActions_[i].enabled || viewActions_[i].enabled()); }, opt);
    assert(r.ok());
  }
}

void ViewContext::setViewAction(std::string_view id, std::function<void()> handler, std::function<bool()> enabled) {
  for (ViewAction& a : viewActions_)
    if (a.id == id) {
      a.handler = std::move(handler);
      a.enabled = std::move(enabled);
      return;
    }
  assert(false && "setViewAction: unknown view action id");
}

void ViewContext::clearViewAction(std::string_view id) {
  for (ViewAction& a : viewActions_)
    if (a.id == id) {
      a.handler = nullptr;
      a.enabled = nullptr;
    }
}

ui::ScopeId ViewContext::scope(std::string_view name) {
  if (std::find(scopes_.begin(), scopes_.end(), name) == scopes_.end()) scopes_.emplace_back(name);
  return keymap_.scope(name);
}

Status ViewContext::applyKeymap(const ui::KeymapFile& file) {
  // Fresh bindings, same scope ids: re-intern the scopes in their original order.
  ui::Keymap fresh;
  for (const std::string& s : scopes_) (void)fresh.scope(s);
  if (Status st = ui::applyKeymapFile(file, actions_, fresh); !st) return st;
  for (const ui::KeymapEntry& e : file.entries)
    if (std::find(scopes_.begin(), scopes_.end(), e.scope) == scopes_.end()) scopes_.push_back(e.scope);
  keymap_ = std::move(fresh);
  return Ok{};
}

Status ViewContext::applyDefaultKeymap() {
  auto file = ui::loadKeymapFile(ui::defaultKeymapPath());
  if (!file) return std::move(file).error();
  return applyKeymap(*file);
}

void ViewContext::registerGlobalActions() {
  auto add = [this](std::string id, ui::ActionRegistry::Handler h, ui::ActionRegistry::Enabled en = {}) {
    [[maybe_unused]] auto r = actions_.add(std::move(id), std::move(h), std::move(en));
    assert(r.ok());
  };
  add("transport.playStop", [this] { togglePlay(); });
  add("transport.continue", [this] { continuePlayback(); });
  // Disabled while there is nothing to undo or a gesture is running: the chord then falls through (Q27), so a
  // text field's own Ctrl+Z handling (none yet) would get it.
  add("edit.undo", [this] { (void)undo(); }, [this] { return store_.history().canUndo() && !store_.inGesture(); });
  add("edit.redo", [this] { (void)redo(); }, [this] { return store_.history().canRedo() && !store_.inGesture(); });
  add("file.new", [this] { requestNew(); });
  add("file.open", [this] { requestOpen(); }, [this] { return dialogs_ != nullptr; });
  add("file.save", [this] { requestSave(); }, [this] { return dialogs_ != nullptr || !projectDir_.empty(); });
  add("file.saveAs", [this] { requestSaveAs(); }, [this] { return dialogs_ != nullptr; });
  add("view.toggleClipDevice", [this] { toggleClipDevice(); });
  add("view.toggleDetail", [this] { setDetailVisible(!detailVisible_); });
  add("edit.toggleDrawMode", [this] { setDrawMode(!drawMode_); });
  // Ctrl+1–5 edit the one grid both editors share (T3.3 GridSpec commands; Q33).
  add("grid.narrow", [this] { setGrid(grid_.narrowed()); });
  add("grid.widen", [this] { setGrid(grid_.widened()); });
  add("grid.toggleTriplet", [this] { setGrid(grid_.tripletToggled()); });
  add("grid.toggleSnap", [this] { setGrid(grid_.snapToggled()); });
  add("grid.toggleFixed", [this] { setGrid(grid_.modeToggled()); });
  add("view.toggleTunings", [this] { setTuningsVisible(!tuningsVisible_); });
  add("view.toggleBrowser", [this] { setBrowserVisible(!browserVisible_); });  // T5.10
  // T5.8: File > Collect All and Save (an untitled project is saved first) and Relink Missing Samples.
  add("file.collectAllAndSave",
      [this] {
        if (projectDir_.empty()) requestSaveAs([this] { (void)collectAllAndSave(); });
        else (void)collectAllAndSave();
      },
      [this] { return !collecting() && (dialogs_ != nullptr || !projectDir_.empty()); });
  add("file.relinkSamples",
      [this] {
        if (dialogs_) dialogs_->chooseFolder([this](std::string folder) { (void)relinkMissingSamples(folder); });
      },
      [this] { return dialogs_ != nullptr && !missing_.empty(); });
}

}  // namespace pq::views
