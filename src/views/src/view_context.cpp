#include "pq/views/view_context.h"

#include <algorithm>
#include <filesystem>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <thread>

#include "pq/document/pitch_resolve.h"
#include "pq/document/serialize.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/browser/library.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/file_probes.h"
#include "pq/views/panel/group_responses.h"
#include "pq/views/sample_analysis.h"
#include "pq/views/sample_drop.h"
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
  sampleAnalysis_ = std::make_unique<SampleAnalysis>(*this);  // T6.8
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
  sampleAnalysis_.reset();  // T6.8: joins its worker
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
  if (!doc::isAutomatable(r)) return false;
  if (doc::isTrackParam(r.kind) || doc::isDeviceParamKind(r.kind)) {  // T4.18: a device's lane on its track
    const doc::Track* t = doc::isTrackParam(r.kind) ? doc::findTrackPtr(p, doc::TrackId{r.owner}) : doc::laneHome(p, r);
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
  store_.setDeviceLibrary(lib);  // GU.1: edits in units resolve curves with it
  if (devices_ == lib) return;
  devices_ = lib;
  ++structural_;  // EQ.4
  structuralSerial_ = docSerial_;  // EQ.4b: (the current document starts the new revision: a lowering of it with this library is current)
  notify(view_change::kDevices);
}

void ViewContext::setModulationSource(ModulationSource* source) {
  if (modulation_ == source) return;
  modulation_ = source;
  notify(view_change::kModulation);
}

void ViewContext::setAnalysisSource(std::function<std::shared_ptr<TapRing>(doc::DeviceId)> tapOf, double sampleRate) {
  // A new source (another rate): the hub keeps the feeds displays still hold until they let go (their targets ask for
  // the new ones at their next frame).
  if (sampleRate > 0.0) sampleRate_ = sampleRate;
  if (analysis_) analysis_->setSource(std::move(tapOf), sampleRate);
  else  // the poster is looked up when a wake is posted (the app may set it later)
    analysis_ = std::make_unique<devices::AnalysisHub>(std::move(tapOf), sampleRate, [this](std::function<void()> f) {
      if (post_) post_(std::move(f));
    });
  if (signalTapOf_) analysis_->setPathSource(signalTapOf_);  // GU.7
}

devices::SpectrumFeed* ViewContext::analysisFeed(doc::DeviceId device) {
  return analysis_ ? analysis_->feed(device) : nullptr;
}

void ViewContext::setSignalTapSource(std::function<std::shared_ptr<TapRing>(doc::DeviceId, std::string_view)> tapOf) {
  signalTapOf_ = std::move(tapOf);
  if (analysis_) analysis_->setPathSource(signalTapOf_);
}

devices::SpectrumFeed* ViewContext::analysisFeed(doc::DeviceId device, std::string_view path) {
  if (!analysis_) return nullptr;
  return analysis_->feed(device, path);
}

GroupResponses& ViewContext::groupResponses() {
  if (!responses_) responses_ = std::make_unique<GroupResponses>(*this);
  return *responses_;
}

void ViewContext::setLowering(uint64_t serial, doc::Snapshot from, std::shared_ptr<const doc::LoweredProject> lowered) {
  if (!lowered || serial < loweringSerial_) return;  // (an older compile's, arriving late)
  loweringSerial_ = serial;
  loweringFrom_ = std::move(from);
  lowering_ = std::move(lowered);
  if (serial >= structuralSerial_) notify(view_change::kResponses);
}

std::shared_ptr<const doc::LoweredProject> ViewContext::currentLowering(doc::Snapshot* from) const {
  if (!lowering_ || loweringSerial_ < structuralSerial_) return nullptr;
  if (from != nullptr) *from = loweringFrom_;
  return lowering_;
}

