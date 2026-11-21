// AP.7: the roll preview's worker; AP.8b: Flatten on it too (pq/views/note_preview.h).
#include "pq/views/note_preview.h"

#include <algorithm>
#include <string>
#include <utility>

#include "pq/document/note_devices.h"
#include "pq/views/view_context.h"

namespace pq::views {

NotePreviews::NotePreviews(ViewContext& ctx) : ctx_(ctx) {
  worker_ = std::thread([this] { run(); });
  workerId_ = worker_.get_id();
}

NotePreviews::~NotePreviews() {
  *alive_ = false;
  cancel_.store(true, std::memory_order_relaxed);  // L10: a long render stops instead of holding the join
  {
    std::lock_guard lock(m_);
    quit_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void NotePreviews::setSource(NotePreviewFn fn, std::function<NoteRenderConfig()> config) {
  fn_ = std::move(fn);
  config_ = std::move(config);
}

void NotePreviews::request(doc::Snapshot doc, doc::TrackId track, Ticks until) {
  if (!fn_ || !doc) return;
  NotePreviewRequest req;
  req.until = until;
  req.config = config();
  req.cancel = &cancel_;
  {
    std::lock_guard lock(m_);
    Job j{std::move(doc), track, req};
    const auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Job& x) { return x.track == track; });
    if (it != pending_.end()) *it = std::move(j);
    else pending_.push_back(std::move(j));
    last_ = std::chrono::steady_clock::now();
  }
  cv_.notify_all();
}

void NotePreviews::post(Job j) {
  {
    std::lock_guard lock(m_);
    flattens_.push_back(std::move(j));
  }
  cv_.notify_all();
}

bool NotePreviews::flatten(doc::Snapshot doc, doc::TrackId track) {
  if (!fn_ || !doc) return false;
  NotePreviewRequest req;
  req.until = doc::kMaxTicks;
  req.flatten = true;
  req.config = config();
  req.cancel = &cancel_;
  post(Job{std::move(doc), track, req, 0});
  return true;
}

bool NotePreviews::flattening() const {
  std::lock_guard lock(m_);
  if (!flattens_.empty()) return true;
  for (const Done& d : done_)
    if (d.flatten) return true;
  return busy_ && flatRunning_;
}

void NotePreviews::run() {
  std::unique_lock lock(m_);
  for (;;) {
    cv_.wait(lock, [&] { return quit_ || !pending_.empty() || !flattens_.empty(); });
    if (quit_) return;
    Job j;
    if (!flattens_.empty()) {  // a Flatten: at once, before any preview
      j = std::move(flattens_.front());
      flattens_.erase(flattens_.begin());
    } else {
      // Debounce: until no request came for kDebounce (a Flatten queued meanwhile goes first).
      while (!quit_ && flattens_.empty() && std::chrono::steady_clock::now() - last_ < kDebounce)
        cv_.wait_until(lock, last_ + kDebounce);
      if (quit_) return;
      if (!flattens_.empty()) continue;
      j = std::move(pending_.front());
      pending_.erase(pending_.begin());
    }
    busy_ = true;
    flatRunning_ = j.req.flatten;
    lock.unlock();
    Result<NotePreview> r = fn_(*j.doc, j.track, j.req);
    Done d;
    d.doc = std::move(j.doc);
    d.track = j.track;
    d.flatten = j.req.flatten;
    d.retries = j.retries;
    if (r) d.result = std::make_shared<const NotePreview>(std::move(r.value()));
    else d.error = r.error().message;
    if (!d.result && !d.flatten) d.result = std::make_shared<const NotePreview>();
    lock.lock();
    ++computes_;
    done_.push_back(std::move(d));
    busy_ = false;
    flatRunning_ = false;
    if (pending_.empty() && flattens_.empty()) idle_.notify_all();
    if (const ViewContext::UiPoster& poster = ctx_.uiPoster()) {
      std::weak_ptr<bool> alive = alive_;
      lock.unlock();
      poster([this, alive] {
        if (const auto a = alive.lock(); a && *a) (void)pump();
      });
      lock.lock();
    }
  }
}

bool NotePreviews::pump() {
  std::vector<Done> got;
  {
    std::lock_guard lock(m_);
    got.swap(done_);
  }
  if (got.empty()) return false;
  bool previews = false;
  for (Done& d : got) {
    if (d.flatten) {
      finishFlatten(d);
      continue;
    }
    latest_[d.track.value] = std::move(d);
    previews = true;
  }
  // L10: results of tracks that are gone are dropped (the map never grows past the document's tracks).
  if (const doc::Snapshot& now = ctx_.doc())
    for (auto it = latest_.begin(); it != latest_.end();)
      it = doc::findTrackPtr(*now, doc::TrackId{it->first}) == nullptr ? latest_.erase(it) : std::next(it);
  if (previews) {
    ++serial_;
    ctx_.notePreviewsChanged();
  }
  return true;
}

void NotePreviews::finishFlatten(Done& d) {
  if (d.error) {
    ctx_.setStatus("Flatten to Clip: " + *d.error);
    return;
  }
  const NotePreview& r = *d.result;
  if (!r.any) {
    ctx_.setStatus("Flatten to Clip: the track has no active note device");
    return;
  }
  // H3: a cable's value is only known while the project plays: what the render wrote would not be what plays.
  if (!r.cable.empty()) {
    ctx_.setStatus("Flatten to Clip: a cable modulates " + r.cable +
                   " (its value is only known while the project plays; remove the cable to flatten)");
    return;
  }
  const doc::Snapshot& now = ctx_.doc();
  if (now != d.doc) {  // edited while it rendered: render again from what the document is now
    if (d.retries >= kFlattenRetries || !now) {
      ctx_.setStatus("Flatten to Clip: the project kept changing while it rendered; try again");
      return;
    }
    Job j{now, d.track, {}, d.retries + 1};
    j.req.until = doc::kMaxTicks;
    j.req.flatten = true;
    j.req.config = config();
    j.req.cancel = &cancel_;
    post(std::move(j));
    return;
  }
  doc::ops::EditResult e = doc::ops::flattenNoteStages(*now, ctx_.store().ids(), d.track, r.clips);
  if (!e) {
    ctx_.setStatus("Flatten to Clip: " + e.error().message);
    return;
  }
  if (Status st = ctx_.store().dispatch(e->command); !st) {
    ctx_.setStatus("Flatten to Clip: " + st.error().message);
    return;
  }
  size_t n = 0;
  for (const doc::ClipGenerated& c : r.clips) n += c.notes.size();
  ctx_.setStatus("Flattened " + std::to_string(n) + (n == 1 ? " note" : " notes") + " into the clips; the note device is off");
}

std::shared_ptr<const NotePreview> NotePreviews::latest(doc::TrackId track) const {
  const auto it = latest_.find(track.value);
  return it == latest_.end() ? nullptr : it->second.result;
}

doc::Snapshot NotePreviews::latestDoc(doc::TrackId track) const {
  const auto it = latest_.find(track.value);
  return it == latest_.end() ? nullptr : it->second.doc;
}

bool NotePreviews::waitIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(m_);
  return idle_.wait_for(lock, timeout, [&] { return pending_.empty() && flattens_.empty() && !busy_; });
}

uint64_t NotePreviews::computes() const noexcept {
  std::lock_guard lock(m_);
  return computes_;
}

bool flattenTrackNoteDevices(ViewContext& ctx, doc::TrackId track) {
  const doc::Snapshot snap = ctx.doc();
  if (!snap) return false;
  const doc::Track* t = doc::findTrackPtr(*snap, track);
  bool any = false;
  if (t != nullptr)
    for (const doc::DevicePtr& d : t->devices) any = any || (d->active && doc::isNoteDevice(*d));
  if (!any) {
    ctx.setStatus("Flatten to Clip: the track has no active note device");
    return false;
  }
  if (const std::string why = doc::ops::flattenRefusal(*snap, track); !why.empty()) {
    ctx.setStatus("Flatten to Clip: " + why);
    return false;
  }
  NotePreviews& np = ctx.notePreviews();
  if (np.flattening()) {
    ctx.setStatus("Flatten to Clip: a Flatten is still rendering");
    return false;
  }
  if (!np.flatten(snap, track)) {
    ctx.setStatus("Flatten to Clip: no note-device renderer (no playback engine)");
    return false;
  }
  ctx.setStatus("Flatten to Clip: rendering…");
  return true;
}

}  // namespace pq::views
