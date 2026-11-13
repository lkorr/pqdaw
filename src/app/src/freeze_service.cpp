// FZ.2: frozen-track renders (pq/app/freeze_service.h).
#include "pq/app/freeze_service.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "pq/app/export_render.h"
#include "pq/document/serialize.h"
#include "pq/media/wav_file.h"

namespace pq::app {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

Error freezeError(const std::string& m) { return Error{ErrorCode::InvalidArgument, "freeze: " + m, {}}; }

fs::path u8path(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string u8str(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t fnv(std::string_view s, uint64_t h = 1469598103934665603ull) noexcept {
  for (const char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h;
}

void removeQuietly(const fs::path& p) {
  std::error_code ec;
  fs::remove(p, ec);
}

bool frozenable(const doc::Track& t) noexcept {
  return t.active && (t.kind == doc::TrackKind::Midi || t.kind == doc::TrackKind::Audio);
}
// A track whose source is a sum of other tracks: a block of it may hear any of them.
bool sumTrack(const doc::Track& t) noexcept {
  return t.kind == doc::TrackKind::Group || t.kind == doc::TrackKind::Return || t.kind == doc::TrackKind::Master;
}
bool tuningLane(const doc::LanePtr& l) noexcept {
  return !doc::isTrackParam(l->param.kind) && !doc::isDeviceParamKind(l->param.kind);
}

// What the own contribution cannot depend on, cleared (the mixer's live inputs stay: arm and monitor gate an audio
// track's clips and input).
doc::Mixer normalMixer(const doc::Mixer& m) {
  doc::Mixer n;
  n.arm = m.arm;
  n.monitor = m.monitor;
  n.audioIn = m.audioIn;
  n.midiIn = m.midiIn;
  return n;
}

// A track of the render document. `content`: it is kept (blocks, clips, device lanes); otherwise an empty container
// (an ancestor of a kept track: its tuning assignment). Tuning lanes go to `tuningLanes` (they drive tunings wherever
// they live).
std::shared_ptr<doc::Track> normalTrack(const doc::Track& t, bool content, std::vector<doc::LanePtr>& tuningLanes) {
  auto n = std::make_shared<doc::Track>(t);
  n->name.clear();
  n->colour = doc::Colour{};
  n->mixer = normalMixer(t.mixer);
  n->frozen = false;
  n->lanes.clear();
  for (const doc::LanePtr& l : t.lanes) {
    if (tuningLane(l)) tuningLanes.push_back(l);
    else if (content && doc::isDeviceParamKind(l->param.kind)) n->lanes.push_back(l);
  }
  if (!content) {
    n->devices.clear();
    n->clips.clear();
    n->takes.clear();
    n->auditionTake = doc::LaneId{};
  }
  return n;
}

std::string keyField(uint64_t v) { return hex64(v); }

// The render a cache `.json` describes, if it matches `key` and its WAV exists.
std::optional<FreezeService::Render> lookupCache(const fs::path& dir, doc::TrackId track, const FreezeKey& key) {
  std::error_code ec;
  const fs::path meta = dir / u8path(std::to_string(track.value) + ".json");
  if (!fs::is_regular_file(meta, ec)) return std::nullopt;
  std::ifstream in(meta, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  const json j = json::parse(ss.str(), nullptr, false);
  if (!j.is_object()) return std::nullopt;
  auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
  auto num = [&](const char* k) { return j.contains(k) && j[k].is_number_integer() ? j[k].get<uint64_t>() : 0ull; };
  if (num("version") != 1 || num("rate") != key.rate || str("tempo") != keyField(key.tempo) ||
      str("tunings") != keyField(key.tunings) || str("content") != keyField(key.content))
    return std::nullopt;
  const std::string file = str("file");
  if (file.empty() || file.find_first_of("/\\") != std::string::npos) return std::nullopt;
  const fs::path wav = dir / u8path(file);
  if (!fs::is_regular_file(wav, ec) || fs::file_size(wav, ec) == 0) return std::nullopt;
  FreezeService::Render r;
  r.track = track;
  r.path = u8str(wav);
  r.frames = num("frames");
  r.channels = static_cast<uint32_t>(num("channels"));
  r.rate = static_cast<uint32_t>(num("rate"));
  r.current = true;
  if (r.frames == 0 || r.channels == 0 || r.channels > 2) return std::nullopt;
  return r;
}

Status writeCacheMeta(const fs::path& dir, doc::TrackId track, const FreezeKey& key, const fs::path& wav,
                      const OwnRender& out) {
  json j;
  j["version"] = 1;
  j["track"] = track.value;
  j["rate"] = key.rate;
  j["tempo"] = keyField(key.tempo);
  j["tunings"] = keyField(key.tunings);
  j["content"] = keyField(key.content);
  j["file"] = u8str(wav.filename());
  j["frames"] = out.frames;
  j["channels"] = out.channels;
  const fs::path meta = dir / u8path(std::to_string(track.value) + ".json");
  const fs::path tmp = dir / u8path(std::to_string(track.value) + ".json.part");
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    o << j.dump(2);
    if (!o) return freezeError("cannot write " + u8str(tmp));
  }
  std::error_code ec;
  fs::rename(tmp, meta, ec);
  if (ec) {
    removeQuietly(tmp);
    return freezeError("cannot write " + u8str(meta) + ": " + ec.message());
  }
  return Ok{};
}

// The 32-bit float WAV renderPass writes into (opened at begin: the bus's channel count).
class OwnSink final : public engine::AudioSink {
public:
  explicit OwnSink(fs::path path) : path_(std::move(path)) {}
  Status begin(uint32_t ch, uint32_t rate, uint64_t) override {
    channels = ch;
    open_ = true;
    return w_.open(path_, ch, rate);
  }
  Status write(const float* const* ch, uint32_t frames) override { return w_.write(ch, frames); }
  Status end() override {
    if (!open_) return Ok{};
    open_ = false;
    return w_.close();
  }
  uint32_t channels = 0;

private:
  fs::path path_;
  media::WavWriter w_;
  bool open_ = false;
};

}  // namespace

std::string hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
  return b;
}

// ---- The render document ----------------------------------------------------------------------------------------

std::optional<doc::Project> freezeDocument(const doc::Project& p, doc::TrackId id) {
  const doc::Track* f = doc::findTrackPtr(p, id);
  if (f == nullptr || !frozenable(*f)) return std::nullopt;
  // The tracks whose blocks (or source) feed F's blocks through cables, transitively.
  std::unordered_map<uint32_t, const doc::Track*> owner;  // device id → its track
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::DevicePtr& d : t.devices) owner.emplace(d->id.value, &t);
  });
  const doc::WireList& wires = doc::wiresOf(p);
  auto sourceOf = [&](const doc::Wire& w) -> const doc::Track* {
    if (w.fromNoteSource()) return doc::findTrackPtr(p, w.noteSource);
    const auto it = owner.find(w.from.value);
    return it != owner.end() ? it->second : nullptr;
  };
  auto targetOf = [&](const doc::Wire& w) -> const doc::Track* {
    if (w.toMixer()) return doc::findTrackPtr(p, w.mixer);
    const auto it = owner.find(w.to.value);
    return it != owner.end() ? it->second : nullptr;
  };
  std::set<uint32_t> keep{id.value};
  std::vector<const doc::Track*> todo{f};
  bool whole = false;
  while (!todo.empty() && !whole) {
    const doc::Track* t = todo.back();
    todo.pop_back();
    for (const doc::Wire& w : wires) {
      if (w.toMixer() || targetOf(w) != t) continue;  // a mixer's input is not what its blocks hear
      const doc::Track* s = sourceOf(w);
      if (s == nullptr || keep.count(s->id.value) != 0) continue;
      if (sumTrack(*s)) {
        whole = true;
        break;
      }
      keep.insert(s->id.value);
      todo.push_back(s);
    }
  }
  auto kept = [&](const doc::Track* t) { return t != nullptr && keep.count(t->id.value) != 0; };

  doc::Project q;
  q.timeline = p.timeline;
  q.tunings = p.tunings;
  q.defaultTuning = p.defaultTuning;
  q.locators = std::make_shared<const doc::LocatorList>();
  std::vector<doc::LanePtr> tuningLanes;
  if (whole) {
    // A sum (a group's, a return's, the master's) feeds F: anything may reach it, F's own output too (through its
    // group). The project as it is; only the loop, locators, punch and record settings are not part of it.
    q.tracks = p.tracks;
    q.returns = p.returns;
    q.master = p.master;
    q.samples = p.samples;
    q.wires = p.wires;
    return q;
  } else {
    // A kept track keeps its content; an ancestor of one is an empty container; everything else goes.
    std::function<bool(const doc::Track&)> anyKept = [&](const doc::Track& t) {
      if (kept(&t)) return true;
      return std::any_of(t.children.begin(), t.children.end(), [&](const doc::TrackPtr& c) { return anyKept(*c); });
    };
    std::function<doc::TrackPtr(const doc::Track&)> copy = [&](const doc::Track& t) -> doc::TrackPtr {
      auto n = normalTrack(t, kept(&t), tuningLanes);
      n->children.clear();
      for (const doc::TrackPtr& c : t.children)
        if (anyKept(*c)) n->children.push_back(copy(*c));
      return n;
    };
    for (const doc::TrackPtr& t : p.tracks)
      if (anyKept(*t)) q.tracks.push_back(copy(*t));
    for (const doc::TrackPtr& t : p.returns)
      if (kept(t.get())) q.returns.push_back(copy(*t));
    // Tuning lanes of the tracks left out (they drive their tunings from anywhere).
    doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
      if (kept(&t) || t.kind == doc::TrackKind::Master) return;
      bool ancestor = false;
      for (const doc::TrackPtr& c : t.children) ancestor |= anyKept(*c);
      if (ancestor) return;  // collected by copy()
      for (const doc::LanePtr& l : t.lanes)
        if (tuningLane(l)) tuningLanes.push_back(l);
    });
    if (p.master) {
      auto m = normalTrack(*p.master, kept(p.master.get()), tuningLanes);
      m->lanes.insert(m->lanes.end(), tuningLanes.begin(), tuningLanes.end());
      q.master = std::move(m);
    }
    // The samples the kept tracks' clips play.
    std::set<uint32_t> used;
    doc::forEachTrack(q, [&](const doc::Track& t, const doc::Track*) {
      auto add = [&](const std::vector<doc::ClipSlot>& clips) {
        for (const doc::ClipSlot& c : clips)
          if (c.audio) used.insert(c.audio->sample.value);
      };
      add(t.clips);
      for (const doc::TakeLanePtr& l : t.takes) add(l->clips);
    });
    if (!used.empty()) {
      auto s = std::make_shared<doc::SampleList>();
      for (const doc::SampleRef& r : doc::samplesOf(p))
        if (used.count(r.id.value) != 0) s->push_back(r);
      q.samples = std::move(s);
    }
  }
  // The cables: into a kept track's blocks from a kept track, and a kept track's own cables into its mixer. A cable
  // into F's mixer from another track is not F's own contribution (it stays live in playback).
  if (!wires.empty()) {
    auto w2 = std::make_shared<doc::WireList>();
    for (const doc::Wire& w : wires) {
      const doc::Track* s = sourceOf(w);
      const doc::Track* t = targetOf(w);
      if (s == nullptr || t == nullptr || !kept(s) || !kept(t)) continue;
      if (!w.toMixer() || s == t) w2->push_back(w);
    }
    if (!w2->empty()) q.wires = std::move(w2);
  }
  return q;
}

