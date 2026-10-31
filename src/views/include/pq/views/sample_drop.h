#pragma once

// Dropping audio files into the arrangement (T5.7; docs/plans/M5.md "Decisions": multi-file drops). The rules:
//   - onto an audio track: the first file lands there at the drop tick (overwriting what lies under it, Live's drop),
//     each further file on a new audio track inserted after it (stems: one track per file);
//   - onto a MIDI or group track: a new audio track after it for each file;
//   - below the last track: new audio tracks at the end of the top level;
//   - Ctrl held: all files one after another on ONE track (the target audio track, or one new track);
//   - return tracks and the master take no clips (refused).
// New tracks are named after their first file. Every drop is one undo step however many files and tracks it makes
// (the track creations and the clips are one Batch). A sample already in the project's table (same resolved file) is
// reused rather than added twice.
//
// UI thread only.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/document/samples.h"
#include "pq/views/file_probes.h"

namespace pq::views {

class ViewContext;

struct SampleDropTarget {
  enum class Kind : uint8_t { OnTrack, AfterTrack, AtEnd, Refused };
  Kind kind = Kind::Refused;
  doc::TrackId track{};  // OnTrack: the audio track; AfterTrack: the track new ones follow
};

// Where a track row sends a drop (OnTrack for an audio track, AfterTrack for MIDI / group, Refused for returns and
// the master); an invalid id = below the last track (AtEnd).
SampleDropTarget sampleDropTargetFor(const doc::Project& p, doc::TrackId row);

struct SampleDropPlan {
  struct Item {
    uint32_t file = 0;      // index into the audio files given
    int32_t newTrack = -1;  // index of the new track it lands on (−1: target.track)
    Ticks start = 0;
    Ticks length = 0;       // from the probed length (a placeholder bar while the probe is pending)
    bool pending = false;   // the file's probe has not finished
  };
  SampleDropTarget target;
  std::vector<Item> items;
  uint32_t newTracks = 0;
};

// The plan of a drop of `audio` (the dropped files that are audio, in order) at `at`. `oneTrack` = Ctrl.
SampleDropPlan planSampleDrop(const doc::Project& p, const SampleDropTarget& target, std::span<const ProbedFile> audio,
                              Ticks at, bool oneTrack);

// The edit of a plan: new tracks, new table entries (ids from `ids`) and the clips, as ONE Batch. Every file must be
// Ready. `resolved` = the context's sample resolutions (reuse of files already in the table).
Result<doc::ops::Edit> sampleDropEdit(const doc::Project& p, doc::IdSource& ids, const SampleDropPlan& plan,
                                      std::span<const ProbedFile> audio, std::span<const doc::SampleResolution> resolved,
                                      std::string_view projectDir);

// Plans, builds and dispatches the drop (one undo step). The created clips are returned (selected by the caller).
Result<std::vector<doc::ClipRef>> dropSamples(ViewContext& ctx, const SampleDropTarget& target,
                                              std::span<const ProbedFile> audio, Ticks at, bool oneTrack);

}  // namespace pq::views
