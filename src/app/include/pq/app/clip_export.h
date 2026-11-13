#pragma once

// Audio clips as WAV files (T5.12): what a clip dragged out to Explorer carries, and what an Alt-drop of a clip on a
// browser folder writes. MF.3: note clips drag out as .mid files (pq/views/midi_export.h writeMidiClips, in
// ViewPrefs::midiExportMode). The audio is ProjectLink::renderClip's (the clip as it plays alone: gain, fades, transpose,
// reverse; no track devices), at the engine rate, as 32-bit float (default) or 24-bit PCM (ViewPrefs::dragOut24Bit).
//
// Not RT code (file I/O; the sample may be decoded into the pool first).

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/commands.h"
#include "pq/document/midi_export.h"
#include "pq/document/model.h"

namespace pq::app {

class ProjectLink;

// Clips longer than this render when a drop target first asks for the file (os_drag.h), shorter ones when the drag starts.
inline constexpr double kLazyRenderSeconds = 60.0;

// How long the clip plays (seconds at the project's tempo map); 0 for a missing clip.
double clipSeconds(const doc::Project& p, doc::ClipRef clip);
// A free path "<folder>/<clip name>.wav" ("<name> 2.wav", … when taken; characters Windows refuses become '_').
std::filesystem::path clipWavPath(const doc::Project& p, doc::ClipRef clip, const std::filesystem::path& folder);
// MF.3: the same with ".mid".
std::filesystem::path clipMidiPath(const doc::Project& p, doc::ClipRef clip, const std::filesystem::path& folder);
// Renders the clip and writes it to `path`. Errors: not an audio clip, a missing sample, I/O.
Status writeClipWav(ProjectLink& link, const doc::Project& p, doc::ClipRef clip, const std::filesystem::path& path,
                    bool pcm24);

// What a drag of clips out of the window carries (ShellApp::onDragOut, before the OLE drag): note clips written now as
// "<folder>/<clip>.mid" in `midiMode` (one file per clip); audio clips rendered now as WAV, or listed in `lazy` when
// longer than kLazyRenderSeconds (rendered when the drop target asks). `link` null: audio clips are refused.
struct ClipDragOut {
  std::vector<std::string> files;  // absolute UTF-8 paths written
  std::vector<doc::ClipRef> lazy;  // audio clips to render on demand
  std::string refused;             // why the last clip left out was (empty: none)
};
ClipDragOut dragOutClips(ProjectLink* link, const doc::Project& p, std::span<const doc::ClipRef> clips,
                         const std::filesystem::path& folder, bool pcm24, doc::MidiExportMode midiMode);

}  // namespace pq::app