Result<FreezePlan> planFreeze(const doc::Project& p, doc::TrackId track, double rate) {
  std::optional<doc::Project> q = freezeDocument(p, track);
  if (!q) return freezeError("track " + std::to_string(track.value) + " is not an active MIDI or audio track");
  FreezePlan plan;
  // The range: 0 → the last clip a kept track plays (at least a beat).
  Ticks to = kPpq;
  doc::forEachTrack(*q, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::ClipSlot& c : doc::playedClips(t))
      if (c.active()) to = std::max(to, c.end());
  });
  plan.to = to;
  auto text = doc::serializeProject(*q);
  if (!text) return text.error();
  plan.key.rate = static_cast<uint32_t>(std::lround(rate));
  plan.key.content = fnv(*text);
  const json j = json::parse(*text, nullptr, false);
  if (j.is_object()) {
    if (j.contains("timeline")) plan.key.tempo = fnv(j["timeline"].dump());
    if (j.contains("tunings")) plan.key.tunings = fnv(j["tunings"].dump());
  }
  plan.project = std::make_shared<const doc::Project>(std::move(*q));
  return plan;
}

// ---- The render core ----------------------------------------------------------------------------------------------

Result<OwnRender> renderTrackOwn(const OwnRenderRequest& rq) {
  if (!rq.project) return freezeError("no document");
  if (rq.to <= rq.from) return freezeError("empty range");
  if (rq.file.empty()) return freezeError("no file");
  auto report = [&](float f) {
    if (rq.progress != nullptr) rq.progress->store(f, std::memory_order_relaxed);
  };
  report(0.0f);
  auto link = makeOfflineLink(rq.project, rq.rate, rq.projectDir, rq.chanceSeed, rq.jit, {rq.track});
  if (!link) return link.error();
  const engine::ProjectProgram* program = (*link)->engine().program();
  const std::string busKey = std::to_string(rq.track.value) + ":own";
  uint32_t bus = engine::kNoBus;
  if (program != nullptr)
    for (uint32_t b = 0; b < program->layout().busKey.size(); ++b)
      if (program->layout().busKey[b] == busKey) bus = b;
  if (bus == engine::kNoBus) return freezeError("track " + std::to_string(rq.track.value) + " has no own bus");
  fs::path part = rq.file;
  part += ".part";
  OwnSink sink(part);
  PassSpec spec;
  spec.from = rq.from;
  spec.to = rq.to;
  spec.tailMaxSeconds = rq.tailMaxSeconds;
  spec.targets.push_back(PassTarget{bus, &sink});
  spec.cancel = rq.cancel;
  if (rq.progress != nullptr)
    spec.progress = [&](uint64_t done, uint64_t total) {
      report(total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0f);
    };
  auto r = renderPass(**link, *rq.project, spec);
  link->reset();  // the private link's compile memory goes before the file work
  if (!r || r->cancelled) {
    removeQuietly(part);
    if (!r) return r.error();
    OwnRender out;
    out.cancelled = true;
    return out;
  }
  std::error_code ec;
  fs::rename(part, rq.file, ec);
  if (ec) {
    removeQuietly(part);
    return freezeError("cannot write " + u8str(rq.file) + ": " + ec.message());
  }
  OwnRender out;
  out.frames = r->targets.at(0).frames;
  out.peak = r->targets.at(0).peak;
  out.channels = sink.channels;
  report(1.0f);
  return out;
}

