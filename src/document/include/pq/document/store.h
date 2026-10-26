#pragma once

// DocumentStore: the UI thread's owner of the document (T3.4). Holds the current snapshot, applies commands,
// records undo history, coalesces gestures and notifies listeners with the ChangeSet of every change.
//
// Gestures: a drag or knob turn dispatches many commands between beginGesture() and commitGesture(). Each one
// updates the current snapshot and notifies listeners at once (live preview: the engine gets param writes during
// the drag), but only commitGesture() records history, as ONE entry from the snapshot before the gesture to the
// last one. cancelGesture() (Esc) restores the snapshot from before the gesture and leaves no history.
//
// Not thread-safe: UI thread only. The engine sees the document only through the plain-data images T3.16/T3.17
// build from snapshots.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/change_set.h"
#include "pq/document/commands.h"
#include "pq/document/history.h"
#include "pq/document/model.h"

namespace pq::doc {

// View state that is saved with the project but is not part of undo (Q15: fold, heights, like Ableton; selection,
// zoom and scroll stay in the views). Keyed by id, so it survives undo/redo of the entities it describes.
struct TrackView {
  bool folded = false;
  int32_t height = 0;  // px; 0 = the theme's default
};
struct LaneView {
  bool visible = true;
  int32_t height = 0;
};
// T4.13: a manual nudge of the project patch view's automatic layout (QUESTIONS Q129 item 7): world units (logical px
// at zoom 1) added to where the layout puts a band (key: its track id) or a device block (key: its device id).
struct PatchNudge {
  float dx = 0.0f;
  float dy = 0.0f;
  friend bool operator==(const PatchNudge&, const PatchNudge&) noexcept = default;
};
struct ViewState {
  std::unordered_map<TrackId, TrackView> tracks;
  std::unordered_map<LaneId, LaneView> lanes;
  std::unordered_map<uint32_t, PatchNudge> patchNudges;  // T4.13: by raw track or device id
};

enum class StoreEventKind : uint8_t {
  Dispatch,       // a command changed the document (inside or outside a gesture)
  Undo,
  Redo,
  GestureCommit,  // the document did not change (before == after); `changes` = the whole gesture's union
  GestureCancel,  // the document went back to the pre-gesture snapshot; `changes` = what the gesture had touched
  Reset,          // a new document was loaded; `changes` is empty (listeners rebuild everything)
};

struct StoreEvent {
  StoreEventKind kind;
  const Snapshot& before;
  const Snapshot& after;
  const ChangeSet& changes;
};

using Listener = std::function<void(const StoreEvent&)>;
using ListenerId = uint32_t;

class DocumentStore {
public:
  // A new default project (makeDefaultProject).
  DocumentStore();
  // An existing project (e.g. loaded by T3.5); the id source continues after its largest id.
  explicit DocumentStore(Snapshot initial);
  DocumentStore(const DocumentStore&) = delete;
  DocumentStore& operator=(const DocumentStore&) = delete;

  const Snapshot& current() const noexcept { return current_; }
  IdSource& ids() noexcept { return ids_; }
  ViewState& view() noexcept { return view_; }
  const ViewState& view() const noexcept { return view_; }
  const History& history() const noexcept { return history_; }

  // Applies a command. A no-op command changes nothing (no history entry, no notification). On error the document
  // is unchanged. lastChanges() then describes this dispatch (empty for a no-op or an error).
  Status dispatch(const Command& c);
  // T4.4-fix1: as dispatch(), but a change dispatched with the same non-zero `key` as the one that recorded the latest
  // history entry — with nothing recorded, undone or redone since and no gesture open — joins that entry (its `after`
  // and ChangeSet) instead of adding one, still notifying every listener (each change plays at once). The circuit
  // editor's typing run in a device's text is one undo step this way. Any other dispatch, gesture, undo or redo ends the
  // run.
  Status dispatchCoalescing(const Command& c, uint64_t key);
  const ChangeSet& lastChanges() const noexcept { return *last_; }

  // Gestures (see the file comment). An empty label takes the first dispatched command's label.
  Status beginGesture(std::string label = {});
  Status commitGesture();
  Status cancelGesture();
  bool inGesture() const noexcept { return gesture_.has_value(); }

  // Undo/redo restore the stored snapshot pointer. Both return false when there is nothing to do or a gesture is
  // in progress (commit or cancel it first).
  bool undo();
  bool redo();

  // Replaces the document (open/new) and clears the history; ids continue after the new document's largest id.
  void reset(Snapshot s);

  ListenerId subscribe(Listener l);
  void unsubscribe(ListenerId id) noexcept;

private:
  struct Gesture {
    std::string label;
    Snapshot base;
    ChangeSet changes;
  };

  void notify(StoreEventKind kind, const Snapshot& before, const Snapshot& after, const ChangeSet& changes);

  IdSource ids_;  // before current_: the default constructor draws the default project's ids from it
  Snapshot current_;
  ViewState view_;
  History history_;
  std::optional<Gesture> gesture_;
  uint64_t coalesceKey_ = 0;         // dispatchCoalescing: the key of the run that recorded entry coalesceAt_ − 1
  size_t coalesceAt_ = 0;
  ChangeSet step_;                   // scratch for each dispatch
  const ChangeSet* last_ = &empty_;  // what lastChanges() returns
  ChangeSet empty_;
  // Listeners live behind a pointer, so a call in progress survives the vector growing (a listener subscribing
  // another) and the listener unsubscribing itself (only flagged; entries are erased when no notification is running,
  // M3 review M11: compacting inside a notification shifted the running loop's index past a listener).
  struct Entry {
    ListenerId id;
    std::unique_ptr<Listener> fn;
    bool live = true;
  };
  std::vector<Entry> listeners_;
  ListenerId nextListener_ = 1;
  uint32_t notifyDepth_ = 0;
  bool removedWhileNotifying_ = false;
};

}  // namespace pq::doc
