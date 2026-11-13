// FZ.4: the edits that render first (pq/app/render_edits.h).
#include "pq/app/render_edits.h"

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
#include <condition_variable>
#include <system_error>

#include "pq/app/freeze_service.h"
#include "pq/app/project_link.h"
#include "pq/document/commands.h"
#include "pq/document/samples.h"
#include "pq/media/decoder.h"
#include "pq/media/probe.h"
#include "pq/media/wav_file.h"
#include "pq/views/view_context.h"

namespace pq::app {

namespace fs = std::filesystem;
using views::RenderEditJob;

namespace {

fs::path u8path(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string u8str(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

// A file-name-safe version of a track name.
std::string safeName(std::string_view s) {
  std::string out;
  for (char c : s) out += std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || (c >= 0 && c < 32) ? '_' : c;
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out.empty() ? std::string("Track") : out;
}

// `<dir>/<stem>.wav`, else `<dir>/<stem> 2.wav`, … (never an existing file).
fs::path freshFile(const fs::path& dir, const std::string& stem) {
  std::error_code ec;
  fs::path p = dir / u8path(stem + ".wav");
  for (int n = 2; fs::exists(p, ec) || fs::exists(fs::path(p) += ".part", ec); ++n) p = dir / u8path(stem + " " + std::to_string(n) + ".wav");
  return p;
}

void removeQuietly(const fs::path& p) {
  std::error_code ec;
  fs::remove(p, ec);
}

const char* labelOf(RenderEditJob::Kind k) {
  return k == RenderEditJob::Kind::Flatten ? "Flatten" : k == RenderEditJob::Kind::Bounce ? "Bounce to New Track" : "Consolidate";
}

// renderClips' sink: a 32-bit float WAV written as `<file>.part`.
class WavSink final : public engine::AudioSink {
public:
  explicit WavSink(fs::path path) : path_(std::move(path)) {}
  Status begin(uint32_t ch, uint32_t rate, uint64_t) override {
    open_ = true;
    return w_.open(path_, ch, rate);
  }
  Status write(const float* const* ch, uint32_t frames) override { return w_.write(ch, frames); }
  Status end() override {
    if (!open_) return Ok{};
    open_ = false;
    return w_.close();
  }

private:
  fs::path path_;
  media::WavWriter w_;
  bool open_ = false;
};

bool sameClips(const doc::Track& a, const doc::Track& b) {
  if (a.clips.size() != b.clips.size() || a.auditionTake != b.auditionTake) return false;
  for (size_t i = 0; i < a.clips.size(); ++i)
    if (a.clips[i].id != b.clips[i].id || a.clips[i].audio != b.clips[i].audio || a.clips[i].clip != b.clips[i].clip) return false;
  return a.auditionTake == doc::LaneId{} || a.takes == b.takes;
}

}  // namespace

// ---- Regions -------------------------------------------------------------------------------------------------------

Result<std::vector<doc::ops::RenderRegion>> renderRegions(const fs::path& file, const TempoMap& tempo, Ticks origin) {
  auto dec = media::Decoder::open(file);
  if (!dec) return dec.error();
  const media::SampleInfo info = (*dec)->info();
  const double rate = info.sampleRate;
  const uint32_t nch = std::max<uint32_t>(1, info.channels);
  if (rate <= 0.0) return Error{ErrorCode::InvalidArgument, "render regions: no sample rate", {}};
  // Loud spans in frames, joined across short gaps.
  const auto gap = static_cast<int64_t>(std::llround(kRegionGapSeconds * rate));
  const auto pad = static_cast<int64_t>(std::ceil(kRegionPadSeconds * rate));
  std::vector<std::pair<int64_t, int64_t>> spans;  // [first loud, last loud + 1)
  constexpr uint64_t kChunk = 16384;
  std::vector<std::vector<float>> buf(nch, std::vector<float>(kChunk));
  std::vector<float*> ptr(nch);
  for (uint32_t c = 0; c < nch; ++c) ptr[c] = buf[c].data();
  int64_t at = 0;
  for (;;) {
    auto got = (*dec)->read(ptr.data(), kChunk);
    if (!got) return got.error();
    if (*got == 0) break;
    for (uint64_t i = 0; i < *got; ++i) {
      bool loud = false;
      for (uint32_t c = 0; c < nch && !loud; ++c) loud = std::fabs(buf[c][i]) > kRegionFloor;
      if (!loud) continue;
      const int64_t f = at + static_cast<int64_t>(i);
      if (!spans.empty() && f - spans.back().second < gap) spans.back().second = f + 1;
      else spans.emplace_back(f, f + 1);
    }
    at += static_cast<int64_t>(*got);
  }
  const SampleTime o = tempo.ticksToSamples(origin, rate);
  std::vector<doc::ops::RenderRegion> out;
  for (const auto& [a0, b0] : spans) {
    const SampleTime a = o + std::max<int64_t>(0, a0 - pad), b = o + std::min<int64_t>(at, b0 + pad);
    // Outwards to ticks: the last tick at or before frame a, the first at or after frame b.
    Ticks s = tempo.firstTickAtOrAfterSample(a, rate);
    if (tempo.ticksToSamples(s, rate) > a) --s;
    while (s > origin && tempo.ticksToSamples(s, rate) > a) --s;
    s = std::max(s, origin);
    const Ticks e = tempo.firstTickAtOrAfterSample(b, rate);
    if (e <= s) continue;
    if (!out.empty() && s <= out.back().end) out.back().end = std::max(out.back().end, e);
    else out.push_back({s, e});
  }
  return out;
}

// ---- The service ---------------------------------------------------------------------------------------------------

struct RenderEditService::Job {
  RenderEditJob req;
  doc::Snapshot snap;  // the document at start()
  double rate = 48000.0;
  fs::path dir;        // the project's folder (`samples/<kind>/` under it) or the unsaved folder (files directly in it)
  bool titled = false;
  std::string projectDir;
  uint64_t chanceSeed = 0;
  bool jit = true;
  struct Item {
    doc::TrackId track;
    std::string name;
    std::string source;          // Flatten: the render's file
    FreezePlan plan;             // Bounce
    ProjectLink::SamplePaths paths;  // Consolidate
    fs::path file;               // the result
    Ticks origin = 0;
    std::vector<doc::ops::RenderRegion> regions;
    int64_t frames = 0;
    uint32_t channels = 0;
  };
  std::vector<Item> items;
  std::atomic<bool> cancel{false};
  std::atomic<uint32_t> index{0};
  std::atomic<float> progress{0.0f};  // the current item's
  std::atomic<bool> done{false};
  // Written by the worker before `done` (release).
  bool cancelled = false;
  std::string error;
  // The worker's wake for waitIdle.
  std::mutex m;
  std::condition_variable cv;
};

namespace {

// The worker: renders every item; on cancel or error deletes what it wrote.
void runJob(RenderEditService::Job& j, ProjectLink& link) {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  std::vector<fs::path> written;
  auto fail = [&](std::string m) {
    for (const fs::path& f : written) removeQuietly(f);
    j.error = std::move(m);
  };
  const TempoMap& tempo = j.snap->timeline->tempoMap;
  for (size_t i = 0; i < j.items.size() && j.error.empty(); ++i) {
    if (j.cancel.load(std::memory_order_relaxed)) break;
    RenderEditService::Job::Item& it = j.items[i];
    j.index.store(static_cast<uint32_t>(i), std::memory_order_relaxed);
    j.progress.store(0.0f, std::memory_order_relaxed);
    const char* sub = j.req.kind == RenderEditJob::Kind::Flatten  ? "frozen"
                      : j.req.kind == RenderEditJob::Kind::Bounce ? "bounced"
                                                                  : "consolidated";
    const fs::path dir = j.titled ? j.dir / "samples" / sub : j.dir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string stem = safeName(j.req.kind == RenderEditJob::Kind::Bounce ? it.name + " Bounce" : it.name);
    it.file = freshFile(dir, stem);
    fs::path part = it.file;
    part += ".part";
    bool regions = false;
    switch (j.req.kind) {
      case RenderEditJob::Kind::Flatten: {
        fs::copy_file(u8path(it.source), part, ec);
        if (ec) {
          removeQuietly(part);
          fail("cannot copy the render: " + ec.message());
          continue;
        }
        it.origin = 0;
        regions = true;
        break;
      }
      case RenderEditJob::Kind::Bounce: {
        OwnRenderRequest rq;
        rq.project = it.plan.project;
        rq.track = it.track;
        rq.from = j.req.range ? j.req.from : 0;
        rq.to = j.req.range ? j.req.to : it.plan.to;
        rq.tailMaxSeconds = j.req.range ? 0.0 : kFreezeTailSeconds;
        rq.rate = j.rate;
        rq.file = part;  // renderTrackOwn writes `<part>.part` and renames it onto `part`
        rq.projectDir = j.projectDir;
        rq.chanceSeed = j.chanceSeed;
        rq.jit = j.jit;
        rq.cancel = &j.cancel;
        rq.progress = &j.progress;
        auto r = renderTrackOwn(rq);
        if (!r) {
          fail(r.error().message);
          continue;
        }
        if (r->cancelled) continue;
        it.origin = rq.from;
        regions = !j.req.range;
        if (j.req.range) it.regions.push_back({j.req.from, j.req.to});
        break;
      }
      case RenderEditJob::Kind::Consolidate: {
        WavSink sink(part);
        const Status st = link.renderClips(*j.snap, it.track, j.req.from, j.req.to, it.paths, sink, &j.cancel, &j.progress);
        if (!st) {
          removeQuietly(part);
          fail(st.error().message);
          continue;
        }
        it.origin = j.req.from;
        it.regions.push_back({j.req.from, j.req.to});
        break;
      }
    }
    if (j.cancel.load(std::memory_order_relaxed)) {
      removeQuietly(part);
      break;
    }
    fs::rename(part, it.file, ec);
    if (ec) {
      removeQuietly(part);
      fail("cannot write " + u8str(it.file) + ": " + ec.message());
      continue;
    }
    written.push_back(it.file);
    const Result<media::SampleInfo> info = media::probe(it.file);
    if (!info) {
      fail(info.error().message);
      continue;
    }
    it.frames = static_cast<int64_t>(info->frames);
    it.channels = info->channels;
    if (regions) {
      auto r = renderRegions(it.file, tempo, it.origin);
      if (!r) {
        fail(r.error().message);
        continue;
      }
      it.regions = std::move(*r);
    }
  }
  if (j.error.empty() && j.cancel.load(std::memory_order_relaxed)) {
    for (const fs::path& f : written) removeQuietly(f);
    j.cancelled = true;
  }
  {
    std::lock_guard lock(j.m);
    j.done.store(true, std::memory_order_release);
  }
  j.cv.notify_all();
}

}  // namespace

RenderEditService::RenderEditService(ProjectLink& link, views::ViewContext& ctx, bool jit)
    : link_(link), ctx_(ctx), jit_(jit), alive_(std::make_shared<bool>(true)) {}

RenderEditService::~RenderEditService() {
  alive_.reset();
  if (job_) job_->cancel.store(true, std::memory_order_relaxed);
  if (worker_.joinable()) worker_.join();
  if (job_ && job_->done.load(std::memory_order_acquire) && !job_->cancelled && job_->error.empty())
    for (const Job::Item& it : job_->items) removeQuietly(it.file);  // rendered, never applied
}

bool RenderEditService::busy() const { return job_ != nullptr; }

void RenderEditService::cancel() {
  if (job_) job_->cancel.store(true, std::memory_order_relaxed);
}

bool RenderEditService::start(const RenderEditJob& req) {
  const char* label = labelOf(req.kind);
  if (job_) {
    ctx_.setStatus(std::string(label) + ": another render edit is still running");
    return false;
  }
  const doc::Snapshot snap = ctx_.doc();
  auto j = std::make_shared<Job>();
  j->req = req;
  j->snap = snap;
  j->rate = link_.sampleRate();
  j->projectDir = ctx_.projectDir();
  j->titled = !j->projectDir.empty();
  j->dir = u8path(j->titled ? j->projectDir : ctx_.unsavedRecordingDir());
  j->chanceSeed = link_.config().chanceSeed;
  j->jit = jit_;
  if (j->dir.empty()) {
    ctx_.setStatus(std::string(label) + ": save the project first (no folder for the rendered audio)");
    return false;
  }
  if (req.kind != RenderEditJob::Kind::Flatten && (!req.range ? req.kind == RenderEditJob::Kind::Consolidate : req.to <= req.from)) {
    ctx_.setStatus(std::string(label) + ": select a time range");
    return false;
  }
  for (doc::TrackId id : req.tracks) {
    const doc::Track* t = doc::findTrackPtr(*snap, id);
    if (t == nullptr) continue;
    Job::Item it;
    it.track = id;
    it.name = t->name;
    switch (req.kind) {
      case RenderEditJob::Kind::Flatten: {
        const std::optional<FreezeService::Render> r = t->frozen ? link_.frozenRender(id) : std::nullopt;
        if (!r) {
          ctx_.setStatus("Flatten: " + t->name + " does not play its render yet (freeze it and wait for the render)");
          return false;
        }
        if (const Status can = doc::ops::flattenable(*snap, id); !can) {  // FZ.x-fix: before rendering anything
          ctx_.setStatus("Flatten: " + t->name + ": " + can.error().message);
          return false;
        }
        it.source = r->path;
        break;
      }
      case RenderEditJob::Kind::Bounce: {
        Result<FreezePlan> plan = planFreeze(*snap, id, j->rate);
        if (!plan) {
          ctx_.setStatus("Bounce to New Track: " + t->name + ": " + plan.error().message);
          return false;
        }
        it.plan = std::move(*plan);
        break;
      }
      case RenderEditJob::Kind::Consolidate: {
        if (t->kind != doc::TrackKind::Audio) continue;
        it.paths = link_.samplePaths(*snap, id);
        break;
      }
    }
    j->items.push_back(std::move(it));
  }
  if (j->items.empty()) {
    ctx_.setStatus(std::string(label) + ": nothing to render");
    return false;
  }
  job_ = j;
  worker_ = std::thread([this, j, alive = std::weak_ptr<bool>(alive_), post = ctx_.uiPoster()] {
    // Progress: the status line every ~100 ms while it renders (posted; a closure finds the service gone: nothing).
    std::atomic<bool> finished{false};
    std::thread ticker;
    if (post) {
      ticker = std::thread([&] {
        while (!finished.load(std::memory_order_acquire)) {
          post([this, alive] {
            if (!alive.expired()) showProgress();
          });
          std::unique_lock lock(j->m);
          j->cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return finished.load(std::memory_order_acquire); });
        }
      });
    }
    runJob(*j, link_);
    finished.store(true, std::memory_order_release);
    j->cv.notify_all();
    if (ticker.joinable()) ticker.join();
    if (post)
      post([this, alive] {
        if (!alive.expired()) (void)poll();
      });
  });
  showProgress();
  return true;
}

void RenderEditService::showProgress() {
  if (!job_ || job_->done.load(std::memory_order_acquire)) return;
  const auto n = static_cast<float>(std::max<size_t>(1, job_->items.size()));
  const float f = (static_cast<float>(job_->index.load(std::memory_order_relaxed)) + job_->progress.load(std::memory_order_relaxed)) / n;
  const int pct = std::clamp(static_cast<int>(f * 100.0f), 0, 99);
  ctx_.setStatus(std::string(labelOf(job_->req.kind)) + ": rendering… " + std::to_string(pct) + "%",
                 views::StatusAction{"Cancel", [this] { cancel(); }});
}

bool RenderEditService::waitIdle(double timeoutSeconds) {
  if (!job_) return true;
  std::shared_ptr<Job> j = job_;
  std::unique_lock lock(j->m);
  return j->cv.wait_for(lock, std::chrono::duration<double>(timeoutSeconds),
                        [&] { return j->done.load(std::memory_order_acquire); });
}

bool RenderEditService::poll() {
  if (!job_ || !job_->done.load(std::memory_order_acquire)) return false;
  if (worker_.joinable()) worker_.join();
  std::shared_ptr<Job> j = std::move(job_);
  job_.reset();
  const std::string label = labelOf(j->req.kind);
  auto dropFiles = [&] {
    for (const Job::Item& it : j->items) removeQuietly(it.file);
  };
  if (j->cancelled) {
    ctx_.setStatus(label + ": cancelled");
    return true;
  }
  if (!j->error.empty()) {
    ctx_.setStatus(label + ": " + j->error);
    return true;
  }
  // The document must still hold what was rendered.
  const doc::Snapshot cur = ctx_.doc();
  bool stale = false;
  for (const Job::Item& it : j->items) {
    const doc::Track* now = doc::findTrackPtr(*cur, it.track);
    if (now == nullptr) {
      stale = true;
      break;
    }
    switch (j->req.kind) {
      case RenderEditJob::Kind::Flatten: {
        const std::optional<FreezeService::Render> r = now->frozen ? link_.frozenRender(it.track) : std::nullopt;
        stale = !r || r->path != it.source;
        break;
      }
      case RenderEditJob::Kind::Bounce:
        stale = freezeDocument(*cur, it.track) != freezeDocument(*j->snap, it.track);
        break;
      case RenderEditJob::Kind::Consolidate: {
        const doc::Track* was = doc::findTrackPtr(*j->snap, it.track);
        stale = was == nullptr || now->kind != doc::TrackKind::Audio || !sameClips(*was, *now) ||
                !(*cur->timeline == *j->snap->timeline) || cur->tunings != j->snap->tunings || cur->samples != j->snap->samples;
        break;
      }
    }
    if (stale) break;
  }
  if (stale) {
    dropFiles();
    ctx_.setStatus(label + ": cancelled (the track changed while it rendered)");
    return true;
  }
  // One Batch: each item's edit built on the document its predecessors made.
  doc::IdSource& ids = ctx_.store().ids();
  auto refOf = [&](const Job::Item& it) -> Result<doc::SampleRef> {
    const doc::SampleProbeFn probe = [&](std::string_view) -> Result<doc::SampleProbe> {
      return doc::SampleProbe{it.frames, j->rate, std::max<uint32_t>(1, it.channels)};
    };
    return doc::makeSampleRef(ids, u8str(it.file), ctx_.projectDir(), probe);
  };
  doc::Batch all{label, {}};
  doc::Snapshot scratch = cur;
  auto add = [&](const doc::ops::EditResult& e) -> Status {
    if (!e) return e.error();
    doc::ChangeSet cs;
    auto next = doc::apply(scratch, e->command, cs);
    if (!next) return next.error();
    scratch = std::move(*next);
    for (const doc::Command& c : e->batch().commands) all.commands.push_back(c);
    return Ok{};
  };
  Status st = Ok{};
  if (j->req.kind == RenderEditJob::Kind::Consolidate) {
    std::vector<doc::ops::ConsolidatedAudio> parts;
    for (const Job::Item& it : j->items) {
      Result<doc::SampleRef> ref = refOf(it);
      if (!ref) {
        st = ref.error();
        break;
      }
      parts.push_back({it.track, std::move(*ref)});
    }
    if (st) st = add(doc::ops::consolidateAudio(*scratch, ids, parts, j->req.from, j->req.to));
  } else {
    for (const Job::Item& it : j->items) {
      Result<doc::SampleRef> ref = refOf(it);
      if (!ref) {
        st = ref.error();
        break;
      }
      st = j->req.kind == RenderEditJob::Kind::Flatten
               ? add(doc::ops::flattenTrack(*scratch, ids, it.track, *ref, it.regions))
               : add(doc::ops::bounceToNewTrack(*scratch, ids, it.track, *ref, it.origin, it.regions));
      if (!st) break;
    }
  }
  if (st) st = ctx_.store().dispatch(all);
  if (!st) {
    dropFiles();
    ctx_.setStatus(label + ": " + st.error().message);
    return true;
  }
  const std::string what = j->items.size() == 1 ? j->items[0].name : std::to_string(j->items.size()) + " tracks";
  ctx_.setStatus(j->req.kind == RenderEditJob::Kind::Flatten  ? "Flattened " + what + ": it plays its render as audio clips"
                 : j->req.kind == RenderEditJob::Kind::Bounce ? "Bounced " + what + " to a new track (the source is deactivated)"
                                                              : "Consolidated " + what);
  return true;
}

}  // namespace pq::app
