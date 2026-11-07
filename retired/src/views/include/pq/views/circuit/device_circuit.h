#pragma once

// "Open in Circuit View" on a device of the project (T4.4-fix1; SPEC §15 2026-10-26: the circuit editor works on the
// active project, not on loose files).
//
// The editor (circuit_editor.h, with a CircuitEditor::Host) shows the device's patch file — its own text for a user
// device (model.h UserPatch), else the stock file's — at the device's patch, with the NodeIds the project plays (the
// library's resolution / the stock parse), so its helper patches open and imports stay read-only as in the
// stand-alone editor. Imports resolve relative to the stock file (the stdlib).
//   edits      every edit whose text lowers is a document command (doc::ops::editDevicePatch, carrying the editor's
//              parse as the ids to play): the first edit of a stock device FORKS it — the instance's own copy, stored
//              in the project; the stock file and every other instance stay as they are. A typing run in the text pane
//              is one undo step (DocumentStore::dispatchCoalescing); a circuit edit is one step. The playback link
//              recompiles only that track's unit, keeping state (the ids carried).
//   undo       Ctrl+Z / Ctrl+Shift+Z here are the document's undo / redo (one history with the Ableton view); a change
//              of the device's text from anywhere (undo, redo, Revert to Stock) shows in the editor at once
//              (PatchEditor::resetText). A text that does not parse stays in the editor only (the project keeps its
//              last good text) until it parses.
//   gone       the device removed (deleted, its track deleted, the undo of its creation): the editor closes.
//   playback   the project plays the device (Play is hidden). `player` (ViewContext::devicePlayers: the app's link
//              to the project unit) carries probes and cost through that unit; without one the editor only edits.
// UI thread only.

#include "pq/document/ids.h"
#include "pq/views/circuit/circuit_editor.h"

namespace pq::views {
class ViewContext;
}

namespace pq::views::circuit {

// Opens the editor over the whole window of `from`'s tree on `device` of ctx's document. nullptr (and a status line)
// when the device, the device library or the overlay layer is missing.
CircuitEditor* openDeviceInCircuitView(ui::Widget& from, ViewContext& ctx, doc::DeviceId device);

// The title the editor shows for `device`: "<device name> · <track name>", "(edited)" for a user device.
std::string deviceEditorTitle(const ViewContext& ctx, doc::DeviceId device);

// T4.15: the editor's Inactive toggle starts as ctx's view prefs say and a flip of it is remembered there (ctx must
// outlive the editor, as the shell's context does).
void followViewPrefs(CircuitEditor& editor, ViewContext& ctx);

}  // namespace pq::views::circuit
