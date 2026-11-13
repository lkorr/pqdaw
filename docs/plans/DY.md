# DY: dynamics (Compressor, Limiter), Reverb, MIDI files
User 2026-11-12 ("do 1, 2, 3, 5, 6": items 3 and 5). Draft against main df0c63a9. SPEC §8 (Compressor, Limiter, Reverb rows;
sidechain paragraph), §10 (MIDI files). Tasks DY.n, RV.n, MF.n.

## Core-structure decisions (approved by the user before launch)
1. **Sidechain is an ordinary cable, not a new port or container.** A dynamics module holds a child group "Sidechain"
   (In → EQ Band, off by default → gain → Out). The user cables any track's signal into it (the track's own signal = pre-FX,
   any device output = post-FX). New lowering rule, by target only: a cable from another track into a callee's *signal input*
   (lower.cpp's `signalInput` branch: not a param, not a `<param>_add` input) becomes `graph::WireKind::Sidechain`:
   latency-compensated, merging the two tracks into one unit (SPEC §8, §15). A cable into a param stays `Modulation` (not
   delayed, SPEC 2026-10-24). Today every cross-track cable is `Modulation` (lower.cpp:3719): sidechains are not compensated.
2. **New atoms reused by later devices:** `follow` (attack/release one-pole envelope, no z1 loop), `window` (sliding max / min /
   mean over a const time), an optional `time` input (seconds) on `latency`, `truepeak` (BS.1770 4× estimator), and `fdn` (the
   reverb core as ONE native atom; everything around it is atom blocks, the module opens as a group like every other).
3. **Static transfer curves are derived, not special-cased.** `graph::StaticTransfer` evaluates any memoryless circuit at N input
   values (reusing LinearResponse's control-op evaluator: Set + foldPureAtom); a new `transfer` panel element draws the curve of
   a bound group (the compressor's "Gain Computer" child group; Saturator curve groups for free).
4. **MIDI import pitch rule:** a plain key becomes the same identity live input on that track would (through its `InputPitchMap`:
   Keys → Degree, Hz → frozen Cents; input_map.h); a bent / MTS-retuned key → Cents (or a Degree in Keys mode within 0.5 ¢ of a
   degree). Tempo and time signatures imported only into a project without clips (the `captureClip` bpm rule).

## Per-device build choice (CPU)
- **Compressor, Limiter: atom blocks.** Everything is block-wise; the only recursion is inside the `follow` kernel (a few cycles
  per sample per channel), so no per-sample Loop region. ≈ 25 stateless ops run 8 frames at a time; estimate ≤ ~10 ns per stereo
  sample (ship), measured by the worker. A native engine might be ~2× cheaper but not openable/rebuildable.
- **Reverb: native `fdn` atom + atom blocks.** As atoms, an 8-line FDN with diffusers is ~100+ ops and global ops compute ONE
  channel each (schedule.h Op::channel): no SIMD across lines, feedback in sub-blocks bounded by the shortest line; estimated
  3–5× the cost of a core where the 8 lines are one AVX2 vector. Precedent: EQ's `svf` atom.

## DY.1 Dynamics atoms: `follow`, `window`, `latency.time` (DSP)
- `follow(x, attack, release)` → out: y += (x − y)·(x > y ? ca : cr), c = 1 − exp(−1/(t·sr)) per block while attack/release are
  param-rate (per sample only if wired at audio rate); attack 0 = instant. Stateful, kBroadcast, voice-lane SIMD like `svf`.
  Silence class: Memory with a tail (settles to ≤ ε, then flushes), checked in tests/dsp/test_silence.cpp. Rewrite
  stdlib/dynamics/envfollow.pqp on it (same law; its reference test must still pass).
- `window(x, time const, op const: max|min|mean)` → out; L = max(1, round(time·sr)), preallocated (kVariableState like the delays'
  `max`). max/min: van Herk–Gil-Werman (≈ 3 compares/sample, branch-free, bounded); mean: running sum in double + ring.
- `latency(x, frames, time const = 0)`: total = frames + round(time·sr), whole samples (never interpolates); latency rule and kernel
  agree on rounding; existing `latency(x, 480)` texts unchanged.
- Files: src/dsp/include/pq/dsp/{atoms.h,atom_state.h}, src/dsp/src/{atoms.cpp,kernels_dynamics.cpp(new),kernels_memory.cpp},
  src/graph/src/{atoms.cpp,kernel_map.cpp,latency.cpp,inspect.cpp}, views circuit catalogue + symbols, docs/PATCH_LANGUAGE.md atom
  list, ARCHITECTURE atom table, bench/dsp/bench_atoms.cpp.
- Accept: `follow` step response (63 % after one time constant, attack ≠ release), instant attack; `window` max/min/mean equal a
  brute-force reference on random input for L ∈ {1, 7, 64, 1153}, independent of block size; `latency` with time reports and
  delays the same whole number of samples at 44.1 / 48 / 96 kHz (pathLatency == impulse); interpreter == JIT; RtScope; bench rows.
- Verify: `-Affected -Slow`, then `-Preset ship -Target pq_bench_dsp -NoTests` once.

## DY.2 Sidechain routing: compensated cross-track input cables + source picker
- lower.cpp: `LWire::signalIn` (set only in the `signalInput` branch); emit `W::Sidechain` when
  `w.fromNode != w.toNode && signalIn && !toMixer`. Merging and PDC already handle Sidechain (project.cpp partition;
  project_compile.cpp wireDelay; latency_plan.h).
- views project_patch/model.cpp (~231, 911, 1092, 2027–2034): Sidechain cables like Modulation cables (ports `m<wire>`), drawn
  as audio cables.
- New panel element `cablesource` (views/src/panel/cable_source.cpp), Value slot bound to an input block. Menu: "None", then per
  track "<name>: Pre FX" (Wire::noteSource + kMainPort) and "<name>: Post FX" (the device feeding the track's mixer). Picking =
  ONE undo step via `PanelTarget::run`: remove the old cross-track wire into the bound input, `ops::connect` the new one, set an
  optional bound "on" value block to 1. Shows the current source read from the wires (a cable drawn in the patch view shows too).
- ARCHITECTURE "Project graph" and "Plugin-delay compensation": state the input-vs-param rule.
- Accept: kick track `latency(x, 480)` into a VCA's input on a bass track → detector signal aligned with the bass's own audio
  (impulse test), unit total reported; an LFO into a param stays undelayed; pick/clear a source through real window events = one
  undo step; save/load round-trips.
- Verify: `-Affected`; views part in `pq_tests_views` default tier.

## DY.3 Transfer display and gain-reduction meter 👁
- `graph::StaticTransfer` (src/graph/src/transfer.cpp or response.cpp; header pq/graph/response.h): prepare(typed, source, sink,
  settable), evaluate(span<const float> xs) → ys via the existing control evaluator (factor the shared part out of LinearResponse;
  no duplicate); any stateful atom on the path → Unavailable. `doc::groupTransfer(project, group)` next to group_response.cpp:
  lowered once per structural revision, re-evaluated on param changes.
- `transfer` element (views/src/panel/transfer_element.cpp): x = input dB, y = output dB (−60 … +6), 1:1 diagonal, the curve, a dot
  at the live input level (Signal binding; zero cost while hidden).
- Meter style `mode: level | reduction` (engine_elements.cpp Meter): reduction reads the bound signal as dB of reduction (≥ 0),
  drawn downward from the top over `range` (12 / 24 / 48 dB).
- Accept: Saturator "Hard Curve" group's curve exact against a scalar reference; a group holding z1 gives no curve; goldens; REVIEW.
- Verify: `-Affected`; goldens in `pq_tests_views`.

## DY.4 Compressor module 👁 (after DY.1, DY.2, DY.3)
- `ModuleKind::Compressor`, catalogue `module:compressor`, `compressorBlocks()` in modules.cpp (Kit style of Utility / Saturator).
  Stereo path:
  1. Detector source: `mux(Sidechain, In, lerp(In, SC, SC Mix))`; SC = the "Sidechain" child group (In ← the cable; an EQ Band
     child group off by default; Gain). Literal mux folds away.
  2. Level: Peak = max(|L|, |R|) (linked); RMS = sqrt(window(max of squares, 5 ms, mean)); then `fast_gain_to_db(max(level, 1e-6))`.
  3. "Gain Computer" child group (memoryless; In = level dB, Out = reduction dB ≥ 0). Compress: k = clip(over + W/2, 0, W),
     GR = (1 − 1/R)·(k²/(2·max(W, 1e-3)) + max(0, over − W/2)). Expand: mirror below threshold (downward), capped at 70 dB.
  4. Envelope Log (default): follow(GR, attack, release); Lin: follow on the linear level before step 3. Auto release:
     max(follow(GR, a, rel/4), follow(GR, a, rel·4) − 3 dB). Record the law.
  5. out = latency(In, time = Lookahead) · fast_db_to_gain(makeup − GR); Dry/Wet via gain_add. Lookahead 0 / 1 / 10 ms is a
     stepped value block → constFeeds into latency.time (one recompile; PDC follows). Listen: Out = SC signal.
- The GR block's output is a cable source (SPEC: detector envelope exportable).
- Panel: Threshold, Ratio, Attack, Release, Auto, Knee, Makeup, Dry/Wet; chips Mode (Peak / RMS / Expand), Env (Log / Lin),
  Lookahead (0 / 1 / 10); `transfer` (Gain Computer, dot = level); reduction `meter`; Sidechain section (`cablesource`, Gain,
  Mix, EQ on + freq, Listen).
- Accept: static curve (sine at −40 … 0 dB vs formula ±0.1 dB, knee 0 and 12 dB); attack/release measured on a step; lookahead
  10 ms reports 480 samples at 48 kHz and reduction starts before the transient; a sidechain from another track ducks a pad
  (audio test through the project); RtScope; interpreter == JIT; bench row (ns per stereo sample); goldens; REVIEW.
- Verify: `pq_tests_playback` (new test_compressor_module.cpp); golden in `pq_tests_views`.

## DY.5 `truepeak` atom (DSP)
- `truepeak(x)` → max |·| of the 4 polyphase BS.1770-4 Annex 2 interpolants (48 taps, 12 per phase) and the sample itself, about
  x[n − 6]: fixed group delay 6, reports no latency (the module aligns). 12-tap phases SIMD in AVX2.
- Accept: within 0.2 dB of a 16× sinc-oversampled reference on sines 0.24–0.49·sr with random phases; no lag beyond 6 samples;
  RtScope; interpreter == JIT; bench row. Verify: `-Affected -Slow` + ship link.

## DY.6 Limiter module 👁 (after DY.1, DY.3, DY.5)
- `module:limiter`: Gain (dB), Ceiling (dB), Release (+ Auto), Lookahead 1.5 / 3 / 6 ms (stepped → constFeeds), Link Stereo /
  L-R / M-S (literal mux; M/S = the EQ's matrix), True Peak.
- Path: x = In·gain; r = max(0, gain_to_db(det) − ceiling), det = |x| linked per Link, or with True Peak `truepeak(x)` (without
  it `latency(|x|, frames 6)` so both modes keep one delay); h = window(r, la, max); g = follow(h, 0, release) (auto as DY.4);
  b = window(g, la, mean); out = latency(x, frames 6, time la) · db_to_gain(−b), then a final `clip` at the ceiling. Proof in a
  comment: the mean of L values each ≥ r[m] over the L samples covering m keeps the delayed sample under the ceiling. The audio
  delay is la + 6 in both modes (toggling True Peak never replans PDC).
- Panel: knobs, chips, reduction `meter`, output `meter`.
- Accept: no output sample above the ceiling on a test set (impulses, square bursts, full-scale noise, sweeps) at every
  lookahead; True Peak: true peak ≤ ceiling + 0.2 dB; latency = round(la·sr) + 6; release times; L-R limits only the hot
  channel; goldens; REVIEW.
- Verify: `pq_tests_playback` (test_limiter_module.cpp); golden in `pq_tests_views`.

## RV.1 `fdn` atom (DSP; after DY.1: same registration files)
- `fdn(l, r, size, decay, dampHi, dampLo, loMult, diffusion, modRate, modDepth, freeze; lines const 8|16, stages const 0…4,
  seed const, maxSize const)` → earlyL, earlyR, lateL, lateR.
  - Input diffusion: stereo upmixed to 8 channels; `stages` steps (Signalsmith-style): per-channel delay (random in a
    size-scaled range from seed), polarity flips, Hadamard 8; `diffusion` crossfades each step's output with its input. Early
    outputs = diffuser outputs folded to stereo.
  - Feedback: lines with exponentially spread lengths ∝ size (pairwise coprime), Householder matrix, per-line gain
    10^(−3·len/(T60·sr)), first-order HF lowpass + LF shelf (loMult) per line, 8 lanes at once. Chorus: each line's read position
    wobbles by modDepth (≤ ~2 ms) at modRate with spread phases, cubic read. Freeze: input muted, gain 1, damping bypassed.
  - size smoothed per block; coefficients per block; buffers sized from maxSize (kVariableState). Silence: feedback tail
    (state ≤ ε → flush); frozen never sleeps.
- Accept: T60 within 10 % of `decay` at 1 kHz for 0.5 / 2 / 10 s; HF decays faster with damping; echo density ≥ 1 per ms within
  ~50 ms; freeze holds energy within 0.5 dB over 10 s; L/R decorrelated; RtScope; interpreter == JIT; sleeps after the tail;
  bench rows (8 / 16 lines; target ≤ ~40 ns per stereo sample at 8 lines, ship).
- Verify: `-Affected -Slow` + ship link.

## RV.2 Reverb module 👁 (after RV.1)
- `module:reverb`: Predelay (`delay`, 0.5–250 ms), input filter (svf hp + lp, on/off literal mux), `fdn` (Size, Decay 0.2–60 s,
  HF / LF damping, Diffusion, Density = stages chip, Chorus rate / amount, Freeze), Reflect / Diffuse levels (dB gains on early /
  late), Stereo width (M/S, Utility's pattern), Dry/Wet (gain_add); Quality chip 8 / 16 lines.
- Panel: three rows (Input, Diffusion network, Output), Freeze button, Quality chip.
- Accept: dry/wet 0 bit-exact pass-through; predelay lands on the right sample; width 0 = mono; on a return an idle reverb sleeps
  after its tail; golden; REVIEW.
- Verify: `pq_tests_playback` (test_reverb_module.cpp); golden in `pq_tests_views`.

## MF.1 Standard MIDI File reader and writer
- `pq/document/midi_file.h`, src/document/src/midi_file.cpp; in-house, bytes in / bytes out (no file I/O).
  `parseMidiFile(span<const uint8_t>) → Result<MidiFile>`, `writeMidiFile(const MidiFile&) → vector<uint8_t>`; MidiFile = format,
  division (PPQ or SMPTE), tracks of timed events.
- Reader: bounds-checked; running status; F0/F7 sysex; meta (tempo, time signature, track name, end of track); RIFF RMID unwrap;
  unknown chunks skipped; truncated → error, never a crash.
- `MidiNotes resolveNotes(const MidiFile&)`: note-on/off pairing per (track, channel, key) FIFO; velocity 0 = off; bend at
  note-on in cents per channel using RPN 0 (default ±2; MPE member channels ±48 after an MCM RPN 6; master channel bend added).
  MTS: real-time and non-real-time single-note tuning change (08 02 / 08 07), bulk dump (08 01), scale/octave 1- and 2-byte
  (08 08 / 08 09) into a per-key cents table over time. Sustain (CC64) extends note ends. Unpaired notes end at the track end.
  Output per note: tick, length, key, channel, velocity, release velocity, offset cents; tempo and signature lists.
- Accept: hand-built byte fixtures per feature; write → read round-trips; truncated/garbage → errors; 10 MB file < 50 ms (release).
- Verify: `pq_tests_document` default tier.

## MF.2 MIDI import (after MF.1)
- `doc::ops::importMidi(p, ids, notes, ImportMidiOptions{target track, at, newTracksAfter}) → EditResult`: ONE undo step on the
  `captureClip` path (CapturedClip per track).
  - Ticks: round(t·kPpq/division), exact int64; SMPTE: seconds → ticks via the 120 bpm map.
  - Tracks: format 1 = one MIDI track per MTrk with notes (named from FF 03); format 0 = ONE track. The first goes to the drop's
    MIDI track when there is one, the rest new tracks after it (`createTrack`: default instrument). One clip per track from tick
    0 to the last note-off, rounded up to a bar.
  - Pitch: decision 4; `buildInputPitchMap(p, track)` per target track; `freezeHz` for Cents.
  - Tempo and signatures: `SetTempoMap` / `SetTimeSignatures` only when `!projectHasClips(p)`; a signature change not on a bar
    moves to the next bar (reported).
- UI: arrangement FileDrop (file_drop.cpp: replace "MIDI file import is not available yet"; .mid with audio in one drop works;
  status line reports dropped controllers / moved signatures); browser `.mid` items (ItemKind::Clip) via `loadClipItem`; project
  patch lane drop via FileDrop too.
- Accept: 12edo file onto a Keys track → Degree notes; onto an Hz track → Cents at the same Hz as live input; ±2 bend file →
  exact Cents; MTS 31edo file onto a 31edo Keys track → Degrees; 3-track file → 3 tracks; tempo imported into an empty project
  only; one undo step; drop through real window events.
- Verify: `-Affected`; drop test in `pq_tests_views`.

## MF.3 MIDI export (after MF.1; parallel with MF.2)
- `doc::exportMidi(p, span<ClipRef>, ExportMidiOptions{mode, division = 960}) → Result<vector<uint8_t>>`, format 1: track 0 =
  tempo map + signatures (from the clip's start), one MTrk per clip; a clip exported as it plays over its arrangement length (loop
  unrolled); disabled notes skipped; stored velocity; static Hz from `resolveTrackPitches`. Modes (v1):
  - **12edo**: nearest key, no bends.
  - **MPE**: lower zone with MCM, members 2–16 at ±48 semitones (RPN 0 sent); each note on a free member channel (round-robin)
    with the bend to its exact pitch; > 15 at once: oldest channel reused (counted, warned).
  - **Degrees ("EDO mapped")**: key = 60 + absolute degree in the clip's tuning (.kbm inverse when it has one); non-degree notes
    snap to the nearest degree; round-trips with Keys / Consecutive import.
- App: clip context menu "Export MIDI…" (mode chip); drag-out of note clips (shell_app.cpp onDragOut: replace the "MIDI clips drag
  out as .mid files with M11" refusal; writes `<clip>.mid` next to clipWavPath's logic, in app/clip_export); drag-out uses
  ViewPrefs' last mode (default MPE).
- Accept: export → MF.1 import round-trips pitches (12edo exact; MPE within 0.01 ¢; Degrees identical degrees); tempo and
  signatures present; drag-out produces a file (app test through the handler, no OLE).
- Verify: `-Affected`; `pq_tests_app` for the shell hook.

## Order and parallelism (≤ 2 workers)
| Wave | Worker A | Worker B |
|---|---|---|
| 1 | DY.1 (dsp/graph atom registries) | MF.1 (document/midi_file) |
| 2 | RV.1 (same registries, after DY.1) | DY.2 (lower.cpp, project_patch, panel cable_source) |
| 3 | DY.3 (graph response, panel transfer + meter) | MF.2 (edit ops, file_drop, library_load) |
| 4 | DY.4 (modules.cpp) | MF.3 (export, shell_app, clip_export) |
| 5 | RV.2 (modules.cpp) | DY.5 (dsp/graph registries) |
| 6 | DY.6 (modules.cpp) | — |
modules.cpp (kModules, ModuleKind) is shared by DY.4, RV.2, DY.6: serial.

## Choices (recommended defaults)
- Sidechain merges the two tracks' units (SPEC §15; one-line choice in lower.cpp). v1 taps: Pre FX + Post FX (post-mixer later).
- MTS export and mos-explorer "EDO mapped" (listValidEdos / edoDegreeSteps): deferred; v1 ships 12edo, MPE, Degrees.
- Format 0 → one track (keeps channel-per-note microtonal files intact). Sustain applied to note ends on import; other CCs and
  mid-note bends dropped (expression curves come with M7), reported.
- Lookahead / Density change recompiles (≈ 11 ms swap fade): accepted. Compressor RMS = 5 ms `window` mean. Reverb 8 lines
  default, 16 = "High".
- Not now: Glue Compressor, Gate, Auto Filter's follower (all reuse follow / window / Sidechain); multiband; MIDI preview in the browser.
