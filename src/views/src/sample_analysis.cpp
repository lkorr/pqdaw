// The views' sample analysis (T6.8): see pq/views/sample_analysis.h.

#include "pq/views/sample_analysis.h"

#include <filesystem>
#include <mutex>

#include "pq/media/waveform.h"
#include "pq/views/view_context.h"

namespace pq::views {

namespace fs = std::filesystem;

namespace {
fs::path utf8Path(std::string_view s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }

// The job: tempo (and on the way the transients) of one file, through the media caches.
bool analyse(const media::TransientRequest& r, const std::atomic<bool>* stop, SampleAnalysisResult& out) {
  Result<media::TempoPtr> t = media::loadOrDetectTempo(r, stop);
  if (!t) return false;
  out.tempo = std::move(*t);
  out.transients = media::transientCache().find(r.contentHash, r.frames);
  if (!out.transients) {  // the tempo came from its cache file but the transients' memory entry is gone
    Result<media::TransientsPtr> tr = media::loadOrDetectTransients(r, stop);
    if (!tr) return false;
    out.transients = std::move(*tr);
  }
  return true;
}
}  // namespace

struct SampleAnalysis::Inbox {
  std::mutex m;
  std::vector<Arrival> arrivals;
  bool posted = false;
  ViewContext::UiPoster post;
  SampleAnalysis* owner = nullptr;
};

SampleAnalysis::SampleAnalysis(ViewContext& ctx) : ctx_(ctx), inbox_(std::make_shared<Inbox>()) { inbox_->owner = this; }

SampleAnalysis::~SampleAnalysis() {
  {
    std::lock_guard<std::mutex> l(inbox_->m);
    inbox_->owner = nullptr;
  }
  worker_.reset();  // joins (a running job sees stopping() and ends)
}

SampleAnalysis::Entry& SampleAnalysis::entry(doc::SampleId id) {
  auto it = entries_.find(id.value);
  if (it == entries_.end()) {
    Entry e;
    e.path = ctx_.samplePath(id);
    it = entries_.emplace(id.value, std::move(e)).first;
  }
  return it->second;
}

void SampleAnalysis::request(doc::SampleId id, Entry& e) {
  e.requested = true;
  ++requests_;
  media::TransientRequest r;
  r.path = utf8Path(e.path);
  if (const doc::SampleRef* s = doc::findSample(*ctx_.doc(), id)) {
    r.contentHash = s->contentHash;
    r.frames = s->frames;
  }
  if (projectCache_ && !ctx_.projectDir().empty()) r.cacheDir = utf8Path(ctx_.projectDir()) / "cache" / "analysis";
  const ViewContext::UiPoster& post = ctx_.uiPoster();
  if (!post) {  // tests, scenes: synchronously
    e.failed = !analyse(r, nullptr, e.result);
    e.done = true;
    ++revision_;
    finish(e);
    return;
  }
  if (!worker_) worker_ = std::make_unique<media::AnalysisWorker>(true);
  {
    std::lock_guard<std::mutex> l(inbox_->m);
    inbox_->post = post;
  }
  std::weak_ptr<Inbox> weak = inbox_;
  const std::atomic<bool>* stop = &worker_->stopping();
  worker_->post([weak, r, id = id.value, gen = generation_, path = e.path, stop] {
    Arrival a;
    a.id = id;
    a.generation = gen;
    a.path = path;
    a.failed = !analyse(r, stop, a.result);
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
    if (p) p([weak] {
      std::shared_ptr<Inbox> i = weak.lock();
      if (!i) return;
      SampleAnalysis* o = nullptr;
      {
        std::lock_guard<std::mutex> l(i->m);
        o = i->owner;
      }
      if (o) o->drain();
    });
  });
}

void SampleAnalysis::finish(Entry& e) {
  std::vector<Callback> w;
  w.swap(e.waiting);
  const SampleAnalysisResult* r = e.failed ? nullptr : &e.result;
  for (Callback& fn : w) fn(r);
}

const SampleAnalysisResult* SampleAnalysis::get(doc::SampleId id) {
  Entry& e = entry(id);
  if (e.done) return e.failed ? nullptr : &e.result;
  if (e.path.empty()) return nullptr;
  if (!e.requested) request(id, e);
  return e.done && !e.failed ? &e.result : nullptr;
}

void SampleAnalysis::whenAnalysed(doc::SampleId id, Callback fn) {
  Entry& e = entry(id);
  if (e.done || e.path.empty()) {
    fn(e.done && !e.failed ? &e.result : nullptr);
    return;
  }
  e.waiting.push_back(std::move(fn));
  if (!e.requested) request(id, e);  // synchronous without a poster: runs the callback now
}

void SampleAnalysis::drain() {
  std::vector<Arrival> got;
  {
    std::lock_guard<std::mutex> l(inbox_->m);
    got.swap(inbox_->arrivals);
    inbox_->posted = false;
  }
  bool changed = false;
  for (Arrival& a : got) {
    if (a.generation != generation_) continue;
    auto it = entries_.find(a.id);
    if (it == entries_.end() || it->second.path != a.path) continue;
    Entry& e = it->second;
    e.done = true;
    e.failed = a.failed;
    e.result = std::move(a.result);
    changed = true;
    finish(e);
  }
  if (changed) {
    ++revision_;
    ctx_.sampleAnalysisChanged();
  }
}

void SampleAnalysis::samplesResolved() {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (ctx_.samplePath(doc::SampleId{it->first}) != it->second.path && it->second.waiting.empty()) {
      it = entries_.erase(it);
      ++revision_;
    } else {
      ++it;
    }
  }
}

void SampleAnalysis::clear() {
  entries_.clear();
  ++generation_;
  ++revision_;
}

void SampleAnalysis::waitIdle() {
  if (worker_) worker_->waitIdle();
  drain();
}

}  // namespace pq::views
