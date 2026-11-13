#pragma once

// MF.2: Standard MIDI Files into the project (docs/plans/DY.md "MF.2"): the arrangement's / a band lane's FileDrop
// (arrangement/file_drop.cpp: .mid with audio files in one drop), the browser's .mid items (loadClipItem, the browser's
// load). Reads each file, parses and resolves it (pq/document/midi_file.h), imports it (pq/document/midi_import.h
// ops::importMidi) and dispatches all of it as ONE undo step; the status line text names what was dropped.
//
// UI thread only (reads the files synchronously: a 10 MB file parses in ~20 ms).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/model.h"

namespace pq::views {

class ViewContext;

inline bool isMidiFilePath(std::string_view path) noexcept {
  auto endsWith = [&](std::string_view ext) {
    if (path.size() < ext.size()) return false;
    for (size_t i = 0; i < ext.size(); ++i) {
      char c = path[path.size() - ext.size() + i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != ext[i]) return false;
    }
    return true;
  };
  return endsWith(".mid") || endsWith(".midi");
}

struct MidiFilesImported {
  std::vector<doc::ClipRef> clips;  // the clips made (select them)
  std::string status;               // the status line's summary (what was imported, what was dropped or moved)
};

// Imports `paths` (MIDI files) at `at`, in order, as ONE undo step (`coalesceKey` ≠ 0: dispatched with
// Store::dispatchCoalescing under that key, so a drop's audio part can join the same step). The first track of the first
// file lands on `target` when it is a MIDI track; every other track is a new MIDI track after the last one placed
// (the first new one after `target`; invalid `target`: at the end). Files that cannot be read or hold no notes are
// named in the status; an error only when nothing was imported.
Result<MidiFilesImported> importMidiFiles(ViewContext& ctx, std::span<const std::string> paths, doc::TrackId target,
                                          Ticks at, uint64_t coalesceKey = 0);

}  // namespace pq::views
