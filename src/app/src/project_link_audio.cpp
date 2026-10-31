// T5.4: audio clips through the playback link (pq/app/project_link.h "Audio clips"): the sample pool and its loader,
// the audio sequence images, prefetch and the disk indicator.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "pq/app/project_link.h"
#include "pq/engine/clip_player.h"

namespace pq::app {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;

// Every active audio track with its path (main tree; deactivated groups hide their members).
template <class F>
void forEachAudio(const doc::Project& p, F&& f) {
  struct Walk {
    F& f;
    void operator()(const std::vector<doc::TrackPtr>& list, doc::TrackPath path) {
      for (uint32_t i = 0; i < list.size(); ++i) {
        const doc::Track& t = *list[i];
        if (!t.active || path.depth >= doc::kMaxTrackDepth) continue;
        doc::TrackPath here = path;
        here.index[here.depth++] = i;
        if (t.kind == doc::TrackKind::Audio) f(here, t);
        if (t.kind == doc::TrackKind::Group) (*this)(t.children, here);
      }
    }
  } walk{f};
  walk(p.tracks, doc::TrackPath{});
}

std::filesystem::path pathFromUtf8(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Offline renders: a streamed read waits for the disk thread (deterministic whatever the disk).
void waitFill(void* user, media::StreamId id, int64_t frames) {
  (void)static_cast<media::SamplePool*>(user)->waitFilled(id, frames);
}

// Field by field (doc::AudioClipDesc's defaulted == needs one on pq::ContentLoop, which core does not declare).
bool sameClip(const doc::AudioClipDesc& a, const doc::AudioClipDesc& b) {
  return a.id == b.id && a.sample == b.sample && a.start == b.start && a.length == b.length && a.warp == b.warp &&
         a.reverse == b.reverse && a.playable == b.playable && a.startSeconds == b.startSeconds &&
         a.seconds == b.seconds && a.contentStartFrame == b.contentStartFrame && a.speed == b.speed &&
         a.rateRatio == b.rateRatio && a.contentStart == b.contentStart && a.loop.enabled == b.loop.enabled &&
         a.loop.start == b.loop.start && a.loop.end == b.loop.end && a.framesPerTick == b.framesPerTick &&
         a.gain == b.gain && a.fadeIn == b.fadeIn && a.fadeOut == b.fadeOut && a.fadeInCurve == b.fadeInCurve &&
         a.fadeOutCurve == b.fadeOutCurve && a.crossfadeIn == b.crossfadeIn;
}

bool sameKey(const std::vector<std::pair<uint32_t, doc::AudioTrackDesc>>& a,
             const std::vector<std::pair<uint32_t, doc::AudioTrackDesc>>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].first != b[i].first || a[i].second.id != b[i].second.id ||
        a[i].second.clips.size() != b[i].second.clips.size())
      return false;
    for (size_t k = 0; k < a[i].second.clips.size(); ++k)
      if (!sameClip(a[i].second.clips[k], b[i].second.clips[k])) return false;
  }
  return true;
}

engine::AudioClipDesc toEngine(const doc::AudioClipDesc& d, uint32_t sample, bool playable) {
  engine::AudioClipDesc c;
  c.id = d.id.value;
  c.sample = sample;
  c.start = d.start;
  c.length = d.length;
  c.warp = d.warp == doc::AudioWarp::Off ? engine::ClipWarp::Off : engine::ClipWarp::RePitch;
  c.reverse = d.reverse;
  c.playable = playable;
  c.contentStartFrame = d.contentStartFrame;
  c.speed = d.speed;
  c.contentStart = d.contentStart;
  c.loop = d.loop;
  c.framesPerTick = d.framesPerTick;
  c.gain = d.gain;
  c.fadeIn = d.fadeIn;
  c.fadeOut = d.fadeOut;
  c.fadeInCurve = d.fadeInCurve;
  c.fadeOutCurve = d.fadeOutCurve;
  c.crossfadeIn = d.crossfadeIn;
  return c;
}

}  // namespace

uint64_t ProjectLink::diskUnderruns() const { return pool_ != nullptr ? pool_->stats().underruns : 0; }

void ProjectLink::setUiPoster(std::function<void(std::function<void()>)> post) {
  std::lock_guard lock(loadMutex_);
  post_ = std::move(post);
}

std::string ProjectLink::samplePathOf(const doc::Project& p, doc::SampleId id) {
  if (ctx_ != nullptr) return ctx_->samplePath(id);
  if (resolvedFor_ != p.samples.get()) {
    resolved_ = doc::resolveSamples(p, doc::SampleSearch{cfg_.projectDir, {}});
    resolvedFor_ = p.samples.get();
  }
  for (const doc::SampleResolution& r : resolved_)
    if (r.id == id) return r.path;
  return {};
}

