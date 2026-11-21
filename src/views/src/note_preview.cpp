// AP.7: the roll preview's worker (pq/views/note_preview.h).
#include "pq/views/note_preview.h"

#include <algorithm>
#include <string>
#include <utility>

#include "pq/views/view_context.h"

namespace pq::views {

NotePreviews::NotePreviews(ViewContext& ctx) : ctx_(ctx) {
  worker_ = std::thread([this] { run(); });
  workerId_ = worker_.get_id();
}

NotePreviews::~NotePreviews() {
  *alive_ = false;
  {
    std::lock_guard lock(m_);
    quit_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void NotePreviews::setSource(NotePreviewFn fn) { fn_ = std::move(fn); }

void NotePreviews::request(doc::Snapshot doc, doc::TrackId track, Ticks until) {
  if (!fn_ || !doc) return;
  {
    std::lock_guard lock(m_);
    Job j{std::move(doc), track, until};
    const auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Job& x) { return x.track == track; });
    if (it != pending_.end()) *it = std::move(j);
    else pending_.push_back(std::move(j));
    last_ = std::chrono::steady_clock::now();
  }
  cv_.notify_all();
}

void NotePreviews::run() {
  std::unique_lock lock(m_);
  for (;;) {
    cv_.wait(lock, [&] { return quit_ || !pending_.empty(); });
    if (quit_) return;
    // Debounce: until no request came for kDebounce.
    while (!quit_ && std::chrono::steady_clock::now() - last_ < kDebounce)
      cv_.wait_until(lock, last_ + kDebounce);
    if (quit_) return;
    Job j = std::move(pending_.front());
    pending_.erase(pending_.begin());
    busy_ = true;
    lock.unlock();
    Result<NotePreview> r = fn_(*j.doc, j.track, j.until);
    std::shared_ptr<const NotePreview> res =
        r ? std::make_shared<const NotePreview>(std::move(r.value())) : std::make_shared<const NotePreview>();
    lock.lock();
    ++computes_;
    done_.push_back({std::move(j.doc), j.track, std::move(res)});
    busy_ = false;
    if (pending_.empty()) idle_.notify_all();
    if (const ViewContext::UiPoster& post = ctx_.uiPoster()) {
      std::weak_ptr<bool> alive = alive_;
      lock.unlock();
      post([this, alive] {
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
  for (Done& d : got) latest_[d.track.value] = std::move(d);
  ++serial_;
  ctx_.notePreviewsChanged();
  return true;
}

std::shared_ptr<const NotePreview> NotePreviews::latest(doc::TrackId track) const {
  const auto it = latest_.find(track.value);
  return it == latest_.end() ? nullptr : it->second.result;
}

doc::Snapshot NotePreviews::latestDoc(doc::TrackId track) const {
  const auto it = latest_.find(track.value);
  return it == latest_.end() ? nullptr : it->second.doc;
}

Result<NotePreview> NotePreviews::computeNow(const doc::Project& doc, doc::TrackId track, Ticks until) const {
  if (!fn_) return Error{ErrorCode::InvalidArgument, "no note-device renderer (no playback engine)", {}};
  return fn_(doc, track, until);
}

bool NotePreviews::waitIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(m_);
  return idle_.wait_for(lock, timeout, [&] { return pending_.empty() && !busy_; });
}

uint64_t NotePreviews::computes() const noexcept {
  std::lock_guard lock(m_);
  return computes_;
}


bool flattenTrackNoteDevices(ViewContext& ctx, doc::TrackId track) {
  const doc::Snapshot snap = ctx.doc();
  if (!snap) return false;
  Result<NotePreview> r = ctx.notePreviews().computeNow(*snap, track, doc::kMaxTicks);
  if (!r) {
    ctx.setStatus("Flatten to Clip: " + r.error().message);
    return false;
  }
  if (!r->any) {
    ctx.setStatus("Flatten to Clip: the track has no active note device");
    return false;
  }
  doc::ops::EditResult e = doc::ops::flattenNoteStages(*snap, ctx.store().ids(), track, r->clips);
  if (!e) {
    ctx.setStatus("Flatten to Clip: " + e.error().message);
    return false;
  }
  if (Status st = ctx.store().dispatch(e->command); !st) {
    ctx.setStatus("Flatten to Clip: " + st.error().message);
    return false;
  }
  size_t n = 0;
  for (const doc::ClipGenerated& c : r->clips) n += c.notes.size();
  ctx.setStatus("Flattened " + std::to_string(n) + (n == 1 ? " note" : " notes") + " into the clip; the note device is off");
  return true;
}

}  // namespace pq::views
