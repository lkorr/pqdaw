# MM plan: meta modules (draft for user review)

Status: **draft 2026-11-01**. A planning agent wrote it from SPEC §3.1, §7.1–7.7, §11, §13 and §15 (2026-11-01 "meta modules", the binding decision record; also 2026-10-26 "no super modules / oversample deferred", which the 2026-11-01 entry reverses for user-facing blocks, and 2026-10-31 "the whole project is one circuit"), from the user's sketch (`IMG_4990.jpg`, "Wave table") and two reference screenshots (Live's Wavetable display; SeptaBee's phase generator), and from the code on `main` at 1032f28. Once approved, the tasks become `TMM.n` in TASKS.md.

MM = **a general meta-module framework**, then the modules built on it:
- **The framework**: a subcircuit with its own interactive vector face. It opens (expands) to its atoms, keeps its state in the document, has a text form, and draws and stays interactive at every zoom of the one circuit view. Placed directly in a track's chain, it is also a device whose face is its Device-view panel.
- **The modules**: the **Phase accumulator**, the **Wavetable** (Phase / Shape modes, keyframes with spectral morph, harmonic editor, draw tools, fold / warp / sync, one dot per voice, no audible aliasing) and an **Envelope** (needed by the last item).
- **A new "Meta Operator"** built from them, next to the untouched Live-parity Operator.

## What exists today (inventory from the code)

