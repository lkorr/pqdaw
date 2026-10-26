#pragma once

// Where a note clip's content plays on the arrangement timeline (T3.6). Shared by the edit operations (split,
// consolidate, resize, time edits) and by anything that draws or lowers a clip's playback.
//
// The tick math is pq/core/clip_time.h (shared with the engine's SequenceImage, M3 review L16); these are its
// NoteClip forms. A clip plays content from `contentStart` (Ableton's start marker) at arrangement tick `start`, for
// `length` ticks; with the loop on, playback runs from contentStart up to loop.end and then repeats
// [loop.start, loop.end) until the clip ends (a start marker at or after loop.end is folded into the loop).
//
// A note sounds in a pass over the content if its start lies in the content range of that pass; it is cut at the end
// of the pass (the clip end or the loop wrap), as in Ableton. Notes starting before the start marker or outside the
// loop are kept in the clip but never play (Q15: notes outside the loop brace are kept and hidden).

#include <cstdint>
#include <utility>

#include "pq/core/clip_time.h"
#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::doc {

using pq::ClipSegment;  // content [contentFrom, contentTo) plays at arrangement tick `at`

inline ContentLoop contentLoop(const NoteClip& c) noexcept { return {c.loop.enabled, c.loop.start, c.loop.end}; }

inline Ticks clipEnd(const NoteClip& c) noexcept { return c.start + c.length; }

inline bool clipLooped(const NoteClip& c) noexcept { return contentLooped(contentLoop(c)); }

// Content tick that plays `offset` ticks after the clip start (0 ≤ offset; offsets past the clip end continue the
// same pattern).
inline Ticks clipContentAt(const NoteClip& c, Ticks offset) noexcept {
  return pq::clipContentAt(c.contentStart, contentLoop(c), offset);
}

// Calls f(const ClipSegment&) for every pass of the clip that overlaps the arrangement window [from, to), clipped to
// the window, in time order. The number of calls is bounded by the window length / loop length + 2.
template <class F>
void forEachClipSegment(const NoteClip& c, Ticks from, Ticks to, F&& f) {
  pq::forEachClipSegment(c.start, c.length, c.contentStart, contentLoop(c), from, to, std::forward<F>(f));
}

}  // namespace pq::doc
