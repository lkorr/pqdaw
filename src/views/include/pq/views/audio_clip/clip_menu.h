#pragma once

// SX.2: the radial clip menu (ui::RadialMenu, pq/ui/radial_menu.h): every control of the audio clip panel
// (AudioClipView's Clip / Warp / mode cards) around a clip in the arrangement, through the same edits
// (pq/views/audio_clip/clip_edits.h). Opened from the button on a selected audio clip, Ctrl+Space or the clip's
// context menu "Clip Menu…" (arrangement/editing.cpp).
//
//   centre   the clip's name and its warp state ("Beats · 120": the mode and the source tempo; "Off")
//   top      Gain and Detune knobs
//   bottom   the Transpose knob (its value the resolved cents; drag: steps of the clip's tuning, in Cents free cents to
//            0.1, in Ratio the nearest ratio with a denominator <= 16; double-click types "7", "386.3c", "+7st", "5/4"),
//            the unit chips beside it (stp / ¢ / ratio: the transpose re-expressed in that unit, clip_edits::inUnit)
//   left     the warp modes Off, Re-Pitch; right Beats, Tones, Texture, Complex, Pro (one choice: a shared binding).
//            Beats / Tones / Texture / Pro open their settings on a sub-ring: Beats Preserve chips, Transient Loop
//            chips (No Loop / → / ↔), Envelope; Tones Grain; Texture Grain, Flux; Pro Formants, Envelope (sliders)
//   around   Fade In / Fade Out sliders (ms; Alt-drag: the curve, as on the clip's handles), Reverse, and Tempo: a
//            category whose sub-ring holds the BPM knob (typed on double-click), :2 / ×2 chips and Loop (off ring 0, so
//            the menu stays compact around the clip)
// A control the clip's mode ignores is drawn disabled (transpose / detune in Re-Pitch; BPM, :2, ×2, Loop for Off; a
// mode's settings unless it is that mode). Knob / slider drags and wheel runs are one undo step; a chip or toggle is
// one. The menu edits the clip it was opened for only (a multi-selection: the clicked clip, as the panel). It follows
// the document (refresh on every change) and closes when the clip is gone (deleted, undone away).
//
// UI thread only.

#include <functional>
#include <vector>

#include "pq/document/model.h"
#include "pq/ui/radial_menu.h"
#include "pq/views/view_context.h"

namespace pq::views {

// The menu's items for audio clip `ref` (bindings look the clip up on every call; tests drive them directly).
std::vector<ui::RadialItem> clipMenuItems(ViewContext& ctx, const doc::ClipRef& ref);

// Opens the clip menu centred on `anchor` (device px in `from`'s tree; moved to keep the rings on screen) in `from`'s
// overlay layer. nullptr: no layer, or `ref` is not an audio clip. `onClosed` runs once when it closes.
ui::RadialMenu* openClipMenu(ViewContext& ctx, ui::Widget& from, const ui::IRect& anchor, const doc::ClipRef& ref,
                             std::function<void()> onClosed = {}, bool animate = true);

}  // namespace pq::views