| Area | State on `main` | Where |
|---|---|---|
| `wavetable` atom | `wavetable(phase, table: const)`, Generator, stateful (`WavetableState`: float `prevPhase`/`prevDelta` to estimate the increment; min of the last two \|Δphase\|). Half-octave mip levels (`2·log2 N − 1`), adjacent levels crossfaded, **linear** interpolation via `gatherPair`, a `kSkip` fast path; mono path vectorised over time. No position input, one cycle per table. Bench: 1.66–1.91 ns/sample (L1), 10.3–13.3 ns per 8-voice frame (L8). | `src/graph/src/atoms.cpp:55,157`, `src/dsp/src/atoms.cpp:72`, `src/dsp/src/kernels_gen.cpp:146–179,521`, `src/dsp/include/pq/dsp/atom_state.h:98`, `bench/RESULTS.md:481–498` |
| Tables | `WavetableView{data, N (16..65536, power of two), levels, stride N+1}`, each level band-limited by a double FFT that zeroes harmonics above (N/2)·2^(−j/2), one guard sample. `BasicShape` Sine/Saw/Square/Triangle. 16 built-ins (Saw/Square 3–64 partials, Triangle, Noise Loop; 2048-point, lazily built) at `table ≥ kBuiltinWavetableBase` = 65536. `ShaperView{data, lo, scale, maxPos, segments}`. | `src/dsp/include/pq/dsp/tables.h`, `src/dsp/src/tables.cpp:61,181` |
| `shaper` atom | `shaper(x, table: const)`, stateless: clamp to [lo, hi] (NaN → lo), linear interpolation, 2 gathers. **No ADAA, no oversampling.** 0.58 ns/sample. | `src/graph/src/atoms.cpp:84,205`, `src/dsp/src/kernels_gen.cpp:282,362–389` |
| Phase between atoms | `phasor` keeps a **uint32** accumulator per lane (`PhasorState`), has `hz` and `reset` (sync) inputs, and outputs float `phase/2³²` from the top 24 bits. `sine` / `wavetable` read a float 0..1 phase. The JIT inlines phasor. | `src/dsp/include/pq/dsp/atom_state.h:66`, `src/dsp/src/kernels_gen.cpp:19`, `src/jit/src/codegen.cpp:1244` |
| Table resources | `engine::Resources{wavetables, shapers, samples, tunings}`. The `table` const becomes `op.resource` at schedule time and is **baked into the image** (interpreter `rec.args.resource`; JIT pointer-table slot, kernel called, not inlined). Nothing swaps a table at runtime. The app fills only `tunings`, so no project-defined table reaches the engine today. Lifetime precedent: tuning tables, ref-counted by ProjectLink, kept alive by `UnitImage::keepAlive` and freed through `EpochReclaimer`. | `src/engine/include/pq/engine/interpreter.h:40`, `src/graph/src/schedule.cpp:1560–1640`, `src/engine/src/image_host.cpp:83–97,217`, `src/jit/src/jit_image.cpp:54,192`, `src/engine/include/pq/engine/project_program.h:146`, `src/app/src/project_link.cpp:462` |
| Oversampling | **No native mechanism**: the `oversample {}` region is deferred (SPEC §15 2026-10-26, Q14 #9, Q121). `stdlib/util/oversample.pqp` has polyphase IIR half-bands (hiir allpass chains) as base-rate subcircuits; Saturator HQ / EQ Eight unroll the nonlinear path once per oversampled sample by hand. Saturator: 2.04 / 17.4 / 32.7 / 45.9 ns per stereo sample at 1/2/4/8×. `dsp/resample.h` has `decimate2` (sinc). | `stdlib/util/oversample.pqp`, `stdlib/devices/saturator.pqp:76–134`, `bench/RESULTS.md:904`, `src/dsp/include/pq/dsp/resample.h` |
| FFT and alias tests | `dsp::fftInPlace` (double, complex radix-2, off-thread) and `dsp::RealFft` (float, 8–65536). Tests: `harmonicMagnitude`, saw-wavetable spur test (harmonics to 16 kHz, Blackman-Harris 92 dB), `aliasingDb` (bin-exact sine, non-harmonic power to 20 kHz). | `src/dsp/include/pq/dsp/fft.h`, `tests/dsp/test_tables.cpp:17`, `tests/dsp/test_atoms_memory_gen.cpp:285–383`, `tests/stdlib/test_devices.cpp:469–497` |
| Adding an atom | `dsp` (`Atom` enum, state struct, `kInfo`, `laneStateInfo`, `silenceInfo`, `initState`, kernel + find function), `graph` (`atoms.cpp` ports + signature, `kernel_map.cpp`, `inspect.cpp` cost weights), `engine` (`resourceFor` for a new resource kind), JIT only if inlined; tests `test_atoms_*`, `test_lane_state`, `test_silence`, `test_exact_lanes`, `test_dsp_rt`, `tests/graph/test_atoms.cpp`; `bench/dsp/bench_atoms.cpp`. | as listed |
| Document: device data | `Device{id, patch, kind, active, params, settings, source (UserPatch text), rack, name}`. Only flat name → number maps plus patch text: **no per-device binary/table data**. Operator's drawn partials are 48 `@hidden` params. `SampleRef` (path + hash) is the only resource table; no blobs. **Schema 9**; `kBuiltinMigrations` has one step per version (3→9 are no-ops). Commands are variant structs; change classes Meta / Param / Sequence / Automation / Timeline / Structural; `dispatchCoalescing` for gestures. | `src/document/include/pq/document/model.h:405–452,655`, `serialize.h:64`, `src/document/src/serialize.cpp:684,1844,3195`, `commands.h:165–242`, `change_set.h`, `store.h:93` |
| "Runtime data, no recompile" | Only engine-owned images (`SequenceImage`, `AudioSequenceImage`, automation, tuning tables), published by atomic swap. None per device. | `src/engine/include/pq/engine/sequence.h`, `audio_sequence.h`, `src/app/src/project_link.cpp:786–836` |
| Lowering | Builds graph IR directly. A device is a subcircuit call `d<id>`; settings are literals (a change recompiles the track); params become runtime params `d<id>__<name>`, key `dev<id>:<name>`. Only **root-level `param`s** are device params (automation, macros, wires: `describeDevice`, `ParamRef::device`). User devices are renamed `<name>__u<id>_<hash>`. | `src/document/src/lower.cpp:1086–1098`, `src/document/src/devices.cpp:89,175,275,375`, `model.h:120–148` |
| Patch language | Attributes exist only on `param`s; there is no node-level metadata; calls resolve to same-file patch → imported/prelude patch → atom; tables as values are an open item. `ParseOptions::previous` keeps NodeIds across text edits. | `docs/PATCH_LANGUAGE.md`, `src/graph/include/pq/graph/patch.h:49,100`, `src/graph/src/parse.cpp`, `print.cpp`, `id_match.h` |
| Live values to the UI | Probes need a **recompile** (`ProbeTarget` in `ScheduleOptions::probes`, part of the compile-cache key); values go through `TapRing` (lossy seqlock, one writer / one reader, free with no reader) or `ProbeLevel`. Per voice: **only newest voice or the voice sum**, no per-voice-index probe. Spectrum: one `SpectrumFeed` worker per tap. | `src/graph/include/pq/graph/inspect.h:41,53`, `src/engine/include/pq/engine/inspect.h`, `src/core/include/pq/core/tap_ring.h`, `probe_level.h`, `src/views/src/devices/analysis.cpp` |
| UI renderer | Vulkan; DrawList in device px, **no transform stack**: AA rounded rects, circles, rings, arcs (butt/round caps), triangles, `addConvexPolyFilled` (convex only), gradients, `addBlob` glow (additive). `lines.h` (from PQ Meter): `smoothPolyline`, Catmull-Rom `splinePath`, `smoothBand`, `gradientPolyline/Band`, scope/spectrum looks with bloom. **Text = hinted bitmaps at integer px 4–256** (SPEC §11 forbids MSDF). Tile atlas (R8 coverage, 4 × 2048², tinted per draw) + CPU `rasterizeCoverage`. | `src/ui/include/pq/ui/draw_list.h:190–232`, `lines.h`, `text.h`, `tile_atlas.h`, `raster.h`, `ui_scale.h` |
| Circuit canvas | Zoom 0.02–8; Far < 0.3 (outlined boxes, whole picture cached in 256-px world chunks), Mid < 0.75 (symbols), Near (titles, pins). Symbol bodies are 2 coverage tiles per zoom level, rebuilt 0.12 s after zoom settles, stretched while zooming. **A symbol cannot draw per-instance live content; a node cannot host controls.** Hooks: `paintUnderlay/Overlay` (every level, outside the Far cache), `hitTest`, `onOpen`. **T4.2 partial: pan p99 2.91 ms at 2× fully zoomed out** (chunk building), never re-measured. | `src/views/include/pq/views/circuit/circuit_canvas.h:62–67`, `src/views/src/circuit/circuit_canvas.cpp:731–1048`, `symbols.h`, `bench/RESULTS.md:608` |
| Project patch view (the one circuit view) | `ProjectCanvas : CircuitCanvas`; node kinds Source, Sum, Device, Mixer, Output. **Precedents**: the mixer strip (interactive in-node controls painted in `paintOverlay`, hit by hand in `onPointer`, hidden at Far) and the lane windows (live content at every level, damage only on the playhead's columns). Double-click a closed device = open in place to its atoms (read-only); inside an opened device = Device-level editor; its title strip = close. | `src/views/include/pq/views/project_patch/project_patch_view.h`, `src/views/src/project_patch/canvas.cpp:118,273,288–330,515–560`, `lane_window.cpp` |
| Device panels | `<device>.panel.json` ("pqdaw-panel 1"): knob, box, toggle, chooser, label, tabs, `visual` (a hard-coded `PanelDisplay` class from `makeDisplay` / `makeOperatorDisplay`); absolute rects in a fixed size; selectors, `when`. `autoPanel()` fallback. Chain: one `DevicePanel` per device, `DocTarget` turns edits into commands. | `src/views/include/pq/views/devices/panel_desc.h`, `src/views/src/devices/device_displays.cpp:845`, `operator_displays.cpp:567`, `panel_internal.h:66`, `device_chain.h`, `device_chain.cpp:71,258,303` |
| Text pane | `ui::CodeEditor` + `PatchEditor` (text = the source of truth, one undo history) in the Device-level editor; the project view has none. | `src/ui/include/pq/ui/code_editor.h`, `src/views/src/circuit/patch_editor.cpp` |
| Operator | `stdlib/devices/operator.pqp`, 746 lines: `operator` (~26 structural consts: algorithm 1–11, waves 0–22, loop modes…), `op_bank` (4 × `phasor` + ½·Σ modulators), `op_wave` (a `mux` over sine, built-in tables, noise, 16-partial `op_partials`), 6 `msenv`, LFO, filter; panel with Envelope / Algorithm / Partials displays. | `stdlib/devices/operator.pqp:92–746`, `operator.panel.json`, `src/views/src/devices/operator_displays.cpp` |
| Goldens / REVIEW | `tests/views/golden/*.png` via named scenes (`view_scenes.cpp`, `pqdaw --screenshot <scene>`), `tests/ui/golden_check.h` tolerances; REVIEW entries "What to look at / To judge / Known gaps". | `src/views/src/view_scenes.cpp`, `tests/views/test_project_patch.cpp:1291` |

## Before starting

| Item | Why MM cares | State |
|---|---|---|
| T5.x-one-circuit 👁 review (Q165) | faces live in the project patch view; its open/close gestures (double-click inside = edit, title strip = close) are what TMM.4 extends | merged, review pending; not blocking |
| T4.2 pan p99 (2.91 ms at 2×, Far) | TMM.4 adds content at Far; its budget is measured against the same scene | partial; TMM.4 must not make it worse (criterion below), fixing it is not MM's job |
| T4.x-gate | orchestrator idle-machine benches | open; independent |

Nothing blocks: per-track units, hot swap, sleep, the tile atlas, the lane-window live-content pattern, device automation and the user library are merged.

## Decisions taken in this plan (SPEC §15 2026-10-30/11-01: decide sensibly)

**Architecture**
- **New module `meta`** (`pq_meta`, `pq::meta`, depends on `graph` + `dsp` + `core`). It holds the meta-module **type registry**, each type's immutable state struct, its **expansion generator** (state + context → a generated `graph::Patch` + resource requests), and the off-thread **table builder** worker class. `document` depends on it (state in the model, lowering), `views` depends on it (faces read state), and `app` runs the builder. `engine` never includes it; it only sees `dsp` tables through `Resources`. Faces live in `views` (`pq/views/meta/`). ARCHITECTURE §1 changes in TMM.3.
- **A meta module in text is an ordinary call** named `meta_<type>(…)`, e.g. `wt = meta_wavetable(acc, position: 0.25)`. This needs no grammar change: `meta_` is a reserved prefix, and the compiler's loader supplies the generated patch as a prelude (the existing "prelude given by the compiler" path). Arguments are **wires only**; an unwired control takes its value from the state. A typed literal is a constant wire: the face shows that control as fixed. Meta calls must be named bindings (the editor names them `acc1`, `wt1`, …) because **the name path is the state key**. `pqc` and standalone files expand with the default state.
- **State lives in the document, keyed by (device, name path)**: `Device::meta`, an immutable map from path to `MetaInstance{type, version, params, settings, data}`.
  - A chain-level meta device has `Device.patch = "meta:<type>"` and path `""`. Its controls are the device's own root params and settings (`Device.params/settings`), so automation, macros and wires work exactly as for any device (T4.18).
  - A nested instance (inside a device's circuit) keeps its values in `MetaInstance.params/settings`. Its params become runtime params `dev<id>:<path>.<name>`, so a face drag never recompiles.
  - Editing a meta module's state inside a **stock** device (e.g. drawing in the Meta Operator's wavetable) does **not** fork the device: only wiring/structure edits fork, as today.
  - Nested params are wire targets in their circuit. They are not automatable or macro-mappable in MM; that is listed as later work.
- **Schema 10**: a `"meta"` object per device. Table data is base64 float32 LE (one 2048-sample cycle ≈ 11 KB). The migration 9 → 10 is a no-op.
- **A new change class `MetaData`**: a state-data edit (keyframes, harmonics, drawing) never recompiles. The link hands it to the table builder, and the new table reaches the engine through a **table slot** (an atomic pointer swap with a short crossfade), the same way note edits reach the sequence image.
- **Expanded = generated atoms, read-only.** Double-clicking a face's frame opens it in place to its atoms (as a device opens today). The Wavetable expands to one `wavetable_pos` node with a position input, plus atoms for warp / fold / sync on the live path (SPEC: "only the table itself is not expressible as atoms"). Editing the expansion ("convert to plain subcircuit") is later work.

**DSP**
- **The table**: a `dsp::FrameTable` holds F dense frames (64 by default, up to 256) generated off-thread from the keyframes, × mip levels.
  - **Mip levels follow the "sr − 20 kHz" rule**: a level's top pitch keeps harmonics up to sr − 20 kHz, so any harmonic above Nyquist aliases to ≥ 20 kHz; level spacing r = min(2, (sr − 20k)/20k) (≈ half-octave at 48 kHz, ≈ 1.2 at 44.1 kHz, octave at 96 kHz). The audible band reaches ≥ 19.8 kHz at every pitch, so **no level crossfade is needed**: one level per lane per sample, from \|Δphase\| as today.
  - **Each level is stored 4× oversampled** relative to its own band (length = next pow2 ≥ 8·H_j). It is read with a **6-point 5th-order interpolator designed for 4×-oversampled data** (Niemitalo's "optimal" family, or equivalent: whatever the worker measures at ≥ 105 dB image rejection).
  - **Frames blend linearly** between adjacent dense frames.
  - Memory target ≤ 8 MiB per 64-frame table at 48 kHz. Identical specs share one table (hash cache).
- **Keyframe cycles are 2048 samples** (≤ 1023 harmonics + DC + Nyquist, enough for 20 Hz notes to reach 20 kHz). The canonical form stored is the **samples**; the harmonic editor is a view of them. An edit there writes the samples back through a double-precision iFFT and changes only the edited harmonics (others ≤ 1e-6), so drawing ↔ harmonics is lossless both ways.
- **Spectral morph** (the default per gap): per harmonic, magnitude interpolated linearly and phase along the shortest arc. **Crossfade**: linear in time. Selectable per gap.
- **Static warp / fold / sync are baked**. A control is static when it has no automation lane, no modulation wire and no macro mapping. A static control is applied to each dense frame at 16× resolution (bake aliasing ≤ −110 dB, checked against a 64× bake), then band-limited per level.
  - Turning a static knob rebuilds off-thread and swaps with a crossfade. It never recompiles: the param exists only as table data.
  - Adding automation, a wire or a macro to warp / fold / sync is a structural change. It recompiles the track onto the **live path**.
- **Warp modes**: Bend (an asymmetric power curve on the phase), Mirror (phase folded about ½, continuous), Pulse width (the two half-cycles squeezed to w and 1 − w, continuous). **Fold**: a sine wavefolder with gain 1 + k·fold. **Sync**: band-limited hard sync, the cycle read at (1 + 15·sync)× per period. The baked and live paths use **one formula set** (`dsp::warpPhase`, `foldValue`, `syncPhase`), shared by the builder, the live atoms and the tests.
- **Live path = a minimal `oversample(n) { … }` region in the language (TMM.6).** This builds the deferred SPEC §7.2 region in its smallest general form, because the 2026-11-01 decision requires audio-rate warp/fold/sync to be oversampled and future meta modules (shapers, the Meta Operator's optional HQ) need the same. The region:
  - is compiled as a sub-schedule at n× rate over n·frames buffers, with `sr` = n·sr inside, so stateful atoms and filters inside are correct;
  - upsamples audio-rate inputs through native polyphase IIR half-band kernels, and **phase inputs** (listed in `oversample(n, phase: p)`) by unwrapped linear interpolation (exact for ramps);
  - decimates the outputs.

  The alternative of hand-unrolling copies, as Saturator does, would make the expanded circuit unreadable.
- **Shape mode**: the input is clamped to [−1, 1] and mapped to table position u = (x + 1)/2, without wrap. The level comes from \|Δu\|. **First-order ADAA** uses per-frame antiderivative tables (accumulated in double; the frame blend is linear, so blending antiderivatives is exact), with the usual midpoint fallback when \|Δu\| is tiny. An optional oversample factor (default 2×) covers hard clamping and drive.
- **Phase precision**: phase stays a float 0..1 between atoms (24 bits from the uint32 accumulator). The resulting floor is ≈ a_h · h · 1.1e-7 per harmonic, so it is ≤ −100 dBFS for any spectrum with a_h·h ≤ 0.09 (any 1/h-or-faster spectrum at full scale). The alias tests use such tables. A uint32 `phase` signal kind is noted as later work, not built.
- **Per-voice dots without probes**. A **voice tap** (`pq/core/voice_tap.h`: a lossy seqlock record per voice group, written only while a reader is registered, so one relaxed load per block otherwise) carries per lane, per block: 8 phase samples spread over the block, the position, the increment and the voice slot.
  - The `wavetable_pos` atom writes it through its table slot.
  - A generic sink atom `voice_tap(x, slot: const)` serves the other modules (the phase accumulator's phase, the envelope's stage position).
  - No recompile is needed to show dots.
  - The face draws each voice's newest sampled phase as a glowing dot with a dotted vertical bar. Under it sits a faint additive **glow density**: a histogram of the ~100 sampled read phases per voice from the last displayed frame. This is honest about the 60 fps vs audio-rate problem: the dot is a sample, and the glow shows where the reads landed.

**UI**
- **Faces are vector code, not bitmaps.** `views::MetaFace` paints in world units through a `ui::FacePainter` (a world → device transform over DrawList and `lines.h`: polylines, arcs, rings, triangle-strip fills under curves, dashed lines, additive glow). DrawList has no transform stack, so the painter applies it.
  - **Text** is hinted bitmap at round(px · zoom) (SPEC §11), down to 4 px. Below that, a glyph run is drawn as a thin bar of its extent: legibility is physically gone, but nothing is removed from the layout. Faces are label-free by design (values in tooltips and the Info view), so this rarely matters.
- **Caching keeps the look identical.**
  - Each face splits into ≤ 3 **static single-colour layers** (frame, stacked frames, slider tracks). These are rasterised into coverage tiles at the exact zoom, rebuilt 0.12 s after zoom settles and stretched while zooming, as symbol bodies are now.
  - A **live layer** (dots, highlighted frame, knob values) is drawn as geometry each frame.
  - Faces are **excluded from the Far chunk cache** and painted at every level, including Far, so **zooming out removes nothing**.
  - Damage covers only a face's live rects, at ≤ 60 Hz while any of its voices sound; the idle cost is zero.
- **Interactive at every zoom.** Hit-testing and drags run in face-local world units, so a knob at zoom 0.05 still drags (Shift = fine). Wheel over a face goes to the face only where the face uses it (harmonic scroll, position); elsewhere it pans or zooms the canvas.
- **Phase accumulator face** (SeptaBee-like, our own): a circle, label-free.
  - The left arc is **frequency** (log, 0 at the bottom); the right arc is **amplitude**; the bottom arc is **phase offset**.
  - The centre is the output jack, ringed by per-voice phase dots.
  - The input jacks (`hz`, `pm`) are at the left.
  - The frequency law is f = 20 kHz · (2^(14t) − 1)/(2^14 − 1), so t = 0 gives exactly 0 Hz and t = 1 gives 20 kHz.
  - Amplitude range is 0–4, default 1.
- **Wavetable face** (the sketch, rotated upright):
  - Top: the amp and phase harmonic rows, and the Basic Shapes chooser with a small tool button.
  - Middle: the display.
  - Right: the vertical position slider with keyframe markers.
  - Below the display: the horizontal **phase** slider. It is the phase input's base value: a wired signal adds to it. With nothing wired in a voice context, the input is the module's own `phasor(voice.hz)`, so a lone Wavetable is an oscillator.
  - Bottom: the fold / warp / sync knobs, with the warp mode as a 3-glyph switch.
  - The input jack is top-left, the output bottom-right. A small Phase/Shape mode switch sits by the input jack.
  - The display follows Live's: up to 64 dense frames drawn as offset lines receding up-right, with the current blended frame highlighted in the theme's accent.

**Chain and devices**
- **Voice continuity in a chain.** Consecutive meta devices after the instrument position (the first device of a MIDI track) stay **per voice** until the first non-meta device or the chain end, where voices are summed. So "Phase accumulator → Wavetable" in a chain plays a polyphonic oscillator. After an audio-producing device, a new Wavetable defaults to Shape mode.
- **The Phase accumulator has no ratio control** (the user's design: frequency added in Hz). Its `hz` input (default `voice.hz`) takes `voice.hz * 3/2` where ratios are wanted. The Meta Operator does exactly this with exact n/d ratio params on its panel.
- **The Meta Operator's algorithm** is a structural setting (1–11, Live's), switching which `pm` wires exist. Switched-off wires are hidden by T4.15's dead-branch hiding, so the circuit shows only the live algorithm.
- **Name**: "Meta Operator" (catalog, Instruments).

## Tasks (in dependency order; one worker each)

**TMM.1 Frame tables: multi-frame band-limited wavetables + the off-thread builder** (`dsp`; `bench`). Depends on: —. **Parallel with TMM.3.**
- **`dsp::FrameTable`** (`pq/dsp/frame_table.h`) and **builder** (`frame_table_build.h`), with layout, mip rule, interpolator and frame blend as in Decisions.
  - `FrameTableSpec{keyframes [{pos ∈ [0,1], cycle[2048]}], gap modes, denseFrames, bake {warpMode, warp, fold, sync} (each optional), removeDc, normalise, sampleRate}` → `FrameTable`.
  - Built in double, with no allocation after the builder's arenas are sized.
  - Includes the shared formula set `warpPhase / foldValue / syncPhase` and `basicCycle` extended to Sine / Triangle / Saw / Square / Pulse 25 % / Noise (seeded).
- **Reference and fast readers**:
  - a scalar double reference reader;
  - AVX2 readers for L1 and L8: level per sample from \|Δphase\|, frame pair and blend, the 6-point interpolation;
  - a one-frame fast path (F = 1, or the position resting on a frame).

  These are exposed as functions that TMM.2's kernel wraps.
- **Accept:**
  - **Alias suite** (a reusable helper `tests/support/alias_meter.h`): bin-exact f0, rectangular window, every non-harmonic bin below 20 kHz counted. Inputs: saw, square, triangle, a saw → square spectral morph at 16 positions, and a 64-keyframe drawn fixture (smoothed random walks, normalised to 0 dBFS and filtered to a_h·h ≤ 0.09). Each is played at 25 log-spaced pitches 20 Hz – 12 kHz, at 44.1 / 48 / 96 kHz, including pitches straddling every level switch. Result: **every alias ≤ −100 dBFS or ≥ 20 kHz**, and every harmonic ≤ 19.8 kHz present within 0.1 dB.
  - The reader matches the double reference ≤ 1e-6.
  - Spectral morph at the midpoint of two single-harmonic frames with phases 0 and π/2 gives that harmonic at the interpolated magnitude and π/4.
  - A bake at 16× is within −110 dB of a bake at 64×.
  - Build time (release, one thread): 64 frames without bake ≤ 20 ms; with bake ≤ 150 ms. Memory ≤ 8 MiB for 64 frames at 48 kHz.
  - Bench rows (`bench_dsp`, ship): read L1 ≤ 4 ns/sample, L8 ≤ 30 ns/frame with position blend, ≤ 20 ns single frame.
  - Link `ship pq_bench_dsp` once (CLAUDE.md: kernel code).
- **CPU**: one level and two frames per lane (12 gathers) instead of today's two levels; the position blend is skipped when the blend weight is 0; static warp / fold / sync cost nothing at play time.

**TMM.2 `wavetable_pos` atom, live table slots, voice taps** (`dsp`, `graph`, `engine`, `jit`, `core`). Depends on: TMM.1.
- **Atom** `wavetable_pos(phase, position, table: const)` → `out`. Generator, stateful (previous phase and delta, the crossfade's old table pointer and countdown). All the files listed for "Adding an atom" are touched. The JIT calls the kernel, as for `wavetable`.
- **Table slots**:
  - `dsp::FrameTableSlot{std::atomic<const FrameTable*>, generation, VoiceTap}` is a new resource kind (`Resource::FrameTable`, `engine::Resources::frameTables`, `resourceFor`).
  - Slots are **owned by ProjectLink** (like tuning tables) and outlive images (`keepAlive`).
  - A writer publishes a new table pointer. The kernel notices the new generation at block start and crossfades old → new over 64 samples.
  - The old table is retired through `EpochReclaimer` after the audio thread's epoch has advanced past the crossfade.
- **Voice taps**: `pq/core/voice_tap.h` (format in Decisions), the generic sink atom `voice_tap(x, slot: const)` (`Resource::VoiceTap`, `Resources::voiceTaps`), slot allocation by lowering (the API used by TMM.3+), and the reader side (`VoiceTapReader`: newest record per voice slot, sampled-phase history).
- **Accept:**
  - Interpreter and JIT are bit-identical, and both match the TMM.1 reference ≤ 1e-6.
  - RtScope on process with swaps and taps.
  - A swap between two different tables mid-note: maximum sample-to-sample step ≤ the larger table's own maximum step + 1e-3 (no click).
  - 10⁶ random swaps in a two-thread stress: no use-after-free (ASan-style poisoning of retired tables in the test).
  - State transfer across hot swap; `test_silence` declaration (promises nothing).
  - Taps: with no reader, the bench is equal within noise. With a reader, ≥ 95 % of blocks arrive at 48 kHz / 64 frames, and the sampled phases equal the phases read (exact).
  - Bench rows for `wavetable_pos` and `voice_tap`; ship `pq_bench_dsp` link.
- **CPU**: one acquire load per block per slot; tap writes only with a reader; the crossfade path is a separate kernel loop taken only during the 64 samples.

**TMM.3 Meta-module framework: types, document state, lowering, text form; the Phase accumulator type** (`meta` new, `graph`, `document`, `stdlib` docs; ARCHITECTURE §1, "Document model"; PATCH_LANGUAGE §3.2 "Meta modules"). Depends on: —. **Parallel with TMM.1.**
- **`meta` module**:
  - `meta::Type` with: id, version, controls (params with ranges / curves / defaults / which are bakeable; settings), state struct + JSON codec hooks (the document serializer owns JSON), `expand(state, ExpandContext{voice context?, static-ness per control, slot ids}) → {graph::Patch, resource requests}` (deterministic, hash-stable), and a default state.
  - A registry, plus a test-only type.
- **Document**:
  - `Device::meta` map and `MetaInstance` as in Decisions; schema 10 + no-op migration; strict-reader errors with paths.
  - Commands: insert a chain meta device (`"meta:<type>"`); add, rename or delete a nested instance (with the patch editor's call insert / rename, so the state key follows the name); `SetMetaParam` (Param), `SetMetaSetting` (Structural), `SetMetaData` (new class **MetaData**).
  - Gestures coalesce into one undo step.
- **Lowering**:
  - Meta calls resolve to the generated patch `meta_<type>__d<dev>_<path>_<hash>`; nested params become runtime params `dev<id>:<path>.<name>`.
  - Static-ness (automation lane / wire / macro on a control) is passed to `expand`.
  - The link gets a per-track list of meta resources (slots to create or keep, data to build).
- **The Phase accumulator** (`phase_acc`):
  - Controls `freq` (law in Decisions), `phase` (0..1, wraps), `amp` (0–4).
  - Inputs `hz` (default `voice.hz` in a voice context, else 0) and `pm` (audio, added to the phase).
  - Expansion: `out = amp * frac(phasor(hz + freq) + phase + pm)`. The per-voice phasor resets at note-on, so `phase` is the start phase.
- **Accept:**
  - Save → load is deep-equal with nested and chain-level instances; a schema-9 file loads unchanged.
  - Each edit is one undo step (scripted).
  - Golden lowering texts (`doc::patchText`): a chain `meta:phase_acc`, and a nested `meta_phase_acc` in a user device.
  - At `freq` 0 the output is **bit-identical to `frac(phasor(voice.hz))`**; at 100 Hz, zero crossings are at note + 100 Hz (≤ 1e-4 ¢ over 10 s).
  - A `freq` drag on a nested instance compiles nothing (compile counter) and leaves the unit hash unchanged.
  - Renaming a nested instance keeps its state.
  - A standalone `.pqp` with `meta_phase_acc(...)` checks and renders in `pqc`.
- **CPU**: the expansion is phasor + 3 ops (≈ a phasor's cost, bench row); state edits are Param or MetaData, never Structural, except settings and static-ness changes.

**TMM.4 Faces: vector face toolkit, hosting in the circuit view at every zoom, expand; the Phase accumulator face** 👁 (`ui`, `views`). Depends on: TMM.3 (TMM.2 for the dots; until it merges, the face's dot layer is tested with a synthetic tap). **Parallel with TMM.2.**
- **`ui::FacePainter`** (`pq/ui/face_painter.h`): transform, the primitives in Decisions, snapped text with the 4-px bar fallback, and a static-layer recorder that renders into coverage tiles through `rasterizeCoverage`.
- **`views::MetaFace`** (`pq/views/meta/meta_face.h`) and a registry by type:
  - `worldSize`, `paintStatic(layer)`, `paintLive`, `liveRects`, `hitTest`, pointer / drag / wheel / hover, `tooltip` (Info view);
  - bound to a `MetaTarget` (commands; chain-level vs nested) and a `VoiceTapReader`.
- **Hosting** in `CircuitCanvas` / `ProjectCanvas`:
  - A meta node's block *is* its face (layout size from the face; T4.3 auto-layout respects it).
  - Painted at Far / Mid / Near, outside the Far chunk cache; static-layer tiles per exact zoom (settle + stretch); the live layer each frame; damage limited to live rects at ≤ 60 Hz while voices sound.
  - Pins stay on the face edges (jacks drawn as the face's own).
  - Double-click the face frame = open in place to atoms (read-only); its title strip closes again.
  - The Device-level editor's text pane shows `acc1 = meta_phase_acc(...)` and accepts typing one (two-way).
- **The Phase accumulator face** as in Decisions. Its dots come from a `voice_tap` on the output, which TMM.4 adds to the phase_acc expansion once TMM.2 has merged (otherwise in TMM.10).
- **Accept:**
  - Goldens `meta-phase-acc` at zoom 0.05 / 0.25 / 1 / 3 and 1× / 2× scale, plus `project-meta-faces` (a band with faces, the whole project fitted).
  - A test that the same primitives are emitted at every zoom (counts equal).
  - Tile vs direct drawing within the golden tolerance after settle.
  - TestDriver: frequency, phase and amp drags at zoom 0.05 and 3 each give one undo step with the right values; double-click reset; the expand / close gestures.
  - **Perf** (release, 2×, the M4 fixture plus 32 faces): pan median and p99 each within +0.3 ms of the same project with plain blocks; 16 faces × 8 animated voices ≤ 1 ms paint per frame; idle 0 % CPU.
  - REVIEW entry.
- **CPU**: static layers rasterised once per zoom settle; live geometry only for what moves; nothing repaints while silent.

**TMM.5 Wavetable module: state, keyframes, baked path, builder wiring** (`meta`, `document`, `app`). Depends on: TMM.1, TMM.2, TMM.3. **Parallel with TMM.6.**
- **The `wavetable` type**:
  - Data: `WavetableData{keyframes (1–256, pos + 2048-sample cycle), gap modes, denseFrames = 64, removeDc, normalise}`.
  - Settings: `mode` (Phase only here; Shape arrives in TMM.8) and `warpMode`.
  - Params: `phase` (the slider), `position`, `fold`, `warp`, `sync`.
  - Inputs: `phase` (default `phasor(voice.hz)` in a voice context) and `position` (added).
  - Expansion (baked): `wavetable_pos(frac(phase + in), clip(position + pos), table: slot)` plus the voice tap.
  - Until TMM.8, warp / fold / sync are **not offered** for automation, wires or macros (like T4.18's macro-mapped params). This is honest scope, not a stub.
- **App `MetaResources`** (in ProjectLink):
  - one FrameTableSlot per instance;
  - a builder thread (below normal priority, newest-wins per instance, specs hash-cached so identical presets share);
  - **offline renders wait** for pending builds (deterministic, as streamed samples block-fill);
  - knob changes on baked controls go to the builder, not to the param block.
- **Accept:**
  - A keyframe edit is audible without a recompile (compile counter 0): ≤ 30 ms for 64 frames, ≤ 160 ms with bake (release, indicative).
  - The swap is click-free (TMM.2's step bound, end to end).
  - A baked fold drag gives no recompile, one undo step, and audio follows.
  - Save → load keeps cycles bit-exact; 64 keyframes save + load ≤ 20 ms.
  - Render golden `wavetable-morph` (saw → square spectral sweep, crossfade gap, baked warp in each mode).
  - The alias suite on the **module output** (Phase mode, baked warp / fold / sync at 5 values each): ≤ −100 dBFS or ≥ 20 kHz.
  - A Wavetable with static knobs costs the atom + phasor (bench row, ship).
  - RtScope through a keyframe edit while playing.
- **CPU**: bakes off-thread; shared tables for identical specs; no recompiles for data or baked knobs.

**TMM.6 Minimal `oversample(n) { … }` region** (`graph` parser / printer / infer / schedule, `engine` interpreter, `jit`, `dsp` half-band kernels, `views` region frame; PATCH_LANGUAGE §2 + §3; QUESTIONS note about SPEC 2026-10-26). Depends on: —. **Parallel with TMM.5** (or earlier whenever a worker slot is free).
- **Grammar**: `oversample "(" Int [ "," "phase" ":" Ident { "," Ident } ] ")" "{" { binding } "}"`, a patch member, with n ∈ {1, 2, 4, 8, 16}. Names bound inside are readable outside (decimated); outer audio-rate names read inside are upsampled (`phase:` names by unwrapped linear interpolation); param / const inputs pass as they are. `sr` inside = n·sr. Regions may not nest in MM.
- **Compiler**:
  - the region becomes a sub-schedule over n·frames buffers (per-voice lanes too);
  - stateful atoms and feedback SCCs inside run at the region's rate;
  - converters are native AVX2 polyphase IIR half-band kernels (`os_up2` / `os_down2` cascades; coefficients designed for ≥ 110 dB stopband over the band that folds below 20 kHz at 44.1 kHz and above);
  - n = 1 compiles to the plain nodes;
  - layout keys and NodeIds are stable;
  - both backends.
- **The circuit view** draws a region as a framed group labelled "×n" (T4.3 layout, T4.4 editing: select nodes → "Oversample ×n"; text two-way).
- **Accept:**
  - parse → print → parse round trip;
  - a region holding `tanh(x · 8)` at 4× / 8× matches a double reference (upsample → shape → decimate with the same filters) ≤ 1e-5, and aliasing of a 5 kHz full-scale sine is ≤ the half-band spec;
  - a `phasor` inside a region at 4× keeps exact pitch;
  - interpreter and JIT bit-identical;
  - RtScope;
  - converter cost per channel-sample recorded (target ≤ 1.5 ns per ×2 stage, L8 per lane);
  - group delay per factor documented;
  - Saturator and EQ Eight untouched (their goldens unchanged).
- **CPU**: only the region runs at n×; params are not upsampled; converters vectorised over voice lanes.

**TMM.7 Wavetable face I: display, voice dots, position + keyframes, phase slider, knobs** 👁 (`views`). Depends on: TMM.4, TMM.5. **Parallel with TMM.8.**
- **The face as in Decisions.**
  - The **display** draws the stacked dense frames (a static layer) and the current blended frame highlighted (live).
  - **Per voice**: a glowing dot on the highlighted curve at the newest sampled read phase, a dotted vertical bar, and the glow density from the last frame's samples. Voices fade in and out over 80 ms when lanes start or stop.
  - The **position slider** shows keyframe markers: click selects, drag moves (snaps to dense frames with Ctrl), double-click on the track adds a keyframe from the blended frame, Delete removes (never the last one). A small glyph in each gap toggles spectral ↔ crossfade.
  - The **phase slider** carries per-voice ticks when wired.
  - **Knobs** for fold / warp / sync, with the warp-mode switch.
  - The **harmonic rows** show the current keyframe's amplitude and phase (read-only here; editing is TMM.9). The **Basic Shapes** chooser replaces the selected keyframe's cycle (one undo step).
- **Accept:**
  - Goldens `meta-wavetable` (zoom 0.1 / 0.5 / 1 / 2, 1× / 2×) and `meta-wavetable-voices` (8 voices from a NullDevice-driven engine, dots at their sampled phases).
  - TestDriver for every gesture (one undo step each).
  - Taps → dots latency ≤ 1 displayed frame.
  - Paint: live layer ≤ 0.3 ms per face at 2× with 64 frames and 8 voices; 16 animating faces ≤ 1.5 ms per frame; idle 0 %.
  - REVIEW entry (with the Live screenshot as the reference for the stacked look).

**TMM.8 Live audio-rate warp / fold / sync, Shape mode with ADAA, the alias proof** (`dsp`, `meta`, `graph` atoms if needed, `views` mode switch). Depends on: TMM.5, TMM.6. **Parallel with TMM.7.**
- **Live path**: when warp, fold or sync is modulated, the expansion becomes `oversample(os, phase: ph) { y = fold(wavetable_pos(warp(sync(ph)), pos)) }` built from atoms. Warp, fold and sync use the shared formulas as subcircuits in a new `stdlib/meta/warp.pqp`, or new atoms if a formula is not expressible cheaply. `os` is a setting (2 / 4 / 8 / 16, default 4).
  - **Sync's phase jump** gets a band-limited step correction (a minBLEP residual of the table's value jump at the oversampled rate). Bend, mirror, pulse width and fold are continuous and rely on oversampling alone.
  - warp / fold / sync become automatable, wireable and macro-mappable. The baked ↔ live switch follows static-ness (a structural recompile only when a lane, wire or macro is added or removed).
- **Shape mode** as in Decisions:
  - antiderivative tables built by the TMM.1 builder (extended), the ADAA read (new atom `wavetable_shape(x, position, table)` or a mode of `wavetable_pos`; the worker decides, documented);
  - the `os` setting;
  - the face's Phase / Shape switch and the display drawing dots at the input value.
- **Accept:**
  - Live and baked paths agree below 20 kHz within −80 dB at static values (same formulas).
  - The alias suite, measured against a 64× double-precision reference render (bins compared below 20 kHz), with warp / fold modulated by a 1–5 kHz sine at full depth at `os` 4: **≤ −100 dBFS or ≥ 20 kHz** for bend / mirror / pulse width / fold. For audio-rate **sync**: measured, target ≤ −100 dBFS at `os` 8; the achieved number recorded and a QUESTIONS entry if short.
  - Shape mode, sine input 100 Hz – 5 kHz at amplitude ≤ 1 through saw / square / drawn-fixture curves, `os` 2 + ADAA: ≤ −100 dBFS or ≥ 20 kHz. Driven past ±1 (amplitude 4, `os` 4): measured and recorded (target ≤ −90 dBFS).
  - Bench rows (ship): live path per `os`; Shape mode ADAA vs plain `shaper`.
  - RtScope; ship `pq_bench_dsp` link.
- **CPU**: the live path is paid only while something modulates; only the region's nodes run at `os`×; ADAA costs one extra table read per sample.

**TMM.9 Wavetable face II: harmonic editor, draw tools, basic shapes** 👁 (`views`, `meta` data edits). Depends on: TMM.7.
- **Harmonic editor (Serum-like)**:
  - two rows of bars, amplitude (linear; Alt shows dB) and phase (−π..π, centred);
  - 32 harmonics visible by default; wheel scrolls and Ctrl+wheel zooms, up to all 1023 (+ DC shown as its own bar);
  - drag paints across bars, Shift draws a line across them, right-click / double-click resets a bar;
  - an edit writes the keyframe's cycle through a double iFFT (only the edited harmonics change).
- **Draw tools** (the small button by Basic Shapes picks one): pencil (freehand, linear between pointer samples), line, smooth curve (Catmull-Rom), and a snap grid (off / 8 / 16 / 32 columns, y steps), with optional normalise and remove-DC.
  - Drawing works on the selected keyframe. If the position lies between keyframes, the first stroke creates one there from the blended frame.
  - Each stroke is one undo step.
- **Basic shapes**: Sine, Triangle, Saw, Square, Pulse 25 %, Noise.
- **Accept:**
  - Lossless both ways: draw → open the harmonic view → back leaves the cycle byte-equal; a harmonic edit changes no other harmonic beyond 1e-6.
  - Zoom to 1023 harmonics and back.
  - Stroke → audible table ≤ 30 ms (indicative).
  - Goldens `meta-wavetable-harmonics` (zoomed to 256) and `meta-wavetable-draw`.
  - Harmonic rows paint ≤ 0.2 ms with 512 bars visible.
  - TestDriver gestures; REVIEW entry.

**TMM.10 Meta modules as devices: chains, Device-view face panels, voice continuity, menus, presets** 👁 (`document` lowering, `views` devices + project view, `library` categories). Depends on: TMM.4, TMM.5. **Parallel with TMM.9.**
- **Adding**: the device "+" menu and browser get a **Meta Modules** category (Phase Accumulator, Wavetable; Envelope after TMM.11). Inserting into a band in the project view makes a chain device; inserting inside a Device-level editor makes a nested instance (forking a stock device as today).
- **Device view**: a chain-level meta device's panel is its face at scale 1 (a `FacePanel` host, device width from the face), under the usual title bar (on / off, name, presets, hot-swap, automation LEDs on the face's controls via T4.18). Panel JSON gains `{"type": "face", "meta": "<path>", "rect": […]}` so a device can show nested faces (used by TMM.12).
- **Voice continuity and defaults** as in Decisions (lowering).
- **Presets / library**: `.pqpreset` carries meta state and data. Copy, duplicate and paste of devices copy state. Hot-swap works.
- **Accept:**
  - A MIDI track "Phase accumulator → Wavetable → Auto Filter" plays an 8-voice chord with each voice's pitch exact (≤ 1e-4 ¢ vs the resolved Hz, in 31edo).
  - Goldens `device-meta-chain` (Device view) and `project-meta-chain` (band).
  - Library round trip deep-equal; hot-swap keeps wires.
  - Automation on a chain-level Wavetable's fold switches to the live path (one recompile) and back when the lane is deleted.
  - TestDriver; REVIEW entry.

**TMM.11 Envelope meta module** 👁 (`meta`, `views`, `stdlib` expansion via `msenv`). Depends on: TMM.4 (TMM.2 for dots). Can run beside TMM.9 or TMM.10.
- **The `envelope` type**:
  - Params: attack, decay, sustain, release, a curve per segment, amount.
  - Inputs: `gate` (default `voice.gate`) and `in` (when wired, out = env · in: a VCA, which the Meta Operator uses).
  - Expansion through `msenv` (no new DSP).
- **Face**: label-free curve with draggable breakpoints (log time horizontally, level vertically); Alt-drag bends a segment; one dot per voice riding the curve (`voice_tap` of stage and position); gate and in/out jacks.
- **Accept:**
  - Renders equal `msenv` with the same settings (bit-identical).
  - Goldens `meta-envelope` (zooms, 8 voices in different stages).
  - TestDriver; perf as TMM.4; REVIEW entry.

**TMM.12 Meta Operator** 👁 (`stdlib/devices/meta_operator.pqp` + `.panel.json`, `document` catalog, `views`, `tests/stdlib`, `bench`). Depends on: TMM.8, TMM.9, TMM.10, TMM.11.
- **Four operators.** Each is a Phase accumulator (`hz = voice.hz * ratioN / denN`; exact n/d ratio params plus Fixed Hz), then a Wavetable (default sine; drawing and harmonics from the module), then an Envelope used as a VCA.
- **Wiring**:
  - Modulation: modulator outputs × index go into the carriers' `pm` inputs, switched by `algorithm` 1–11 (structural; Live's topologies); feedback on D through `z1`.
  - Output: the sum of carriers, then `voices`, glide and spread.
  - No filter, LFO or pitch envelope: put Auto Filter after it; an LFO is a Phase accumulator with `hz` 0 into a Wavetable, wireable anywhere.
- **An `hq` setting** wraps the operator core in `oversample(2)` (FM sidebands alias by nature; measured).
- **Panel**: the four operators' faces in columns plus ratio boxes and the algorithm chooser.
- **Circuit**: at the device level ≤ 20 visible nodes (12 faces + ratio multipliers + Σ + out). The Live-parity Operator's count is recorded for comparison.
- **The existing Operator is untouched.**
- **Accept:**
  - A C++ reference (`tests/stdlib/meta_operator_reference.h`) ≤ 1e-5 on both backends.
  - Pitch exact in 31edo and a MOS.
  - 64 voices (ship, `bench_render operator/meta-*`) ≤ 1.2 × the stock Operator's cost with sine waves, recorded with drawn tables and `hq`.
  - Existing Operator goldens and tests unchanged.
  - Goldens `meta-operator-device` and `meta-operator-circuit` (band opened in the project view).
  - Presets save and load; REVIEW entry.

**TMM.13 MM review** (an independent agent, as for M0–M5). Includes a whole-suite `-Full`, ship benches for the frame-table reads / live path / Meta Operator, a re-run of the alias suite, and a UI critique against "ui is everything here".

## Parallel pairing (≤ 2 workers)
1. **TMM.1 ∥ TMM.3** (dsp tables vs meta framework + document; disjoint files)
2. **TMM.2 ∥ TMM.4** (atom / engine / JIT vs ui / views faces)
3. **TMM.5 ∥ TMM.6** (wavetable module + app builder vs the oversample region in the compiler). TMM.6 has no dependencies and can take any free slot earlier.
4. **TMM.7 ∥ TMM.8** (face vs live DSP)
5. **TMM.9 ∥ TMM.10**, then **TMM.11** (beside whichever finishes first)
6. **TMM.12**, then **TMM.13**

## Questions for the user (provisional choices are in Decisions; work proceeds on them)
1. **The Phase accumulator's amplitude multiplies the phase** (as written: "a multiplier on the output"). At integer values it is a clean frequency multiple. At non-integer values (e.g. 0.5) the Wavetable reads part of the cycle and then jumps back, which is a hard-sync-like discontinuity that aliases. The Wavetable cannot band-limit it, because it doesn't know where the jump comes from.
   - **Provisional**: build it as written, have the face snap amplitude to integers with Ctrl, and record the alias level for non-integer values. The Wavetable's **Sync** knob is the alias-free way to read part of a cycle.
   - **Alternative**: amplitude as an output gain applied after the Wavetable reads.

   Which do you want?

## Risks
- **Frame budget**: faces add paint work in a canvas whose Far pan p99 already misses 2 ms. Mitigations: static layers in tiles, the live layer only where voices move, Far chunks unchanged; TMM.4's criterion is a delta against the same scene.
- **−100 dB in every case**: phase precision (float 24-bit), audio-rate sync and hard-clamped Shape mode are the weak spots. The plan states the bounds, measures them and records shortfalls instead of claiming them.
- **Table memory**: 64 frames × levels × 4× oversampled storage ≈ 8 MiB per distinct table. Hash sharing covers copies; 50 distinct tables ≈ 400 MiB would be a problem worth knowing about (recorded in RESULTS).
- **The region in the scheduler / JIT** (TMM.6) is the largest compiler change since per-track units. It is kept minimal (no nesting) and tested against a double reference on both backends.
- **State keyed by name path**: a hand edit in the text pane that renames a meta binding loses its state unless the editor's rename runs. Mitigation: the text pane matches renamed meta calls through `ParseOptions::previous` NodeIds and carries the state (TMM.3 test).
