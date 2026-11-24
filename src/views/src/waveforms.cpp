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
  int64_t from = 0, to = 0;  // a window read: the range asked for (its pending entry)
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
  // M5 review H3: clamp to the file first. Callers ask for a margin around the visible frames ([−2, …) at a clip's
  // file start, past the end at its end); reads are clamped to the file, so an unclamped request never matched what
  // arrived, and every paint queued another read whose arrival repainted.
  if (e.fileFrames < 0 && e.overview && e.overview->complete()) e.fileFrames = static_cast<int64_t>(e.overview->frames());
  frame0 = std::max<int64_t>(frame0, 0);
  if (e.fileFrames >= 0) {
    frame1 = std::min(frame1, e.fileFrames);
    frame0 = std::min(frame0, frame1);
  }
  for (Entry::Win& w : e.windows)
    if (w.w->frame0 <= frame0 && w.w->frame0 + w.w->frames >= frame1) {
      w.use = ++windowClock_;
      return w.w.get();
    }
  if (frame1 <= frame0) return nullptr;  // nothing of the file is wanted
  if (e.injected || e.path.empty() || e.windowFailed) return nullptr;
  if (frame1 - frame0 > kMaxWindowFrames) return nullptr;  // more than one read holds (the overview draws it)
  for (const auto& [pa, pb] : e.pending)
    if (pa <= frame0 && pb >= frame1) return nullptr;  // in flight
  const int64_t len = std::max<int64_t>(1, frame1 - frame0);
  int64_t a = frame0 - len * kWindowMarginFactor, b = frame1 + len * kWindowMarginFactor;
  if (b - a > kMaxWindowFrames) {
    a = std::max(a, frame0 - (kMaxWindowFrames - len) / 2);
    b = std::min(b, a + kMaxWindowFrames);
  }
  a = std::max<int64_t>(a, 0);
  if (e.fileFrames >= 0) b = std::min(b, e.fileFrames);
  ++windowReads_;
  const ViewContext::UiPoster& post = ctx_.uiPoster();
  if (!post) {
    Result<media::SampleWindowPtr> w = media::readSampleWindow(utf8Path(e.path), a, b - a);
    if (!w) {
      e.windowFailed = true;
      return nullptr;
    }
    e.fileFrames = (*w)->fileFrames;
    const media::SampleWindow* got = storeWindow(e, std::move(*w));
    ++revision_;
    frame1 = std::min(frame1, e.fileFrames);
    return (got->frame0 <= frame0 && got->frame0 + got->frames >= frame1) ? got : nullptr;
  }
  if (!reads_) reads_ = std::make_unique<media::AnalysisWorker>(true);
  {
    std::lock_guard<std::mutex> l(inbox_->m);
    inbox_->post = post;
  }
  e.pending.emplace_back(a, b);
  std::weak_ptr<Inbox> weak = inbox_;
  reads_->post([weak, path = e.path, id = id.value, gen = generation_, a, b] {
    Result<media::SampleWindowPtr> w = media::readSampleWindow(utf8Path(path), a, b - a);
    Arrival arr;
    arr.id = id;
    arr.generation = gen;
    arr.path = path;
    arr.isWindow = true;
    arr.from = a;
    arr.to = b;
    if (w) arr.window = std::move(*w);
    else arr.failed = true;
    deliver(weak, std::move(arr));
  });
  return nullptr;
}

const media::SampleWindow* Waveforms::storeWindow(Entry& e, media::SampleWindowPtr w) {
  // A read window the new one covers is of no more use.
  std::erase_if(e.windows, [&](const Entry::Win& x) {
    const bool inside = !x.injected && x.w->frame0 >= w->frame0 && x.w->frame0 + x.w->frames <= w->frame0 + w->frames;
    if (inside) windowBytes_ -= x.w->data.size() * sizeof(float);
    return inside;
  });
  windowBytes_ += w->data.size() * sizeof(float);
  const media::SampleWindow* got = w.get();
  e.windows.push_back({std::move(w), ++windowClock_, false});
  // This sample's least recently used read windows past kWindowsPerSample (never the new one).
  for (;;) {
    size_t read = 0, oldest = SIZE_MAX;
    for (size_t i = 0; i < e.windows.size(); ++i) {
      if (e.windows[i].injected) continue;
      ++read;
      if (e.windows[i].w.get() != got && (oldest == SIZE_MAX || e.windows[i].use < e.windows[oldest].use)) oldest = i;
    }
    if (read <= kWindowsPerSample || oldest == SIZE_MAX) break;
    windowBytes_ -= e.windows[oldest].w->data.size() * sizeof(float);
    e.windows.erase(e.windows.begin() + static_cast<std::ptrdiff_t>(oldest));
  }
  // M5 review M9: windows used to stay until the project closed (deep-zoom scrolling over many samples kept ~1 GB).
  // The least recently used ones (never this one) go until the windows fit.
  while (windowBytes_ > kWindowCacheBytes) {
    Entry* in = nullptr;
    size_t at = 0;
    for (auto& [k, o] : entries_)
      for (size_t i = 0; i < o.windows.size(); ++i) {
        const Entry::Win& x = o.windows[i];
        if (x.injected || x.w.get() == got) continue;
        if (in == nullptr || x.use < in->windows[at].use) {
          in = &o;
          at = i;
        }
      }
    if (in == nullptr) break;
    windowBytes_ -= in->windows[at].w->data.size() * sizeof(float);
    in->windows.erase(in->windows.begin() + static_cast<std::ptrdiff_t>(at));
  }
  return got;
}

void Waveforms::dropWindows(Entry& e) {
  for (const Entry::Win& x : e.windows)
    if (!x.injected) windowBytes_ -= x.w->data.size() * sizeof(float);
  e.windows.clear();
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
      std::erase_if(e.pending, [&](const std::pair<int64_t, int64_t>& p) { return p.first == a.from && p.second == a.to; });
      if (a.window) {
        e.fileFrames = a.window->fileFrames;
        (void)storeWindow(e, std::move(a.window));
      } else {
        e.windowFailed = true;  // not retried on every paint; a re-resolution or a new project starts over
      }
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
  dropWindows(e);
  e.windows.clear();
  e.windows.push_back({std::move(w), ++windowClock_, true});  // supplied: neither counted nor evicted
  ++revision_;
}

void Waveforms::samplesResolved() {
  bool changed = false;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (!it->second.injected && ctx_.samplePath(doc::SampleId{it->first}) != it->second.path) {
      dropWindows(it->second);
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
  windowBytes_ = 0;
  ++generation_;
  ++revision_;
}

void Waveforms::waitIdle() {
  if (builds_) builds_->waitIdle();
  if (reads_) reads_->waitIdle();
  drain();
}

}  // namespace pq::views
