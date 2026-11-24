# WF: clean waveforms everywhere, zoom to the samples (user 2026-11-23)

User: "waveforms need to look clean everywhere they appear; in the sampler they are super quantized, you should be able
to zoom all the way in to see the samples. Same for every time a waveform is represented in the DAW."

Found 2026-11-23: `ui::drawWaveform` / `rasterWaveform` (pq/ui/waveform.h) already pick an overview mip at ≥ 256 frames
per px and read real samples below that (`Waveforms::window`), down to the line between samples. The Sampler does not
use them: `sampler_face.cpp` builds its own columns from the overview only (`Columns`, `Whole`, ≈ lines 425-540,
1049-1070), so any sample shorter than 256 frames per column is the finest mip stretched (the steps), and its editor
cannot zoom. Other sites: arrangement/audio_paint.cpp, audio_clip/audio_clip_view.cpp, project_patch/lane_window.cpp,
browser/browser_parts.cpp, meta/face_targets.cpp, common/waveform_data.h.

### WF.1 One waveform renderer, sample-exact at every zoom 👁 (ui + views) · w2
- **One renderer**: every drawing of a sample's waveform goes through pq/ui/waveform.h (direct or tile); hand-rolled
  column code is deleted (the Sampler's closed well and editor first; audit the other sites and list each: what it
  used, what it uses now).
- **Never the stretched mip as the final picture**: below the finest mip's resolution every site requests the sample
  window and redraws when it arrives (stretched only while loading). Applies at fit zoom too (short samples).
- **Clean at every scale**: (a) columns ≥ 1 frame per px: min / max with antialiased top / bottom edges (fractional
  coverage; no 1-px stair-steps on slopes); (b) < 1 frame per px: the band-limited (windowed-sinc) curve through the
  samples as an antialiased line of the theme's waveform weight, not straight segments; (c) from ≈ 8 px per sample: a
  dot on each sample (stems to zero optional; decide by look). The transitions (mip → samples → curve) must not pop:
  golden at each regime and one at each boundary.
- **Zoom limit**: every zoomable waveform view (arrangement, audio clip view, Sampler editor) zooms in to ≥ 32 px per
  sample; fix any limit that stops earlier (int64 ticks / double frames must hold: check the arrangement's zoom math).
- **Sampler editor zoom**: wheel = zoom about the pointer, Shift+wheel / ruler drag = scroll (follow the audio clip
  view's gestures and HOTKEYS.md), double-click the ruler = fit; handles, slice lines, warp markers, playheads, grain
  region follow the view; a handle outside the view is reachable (scroll while dragging at the edge). The closed well
  stays the fit view. Zoom / scroll is view state (not undoable, not saved).
- **Other waveform-like displays** (wavetable frames, scope, LFO / envelope shapes, browser preview): audit; anything
  drawn as visible steps or an aliased line gets the antialiased line path; list what was left and why.
- CPU: cost stays proportional to visible px; the sinc curve evaluates only visible samples (≤ a few hundred px ×
  taps); tiles cached by (sample, window revision, view); nothing while idle. Bench `drawWaveform` / `rasterWaveform`
  in each regime before / after if a ui bench exists.
- Verify: `pq_tests_ui` + `pq_tests_views` default tier; real-dispatch tests for the editor zoom / scroll gestures and
  for handle drags while zoomed; goldens: waveform goldens move by design (look at each); live check in the worker's
  own instance (script harness from SM.17: `PQ_BUGREPORT_SCRIPT`), zooming from the whole file to single samples in
  the Sampler editor, the audio clip view and the arrangement. REVIEW entry.

### WF.2 The other waveform-like displays + WF.1 leftovers 👁 (ui + views) · w2
WF.1 did the sample waveforms and did not audit the rest. Here:
- **Audit every other place a signal is drawn as a curve**: wavetable frames (2D and 3D faces), scope, band / loudness
  visualisers' traces, LFO and envelope shapes, the Math / module output scopes, automation-free previews in the
  browser, filter / EQ response curves. For each: how it is drawn, whether it shows steps, aliased edges, too few
  points for its width, or a line that breaks at high slope. Fix them through one shared antialiased-line path
  (the face painter's / `pq/ui/lines.h`; no new per-site line code); a curve gets at least one point per device px
  of its width (computed only when its input changes).
- **WF.1 leftovers**: the moiré in the Sampler's closed well for tones near 2 px per cycle; the 1 px line "roping"
  at shallow slopes (check the line's weight / coverage); the arrangement ruler repeating labels zoomed past 1/64;
  edge scroll while dragging a handle in the audio clip view; the Sampler's "Ping-Pong" cell text overflowing its
  cell (sampler-panel-classic golden).
- **Numbers**: release-preset A/B of `drawWaveform` / `rasterWaveform` (before WF.1 = 47 µs for 2560 px stereo from
  the overview) → bench/RESULTS.md; if the antialiased columns cost more than ≈ 1.5× get it back (fewer vertices per
  column, instancing, or raster tiles) without losing the look.
- Verify: `pq_tests_ui` + `pq_tests_views` default tier; goldens of the displays touched move by design (look at
  each); live check in the worker's own instance. REVIEW entry with the per-display audit.
