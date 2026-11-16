# VZ: Visualisers — PQ Meter parity and the production modules

User 2026-11-15: "look at pq meter and see the changes/updates we've made to the performance, customization, bug fixes
and looks … make sure those same fixes are applied to our visualizers. then add a bunch of the visualizers in; stereo
image, phase correlation etc … a ton of the visualizers from pq meter that are remaining that would be useful for
production."

PQ Meter is the user's own app (`C:\Users\Luke\Desktop\programming\pq meter\src\`); porting its code is fine (the
"no code is copied" note in `dsp/spectrum.h` is stale: say "ported from" in new headers). PQ Meter's behaviour and look
are the style guide; pqdaw's face conventions (FacePainter, option_sheet.h, the VZ.1 ANALYSIS / DISPLAY sheet, SAVE
DEFAULT / RESET) are the frame. Paths below: **PQ** = `pq meter/src/`, **pqdaw** = `pqdaw/src/`.

## VZ.2 Spectrum / Spectrogram parity with PQ Meter 1.0.21 👁

Audit 2026-11-15 (PQ Meter commits ab0aab1 … f6bbc4a against pqdaw main a976961c). Each item: port it, or say why not.

Shared
- **X1 one frequency axis** for Spectrum and Spectrogram, like PQ `dsp/freq_axis.h` (scale Log / Mel / Linear, names,
  `freqGridLines`, `FreqHybrid`). Today the Spectrogram has its own (`dsp/src/spectrogram.cpp:63-100`) and the Spectrum
  a fixed log axis (`dsp/src/spectrum.cpp:239-250`, `views/include/pq/views/devices/spectrum_plot.h:47-53`).
- **S2 / G7 continuous log axis** (8f6b6ef): the linear-bottom hybrid applies whenever fMin < 20 Hz, its corner sliding
  to 40 − fMin (PQ `dsp/freq_axis.h:57-77`), so the axis no longer jumps between 0 and 1 Hz.
- **X2 colour stops in dB** (PQ `ui/gradient.h:44` LevelAxis): a Custom ramp whose stops are dB values; moving an end
  keeps the middle stops' dB; **fenced stop sliders** (PQ `ui/widgets.h:261` sliderFloatBounded): a stop halts at its
  neighbours. Used by the Spectrum's and the Spectrogram's colour (Level source, Gamma folded in: PQ
  `modules/spectrogram.cpp:2762`).

Spectrum (pqdaw `views/src/meta/spectrum_face.cpp`, `views/src/devices/spectrum_plot.cpp`, `dsp/src/spectrum.cpp`,
`ui/src/lines.cpp`)
- **S1** scale Log / Mel / Linear + Min / Max frequency options; analysis points even on the chosen axis; Linear grid
  in even 2 kHz steps; the slope guide drawn as a curve on Mel / Linear (PQ `modules/spectrum.h:450`).
- **S3** dense read: where display points are denser than bins, a monotone cubic through per-bin log-power nodes (PQ
  `dsp/spectrum_grid.h:300 nodeLn, :342 denseRead`), replacing the linear join (`dsp/src/spectrum.cpp:132-145`).
- **S4** anti-aliased gradient stroke: shared vertices, mitre capped at 2× half-width, 1 px fringe (PQ
  `modules/spectrum.cpp:217`), replacing a hard quad per segment (`ui/src/lines.cpp:269-291`).
- **S5** one tight glow pass instead of three plus the 1.9× body; glow off by default (PQ `modules/spectrum.cpp:1477-1500`).
- **S6** Average line: long-term power mean, bias-corrected, skips silence, restarts on axis / slope / channel / FFT
  change, 0–60 s, held through freeze (PQ `modules/spectrum.cpp:843, :964`, `spectrum.h:384-397`).
- **S7** "tilt +4.5 dB/oct" tag whenever the slope ≠ 0 (PQ `modules/spectrum.cpp:1591`).
- **S8** colour: the colormaps and colour modes PQ Meter's Spectrum has (incl. Bars), a separate colour window ("Colors
  follow scale" off by default; Quiet default −80 dB adjustable, Loud pinned at 0 dBFS; PQ `modules/spectrum.h:405-417`).
