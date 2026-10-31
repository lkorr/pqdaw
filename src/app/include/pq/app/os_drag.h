#pragma once

// Drag out of the window (T5.12; docs/plans/M5.md T5.12): an OLE drag source whose IDataObject offers CF_HDROP, so a
// drop on Explorer (or the desktop, or another program that takes files) copies the files.
//
// The files may be produced lazily: `FileProvider` runs the first time the drop target asks for the data (GetData;
// Explorer asks when the drag enters it), so a long clip is rendered only when someone wants it (the plan's "clips
// longer than 60 s render lazily in GetData"). QueryGetData and EnumFormatEtc answer without running it.
//
// Threading: DoDragDrop runs a modal loop on the UI thread (it pumps the window's messages until the drop or Esc). The
// audio thread is independent of it: playback goes on; the UI (meters, playhead) resumes after the loop returns.
//
// Not RT code. Windows only.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "pq/ui/drag.h"

namespace pq::app {

// The files to drag (absolute UTF-8 paths). Empty: the data is not available (GetData fails).
using FileProvider = std::function<std::vector<std::string>()>;

// A new IDataObject (refcount 1; the caller Releases it) offering CF_HDROP of `provider`'s files, produced on the first
// GetData (then kept). `calls` (optional) counts how often the provider ran (tests).
void* makeFileDataObject(FileProvider provider, int* calls = nullptr);
// Releases a COM object returned above.
void releaseComObject(void* object);
// M5 review H9: whether an IDataObject* is one this process is dragging out (our data objects also offer a private
// "pqdaw.DragOut" format holding the process id, answered without producing the files). The app's own drop target
// refuses those: a clip dragged out of the window and back in would otherwise land as a copy of its drag-out temp WAV
// (deleted at exit) instead of the move, and hovering would force the lazy render of a long clip. Another pqdaw
// instance's drags are accepted.
bool isOwnDragData(void* dataObject);

// Runs the OLE drag (modal: returns after the drop or a cancel). OLE must be initialised on this thread (SDL does).
// Returns Copy when a target took the files, None when the drag was cancelled or refused.
ui::DropEffect dragFilesOut(FileProvider provider);

// %TEMP%\pqdaw\dragout (created on demand): where rendered drag files go. cleanDragOutFolder() deletes what is in it
// (the app calls it at start and at exit; files still in use stay).
std::filesystem::path dragOutFolder();
void cleanDragOutFolder();

}  // namespace pq::app
