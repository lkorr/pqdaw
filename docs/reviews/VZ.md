# VZ review: PQ Meter parity and the analyser modules (VZ.2 – VZ.7)

Reviewer: independent agent, 2026-11-16, on `origin/main` 41913c6f (VZ commits from 2a07596b). Read: the VZ diffs, PQ
Meter's `src/`, the new goldens. Reproduced by a throwaway test (not committed): **C1, H1, H2, M1** (marked REPRO).
Everything else is from reading code; PLAUSIBLE marks what was not confirmed.

Counts: **C 1 · H 2 · M 11 · L 17**.

What holds up (checked, no action): K-weighting (pre-warped, = PQ), M/S windows, the −70 / −10 LU gates, LRA (3 s on
1 s grid, −20 LU, 10–95 %), true peak, the cross-spectrum (L·conj R, mono sum, normalisation), the LR4 tree with all-pass
compensation, the reassignment formulas (signs and units), PCHIP, the hybrid axis's continuity, the toroidal canvas's
leaver repair (exact), the camera's projection (no near-zero depth), feed locking (every publish/read pair under one
mutex, no torn reads), idle (hidden faces let go after 0.5 s; the worker blocks at 0 consumers), the tone path's RT
safety (`render` noexcept, no alloc/locks; RtScope tests exist), option persistence (only non-default fields, validated).

## Critical

**C1 · Heap overflow in `CrossSpectrum` when the tracker window changes while Particles is off** (REPRO)
- `src/dsp/src/stereo.cpp:117-135`: the peak FFT, `peakWin_`, `pre_/pim_`, `power_` are rebuilt on a `peakFft` change
  only while Particles is on; the `else` branch rebuilds only when `peakFft_` is null. `track()` (`:260-267`) then loops
  `M = cfg_.peakFft` over 2048-sized buffers.
- Scenario (Stereo Image sheet, all visible rows: `stereo_faces.cpp:458-462`, `:420`): Mode Particles → another mode →
  Tracker window 4096 → Mode Particles. The worker reads `peakWin_[2048…4095]` and writes `power_[1025…2048]`: heap
  corruption, crash at some later point on the analysis worker.
- Repro: configure {Particles, 2048} → {Glow, 4096} → {Particles, 4096}: `peakWin_.size() == 2048`,
  `power_.size() == 1025` with `cfg_.peakFft == 4096`.
- Fix: rebuild whenever `(parts & Particles) && (!peakFft_ || peakWin_.size() != cfg_.peakFft)`; add a test that walks
  configurations in sequence (none does today).

## High

**H1 · A face that was hidden writes into its destroyed feed** (REPRO)
- `src/views/src/meta/analyser_face.h:106-109` (`paintLive`) and `:68-69` (`pollVoices`) call `syncOptions()` before
  `syncFeed()`; `applyOptions` (`:420-423`) calls `sendRequest(*feed_)` on the cached raw pointer. After `letGo()` the
  face holds no consumer, and `AnalysisHub::setSource` / `feed(device[, path])` (`analysis.cpp:1081-1102, 1145-1153`)
  destroy feeds with 0 consumers at once.
- Scenario: a Waterfall / LUFS / Stereo face's panel is hidden (it lets go), the rate changes or its tap is replaced (the
  feed is freed), the instance's data changes (undo, or the same instance's other face edits), the panel is shown:
  `setLoudness` / `setWaterfall` / … lock a freed mutex and write freed memory. The pre-VZ Spectrum has the same shape
  (`spectrum_face.cpp:234, 571-582`).
- Repro: a LUFS face on feed A, hidden until it lets go, A destroyed in place, data changed, `paintLive`: A's freed
  bytes change (a control run without the data change leaves them untouched).
- Also (PLAUSIBLE, same root): `syncFeed` compares raw pointers (`:452`), so a new feed allocated at the freed address
  is taken for the old one; `feedChanged()` never runs and the Waterfall's `gen_/end_/seq_` skip slices until the new
  count passes the old.
- Fix: `syncFeed()` first in both entry points; `applyOptions` sends only when `acquired_` (acquire sends anyway). Better:
  the hub hands out `weak_ptr<SpectrumFeed>` (or a feed serial) and the face compares that.

**H2 · Semisine and Triangle tone preview are silent from 10.24 kHz up** (REPRO)
- `src/dsp/src/tone_preview.cpp:21-49`, `table()` `:81`: band 9 is built with every partial weighted at its band top
  `fb = 20480`; `weight(20480) = clamp((20000 − 20480) / 2000) = 0` zeroes the fundamental too, so the table is all zero.
- Scenario: click the Spectrum at 12 kHz with Triangle: nothing. Measured: Sine −3.0 dB, Semisine / Triangle −300 dB at
  12 k and 18 k.
