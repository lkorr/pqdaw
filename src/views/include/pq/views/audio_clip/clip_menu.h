#pragma once

// SX.2 / SX.5: the clip menu (ui::RadialMenu laid out as bands, pq/ui/radial_menu.h): the audio clip panel's controls
// (AudioClipView's Clip / Warp / mode cards) in a compact popup hugging the clip in the arrangement, through the same
// edits (pq/views/audio_clip/clip_edits.h). Gain and the fades are not here: the clip's own handles edit them. SX.6: the
// arrangement shows it whenever the selection is exactly one audio clip (arrangement/editing.cpp syncClipPopup: it
// follows the clip, hides during drags, Esc / Ctrl+Space dismiss it), centred on the x it was opened at; it never covers
// the clip (a band that does not fit above / below goes to the other side, else it is clamped on screen). Modeless
// (RadialMenuOptions::modeless): a press beside it reaches the arrangement, keys go to the arrangement unless a typed
// entry is open.
//
//   above   one row: the Warp toggle (lit while warped; on: Preferences' default warp mode, warped from the shape the
//           clip has: clip_edits::setWarped), then the modes Re-Pitch | Beats, Tones, Texture, Complex, Pro (one choice:
//           a shared binding; none lit while Off; a mode chosen on an Off clip warps it from its shape too); directly
//           above them the chosen mode's settings: Beats Preserve chips, Transient Loop chips (No Loop / → / ↔),
//           Envelope; Tones Grain; Texture Grain, Flux; Pro Formants, Envelope (sliders); nothing for Off / Re-Pitch /
//           Complex
//   below   the Transpose knob (its value the resolved cents; drag: steps of the clip's tuning, in Cents free cents to
//           0.1, in Ratio the nearest ratio with a denominator <= 16; double-click types "7", "386.3c", "+7st", "5/4"),
//           the unit chips beside it (stp / ¢ / n/d: the transpose re-expressed in that unit, clip_edits::inUnit), the
//           Detune knob; then the BPM knob (typed on double-click), :2 / ×2, Loop
// A control the clip's mode ignores is drawn disabled (transpose / detune in Re-Pitch; BPM, :2, ×2, Loop for Off).
// Reverse is not here (R, and the clip panel). Knob / slider drags and wheel runs are one undo step; a chip or toggle
// is one. The menu edits the clip it was opened for only (a multi-selection: the clicked clip, as the panel). It
// follows the document (refresh on every change) and closes when the clip is gone (deleted, undone away).
//
// UI thread only.

#include <functional>
#include <limits>
#include <vector>

#include "pq/document/model.h"
#include "pq/ui/radial_menu.h"
#include "pq/views/view_context.h"

namespace pq::views {

// The menu's items for audio clip `ref` (bindings look the clip up on every call; tests drive them directly).
std::vector<ui::RadialItem> clipMenuItems(ViewContext& ctx, const doc::ClipRef& ref);

// Opens the clip menu around `anchor` (the clip's visible box, device px in `from`'s tree), centred on x `atX` (the
// same px; NaN: the box's centre), in `from`'s overlay layer. nullptr: no layer, or `ref` is not an audio clip.
// `onClosed` runs once when it closes.
ui::RadialMenu* openClipMenu(ViewContext& ctx, ui::Widget& from, const ui::IRect& anchor, const doc::ClipRef& ref,
                             float atX = std::numeric_limits<float>::quiet_NaN(), std::function<void()> onClosed = {},
                             bool animate = true);

}  // namespace pq::views
