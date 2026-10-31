#pragma once

// Drag sessions (T5.7; SPEC §9.1 "drag and drop between the browser, the arrangement and Explorer"): one payload moved
// by the pointer from a source to a drop target, inside the window or from outside it (files from Explorer). Both kinds
// go through the same target interface, so a browser drag and an Explorer drop of the same files behave alike.
//
//   source   a widget that holds the pointer capture (a press on a browser row) calls UiTree::beginDrag(payload) once
//            the pointer has moved far enough; from then on the tree routes pointer moves to drop targets instead of
//            the source, the release drops, Esc (or a lost capture) cancels. The source sees onCaptureLost at the end.
//   external the app maps an OS drag (an IDropTarget on the window, pq/app os_drop.h) to UiTree::dragEnter /
//            dragMove / dragExit / dragDrop. Explorer owns the drag loop then (Esc there is a dragExit here).
//   target   the deepest widget under the pointer whose dragOver() accepts the payload (effect ≠ None), asking
//            ancestors while widgets say None. dragOver is called on every move (cheap: hit testing and arithmetic,
//            plus whatever preview the target draws; it may start background work such as probing dropped files the
//            first time it sees a payload). dragLeave when the pointer moves to another target, leaves the window or the
//            drag is cancelled; drop() at the release instead (no dragLeave follows a drop).
//   scroll   while the pointer rests near the edge of a scrolling widget, the tree calls autoScrollDrag(pos, dt) on the
//            widget under the pointer and its ancestors once per frame; each scrolls its own axis (ScrollView: the edges
//            of its viewport) and returns whether it moved, which keeps frames coming until the pointer leaves the edge.
//   ghost    the tree paints a ghost next to the pointer (the payload's label in a small box, or the payload's own
//            painter) over everything; targets draw their own previews (the arrangement's ghost clips).
//
// UI thread only. Nothing here is RT-safe.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pq/ui/draw_list.h"
#include "pq/ui/input.h"

namespace pq::ui {

struct PaintContext;
struct IRect;

// What is dragged.
enum class DragKind : uint8_t {
  Files,         // files from Explorer or another program: `paths`
  BrowserItems,  // browser entries (T5.10): `paths` (their absolute paths) and `ids` (index entries)
  Clips,         // arrangement clips (T5.12 drags them out): `ids` = packed clip references of the source's choosing
  Device,        // a device from a chain (T5.11): `ids` = device ids
};

struct DragPayload {
  DragKind kind = DragKind::Files;
  std::vector<std::string> paths;  // absolute UTF-8 paths (Files, BrowserItems)
  std::vector<uint64_t> ids;       // per kind, see DragKind
  std::string label;               // what the ghost says ("Kick.wav", "3 files"); empty: no ghost label
  bool external = false;           // came from outside the window (an OS drag)
  // An optional ghost painter: draws into the given rect (device px, at the pointer's lower right, `ghostSize` logical
  // px). Null: the default ghost (the label in a small box).
  std::function<void(PaintContext&, const IRect&)> ghost;
  float ghostWidth = 0, ghostHeight = 0;  // logical px (the painter's box); 0: measured from the label
};

// What a drop here would do. None = refused (the cursor shows NotAllowed, an OS drag shows its "no" cursor).
enum class DropEffect : uint8_t { None, Copy, Move, Link };

struct DropFeedback {
  DropEffect effect = DropEffect::None;
  Cursor cursor = Cursor::Inherit;  // the pointer shape over the target (Inherit: Arrow when accepted)
};

}  // namespace pq::ui
