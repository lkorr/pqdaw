#pragma once

// Audio clips as WAV files (T5.12): what a clip dragged out to Explorer carries, and what an Alt-drop of a clip on a
// browser folder writes. The audio is ProjectLink::renderClip's (the clip as it plays alone: gain, fades, transpose,
// reverse; no track devices), at the engine rate, as 32-bit float (default) or 24-bit PCM (ViewPrefs::dragOut24Bit).
//
// Not RT code (file I/O; the sample may be decoded into the pool first).

#include <filesystem>
#include <string>

#include "pq/core/result.h"
#include "pq/document/commands.h"
#include "pq/document/model.h"

namespace pq::app {

class ProjectLink;

// Clips longer than this render when a drop target first asks for the file (os_drag.h), shorter ones when the drag starts.
inline constexpr double kLazyRenderSeconds = 60.0;

// How long the clip plays (seconds at the project's tempo map); 0 for a missing clip.
double clipSeconds(const doc::Project& p, doc::ClipRef clip);
// A free path "<folder>/<clip name>.wav" ("<name> 2.wav", … when taken; characters Windows refuses become '_').
std::filesystem::path clipWavPath(const doc::Project& p, doc::ClipRef clip, const std::filesystem::path& folder);
// Renders the clip and writes it to `path`. Errors: not an audio clip, a missing sample, I/O.
Status writeClipWav(ProjectLink& link, const doc::Project& p, doc::ClipRef clip, const std::filesystem::path& path,
                    bool pcm24);

}  // namespace pq::app
