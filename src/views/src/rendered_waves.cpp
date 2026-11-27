// SX.4: rendered clip waveforms in the views' waveform cache (pq/views/waveforms.h "RENDERED waveforms").

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "pq/document/audio_clip.h"
#include "pq/views/view_context.h"
#include "pq/views/waveforms.h"

namespace pq::views {

namespace {

using Clock = std::chrono::steady_clock;

// FNV-1a over 64-bit words, finished with a mix (the key only has to separate contents, not resist attacks).
struct Hasher {
  uint64_t v = 0xcbf29ce484222325ull;
  void word(uint64_t x) noexcept {
    for (int i = 0; i < 8; ++i) {
      v ^= (x >> (i * 8)) & 0xffu;
      v *= 0x100000001b3ull;
    }
  }
  void operator()(uint64_t x) noexcept { word(x); }
  void operator()(int64_t x) noexcept { word(static_cast<uint64_t>(x)); }
  void operator()(uint32_t x) noexcept { word(x); }
  void operator()(int32_t x) noexcept { word(static_cast<uint64_t>(static_cast<int64_t>(x))); }
  void operator()(bool x) noexcept { word(x ? 1u : 0u); }
  void operator()(double x) noexcept { word(std::bit_cast<uint64_t>(x)); }
  void operator()(float x) noexcept { word(std::bit_cast<uint32_t>(x)); }
  uint64_t done() const noexcept {
    uint64_t z = v + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return z != 0 ? z : 1;
  }
};

uint64_t stringHash(const std::string& s) noexcept {
  Hasher h;
  for (const char ch : s) h(static_cast<uint32_t>(static_cast<unsigned char>(ch)));
  h(static_cast<uint64_t>(s.size()));
  return h.done();
}

}  // namespace

uint64_t clipRenderKey(const doc::Project& p, doc::TrackId track, const doc::AudioClip& c) {
  if (!rendersWaveform(c.warp)) return 0;
  Hasher h;
  h(c.id.value);  // Texture's grain randomness is seeded by it
  h(c.sample.value);
  h(c.start);
  h(c.length);
  h(static_cast<uint32_t>(c.warp));
  h(c.contentStart);
  h(c.loop.enabled);
  h(c.loop.start);
  h(c.loop.end);
  h(c.sourceBpm);
  h(c.reverse);
  h(static_cast<uint32_t>(c.transpose.unit));
  h(c.transpose.cents);
  h(c.transpose.steps);
  h(c.detuneCents);
  // The resolved pitch (a Steps transpose through the clip's tuning, the ratio, the detune).
  if (const std::optional<doc::TrackPath> path = doc::findTrack(p, track)) h(doc::audioPitchRatio(p, *path, c));
  h(static_cast<uint64_t>(c.markers.size()));
  for (const doc::WarpMarker& m : c.markers) {
    h(m.content);
    h(m.sourceFrame);
  }
  const doc::WarpParams& w = c.warpParams;
  h(w.beatsGranulation);
  h(static_cast<uint32_t>(w.beatsLoop));
  h(w.beatsEnvelope);
  h(w.grainSizeMs);
  h(w.flux);
  h(w.formants);
  h(w.envelope);
  if (const doc::SampleRef* s = doc::findSample(p, c.sample)) {
    h(s->contentHash);
    h(s->size);
    h(s->frames);
    h(s->rate);
    h(s->channels);
  }
  // The tempo over the clip (the stretch follows it): its time at 17 points from the start to the end.
  const TempoMap& tm = p.timeline->tempoMap;
  for (int i = 0; i <= 16; ++i) h(tm.ticksToSeconds(c.start + c.length * i / 16));
  return h.done();
}

struct Waveforms::RenderJob {
  doc::Snapshot doc;
  doc::ClipRef ref;
  std::string path;
  ClipWaveConfig config;
  uint64_t key = 0;
  Ticks start = 0, length = 0;
  uint64_t frameLimit = 0;  // a render with more bytes of frames keeps only its overview (half the budget)
  Clock::time_point due{};
  uint64_t generation = 0;
};

struct Waveforms::RenderDone {
  uint64_t key = 0;
  uint32_t clip = 0;
  Ticks start = 0, length = 0;
  uint64_t generation = 0;
  RenderedWave wave;
  const void* doc = nullptr;  // the snapshot it rendered (a failure is retried on another one)
  bool failed = false, cancelled = false;
};

struct Waveforms::Renders {
  // Shared with the render thread (under m).
  std::mutex m;
  std::condition_variable cv, idle;
  std::vector<RenderJob> queue;  // one per clip (its newest key)
  bool running = false;
  uint32_t runningClip = 0;
  uint64_t runningKey = 0;
  std::shared_ptr<std::atomic<bool>> runningCancel;
  bool pumping = false;  // the worker runs the pump job
  bool quit = false;
  std::vector<RenderDone> done;
  bool posted = false;
  uint64_t started = 0;
  uint64_t generation = 0;
  ViewContext::UiPoster post;
  Waveforms* owner = nullptr;
  ClipWaveFn fn;  // set before the thread starts, never changed while it lives
  // UI thread only.
  std::unique_ptr<media::AnalysisWorker> worker;
  struct Cached {
    RenderedWave w;
    uint32_t clip = 0;
    Ticks start = 0, length = 0;
    uint64_t use = 0;
    const void* doc = nullptr;
    bool failed = false;
  };
  std::unordered_map<uint64_t, Cached> cache;
  std::unordered_map<uint32_t, uint64_t> latest;  // clip -> the key of its render drawn last
  std::unordered_set<uint64_t> asked;             // keys queued or running
};

namespace {

uint64_t waveBytes(const RenderedWave& w) noexcept {
  return (w.overview ? w.overview->bytes() : 0) + (w.frames ? w.frames->data.size() * sizeof(float) : 0);
}

// The overview (and the frames as one window) of a finished render; on the render thread.
bool makeWave(std::vector<std::vector<float>>& ch, double rate, uint64_t key, uint64_t frameLimitBytes, RenderedWave& out) {
  if (ch.empty() || ch[0].empty()) return false;
  const auto channels = static_cast<uint32_t>(std::min<size_t>(ch.size(), 2));
  const uint64_t frames = ch[0].size();
  std::shared_ptr<media::WaveformOverview> o =
      media::WaveformOverview::create(channels, frames, static_cast<uint32_t>(std::lround(rate)), key);
  {
    media::OverviewBuilder b(*o);
    const float* planar[2] = {ch[0].data(), ch[channels - 1].data()};
    b.add(planar, frames);
    b.finish();
  }
  out.overview = std::move(o);
  out.rate = rate;
  out.key = key;
  // A render longer than half the budget keeps only its overview (its deep zoom draws the finest level stretched).
  const uint64_t bytes = frames * channels * sizeof(float);
  if (bytes <= frameLimitBytes) {
    auto w = std::make_shared<media::SampleWindow>();
    w->frame0 = 0;
    w->frames = static_cast<int64_t>(frames);
    w->channels = channels;
    w->fileFrames = static_cast<int64_t>(frames);
    w->data.resize(static_cast<size_t>(frames) * channels);
    for (uint32_t c = 0; c < channels; ++c) std::memcpy(w->data.data() + c * frames, ch[c].data(), frames * sizeof(float));
    out.frames = std::move(w);
  }
  return true;
}

}  // namespace

void Waveforms::setClipRenderer(ClipWaveFn fn, std::function<ClipWaveConfig()> config) {
  stopRenders();
  renderFn_ = std::move(fn);
  renderConfig_ = std::move(config);
}

void Waveforms::stopRenders() {
  if (!renders_) return;
  {
    std::lock_guard l(renders_->m);
    renders_->quit = true;
    renders_->owner = nullptr;
    renders_->queue.clear();
    if (renders_->runningCancel) renders_->runningCancel->store(true, std::memory_order_relaxed);
  }
  renders_->cv.notify_all();
  renders_->worker.reset();  // joins (the pump sees quit; the render sees its cancel flag)
  renders_.reset();
  renderBytes_ = 0;
  ++revision_;
}

void Waveforms::clearRenders() {
  if (!renders_) return;
  Renders& r = *renders_;
  {
    std::lock_guard l(r.m);
    r.queue.clear();
    if (r.runningCancel) r.runningCancel->store(true, std::memory_order_relaxed);
    ++r.generation;  // what is still running is dropped when it arrives
  }
  r.cv.notify_all();
  r.cache.clear();
  r.latest.clear();
  r.asked.clear();
  renderBytes_ = 0;
}

uint64_t Waveforms::rendersStarted() const noexcept {
  if (!renders_) return 0;
  std::lock_guard l(renders_->m);
  return renders_->started;
}

bool Waveforms::waitRendersIdle(std::chrono::milliseconds timeout) {
  if (!renders_) return true;
  std::unique_lock l(renders_->m);
  return renders_->idle.wait_for(l, timeout, [&] { return renders_->queue.empty() && !renders_->running; });
}

void Waveforms::requestRender(RenderJob j) {
  j.frameLimit = renderBudget_ / 2;
  if (!renders_) {
    renders_ = std::make_shared<Renders>();
    renders_->fn = renderFn_;
    renders_->owner = this;
  }
  Renders& r = *renders_;
  const ViewContext::UiPoster& poster = ctx_.uiPoster();
  if (!poster) {  // tests, scenes: at once, on this thread
    RenderDone d;
    d.key = j.key;
    d.clip = j.ref.clip.value;
    d.start = j.start;
    d.length = j.length;
    d.generation = r.generation;
    d.doc = j.doc.get();
    {
      std::lock_guard l(r.m);
      ++r.started;
    }
    ClipWaveRequest q{j.ref, j.path, j.config, nullptr};
    Result<std::vector<std::vector<float>>> res = r.fn(*j.doc, q);
    d.failed = !res || !makeWave(*res, j.config.sampleRate, j.key, j.frameLimit, d.wave);
    storeRender(std::move(d));
    return;
  }
  r.asked.insert(j.key);
  j.generation = r.generation;
  bool startPump = false;
  {
    std::lock_guard l(r.m);
    r.post = poster;
    const uint32_t clip = j.ref.clip.value;
    const uint64_t key = j.key;
    // A newer key of the same clip replaces its queued job and cancels its running one.
    const auto it = std::find_if(r.queue.begin(), r.queue.end(), [&](const RenderJob& x) { return x.ref.clip.value == clip; });
    if (it != r.queue.end()) {
      r.asked.erase(it->key);
      *it = std::move(j);
    } else {
      r.queue.push_back(std::move(j));
    }
    if (r.running && r.runningClip == clip && r.runningKey != key && r.runningCancel)
      r.runningCancel->store(true, std::memory_order_relaxed);
    if (!r.pumping) {
      r.pumping = true;
      startPump = true;
    }
  }
  r.cv.notify_all();
  if (!startPump) return;
  if (!r.worker) r.worker = std::make_unique<media::AnalysisWorker>(true);
  std::weak_ptr<Renders> weak = renders_;
  r.worker->post([weak] {
    std::shared_ptr<Renders> rp = weak.lock();
    if (!rp) return;
    Renders& rr = *rp;
    std::unique_lock l(rr.m);
    for (;;) {
      if (rr.quit) {
        rr.pumping = false;
        rr.idle.notify_all();
        return;
      }
      if (rr.queue.empty()) {
        rr.pumping = false;
        rr.idle.notify_all();
        return;
      }
      auto first = std::min_element(rr.queue.begin(), rr.queue.end(),
                                    [](const RenderJob& a, const RenderJob& b) { return a.due < b.due; });
      if (Clock::now() < first->due) {  // the debounce: an edit of the clip meanwhile moves its due time
        rr.cv.wait_until(l, first->due);
        continue;
      }
      RenderJob job = std::move(*first);
      rr.queue.erase(first);
      auto cancel = std::make_shared<std::atomic<bool>>(false);
      rr.running = true;
      rr.runningClip = job.ref.clip.value;
      rr.runningKey = job.key;
      rr.runningCancel = cancel;
      ++rr.started;
      l.unlock();
      RenderDone d;
      d.key = job.key;
      d.clip = job.ref.clip.value;
      d.start = job.start;
      d.length = job.length;
      d.generation = job.generation;
      d.doc = job.doc.get();
      {
        ClipWaveRequest q{job.ref, job.path, job.config, cancel.get()};
        Result<std::vector<std::vector<float>>> res = rr.fn(*job.doc, q);
        job.doc.reset();  // the snapshot goes on this thread, not held until the UI drains
        if (cancel->load(std::memory_order_relaxed)) d.cancelled = true;
        else d.failed = !res || !makeWave(*res, job.config.sampleRate, job.key, job.frameLimit, d.wave);
      }
      l.lock();
      rr.running = false;
      rr.runningCancel.reset();
      rr.done.push_back(std::move(d));
      if (!rr.posted && rr.owner != nullptr && rr.post) {
        rr.posted = true;
        ViewContext::UiPoster p = rr.post;
        std::weak_ptr<Renders> w2 = rp;
        l.unlock();
        p([w2] {
          std::shared_ptr<Renders> r2 = w2.lock();
          if (!r2) return;
          Waveforms* o = nullptr;
          {
            std::lock_guard g(r2->m);
            o = r2->owner;
          }
          if (o != nullptr) (void)o->drainRenders();
        });
        l.lock();
      }
    }
  });
}

bool Waveforms::drainRenders() {
  if (!renders_) return false;
  std::vector<RenderDone> got;
  {
    std::lock_guard l(renders_->m);
    got.swap(renders_->done);
    renders_->posted = false;
  }
  if (got.empty()) return false;
  bool any = false;
  for (RenderDone& d : got) {
    renders_->asked.erase(d.key);
    if (d.generation != renders_->generation || d.cancelled) continue;
    storeRender(std::move(d));
    any = true;
  }
  return any;
}

void Waveforms::storeRender(RenderDone d) {
  const auto t0 = Clock::now();
  Renders& r = *renders_;
  Renders::Cached& c = r.cache[d.key];
  renderBytes_ -= std::min(renderBytes_, waveBytes(c.w));
  c.w = std::move(d.wave);
  c.clip = d.clip;
  c.start = d.start;
  c.length = d.length;
  c.failed = d.failed;
  c.doc = d.doc;
  c.use = ++renderClock_;
  renderBytes_ += waveBytes(c.w);
  ++rendersDone_;
  // The budget: the least recently drawn renders' frames first (never this one's), then whole renders.
  while (renderBytes_ > renderBudget_) {
    Renders::Cached* lru = nullptr;
    for (auto& [k, e] : r.cache)
      if (k != d.key && e.w.frames && (lru == nullptr || e.use < lru->use)) lru = &e;
    if (lru != nullptr) {
      renderBytes_ -= lru->w.frames->data.size() * sizeof(float);
      lru->w.frames.reset();
      continue;
    }
    auto old = r.cache.end();
    for (auto it = r.cache.begin(); it != r.cache.end(); ++it)
      if (it->first != d.key && (old == r.cache.end() || it->second.use < old->second.use)) old = it;
    if (old == r.cache.end()) break;
    renderBytes_ -= std::min(renderBytes_, waveBytes(old->second.w));
    if (const auto l = r.latest.find(old->second.clip); l != r.latest.end() && l->second == old->first) r.latest.erase(l);
    r.cache.erase(old);
  }
  ++revision_;
  lastSwap_ = std::chrono::duration<double>(Clock::now() - t0).count();
  ctx_.waveformsChanged();
}

RenderedLookup Waveforms::rendered(const doc::Snapshot& doc, doc::ClipRef ref, const doc::AudioClip& c, uint64_t key) {
  RenderedLookup out;
  if (!renderFn_ || key == 0 || !doc) return out;
  const ClipWaveConfig cfg = renderConfig_ ? renderConfig_() : ClipWaveConfig{};
  Entry& e = entry(c.sample);
  if (e.path.empty()) return out;  // a missing sample: nothing to render
  if (e.pathHash == 0) e.pathHash = stringHash(e.path);
  Hasher h;
  h(key);
  h(e.pathHash);
  h(cfg.sampleRate);
  h(cfg.blockFrames);
  const uint64_t full = h.done();
  if (renders_) {
    Renders& r = *renders_;
    if (auto it = r.cache.find(full); it != r.cache.end()) {
      // A failure (the clip not in its lane's lowering: a take lane, an audition) is retried on another snapshot.
      if (it->second.failed && it->second.doc != doc.get()) {
        r.cache.erase(it);
      } else {
        it->second.use = ++renderClock_;
        if (it->second.failed) return out;
        r.latest[c.id.value] = full;
        out.wave = &it->second.w;
        return out;
      }
    }
    if (r.asked.count(full) != 0) {
      out.rendering = true;
    }
  }
  if (!out.rendering) {
    RenderJob j;
    j.doc = doc;
    j.ref = ref;
    j.path = e.path;
    j.config = cfg;
    j.key = full;
    j.start = c.start;
    j.length = c.length;
    j.due = Clock::now() + kRenderDebounce;
    requestRender(std::move(j));
    if (const auto it = renders_->cache.find(full); it != renders_->cache.end()) {  // rendered at once (no poster)
      if (it->second.failed) return out;
      renders_->latest[c.id.value] = full;
      out.wave = &it->second.w;
      return out;
    }
    out.rendering = true;
  }
  // Meanwhile: the clip's last render drawn, if its bounds are the same.
  Renders& r = *renders_;
  if (const auto l = r.latest.find(c.id.value); l != r.latest.end())
    if (const auto it = r.cache.find(l->second); it != r.cache.end() && !it->second.failed && it->second.start == c.start &&
                                                 it->second.length == c.length) {
      it->second.use = ++renderClock_;
      out.wave = &it->second.w;
      out.stale = true;
    }
  return out;
}

const media::SampleWindow* Waveforms::renderedFrames(const doc::Snapshot& doc, doc::ClipRef ref, const doc::AudioClip& c,
                                                     const RenderedWave& w) {
  if (w.frames) return w.frames.get();
  // Dropped for the budget: rendered again with its frames (at once: a deep zoom asked), the finest overview level
  // drawn stretched meanwhile. A render too long to keep frames never has them.
  if (!renders_ || !renderFn_ || !doc || !w.overview) return nullptr;
  if (w.overview->frames() * w.overview->channels() * sizeof(float) > renderBudget_ / 2) return nullptr;
  const auto it = renders_->cache.find(w.key);
  if (it == renders_->cache.end() || renders_->asked.count(w.key) != 0) return nullptr;
  const ClipWaveConfig cfg = renderConfig_ ? renderConfig_() : ClipWaveConfig{};
  RenderJob j;
  j.doc = doc;
  j.ref = ref;
  j.path = entry(c.sample).path;
  j.config = cfg;
  j.key = w.key;
  j.start = c.start;
  j.length = c.length;
  j.due = Clock::now();
  if (ctx_.uiPoster()) {
    requestRender(std::move(j));
    return nullptr;
  }
  requestRender(std::move(j));  // no poster: rendered now, stored under the same key
  const auto again = renders_->cache.find(w.key);
  return again != renders_->cache.end() && again->second.w.frames ? again->second.w.frames.get() : nullptr;
}

}  // namespace pq::views
