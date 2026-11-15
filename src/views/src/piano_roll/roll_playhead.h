#pragma once

// bug-roll-playhead: the transport's playhead in the clip view and the ruler as its scrub area (Live's). The clip's
// time mapping is pq/document/clip_time.h's (clipContentAt): content tick contentStart plays at the clip's start, a
// looped clip repeats [loop.start, loop.end) after its first pass.

#include <optional>

#include "pq/core/time.h"
#include "pq/document/model.h"
#include "pq/ui/widget.h"

namespace pq::views::roll {

class RollState;

// The content tick of clip `c` that plays at song tick `song`; nullopt outside [c.start, c.start + c.length).
std::optional<Ticks> contentAtSong(const doc::NoteClip& c, Ticks song) noexcept;

// The song tick at which content tick `content` plays. Content outside what plays is clamped into it (unlooped: the
// start marker … its end; looped: from the earlier of the start marker and the loop start to the loop end). Looped,
// the pass the transport is in (`playing`: its song tick) when that pass plays it, else the first pass that does.
Ticks songAtContent(const doc::NoteClip& c, Ticks content, std::optional<Ticks> playing) noexcept;

// The playhead line (the theme's arrangement.playhead, one device px wide, as the arrangement's and the lanes') over
// `r` (the ruler, the note area or the velocity lane), when the roll shows one.
void paintPlayhead(ui::PaintContext& ctx, const RollState& s, const ui::IRect& r);

// The scrub area's click or drag at content tick `contentTick`: snapped to the roll's grid (Alt: not), mapped to the
// song and played from there (ViewContext::playFrom: stopped, playback starts; playing, it jumps). `last` (a scrub's
// previous song tick) skips a repeat. Returns the song tick.
Ticks scrubAt(RollState& s, double contentTick, ui::Mods mods, Ticks last = -1);

}  // namespace pq::views::roll