// ---- The service -------------------------------------------------------------------------------------------------

FreezeService::FreezeService(Options o) : opt_(std::move(o)) {}

FreezeService::~FreezeService() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
    for (const auto& j : jobs_) j->cancel.store(true, std::memory_order_relaxed);
    if (running_) running_->cancel.store(true, std::memory_order_relaxed);
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void FreezeService::dropJob(Entry& e) {  // mutex_ held
  if (!e.job) return;
  e.job->cancel.store(true, std::memory_order_relaxed);
  const auto it = std::find(jobs_.begin(), jobs_.end(), e.job);
  if (it != jobs_.end()) {
    jobs_.erase(it);
    ++stats_.cancelled;
  }
  e.job.reset();
  cv_.notify_all();
}

void FreezeService::queue(Entry& e, doc::TrackId id, const std::string& projectDir, double delay) {  // mutex_ held
  dropJob(e);
  auto job = std::make_shared<Job>();
  job->track = id;
  job->plan = e.plan;
  job->dir = e.dir;
  job->projectDir = projectDir;
  job->due = nowSeconds() + delay;
  jobs_.push_back(job);
  e.job = std::move(job);
  if (!worker_.joinable()) worker_ = std::thread([this] { workerMain(); });
  cv_.notify_all();
}

void FreezeService::plan(Entry& e, const doc::Project& p, const doc::Track& t, const std::string& cacheDir,
                         const std::string& projectDir, bool fresh) {  // mutex_ held
  const fs::path dir = u8path(cacheDir) / "cache" / "freeze";
  std::optional<doc::Project> d = freezeDocument(p, t.id);
  if (!d) return;
  // The cheap check: the same render document (shared subtrees compare by address) and folder: the same key.
  if (!fresh && e.plan.project && dir == e.dir && *e.plan.project == *d) return;
  ++stats_.planned;
  auto planned = planFreeze(p, t.id, opt_.rate);
  e.dir = dir;
  if (!planned) {
    dropJob(e);
    e.plan = FreezePlan{};
    e.error = planned.error().message;
    if (e.file) e.file->current = false;
    return;
  }
  const bool sameKey = e.plan.project && planned->key == e.plan.key;
  e.plan = std::move(*planned);
  if (sameKey && !fresh && (e.job || (e.file && e.file->current))) return;
  e.error.clear();
  if (e.file && e.fileKey == e.plan.key) {
    dropJob(e);
    e.file->current = true;
    return;
  }
  if (e.file) e.file->current = false;
  if (std::optional<Render> hit = lookupCache(dir, t.id, e.plan.key)) {
    dropJob(e);
    if (e.file && e.file->path != hit->path) superseded_.push_back(e.file->path);
    hit->clipId = nextClip_++;
    e.file = std::move(hit);
    e.fileKey = e.plan.key;
    ++stats_.cacheHits;
    return;
  }
  queue(e, t.id, projectDir, fresh ? 0.0 : kFreezeDebounceSeconds);
}

