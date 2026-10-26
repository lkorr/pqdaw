#pragma once

// Where a clip's content plays on the arrangement timeline: the pure tick math, in one place (M3 review L16). Used by
// the document (pq/document/clip_time.h: edit operations, drawing) and by the engine's SequenceImage
// (pq/engine/sequence.h), which describe a clip with their own structs; before, each had its own copy of the rules.
//
// A clip plays content from `contentStart` (Ableton's start marker) at arrangement tick `start`, for `length` ticks.
// With the loop off, content tick contentStart + o plays at start + o. With the loop on (enabled and end > start),
// playback runs from contentStart up to loop.end and then repeats [loop.start, loop.end) until the clip ends. A start
// marker at or after loop.end is folded into the loop (it plays from the matching position inside the loop), so every
// offset has exactly one content position. Each run over the content is a pass: the first one ends at the loop wrap
// (or the clip end), every later one is one loop length long.
//
// All functions are RT-safe (integer arithmetic only).

#include <algorithm>
#include <cstdint>
#include <limits>

#include "pq/core/time.h"

namespace pq {

struct ContentLoop {
  bool enabled = false;
  Ticks start = 0;  // content ticks
  Ticks end = 0;
};

constexpr bool contentLooped(const ContentLoop& l) noexcept { return l.enabled && l.end > l.start; }  // RT-safe

// Content tick that plays `offset` ticks after the clip start (0 ≤ offset; offsets past the clip end continue the
// same pattern).
constexpr Ticks clipContentAt(Ticks contentStart, const ContentLoop& l, Ticks offset) noexcept {  // RT-safe
  const Ticks p = contentStart + offset;
  if (!contentLooped(l) || p < l.end) return p;
  return l.start + (p - l.end) % (l.end - l.start);
}

// Length of the first pass from the clip start: up to the loop wrap when looped (loop.end minus the content at
// offset 0, so ≥ 1), otherwise unbounded (the clip end cuts it).
constexpr Ticks clipFirstPassLength(Ticks contentStart, const ContentLoop& l) noexcept {  // RT-safe
  return contentLooped(l) ? l.end - clipContentAt(contentStart, l, 0) : std::numeric_limits<Ticks>::max();
}

// One contiguous pass over the content: content [contentFrom, contentTo) plays at arrangement tick `at`.
struct ClipSegment {
  Ticks contentFrom = 0;
  Ticks contentTo = 0;
  Ticks at = 0;
};

// Calls f(const ClipSegment&) for every pass of the clip [start, start + length) that overlaps the arrangement window
// [from, to), clipped to the window, in time order. The number of calls is bounded by the window length / loop length
// + 2.
template <class F>
void forEachClipSegment(Ticks start, Ticks length, Ticks contentStart, const ContentLoop& l, Ticks from, Ticks to,
                        F&& f) {
  const Ticks end = std::min(to, start + length);
  Ticks o = std::max<Ticks>(from - start, 0);
  while (start + o < end) {
    const Ticks content = clipContentAt(contentStart, l, o);
    Ticks len = end - (start + o);
    if (contentLooped(l)) len = std::min(len, l.end - content);
    f(ClipSegment{content, content + len, start + o});
    o += len;
  }
}

}  // namespace pq