void ProjectLink::requestLoad(const std::string& path) {
  {
    std::lock_guard lock(loadMutex_);
    if (loaded_.count(path) != 0) return;
    loaded_.emplace(path, LoadedSample{});
    loadQueue_.push_back(path);
    ++loadsPending_;
  }
  loadCv_.notify_all();
}

bool ProjectLink::sampleLoadFinished(const std::string& path) const {
  std::lock_guard lock(loadMutex_);
  const auto it = loaded_.find(path);
  return it != loaded_.end() && it->second.done;
}

void ProjectLink::loaderMain() {
  for (;;) {
    std::string path;
    {
      std::unique_lock lock(loadMutex_);
      loadCv_.wait(lock, [&] { return loaderQuit_ || !loadQueue_.empty(); });
      if (loaderQuit_) return;
      path = std::move(loadQueue_.front());
      loadQueue_.pop_front();
    }
    auto r = pool_->load(pathFromUtf8(path));  // probe + decode (or head + stream source): may take a while
    std::function<void(std::function<void()>)> post;
    {
      std::lock_guard lock(loadMutex_);
      LoadedSample& e = loaded_[path];
      e.done = true;
      if (r.ok()) e.ref = std::move(r.value());
      else e.error = r.error().message;
      --loadsPending_;
      post = post_;
    }
    samplesArrived_.store(true, std::memory_order_release);
    loadCv_.notify_all();
    if (post) post([alive = std::weak_ptr<int>(alive_), this] {
      if (alive.lock()) takeLoadedSamples();
    });
  }
}

void ProjectLink::takeLoadedSamples() {
  if (!samplesArrived_.exchange(false, std::memory_order_acq_rel) || !current_) return;
  {
    std::lock_guard lock(loadMutex_);
    for (const auto& [path, e] : loaded_) {
      if (!e.done || e.error.empty()) continue;
      setError("Could not load " + path + ": " + e.error);
    }
  }
  publishAudio(*current_);
}

bool ProjectLink::waitForSamples(double timeoutSeconds) {
  bool ok = true;
  {
    std::unique_lock lock(loadMutex_);
    ok = loadCv_.wait_for(lock, std::chrono::duration<double>(timeoutSeconds), [&] { return loadsPending_ == 0; });
  }
  takeLoadedSamples();
  return ok;
}