bool FreezeService::sync(const doc::Snapshot& s, const std::string& cacheDir, const std::string& projectDir,
                         bool reset) {
  if (!s) return false;
  std::lock_guard lock(mutex_);
  auto fingerprint = [&] {  // what the link publishes: each track's file and whether it is current
    std::vector<std::tuple<uint32_t, std::string, bool>> f;
    for (const auto& [id, e] : entries_)
      if (e.file) f.emplace_back(id, e.file->path, e.file->current);
    return f;
  };
  const auto before = fingerprint();
  const uint64_t jobsBefore = stats_.planned;
  if (reset) {
    for (auto& [id, e] : entries_) {
      dropJob(e);
      if (e.file) superseded_.push_back(e.file->path);
    }
    entries_.clear();
  }
  std::set<uint32_t> seen;
  doc::forEachTrack(*s, [&](const doc::Track& t, const doc::Track*) {
    if (!t.frozen || !frozenable(t)) return;
    seen.insert(t.id.value);
    auto it = entries_.find(t.id.value);
    const bool fresh = it == entries_.end();
    if (fresh) it = entries_.emplace(t.id.value, Entry{}).first;
    plan(it->second, *s, t, cacheDir, projectDir, fresh);
  });
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (seen.count(it->first) != 0) {
      ++it;
      continue;
    }
    dropJob(it->second);
    if (it->second.file) superseded_.push_back(it->second.file->path);
    it = entries_.erase(it);
  }
  const bool changed = fingerprint() != before;
  if (changed || reset || stats_.planned != jobsBefore) bump();
  return changed;
}

