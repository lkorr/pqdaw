#pragma once

// MF.3: note clips → a Standard MIDI File (docs/plans/DY.md "MF.3"; the bytes: pq/document/midi_export.h exportMidi).
// The arrangement's clip menu "Export MIDI" > MPE… / 12edo… / Degrees… asks where to save (FileDialogs::
// chooseMidiFileToSave) and writes the selected note clips as one file; the app's drag-out writes one "<clip>.mid" per
// note clip in ViewPrefs::midiExportMode (the last mode chosen in the menu; default MPE).
//
// UI thread only (the export is linear in the notes; the write is one small file).

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/commands.h"
#include "pq/document/midi_export.h"
#include "pq/document/model.h"

namespace pq::views {

class ViewContext;

// The note clips among `clips` (audio clips and missing ones left out), in order.
std::vector<doc::ClipRef> noteClipsOf(const doc::Project& p, std::span<const doc::ClipRef> clips);

// Exports `clips` in `mode` to `pathUtf8` (".mid" appended when it has no extension; the folder is created). Returns the
// status line's text (doc::describeMidiExport). Errors: exportMidi's, I/O.
Result<std::string> writeMidiClips(const doc::Project& p, std::span<const doc::ClipRef> clips, doc::MidiExportMode mode,
                                   std::string_view pathUtf8);

// The menu's action: remembers `mode` (ViewPrefs::midiExportMode), asks for the file (suggested "<first clip>.mid") and
// writes the note clips among `clips` there, from the document as it is when the dialog answers; the status line says
// what was written (or why not). No note clip: the status line says so, no dialog.
void exportMidiClips(ViewContext& ctx, std::vector<doc::ClipRef> clips, doc::MidiExportMode mode);

}  // namespace pq::views