void ProjectLink::publishAudio(const doc::Project& p, bool force) {
  std::vector<engine::AudioTrackDesc> slots;
  std::vector<engine::ClipSample> samples;
  std::vector<const void*> samplesKey;
  std::vector<std::pair<uint32_t, doc::AudioTrackDesc>> key;
  std::unordered_map<uint32_t, AudioCacheEntry> next;
  std::unordered_map<uint32_t, std::string> paths;          // sample id → absolute path ("" = missing)
  std::unordered_map<uint32_t, uint32_t> index;             // sample id → index into `samples` (kNone: not ready)
  auto pathOf = [&](doc::SampleId id) -> const std::string& {
    auto it = paths.find(id.value);
    if (it == paths.end()) it = paths.emplace(id.value, samplePathOf(p, id)).first;
    return it->second;
  };
  doc::AudioLowerOptions lo;
  lo.available = [&](doc::SampleId id) { return !pathOf(id).empty(); };
  forEachAudio(p, [&](const doc::TrackPath& path, const doc::Track& t) {
    const uint32_t slot = slotForTrack(t.id);
    if (slot == kNone) return;
    AudioCacheEntry e;
    auto it = audioCache_.find(t.id.value);
    if (it != audioCache_.end() && it->second.track == &t && it->second.tempo == p.timeline.get() &&
        it->second.tunings == p.tunings.get() && it->second.samples == p.samples.get() &&
        it->second.resolution == resolution_) {
      e = std::move(it->second);
    } else {
      e.track = &t;
      e.tempo = p.timeline.get();
      e.tunings = p.tunings.get();
      e.samples = p.samples.get();
      e.resolution = resolution_;
      e.from = current_;
      e.desc = doc::lowerAudioTrack(p, path, lo);
      ++stats_.audioLowerings;
    }
    engine::AudioTrackDesc et;
    et.id = t.id.value;
    et.clips.reserve(e.desc.clips.size());
    for (const doc::AudioClipDesc& d : e.desc.clips) {
      uint32_t si = kNone;
      if (d.playable) {
        auto ix = index.find(d.sample.value);
        if (ix == index.end()) {
          const std::string& abs = pathOf(d.sample);
          if (!abs.empty()) {
            media::SampleRef ref;
            bool known = false;
            {
              std::lock_guard lock(loadMutex_);
              const auto l = loaded_.find(abs);
              known = l != loaded_.end();
              if (known && l->second.done) ref = l->second.ref;
            }
            if (!known) requestLoad(abs);
            if (ref != nullptr) {
              si = static_cast<uint32_t>(samples.size());
              engine::ClipSample cs;
              cs.pooled = ref;
              samplesKey.push_back(ref.get());
              samples.push_back(std::move(cs));
            }
          }
          ix = index.emplace(d.sample.value, si).first;
        }
        si = ix->second;
      }
      et.clips.push_back(toEngine(d, si == kNone ? 0u : si, d.playable && si != kNone));
    }
    if (slots.size() <= slot) slots.resize(slot + 1);
    key.emplace_back(slot, e.desc);
    slots[slot] = std::move(et);
    next.emplace(t.id.value, std::move(e));
  });
  audioCache_ = std::move(next);
  std::sort(key.begin(), key.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  if (!force && sameKey(key, audioKey_) && samplesKey == audioSamplesKey_) return;  // nothing an audio track plays changed
  audioKey_ = std::move(key);
  audioSamplesKey_ = std::move(samplesKey);
  engine::AudioBuildOptions bo;
  bo.sampleRate = cfg_.sampleRate;
  bo.edgeDeclick = cfg_.edgeDeclick;
  auto img = engine::AudioSequenceImage::build(slots, std::move(samples), p.timeline->tempoMap, bo);
  if (!img.ok()) {
    setError(img.error().message);
    return;
  }
  const engine::AudioSequenceImage* raw = img.value().get();
  if (Status s = engine_->timeline().publishAudio(std::move(img.value())); !s.ok()) {
    setError(s.error().message);
    return;
  }
  lastAudio_ = raw;
  prefetchedAt_ = INT64_MIN;  // the streams the next locate prefetches belong to this image
  ++stats_.audioPublishes;
  engine_->wake();
}

void ProjectLink::prefetchAt(Ticks tick) {
  if (pool_ == nullptr || lastAudio_ == nullptr || tick == prefetchedAt_) return;
  pool_->clearPrefetches();
  prefetchedAt_ = tick;
  const double sr = cfg_.sampleRate;
  const auto lookahead = static_cast<SampleTime>(std::llround(cfg_.clipLookaheadSeconds * sr));
  auto at = [&](Ticks t) {
    const SampleTime s = lastAudio_->tempo().ticksToSamples(t, sr);
    for (uint32_t slot = 0; slot < lastAudio_->numSlots(); ++slot)
      for (const engine::AudioSequenceImage::Clip& c : lastAudio_->clips(slot)) {
        if (c.playEnd <= s || c.playStart >= s + lookahead || c.sample == engine::AudioSequenceImage::kNoSample) continue;
        const engine::ClipSample& cs = lastAudio_->sample(c.sample);
        if (cs.pooled == nullptr || !cs.view().streamed()) continue;
        const auto st = engine::ClipPlayer::streamStart(*lastAudio_, c, s, cfg_.blockFrames);
        if (st && pool_->prefetch(*cs.pooled, st->frame, st->dir)) ++stats_.prefetches;
      }
  };
  at(tick);
  if (loopSent_ && loop_.enabled && loop_.start != tick) at(loop_.start);
}

void ProjectLink::engageDisk(bool on) {
  if (pool_ == nullptr || on == engaged_) return;
  if (on) pool_->engage();
  else pool_->disengage();
  engaged_ = on;
}

void ProjectLink::reportDisk() {
  const uint64_t u = diskUnderruns();
  if (u <= diskSeen_) return;
  const uint64_t burst = u - diskSeen_;
  diskSeen_ = u;
  const double now = nowSeconds();
  if (now - diskReported_ < 5.0) return;
  diskReported_ = now;
  const std::string m = "Disk too slow: " + std::to_string(burst) +
                        " streamed read(s) came late and played silence (" + std::to_string(u) + " this session)";
  setError(m);
  if (ctx_ != nullptr) ctx_->setStatus(m);
}

Result<engine::RenderStats> ProjectLink::renderOffline(Ticks fromTick, uint64_t frames, engine::AudioSink& sink,
                                                        uint32_t blockFrames) {
  flush();
  (void)waitForSamples();
  engine::ClipPlayer& clips = engine_->timeline().clipPlayer();
  clips.setStreamFill(&waitFill, pool_.get());  // the audio thread is not running: offline renders own the engine
  const bool wasEngaged = engaged_;
  engageDisk(true);
  engine::TransportCommand play{engine::TransportCommandKind::Play, false, fromTick, 0, 0};
  if (!engine_->post(play)) {
    clips.setStreamFill(nullptr, nullptr);
    engageDisk(wasEngaged);
    return Error{ErrorCode::Overflow, "playback link: transport ring full", {}};
  }
  ++commandsPosted_;
  auto r = engine_->renderOffline(frames, sink, blockFrames != 0 ? blockFrames : cfg_.blockFrames);
  engine::TransportCommand stopCmd{engine::TransportCommandKind::Stop, false, 0, 0, 0};
  if (engine_->post(stopCmd)) ++commandsPosted_;
  clips.setStreamFill(nullptr, nullptr);
  engageDisk(wasEngaged);
  return r;
}

}  // namespace pq::app
