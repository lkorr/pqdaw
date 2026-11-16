// bug-live-shape piano roll: shape drags edit the document while they run (the user: "if im changing the shape of a
// note, bending it, applying vibrato etc and its actively playing it should be following along with whats changing
// in real time"). The contract is roll_state.h "live shape drags".
//
// Flow: dragTo updates the drag's preview as it always did and calls liveChanged() (a frame request on the note
// area); NoteAreaWidget::onFrame calls flushLive(), which builds the edit the release makes (liveEdit: the kind's own
// builder, on the press-time clip and snapshot) and dispatches it with DocumentStore::dispatchReplacing inside one
// store gesture — opened by the first edit that changes something, labelled as the release's undo step. Every
// dispatch replaces the one before (a drag dragged back to where it was pressed puts the document back exactly). The
// release (commitLive) dispatches the final state when the last frame did not, and commits; Esc, a lost capture and
// the view going (cancelDrag) cancel the gesture. While it is open refresh() keeps the press-time clip: the builders
// read it, and the preview the roll paints is exactly what was dispatched.
//
// Cost (UI thread, per frame whose drag state changed): the kind's builder (one record per edited note), the store's
// apply (one clip copy) and the listeners — the ProjectLink re-lowers that one clip and rebuilds the sequence image
// (PROGRESS bug-live-shape has the numbers) — and the roll itself nothing (refresh is held: no chunk, path or group
// cache is rebuilt per frame). A frame whose drag state equals the last dispatched one (liveKey) dispatches nothing;
// pointer moves between two frames coalesce into one dispatch.

#include <chrono>
#include <cmath>
#include <string>

#include "pq/document/commands.h"
#include "roll_widgets.h"

