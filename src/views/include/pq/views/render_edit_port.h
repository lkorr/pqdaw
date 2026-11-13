#pragma once

// FZ.4 (docs/plans/RB.md D5, "### FZ.4"): the plain-data port between the edits that RENDER FIRST — Flatten, Bounce
// to New Track, Consolidate on audio tracks — and the app that renders them (pq/app/render_edits.h over the playback
// link), like ExportPort: views never include app or engine. The views decide what is asked (the arrangement's menus and
// Ctrl+J); the app renders on a worker thread, shows progress on the status line (with a Cancel button) and, when the
// render completes, applies the edit as ONE undo step — or drops it when the document changed what was rendered.
// UI thread only.

#include <cstdint>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::views {

struct RenderEditJob {
  enum class Kind : uint8_t {
    Flatten,      // frozen tracks whose render is Ready become audio tracks playing it (ops::flattenTrack)
    Bounce,       // each track's own contribution onto a new audio track below it; the track deactivated
    Consolidate,  // each audio track's clips over [from, to) replaced by one clip of what its clip player plays
  };
  Kind kind = Kind::Flatten;
  std::vector<doc::TrackId> tracks;  // MIDI / audio tracks (Consolidate: audio tracks), row order
  bool range = false;                // Bounce: [from, to) (a time selection), else the whole track; Consolidate: set
  Ticks from = 0, to = 0;
};

class RenderEditPort {
public:
  virtual ~RenderEditPort() = default;
  // Starts `job`. False when it does not start (another render edit runs, nothing to render): the status line says why.
  virtual bool start(const RenderEditJob& job) = 0;
  virtual bool busy() const = 0;
  // Stops the running job: no edit, no file left (a no-op when none runs).
  virtual void cancel() = 0;
};

}  // namespace pq::views