bool FreezeService::takeResults() {
  std::vector<std::shared_ptr<Job>> done;
  bool changed = false;
  {
    std::lock_guard lock(mutex_);
    done.swap(results_);
    for (const std::shared_ptr<Job>& job : done) {
      const auto it = entries_.find(job->track.value);
      if (it == entries_.end() || it->second.job != job) {  // superseded while it rendered: its file is litter
        if (job->ok) removeQuietly(u8path(job->path));
        continue;
      }
      Entry& e = it->second;
      e.job.reset();
      if (!job->ok) {
        e.error = job->error;
        changed = true;
        continue;
      }
      std::optional<Render> old = std::move(e.file);
      Render r;
      r.track = job->track;
      r.path = job->path;
      r.frames = job->out.frames;
      r.channels = job->out.channels;
      r.rate = job->plan.key.rate;
      r.clipId = nextClip_++;
      r.current = job->plan.key == e.plan.key;
      e.file = std::move(r);
      e.fileKey = job->plan.key;
      e.error.clear();
      if (old && old->path != e.file->path) {
        superseded_.push_back(old->path);
        removeQuietly(u8path(old->path));  // best effort (a stream the pool still holds keeps it)
      }
      changed = true;
    }
  }
  if (!done.empty()) bump();
  return changed;
}

