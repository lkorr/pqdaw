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
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "pq/app/export_render.h"
#include "pq/document/samples.h"
#include "pq/document/serialize.h"
#include "pq/jit/compiler.h"
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
// (an ancestor of a kept track: its tuning assignment). `full` (H3: its output reaches a sum F hears): its mixer and
// mixer lanes stay (solo cleared; sends and an output to tracks `kept` does not hold dropped). Tuning lanes go to
// `tuningLanes` (they drive tunings wherever they live).
template <class Kept>
std::shared_ptr<doc::Track> normalTrack(const doc::Track& t, bool content, bool full, const Kept& kept,
                                        std::vector<doc::LanePtr>& tuningLanes) {
  auto n = std::make_shared<doc::Track>(t);
  n->name.clear();
  n->colour = doc::Colour{};
  n->frozen = false;
  if (full) {
    n->mixer.solo = false;
    std::erase_if(n->mixer.sends, [&](const doc::Send& s) { return !kept(s.target); });
    if (n->mixer.output.kind == doc::OutputKind::Track && !kept(n->mixer.output.track))
      n->mixer.output = doc::Output{doc::OutputKind::None, {}};
  } else {
    n->mixer = normalMixer(t.mixer);
  }
  n->lanes.clear();
  for (const doc::LanePtr& l : t.lanes) {
    if (tuningLane(l)) {
      tuningLanes.push_back(l);
    } else if (content && doc::isDeviceParamKind(l->param.kind)) {
      n->lanes.push_back(l);
    } else if (full && doc::isTrackParam(l->param.kind) && l->param.kind != doc::ParamKind::TrackSolo &&
               (l->param.kind != doc::ParamKind::TrackSend || kept(doc::TrackId{l->param.target}))) {
      n->lanes.push_back(l);
    }
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

fs::path metaPath(const fs::path& dir, doc::TrackId track) {
  return dir / u8path(std::to_string(track.value) + ".json");
}

// The render a cache `.json` describes, if it matches `key` and its WAV exists.
std::optional<FreezeService::Render> lookupCache(const fs::path& dir, doc::TrackId track, const FreezeKey& key) {
  std::error_code ec;
  const fs::path meta = metaPath(dir, track);
  if (!fs::is_regular_file(meta, ec)) return std::nullopt;
  std::ifstream in(meta, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  const json j = json::parse(ss.str(), nullptr, false);
  if (!j.is_object()) return std::nullopt;
  auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
  auto num = [&](const char* k) { return j.contains(k) && j[k].is_number_integer() ? j[k].get<uint64_t>() : 0ull; };
  if (num("version") != 2 || num("rate") != key.rate || str("engine") != keyField(key.engine) ||
      str("tempo") != keyField(key.tempo) || str("tunings") != keyField(key.tunings) ||
      str("content") != keyField(key.content) || str("samples") != keyField(key.samples))
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

// The WAV a cache folder's `.json` names ("" when unreadable).
std::string cacheFileOf(const fs::path& meta) {
  std::ifstream in(meta, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  const json j = json::parse(ss.str(), nullptr, false);
  if (!j.is_object() || !j.contains("file") || !j["file"].is_string()) return {};
  return j["file"].get<std::string>();
}

Status writeCacheMeta(const fs::path& dir, doc::TrackId track, const FreezeKey& key, const fs::path& wav,
                      uint64_t frames, uint32_t channels) {
  json j;
  j["version"] = 2;
  j["track"] = track.value;
  j["rate"] = key.rate;
  j["engine"] = keyField(key.engine);
  j["tempo"] = keyField(key.tempo);
  j["tunings"] = keyField(key.tunings);
  j["content"] = keyField(key.content);
  j["samples"] = keyField(key.samples);
  j["file"] = u8str(wav.filename());
  j["frames"] = frames;
  j["channels"] = channels;
  const fs::path meta = metaPath(dir, track);
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
    w_.enableRf64(longWavLimit());  // M2: a long render becomes RF64 instead of failing at 4 GiB
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

std::string freezeFileName(doc::TrackId track, const FreezeKey& key) {
  return std::to_string(track.value) + "-" + std::to_string(key.rate) + "-" +
         hex64(key.content ^ (key.engine * 0x9E3779B97F4A7C15ull) ^ (key.samples * 0xBF58476D1CE4E5B9ull)) + ".wav";
}

// ---- The render document ----------------------------------------------------------------------------------------

std::optional<doc::Project> freezeDocument(const doc::Project& p, doc::TrackId id) {
  const doc::Track* f = doc::findTrackPtr(p, id);
  if (f == nullptr || !frozenable(*f)) return std::nullopt;
  std::unordered_map<uint32_t, const doc::Track*> owner;   // device id → its track
  std::unordered_map<uint32_t, const doc::Track*> parent;  // track id → its parent (null: top level, a return, master)
  std::vector<const doc::Track*> all;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track* up) {
    for (const doc::DevicePtr& d : t.devices) owner.emplace(d->id.value, &t);
    parent.emplace(t.id.value, up);
    all.push_back(&t);
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
  // Where a track's output goes (the master's: nowhere).
  auto outputOf = [&](const doc::Track& c) -> const doc::Track* {
    if (c.kind == doc::TrackKind::Master) return nullptr;
    switch (c.mixer.output.kind) {
      case doc::OutputKind::Default: {
        const auto it = parent.find(c.id.value);
        const doc::Track* up = it != parent.end() ? it->second : nullptr;
        return up != nullptr ? up : p.master.get();
      }
      case doc::OutputKind::Master: return p.master.get();
      case doc::OutputKind::Track: return doc::findTrackPtr(p, c.mixer.output.track);
      case doc::OutputKind::None: return nullptr;
    }
    return nullptr;
  };
  // What reaches sum S: its output (a group, the master) or a stored send (a return).
  auto reaches = [&](const doc::Track& c, const doc::Track& s) {
    if (outputOf(c) == &s) return true;
    if (s.kind != doc::TrackKind::Return) return false;
    return std::any_of(c.mixer.sends.begin(), c.mixer.sends.end(), [&](const doc::Send& x) { return x.target == s.id; });
  };
  // The tracks whose blocks (or source) feed F's blocks through cables, transitively (`keep`); a sum's source brings its
  // contributors with their mixers (`full`; H3).
  std::set<uint32_t> keep{id.value}, full, sums;
  std::vector<const doc::Track*> todo{f};
  auto addSum = [&](const doc::Track& s) {
    std::vector<const doc::Track*> stack{&s};
    while (!stack.empty()) {
      const doc::Track* x = stack.back();
      stack.pop_back();
      if (!sums.insert(x->id.value).second) continue;
      for (const doc::Track* c : all) {
        if (c == x || !reaches(*c, *x)) continue;
        keep.insert(c->id.value);
        if (full.insert(c->id.value).second) todo.push_back(c);  // (again: its mixer's cables now count)
        if (sumTrack(*c)) stack.push_back(c);
      }
    }
  };
  while (!todo.empty()) {
    const doc::Track* t = todo.back();
    todo.pop_back();
    const bool mixerCounts = full.count(t->id.value) != 0;  // a contributor's mixer input reaches F
    for (const doc::Wire& w : wires) {
      if (targetOf(w) != t || (w.toMixer() && !mixerCounts)) continue;
      const doc::Track* s = sourceOf(w);
      if (s == nullptr) continue;
      if (keep.insert(s->id.value).second) todo.push_back(s);
      if (w.fromNoteSource() && sumTrack(*s)) addSum(*s);
    }
  }
  auto kept = [&](const doc::Track* t) { return t != nullptr && keep.count(t->id.value) != 0; };
  auto keptId = [&](doc::TrackId t) { return keep.count(t.value) != 0; };
  auto isFull = [&](const doc::Track& t) { return full.count(t.id.value) != 0; };

  doc::Project q;
  q.timeline = p.timeline;
  q.tunings = p.tunings;
  q.defaultTuning = p.defaultTuning;
  q.locators = std::make_shared<const doc::LocatorList>();
  std::vector<doc::LanePtr> tuningLanes;
  // A kept track keeps its content; an ancestor of one is an empty container; everything else goes.
  std::function<bool(const doc::Track&)> anyKept = [&](const doc::Track& t) {
    if (kept(&t)) return true;
    return std::any_of(t.children.begin(), t.children.end(), [&](const doc::TrackPtr& c) { return anyKept(*c); });
  };
  std::function<doc::TrackPtr(const doc::Track&)> copy = [&](const doc::Track& t) -> doc::TrackPtr {
    auto n = normalTrack(t, kept(&t), isFull(t), keptId, tuningLanes);
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
    auto m = normalTrack(*p.master, kept(p.master.get()), false, keptId, tuningLanes);
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
  // The cables: into a kept track's blocks from a kept track, a kept track's own cables into its mixer, and (H3) the
  // cables into a contributor's mixer. A cable into F's mixer from another track is not F's own contribution (it stays
  // live in playback) unless F is a contributor.
  if (!wires.empty()) {
    auto w2 = std::make_shared<doc::WireList>();
    for (const doc::Wire& w : wires) {
      const doc::Track* s = sourceOf(w);
      const doc::Track* t = targetOf(w);
      if (s == nullptr || t == nullptr || !kept(s) || !kept(t)) continue;
      if (!w.toMixer() || s == t || isFull(*t)) w2->push_back(w);
    }
    if (!w2->empty()) q.wires = std::move(w2);
  }
  return q;
}

Result<FreezePlan> planDocument(doc::Snapshot q, double rate, const std::string& projectDir) {
  if (!q) return freezeError("no render document");
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
  plan.key.engine = jit::binaryHash();
  plan.key.content = fnv(*text);
  const json j = json::parse(*text, nullptr, false);
  if (j.is_object()) {
    if (j.contains("timeline")) plan.key.tempo = fnv(j["timeline"].dump());
    if (j.contains("tunings")) plan.key.tunings = fnv(j["tunings"].dump());
  }
  // L7: the sample files as they are on disk now (size, last write time; 0 for a missing one).
  uint64_t h = 1469598103934665603ull;
  for (const doc::SampleRef& r : doc::samplesOf(*q)) {
    const std::string at = doc::isAbsoluteSamplePath(r.path) ? r.path : doc::absoluteSamplePath(r.path, projectDir);
    std::error_code sec, tec;
    const uint64_t size = fs::file_size(u8path(at), sec);
    const auto stamp = fs::last_write_time(u8path(at), tec);
    const uint64_t t = tec ? 0 : static_cast<uint64_t>(stamp.time_since_epoch().count());
    for (const uint64_t v : {static_cast<uint64_t>(r.id.value), sec ? uint64_t{0} : size, t})
      for (int b = 0; b < 8; ++b) {
        h ^= (v >> (8 * b)) & 0xFFu;
        h *= 1099511628211ull;
      }
  }
  plan.key.samples = doc::samplesOf(*q).empty() ? 0 : h;
  plan.project = std::move(q);
  return plan;
}

Result<FreezePlan> planFreeze(const doc::Project& p, doc::TrackId track, double rate) {
  std::optional<doc::Project> q = freezeDocument(p, track);
  if (!q) return freezeError("track " + std::to_string(track.value) + " is not an active MIDI or audio track");
  return planDocument(std::make_shared<const doc::Project>(std::move(*q)), rate);
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

bool FreezeService::fileInUse(const std::string& path) const {  // mutex_ held
  for (const auto& [id, e] : entries_)
    if (e.file && e.file->path == path) return true;
  return false;
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
  job->plan.project = e.doc;
  job->rate = e.rate;
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
  const uint32_t rate = static_cast<uint32_t>(std::lround(opt_.rate));
  std::optional<doc::Project> d = freezeDocument(p, t.id);
  if (!d) return;
  // The cheap compare (H3): the same render document (shared subtrees compare by address), rate and folder: nothing to do.
  const bool sameDoc = !fresh && e.doc && e.rate == rate && *e.doc == *d;
  if (sameDoc && dir == e.dir) return;
  ++stats_.synced;
  if (!sameDoc) {
    e.doc = std::make_shared<const doc::Project>(std::move(*d));
    e.rate = rate;
  }
  e.dir = dir;
  e.error.clear();
  if (e.file) {
    // An undo back to what the render was made from plays it again; any other change makes it stale (live).
    const bool rendered = e.fileDoc && e.fileKey.rate == rate && (e.fileDoc == e.doc || *e.fileDoc == *e.doc);
    e.file->current = rendered;
    if (rendered && u8path(e.file->path).parent_path() == dir) {
      dropJob(e);
      return;
    }
  }
  queue(e, t.id, projectDir, fresh || sameDoc ? 0.0 : kFreezeDebounceSeconds);
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
  const uint64_t syncedBefore = stats_.synced;
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
  if (changed || reset || stats_.synced != syncedBefore) bump();
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
      if (it == entries_.end() || it->second.job != job) {  // superseded meanwhile: a file it rendered is litter
        if (job->ok && u8path(job->path).parent_path() == job->dir && !fileInUse(job->path))
          removeQuietly(u8path(job->path));
        continue;
      }
      Entry& e = it->second;
      e.job.reset();
      if (!job->ok) {
        e.error = job->error;
        if (e.file) e.file->current = false;
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
      r.clipId = old && old->path == r.path ? old->clipId : nextClip_++;
      r.current = true;
      e.file = std::move(r);
      e.fileKey = job->plan.key;
      e.fileDoc = job->plan.project;
      e.error.clear();
      if (old && old->path != e.file->path) {
        superseded_.push_back(old->path);
        // Best effort (a stream the pool still holds keeps it; the next sweep takes it). A file in another folder (the
        // project before a Save As) belongs to that folder's cache.
        if (u8path(old->path).parent_path() == e.dir) removeQuietly(u8path(old->path));
      }
      changed = true;
    }
  }
  if (!done.empty()) bump();
  return changed;
}

void FreezeService::setRate(double rate) {
  std::lock_guard lock(mutex_);
  opt_.rate = rate;  // the next sync sees every entry at another rate: stale, re-planned
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
  if (e.file && e.file->current) {  // (a job meanwhile copies it into a new folder: it plays)
    s.state = FreezeState::Ready;
    s.progress = 1.0f;
  } else if (e.job) {
    s.state = FreezeState::Rendering;
    s.progress = e.job->progress.load(std::memory_order_relaxed);
  } else if (!e.error.empty()) {
    s.state = FreezeState::Failed;
    s.error = e.error;
  } else {
    s.state = FreezeState::Rendering;  // (synced, nothing queued: not reached)
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

void FreezeService::finish(const std::shared_ptr<Job>& job) {  // mutex_ held
  job->done = true;
  const auto it = std::find(jobs_.begin(), jobs_.end(), job);
  if (it != jobs_.end()) jobs_.erase(it);
  results_.push_back(job);
}

void FreezeService::sweep(const fs::path& dir) {
  if (std::find(swept_.begin(), swept_.end(), dir) != swept_.end()) return;
  swept_.push_back(dir);
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  std::set<std::string> named;  // the WAVs the folder's `.json` files name
  std::vector<fs::path> candidates;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code fec;
    if (!it->is_regular_file(fec)) continue;
    const fs::path& f = it->path();
    const std::string ext = u8str(f.extension());
    if (ext == ".json") {
      if (std::string n = cacheFileOf(f); !n.empty()) named.insert(std::move(n));
    } else if (ext == ".wav" || ext == ".part") {  // (`<file>.wav.part`, `<id>.json.part`: an interrupted write)
      candidates.push_back(f);
    }
  }
  if (ec) return;  // an unreadable folder: delete nothing
  uint64_t n = 0;
  for (const fs::path& f : candidates) {
    if (u8str(f.extension()) == ".wav" && named.count(u8str(f.filename())) != 0) continue;
    {
      std::lock_guard lock(mutex_);
      if (fileInUse(u8str(f))) continue;
    }
    std::error_code rec;
    if (fs::remove(f, rec)) ++n;
  }
  std::lock_guard lock(mutex_);
  stats_.swept += n;
}

void FreezeService::planJob(const std::shared_ptr<Job>& job) {
  Result<FreezePlan> planned = planDocument(job->plan.project, static_cast<double>(job->rate), job->projectDir);
  sweep(job->dir);
  std::optional<Render> have;  // the track's file now, and the key it was made for
  FreezeKey haveKey;
  {
    std::lock_guard lock(mutex_);
    running_.reset();
    if (job->cancel.load(std::memory_order_relaxed)) {
      cv_.notify_all();
      return;
    }
    ++stats_.planned;
    if (!planned) {
      job->error = planned.error().message;
      ++stats_.failed;
      finish(job);
      cv_.notify_all();
      bump();
      if (opt_.notify) opt_.notify();
      return;
    }
    job->plan = std::move(*planned);
    const auto it = entries_.find(job->track.value);
    if (it != entries_.end() && it->second.job == job && it->second.file) {
      have = it->second.file;
      haveKey = it->second.fileKey;
    }
  }
  // What to reuse: the track's own file (the same key: another folder, M6, or the same file), else the folder's cache.
  std::optional<Render> found;
  std::string error;
  if (have && haveKey == job->plan.key) {
    const fs::path from = u8path(have->path);
    if (from.parent_path() == job->dir) {
      found = have;
    } else {
      std::error_code ec;
      fs::create_directories(job->dir, ec);
      const fs::path to = job->dir / u8path(freezeFileName(job->track, job->plan.key));
      fs::path part = to;
      part += ".part";
      fs::copy_file(from, part, fs::copy_options::overwrite_existing, ec);
      if (!ec) fs::rename(part, to, ec);
      if (!ec) {
        if (Status st = writeCacheMeta(job->dir, job->track, job->plan.key, to, have->frames, have->channels); st) {
          found = have;
          found->path = u8str(to);
        }
      } else {
        removeQuietly(part);  // (the copy failed: it renders instead)
      }
    }
  }
  if (!found) found = lookupCache(job->dir, job->track, job->plan.key);
  {
    std::lock_guard lock(mutex_);
    if (job->cancel.load(std::memory_order_relaxed)) {
      cv_.notify_all();
      return;
    }
    if (!found) {
      job->planned = true;  // renders next
      cv_.notify_all();
      return;
    }
    job->ok = true;
    job->path = found->path;
    job->out.frames = found->frames;
    job->out.channels = found->channels;
    ++stats_.cacheHits;
    finish(job);
  }
  cv_.notify_all();
  bump();
  if (opt_.notify) opt_.notify();
}

void FreezeService::renderJob(const std::shared_ptr<Job>& job) {
  std::error_code ec;
  fs::create_directories(job->dir, ec);
  const fs::path wav = job->dir / u8path(freezeFileName(job->track, job->plan.key));
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
  {
    std::lock_guard lock(mutex_);  // (the .json is written under it: a sync's cache lookup sees both files or neither)
    running_.reset();
    if (r && !r->cancelled && job->cancel.load(std::memory_order_relaxed)) {  // finished after its cancel: superseded
      if (!fileInUse(u8str(wav))) removeQuietly(wav);
      r->cancelled = true;
    }
    if (r && !r->cancelled)
      if (Status st = writeCacheMeta(job->dir, job->track, job->plan.key, wav, r->frames, r->channels); !st) {
        if (!fileInUse(u8str(wav))) removeQuietly(wav);
        r = st.error();
      }
    if (!r) {
      job->error = r.error().message;
      ++stats_.failed;
      deliver = true;
    } else if (r->cancelled) {
      ++stats_.cancelled;
    } else {
      job->ok = true;
      job->path = u8str(wav);
      job->out = *r;
      ++stats_.finished;
      deliver = true;
    }
    if (deliver) {
      job->done = true;
      results_.push_back(job);
    }
  }
  cv_.notify_all();
  bump();
  if (deliver && opt_.notify) opt_.notify();
}

void FreezeService::workerMain() {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  for (;;) {
    std::shared_ptr<Job> job;
    bool render = false;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
      if (quit_) return;
      // A due job to plan first (planning is cheap and a load's cache hits must not wait behind a render), then a
      // planned one to render. The debounce: a newer edit cancels a job meanwhile.
      const double now = nowSeconds();
      double next = std::numeric_limits<double>::infinity();
      for (const std::shared_ptr<Job>& j : jobs_) {
        if (j->planned) continue;
        if (j->due <= now) {
          job = j;
          break;
        }
        next = std::min(next, j->due);
      }
      if (!job)
        for (const std::shared_ptr<Job>& j : jobs_)
          if (j->planned) {
            job = j;
            render = true;
            break;
          }
      if (!job) {
        cv_.wait_for(lock, std::chrono::duration<double>(next - now));
        continue;
      }
      if (render) {
        jobs_.erase(std::find(jobs_.begin(), jobs_.end(), job));
        ++stats_.started;
      }
      running_ = job;
    }
    if (render) renderJob(job);
    else planJob(job);
  }
}

}  // namespace pq::app
