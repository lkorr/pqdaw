# SX: direct sample manipulation (user 2026-11-27)

User: "a button that appears when you click on a sample in the timeline, that opens the nested circular Sims-like
menu (as for the Math module); a transpose knob below, warp modes on the sides; everything in the clip panel below
accessible there: warp modes, sliders for the warp modes' params. The waveform must represent what is going on: a
volume fade must show on the waveform, accurately. Warp modes change the shape of the drawn waveform to match (Beats +
transpose glitches it), with a clear label so warped and consolidated clips are told apart at a glance."

Found 2026-11-27 (code survey):
- `ui::RadialMenu` (pq/ui/radial_menu.h) only PICKS leaves; it cannot host knobs / sliders. Opened by the Math element
  and the panel `menu` look=radial.
- The clip panel is `views::AudioClipView` (audio_clip_view.h:5-54 lists every control and its op): Transpose (steps /
  cents / ratio), Detune, Gain, Reverse, Fade In / Out; 7 warp-mode chips, BPM (:2 / ×2), Loop; per mode Beats
  Preserve / Transient Loop / Envelope, Tones Grain, Texture Grain + Flux, Complex Pro Formants + Envelope.
- Arrangement drawing (`arrangement/audio_paint.cpp` `paintClipWaveform`, `internal.h` `forEachAudioPiece`): gain
  scales the drawing, reverse mirrors it; fades are only a darkening overlay + curve (the waveform is not shaped);
  warped clips are mapped by ONE slope at `sourceBpm`: **warp markers are ignored** (the clip view honours them); no
  stretch / transpose is ever reflected. The title badge already prints "Beats 120 +3st …" in small text.
- `ProjectLink::renderClip` renders a whole clip offline (gain, fades, warp, transpose, reverse), off the UI thread.

## SX.1 Radial menu hosts controls (ui) 👁 · w1
Extend the general radial menu (not clip-specific) so an item can be a CONTROL, not only a pick leaf:
- Item kinds: leaf (pick, as now), category (ring), **knob** (rotary, value + label + typed entry on double-click),
  **slider** (arc or short bar), **chip group** (2–7 exclusive choices, the current one lit), **toggle**. Each carries
  get / set / begin / end callbacks (gesture begin / end = one undo step) and a display formatter; no `std::function`
  in any paint loop that matters (UI thread, fine), values read at open and on change only.
- **Placement**: a ring item may be pinned to a side (top / bottom / left / right) or a fixed angle, so a menu can lay
  out "knob below, modes on the sides". Controls are bigger than bubbles (knob ≈ 2× bubble radius).
- The menu stays open while controls are dragged; a pick leaf still closes it; Esc / click outside close it.
  A control inside a sub-ring keeps that ring open while dragged even if the pointer leaves it.
- Wheel over a knob / slider nudges it (Shift fine); Alt-click / double-click resets to default where one is given.
- Tests through real dispatch (window → overlay → widget): drag a knob, a slider, pick a chip, typed entry, Esc, click
  outside mid-gesture (ends the gesture, one undo step), the opener's document changing under the menu (values
  refresh, no dangling pointer). Goldens: a menu with every control kind, 1× and 2×.
- CPU: layout once per open / level change; no allocation per frame; paints only while open / animating.

## SX.2 The clip menu: a button on the selected clip opens it (views) 👁 · w1, after SX.1
- **Button**: a small round button on a SELECTED audio clip (title strip, right end; hidden when the clip is narrower
  than ≈ 3× the button), hit-tested before move / resize; also Ctrl+Space / a context-menu entry "Clip Menu…".
  Click opens the radial menu centred on the clip (clamped on screen).
- **Layout**: centre = clip name + warp state. Below: the Transpose knob (its unit cycles cents / steps / ratio by a
  small chip beside it; ratio typed). Left and right arcs: the 7 warp-mode chips (Off, Re-Pitch on one side; Beats,
  Tones, Texture, Complex, Pro on the other). Hovering / clicking a time-stretch mode opens its sub-ring with its
  params as sliders / chips (Beats: Preserve chips, Transient Loop chips, Envelope; Tones: Grain; Texture: Grain,
  Flux; Pro: Formants, Envelope). Above: Gain knob, Detune knob. Remaining: Reverse toggle, Fade In / Fade Out
  sliders (length; Alt-drag = curve, as on the handles), BPM (:2 / ×2 chips + typed), Loop toggle.