- Fix: the fundamental always at full weight; weight harmonics k ≥ 2 at k·fb. Test at 12 k and 18 k (today's band test
  stops at 7 k).

## Medium

- **M1 · Tone glide parks off pitch** (REPRO). `tone_preview.cpp:111`: `l2 += (lt − l2)·glide` in float stops moving once
  the step is under an ulp of log2 Hz. 440 → 660 Hz at 96 kHz parks at 659.749 Hz (−0.66 cents; ~−0.33 at 48 k, ~−1.3 at
  192 k). Wrong for "snap to the track's tuning". Fix: snap `l2 = lt` under 1e-5 octave, or keep it in double. The tests'
  1e-3 – 2e-3 pitch tolerances hide it.
- **M2 · Tone gain ramp sized from the new target.** `tone_preview.cpp:97-99`: `span = max(tGain, 1e-6)`. A take-over
  from 0 dB to a −40 dB hold takes ~3 s to come down; at −90 dB, minutes. Fix: step = |goal − g| / ramp time, set when the
  goal changes (or span = max(g, tGain)).
- **M3 · The analysis worker never flushes denormals.** `analysis.cpp` has no `DenormalGuard` (`SpectrumFeed::run`
  `:1071`); CLAUDE.md rule 3 requires FTZ on audio-adjacent threads and `waveform.cpp:59` (`lp_ += a·(v − lp_)`), the
  band biquads and `StereoFieldState` decay into subnormals during played silence. PLAUSIBLE cost (not measured). Fix: one
  `pq::DenormalGuard` at the top of `run()`.
- **M4 · SAVE DEFAULT never reaches a module added from the catalog** (VZ.1's feature; every VZ module is placed this
  way). `engineModuleBlocks` (`src/document/src/modules.cpp:548-566`, `Builder::add` `:119-126`) makes the meta device
  with no `""` instance, so `DocTarget::data()` (`face_targets.cpp:139-144`) shows the factory default; the first edit's
  `AddMetaInstance{defaultMetaInstance}` (`edit_meta.cpp:67`) is then overwritten by factory + that edit. Only
  `edit_devices.cpp:171` and `library_load.cpp:217` apply user defaults. Fix: in `engineModuleBlocks` set
  `meta = {"", defaultMetaInstance(*t)}`; test through `moduleBlocks(kind)` (today's test checks only
  `defaultMetaInstance`, `test_spectrum_display.cpp:415-433`).
- **M5 · Alt+wheel zoom makes one undo step per notch** on the Waterfall and Stereo Spectrogram
  (`waterfall_face.cpp:294-299`, `stereo_spectrogram_face.cpp:336-341`: `writePose` outside a gesture; no `ownEndWheel`).
  Convention elsewhere is one step per wheel run (`meta_face.h:206`). Fix: begin on the first notch, end in `ownEndWheel`.
- **M6 · Stereo Spectrogram point store: one heap allocation per point.** `stereo_spectrogram_face.cpp:473, 475`:
  `std::deque<CloudPt>` (32 B) and `std::deque<RibSlice>`; MSVC's deque block holds one element over 8 B, so every
  `push_back` (`:279`) mallocs and `pop_front` (`:310`) frees (thousands per second on the UI thread), and `rebuild()`
  (`:895`) walks up to 100 k scattered nodes (~30×/s while orbiting). Fix: a fixed ring indexed by sequence (the tiles
  already address points by sequence).
- **M7 · Stereo Image glow / particles: up to 32 k rounded rects per frame.** `stereo_faces.cpp:574-591`: 256 × 128 cells,
  each a `roundedRect` 2.5× the cell (~6× overdraw); `stride` merges only sub-pixel rows. Fix: write the grid to a
  128 × 256 texture (TexturePool, as the Spectrogram) and draw one quad.
- **M8 · Chips are clickable while not drawn.** `analyser_face.h` `hitTest` ignores `pxPerUnit`; SET/CLEAR/FREEZE and the
  SPEC/SIDE/TOP/3D chips are drawn only when labels show, but hit at every zoom: a zoomed-out click near a face's top
  freezes it, opens the sheet or snaps the camera invisibly. Fix: gate on the same `kLabelPx * pxPerUnit` test.
- **M9 · Band filters' float coefficients at high rates** (PLAUSIBLE). `band_levels.cpp:18-46` (and `stereo.cpp:452`)
  compute `cos(w0)` and `1 + a1 + a2` in float; at 192 kHz, 60 Hz the cancellation is several % of DC gain and the bands
  stop summing flat. Fix: design in double (`1 − cos = 2 sin²(w0/2)`); run the flat-sum test at 192 kHz (it runs at 48 k
  only).
