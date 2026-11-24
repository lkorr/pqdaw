# SM: the Sampler (a sample instrument with a built-in granulator)

User 2026-11-20: "A sampler meta module; dragging a sample onto the circuit grid places the sample there as a cute little
waveform in an oval that encompasses it. It receives an input frequency node to play via keyboard, and basic
warping/chopping like Ableton (function pretty much like Ableton's sampler in warping and slice mode). A granulator built
in that you can switch to. Simple in design, then open up additional options, expanding to reveal further settings. Same
theme/design/UX as the rest of the meta modules and visualisers."

Decisions (asked 2026-11-20):
- **One sample per module**: Simpler parity (Classic / One-Shot / Slice) + a Granular mode. Multisample zones later.
- **A group preset** (one hierarchy, SPEC §15 2026-11-07): `ModuleKind::Sampler`, catalogue item `module:sampler`, an
  instrument group (`instrument = true`) holding the native sampler engine (`meta:sampler`, a leaf; the GU.7 pattern of
  `modules.h` "modules around a NATIVE ENGINE") + the existing Envelope, an EQ band (svf filter) and an LFO, pre-wired,
  rewirable when the group is opened. The expanded panel embeds their panels (GU.6).
- **Slice selection by key**: slice n = key 36 + n, from a new per-voice signal `voice.key` (SM.1), independent of Hz.
- **Warp per voice, all modes** (Re-Pitch, Beats, Tones, Texture, Complex, Complex Pro), no cap beyond the group's voice
  count; stretchers preallocated per voice lane off-thread, only while the warp setting is Complex / Pro.
- **Granulator** (Granulator III-like): Classic / Loop / Cloud; position, scan, grain size, density, spray (time / pitch /
  pan), shape, grains per voice. Extras: Freeze, Harmonic grains (exact n/d ratios of the played note), a dot per live
  grain on the waveform. No live-input granulation.
- **Root pitch**: detected on drop, editable, shown as Hz or nearest identity in the track's tuning; unpitched →
  261.63 Hz. Rate = `voice.hz / rootHz · fileRate / sr` (SPEC §9 Simpler row).
- **Slicing** by transient / beat / region / manual + "Slice to Notes".
- **Drop** (user 2026-11-20, round 4): a sample dropped anywhere on the circuit grid places a sampler there, on any band
  (MIDI, audio, return), like a phase accumulator places. It auto-wires `notes → sampler → out` only on a MIDI band
  whose notes path has no instrument; everywhere else it lands unwired. On an audio track it is still a full module:
  it plays when something drives it (notes wired from another track's lane, a gate, or an LFO scrubbing its Position
  input). A drop on an existing sampler replaces its sample.
- **Expand ≠ Open** (approved): the chevron (or a double-click on the waveform) grows the settings panel in place; Open,
  as for every group, shows the inner circuit for rewiring.
- **Slice key for drawn notes** = 60 + absolute degree (or the `.kbm` key) — approved.
- Plan + workers in two lanes (w1 / w2) after user approval.

## Where it sits (what exists in code, what is new)
- **The group** is built like the Wavetable Synth (`modules.h:146`, `defaultInstrumentBlocks` `modules.h:201`):
  `In (Hz) → Sampler engine → EQ band (svf) → Envelope → Out`; `LFO → (Units wire) → EQ band Frequency`;
  `Sampler.gate → Envelope gate` (One-Shot: the engine's gate out stays high until the sample ends). The engine's
  `done` (Type::voiceEnd) ends the voice in One-Shot; otherwise the Envelope's `done`.
- **The engine** is a meta type `sampler` (`registry.cpp`). Expansion: `sample_read_*` (`dsp/atoms.h:167`) for
  Classic / One-Shot / Slice with warp Off / Re-Pitch; new native atoms `sample_slice` (key → slice bounds),
  `sampler_warp`, `granular`, each chosen by a SETTING (mode / warp-mode changes recompile and hot-swap, so unused paths
  cost nothing — the MX op-swap rule).
  - Inputs (general, so it works without a note source — the user's audio-track case): `hz` (default = root Hz:
    plays at the recorded pitch), `gate` (a rising edge triggers, as a voice gate), `key`, and **`position`** (unwired:
    the engine's own playhead; wired: a 0…1 scrub position read directly, e.g. from an LFO, with the rate taken from
    its slope — in Gran it sets the grain position). No special case per band: a sampler with nothing wired is silent.
  - Params (runtime, automatable): start, end, loop start / length / fade, transpose, detune, gain, root Hz, warp
    params, granular params, freeze.
  - Settings (structural): mode, warp mode, granular mode, grains per voice (1…32), loop on, harmonic grains on.
  - Data (no recompile): `{sample: SampleId, slices: Floats (frames), sliceBy, sensitivity, markers: Floats (frame,
    beat pairs), sampleBpm, root identity text, harmonics: n/d list}`.
- **Sample access is new.** `engine::Resources::samples` (`interpreter.h:47`) is filled only by tests;
  `dsp::SampleView` (`tables.h:126`) is mono and fixed at image build; `meta::ResourceKind` (`type.h:110`) has no
  Sample. SM.2 adds `ResourceKind::Sample` → a **`dsp::SampleSlot`** modelled on `FrameTableSlot` (atomic newest entry:
  view, channels, file rate, mips, slice / marker tables, generation, retired via the EpochReclaimer). `sample_read`
  reads the slot (no compat path). Replacing a sample = a data edit publishing a new entry; voices take it at their next
  trigger.
- **Residency**: a sampler's sample is PINNED resident in `media::SamplePool` (today the pool streams > 32 MiB and
  demotes under budget, `sample_pool.h:58`). New `pin` / `unpin` exempt from demotion; length cap 10 min (refused with
  a status message beyond). Mips built at pin time so reads above rate 2 use the band-limited 2^L copy.
- **The warp engine is clip-timeline code.** `ClipPlayer` (`clip_player.cpp:1249–1904`) renders Beats / Tones /
  Texture / Complex per clip voice against song-time laws, constant pitch ratio, streamed readers. SM.4 extracts the
  per-voice part into **`dsp::WarpVoice`** (slice / grain / vocoder scheduling over a resident source, per-sample rate
  input); the clip player becomes its first client with bit-identical renders, `sampler_warp` the second. `StretchPool`
  moves from `pq_engine` to `pq_dsp` (`pq_signalsmith` is engine-private today, `engine/CMakeLists.txt:43`).
- **Voice key (SM.1)**: no MIDI view exists in code (no `key` on `NoteEvent`, `note_events.h:79`). Add it like T4.17's
  `noteDegree`: `NoteEvent::key` → `voices.cpp` `onsetKey_` (next to `onsetDegree_`) → atom `voice.key` (param rate).
  Key = raw key for live input (`project_link_input.cpp:293`); for drawn Degree notes 60 + absolute degree, or the
  lowest `.kbm` key mapping to that degree (the `midi_export.h` Degrees rule, so import / export round-trips); for Cents /
  Ratio notes the nearest 12-TET key; the arp recomputes it from its new `noteDegree`, else from the new Hz. One
  function `doc::midiKeyOf` shared by lowering (`sequence.h:111`), MIDI export and later VST MIDI.

## Semantics (orchestrator defaults; note any change as a Q)
- **Classic**: polyphonic; gate holds the sustain loop. Loop Off / Forward / Ping-pong. **Loop fade**: a new `fade`
  input on `sample_read` (0 = today's exact path; > 0 = equal-power crossfade of the last `fade` frames before loopEnd
  with those before loopStart, a separate loop taken only inside the fade).
- **One-Shot**: gate ignored, plays start → end, `done` at the end; fade in / out ms; Trigger / Gate mode.
- **Slice**: `sample_slice(voice.key)` → slice n = key − 36 at note-on (out of range: silent). Rate = transpose only ×
  `voice.hz / voice.noteHz` (glides / bends still move it). Playback Mono (choke; default) / Poly; Trigger / Gate.
- **Re-Pitch, warp on**: rate = `(bpm / sampleBpm) · voice.hz / rootHz`.
- **Beats / Tones / Texture / Complex / Pro**: source position follows tempo from note-on (content beats since trigger
  → markers' piecewise-linear law → source frame; `transport.bpm`, stopped = song tempo). Pitch rate r =
  `voice.hz / rootHz` per sample (glides continuous): slice / grain readers integrate r; Complex resamples the vocoder
  output at r. Parameters as the clip's (granulation, envelope, grain size, flux; formants / envelope order for Pro).
- **Complex note-on**: a stretcher needs ≈ 70 ms priming; a voice starts as Tones and crossfades to Complex over 2 ms
  once primed (the existing stretchRetries handover), priming spread at kStretchPrimeRate, never a burst.
- **Granular** (`granular` atom; ≤ G grains per lane; Hann / Tukey / Gauss / Triangle / Exp from a 1024-point table):
  - Classic (tonal, pitch-synchronous): grains every 1 / `voice.hz`, read at source rate × transpose; size = formant.
  - Loop: spawn at density Hz, read at `voice.hz / rootHz`; position moves by scan, wraps in [start, end].
  - Cloud: as Loop with Poisson onsets and per-grain random position over the spray range.
  - Spray time (± ms) / pitch (± st) / pan; randomness a counter hash of node id, voice, grain number (repeatable).
  - Freeze: scan stops, position holds, grains keep spawning.
  - Harmonic grains: each new grain takes the next ratio from the list (default 1, 2, 3/2, 3, 4, 5); rate =
    `(voice.hz · n / d) / rootHz`, n and d integers until the final float32 multiply.
- **Root display**: Hz or nearest identity + cents remainder; stored as `root` Hz + identity text in data.

## Face (👁)
- **Closed** (≈ 150 × 84 units): an OVAL plate (new `FaceShape` ellipse case with rx / ry for ports, hit-test, plate;
  round-only today, `meta_face.h:237`). Inside: the waveform (`views/waveforms.h` overview, static layer keyed on sample
  id + start / end); a glowing playhead per sounding voice (VoiceTap written by the engine, like the wavetable dots via
  `tap_voices.cpp`) or grain dots in Gran; three mini knobs on the lower arc by mode (Transpose · Filter · Volume, or
  Position · Size · Density); an Expand chevron; In / Out ports on the oval.
- **Expanded** (Expand ≠ Open, see conflicts): grows in place from its top-left. Large waveform (start / end / loop
  handles, fade shade, draggable slice lines with double-click add / remove, warp markers on a beat ruler, grain region
  with live dots); mode row [Classic | 1-Shot | Slice | Gran]; fold sections Sample (root, gain, transpose, loop), Warp,
  Slices, Granular, Envelope / Filter / LFO (embedded). Only Sample open by default; fold state is view state; sections
  that don't apply to the mode are hidden.
- **Panel mechanism (general, SM.9)**: a `fold` panel element (header owning elements up to the next fold) and
  `Panel::fit` (shown height = sum of open sections). The project patch re-lays out a block whose face size changes
  (BR.H's "face settling its size" path), keeping the stored top-left.
- **CPU**: a hidden / offscreen face deregisters its tap readers (audio side: one relaxed load); static layers cached;
  paint only while voices or grains are live.

## Tasks
### SM.1 `voice.key` (document + engine + graph) · w1
`NoteEvent::key` (int16, −1 unknown), `doc::midiKeyOf`, sequence / live input / arp plumbing, `voice.key` atom.
- Accept: Degree / Cents / Ratio notes, a `.kbm` tuning and live keys give the keys above; arp steps carry their key;
  `midi_export` uses the same function (its tests unchanged).
- Verify: `-Affected -Slow`.

### SM.2 Sample slot, pinning, `sample_read` on slots, the engine type (dsp + media + meta + app) · w1, after SM.1
`dsp::SampleSlot` + `ResourceKind::Sample`; link resolves the SampleId, pins, builds mips, publishes. `sample_read`:
slot input, stereo out (mono duplicated by copy), `fade` input, mip level per call from |rate|. `sample_slice`; the
`sampler` meta type (Classic / One-Shot / Slice with warp Off and Re-Pitch). `Type` hook `sampleRefs(data)` so
`missingSamples` / `planCollect` / relink count sampler uses (`samples.h:78` counts clips only).
- Accept: C++ reference per mode (loop fade, ping-pong, slices by key) on both backends; mid-note swap click-free,
  effective at next trigger; RtScope over 10k blocks with note-ons, swaps, slice publishes. **Re-Pitch stereo ≤ 1.1 ×
  two of today's mono `sample_read_cubic`** (pq_bench_dsp, ship).
- Verify: `-Affected -Slow` + ship `pq_bench_dsp` link.

### SM.3 The Sampler group preset (document) · w2, after SM.2
`ModuleKind::Sampler`, `samplerBlocks(ids, SamplerOptions, SamplerRoles*)`, spawned everywhere the Wavetable Synth is;
`ops::setSamplerSample` (data + root + slices, one undo step).
- Accept: save / load / undo round-trip; plays through the real Engine path (pq_tests_playback); inner rewiring works.
- Verify: `-Affected`.

### SM.4 `dsp::WarpVoice` extraction + `sampler_warp` Beats / Tones / Texture / warped Re-Pitch (dsp + engine) · w1, after SM.2
Move the per-voice Beats / Tones / Texture renderer out of `clip_player.cpp` into dsp (per-sample rate input,
note-on-anchored law); the clip player uses it. `sampler_warp` atom with markers / transients from the slot.
- Accept: every existing clip-player warp test / render **bit-identical**; sampler warp renders equal at block sizes
  32 / 64 / 512 / 37; tempo-sync drift 0 over 64 bars at 120 → 97 BPM; glides continuous; RtScope.
- Verify: `-Affected -Slow` + ship link.

### SM.5 Complex / Complex Pro per voice (dsp + engine + app) · w1, after SM.4
`StretchPool` → `pq_dsp`; one stretcher per voice lane per instance, built off-thread when the warp setting is
Complex / Pro (voices × channels, adopted before the image goes live, freed otherwise), shown in `device_cost.h`.
Note-on Tones → Complex handover; per-voice phase offsets spread spectral work.
- Accept: no audio-thread allocation (RtScope, 10k blocks of notes + glides); pitch < 0.5 ¢ at +7 st in 31-EDO;
  16 voices measured in `bench/RESULTS.md` (≈ 1.6 % of a core per voice expected, T6.5 row); a Classic project
  allocates no stretcher.
- Verify: `-Affected -Slow` + ship `pq_bench_dsp`.

### SM.6 `granular` atom + GrainTap (dsp + core + meta) · w1, after SM.2
Three modes, spray, shape, freeze, harmonic grains. SIMD: 8 grains per AVX2 op (cubic gathers, both channels), only
live grains (compacted active list), scalar spawning per block. `pq::GrainTap` (VoiceTap discipline: lossy seqlock,
interest flag, zero cost unread), `ResourceKind::GrainTap`.
- Accept: reference render on both backends; repeatable across block sizes; cost ∝ live grains, silence 0; bench
  16 voices × 16 grains (ship); RtScope.
- Verify: `-Affected -Slow` + ship link.

### SM.7 Root pitch detection (media + views) · w2
`media::detectPitch` (McLeod / YIN over the steadiest loud 0.5 s, with confidence) inside the `SampleAnalysis` job;
result in `SampleAnalysisResult`.
- Accept: synthetic tones, a sung note, a plucked string within 5 ¢; noise / drums → unpitched (261.63).
- Verify: `pq_tests_media`.

### SM.8 Grid drop + auto-wire + replace (views) · w2, after SM.3
New branch in `ProjectCanvas::dragOver` / `drop` (`lane_edit.cpp:303`) for audio files (Explorer or browser) over any
band's device area (MIDI, audio, return): placement per the Drop decision (auto-wire only on a MIDI band with an empty
notes path; else a free unwired block), PatchNudge to the drop point, root detection +
slicing when analysis arrives (`whenAnalysed`). Drop on a sampler replaces its sample; several files → one sampler
each, offset down; one undo step.
- Accept: real-dispatch tests per path, incl. a drop on an audio track (placed, unwired, plays when an LFO drives Position) and Esc mid-drag.
- Verify: `pq_tests_views`.

### SM.9 Oval face shape + fold panel mechanism (views + document) 👁 · w2
`FaceShape` ellipse, `fold` element, `Panel::fit`, project-patch re-layout on face size change.
- Accept: goldens folded / unfolded at 1× / 2×; real-dispatch fold / unfold; neighbours not moved; undo doesn't record
  fold state.
- Verify: `pq_tests_views`.

### SM.10 Closed face 👁 · w2, after SM.3, SM.9
Oval, waveform, per-voice playheads (VoiceTap), three knobs, chevron. REVIEW entry.
- Accept: goldens; a hidden face's tap has no reader (asserted).
- Verify: `pq_tests_views`.

### SM.11 Expanded panel + waveform editor 👁 · w2, after SM.10
Markers, loop / fade, mode row, sections with embedded Envelope / Filter / LFO panels, Warp section + warp markers on
the beat ruler, root field (Hz / identity).
- Accept: real-dispatch drags one undo step each; goldens per mode.
- Verify: `pq_tests_views`.

### SM.12 Slicing + Slice to Notes (document + views) · w2, after SM.11
By transient (`media::transients`, sensitivity), beat (1/16 … 4 bars via markers / sampleBpm), region (n equal),
manual; all data writes. **Slice to Notes**: a note clip at the insert marker, one note per slice in order and timing
(beats with warp on, seconds via the tempo map otherwise), key 36 + n through `midiKeyOf`'s inverse.
- Accept: the written clip reproduces the sample's rhythm (pq_tests_playback, onsets within 1 sample).
- Verify: `-Affected`.

### SM.13 Granular section + grain dots 👁 · w2, after SM.6, SM.11
Granular controls, harmonic list editor (n/d chips), live grain dots from the GrainTap (size by amplitude, glow like
the wavetable dots). REVIEW entry.
- Verify: `pq_tests_views`.

## Rework 2026-11-23 (user: one warp implementation for clip and sampler; a more intuitive panel)
Audit (2026-11-23): Tones / Texture share a core; Beats geometry, the whole Complex drive and the marker law exist
twice or three times; Sampler Re-Pitch ignores warp markers. The 22 bit-identical clip hashes
(`tests/engine/test_warp.cpp` "SM.4: clip-player … bit-identical") were what forced the second forms: they are
re-recorded ONCE per task below, after the existing quality checks (same file: Beats ± 1 ms, Tones ± 0.5 ¢, Texture
level, Complex cases, block-size and streamed = resident invariance) pass unchanged. Those checks are not weakened.

### SM.15 One warp law, one Beats, shared rules (dsp + engine + document + meta) · w1
- **`dsp::WarpLaw`** (double segments; frame ↔ content beat, fold / unfold of a loop): the only marker law. Replaces
  `AudioSequenceImage::frameAtContent / contentAtFrame`, `clip_player.cpp segIndex`, `warp_voice.cpp warpFrameOfBeat /
  warpBeatOfFrame / segmentOf / Law`; `doc::warpFrameAt / warpContentAt` call it (ticks converted at the edge). Built
  once per image / per run record, never per render call.
- **One Beats renderer**: positions and gains are pure functions of the integer clock d = Φ(t) − Φ(T) (32.32); events
  at the first sample with d ≥ L·2³² (+ i·L/2); fades in source frames scaled by the rate at pass entry. A
  constant-step fast path solves the event sample by integer division and emits pieces + ramps, bit-equal to the
  per-sample evaluation (test: fast path == per-sample path, and a render does not depend on block size or on where a
  glide starts). `warpRenderPass / warpRenderSlices` (closed form) and `renderPassClock` collapse into it; the host
  supplies the reader (clip: sinc pieces, reverse, streamed; sampler: cubic) and the pass enumeration. Envelope < 100
  must not leave the fast path.
- **One of each rule / constant / enum**: grain size, align length / lags, jitter, 2 ms fade, max align
  (`audio_sequence.cpp:54-78,137` vs `warp_voice.*`); one warp-mode enum and one slice-loop enum in `pq/dsp/warp.h`
  used by document, engine, meta and dsp (delete `engine::ClipWarp`, `meta::SamplerWarp`, `dsp::WarpVoiceMode`,
  `engine::SliceLoop`, `dsp::WarpSliceLoop`, the float-threshold dispatch in `kernels_sampler_warp.cpp:132-139`); one
  warp parameter struct below the document's; `toFixed` / `floorDiv` / `k2m32` / cubic `interp4` once in a dsp header
  (granular and sample_read use them too). One window: the precomputed Hann + AVX2 gain for both hosts.
- **Sampler Re-Pitch follows the markers**: the `sample_read` rate takes the law's local slope (constant per marker
  segment), not only bpm / sampleBpm.
- Verify: `-Affected -Slow`; ship `pq_bench_dsp` link; bench clip Beats / Tones / Texture and sampler Beats / Texture
  / Tones before and after (clip Beats must stay ≈ 1.2 ns/frame; sampler Beats / Texture at a constant rate should
  drop well below today's 10 – 15) → bench/RESULTS.md. Report lines deleted.

### SM.16 One Complex stream + sampler warp CPU (dsp + engine) · w1, after SM.15
- **`dsp::ComplexStream`**: state (vLo / vHi / ys / head / u0 / zeroBelow / zeroAbove), lead (golden ratio), prime,
  ensure / chunk with the make-room move, gather through the host's source, the formant rule, one set of constants.
  `ClipPlayer` (keeps its pre-roll schedule, reverse, streamed sources; Y = (t − tA)·r) and `ComplexVoice` (keeps lane
  ownership, Tones cover, 2 ms cross) both drive it; their own copies are deleted.
- Sampler path, from the audit: contiguous (non-gather) reads whenever the step is constant, not only at rate 1; a
  mono sample is read once; Complex integrates Φ once, computes chunk demand once per block and reads with the vector
  cubic; formant params set only when the rate changed; lane state not copied in / out for idle lanes, and a voice
  carries only its mode's state if that is cheap to do; bound the Tones catch-up after a lost stretcher
  (`warpGrainCentre` can align 64 grains in one chunk).
- Verify: as SM.15; bench 16 Complex voices (684 ns / voice-frame today), +18\31, Pro.

### SM.17 Panel pass: radial choices, warp in the header, labels, live check 👁 (views + document + ui) · w2
- **General**: the panel `menu` element gets style `look` = `list` (today) | `radial`: a pill showing the current item;
  a click opens `ui::RadialMenu` centred on it (items from `items`; `Category/Item` makes a category ring; current
  opened to; type to filter); a pick is the same one-undo-step write the list makes. Wheel over the pill steps the value.
- **Rule**: 2 – 3 choices = cells (all visible, one click: the `tabs` element); 5 or more = radial; no drop-down list
  left in the Sampler.
- **Header** (right of the oval): mode row; Root; then **Warp** (radial: Off · Re-Pitch · Beats · Tones · Texture ·
  Complex · Complex Pro) with Sample BPM beside it while warp is on — in place of the "Open the group to rewire it"
  hint. Hidden in Gran. In Slice only Off / Re-Pitch are offered (the others do nothing there).
- **Warp section**: exists only while a stretch mode is chosen (Beats … Pro), open, holding only that mode's controls
  (Beats: Envelope, loop cells; Tones: Grain; Texture: Grain, Flux; Complex Pro: Formants / Envelope if the engine
  has them). No folded, empty Warp header otherwise.
- Loop (Off | Forward | Ping-pong), Trigger | Gate, Beats loop, Grain mode (Classic | Loop | Cloud) = cells; grain
  Shape = radial (draw the window's curve as the glyph if the menu takes one cheaply, else its name).
- **Closed face**: names under the three mini knobs (they change with the mode, so they need them); the empty face
  says what to do (drop a sample here).
- **Editor**: a press on the waveform never moves the block; hover on a handle shows what it is (info view + cursor).
- **Live check** (none of SM.10 – SM.13 was looked at in the running app): build `pqdaw` in the worktree, launch that
  instance only, drop a sample, go through every mode and warp choice, expand / fold, drag each handle; fix what is
  wrong or awkward, list what is left. Real-dispatch tests for the radial pick (pointer, keys, filter) through the
  overlay.
- Verify: `pq_tests_views` default tier (+ `pq_tests_document` for the preset); goldens: the sampler-panel-* and
  sampler-face-* move by design; anything else moving is a bug. REVIEW entry.

## Lanes
- **w1**: SM.1 → SM.2 → SM.4 → SM.5, then SM.6.
- **w2**: SM.7, SM.9 first (independent), then SM.3 (after SM.2) → SM.8 → SM.10 → SM.11 → SM.12 → SM.13 (also needs
  SM.6).
- Orchestrator `-Full` after SM.5 and at the end.

## Open risks (planner, 2026-11-20)
1. No circuit-level sample path exists in the product (`Resources::samples` test-only, `sample_read` mono and bound at
   image build, no Sample ResourceKind): SM.2 is bigger than "use the sample atom".
2. Warp reuse means extracting ≈ 600 lines of RT code from `ClipPlayer` under bit-identical clip goldens (SM.4); the
   riskiest task. The alternative (a second Beats / Tones implementation) duplicates code.
3. Signalsmith is private to `pq_engine`; kernels live in `pq_dsp`. Unverified whether the JIT / interpreter kernel-call
   path accepts a state that large; fallback is a function-pointer resource.
4. Complex per voice: ≈ 70 ms priming per note (first ≈ 70 ms of each note is Tones); ≈ 0.75 MiB per stereo stretcher
   (64 voices × 3 samplers ≈ 150 MiB).
5. Time-varying pitch inside Complex (glides / vibrato change the stretch factor per chunk) is untested.
6. Pinned samples bypass demotion, so the pool's 2 GiB budget can be exceeded; the 10 min cap is a default.
7. No pitch detector exists (SM.7 is new DSP).
8. Granulator III mode semantics are orchestrator defaults, not verified against Live.
9. JIT arena limits vs large per-lane state (32 grains × 8 lanes, WarpVoice) unmeasured.

## Conflicts with the existing architecture (put to the user 2026-11-20)
- **"Opened = grows in place"** vs one hierarchy: opening a group already shows its circuit. The plan adds a separate
  **Expand** (chevron) and keeps Open for rewiring. Panels are fixed-size today (`panel.h:119`), hence SM.9's fold / fit.
- **Slice by MIDI key**: no key signal exists; for drawn notes it derives from the degree (60 + absolute degree), so in
  31-EDO slices follow consecutive degrees and a `.kbm` changes the mapping; Cents / Ratio notes use the nearest 12-TET
  key. Independent of Hz, not of the tuning's identity layout.
- **Auto-wire "if no instrument yet"** almost never fires: every new MIDI track gets Basic Synth
  (`edit_tracks.cpp:107`). User: keep it to an empty notes path; place everywhere else unwired.
- **Per-voice stretchers with no cap** give each instance its own per-lane stretchers (memory, risk 4), unlike the
  clip player's shared pool.
- **SPEC "Simpler built from `sampleRead`"**: warp and granular here are large native atoms (GU.7 engine pattern), as
  the Wavetable and Envelope are.
