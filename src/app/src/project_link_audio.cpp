// T5.4: audio clips through the playback link (pq/app/project_link.h "Audio clips"): the sample pool and its loader,
// the audio sequence images, prefetch and the disk indicator. T6.3: the warp modes (markers → engine::WarpSegments,
// Beats / Tones / Texture settings) and the transient analysis Beats slices at (on the loader thread, cached by content
// hash in <project>/cache/analysis and in memory: media::loadOrDetectTransients).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "pq/app/project_link.h"
#include "pq/dsp/resample.h"
#include "pq/engine/clip_player.h"
#include "pq/media/transients.h"

namespace pq::app {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
// T6.3: a loader job (mipQueue_) with this bit analyses the sample's transients instead of building mips (the link's
// declaration is shared with other lanes, so the job kind travels in the levels word).
constexpr uint32_t kTransientJob = 0x80000000u;

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
         a.fadeOutCurve == b.fadeOutCurve && a.crossfadeIn == b.crossfadeIn && a.cutEnd == b.cutEnd &&
         a.markers == b.markers && a.pitchRatio == b.pitchRatio && a.warpParams == b.warpParams;
}

// The engine's warp mode (T6.5: Complex / Complex Pro are the engine's phase vocoder, pq/engine/stretch_pool.h).
engine::ClipWarp engineWarp(doc::AudioWarp w) {
  switch (w) {
    case doc::AudioWarp::Off: return engine::ClipWarp::Off;
    case doc::AudioWarp::RePitch: return engine::ClipWarp::RePitch;
    case doc::AudioWarp::Beats: return engine::ClipWarp::Beats;
    case doc::AudioWarp::Tones: return engine::ClipWarp::Tones;
    case doc::AudioWarp::Texture: return engine::ClipWarp::Texture;
    case doc::AudioWarp::Complex: return engine::ClipWarp::Complex;
    case doc::AudioWarp::ComplexPro: return engine::ClipWarp::ComplexPro;
  }
  return engine::ClipWarp::Off;
}

// The analysis cache directory of each queued transient job (by content hash): set on the UI thread when the job is
// queued, taken by the loader.
std::mutex gAnalysisDirsMutex;
std::unordered_map<uint64_t, std::filesystem::path> gAnalysisDirs;

void putAnalysisDir(uint64_t hash, std::filesystem::path dir) {
  std::lock_guard lock(gAnalysisDirsMutex);
  gAnalysisDirs[hash] = std::move(dir);
}

std::filesystem::path takeAnalysisDir(uint64_t hash) {
  std::lock_guard lock(gAnalysisDirsMutex);
  const auto it = gAnalysisDirs.find(hash);
  if (it == gAnalysisDirs.end()) return {};
  std::filesystem::path d = std::move(it->second);
  gAnalysisDirs.erase(it);
  return d;
}

// Beats with granulation 0 slices at the sample's transients.
bool wantsTransients(const doc::AudioClipDesc& d) {
  return d.warp == doc::AudioWarp::Beats && d.warpParams.beatsGranulation == 0;
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

engine::AudioClipDesc toEngine(const doc::AudioClipDesc& d, uint32_t sample, bool playable,
                               std::span<const double> transients = {}) {
  engine::AudioClipDesc c;
  c.id = d.id.value;
  c.sample = sample;
  c.start = d.start;
  c.length = d.length;
  c.warp = engineWarp(d.warp);
  c.reverse = d.reverse;
  c.playable = playable;
  c.contentStartFrame = d.contentStartFrame;
  c.speed = d.speed;
  c.contentStart = d.contentStart;
  c.loop = d.loop;
  c.framesPerTick = d.framesPerTick;
  // T6.3: the warp markers as the law's segments (doc::warpFrameAt: a lone marker at framesPerTick through it; else
  // between neighbours, the first and last segments extended).
  const std::vector<doc::WarpMarker>& m = d.markers;
  if (m.size() == 1) {
    c.segments.push_back({m[0].content, m[0].sourceFrame, d.framesPerTick});
  } else {
    for (size_t i = 0; i + 1 < m.size(); ++i)
      c.segments.push_back({m[i].content, m[i].sourceFrame,
                            (m[i + 1].sourceFrame - m[i].sourceFrame) / static_cast<double>(m[i + 1].content - m[i].content)});
  }
  c.stretch.pitchRatio = d.pitchRatio;
  c.stretch.granulation = d.warpParams.beatsGranulation;
  c.stretch.sliceLoop = d.warpParams.beatsLoop == doc::BeatsLoop::Off       ? engine::SliceLoop::Off
                        : d.warpParams.beatsLoop == doc::BeatsLoop::Forward ? engine::SliceLoop::Forward
                                                                            : engine::SliceLoop::BackForward;
  c.stretch.envelope = d.warpParams.beatsEnvelope;
  c.stretch.grainSeconds = static_cast<double>(d.warpParams.grainSizeMs) / 1000.0;
  c.stretch.flux = d.warp == doc::AudioWarp::Texture ? d.warpParams.flux : 0.0f;
  c.stretch.seed = d.id.value;
  c.stretch.formants = d.warpParams.formants;          // T6.5 Complex Pro
  c.stretch.formantEnvelope = d.warpParams.envelope;
  c.transients = transients;
  c.gain = d.gain;
  c.fadeIn = d.fadeIn;
  c.fadeOut = d.fadeOut;
  c.fadeInCurve = d.fadeInCurve;
  c.fadeOutCurve = d.fadeOutCurve;
  c.crossfadeIn = d.crossfadeIn;
  c.cutEnd = d.cutEnd;
  return c;
}

}  // namespace