- **M10 · Max momentary reads low for bursts off the 100 ms grid** (PLAUSIBLE vs EBU Tech 3341 cases 10–12; same as PQ).
  `loudness.cpp:302-339`: M/S only evaluated each 100 ms, so a 0.4 s burst 50 ms off-grid peaks −0.58 LU low. Fix:
  10 ms sub-blocks for the max-M/S evaluation, gating stays on 100 ms. Add the 3341 cases 9–12.
- **M11 · Scroll draw path untested.** `stereo_spectrogram_face.cpp:1263-1276` (the up-to-4-quad toroidal draw) never runs
  in tests (`FaceRig` has no TexturePool, `tex_ == 0`); `verifyCanvas` proves the max-repair, not where points land after
  a scroll. Fix: paint with a pool after N scrolled updates and compare against a forced rebuild.

## Low

- L1 · `analyser_face.h:430-438`: double-click on a Choice / Toggle row = two undo steps (cycle, then default).
- L2 · `analysis.cpp:546, 581, 597, 339`: `stereo_ / wf_ / cloud_ / gram_` read on the UI thread, written by the worker
  unlocked: formal data race (benign on x86). Compare against the request instead.
- L3 · `analysis.cpp:860-863`: after a > ~5 s stall the cloud stage keeps the oldest points and drops the newest.
- L4 · Feed options are last-writer-wins per stage (only the spectrogram keys requests): two faces of one instance with a
  read-only (`local_`) target fight. Note as a limitation.
- L5 · `audio_output.cpp:356-363`: `setCuePair` moves a sounding tone between pairs with no ramp (click).
- L6 · Tone (PLAUSIBLE): if the rate-mismatch mute or pre-roll begins mid-release, `active_` stays true and the stream
  never suspends until audio resumes normally.
- L7 · `loudness.cpp:462`: `resetMeasurement` keeps `sub_ / subCount_`, so the first gated / LRA block after CLEAR holds
  up to 300 ms / 2.9 s of pre-reset audio.
- L8 · `stereo.h:17`: `phaseDeg` comment says R relative to L; `atan2(L·conj R)` is φL − φR.
- L9 · CPU: `stereo_cloud.cpp:156-163` runs the low-band ribbon's two 16 k FFTs every 256-sample hop (~64× overlap; every
  `loFactor` hops keeps 8×); `waveform.cpp:118,122` `fmod` per sample; `tone_preview` `fastExp2` per sample when not
  gliding; `stereo_faces.cpp:601-631` `pow / exp2 / log` per trail point twice per frame.
- L10 · `waveform.cpp:145-149`: BPM from a float beat difference: ~1 % error near beat 4000, growing with position.
- L11 · Per-paint allocations: `stereo_spectrogram_face.cpp:1244` (`std::vector<double> key`), Waterfall
  `rebuildPalette` key, `paintSheet` building `sheet::Values` + `userDefaultData()` each frame while open; custom
  gradients rebuilt every paint (`stereo_spectrogram_face.cpp:402`, `stereo_faces.cpp:535`).
- L12 · `band_wave_faces.cpp`: Waveform auto-gain eases inside paint (frame-rate dependent; stalls without frames). Move
  to `animate(dt)`.
- L13 · `camera3d.h:86, 115-117`: releasing an orbit near a flat view snaps zoom to 1 and pan to 0 (PLAUSIBLE unintended);
  `:100, 107` yaw / roll clamp at ±180° instead of wrapping.
- L14 · `waterfall_face.cpp:321`, `stereo_spectrogram_face.cpp:362`: the snap chips are always drawn `on = false`; the
  current view is never shown (see UI).
- L15 · `stereo_spectrogram_face.cpp:539-563` (`Shade::colour`): without the amplitude channel alpha is 255, so the
  colormap's black low end paints opaque black specks on the teal well (see UI). Alpha from `t` (or premultiplied max
  channel) fixes it.
- L16 · `loudness_faces.cpp:104-114` (PLAUSIBLE): two `loudnessRead` calls per poll; `r_` and `hist_` can be one point apart.
- L17 · LUFS Meter shows `LRA 0.0 LU` before any LRA exists (`loudness_faces.cpp:438`) while the other rows show `--.-`.

## Tests

- "Every option paints" cases (`test_stereo_display.cpp:248`, `test_loudness_display.cpp:337`,
  `test_view3d_display.cpp:345`) only prove no crash; their comments promise "nothing outside the draw list" and "far out:
  no text" (`rig.scale = 0.4f`), which nothing asserts. Zoom-out legibility is untested.