- **S11** honest Bars: linear, fixed 12 dB fade-in, no hidden gamma (PQ `modules/spectrum.cpp:106 kBarFadeDb`).
- `tests/ui/test_pqmeter_diff.cpp` compares `gradientPolyline` against PQ Meter's current source: update its shim
  (`tests/ui/pqmeter_shim/imgui.h`: `_FringeScale`) and the expected layering to the new stroke; it must build and pass
  against today's PQ Meter.

Spectrogram (pqdaw `views/src/meta/spectrogram_face.cpp`, `dsp/src/spectrogram.cpp`)
- **G1** fade blends toward the Silence colour, not black (PQ `modules/spectrogram.cpp:1165 groundColor`): matters once
  Custom stops exist.
- **G2** Silence opacity (Level source), colour-to-alpha against the ground (PQ `modules/spectrogram.cpp:2776`).
- G7 and X2 above.

Oscilloscope: already current (beat timebase, freeze). One caveat only: an option change while frozen does not
re-derive from the held history (`scope_face.cpp:621`). Leave it unless trivial; another session is editing the scope
files on 2026-11-15, avoid conflicts.

Accept: each item present (or a Q with the reason); goldens of the Spectrum / Spectrogram faces re-recorded and looked
at; pqmeter_diff passing; UI frame cost of 8 Spectrum faces no worse than before (stroke / dense read measured);
REVIEW entry.

## VZ.3 Tone preview on the Spectrum and Spectrogram 👁

PQ b721058 / 5dab446 (PQ `ui/tone_controls.h`, `modules/spectrum.cpp:1792`, `modules/spectrogram.cpp:2185, :2820`):
click / drag on the Spectrum plays that frequency; on the Spectrogram the keyboard's keys play their pitch and Ctrl+click
anywhere on the picture plays (own toggle); a rule at the sounding pitch; options snap, level, waveform. In pqdaw the
tone goes out through the app's existing preview / audition output (find it: `app/audio_output.h`,
`engine/live_input.h`), never into the project. Snap options: off, 12-EDO, **the active tuning's pitches** (the
microtonal extra). RT-safe on the audio side (phase uint32, no allocation); release ramps (no clicks).
Accept: tests through real event dispatch (press → tone at the right Hz, drag glides, release stops, snap to tuning),
the audio path's RtScope test, REVIEW entry.

## VZ.4 Loudness: Level Meter, LUFS Meter, Loudness History 👁

Three pass-through analyser types sharing one analysis-worker front end (port PQ `dsp/loudness.h`: K-weighting,
400 ms / 3 s windows, gating, integrated, LRA histogram; PQ `modules/lufs_meter.cpp:104-139` 4× true peak, `:207`
correlation). Read PQ `modules/level_meter.cpp`, `lufs_meter.cpp`, `loudness_graph.cpp` fully and carry their options
and look.
- **Level Meter** (PQ level_meter): L/R RMS fill, peak cap, hold tick, latched clip lamp, floor, RMS window, peak fall,
  hold, clip threshold, auto clip reset, readouts, zones. Reuse `ui::Meter` / the panel `meter` element
  (`views/src/panel/engine_elements.cpp:324-467`) where it fits.
- **LUFS Meter** (PQ lufs_meter): M / S / I LUFS, LRA, true peak, correlation, max M / S, target, scale, auto-reset.
- **Loudness History** (PQ loudness_graph): scrolling M / S / I traces, target line + tolerance band, scale modes,
  history, auto-reset.
Input: the stereo signal tap (L, R channels; `kWantScope` raw history or a new `kWantLoudness` worker stage — decide by
cost; only while a face is visible). Adding a type: the 17-step list in the VZ survey (meta type, registry, ModuleKind,
modules.cpp info / engine type / panel, panel.cpp elementTap, engine_elements kSections, faces.h + meta_face.cpp, the
face, node_catalog visualiser list, scenes, goldens, tests lists in test_pass_through / test_engine_modules /
test_engine_elements / test_audio_manager).
Accept: EBU Tech 3341 / 3342 cases (1 kHz −23 dBFS sine = −23 LUFS M/S/I ± 0.1; the gating and LRA test signals
generated in the test), true peak vs the `truepeak` atom on inter-sample peaks, correlation +1 / −1 / ~0; goldens;
perf (worker cost per visible face; zero when hidden); REVIEW entry.

