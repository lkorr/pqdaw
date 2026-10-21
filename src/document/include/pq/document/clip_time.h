#pragma once

// Where a note clip's content plays on the arrangement timeline (T3.6). Shared by the edit operations (split,
// consolidate, resize, time edits) and by anything that draws or lowers a clip's playback.
//
// A clip plays content from `contentStart` (Ableton's start marker) at arrangement tick `start`, for `length`
// ticks. With the loop off, content tick contentStart + o plays at start + o. With the loop on, playback runs from
// contentStart up to loop.end and then repeats [loop.start, loop.end) until the clip ends. A start marker at or
// after loop.end is folded into the loop (it plays from the matching position inside the loop), so every offset has
// exactly one content position.
//
// A note sounds in a pass over the content if its start lies in the content range of that pass; it is cut at the end
// of the pass (the clip end or the loop wrap), as in Ableton. Notes starting before the start marker or outside the
// loop are kept in the clip but never play (Q15: notes outside the loop brace are kept and hidden).

#include <algorithm>
#include <cstdint>

#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::doc {

inline Ticks clipEnd(const NoteClip& c) noexcept { return c.start + c.length; }

inline bool clipLooped(const NoteClip& c) noexcept { return c.loop.enabled && c.loop.end > c.loop.start; }

// Content tick that plays `offset` ticks after the clip start (0 ≤ offset; offsets past the clip end continue the
// same pattern).
inline Ticks clipContentAt(const NoteClip& c, Ticks offset) noexcept {
  const Ticks p = c.contentStart + offset;
  if (!clipLooped(c) || p < c.loop.end) return p;
  const Ticks len = c.loop.end - c.loop.start;
  return c.loop.start + (p - c.loop.end) % len;
}

// One contiguous pass over the content: content [contentFrom, contentTo) plays at arrangement tick `at`.
struct ClipSegment {
  Ticks contentFrom = 0;
  Ticks contentTo = 0;
  Ticks at = 0;
};

// Calls f(const ClipSegment&) for every pass of the clip that overlaps the arrangement window [from, to), clipped to
// the window, in time order. The number of calls is bounded by the window length / loop length + 2.
template <class F>
void forEachClipSegment(const NoteClip& c, Ticks from, Ticks to, F&& f) {
  const Ticks end = std::min(to, clipEnd(c));
  Ticks o = std::max<Ticks>(from - c.start, 0);
  while (c.start + o < end) {
    const Ticks content = clipContentAt(c, o);
    Ticks len = end - (c.start + o);
    if (clipLooped(c)) len = std::min(len, c.loop.end - content);
    f(ClipSegment{content, content + len, c.start + o});
    o += len;
  }
}

}  // namespace pq::doc