- **Parity rule**: every control in `AudioClipView`'s Clip / Warp / Mode cards is reachable here, through the SAME
  ops (`setAudioClipTranspose`, …, `setAudioClipWarpParams`); a test enumerates the clip panel's controls and checks
  each has a menu counterpart that produces the identical document. Controls a mode ignores are shown disabled, as
  in the panel. Multi-selection: the menu edits the clicked clip only (the panel's rule) — note it in REVIEW.
- Tests: real dispatch from a click on the button through the overlay; each control edits and undoes in one step; the
  clip deleted / undone away while the menu is open closes it. Goldens: button on a selected clip; the menu for an
  Off clip and a Beats clip with its sub-ring open.

## SX.3 The waveform shows what plays: fades, gain, markers, the warped label (views) 👁 · w2
- **Envelope on the waveform**: the drawn amplitude is multiplied by the clip's gain AND its fade-in / fade-out /
  crossfade gain curves (the exact law the lowering / clip player uses: same curve function, one source of truth;
  find it and call it, don't re-derive). Per column (or per tile vertex), not an overlay; the darkening overlay goes
  (keep the fade curve line + handles for editing). Live while a fade / gain handle is dragged (no tile rebuild per
  drag frame: apply the envelope at draw time over cached source peaks; measure).
- **Warp markers** honoured in the arrangement mapping (`forEachAudioPiece` takes the marker law `warpFrameAt` /
  `dsp::WarpLaw`, one piece per marker segment in view), so the arrangement and the clip view agree.
- **Warped label**: a clear, coloured tag on every clip whose audio is processed: "WARP · Beats", "WARP · Pro",
  "RE-PITCH", with "+3 st" etc. beside it; a clip that plays its file 1:1 (Off, no transpose / detune / reverse) has
  no tag. Legible at the default track height; collapses to a coloured corner mark on short clips. Replaces the
  current small badge text.
- Tests: the drawn envelope against the lowered clip's gain at sampled x (fade lengths, curves −1 / 0 / 1,
  crossfade); marker clip vs clip view mapping. Goldens: fades, crossfade, marker clip, tags; re-record the clip
  goldens that move (look at each).

## SX.4 Warped clips draw their rendered audio (app + views) 👁 · w2, after SX.3
- Every time-stretch clip (Beats, Tones, Texture, Complex, Pro) is drawn from its **rendered** audio, so Beats'
  slicing, transposition and stretch artifacts show. Re-Pitch and Off keep the mapped source peaks (already exact:
  pure resampling).
- **Render without the envelope**: render warp + transpose + detune + reverse + markers + params, NOT gain / fades /
  crossfades (SX.3 applies those at draw time, so fade edits stay live). Add the option to the clip render path
  (`ProjectLink::renderClip` or the clip player), no second warp implementation.
- **Async + cached**: one low-priority background thread; key = hash of every field that changes the rendered
  content + the sample revision + the engine rate; debounced ≈ 150 ms after the last edit; a newer request cancels
  the older; progressive (visible part first). While rendering, the clip shows the SX.3 mapped source peaks with a
  subtle "rendering" mark; then swaps without a pop. Rendered audio lives in the waveform cache as an ordinary
  waveform (same renderer, mips, sample-exact zoom per WF.1); LRU memory budget (≈ 256 MB total, peaks kept and raw
  frames dropped first for long clips, re-rendered on deep zoom).
- CPU: nothing renders while idle or for off-screen clips beyond the cache; never on the audio thread; measure a
  4-min Pro clip and a 4-min Beats clip render time (release) and the UI-thread cost of a swap → PROGRESS.
- Tests: rendered waveform == offline render of the clip (frames, without envelope); cache key changes on each param;
  cancellation; edits during a render. Golden: a Beats clip transposed +7 st (glitched shape) next to its Off copy.
  Live check in the worker's own instance. REVIEW entry.

## SX.5 Clip menu as a compact popup around the clip (ui + views) 👁 · w1 (user 2026-11-27, after seeing SX.2)
User: "gain and the fade ins can be removed (already on the clip itself). Instead of a complete circle that grows to
fill half the screen, condense it around the clip, above and below: more rectangular, warp mode options on top,
transpose stuff below; a clean popup only surrounding the clip, around where it was clicked."
- **Remove** Gain, Fade In, Fade Out from the clip menu (the clip's handles edit them). The parity test covers the
  panel's other controls.
- **Layout**: two bands hugging the clip's rectangle (clamped on screen, centred horizontally on the click x):
  ABOVE: the 7 warp-mode chips in one row (Off · Re-Pitch | Beats · Tones · Texture · Complex · Pro), and directly
  above that, the selected time-stretch mode's settings row (Beats: Preserve chips, Transient Loop chips, Envelope;
  Tones: Grain; Texture: Grain, Flux; Pro: Formants, Envelope; nothing for Off / Re-Pitch / Complex) — no hover
  sub-rings. BELOW: Transpose (knob/value + stp / ¢ / n/d), Detune, Reverse, then BPM (:2 / ×2, typed) and Loop.
  Compact: widths about the clip's / the controls' natural size, not a ring; no part covers the clip itself.
- **Implementation**: reuse SX.1's control items and bindings (same `RadialBinding`, chips / knob / slider / toggle
  painting and gestures), as a band layout of the same general ui menu or a sibling general ui widget sharing them
  — no clip-specific code in ui, no duplicated control code. If the radial ring layout then has no caller besides
  Math / panel menus, keep it for them.
- Tests: real dispatch (button, Ctrl+Space, context entry), each control one undo step, mode switch shows the right
  settings row, clip near the window top / bottom (bands flip / clamp, never off screen), narrow clip. Goldens:
  re-record clip-menu-off / -beats (look at each).