void ViewContext::onStoreEvent(const doc::StoreEvent& e) {
  if (e.before != e.after || e.kind == doc::StoreEventKind::Reset) ++docSerial_;  // EQ.4b
  if (e.kind == doc::StoreEventKind::Reset || e.changes.structural()) {
    ++structural_;  // EQ.4
    structuralSerial_ = docSerial_;
  }
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
    autoTakes_.clear();  // AR.1
  } else {
    // AR.1: while recording with Automation Arm on, a Param change is a touch (recordTouch); a gesture's end releases.
    const bool paramChange = e.kind == doc::StoreEventKind::Dispatch && e.changes.has(doc::kChangeParam);
    const std::optional<Ticks> rec = paramChange ? recordTick(*e.after) : std::nullopt;
    if (e.kind == doc::StoreEventKind::GestureCommit || e.kind == doc::StoreEventKind::GestureCancel)
      overridesChanged = releaseTouches(e.kind == doc::StoreEventKind::GestureCancel);
    if (paramChange)
      for (const doc::ParamRef& r : e.changes.params) {
        if (rec && recordTouch(e, r, *rec)) {
          overridesChanged = true;
          continue;
        }
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
  syncAutoArm();  // KB.1: a track armed or disarmed by hand, the selected track deleted or another kind
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
  sampleAnalysis_->clear();  // T6.8
  refreshSamples();
  notify(view_change::kProject | view_change::kTransport);
}

void ViewContext::newProject() {
  const bool wasUntitled = projectDir_.empty();
  const std::string oldUnsaved = unsavedRecordingDir_;
  store_.reset(doc::makeDefaultProject(store_.ids()));
  store_.view() = {};
  projectDir_.clear();
  afterLoad();
  setStatus("New project");
  if (wasUntitled) discardUnsaved(oldUnsaved, true);  // T6.7b: the untitled project's recordings go with it
  unsavedRecordingDir_ = unsaved_.fresh ? unsaved_.fresh() : std::string();  // C1: its own folder
}

Status ViewContext::openProject(std::string_view dirUtf8) {
  doc::LoadOptions lo;
  lo.devices = devices_;  // GU.1: an older file's values become knob positions through the library's curves
  if (Status s = doc::openProject(dirUtf8, store_, lo); !s) {
    setStatus("Could not open " + std::string(dirUtf8) + ": " + s.error().message);
    return s;
  }
  const bool wasUntitled = projectDir_.empty();
  projectDir_ = std::string(dirUtf8);
  afterLoad();
  if (wasUntitled) discardUnsaved(unsavedRecordingDir_, true);  // T6.7b
  if (missing_.empty()) setStatus("Opened " + projectName());  // else the status line says which samples are missing
  return Ok{};
}

Status ViewContext::saveProject() {
  if (projectDir_.empty())
    return Error{ErrorCode::InvalidArgument, "the project has no location yet (Save As)", {}};
  return saveProjectAs(std::string(projectDir_));
}

// ---- untitled projects' recordings (T6.7, T6.7b; M6 review C1 / H1) ----------------------------------------------

namespace {

std::filesystem::path u8Path(std::string_view s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string u8Str(const std::filesystem::path& p) {
  const std::u8string u = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// A comparable form of a path: absolute, normal, backslashes, lower case (Windows paths ignore case), no trailing slash.
std::wstring pathKey(std::string_view p) {
  std::error_code ec;
  std::wstring w = std::filesystem::absolute(u8Path(p), ec).lexically_normal().wstring();
  std::replace(w.begin(), w.end(), L'/', L'\\');
  for (wchar_t& c : w) c = static_cast<wchar_t>(std::towlower(c));
  while (w.size() > 3 && w.back() == L'\\') w.pop_back();
  return w;
}

// `p` lies inside `root` (lexically, ignoring case), or is `root` itself when `orEqual`.
bool pathInside(std::string_view p, std::string_view root, bool orEqual) {
  const std::wstring a = pathKey(p), r = pathKey(root);
  if (a == r) return orEqual;
  return a.size() > r.size() && a.compare(0, r.size(), r) == 0 && a[r.size()] == L'\\';
}

}  // namespace

void ViewContext::setUnsavedRecordings(UnsavedRecordings u) {
  unsaved_ = std::move(u);
  unsavedRecordingDir_ = projectDir_.empty() ? unsaved_.current : std::string();
}

bool ViewContext::unsavedFolderInUse(std::string_view dirUtf8, bool includeStore) const {
  if (!projectDir_.empty() && pathInside(projectDir_, dirUtf8, true)) return true;  // H1
  for (const std::string& p : savedUnsavedRefs_)
    if (pathInside(p, dirUtf8, false)) return true;
  if (!includeStore) return false;
  auto refs = [&](const doc::Snapshot& s) {
    if (!s) return false;
    for (const doc::SampleRef& r : doc::samplesOf(*s))
      if (doc::isAbsoluteSamplePath(r.path) && pathInside(r.path, dirUtf8, false)) return true;
    return false;
  };
  if (refs(store_.current()) || refs(saved_)) return true;
  for (const doc::History::Entry& e : store_.history().entries())
    if (refs(e.before) || refs(e.after)) return true;
  return false;
}

void ViewContext::discardUnsaved(const std::string& dir, bool includeStore) {
  if (dir.empty() || !unsaved_.discard) return;
  if (unsavedFolderInUse(dir, includeStore)) {  // the last line of defence: a reference survives, the folder stays
    setStatus("Kept the unsaved recordings folder " + dir + ": a project still uses a file in it");
    return;
  }
  unsaved_.discard(dir);
}

// ---- M6 review L13: recordings of earlier runs that ended without cleaning up ------------------------------------------

std::vector<ViewContext::StaleRecordings> ViewContext::staleRecordings() const {
  return unsaved_.findStale ? unsaved_.findStale() : std::vector<StaleRecordings>{};
}

std::vector<std::pair<std::string, std::string>> ViewContext::moveStaleTakes(const std::vector<std::string>& files,
                                                                             std::string_view toDir) {
  namespace fs = std::filesystem;
  std::vector<std::pair<std::string, std::string>> moved;
  std::error_code ec;
  const fs::path to = u8Path(toDir);
  fs::create_directories(to, ec);
  if (ec) return moved;
  // Moved (a rename on the same volume), never copied and deleted: the files are the user's only copy.
  for (const std::string& f : files) {
    const fs::path from = u8Path(f);
    fs::path dest = to / from.filename();
    for (int k = 2; fs::exists(dest, ec) && k < 1000; ++k)
      dest = to / fs::path(from.stem().u8string() + u8" " + fs::path(std::to_string(k)).u8string() + from.extension().u8string());
    std::error_code mec;
    fs::rename(from, dest, mec);
    if (!mec) moved.emplace_back(f, u8Str(dest));
  }
  return moved;
}

size_t ViewContext::placeRecoveredTakes(const std::vector<std::string>& files) {
  if (files.empty()) return 0;
  (void)probes_->request(files);
  (void)probes_->wait(files, std::chrono::milliseconds(5000));
  std::vector<ProbedFile> audio;
  for (ProbedFile& f : probes_->snapshot(files))
    if (f.state == ProbedFile::State::Ready) audio.push_back(std::move(f));
  if (audio.empty()) return 0;
  SampleDropTarget target;
  target.kind = SampleDropTarget::Kind::AtEnd;
  const SampleDropPlan plan = planSampleDrop(*doc(), target, audio, 0, true);
  auto e = sampleDropEdit(*doc(), store_.ids(), plan, audio, {}, projectDir_);
  if (!e.ok()) return 0;
  // One undo step: the drop, then the track named "Recovered".
  doc::Batch b{"Recover Recordings", {}};
  b.commands.push_back(e->command);
  doc::ChangeSet changes;
  if (!e->tracks.empty())
    if (Result<doc::Snapshot> after = doc::apply(doc(), e->command, changes); after.ok())
      if (auto r = doc::ops::renameTrack(**after, e->tracks.front(), "Recovered"); r.ok()) b.commands.push_back(r->command);
  return store_.dispatch(doc::Command{std::move(b)}).ok() ? audio.size() : 0;
}

void ViewContext::dropStaleSession(const StaleRecordings& s) {
  namespace fs = std::filesystem;
  if (!unsaved_.discardStale) return;
  // Anything of the user's left outside the autosave folder (a take that could not be moved) keeps the session.
  const std::wstring autosaveKey = s.autosave ? pathKey(s.autosave->dir) : std::wstring();
  bool left = false;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(u8Path(s.dir), ec), end; !ec && it != end && !left; it.increment(ec)) {
    if (!autosaveKey.empty() && it->is_directory(ec) && pathKey(u8Str(it->path())) == autosaveKey) {
      it.disable_recursion_pending();
      continue;
    }
    left = it->is_regular_file(ec);
  }
  if (!left) (void)unsaved_.discardStale(s.dir);
  else if (s.autosave) (void)unsaved_.discardStale(s.autosave->dir);  // offered later for its takes only
}

bool ViewContext::recoverStaleRecordings(const StaleRecordings& s) {
  if (!projectDir_.empty() || unsavedRecordingDir_.empty()) {
    setStatus("Recover recordings: only into a new, untitled project");
    return false;
  }
  std::vector<std::string> moved;
  for (auto& [from, to] : moveStaleTakes(s.files, unsavedRecordingDir_)) moved.push_back(std::move(to));
  if (moved.empty()) {
    setStatus("Recover recordings: no take could be moved out of " + s.dir);
    return false;
  }
  const size_t n = placeRecoveredTakes(moved);
  // The folder without its takes: gone when nothing else is left in it (nothing of the user's is deleted).
  StaleRecordings takesOnly = s;
  takesOnly.autosave.reset();
  dropStaleSession(takesOnly);
  setStatus(n > 0 ? "Recovered " + std::to_string(n) + " recording(s) from an earlier session"
                  : "Recover recordings: the takes were moved to " + unsavedRecordingDir_ + " but could not be read");
  return n > 0;
}

bool ViewContext::recoverAutosave(const StaleRecordings& s) {
  if (!s.autosave) return false;
  const StaleAutosave& a = *s.autosave;
  doc::LoadOptions lo;
  lo.devices = devices_ != nullptr ? devices_ : store_.deviceLibrary();
  Result<doc::LoadedProject> loaded = doc::loadProject(a.dir, lo);
  if (!loaded) {
    setStatus("Recover: could not read the autosave of " + a.name + ": " + loaded.error().message);
    return false;
  }
  const bool wasUntitled = projectDir_.empty();
  const std::string oldUnsaved = unsavedRecordingDir_;
  store_.reset(loaded->project);  // D6: the history is not kept
  store_.view() = std::move(loaded->view);
  projectDir_ = a.originDir;
  if (wasUntitled) discardUnsaved(oldUnsaved, true);  // T6.7b: the document it replaces
  std::vector<std::string> unreferenced;
  if (projectDir_.empty()) {
    // Untitled: its recordings (absolute paths into the dead session) move into the new document's own folder.
    unsavedRecordingDir_ = unsaved_.fresh ? unsaved_.fresh() : std::string();
    if (!unsavedRecordingDir_.empty() && !s.files.empty()) {
      const auto moved = moveStaleTakes(s.files, unsavedRecordingDir_);
      const doc::SampleList& refs = doc::samplesOf(*store_.current());  // alive until the relocation below
      std::vector<doc::SampleRef> updated;
      for (const auto& [from, to] : moved) {
        const std::wstring key = pathKey(from);
        bool referenced = false;
        for (const doc::SampleRef& r : refs)
          if (doc::isAbsoluteSamplePath(r.path) && pathKey(r.path) == key) {
            doc::SampleRef n = r;
            n.path = doc::storedSamplePath(to, {});
            updated.push_back(std::move(n));
            referenced = true;
          }
        if (!referenced) unreferenced.push_back(to);
      }
      if (!updated.empty()) store_.relocateSamples(updated);  // not an undo step
    }
  }
  afterLoad();
  saved_ = nullptr;  // D6: a recovered document is dirty (nothing on disk holds it)
  lastDirty_ = true;
  notify(view_change::kProject);
  const size_t placed = placeRecoveredTakes(unreferenced);  // takes recorded after the last autosave
  dropStaleSession(s);
  char when[16] = "?";
  const std::time_t t = static_cast<std::time_t>(a.savedAt);
  if (std::tm tm{}; localtime_s(&tm, &t) == 0) std::strftime(when, sizeof when, "%H:%M", &tm);
  setStatus("Recovered " + a.name + " (autosaved " + when + ")" +
            (placed > 0 ? ", " + std::to_string(placed) + " more recording(s) on the track Recovered" : std::string()));
  return true;
}

bool ViewContext::discardStaleRecordings(const StaleRecordings& s) {
  if (!unsaved_.discardStale) return false;
  if (unsavedFolderInUse(s.dir, true)) {  // C1's last line of defence: a project of this run uses a file in it
    setStatus("Kept " + s.dir + ": a project uses a file in it");
    return false;
  }
  const bool done = unsaved_.discardStale(s.dir);
  setStatus(done ? "Discarded the recordings of an earlier session" : "Could not delete " + s.dir);
  return done;
}

// T6.7 / C1: an untitled project's recordings into the project it is first saved as: every file of its folder, and
// every snapshot (current, undo, redo) that references one is remapped.
void ViewContext::moveUnsavedRecordings(std::string_view dirUtf8) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path from = u8Path(unsavedRecordingDir_);
  if (!fs::is_directory(from, ec)) return;
  // The samples any snapshot references inside the folder (one entry per id).
  std::vector<doc::SampleRef> refs;
  auto collect = [&](const doc::Snapshot& s) {
    if (!s) return;
    for (const doc::SampleRef& r : doc::samplesOf(*s))
      if (doc::isAbsoluteSamplePath(r.path) && pathInside(r.path, unsavedRecordingDir_, false) &&
          std::none_of(refs.begin(), refs.end(), [&](const doc::SampleRef& x) { return x.id == r.id; }))
        refs.push_back(r);
  };
  collect(store_.current());
  for (const doc::History::Entry& e : store_.history().entries()) {
    collect(e.before);
    collect(e.after);
  }
  const fs::path toDir = u8Path(dirUtf8) / "samples" / "recorded";
  fs::create_directories(toDir, ec);
  std::vector<std::pair<std::wstring, std::string>> moved;  // old path's key → the stored path in the project
  std::vector<fs::path> files;
  for (const fs::directory_entry& e : fs::directory_iterator(from, ec))
    if (e.is_regular_file(ec)) files.push_back(e.path());
  for (const fs::path& src : files) {
    const std::wstring key = pathKey(u8Str(src));
    const bool referenced = std::any_of(refs.begin(), refs.end(), [&](const doc::SampleRef& r) { return pathKey(r.path) == key; });
    fs::path to = toDir / src.filename();
    for (int n = 2; fs::exists(to, ec); ++n)
      to = toDir / u8Path(u8Str(src.stem()) + " (" + std::to_string(n) + ")" + u8Str(src.extension()));
    ec.clear();
    fs::rename(src, to, ec);
    if (ec) {
      // In use or another volume. A file still being recorded is open (the move is refused) and nothing references it
      // yet: it stays, and its take is moved when it is committed (ProjectLink::finishRecording). A referenced one is
      // copied; when even that fails the project keeps pointing into the folder, which a save then pins.
      if (!referenced) continue;
      ec.clear();
      fs::copy_file(from / src.filename(), to, ec);
      if (ec) continue;
    }
    moved.emplace_back(key, doc::storedSamplePath(u8Str(to), dirUtf8));
  }
  std::vector<doc::SampleRef> updated;
  for (const doc::SampleRef& r : refs) {
    const std::wstring key = pathKey(r.path);
    for (const auto& [k, stored] : moved)
      if (k == key) {
        doc::SampleRef n = r;
        n.path = stored;
        updated.push_back(std::move(n));
        break;
      }
  }
  if (!updated.empty()) store_.relocateSamples(updated);
  fs::remove(from, ec);  // gone when empty (a file left in it keeps it)
}

Status ViewContext::saveProjectAs(std::string_view dirUtf8) {
  if (dirUtf8.empty()) return Error{ErrorCode::InvalidArgument, "save: empty project directory", {}};
  if (!unsaved_.root.empty() && pathInside(dirUtf8, unsaved_.root, true)) {  // M6 review H1
    setStatus("Could not save " + std::string(dirUtf8) + ": that folder holds unsaved recordings and may be deleted");
    return Error{ErrorCode::InvalidArgument, "save: a project cannot be saved inside the unsaved recordings folder", {}};
  }
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
  if (projectDir_.empty() && !unsavedRecordingDir_.empty()) moveUnsavedRecordings(dirUtf8);  // T6.7
  if (Status s = doc::saveProject(dirUtf8, store_); !s) {
    setStatus("Could not save " + std::string(dirUtf8) + ": " + s.error().message);
    return s;
  }
  if (!unsaved_.root.empty())  // C1: a saved project still pointing into an unsaved folder pins it
    for (const doc::SampleRef& r : doc::samplesOf(*store_.current()))
      if (doc::isAbsoluteSamplePath(r.path) && pathInside(r.path, unsaved_.root, false)) savedUnsavedRefs_.push_back(r.path);
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
  sampleAnalysis_->samplesResolved();  // T6.8
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
  auto leave = [this] {  // T6.7b: an untitled project left behind takes its recordings with it
    if (projectDir_.empty()) discardUnsaved(unsavedRecordingDir_, false);  // the store still holds that document
    return true;
  };
  if (!dirty() || !dialogs_) return leave();
  switch (dialogs_->askSaveChanges(projectName())) {
    case SaveChoice::Cancel: return false;
    case SaveChoice::Discard: return leave();
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

void ViewContext::play() {
  transport_.play(startMarker_);
  notify(view_change::kTransport);
}

void ViewContext::continuePlayback() {
  if (transport_.playhead().playing) return;
  transport_.continuePlayback();
  notify(view_change::kTransport);
}

void ViewContext::record() {  // T6.6; T6.7: while playing it records from the playhead, while recording it ends
  if (transport_.playhead().countingIn) transport_.stop();
  else transport_.record(startMarker_);  // stopped: count-in, play, record; playing: record here; recording: off
  notify(view_change::kTransport);
}

void ViewContext::captureMidi() {  // T6.9
  if (!transport_.captureMidi(startMarker_)) setStatus("Capture MIDI: nothing was played on an armed or monitored track");
  else setStatus("Captured MIDI");
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

void ViewContext::setTimeWindow(double start, double span) {
  if (!std::isfinite(start) || !std::isfinite(span)) return;
  TimeWindow w{std::clamp(start, 0.0, static_cast<double>(doc::kMaxTicks / 2)), std::clamp(span, kMinTimeSpan, kMaxTimeSpan)};
  if (w == window_) return;
  window_ = w;
  notify(view_change::kTimeWindow);
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

bool ViewContext::openProjectCircuit(doc::TrackId track, doc::DeviceId device) {
  if (!track.valid() && device.valid()) {
    const doc::DeviceLoc loc = doc::findDevice(*doc(), device);
    if (loc.track != nullptr) track = loc.track->id;
  }
  if (!track.valid() || doc::findTrackPtr(*doc(), track) == nullptr) {
    setStatus(device.valid() ? "No such device" : "No track to show in the circuit view (select one first)");
    return false;
  }
  if (!circuitOpener_) {
    setStatus("The project circuit view is not available here");
    return false;
  }
  return circuitOpener_(track, device);
}

void ViewContext::setSelectedTrack(doc::TrackId t) {
  if (t == selectedTrack_) return;
  selectedTrack_ = t;
  notify(view_change::kSelection);
  syncAutoArm();
}

// --- KB.1: the computer keyboard's track, layout and Live's arm on select ---------------------------------

void ViewContext::syncAutoArm() {
  const doc::Project& p = *store_.current();
  doc::TrackId want;
  if (const doc::Track* sel = selectedTrack_.valid() ? doc::findTrackPtr(p, selectedTrack_) : nullptr;
      sel && sel->kind == doc::TrackKind::Midi) {
    bool anyArmed = false;
    doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) { anyArmed = anyArmed || t.mixer.arm; });
    if (!anyArmed) want = sel->id;
  }
  if (want == autoArm_) return;
  autoArm_ = want;
  transport_.setAutoArm(want.value);
  notify(view_change::kKeyboard);  // the arm buttons
}

doc::TrackId ViewContext::keyboardTrack() const {
  if (autoArm_.valid()) return autoArm_;
  const doc::Project& p = *store_.current();
  const doc::Track* sel = selectedTrack_.valid() ? doc::findTrackPtr(p, selectedTrack_) : nullptr;
  if (sel && sel->kind == doc::TrackKind::Midi && sel->mixer.arm) return sel->id;
  doc::TrackId first;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (!first.valid() && t.kind == doc::TrackKind::Midi && t.mixer.arm) first = t.id;
  });
  if (first.valid()) return first;
  return sel && sel->kind == doc::TrackKind::Midi ? sel->id : doc::TrackId{};
}

ComputerKeyboard::Layout ViewContext::keyboardLayout() {
  const doc::TrackId id = keyboardTrack();
  if (!id.valid()) return {};
  const doc::Project& p = *store_.current();
  const std::optional<doc::TrackPath> path = doc::findTrack(p, id);
  if (!path) return {};
  const doc::Track& t = doc::trackAt(p, *path);
  // A 12-TET key map counts semitones (ProjectLink's keyboard table): the piano layout whatever the tuning.
  if (t.noteInput.keyMap == doc::InputKeyMap::TwelveTetNearest || t.noteInput.keyMap == doc::InputKeyMap::TwelveTetHz)
    return {};
  const doc::TuningDef* def = doc::effectiveTuning(p, *path, liveClipOf(t).clip);  // KB.2: its live clip's tuning
  if (!def) return {};
  if (!keyboardDef_ || !(*keyboardDef_ == *def)) {  // built once per definition (a key press compares, no build)
    Result<tuning::Tuning> built = doc::buildTuning(*def);
    keyboardDef_ = *def;
    keyboardDegrees_ = built.ok() ? doc::degreesPerEquave(*built) : 0;
  }
  if (keyboardDegrees_ <= 0 || keyboardDegrees_ == 12) return {};
  return {false, static_cast<int32_t>(std::min<int64_t>(keyboardDegrees_, int64_t{1} << 20))};
}

doc::ClipRef ViewContext::liveEditedClip() const noexcept {
  return detailVisible_ && detailMode_ == DetailMode::Clip ? detailClip_ : doc::ClipRef{};
}

void ViewContext::audition(float hz, float velocity, float durationMs, doc::ClipRef clip, std::optional<int64_t> degree) {
  transport_.auditionHz(hz, velocity, durationMs);
  lastAudition_ = {lastAudition_.serial + 1, static_cast<double>(hz), durationMs, clip, degree, false};
  notify(view_change::kLiveNotes);
}

void ViewContext::releaseAuditions() {
  transport_.releaseAuditions();
  if (lastAudition_.serial == 0 || lastAudition_.released) return;
  lastAudition_.released = true;
  notify(view_change::kLiveNotes);
}

Ticks ViewContext::liveClipAt() {
  const PlayheadSnapshot ph = transport_.playhead();
  return ph.playing ? ph.tick : startMarker_;
}

doc::LiveClip ViewContext::liveClipOf(const doc::Track& t) {
  const doc::ClipRef e = liveEditedClip();
  return doc::liveClip(t, e.track == t.id ? e.clip : doc::ClipId{}, liveClipAt());
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

// ---- automation recording (AR.1) ----------------------------------------------------------------------------------

std::optional<Ticks> ViewContext::recordTick(const doc::Project& p) {
  if (!p.record.automationArm) return std::nullopt;
  const std::optional<Ticks> t = transport_.recordingTick();
  if (!t || (p.punch.enabled && (*t < p.punch.in || *t >= p.punch.out))) return std::nullopt;
  return t;
}

double ViewContext::recordTolerance(const doc::Project& p, const doc::ParamRef& r, const doc::DeviceLibrary* lib) noexcept {
  switch (r.kind) {
    case doc::ParamKind::TrackVolume:
    case doc::ParamKind::TrackSend: return 0.1;
    case doc::ParamKind::TrackPan: return 0.005;
    case doc::ParamKind::TuningReference: return 0.01;
    case doc::ParamKind::TuningEquave:
    case doc::ParamKind::MosGenerator:
    case doc::ParamKind::MosPeriod:
    case doc::ParamKind::MosStretch: return 0.05;
    case doc::ParamKind::DeviceParam:
    case doc::ParamKind::WireAmount: {
      const doc::ParamRange g = editRange(p, r, lib);
      return std::isfinite(g.max - g.min) ? 0.002 * (g.max - g.min) : 0.0;
    }
    default: return 0.0;
  }
}

// A Param change while recording: a sample of the parameter's touch (a new touch when none is held). False when `r`
// cannot have a lane (it is then an ordinary hand-made change).
bool ViewContext::recordTouch(const doc::StoreEvent& e, const doc::ParamRef& r, Ticks tick) {
  if (!paramValid(*e.after, r)) return false;
  const doc::DeviceLibrary* lib = deviceLibrary();
  auto it = std::find_if(autoTakes_.begin(), autoTakes_.end(), [&](const AutoTake& t) { return t.rec.param == r; });
  if (it == autoTakes_.end()) {
    AutoTake t;
    t.rec.param = r;
    t.rec.paramName = doc::deviceParamNameOf(*e.after, lib, r);
    t.rec.before = currentParamValue(*e.before, r, lib);
    t.rec.tolerance = recordTolerance(*e.after, r, lib);
    // A tuning parameter's new lane: on the selected track, else the master (any track may hold it).
    const doc::Track* home = selectedTrack_.valid() ? doc::findTrackPtr(*e.after, selectedTrack_) : nullptr;
    if (home == nullptr) home = e.after->master.get();
    if (home != nullptr) t.rec.home = home->id;
    autoTakes_.push_back(std::move(t));
    it = autoTakes_.end() - 1;
  }
  const bool held = store_.inGesture();
  if (!it->open) {
    doc::ops::RecordedTouch touch;
    touch.latch = !held;
    it->rec.touches.push_back(std::move(touch));
    it->open = held;
    // The hand's value plays while touched: the lane is overridden (a gesture's touch until its release, a latch until
    // the session ends).
    const auto o = std::lower_bound(overrides_.begin(), overrides_.end(), r);
    if ((o == overrides_.end() || !(*o == r)) && automated(*e.after, r)) {
      overrides_.insert(o, r);
      it->overrode = held;
    }
  }
  doc::ops::RecordedTouch& touch = it->rec.touches.back();
  const auto v = static_cast<float>(currentParamValue(*e.after, r, lib));
  touch.samples.push_back(doc::Breakpoint{tick, v, 0.0f, {}});
  touch.end = std::max(touch.end, tick);
  return true;
}

// A gesture ended: its touches are released (`cancel`, Esc: dropped) and the lanes they overrode play again. True when
// an override was lifted.
bool ViewContext::releaseTouches(bool cancel) {
  bool lifted = false;
  std::optional<Ticks> now;
  bool asked = false;
  for (AutoTake& t : autoTakes_) {
    if (!t.open) continue;
    t.open = false;
    if (cancel) {
      t.rec.touches.pop_back();
    } else {
      if (!asked) {
        now = recordTick(*store_.current());
        asked = true;
      }
      doc::ops::RecordedTouch& touch = t.rec.touches.back();
      if (now) touch.end = std::max(touch.end, *now);
    }
    if (t.overrode) {
      t.overrode = false;
      if (const auto o = std::lower_bound(overrides_.begin(), overrides_.end(), t.rec.param);
          o != overrides_.end() && *o == t.rec.param) {
        overrides_.erase(o);
        lifted = true;
      }
    }
  }
  std::erase_if(autoTakes_, [](const AutoTake& t) { return t.rec.touches.empty(); });
  return lifted;
}

std::vector<doc::ops::RecordedAutomation> ViewContext::takeRecordedAutomation(std::optional<Ticks> end) {
  std::vector<doc::ops::RecordedAutomation> out;
  bool lifted = false;
  for (AutoTake& t : autoTakes_) {
    if (t.open && end && !t.rec.touches.empty()) t.rec.touches.back().end = std::max(t.rec.touches.back().end, *end);
    if (const auto o = std::lower_bound(overrides_.begin(), overrides_.end(), t.rec.param);
        o != overrides_.end() && *o == t.rec.param) {
      overrides_.erase(o);
      lifted = true;
    }
    out.push_back(std::move(t.rec));
  }
  autoTakes_.clear();
  if (lifted) notify(view_change::kAutomation);
  return out;
}

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

void ViewContext::showAutomation(doc::TrackId track, const doc::ParamRef& p) {
  showAutomation_ = ShowAutomationRequest{track, p};
  notify(view_change::kShowAutomation);
}

std::optional<ViewContext::ShowAutomationRequest> ViewContext::takeShowAutomation() noexcept {
  std::optional<ShowAutomationRequest> r = showAutomation_;
  showAutomation_.reset();
  return r;
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
  add("transport.record", [this] { record(); });  // T6.6 / T6.7: Record (F9)
  add("transport.captureMidi", [this] { captureMidi(); });  // T6.9: Capture MIDI (Ctrl+Shift+C)
  add("transport.panic", [this] {  // M6 review L16: all notes off, bends reset (Ctrl+Shift+Space)
    transport_.panic();
    setStatus("Panic: all notes off");
  });
  // T6.4: the computer MIDI keyboard (Live's M).
  add("keyboard.toggleComputerMidi", [this] { computerKeyboard_.setActive(!computerKeyboard_.active()); });
  computerKeyboard_.layoutSource = [this] { return keyboardLayout(); };  // KB.1
  computerKeyboard_.onNotes = [this](const ComputerKeyboard&) { notify(view_change::kLiveNotes); };  // TU.2
  computerKeyboard_.onChanged = [this](const ComputerKeyboard& k) {
    notify(view_change::kKeyboard);  // KB.1: the control bar's button
    if (!k.active()) {
      setStatus("Computer MIDI Keyboard off");
      return;
    }
    const ComputerKeyboard::Layout l = keyboardLayout();
    setStatus("Computer MIDI Keyboard on: " + ComputerKeyboard::layoutName(l, k.scaleMode()) + ", equave " +
              std::string(k.equave() > 0 ? "+" : "") + std::to_string(k.equave()) + ", velocity " +
              std::to_string(k.velocity()) +
              (l.piano ? " (Z / X equave, C / V velocity)" : " (Page Up / Down equave, Home / End velocity)"));
  };
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
