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

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/change_set.h"
#include "pq/document/commands.h"
#include "pq/document/history.h"
#include "pq/document/model.h"

namespace pq::doc {

class DeviceLibrary;

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
// TX.resize (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"): a device block's uniform scale (its face,
// panel or plain block and everything on it in proportion; S-drag in the view), kMinPatchScale … kMaxPatchScale, 1 = its
// own size. Bands keep 1. Saved as the entry's optional "scale" (written only when not 1); like the nudge, not undoable.
inline constexpr float kMinPatchScale = 0.05f;  // TX.block-drag (from 0.5)
inline constexpr float kMaxPatchScale = 3.0f;
// TX.orbit-ports (SPEC §15 2026-11-03 "orbiting ports, a round VCA, Delete on a cable"): a device block's main input /
// output PINNED on its outline at an angle (radians in [0, 2π), atan2 with y down: 0 = right, π/2 = down), the user's
// drag of the port along the block's edge; unpinned ports follow their cables. Saved as the entry's optional "pinIn" /
// "pinOut" (degrees); view state like the nudge (not undoable).
// TX.mixer-look (bug report 2026-11-03: the mixer "positioned always at the top right … though you should be able to
// move it"): a track's entry also keeps where the user PLACED its mixer (`mixerPlaced`; mixerX / mixerY: the mixer's
// top-left from its band's top-left, world units, so it travels with its band and no re-layout of the band's other
// blocks moves it). Unplaced, the layout puts the mixer at the band's top right. Saved as the entry's optional "mixerX" /
// "mixerY"; view state like the nudge (not undoable; Reset Layout clears it).
// TL.1 (SPEC §15 2026-11-08 "track lanes in the circuit view"): a track's entry also keeps its band's LANE HEIGHT (world
// units, kMinPatchLaneH … kMaxPatchLaneH; 0 = the view's default; dragging the lane's bottom edge) and whether its band's
// circuit is FOLDED (collapsed: the band shows its title strip and its lane only). Saved as the entry's optional "laneH" /
// "folded"; view state like the nudge (not undoable).
// BR.tuning-block: a MIDI track's entry also keeps whether its band's TUNING BLOCK (the view of the track's tuning, hitched
// outside its band's left edge) is OPENED to its MOS circle, and (BR.tuning-outside) whether that opened block is grown
// to the full WHEEL (the region chart left of the circle). Saved as the entry's optional "tuningOpen" / "tuningWheel";
// view state like the nudge (not undoable).
inline constexpr float kMinPatchLaneH = 16.0f;
inline constexpr float kMaxPatchLaneH = 1024.0f;
struct PatchNudge {
  float dx = 0.0f;
  float dy = 0.0f;
  float scale = 1.0f;
  bool inPinned = false, outPinned = false;
  float inAngle = 0.0f, outAngle = 0.0f;
  bool mixerPlaced = false;
  float mixerX = 0.0f, mixerY = 0.0f;
  float laneH = 0.0f;   // TL.1
  bool folded = false;  // TL.1
  bool tuningOpen = false;   // BR.tuning-block
  bool tuningWheel = false;  // BR.tuning-outside (only while tuningOpen)
  bool empty() const noexcept {
    return dx == 0.0f && dy == 0.0f && scale == 1.0f && !inPinned && !outPinned && !mixerPlaced && laneH == 0.0f && !folded &&
           !tuningOpen && !tuningWheel;
  }
  friend bool operator==(const PatchNudge&, const PatchNudge&) noexcept = default;
};
// TX.orbit-ports: a pinned angle as saved (degrees in [0, 360), whole hundredths) and back; quantised angles survive a
// save and load exactly (the view pins quantised angles).
inline float patchPinDegrees(float radians) noexcept {
  constexpr float kDeg = 180.0f / 3.14159265358979f;
  float d = std::round(radians * kDeg * 100.0f) / 100.0f;
  d = std::fmod(d, 360.0f);
  return d < 0.0f ? d + 360.0f : d;
}
inline float patchPinRadians(float degrees) noexcept {
  float d = std::fmod(degrees, 360.0f);
  if (d < 0.0f) d += 360.0f;
  return d * (3.14159265358979f / 180.0f);
}
inline float patchPinQuantise(float radians) noexcept { return patchPinRadians(patchPinDegrees(radians)); }
// GU.5 (docs/plans/GU.md "Panels and the panel window"): a group's PANEL WINDOW in the project patch view — open or not,
// minimised (its title bar only), in edit or play mode, and where it floats over the canvas: its top-left from the
// canvas's top-left and its size, logical px (w, h 0: not placed yet, the view's default). View state like a nudge
// (saved with the project as the view's optional "panels", not undoable). Key: the group's device id.
struct PanelWindowView {
  bool open = false, minimised = false, edit = false;
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
  bool empty() const noexcept { return !open && !minimised && !edit && w == 0.0f && h == 0.0f && x == 0.0f && y == 0.0f; }
  friend bool operator==(const PanelWindowView&, const PanelWindowView&) noexcept = default;
};
inline constexpr float kMaxPanelWindowCoord = 1.0e5f;  // |x|, |y|, w, h (logical px) the reader accepts
struct ViewState {
  std::unordered_map<TrackId, TrackView> tracks;
  std::unordered_map<LaneId, LaneView> lanes;
  std::unordered_map<uint32_t, PatchNudge> patchNudges;  // T4.13: by raw track or device id (TX.resize: + scale)
  std::unordered_map<uint32_t, PanelWindowView> panelWindows;  // GU.5: by group (device) id
};

enum class StoreEventKind : uint8_t {
  Dispatch,       // a command changed the document (inside or outside a gesture)
  Undo,
  Redo,
  GestureCommit,  // the document did not change (before == after); `changes` = the whole gesture's union
  GestureCancel,  // the document went back to the pre-gesture snapshot; `changes` = what the gesture had touched
  Reset,          // a new document was loaded; `changes` is empty (listeners rebuild everything)
  SamplesRelocated,  // T5.8: sample paths changed outside the history (relocateSamples); `changes`: Meta + samples
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
  // GU.1: the device library edits made in units resolve their params' curves with (ops::setDeviceParamValue: project
  // builders, scripts; the views set it: ViewContext::setDeviceLibrary). Null: none (the store never reads it itself).
  void setDeviceLibrary(const DeviceLibrary* lib) noexcept { devices_ = lib; }
  const DeviceLibrary* deviceLibrary() const noexcept { return devices_; }
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

  // T5.8: replaces sample table entries (matched by id) in the current snapshot AND every snapshot of the history (and
  // an open gesture's base), so a relink or a Collect All and Save is not an undo step and no undo brings the old
  // path back (samples.h). Entries a snapshot does not hold are skipped there. No history entry; listeners hear
  // SamplesRelocated. Snapshots shared between entries stay shared.
  void relocateSamples(std::span<const SampleRef> updated);

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
  const DeviceLibrary* devices_ = nullptr;  // GU.1
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