uint64_t ProjectLink::diskUnderruns() const { return pool_ != nullptr ? pool_->stats().underruns : 0; }

uint64_t ProjectLink::stretchFallbacks() const {
  return engine_ != nullptr ? engine_->timeline().clipPlayer().stretchFallbacksShared() : 0;
}

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

void ProjectLink::requestMips(const media::SampleRef& s, uint32_t levels) {
  {
    std::lock_guard lock(loadMutex_);
    for (const auto& [q, l] : mipQueue_)
      if (q == s && (l & kTransientJob) == (levels & kTransientJob) && (l & ~kTransientJob) >= (levels & ~kTransientJob)) return;
    mipQueue_.emplace_back(s, levels);
    ++loadsPending_;
  }
  loadCv_.notify_all();
}

void ProjectLink::loaderMain() {
  for (;;) {
    std::string path;
    media::SampleRef mipSample;
    uint32_t mipLevels = 0;
    {
      std::unique_lock lock(loadMutex_);
      loadCv_.wait(lock, [&] { return loaderQuit_ || !loadQueue_.empty() || !mipQueue_.empty(); });
      if (loaderQuit_) return;
      if (!loadQueue_.empty()) {
        path = std::move(loadQueue_.front());
        loadQueue_.pop_front();
      } else {
        mipSample = std::move(mipQueue_.front().first);
        mipLevels = mipQueue_.front().second;
        mipQueue_.pop_front();
      }
    }
    if (mipSample != nullptr && (mipLevels & kTransientJob) != 0) {  // T6.3: transients, then a republish
      media::TransientRequest tr;
      tr.path = mipSample->path();
      tr.contentHash = mipSample->contentHash();
      tr.frames = mipSample->view().frames;
      const media::SampleView view = mipSample->view();
      if (!view.streamed()) tr.resident = &view;
      tr.cacheDir = takeAnalysisDir(tr.contentHash);
      if (!media::loadOrDetectTransients(tr)) {  // a failure leaves the Beats clip on its 1/16 grid (not retried)
        auto none = std::make_shared<media::Transients>();
        none->contentHash = tr.contentHash;
        none->frames = tr.frames;
        media::transientCache().put(std::move(none));
      }
      mipSample.reset();
      std::function<void(std::function<void()>)> post;
      {
        std::lock_guard lock(loadMutex_);
        --loadsPending_;
        post = post_;
      }
      samplesArrived_.store(true, std::memory_order_release);
      loadCv_.notify_all();
      if (post) post([alive = std::weak_ptr<int>(alive_), this] {
        if (alive.lock()) takeLoadedSamples();
      });
      continue;
    }
    if (mipSample != nullptr) {  // no republish: the player reads PooledSample::mips() itself
      (void)pool_->buildMips(*mipSample, mipLevels);
      mipSample.reset();
      {
        std::lock_guard lock(loadMutex_);
        --loadsPending_;
      }
      loadCv_.notify_all();
      continue;
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
  const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
  for (;;) {  // the publish after the loads may ask for mips: wait for those too
    bool ok = true;
    {
      std::unique_lock lock(loadMutex_);
      ok = loadCv_.wait_until(lock, until, [&] { return loadsPending_ == 0; });
    }
    takeLoadedSamples();
    if (!ok) return false;
    std::lock_guard lock(loadMutex_);
    if (loadsPending_ == 0) return true;
  }
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
  // T6.3: the transients Beats clips slice at (kept alive until the build copied them), analysed on the loader.
  std::vector<media::TransientsPtr> transients;
  std::set<uint32_t> asked;
  const std::string& pdir = ctx_ != nullptr ? ctx_->projectDir() : cfg_.projectDir;
  auto transientsOf = [&](uint32_t si) -> std::span<const double> {
    const media::SampleRef& ref = samples[si].pooled;
    if (ref == nullptr) return {};
    if (media::TransientsPtr t = media::transientCache().find(ref->contentHash(), ref->view().frames)) {
      samplesKey.push_back(t.get());  // their arrival changes the image
      transients.push_back(t);
      return t->onsets;
    }
    if (asked.insert(si).second) {
      putAnalysisDir(ref->contentHash(), pdir.empty() ? std::filesystem::path() : pathFromUtf8(pdir) / "cache" / "analysis");
      requestMips(ref, kTransientJob);
    }
    return {};
  };
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
      const std::span<const double> tr = si != kNone && wantsTransients(d) ? transientsOf(si) : std::span<const double>();
      et.clips.push_back(toEngine(d, si == kNone ? 0u : si, d.playable && si != kNone, tr));
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
  if (pool_ != nullptr) {  // M5 review M2: demotion's LRU follows use, and the samples this image plays stay resident
    std::vector<const media::PooledSample*> used;
    used.reserve(raw->numSamples());
    for (uint32_t i = 0; i < raw->numSamples(); ++i) used.push_back(raw->sample(i).pooled.get());
    pool_->markInUse(used);
  }
  // Q151 item 1: the mip levels each resident sample's fastest clip needs (rates above 2).
  std::vector<double> fastest(raw->numSamples(), 0.0);
  for (uint32_t slot = 0; slot < raw->numSlots(); ++slot)
    for (const engine::AudioSequenceImage::Clip& c : raw->clips(slot))
      if (c.sample != engine::AudioSequenceImage::kNoSample) fastest[c.sample] = std::max(fastest[c.sample], c.peakRate);
  for (uint32_t i = 0; i < raw->numSamples(); ++i) {
    const media::SampleRef& ref = raw->sample(i).pooled;
    const uint32_t levels = dsp::mipLevelFor(fastest[i], dsp::kMaxMipLevels);
    if (levels == 0 || ref == nullptr || ref->view().streamed()) continue;
    const media::SampleMips* have = ref->mips();
    if (have == nullptr || have->levels < levels) requestMips(ref, levels);
  }
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

Result<std::vector<std::vector<float>>> ProjectLink::renderClip(const doc::Project& p, doc::ClipRef ref) {
  const std::optional<doc::TrackPath> path = doc::findTrack(p, ref.track);
  if (!path) return Error{ErrorCode::NotFound, "renderClip: no such track", {}};
  const doc::Track& t = doc::trackAt(p, *path);
  const int32_t ci = doc::findClipIndex(t, ref.clip);
  if (ci < 0 || !t.clips[static_cast<size_t>(ci)].audio) return Error{ErrorCode::InvalidArgument, "renderClip: not an audio clip", {}};
  const doc::AudioClip& clip = *t.clips[static_cast<size_t>(ci)].audio;
  const std::string file = samplePathOf(p, clip.sample);
  if (file.empty()) return Error{ErrorCode::NotFound, "the clip's sample is missing", {}};
  if (pool_ == nullptr) return Error{ErrorCode::Unsupported, "renderClip: no sample pool", {}};
  auto sample = pool_->load(pathFromUtf8(file));
  if (!sample) return std::move(sample).error();
  // The track's lowering of this clip (its tuning resolves a Steps transpose), alone: no crossfade.
  const doc::AudioTrackDesc desc = doc::lowerAudioTrack(p, *path);
  const doc::AudioClipDesc* d = nullptr;
  for (const doc::AudioClipDesc& c : desc.clips)
    if (c.id == clip.id) d = &c;
  if (d == nullptr) return Error{ErrorCode::NotFound, "renderClip: the clip did not lower", {}};
  engine::AudioTrackDesc one;
  one.id = t.id.value;
  media::TransientsPtr tr = wantsTransients(*d) ? media::transientCache().find((*sample)->contentHash(), (*sample)->view().frames)
                                                : nullptr;
  one.clips.push_back(toEngine(*d, 0, d->playable, tr != nullptr ? std::span<const double>(tr->onsets) : std::span<const double>()));
  one.clips.back().crossfadeIn = 0;
  std::vector<engine::ClipSample> samples(1);
  samples[0].pooled = *sample;
  const uint32_t channels = std::min<uint32_t>(2, std::max<uint32_t>(1, (*sample)->view().channels));
  engine::AudioBuildOptions bo;
  bo.sampleRate = cfg_.sampleRate;
  bo.edgeDeclick = cfg_.edgeDeclick;
  auto img = engine::AudioSequenceImage::build(std::span<const engine::AudioTrackDesc>(&one, 1), std::move(samples),
                                               p.timeline->tempoMap, bo);
  if (!img) return std::move(img).error();
  const engine::AudioSequenceImage& image = **img;
  if (image.clips(0).empty()) return std::vector<std::vector<float>>(channels);
  const engine::AudioSequenceImage::Clip& c = image.clips(0)[0];
  const SampleTime s0 = c.bodyStart, s1 = c.bodyEnd;
  // From the engine block at or before the look-ahead (the voice exists before the clip starts, as in playback).
  const uint32_t block = cfg_.blockFrames;
  const auto ahead = static_cast<SampleTime>(std::ceil(cfg_.clipLookaheadSeconds * cfg_.sampleRate));
  SampleTime s = std::max<SampleTime>(0, s0 - ahead);
  s -= s % block;
  engine::ClipPlayerConfig pc;
  pc.slots = 1;
  pc.maxBlockFrames = block;
  pc.sampleRate = cfg_.sampleRate;
  pc.maxVoices = 8;
  pc.streamBuffers = 2;
  pc.streams = &pool_->streams();
  pc.lookaheadSeconds = cfg_.clipLookaheadSeconds;
  // T6.5: one clip (a Complex one and its fade-out after a swap at most); none for the other modes (L15).
  const bool complex = engine::clipComplex(c.warp);
  pc.stretchVoices = complex && channels == 2 ? 2u : 0u;
  pc.stretchMonoVoices = complex && channels == 1 ? 2u : 0u;
  pc.stretchCapacity = 0;
  engine::ClipPlayer player(pc);
  player.setStreamFill(&waitFill, pool_.get());
  const bool wasEngaged = engaged_;
  engageDisk(true);
  std::vector<std::vector<float>> out(channels, std::vector<float>(static_cast<size_t>(std::max<SampleTime>(0, s1 - s0)), 0.0f));
  bool first = true;
  while (s < s1) {
    const auto n = static_cast<uint32_t>(std::min<SampleTime>(block, s1 - s));
    player.beginBlock(n);
    if (first) {
      player.setImage(&image, s, true);
      player.seek(s);
      first = false;
    }
    player.play(0, s, n);
    player.endBlock();
    if (player.active()[0] != 0) {
      const SampleTime a = std::max(s, s0);
      for (uint32_t ch = 0; ch < channels; ++ch) {
        const float* src = player.channels()[ch];
        std::copy(src + (a - s), src + n, out[ch].begin() + (a - s0));
      }
    }
    s += n;
  }
  engageDisk(wasEngaged);
  return out;
}

Result<engine::RenderStats> ProjectLink::renderOffline(Ticks fromTick, uint64_t frames, engine::AudioSink& sink,
                                                        uint32_t blockFrames) {
  flush();
  takeLatencyShifts();  // T6.2: automation shifted for the compensation the render plays with
  (void)waitForSamples();
  (void)waitForMetaTables();  // TMM.5: meta modules' tables still building (deterministic, like block-filled streams)
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