namespace pq::views::roll {

bool RollState::liveKind(const DragState& d) noexcept {
  switch (d.kind) {
    case DragKind::BendShape:
    case DragKind::Vibrato:
    case DragKind::VibratoCorner:
    case DragKind::PenVibrato:
    case DragKind::PenDraw:
    case DragKind::CurvePoint:
    case DragKind::CurveBow:
    case DragKind::WarpNew:
    case DragKind::WarpMove:
    case DragKind::WarpGlide: return true;
    // The end edge: the start stays, so the sounding note keeps sounding (a new note-off). A double-click's held add
    // joins the add's undo step on release (DocumentStore::dispatchCoalescing): not live.
    case DragKind::ResizeEnd: return d.bend || d.joinKey == 0;
    case DragKind::ResizeStart: return d.bend;  // an Alt bend leaves the edge where it is; a moved start cuts the note
    default: return false;
  }
}

void RollState::liveChanged() {
  if (!drag.moved || (!liveKind(drag) && !liveOpen_)) return;  // (a drag that left its live mode puts the document back)
  const float sc = (noteArea && noteArea->tree()) ? noteArea->tree()->scale() : 1.0f;
  if ((drag.kind == DragKind::PenDraw || drag.kind == DragKind::PenVibrato) && drag.travel < kMinPenStroke * sc) return;  // a click
  liveDirty_ = true;
  if (noteArea) noteArea->requestFrame();
}

RollState::LiveKey RollState::liveKey() const {
  LiveKey k;
  k.kind = drag.kind;
  k.records = curvePreview_;
  if (drag.kind == DragKind::WarpNew || drag.kind == DragKind::WarpMove || drag.kind == DragKind::WarpGlide) k.warps = drag.warps;
  k.dt = drag.dt;
  k.penLength = drag.penLength;
  k.bend = drag.bend;
  return k;
}

doc::ops::EditResult RollState::liveEdit(DragState& d, std::string& label) const {
  label = "Edit";
  if (!clip_) return noEdit();
  switch (d.kind) {
    case DragKind::BendShape: label = "Bend Shape"; return bendShapeEdit(d);
    case DragKind::Vibrato:
    case DragKind::VibratoCorner:
    case DragKind::PenVibrato: label = "Vibrato"; return vibratoEdit(d);
    case DragKind::PenDraw: label = "Draw Pitch Curve"; return penEdit(d);
    case DragKind::CurvePoint:
    case DragKind::CurveBow:
      label = d.kind == DragKind::CurveBow ? "Bend Segment" : d.curveAdded ? "Add Bend Point" : "Move Bend Point";
      return curveEdit(d.curve, d.curvePts);
    case DragKind::WarpNew:
    case DragKind::WarpMove:
    case DragKind::WarpGlide:
      label = d.kind == DragKind::WarpNew ? "Add Warp" : d.kind == DragKind::WarpMove ? "Move Warp" : "Warp Glide";
      return warpEdit(d);
    case DragKind::ResizeEnd:
    case DragKind::ResizeStart:
      if (d.bend) {
        label = d.kind == DragKind::ResizeStart ? "Bend In" : "Bend Out";
        return bendEdit(d);
      }
      label = "Resize Notes";
      if (d.kind == DragKind::ResizeEnd && d.dt != 0) return resizeEdit(doc::ops::NoteEdge::End, d.dt);
      return noEdit();
    default: return noEdit();
  }
}

void RollState::flushLive() {
  if (!liveDirty_) return;
  liveDirty_ = false;
  if (!clip_ || drag.kind == DragKind::None || !drag.moved) return;
  LiveKey key = liveKey();
  if (liveOpen_ && key == liveLast_) return;  // the same edit as the last frame's
  const auto t0 = std::chrono::steady_clock::now();
  std::string label;
  const doc::ops::EditResult e = liveKind(drag) ? liveEdit(drag, label) : doc::ops::EditResult(noEdit());
  if (!e) {  // the document keeps the last edit that was valid; the release says what is wrong
    ctx.setStatus(label + ": " + e.error().message);
    return;
  }
  doc::DocumentStore& st = ctx.store();
  if (!liveOpen_) {
    if (e->empty()) return;  // nothing to show yet: no gesture
    if (st.inGesture() || !st.beginGesture(label)) return;  // another gesture holds the store: the release edits
    liveOpen_ = true;  // (before the dispatch: its notification's refresh() holds the press-time clip)
  }
  if (Status s = st.dispatchReplacing(e->command); !s) {
    ctx.setStatus(label + ": " + s.error().message);
    return;
  }
  liveLast_ = std::move(key);
  ++liveStats_.dispatches;
  liveStats_.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void RollState::commitLive() {
  const LiveKey key = liveKey();
  DragState d = std::move(drag);
  drag = {};
  liveDirty_ = false;
  std::string label;
  const doc::ops::EditResult e = liveEdit(d, label);  // the final state (the last frame may not have dispatched it)
  doc::DocumentStore& st = ctx.store();
  bool ok = false;
  if (liveOpen_) {
    Status s = Ok{};
    if (e && !(key == liveLast_)) s = st.dispatchReplacing(e->command);
    if (!e || !s) {
      ctx.setStatus(label + ": " + (!e ? e.error().message : s.error().message));
      cancelLive();
    } else {
      liveOpen_ = false;
      liveLast_ = {};
      (void)st.commitGesture();  // ONE undo step (none when the drag ended where it began: the document is the press's)
      ok = !e->empty();
    }
  } else if (!e) {
    ctx.setStatus(label + ": " + e.error().message);
  } else if (!e->empty()) {  // never dispatched live (a click that adds a breakpoint; another gesture held the store)
    if (Status s = st.dispatch(e->command); !s) ctx.setStatus(label + ": " + s.error().message);
    else ok = true;
  }
  clearCurvePreview();
  (void)refresh();
  // What each kind's release says or remembers.
  switch (d.kind) {
    case DragKind::Vibrato:
      if (ok && d.vibDepth != 0.0) vibratoDepth_ = std::fabs(d.vibDepth);
      break;
    case DragKind::PenVibrato:
      if (ok) {
        vibratoDepth_ = d.vibDepth;
        ctx.setStatus(vibratoText(d.vibDepth, d.vibRate) + " (drag the ~ grip above the note's end to change it)");
      }
      break;
    case DragKind::WarpNew:
    case DragKind::WarpMove:
    case DragKind::WarpGlide:
      if (ok) {
        selectWarp({d.group, d.warpIndex});
        if (d.kind == DragKind::WarpNew)
          ctx.setStatus("Warp added: drag the marker to move it, the round grip for the glide's length; right-click it for its shape, Delete removes it");
      }
      break;
    case DragKind::ResizeEnd:
    case DragKind::ResizeStart:
      if (d.bend && d.bendPicks.empty() && e && e->empty()) {
        // (A note with no neighbour there bends from / to the dragged pitch: this one was not dragged up or down.)
        const bool in = d.kind == DragKind::ResizeStart;
        bool same = false;
        if (const doc::Note* pressed = findNote(d.pressed)) (void)bendNeighbour(*pressed, in ? -1 : 1, &same);
        ctx.setStatus(same ? (in ? "Bend In: the note before this one is at the same pitch; drag up or down to bend from another"
                                 : "Bend Out: the note after this one is at the same pitch; drag up or down to bend to another")
                           : (in ? "Bend In: no note before this one; drag up or down for the pitch it comes from"
                                 : "Bend Out: no note after this one; drag up or down for the pitch it goes to"));
      }
      break;
    default: break;
  }
  markAllDirty();
}

void RollState::cancelLive() {
  liveDirty_ = false;
  if (!liveOpen_) return;
  liveOpen_ = false;  // (before the cancel: its notification's refresh() takes the document again)
  liveLast_ = {};
  if (ctx.store().inGesture()) (void)ctx.store().cancelGesture();
  (void)refresh();
}

}  // namespace pq::views::roll
