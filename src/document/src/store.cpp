#include "pq/document/store.h"

#include <algorithm>
#include <unordered_map>

#include "pq/document/history.h"

namespace pq::doc {

// ---- History ---------------------------------------------------------------------------------------------------

void History::push(Entry e) {
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(cursor_), entries_.end());
  entries_.push_back(std::move(e));
  cursor_ = entries_.size();
}

void History::amendLast(Snapshot after, const ChangeSet& more) {
  if (entries_.empty() || cursor_ != entries_.size()) return;
  Entry& e = entries_.back();
  e.after = std::move(after);
  e.changes.merge(more);
  e.changes.normalize();
}

void History::remap(const std::function<Snapshot(const Snapshot&)>& f) {
  for (Entry& e : entries_) {
    e.before = f(e.before);
    e.after = f(e.after);
  }
}

void History::clear() noexcept {
  entries_.clear();
  cursor_ = 0;
}

// ---- DocumentStore ---------------------------------------------------------------------------------------------

DocumentStore::DocumentStore() : current_(makeDefaultProject(ids_)) {}

DocumentStore::DocumentStore(Snapshot initial) : current_(std::move(initial)) {
  ids_.reserveThrough(maxEntityId(*current_));
}

Status DocumentStore::dispatchCoalescing(const Command& c, uint64_t key) {
  const bool join = key != 0 && key == coalesceKey_ && !gesture_ && history_.cursor() == coalesceAt_ &&
                    coalesceAt_ == history_.entries().size() && coalesceAt_ > 0;
  if (!join) {
    Status s = dispatch(c);
    if (s && key != 0 && !gesture_ && history_.cursor() == history_.entries().size() && history_.cursor() > 0 &&
        last_ == &history_.entries().back().changes) {  // it recorded an entry: later ones with `key` may join it
      coalesceKey_ = key;
      coalesceAt_ = history_.cursor();
    }
    return s;
  }
  step_.clear();
  last_ = &empty_;
  Result<Snapshot> r = apply(current_, c, step_);
  if (!r) return std::move(r).error();
  Snapshot next = std::move(r).value();
  if (next == current_) return Ok{};
  Snapshot before = std::move(current_);
  current_ = std::move(next);
  step_.normalize();
  history_.amendLast(current_, step_);
  last_ = &step_;
  notify(StoreEventKind::Dispatch, before, current_, step_);
  return Ok{};
}

Status DocumentStore::dispatch(const Command& c) {
  coalesceKey_ = 0;
  step_.clear();
  last_ = &empty_;
  Result<Snapshot> r = apply(current_, c, step_);
  if (!r) return std::move(r).error();
  Snapshot next = std::move(r).value();
  if (next == current_) return Ok{};
  Snapshot before = std::move(current_);
  current_ = std::move(next);
  if (gesture_) {
    if (gesture_->label.empty()) gesture_->label = std::string(label(c));
    gesture_->changes.merge(step_);
    last_ = &step_;
    notify(StoreEventKind::Dispatch, before, current_, step_);
  } else {
    history_.push(History::Entry{std::string(label(c)), before, current_, std::move(step_)});
    step_.clear();  // moved-from: make it a valid empty set again
    last_ = &history_.entries().back().changes;
    notify(StoreEventKind::Dispatch, before, current_, *last_);
  }
  return Ok{};
}

Status DocumentStore::beginGesture(std::string label) {
  coalesceKey_ = 0;
  if (gesture_) return Error{ErrorCode::InvalidArgument, "a gesture is already in progress", {}};
  gesture_ = Gesture{std::move(label), current_, {}};
  return Ok{};
}

Status DocumentStore::commitGesture() {
  if (!gesture_) return Error{ErrorCode::InvalidArgument, "no gesture in progress", {}};
  Gesture g = std::move(*gesture_);
  gesture_.reset();
  last_ = &empty_;
  if (current_ == g.base) return Ok{};  // nothing changed (or changed and changed back to the same pointer)
  g.changes.normalize();
  history_.push(History::Entry{g.label.empty() ? std::string("Edit") : std::move(g.label), g.base, current_,
                               std::move(g.changes)});
  last_ = &history_.entries().back().changes;
  notify(StoreEventKind::GestureCommit, current_, current_, *last_);
  return Ok{};
}

