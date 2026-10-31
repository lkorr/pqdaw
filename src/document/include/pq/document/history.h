#pragma once

// Unlimited undo/redo as a list of snapshot pairs (SPEC §2: undo = swap snapshots; §9.4: unlimited, every edit
// incl. parameter changes, with a history panel). Undo and redo move a cursor and hand back a stored snapshot
// pointer, so they restore the *identical* snapshot in O(1) without re-applying anything.
//
// Memory: an entry costs its label, its ChangeSet and the nodes its edit created (the snapshot it shares with its
// neighbours is not duplicated: entry i's `after` is entry i+1's `before`).

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/change_set.h"
#include "pq/document/model.h"

namespace pq::doc {

class History {
public:
  struct Entry {
    std::string label;
    Snapshot before;
    Snapshot after;
    ChangeSet changes;
  };

  // Appends an entry after the cursor, discarding everything that could have been redone.
  void push(Entry e);

  bool canUndo() const noexcept { return cursor_ > 0; }
  bool canRedo() const noexcept { return cursor_ < entries_.size(); }
  // Moves the cursor back and returns the entry being undone (restore its `before`), or nullptr.
  const Entry* stepBack() noexcept { return canUndo() ? &entries_[--cursor_] : nullptr; }
  // Moves the cursor forward and returns the entry being redone (restore its `after`), or nullptr.
  const Entry* stepForward() noexcept { return canRedo() ? &entries_[cursor_++] : nullptr; }

  // For the undo panel: entries [0, cursor()) are applied, [cursor(), size) can be redone.
  const std::deque<Entry>& entries() const noexcept { return entries_; }
  size_t cursor() const noexcept { return cursor_; }
  std::string_view undoLabel() const noexcept { return canUndo() ? std::string_view(entries_[cursor_ - 1].label) : ""; }
  std::string_view redoLabel() const noexcept { return canRedo() ? std::string_view(entries_[cursor_].label) : ""; }

  void clear() noexcept;
  // T5.8 (DocumentStore::relocateSamples): every entry's snapshots replaced by f(snapshot).
  void remap(const std::function<Snapshot(const Snapshot&)>& f);
  // T4.4-fix1 (DocumentStore::dispatchCoalescing): the last entry, when it is the one undo would take next, now ends
  // at `after` and also touched `more`.
  void amendLast(Snapshot after, const ChangeSet& more);

private:
  // A deque, not a vector: a push never relocates existing entries, so a commit costs the same at entry 10 and
  // entry 10^5 (a vector's reallocation moved every entry: measured 0.5 ms / ~1 MB spikes at 10k entries).
  std::deque<Entry> entries_;
  size_t cursor_ = 0;
};

}  // namespace pq::doc
