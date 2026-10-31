// The views' waveform overviews (T5.5): see pq/views/waveforms.h.

#include "pq/views/waveforms.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "pq/views/view_context.h"

namespace pq::views {

namespace fs = std::filesystem;

namespace {
fs::path utf8Path(std::string_view s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
// Deep zoom reads: the wanted range plus this many times its length on each side (scrolling stays inside the
// window), at most kMaxWindowFrames in all.
constexpr int64_t kWindowMarginFactor = 1;
constexpr int64_t kMaxWindowFrames = int64_t{1} << 22;
}  // namespace

// Results coming back from the workers, applied on the UI thread by drain().
struct Waveforms::Arrival {
  uint32_t id = 0;
  uint64_t generation = 0;
  std::string path;
  media::OverviewPtr overview;    // first publish / cache hit
  media::SampleWindowPtr window;  // a read finished
  bool failed = false;
  bool isWindow = false;
};

struct Waveforms::Inbox {
  std::mutex m;
  std::vector<Arrival> arrivals;
  bool posted = false;
  ViewContext::UiPoster post;
  Waveforms* owner = nullptr;  // null once the store is gone
};

Waveforms::Waveforms(ViewContext& ctx) : ctx_(ctx), inbox_(std::make_shared<Inbox>()) { inbox_->owner = this; }

Waveforms::~Waveforms() {
  {
    std::lock_guard<std::mutex> l(inbox_->m);
    inbox_->owner = nullptr;
  }
  builds_.reset();  // joins (a running build sees stopping() and ends)
  reads_.reset();
}

void Waveforms::setSharedCacheDir(fs::path dir, uint64_t limitBytes) {
  sharedDir_ = std::move(dir);
  sharedLimit_ = limitBytes;
}

Waveforms::Entry& Waveforms::entry(doc::SampleId id) {
  auto it = entries_.find(id.value);
  if (it == entries_.end()) {
    Entry e;
    e.path = ctx_.samplePath(id);
    it = entries_.emplace(id.value, std::move(e)).first;
  }
  return it->second;
}

// Queues a result for the UI thread and posts one drain unless one is pending (worker threads).
void Waveforms::deliver(const std::weak_ptr<Inbox>& weak, Arrival a) {
  std::shared_ptr<Inbox> in = weak.lock();
  if (!in) return;
  ViewContext::UiPoster p;
  {
    std::lock_guard<std::mutex> l(in->m);
    in->arrivals.push_back(std::move(a));
    if (in->posted || !in->owner) return;
    in->posted = true;
    p = in->post;
  }
  if (!p) return;
  p([weak] {
    std::shared_ptr<Inbox> i = weak.lock();
    if (!i) return;
    Waveforms* o = nullptr;
    {
      std::lock_guard<std::mutex> l(i->m);
      o = i->owner;
    }
    if (o) o->drain();
  });
}

void Waveforms::request(doc::SampleId id, Entry& e) {
  e.requested = true;
  ++requests_;
  media::OverviewRequest r;
  r.path = utf8Path(e.path);
  if (const doc::SampleRef* s = doc::findSample(*ctx_.doc(), id)) r.contentHash = s->contentHash;
  if (projectCache_ && !ctx_.projectDir().empty()) {
    r.cacheDir = utf8Path(ctx_.projectDir()) / "cache" / "waveforms";
  } else if (!sharedDir_.empty()) {
    r.cacheDir = sharedDir_;
    r.cacheLimitBytes = sharedLimit_;
  }
  const ViewContext::UiPoster& post = ctx_.uiPoster();
  if (!post) {  // tests, scenes: synchronously
    Result<media::OverviewPtr> o = media::loadOrBuildOverview(r);
    if (o) e.overview = std::move(*o);
    else e.failed = true;
    ++revision_;
    return;
  }
  if (!builds_) builds_ = std::make_unique<media::AnalysisWorker>(true);
  {
    std::lock_guard<std::mutex> l(inbox_->m);
    inbox_->post = post;
  }
  std::weak_ptr<Inbox> weak = inbox_;
  const std::atomic<bool>* stop = &builds_->stopping();
  builds_->post([weak, r, id = id.value, gen = generation_, path = e.path, stop] {
    bool first = true;
    Result<media::OverviewPtr> res = media::loadOrBuildOverview(
        r,
        [&](const media::OverviewPtr& o) {
          Arrival a;
          a.id = id;
          a.generation = gen;
          a.path = path;
          if (first) a.overview = o;  // later publishes only repaint (the same object grows)
          first = false;
          deliver(weak, std::move(a));
        },
        stop);
    if (!res) {
      Arrival a;
      a.id = id;
      a.generation = gen;
      a.path = path;
      a.failed = true;
      deliver(weak, std::move(a));
    }
  });
}

const media::WaveformOverview* Waveforms::overview(doc::SampleId id) {
  Entry& e = entry(id);
  if (e.injected || e.overview) return e.overview.get();
  if (e.path.empty() || e.failed) return nullptr;
  if (!e.requested) request(id, e);
  return e.overview.get();
}

const media::SampleWindow* Waveforms::window(doc::SampleId id, int64_t frame0, int64_t frame1) {
  Entry& e = entry(id);
  if (e.window && e.window->frame0 <= frame0 && e.window->frame0 + e.window->frames >= frame1) return e.window.get();
  if (e.injected || e.path.empty()) return nullptr;
  if (e.pendingTo > e.pendingFrom && e.pendingFrom <= frame0 && e.pendingTo >= frame1) return nullptr;  // in flight
  const int64_t len = std::max<int64_t>(1, frame1 - frame0);
  int64_t a = frame0 - len * kWindowMarginFactor, b = frame1 + len * kWindowMarginFactor;
  if (b - a > kMaxWindowFrames) {
    a = std::max(a, frame0 - (kMaxWindowFrames - len) / 2);
    b = std::min(b, a + kMaxWindowFrames);
  }
  a = std::max<int64_t>(a, 0);
  const ViewContext::UiPoster& post = ctx_.uiPoster();
  if (!post) {
    Result<media::SampleWindowPtr> w = media::readSampleWindow(utf8Path(e.path), a, b - a);
    if (!w) return nullptr;
    e.window = std::move(*w);
    ++revision_;
    return (e.window->frame0 <= frame0 && e.window->frame0 + e.window->frames >= std::min<int64_t>(frame1, e.window->frame0 + e.window->frames))
               ? e.window.get()
               : nullptr;
  }
  if (!reads_) reads_ = std::make_unique<media::AnalysisWorker>(true);
  {
    std::lock_guard<std::mutex> l(inbox_->m);
    inbox_->post = post;
  }
  e.pendingFrom = a;
  e.pendingTo = b;
  std::weak_ptr<Inbox> weak = inbox_;
  reads_->post([weak, path = e.path, id = id.value, gen = generation_, a, b] {
    Result<media::SampleWindowPtr> w = media::readSampleWindow(utf8Path(path), a, b - a);
    Arrival arr;
    arr.id = id;
    arr.generation = gen;
    arr.path = path;
    arr.isWindow = true;
    if (w) arr.window = std::move(*w);
    else arr.failed = true;
    deliver(weak, std::move(arr));
  });
  return nullptr;
}

void Waveforms::drain() {
  std::vector<Arrival> got;
  {
    std::lock_guard<std::mutex> l(inbox_->m);
    got.swap(inbox_->arrivals);
    inbox_->posted = false;
  }
  if (got.empty()) return;
  bool changed = false;
  for (Arrival& a : got) {
    if (a.generation != generation_) continue;
    auto it = entries_.find(a.id);
    if (it == entries_.end() || it->second.path != a.path) continue;
    Entry& e = it->second;
    if (a.isWindow) {
      e.pendingFrom = e.pendingTo = 0;
      if (a.window) e.window = std::move(a.window);
    } else if (a.failed) {
      e.failed = !e.overview;
    } else if (a.overview) {
      e.overview = std::move(a.overview);
    }
    changed = true;
  }
  if (changed) {
    ++revision_;
    ctx_.waveformsChanged();
  }
}

void Waveforms::put(doc::SampleId id, media::OverviewPtr o) {
  Entry& e = entries_[id.value];
  e.injected = true;
  e.overview = std::move(o);
  ++revision_;
}

void Waveforms::putWindow(doc::SampleId id, media::SampleWindowPtr w) {
  Entry& e = entries_[id.value];
  e.window = std::move(w);
  ++revision_;
}

void Waveforms::samplesResolved() {
  bool changed = false;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (!it->second.injected && ctx_.samplePath(doc::SampleId{it->first}) != it->second.path) {
      it = entries_.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) ++revision_;
}

void Waveforms::clear() {
  entries_.clear();
  ++generation_;
  ++revision_;
}

void Waveforms::waitIdle() {
  if (builds_) builds_->waitIdle();
  if (reads_) reads_->waitIdle();
  drain();
}

}  // namespace pq::views