## VZ.5 Stereo: Stereo Field, Stereo Image, Phase Scope 👁 (after VZ.2: reuse its colour stops and axis)

- **Stereo Field** (PQ `modules/stereo_field.cpp`): goniometer (M up, S across), correlation bar, 3-band
  Linkwitz-Riley mode with per-band correlation, trace ms, gain auto / manual, lines / dots, grid, labels. Reads raw L/R
  history; no new worker DSP beyond the bands.
- **Stereo Image** (PQ `modules/stereo_image.cpp`, 2.4k lines): per log-frequency point pan (R−L)/(R+L) and width
  (1 − |coherence|) from a dual-channel cross-spectrum on the worker; ribbon, glow and heat-map modes; its options.
- **Phase Scope** (PQ `modules/phase_scope.cpp`): mono-sum gain / loss per frequency from the same averaged cross
  spectrum, coherence-gated, mono-warn band, freeze.
One `dsp` cross-spectrum analyser feeds both spectral modules. Accept: tests (a sine panned to p reads p ± 0.02;
identical channels width 0; L = −R full mono loss at every frequency; uncorrelated noise coherence ~0), goldens, perf,
REVIEW entry.

## VZ.6 Band Levels and Waveform 👁

- **Band Levels** (PQ `modules/multiband_rms.cpp`): 3 / 4 / 5-band LR crossover RMS over time, channel mono / L / R /
  M / S, crossover spread, ballistics VU / peak / custom, warped dB axis, fill / line.
- **Waveform** (PQ `modules/waveform.cpp`): scrolling peak + RMS envelope, M/S or L/R lanes, colour by level or
  brightness, scroll or beat-locked loop playhead (the tap's beat channel), auto-gain, dB scale. Reuse
  `dsp/scope.h` ScopeBeat slots where they fit.
Accept: band-split tests (a sine lands in its band; the crossovers sum flat), goldens, perf, REVIEW entry.

## VZ.7 Waterfall and Stereo Spectrogram (3D) 👁 (user 2026-11-15: port them the way PQ Meter renders them)

Both render on the CPU in PQ Meter, so they port directly:
- **Waterfall** (PQ `modules/waterfall.cpp`, 1.9k lines): SE(3) camera, the terrain projected on the CPU into 2D
  triangles (`PrimReserve` quad strips, :70-120). In pqdaw: the same projection into DrawList / FacePainter triangles
  (add an indexed-mesh call to FacePainter if strips don't fit). All its options: FFT / hop, window, multi-resolution,
  channel mono / L / R / M / S, slope, scale (VZ.2's shared axis), filled / wireframe, lit shading, cage / grid /
  labels, free camera + snap views, colour (VZ.2's colour stops).
- **Stereo Spectrogram** (PQ `modules/stereo_spectrogram.cpp`, 3.7k lines): the reassigned stereo STFT point cloud
  (time, freq, pan, power) splatted on the CPU into an RGBA canvas, uploaded as a texture and drawn as an image
  (:2140-2246). In pqdaw: the Spectrogram face's texture path (`views/src/meta/spectrogram_face.cpp`, ui texture pool)
  in place of raw GL; its render modes, detail, camera snaps (yaw 0 = spectrogram, 90 = stereo image heat map, pitch
  90 = pan over time), fade, heat decay, colour, grid / cage / labels. Reuse VZ.5's cross-spectrum where it fits.
Camera drags orbit, double-click snaps; idle = no frames when the signal is silent and the camera rests.
Accept: projection tests (known points land where expected for the snap views), goldens at each snap view, perf (UI
frame per face at 1440p, ship numbers; canvas upload only of changed rows where possible), REVIEW entry.

## Deferred (not in this run; ask the user)
- Kick/Sub Phase and Align (PQ kicksub_phase / kicksub_align): need a two-input analyser (a second tap slot from a
  cable), new plumbing.
- Tonal Reference (reference-file library + decode).
- PQ Spectrogram kick/sub collision ticks.

## Lanes
w-vz1: VZ.2 → VZ.3 → VZ.6. w-vz2: VZ.4 → VZ.5 (after VZ.2 is merged) → VZ.7.