Status DocumentStore::cancelGesture() {
  if (!gesture_) return Error{ErrorCode::InvalidArgument, "no gesture in progress", {}};
  Gesture g = std::move(*gesture_);
  gesture_.reset();
  last_ = &empty_;
  if (current_ == g.base) return Ok{};
  g.changes.normalize();
  step_ = std::move(g.changes);
  last_ = &step_;
  Snapshot before = std::move(current_);
  current_ = std::move(g.base);
  notify(StoreEventKind::GestureCancel, before, current_, step_);
  return Ok{};
}

bool DocumentStore::undo() {
  coalesceKey_ = 0;
  if (gesture_) return false;
  const History::Entry* e = history_.stepBack();
  if (!e) return false;
  Snapshot before = std::move(current_);
  current_ = e->before;
  last_ = &e->changes;
  notify(StoreEventKind::Undo, before, current_, e->changes);
  return true;
}

bool DocumentStore::redo() {
  coalesceKey_ = 0;
  if (gesture_) return false;
  const History::Entry* e = history_.stepForward();
  if (!e) return false;
  Snapshot before = std::move(current_);
  current_ = e->after;
  last_ = &e->changes;
  notify(StoreEventKind::Redo, before, current_, e->changes);
  return true;
}

void DocumentStore::reset(Snapshot s) {
  coalesceKey_ = 0;
  gesture_.reset();
  history_.clear();
  last_ = &empty_;
  ids_.reserveThrough(maxEntityId(*s));
  Snapshot before = std::move(current_);
  current_ = std::move(s);
  notify(StoreEventKind::Reset, before, current_, empty_);
}

void DocumentStore::relocateSamples(std::span<const SampleRef> updated) {
  if (updated.empty()) return;
  // Old snapshot → new one, so entries sharing a snapshot (entry i's after = entry i + 1's before) keep sharing it.
  std::unordered_map<const Project*, Snapshot> done;
  auto remap = [&](const Snapshot& s) -> Snapshot {
    if (!s) return s;
    if (const auto it = done.find(s.get()); it != done.end()) return it->second;
    Snapshot out = s;
    if (s->samples) {
      SampleList list = *s->samples;
      bool changed = false;
      for (const SampleRef& u : updated)
        for (SampleRef& r : list)
          if (r.id == u.id && !(r == u)) {
            r = u;
            changed = true;
          }
      if (changed) {
        auto np = std::make_shared<Project>(*s);
        np->samples = std::make_shared<const SampleList>(std::move(list));
        out = std::move(np);
      }
    }
    done.emplace(s.get(), out);
    return out;
  };
  Snapshot before = current_;
  current_ = remap(current_);
  history_.remap(remap);
  if (gesture_) gesture_->base = remap(gesture_->base);
  if (current_ == before) return;
  // Its own ChangeSet: a relocation may run inside another notification (a listener resolving a dropped sample), whose
  // event still refers to step_; lastChanges() keeps describing the last dispatch.
  ChangeSet cs;
  cs.mark(kChangeMeta);
  for (const SampleRef& u : updated) cs.samples.push_back(u.id);
  cs.normalize();
  notify(StoreEventKind::SamplesRelocated, before, current_, cs);
}

ListenerId DocumentStore::subscribe(Listener l) {
  const ListenerId id = nextListener_++;
  listeners_.push_back({id, std::make_unique<Listener>(std::move(l)), true});
  return id;
}

void DocumentStore::unsubscribe(ListenerId id) noexcept {
  for (size_t i = 0; i < listeners_.size(); ++i)
    if (listeners_[i].id == id) {
      if (notifyDepth_ > 0) {  // a notification may be inside this listener or about to reach an index past it
        listeners_[i].live = false;
        removedWhileNotifying_ = true;
      } else {
        listeners_.erase(listeners_.begin() + static_cast<std::ptrdiff_t>(i));
      }
      return;
    }
}

void DocumentStore::notify(StoreEventKind kind, const Snapshot& before, const Snapshot& after,
                           const ChangeSet& changes) {
  if (listeners_.empty()) return;
  const StoreEvent ev{kind, before, after, changes};
  // Index loop over a vector that may grow (a listener subscribing another: it hears this event too, as before) or
  // have entries flagged dead; nothing is erased until the outermost notification ends.
  ++notifyDepth_;
  for (size_t i = 0; i < listeners_.size(); ++i)
    if (listeners_[i].live) {
      Listener* fn = listeners_[i].fn.get();  // stable while the vector reallocates
      (*fn)(ev);
    }
  if (--notifyDepth_ == 0 && removedWhileNotifying_) {
    removedWhileNotifying_ = false;
    std::erase_if(listeners_, [](const Entry& e) { return !e.live; });
  }
}

}  // namespace pq::doc