- Gestures in `test_view3d_display` / `test_spectrum_display` go through `face->onPointer`, not window dispatch (the
  project's rule after the Tab-place crash); loudness, stereo, band/wave have no pointer tests (SET/FREEZE via
  `*TestControl`). M5 and M8 would have been caught by dispatch tests.
- No test reconfigures a DSP stage in sequence (C1). No tone above 7 kHz (H2); pitch tolerance 1e-3 – 2e-3 (hides M1).
  The alias check measures only energy above 20.5 kHz, missing folded aliases below it.
- `test_view3d`: time reassignment is never exercised (a steady tone lands mid-frame with either sign); add an impulse
  at n0 and check `|t − n0| < 2`. ±10 Hz frequency tolerance could be ~1 Hz.
- `test_loudness`: everything at 48 kHz with 1 kHz tones (barely exercises the K-curve). Add the BS.1770 48 k
  coefficient table to 1e-9, the K response at 25 / 100 / 10 k Hz, cases 1–2 at 44.1 / 96 k, 3341 cases 9–12.
- `test_band_wave`: ±1.2 dB in-band and `Approx(-3.01).epsilon(0.1)` (±0.3 dB) are looser than LR4 needs (~0.6 dB);
  nothing above 48 kHz (M9). `test_spectrum`: `Approx(-2.97f).epsilon(0.25)` is ±0.74 dB.
- No tautologies or loosened golden thresholds found (goldens use the shared `GoldenTolerance`).

## UI / aesthetic (goldens at 41913c6f)

1. **Low-end black specks on the 3D / Stereo Image clouds** (`stereospectrogram-3d`, `-image`, `-pan`): PQ draws on
   near-black, where the colormap's black floor vanishes; pqdaw's teal well shows every faint point as a black dot, so
   the cloud reads as dirt. Fade the floor to transparent (L15).
2. **Snap chips don't show the active view** and are translucent (alpha 160) over the plot: in `waterfall-top` SPEC /
   SIDE / TOP look lit only because the orange image shows through. Draw the current view `on`, opaque chip backing.
3. **Label collisions**: Waterfall 3D frequency labels (`200 300 500`, `2k 3k 5k 10k 15k` overlap, `15k` clipped against
   the dB scale; no collision guard, faces review L10); Stereo Image's right-axis `10k` sits under FREEZE and `500 / 300`
   are clipped by the right edge (`device-meta-stereo`, `module-stereoimage-panel`); `stereospectrogram-pan` draws `-3s`
   under the chips and data runs through SET / FREEZE; Band Levels' red HiMid trace at the floor runs over the time
   labels and the right-edge dB labels are cut.
4. **Zoomed-out legibility** (`project-meta-loudness`, `project-meta-bandwave`): at project-patch zoom the Level Meter
   has no scale or values, the Loudness History no axis, the LUFS readout `-10.0` no unit; Band Levels' legend and the
   Waveform's `10 s ×1.00` / `MID` / `SIDE` are ~5 px. Chip text is 6 px (`analyser_face.h:392`) on all 15 faces. Pick
   one legible zoom-out summary per face (the big number + unit, or the bars with 3 ticks) instead of shrinking text.
5. **Missing axes**: Waterfall SIDE has no dB scale; TOP no colour legend; Stereo Spectrogram PAN labels only `C` and
   `R` (no `L`).
6. **Consistency**: chips, well, frame and sheet are shared across the faces (good); the Level Meter's content is a narrow
   column centred in an empty well (`device-meta-loudness`), unlike the others, which fill theirs.

## Gaps vs PQ Meter, ranked by user value

1. **Colour pickers** (a colour row type for `option_sheet.h`): asked for by every VZ worker (Q?-VZ.2-a, 4-a, 5-a, 6-a,
   7-a); one row type serves all 15 faces and unlocks Custom stop colours (G1's Silence fade is implemented but invisible
   without it).
2. **Kick/Sub Phase and Align** (deferred in the plan: needs a second tap input). The most production-useful PQ module not
   ported; the two-input analyser plumbing would also serve a reference / sidechain spectrum.
3. **The Loudness History scrolls through silence** (Q?-VZ.4-b chose to pause: a long silence shows as a 4–6 s gap, so the
   time axis lies). Step at 10 Hz only while a history face is visible: tiny cost, correct graph.
4. **FFT window choice** beyond Hann / Kaiser (PQ has six) and the Waterfall's 8× long window; cheap, as an analyser
   option.
5. **L/R and M/S overlay channels on the Spectrum** (Q?-VZ.2-a), useful for mix checks; the stereo feed already has both.
6. Tonal Reference (deferred: file library + decode); palette track / camera LFOs (pqdaw's modulation system is the right
   home); Waveform bar numbers on the scroll; Stereo Image group delay.