void FreezeService::setRate(double rate) {
  std::lock_guard lock(mutex_);
  opt_.rate = rate;
  for (auto& [id, e] : entries_) e.plan.project.reset();  // re-planned at the next sync (fresh key)
  bump();
}

bool FreezeService::empty() const {
  std::lock_guard lock(mutex_);
  return entries_.empty();
}

std::vector<FreezeService::Render> FreezeService::renders() const {
  std::lock_guard lock(mutex_);
  std::vector<Render> out;
  for (const auto& [id, e] : entries_)
    if (e.file) out.push_back(*e.file);
  return out;
}

FreezeStatus FreezeService::status(doc::TrackId track) const {
  std::lock_guard lock(mutex_);
  FreezeStatus s;
  const auto it = entries_.find(track.value);
  if (it == entries_.end()) return s;
  const Entry& e = it->second;
  if (e.job) {
    s.state = FreezeState::Rendering;
    s.progress = e.job->progress.load(std::memory_order_relaxed);
  } else if (e.file && e.file->current) {
    s.state = FreezeState::Ready;
    s.progress = 1.0f;
  } else if (!e.error.empty()) {
    s.state = FreezeState::Failed;
    s.error = e.error;
  } else {
    s.state = FreezeState::Rendering;  // (planned, nothing queued: not reached)
  }
  return s;
}

std::vector<std::string> FreezeService::takeSuperseded() {
  std::lock_guard lock(mutex_);
  return std::exchange(superseded_, {});
}

bool FreezeService::waitIdle(double timeoutSeconds) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
  std::unique_lock lock(mutex_);
  return cv_.wait_until(lock, until, [&] { return jobs_.empty() && !running_; });
}

FreezeService::Stats FreezeService::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

void FreezeService::workerMain() {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  for (;;) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
      if (quit_) return;
      job = jobs_.front();
      const double wait = job->due - nowSeconds();
      if (wait > 0.0) {  // the debounce: a newer edit cancels it meanwhile
        cv_.wait_for(lock, std::chrono::duration<double>(wait));
        continue;
      }
      jobs_.pop_front();
      running_ = job;
      ++stats_.started;
    }
    std::error_code ec;
    fs::create_directories(job->dir, ec);
    const fs::path wav =
        job->dir / u8path(std::to_string(job->track.value) + "-" + hex64(job->plan.key.content) + ".wav");
    OwnRenderRequest rq;
    rq.project = job->plan.project;
    rq.track = job->track;
    rq.from = 0;
    rq.to = job->plan.to;
    rq.rate = static_cast<double>(job->plan.key.rate);
    rq.file = wav;
    rq.projectDir = job->projectDir;
    rq.chanceSeed = opt_.chanceSeed;
    rq.jit = opt_.jit;
    rq.cancel = &job->cancel;
    rq.progress = &job->progress;
    Result<OwnRender> r = ec ? Result<OwnRender>(freezeError("cannot create " + u8str(job->dir) + ": " + ec.message()))
                             : renderTrackOwn(rq);
    bool deliver = false;
    if (r && !r->cancelled) {
      if (Status s = writeCacheMeta(job->dir, job->track, job->plan.key, wav, *r); !s) {
        removeQuietly(wav);
        r = s.error();
      }
    }
    {
      std::lock_guard lock(mutex_);
      running_.reset();
      job->done = true;
      if (!r) {
        job->error = r.error().message;
        ++stats_.failed;
        deliver = true;
      } else if (r->cancelled || job->cancel.load(std::memory_order_relaxed)) {
        ++stats_.cancelled;
        if (!r->cancelled) removeQuietly(wav);  // finished after its cancel: superseded
      } else {
        job->ok = true;
        job->path = u8str(wav);
        job->out = *r;
        ++stats_.finished;
        deliver = true;
      }
      if (deliver) results_.push_back(job);
    }
    cv_.notify_all();
    bump();
    if (deliver && opt_.notify) opt_.notify();
  }
}

}  // namespace pq::app
