# Questions for the user

Agents: add questions here instead of guessing on anything important. Use this format:

```
## Q<n> · <date> · <task> · <short title>
- Context: …
- Options: A) … B) …
- Provisional choice: … (why it's the most reversible)
- Status: open | answered (answer: …)
```

## Q1 · 2026-10-20 · T2.1 · Which reference does a `Cents` identity use?
- Context: SPEC §3.1 says `Cents` is "absolute cents from the reference" and "never moves" on tuning change, while §3.2 makes the reference pitch a parameter of each tuning (per project/tuning/clip).
- Options: A) the reference of the note's tuning context (so changing that tuning's reference moves Cents notes); B) a project-wide reference that tuning changes never touch.
- Provisional choice: A. `resolveHz` takes the reference from the `Tuning` passed in; Cents notes still ignore every other tuning change (EDO size, MOS generator…). Switching to B only changes which Hz the caller passes, not the stored identity.
- Status: open

## Q2 · 2026-10-20 · T2.2 · EDO row labels in the piano roll (SPEC §4.1)
- Context: "labelled with an equave index … otherwise `0\31`, `1\31`… with equave number" can be read as (a) the step within the equave as `k\n` plus the equave number shown separately, or (b) `e\n` where e is the equave index.
- Options: A) row label = `stepLabel(degree within equave, n)` plus the equave index (`edoRow()` gives both); B) markers labelled `e\n`.
- Provisional choice: A. `pq::tuning` exposes `stepLabel(k, n)` and `edoRow(step, n)` only; the UI composes the text, so B costs one format string.

## Q3 · 2026-10-20 · T0.5 · Straight 1/64 grid is not a whole number of ticks
- Context: PPQ 27720 = 2³·3²·5·7·11, so a whole note is 110880 ticks and 1/64 = 1732.5 ticks. SPEC §9.2 lists fixed grids down to 1/64. All 3/5/7/11-tuplets of 1/64 are exact (they span 1/32, 1/16 or 1/8), only the straight 1/64 is not.
- Options: A) keep PPQ 27720; straight 1/64 grid lines are computed from the whole note and rounded per line (alternating 1732/1733, no drift), time signatures with denominator 64 are rejected. B) double PPQ to 55440 (1/64 exact; still lcm-friendly; int64 range is no issue).
- Provisional choice: A (`noteValueIsExact(64) == false`, `noteValueTicks(64)` rounds, `TimeSigMap` rejects x/64). Reversible: PPQ is one constant, nothing is persisted yet.
- Status: open

## Q4 · 2026-10-20 · T0.5 · Tempo ramp law
- Context: SPEC §9.2 asks for tempo ramps but not their shape.
- Options: A) BPM linear in ticks (musical time), like tempo automation drawn on a beat-based arrangement (Ableton-style); seconds = closed-form log integral. B) BPM linear in seconds; ticks = quadratic in time.
- Provisional choice: A, implemented with log1p/expm1 so it stays accurate for tiny slopes. A ramp drawn as a straight line on the beat grid is then really straight. Reversible: `TempoCurve` is an enum, so a `LinearInTime` curve can be added beside it.
- Status: open

## Q5 · 2026-10-20 · T2.3 · mos-explorer: the CLI (mos-calc.js) and the app (index.html) disagree
- Context: mos-calc.js says it "mirrors index.html logic exactly", but its copies differ: `buildMOSScale` has no `chainSafeGen` nudge and sets s = 0 (not L) for an equalized scale; `computeHardness` returns a 4-digit string and no "1.00" case; `computeUDP` ignores brightness (always `n−1−root|root`). `generatorForHardness` exists only in the CLI and is built on its versions.
- Options: A) port both, the app's as the main API and the CLI's under `pq::tuning::mos_calc` (only what `generatorForHardness` needs); B) port only the app's and rebuild `generatorForHardness` on it (fixtures would then not come from the original code).
- Provisional choice: A, so every fixture is produced by the original JS. Switching `generatorForHardness` to the app functions later is a one-line change plus a fixture regeneration.
- Status: open

## Q6 · 2026-10-20 · T2.3 · Suspected mos-explorer bugs kept for fidelity
- Context: the C++ must match the JS exactly (T2.3 acceptance), so these are ported as-is:
  1. `rotateMOSScale` sets s = 0 when all steps are equal, while `buildMOSScale` sets s = L. `applyMODMOS` uses chroma = L − s, so on an equalized scale an alteration moves a note by a whole step for rotation ≠ 0 and by nothing for rotation 0.
  2. L and s are rounded to 0.01 ¢ before use, so the MODMOS chroma (and every hardness ratio, and therefore `generatorForHardness`'s bisection) is quantised to 0.01 ¢. MODMOS pitches are consequently not exactly linear in the generator.
  3. `updatePanel()` calls `updateHeldNotes()` before it transfers the mode rotation to a new MOS, so when a generator drag crosses into a region of a different size the held notes are re-mapped against a scale built with the stale rotation; on the next frame the size is unchanged, the cheap path runs, and the notes move to the transferred rotation's pitches (a jump). Read from the code, not reproduced in a browser.
  4. `chainSafeGen` reads the UI selection (`S.selected`), so the pitches of a degenerate generator depend on which region is selected. The C++ takes it as an explicit `RegionHint` parameter.
- Options: A) keep the port bit-faithful and fix in the JS first (then regenerate fixtures); B) fix in pqdaw and diverge from the fixtures.
- Provisional choice: A. For tuning-as-signal (T2.6), MODMOS cannot use the linear form exactly because of 2; T2.6 should decide whether audio uses the exact chroma (L − s unrounded).

## Q7 · 2026-10-20 · T1.1 · `out` is both a keyword and the example's output name
- Context: PATCH_LANGUAGE §1 lists `in` and `out` as keywords, but the §4 example (and SPEC §7.3) declares `out out : audio voice mono` and binds `out = car * env`. Atom ports are also called `in`/`out`.
- Options: A) `in`/`out` are contextual: they start a declaration only when followed by an identifier and `:`, and are otherwise ordinary names; B) keep them reserved and rename the example's output (e.g. `out y : audio`).
- Provisional choice: A. `pq::graph::isKeywordNotName()` rejects every keyword as a name except `in` and `out`; the parser (T1.2) needs one token of lookahead after `in`/`out`. Switching to B is a one-line change in that function plus the example.
- Status: open

## Q8 · 2026-10-20 · T1.1 · Atom names and semantics not fixed by SPEC §7.2
- Context: the atom registry (`src/graph/src/atoms.cpp`) needs exact names and port lists; §7.2 names some atoms only loosely.
- Choices made (all data in one table, so each is a one-line change before T1.2 starts parsing stdlib):
  1. Flip-flops are `srff`, `dff`, `tff` (outputs `q`), because `sr` is the sample-rate constant (PATCH_LANGUAGE §3).
  2. Operators map to atoms: `+ − * / %` → `add sub mul div mod`, unary `−` → `neg`, `! && || ^^` → `not and or xor`, comparisons → `lt le gt ge eq ne`, `c ? a : b` → `select`, `x[i]` → `channel(x, i)` (i const), a tuple `(a, b, …)` → `pack`.
  3. "sine (from phase or frequency)": `sine(phase)` (0..1 phase) and `sine_hz(hz, reset)` (own uint32 phase).
  4. `delayline` with linear/allpass/cubic reads: three atoms `delay`, `delay_allpass`, `delay_cubic`, inputs `(x, time, max)`, time in seconds, `max` const (sizes the buffer); the read is clamped to ≥ 1 sample.
  5. `pulse`: no inputs, true on the first sample the instance runs. `timer(trig, dur)`: dur in seconds. `counter(up, down, reset, wrap)`: counts rising edges, wraps at `wrap` when > 0.
  6. `cents_to_hz(cents, ref = 261.626)` / `hz_to_cents(hz, ref = 261.626)` (SPEC §15 reference).
  7. `sample&hold` is `samplehold(x, trig)`; `gate→real` is `gate_to_real` (as in PATCH_LANGUAGE §3); exact/fast variants are `tan`/`fast_tan` etc.
  8. `track.in`, `sidechain.in`, `send(x, bus)`, `clipPlayer(clip)` are the I/O atoms; a patch's own outputs are `out` declarations, not an atom. `track.in` ends in a keyword after the dot (see Q7); so the T1.2 parser must accept a keyword after `.`.
  9. `oversample { }`, `voicechain { }` and `fft` are not in patch language v0, so they have no IR form yet.
- Status: open

## Q9 · 2026-10-20 · T1.1 · Which stateful atoms break a delay-free cycle?
- Context: PATCH_LANGUAGE §3 says "a cycle that goes through `z1(…)`, `delay(…)` or a stateful atom is ordinary feedback". But some stateful atoms pass their input through in the same sample (`latch`, `samplehold`, `counter`, `schmitt`), so a cycle through them has no delay.
- Options: A) per-input `delayed` flag: a cycle is ordinary feedback only if it passes through an input whose effect on the output waits a sample (`z1.x`, `delay*.x`, `phasor.hz`, `sine_hz.hz`); B) any stateful atom breaks the cycle (the sentence as written), with the scheduler inventing a sample of delay inside latch/samplehold.
- Provisional choice: A (`PortSpec::delayed` in the registry); T1.5 reads it. B would only need the scheduler to check `kStateful` instead.
- Status: open

## Q10 · 2026-10-20 · T1.1 · Attribute arguments, param defaults and ranges
- Context: the grammar's `attr` takes `argList` (expressions), but the examples pass a string (`@label("Cutoff")`) and bare words (`@unit(Hz)`, `@curve(exp)`), which are not expressions. Param defaults and ranges are `expr`.
- Choices: attribute arguments are literals only: number (with unit), bool, string, or identifier (optionally named, `@steps(n: 4)`). Param defaults and range ends are constant literals; the parser folds constant expressions such as `3/2` or `2 * 5ms` to a number (the unit suffix is kept only for a plain literal), and rejects non-constant ones. Literals keep their written unit (`5ms` stays 5 + ms, normalised on use), so printing reproduces them exactly.
- Status: open

## Q11 · 2026-10-20 · T0.7 · Rounded rects: geometric AA vs SDF
- Context: SPEC §11 says "Antialiasing is geometric, as in PQ Meter" and, under other primitives, "SDF rounded rects/borders". T0.7 had to pick one for `addRoundedRectFilled`.
- Options: A) geometric: convex outline + 1 px feathered fringe (ImGui/PQ Meter style), one shader, one 20-byte vertex format, 2·4·(segments+1) vertices per rect. B) SDF: 4 vertices per rect with coverage computed per pixel in the fragment shader, but the rect/radius parameters don't fit the fixed `{pos, uv, rgba8}` vertex, so it needs a second vertex/instance format, pipeline and command type.
- Provisional choice: A. One pipeline and format (fewer state switches, PQ Meter code ports as-is), zero extra fragment cost. Measured (release): 145 ns to build an r=3 rounded rect (32 vertices + 138 indices). It's the more reversible choice: `addRoundedRectFilled`'s signature wouldn't change if an SDF path is added later. If widget-heavy frames show the build/upload cost matters (B would be ≈15 ns and 4 vertices per rect), add B as an instanced pipeline behind the same API.
- Status: open

## Q12 · 2026-10-20 · T0.6 · WASAPI exclusive mode, ASIO licensing, and WASAPI underrun detection
- Context: SPEC §2 asks for "RtAudio (ASIO + WASAPI exclusive/shared)". Checked in the RtAudio source: 6.0.1 and its current master both call `IAudioClient::Initialize(AUDCLNT_SHAREMODE_SHARED, …)` only (or `IAudioClient3::InitializeSharedAudioStream`), so there is no WASAPI exclusive mode. Its WASAPI backend also never reports `RTAUDIO_OUTPUT_UNDERFLOW` (only capture discontinuities), so an output underrun on WASAPI is invisible to the xrun counter. And RtAudio's ASIO backend compiles the Steinberg ASIO SDK 2.3 files it bundles, which are under Steinberg's proprietary SDK licence (newer SDKs also GPLv3), not a permissive licence as CLAUDE.md requires. Finally, shared mode at a rate other than the mix rate makes RtAudio resample with Media Foundation on the audio thread (allocates per callback).
- Options (exclusive + underruns): A) our own WASAPI backend (event-driven, exclusive and shared, `GetCurrentPadding`-based underrun detection, no extra ring buffer: also lower latency than RtAudio's ring) behind the existing `AudioDevice` interface; B) patch RtAudio (vendored fork); C) another library (PortAudio has WASAPI exclusive, MIT-style, but also bundles ASIO SDK for its ASIO host).
- Options (ASIO): A) keep it opt-in (`-DPQ_WITH_ASIO=ON`) and let the user accept Steinberg's licence for their own builds; B) a clean-room ASIO host header (the IASIO COM vtable is small) so ASIO ships under our licence; C) drop ASIO.
- Provisional choice: RtAudio WASAPI shared only, honestly reported: `ShareMode::Exclusive` / `--exclusive` return `ErrorCode::Unsupported`; ASIO compiled only with `PQ_WITH_ASIO=ON` (default OFF; `--asio` explains why when off); the stream defaults to the device's mix rate to avoid the resampler and `StreamInfo::backendResampling` flags it otherwise; `--sine` adds a backend-independent check (frames delivered vs. the span between first and last callback) and a deadline-miss counter. All reversible: backends sit behind `AudioDevice` + `CallbackRunner`, so option A replaces only `rtaudio_device.cpp`. I'd recommend A (own WASAPI) as a later task, and asking the user about ASIO B vs A.

## Q13 · 2026-10-20 · T1.6 · Bool storage: 32-bit lane masks instead of packed bits
- Context: SPEC §7.1 says bools are "stored as SIMD lane masks: one bit per voice per sample in packed form, expanded to 0/1 floats only where a real-valued input consumes them".
- Options: A) block buffers hold 32-bit masks (0 / 0xFFFFFFFF) in float slots, the form AVX2 compares produce and `blendv`/`and` consume directly; B) packed bits (1 byte per frame for 8 voices), with a `movemask` to pack after every compare and a shuffle/compare to expand before every select or logic op.
- Provisional choice: A. Packing and unpacking would cost more instructions per bool op than the logic itself, and a 64-frame × 8-voice bool buffer is only 2 KB. Logic stays "nearly free" (0.06–0.09 ns per voice-sample measured). Reversible: only the bool kernels and the interpreter's buffer sizing depend on it; packed bits can still be used for storage outside the block loop (e.g. event/state snapshots).
- Status: open

## Q14 · 2026-10-20 · T1.6 · Underspecified logic/memory atom semantics
- Context: SPEC §7.2 names these atoms without fixing their exact behaviour. Provisional semantics (documented in `src/dsp/include/pq/dsp/atoms.h`, tested):
  - `pulse(period)`: high on the first sample, then every round(period) samples (a clock source); an invalid period gives a single pulse on the first sample. Alternative: a one-sample trigger on an input edge (that is what `rise` already does).
  - `samplehold(x, trig)` samples on the rising edge of `trig`; `latch(x, enable)` is level-sensitive (transparent while high).
  - `phasor`/`sine_hz` `reset` and `sample_read` `reset` are level-sensitive (phase/playhead held at the start while high); use `rise()` for edge behaviour.
  - `clockdiv(clk, n)` passes every n-th input pulse (keeps the pulse width) rather than producing a 50 % square wave.
  - `counter` wraps within [0, n−1]; reset wins over simultaneous edges.
  - `sr` flip-flop: reset wins when both inputs are high.
- Provisional choice: as listed: each is the cheapest primitive from which the alternatives can be built as subcircuits. Reversible: kernel semantics only; nothing persisted.
- Resolution of the registry differences (T1.3, 2026-10-21). One authoritative table, `pq/graph/kernel_map.h` (`kernelMapping(AtomId)`: kernel + what each registry input becomes), checked by `tests/graph/test_kernel_map.cpp` against both tables (names, input/output counts and bool kinds, state sizes, resources, splat-only inputs, both lane counts):
  - **Durations stay seconds in the language, samples in the kernels** (delay `time`, timer `dur`, pulse `period`): the compiler inserts `· sr` as const/param-rate math (hoisted/folded by T1.4), so kernels never convert per sample. Delay `max` (const seconds) becomes `StateInit::maxDelayFrames`.
  - **Delay minimums are the kernels'** (1 / 2 / 1.5 samples for linear / cubic / allpass): needed for `x` to be a delayed input with those interpolators; registry summaries updated.
  - **`pulse(period)`**: the registry gains the kernel's optional `period` (seconds, default 0 = the old one-shot).
  - **`z1(x, init)`**: `init` stays in the language (const); the kernel side gained `StateInit::z1Init` (written by `initState`, zero runtime cost).
  - **`sampleRead` is replaced by the kernels' four atoms** `sample_read_linear/_cubic(sample, rate, reset, start, loopStart, loopEnd) → (out, playing)` (stateful 32.32 playhead; start/loop points param-bounded) and `sample_pos_linear/_cubic(sample, pos)`. `sample` is a resource const.
  - **`wavetable` loses `hz`**: the kernel picks the mip level from the phase increment (no extra input, no extra cost).
  - **Kernel-only atoms are now registry atoms**: `fast_cents_to_hz`, `fast_hz_to_cents`, `fast_db_to_gain`, `fast_gain_to_db`, `muladd`. `const` stays internal (block fill for Const nodes); so do the split feedback kernels.
  - **`eq`/`ne` on bools**: the dsp comparators are real-only (a true mask is a NaN as a float), so `infer()` rewrites bool `a == b` to `not(xor(a, b))` and `a != b` to `xor(a, b)`.
  - **State sizes**: `AtomSignature::stateBytes` is now the kernel's state block (`sizeof` from `atom_state.h`: one block per channel, 8 lanes), not a per-lane estimate.
  - **No kernel, by design**: `channel`, `pack` (scheduler buffer views/copies), `sr` (const), `voice.*`, `transport.*`, `track.in`, `sidechain.in`, `send`, `clipPlayer`, and the voice reducers `voice_*` (engine: they need the voice table).
  - Semantics kept from the kernels as listed above (edge-triggered samplehold, level-sensitive latch/resets, clockdiv pulse divider, counter wrap within [0, n−1], reset-wins flip-flops); registry summaries now say so.
- Status: resolved for T1.7 (semantics of the listed atoms still open for the user)
- Differences from the T1.1 registry (Q8), as found by T1.6: kernel names follow the registry (`srff dff tff samplehold delay delay_cubic delay_allpass fast_sqrt`); the kernels additionally have `fast_cents_to_hz`, `fast_hz_to_cents`, `fast_db_to_gain`, `fast_gain_to_db`, `muladd`, `const`. Delay and timer durations are in **samples** in the kernels (the compiler multiplies seconds by `sr`); delay minimums are 1 (linear), 2 (cubic), 1.5 (allpass) samples, not 1 for all, because the newest interpolation tap must be ≥ 1 sample old for `x` to be a delayed input. `pulse` takes an optional `period` input (a constant 0 gives the registry's "true on the first sample"). `z1` has no `init` input (state starts at 0; initState could take one). The registry's stateless `sampleRead(sample, pos, loopStart, loopEnd)` corresponds to `sample_pos_*` (no loop wrap yet) and the stateful 32.32 playhead `sample_read_*(rate, reset, start, loopStart, loopEnd)`; a float `pos` input loses sub-sample precision beyond 2^24 samples, which is why the playhead form exists. `phasor`/`sine_hz` have no split kernels yet, so an FM feedback loop through `hz` needs an explicit `z1` until T1.5/T1.7 decide how delayed inputs are scheduled.

## Q15 · 2026-10-20 · M3 planning · Decisions needed before/while building M3
- Context: the M3 task breakdown (TASKS.md T3.1–T3.17) hits these spec gaps. Provisional choices are what workers will use unless you decide otherwise.
- Default instrument before Operator (M4): A) tiny stdlib `basic_synth.pqp` from leaf atoms per MIDI track; B) wait for T1b.3 `adsr`; C) M3 silent. Provisional: A.
- T2.7 vs T3.4 ownership of main tunings: A) T3.4 owns tuning slots/assignment/serialization, T2.7 shrinks to binding tunings to graph signal nodes; B) block T3.4 on T2.7. Provisional: A.
- Mute vs deactivate cost: mute = gain param (no recompile, still costs CPU, like Ableton); deactivate (`0`) = structural (DCE + recompile + crossfade). Provisional: that split.
- Pan law / volume taper: match Ableton's (measure against Live renders) or a documented own law. Provisional: constant-power pan, Live-like fader curve −inf…+6 dB, to be measured.
- Undo scope: selection/zoom/scroll/fold not in undo history (Ableton). Provisional: not in history.
- Ratio/Cents notes in equidistant scale layout: A) interpolated between degree rows; B) nearest row with an offset badge. Provisional: A.
- MOS wheel hotkey (SPEC §4.2 TBD): `W`, Ctrl+Shift+W, or chip-only in M3. Provisional: chip-only + Ctrl+Shift+W.
- Ableton theme reference: which Live 12 theme (Default Dark Neutral Medium?) and may colours be sampled from your Live install/screenshots; font Inter vs IBM Plex Sans (T0.8 bundles Inter). Provisional: Dark Neutral Medium approximated by hand, Inter.
- Computer keyboard as instrument (`M`): M3 or M8. Provisional: M8.
- Live morph following the audio-rate tuning signal during playback: needs T2.6 + engine reports. Provisional: M3 does UI-driven morph only.
- Expression lanes in M3: velocity + chance only; per-note pitch/gain/timbre curves deferred to M7. Provisional: yes.
- Note-clip loop semantics: notes outside the loop brace kept and hidden (Ableton) vs cropped. Provisional: kept.
- Also now relevant: Q2 (EDO row labels), Q3 (PPQ vs straight 1/64), Q4 (tempo ramp law).

## Q16 · 2026-10-20 · T0.8 · Inter has no ♯ ♭ ♮ glyphs
- Context: SPEC §15 picks Inter (or IBM Plex Sans) for UI text. Inter 4.1 has no U+266F ♯, U+266D ♭ or U+266E ♮ (checked in its cmap; ¢, −, ∞, ×, ≈, arrows and super/subscript digits are present). A microtonal DAW prints accidentals everywhere (note names, MOS degree names, JI labels); missing glyphs render as .notdef boxes.
- Options: A) a fallback face for a small symbol set (e.g. a subset of Noto Music or Bravura Text, both OFL), chosen per code point at shaping time; B) draw accidentals as small vector glyphs/sprites in the atlas; C) spell them `#`/`b` in UI text.
- Provisional choice: none implemented yet (the T0.8 scene avoids them). A is the most general and stays within the text system (shape runs per face); it needs a fallback chain in `TextSystem::shape`. To be decided when note-name UI (T3.x) lands.
- Status: open

## Q17 · 2026-10-20 · T0.9 · Glow passes: alpha (PQ Meter) or additive blending?
- Context: the T0.9 brief says the glow passes go through the additive pipeline; PQ Meter's spectrum bloom (`spectrum.cpp` 1360–1378) and scope glow underlay (`oscilloscope.cpp` 860–880) are drawn through ImGui, i.e. straight alpha blending, and "keep PQ Meter's numbers and look" is also a requirement. Additive changes the look: overlapping passes and crossing traces brighten toward white instead of converging on the trace colour.
- Choice: `scopeTrace`, `scopeEnvelope` and `spectrumCurve` take the glow blend mode as a parameter, defaulting to `BlendMode::Alpha` (PQ Meter's look). The `lines` scene shows both: the green scope trace and the spectrum use alpha glow, the orange Catmull-Rom trace uses additive glow.
- Status: open (a design-review call; switching the default is one line in `pq/ui/lines.h`)

## Q18 · 2026-10-20 · T1.2 · How calls resolve, and where the §4 example's `adsr` comes from
- Context: PATCH_LANGUAGE §3 says a call goes to "a leaf atom, a patch in the same file, or a patch from an imported file", without an order. The §4 example (and SPEC §7.3) call `adsr` without importing anything, and `adsr` is a stdlib composite (SPEC §7.2), not an atom.
- Options: A) atoms first (patches cannot shadow them); B) the file's patches first, then imports, then atoms (patches shadow atoms); C) B, but a patch named like an atom is an error.
- Provisional choice: C. Resolution is file → externals → atom, and `patch sine { }` is rejected, so no call is ambiguous and either A or B stays possible later. For the stdlib, `ParseOptions::externals` takes the signatures of patches that are visible without an import (a prelude); the compiler decides what goes in it. The test corpus imports `adsr` explicitly (`tests/graph/cases/lib/envelopes.pqp`) so the §4 text parses unchanged apart from that import line.
- Status: open

## Q19 · 2026-10-20 · T1.2 · Variadic ports in text (mux, pack, demux)
- Context: the IR gives every repeated port of a variadic atom the same name (`in`, `out`), and a call can only infer the count of repeated *inputs* (from its arguments). `demux`'s output count cannot be written in v0.
- Choices: repeated ports are numbered in text (`in0, in1, …`, `out0, out1, …`), for `.out1` selection and layout `wire` lines; repeated inputs are positional only. A parsed `demux` has `minRepeat` (2) outputs.
- Options for demux later: a const argument giving the count (`demux(i, x, n: 4)`), or counting the highest `.outK` used. Either is additive.
- Status: open

## Q20 · 2026-10-20 · T1.2 · What does a binding target `Ident "." Ident` mean?
- Context: the grammar has `target = Ident | Ident "." Ident (* output name, or local *)` but no semantics for the dotted form.
- Options: A) connect to an input of a named node (`h.trig = rise(g)`); B) bind one output of a multi-output local; C) reject it.
- Provisional choice: A, limited to computed nodes (not params/ins/outs/exports, whose meaning would change the callee signature), and only into an input that is still unconnected; a call's required inputs may be supplied this way. The printer never writes it (it writes the argument in the call), so dropping or changing it later affects only hand-written text.
- Status: open

## Q21 · 2026-10-20 · T1.2 · Places where the grammar needs a line break to be unambiguous
- Context: v0 has no statement separator. Two constructs are ambiguous without one: a port type followed by a member that starts with a type word (`in x : real` / `param y = 1` — `param` is also a rate; or a local named `mono`), and solve-block items, where an equation may start with `(`, `-` or `!` and would continue the previous item (`unknown d = c` / `(d - 1) == 0` reads as the call `c(d - 1)`).
- Options: A) line rules: a port type ends at its line end; in a solve block each item is one line unless inside brackets; B) require `;` or newlines everywhere; C) make the type words reserved.
- Provisional choice: A (the smallest rule that removes the ambiguity; every other line break stays insignificant, and the printer writes one member per line). `%` glued to a number is the percent unit (`50%`), with a space it is modulo, as PATCH_LANGUAGE §1 implies ("no space allowed").
- Status: open

## Q22 · 2026-10-20 · T1.2 · Reading an `out`, and the meaning of `-literal`
- Context: (1) an `out` has no output port in the IR; reading its name (`y = out * 2`) needs an alias to its driver, which text could not print back without inventing a name. (2) `-3` must parse to the same IR that a negative constant prints as.
- Choices: (1) reading an `out` or an `export` is an error with a hint to bind the value to a name. (2) `-` directly followed by a number literal is a negative literal; `-(3)` is `neg(3)`; the printer writes `-(3)` for a negated literal. Only one prefix operator per operand (`-(-x)`), as in the grammar.
- Status: open

## Q23 · 2026-10-20 · T2.4 · Scala ratios beyond int64, and lenient reading choices
- Context: the .scl spec requires numerators/denominators up to 2^31−1 and says Scala itself switches to floating point when a ratio gets too large. Real files (the Scala archive) have ratios of up to 25 digits. Ratio identities are int64 `Rational`s (SPEC §3.1). The spec is also silent on blank lines, lines after the last pitch, and leading-period cents (".5").
- Options for large ratios: A) read error (Overflow); B) convert to cents and forget the ratio; C) keep the exact decimal digits (`ScalaPitch::Kind::LargeRatio`) plus their cents, and write the digits back unchanged.
- Provisional choice: C. Nothing is lost on a round trip, the tuning resolves from the cents (accurate to ~1e-13 ¢), and such a pitch simply cannot become a `Ratio` pitch identity (that would need a bigint type). Reading: blank lines after the description are skipped, lines after the counted pitches are ignored, cents need a digit before the period (Scala's own rule for floating-point input), and equaves at or below 1/1 are accepted (archive files do this: a descending scale ending on 16/29, harmonics + subharmonics ending on 1/1). With these rules a local, uncommitted run over all 5401 files of the Scala archive (version 94) parsed, round-tripped and built a `Scale` from every file. The committed fixtures are authored for pqdaw (tests/tuning/fixtures/scl/SOURCES.txt), so no archive file is redistributed.
## Q16 · 2026-10-21 · T3.4 · Document model choices not fixed by SPEC/TASKS
## Q26 · 2026-10-21 · T3.4 · Document model choices not fixed by SPEC/TASKS
- Context: T3.4 had to settle these to build `pq_doc`; each is local to `src/document` and reversible before T3.5 persists anything.
- Choices (provisional):
  1. **Fold/height are not in the snapshot.** Q15 keeps fold out of undo, but undo restores a stored snapshot pointer, so anything inside the snapshot is undone with it. Fold/height (tracks and lanes) live in `DocumentStore::view()` (`ViewState`, keyed by id), saved with the project by T3.5 but never in history. Alternative: put them in `Track` and accept that undo reverts them (simpler, not Ableton-like).
  2. **Tuning assignment is structural.** `SetTrackTuning`/`SetClipTuning` change which tuning signal node feeds a track's voices (SPEC §3.2), so they recompile. Continuous tuning parameters (reference, EDO equave, MOS generator/period/stretch, MOS region hint) are `Param`; discrete shape (kind, EDO n, MOS n/periods/rotation/MODMOS/generator sequence) is `Structural`. `SetTuningDef` classifies by diff, so a wheel drag that keeps the MOS size never recompiles. Alternative: a per-note tuning index in the SequenceImage would make clip tuning changes `Sequence`-only (T3.17 may choose this and reclassify).
  3. **Sends are implicit**: every track has a send to every return at −∞ (Ableton); only non-default sends are stored. Send level = `Param`; pre/post = `Structural`. Return→return sends are allowed (except to itself); a feedback loop through returns is not rejected by the document — T3.17's lowering must add a block delay or refuse it.
  4. **Volume floor** −70 dB (= −∞, Ableton's fader bottom) so every stored value is finite (JSON). Range [−70, +6] dB for volume and sends; automation values are in the parameter's own unit.
  5. **Record arm is `Meta`** until recording/monitoring exist (M5). Mute and solo are `Param` (Q15); solo needs T3.10's `effectiveAudible` over all tracks.
  6. **Automation lanes**: one lane per parameter project-wide; a track parameter is automated on its own track, a tuning parameter on any track. Breakpoint edits are their own `Automation` class (rebuild the AutomationImage only), lane add/remove is `Structural` (the parameter becomes an audio-rate signal, T3.16).
  7. **Ids**: one project-wide counter (`IdSource`) for every entity kind, allocated by the store before a command is built, never reused after undo (engine state transfer is keyed by id). Track and tuning ids are checked against the project; note/clip/lane/breakpoint ids are trusted to come from the `IdSource` (checked only within the command) to keep edits O(log n).
  8. **Tracks with `Inherit`** follow their parent group's tuning, top-level ones the first main tuning; the project always keeps ≥ 1 main tuning. `RemoveTuning` is refused while a track/clip uses it.
  9. **Scale (T2.4) is not yet in `TuningDef`**: `pq::tuning::Tuning` on main has only EDO and MOS; Scale joins the variant when T2.4 lands (no stub).
  10. **Note chunks hold ≤ 128 notes**, not 256: a full 256-note chunk is 16 KB, the whole per-edit budget.
- Status: open

## Q27 · 2026-10-21 · T3.1 · Keymap chords: layout key or physical key? And what a disabled action does
- Context: SPEC §6.1 copies Ableton's shortcuts, which follow the printed key (layout-dependent: Ctrl+Z is the key labelled Z, wherever it sits). SPEC §6.2's macro mode copies an AHK macro whose number row and `qwerty`/`asdf` rows are positional, so on AZERTY or Dvorak they would move if they matched the layout key.
- Options: A) every chord matches the layout key; B) every chord matches the scancode; C) per-binding choice (a `@scancode` flag in the keymap file).
- Provisional choice: A for T3.1. `KeyEvent` carries both `key` and `scancode`, so C can be added in M8 without touching dispatch (the binding table key just gets a bit). Also provisional: a chord whose action is disabled (enabled predicate false) falls through to the next scope instead of being swallowed, and auto-repeat fires only actions registered as `repeatable` (nudges yes, toggles like `B` no).

## Q28 · 2026-10-21 · T1.3 · How the language expresses the poly→global override (SPEC §7.4)
- Context: SPEC §7.4 / §15: poly→global defaults to sum, "with a per-wire override: last, highest, lowest, loudest voice". PATCH_LANGUAGE v0 has no syntax for a per-wire mode.
- Choice: the reducers are atoms, `voice_sum`, `voice_last`, `voice_highest`, `voice_lowest`, `voice_loudest` (x → global; a global x passes through). `infer()` inserts `voice_sum` wherever a voice signal meets a global bound (a root `out … global`, a callee `in … global`), once per boundary; writing a reducer call in the text (`cutoff: voice_highest(env)`) is the override, and the visual editor can store a wire's mode as that node. On bools, sum = any voice (boolean sum = or), highest/lowest = or/and; loudest = the voice value with the largest |x| (not the voice whose audio is loudest, which would need a second signal). `InferOptions::implicitVoiceSum = false` turns the implicit sum into an error. The reducers have no dsp kernel: the engine implements them over the voice table (T1.8).
- Options: A) reducer atoms (as above); B) a wire attribute in text (e.g. `voice.last(x)`-style annotation or a layout-block wire mode), stored on the IR wire.
- Provisional choice: A. It needs no grammar change and keeps the IR wire-attribute-free; B can be added later by lowering the attribute to the same atoms.
- Status: open

## Q29 · 2026-10-21 · T1.3 · Poly of free-running stateful atoms in a voice patch
- Context: "voice if any input is voice" makes `noise()`, `pulse()` and `phasor(220Hz)` in an instrument patch global: one instance shared by all voices. That is the cheapest, but it is audible: a per-voice phasor would restart its phase at note-on and per-voice noise is decorrelated between voices (N global-noise voices sum to N× one noise, not √N×), and `pulse()` fires once at program start instead of at every voice start.
- Options: A) global (cheapest; per-voice behaviour by making an input per-voice, e.g. `phasor(220Hz, reset: voice.trigger)`); B) per-voice whenever the patch is instantiated in a voice context.
- Provisional choice: A, with `InferOptions::freeRunningPoly` (default global) so the document/engine can choose B per patch without touching the compiler. "Free-running" = stateful with no input above const rate.
- Status: open

## Q30 · 2026-10-21 · T1.3 · Inference defaults and boundary semantics
- Choices made in `infer()` (each local to `src/graph/src/infer.cpp`):
  1. Subcircuits are **flattened** per call site (not instantiated as shared code): each instance is typed and specialised on its own, and T1.4/T1.5 see one flat graph. Flattened names are `<call>__<name>`; hot-swap state is keyed by `TypedPatch::stateKey` (hash of the NodeIds along the call path), not by the renumbered flat NodeIds. Code size grows with the number of instances; sharing code between identically-typed instances is a JIT concern (T1b).
  2. A root `in` with no declared rate is assumed **audio** (and global, mono) unless `InferOptions::inputs` says what is connected; an `in` the engine leaves unconnected reads its default.
  3. A mono signal into a wider declared bound stays mono (broadcast is free); `channel(x, i)` of a mono x is x. A root `out … stereo` may therefore be typed mono: the engine broadcasts.
  4. Callee `param`s accept any rate and poly (they are modulation targets) but keep their declared kind and mono width: a stereo signal into a mono param is an error rather than making the instance stereo.
  5. Every stateful atom outputs audio, even `samplehold`/`latch` of param-rate inputs (which could stay param); cheap to refine later.
  6. Units vs durations: durations are seconds in the language (Q14), but a literal `5smp` is normalised to the number 5 (samples), so `delay(x, 5smp, 1s)` would mean 5 s. T1.4 should convert `smp` literals to seconds (`/ sr`) where they meet a seconds input, or reject them there. → Handled by T1.4: Q35.

## Q32 · 2026-10-21 · T3.5 · Project file choices (Scale in TuningDef, reader, strictness)
- Context: T3.5 adds Scale to `TuningDef` and fixes the `project.pqd` format (schemaVersion 1). These choices are not settled by SPEC §12 or TASKS; each is local to `src/document` (and one small `tuning` change), and every format change from here on needs a schema bump plus a migration.
- Choices (provisional):
  1. **Scale tunings store the .scl and the optional .kbm** (`ScaleDef`), and `TuningDef::referenceHz` stays the single reference pitch (the Hz of degree 0) for every kind. Importing with a .kbm derives it once (`makeScaleDef` → `makeScalaTuning`); after that the mapping only decides key → degree (and is kept for export). Alternative: let the .kbm's reference key/frequency drive the pitch, which would make `TuningReference` automation meaningless for mapped scales. Classification: reference = `Param`; pitches or mapping = `Structural` (discrete shape, as Q26.2); description alone = `Meta`. The description must be valid UTF-8 (JSON); `makeScaleDef` converts Latin-1 descriptions with `descriptionUtf8()`.
  2. **The load/save path does not use nlohmann/json**: its parser alone measured 114–133 ms (SAX) / 239–373 ms (DOM) on a 100k-note file against a 250 ms budget, while a specialised pull reader + streaming writer load in 54–65 ms and save in 42–50 ms. nlohmann/json is still a `pq_doc` dependency, used for schema migrations (generic DOM edits of old files, then the normal decode).
  3. **Strict reading, canonical writing**: unknown or duplicate keys are errors; lists the model keeps sorted (clips, locators, breakpoints, sends) must be sorted in the file (notes may be in any order); a send at the implicit default (−70 dB, post-fader) must not be stored; view-state ids must name existing tracks or lanes; ids must be in [1, 2³²−2] so `IdSource::reserveThrough` never wraps. Writing omits view entries for ids that no longer exist and note fields at their defaults. Alternative: normalise silently (sort, drop defaults), which hides hand-editing mistakes.
  4. **`Edo::makeCents` keeps the given cents exactly** (`equaveCents()` used to be `1200·(c/1200)`, which can differ in the last bit), so an EDO with a cents equave round-trips bit-exactly through a file.
  5. **Floats are written as their shortest float representation** (`0.1`, not `0.10000000149011612`) and read with `from_chars<float>`, so they round-trip exactly without double rounding. −0.0 keeps its sign (every float is written with a '.' or an exponent).
- Status: open

## Q31 · 2026-10-21 · T0.7-fix3 · HiDPI: font-role rounding at fractional scales, hairlines, atlas capacity
- Context: font roles render at `round(role px × scale)` (the task text). At 125 % that gives section 12 → 15, body 13 → 16, title 14 → 18 (17.5 rounds away from zero), so title is 2 px above body instead of 1. At 150 %: 18 / 20 (19.5) / 21. Separately, "line widths and feathers stay in device px" means 1 px grid lines and dividers become physically thinner at 150–200 %.
- Options (rounding): A) round half away from zero (now); B) round half down (125 %: 15 / 16 / 17, keeps the 1 px steps); C) a per-scale size table in the theme (T3.2), e.g. tuned by eye per scale.
- Options (hairlines): A) always 1 device px (now, per the review); B) `max(1, floor(scale))` device px, so 200 % gets 2 px lines while 125–175 % stay 1 px.
- Provisional choice: A and A. Both are one-line changes (`UiScale::fontPx`, the scenes' hairline rects) and T3.2's theme can switch to a table without touching callers, since every caller goes through `UiScale`/`FontRole`.
- Atlas note: the single 1024² glyph page holds the 7 scene font roles at 5 scales (1.0–2.0) with ASCII warmed in 967 of 1024 rows (test_hidpi). A window that visits many scales, or T3.2 adding roles, will fill it; multi-page atlas with eviction is already T0.8-fix1. Until then a full atlas drops new glyphs (counted in `TextSystem::Stats::atlasFull`).

## Q33 · 2026-10-21 · T3.3 · Grid details not fixed by SPEC §9.2 / Live's manual
- Context: SPEC §9.2 and TASKS T3.3 fix the grid's menu and keys, not its geometry in odd meters, the adaptive thresholds, or the exact relative-snap/quantize arithmetic. Live's manual doesn't document these either.
- Options / provisional choices (all in `pq/core/grid.h`; each is one function to change, nothing is persisted except `GridSpec`):
  1. Adaptive density: 7 levels −3…+3 (Narrowest, Narrower, Narrow, Medium, Wide, Wider, Widest) as TASKS says, vs Live's 5. Minimum cell = 24·2^level logical px (3…192 px). Alternative: 5 levels mapped onto −2…+2.
  2. Note cells restart at every bar line (7/8 on 1/4: three quarters + one eighth). Adaptive skips note values longer than a beat that don't tile the bar (3/4 goes 1/4 → 1 bar, never 1/2); fixed values are honoured as chosen. Alternative: cells continuous across bars (lines drift off bar lines in odd meters).
  3. Zoomed-out multi-bar cells (1, 2, 4, 8… bars) count from the first bar of each time-signature segment, so a signature change starts a new cell. Alternative: count from bar 1 globally (a cell could straddle a signature change).
  4. Tuplet groups that don't fit the rest of the bar are cut off by the bar line (e.g. 5-tuplet 1/2 = 5 lines per 2 whole notes in a 4/4 bar shows 3 lines, then the bar). Alternative: groups flow across bar lines.
  5. Relative snap (`snapDelta`): anchor = last grid line ≤ origin; delta = nearestLine(anchor + raw) − anchor. On-grid objects snap to lines; off-grid objects move in whole cells keeping their offset. Ties go to the later line everywhere.
  6. Quantize: start-only keeps the length (moves the note, Live's default); start+end quantizes the end to its nearest line, or the line after the quantized start if it would collapse. Amount rounds half up in ticks. Quantize ignores the snap toggle; an Off grid quantizes nothing (T3.6 may pass a fixed grid instead).
  7. Ruler labels: bar numbers are 1-based and skip 0 (pre-roll is −1, −2…); "17" / "17.3" / "17.3.2"; the tick remainder inside a sixteenth (tuplets, 1/32) is returned but not printed.
- Provisional choice: as listed (most reversible: pure functions, no stored data depends on them).
- Status: open

## Q34 · 2026-10-21 · T1.4 · How exact must the optimiser be? (signed zeros, NaN order, voice reducers)
- Context: TASKS T1.4 lists "×1, +0, ×0 when the other side is finite"; the JIT policy (TASKS T1b) allows `contract` and `afn` but not `nnan`/`ninf`, and says nothing about `nsz` (no signed zeros).
- Choices (all in `src/graph/src/optimise.cpp`, documented in `pq/graph/optimise.h`):
  1. **No nsz.** `x + (−0) → x` and `x − (+0) → x` always; `x + (+0)` / `x − (−0)` only when x can never be −0 (a small fact analysis: literals, phasor, gate_to_real, abs, clip with non-negative literal bounds, sums/products of such values). So `param + 0` stays an add (it is param rate, so it costs one op per block). `x · (±0) → ±0` only when x is provably finite AND ≥ +0 (otherwise the zero's sign, or NaN for an infinite x, would change).
  2. **afn**: `pow(x, 2) → x · x` (the correctly rounded square; the CRT pow is within a few ulp of it). **contract**: `a · b + c → muladd` when the product has one consumer and the same rate/poly/width as the sum. No reassociation (`(x·a)·b → x·(a·b)` would hoist more but is not in the policy).
  3. **min/max are not treated as commutative** by CSE, although the registry flags them `kCommutative`: the kernels return the second operand when either is NaN, so `min(a, b)` and `min(b, a)` differ on NaN. The registry flag is left as is (it is right for every non-NaN input and other users may want it).
  4. **A per-voice value is never replaced by a global one** (e.g. `mux(0, p, voice.hz)` → `p` is refused): a downstream `voice_sum` sums N equal voices but passes a global value through once, so the replacement would change the sum. Same for width (a `pack` concatenates widths, so a stereo value is never replaced by a mono one).
  5. **Folding never produces inf/NaN literals** (Patch::validate requires finite literals): such a node stays a const-rate computation with the same runtime value.
- Options: A) as above (exact, the conservative reading of the JIT policy); B) allow `nsz` in the optimiser and the JIT: `x + 0 → x` for any x, `x · 0 → 0` for finite x of any sign. The difference is only observable through a division by the resulting zero (±inf) or `pow(±0, negative)`.
- Provisional choice: A (reversible: adding nsz is a few lines in `simplify()` behind an option).
- Status: open

## Q35 · 2026-10-21 · T1.4 · Where durations become samples, and `smp` literals (resolves Q30.6)
- Context: kernel_map.h marks delay `time`, timer `dur` and pulse `period` as `InputSeconds` ("· sr inserted by the compiler"); Q30.6 asked T1.4 to convert or reject `smp` literals meeting a seconds input.
- Choice: `optimise()` inserts the conversion as an ordinary `mul(x, sr)` node in front of every InputSeconds input and sets `TypedPatch::durationsInSamples`; folding then turns a const duration into a sample count (with a known sample rate), and a param duration stays a param-rate multiply (once per block). A literal written in `smp` that feeds such an input directly is already in samples and is wired as is (exact; no `/ sr · sr` round trip). An `smp` literal into a delay's `max` (InitMaxDelaySeconds, consumed by the builder as ceil(max · sr)) becomes seconds (n / sr) when the sample rate is known; the builder's ceil can then only round up by one frame. Without a known sample rate it stays as written and T1.7 must read its unit. An `smp` literal that goes through arithmetic first (`5smp * 2`) has lost its unit (the language has no dimensions) and is read as seconds.
- Options: A) as above; B) reject `smp` literals at seconds inputs (simpler, but `delay(x, 1smp, …)` is a natural way to write a one-sample delay line); C) T1.7 does the lowering (then nothing folds or hoists it).
- Provisional choice: A.
- Status: open

## Q36 · 2026-10-21 · T0.8-fix1 · Where the shared sprites live once glyph pages are R8
- Context: the task says "R8 glyph pages separate from the RGBA sprite page". Today's "sprite page" holds only the white texel and the glow blob, and both are pure coverage (white with alpha). A separate RGBA sprite texture would split every widget's fill and its label into two draw commands (the reason T0.8 shared one page).
- Options: A) put the white block + blob into every R8 glyph page (64×128 texels per page) and give the draw list a texture id `kAtlasTexture` that is compatible with any page, so fills merge with text; B) a separate RGBA sprite texture plus a two-texture shader (or a per-draw flag), keeping fills and text in one command at the cost of a second sampler per fragment; C) a separate RGBA sprite page and accept a draw-command split per label.
- Provisional choice: A. One pipeline, one sampler, no command splits (the text scene is still 3 commands; a test checks fills between labels stay in one command), glyph atlas memory exactly 1/4 of the RGBA page. Colour sprites (icons) can be added later as ordinary RGBA textures (`createTexture`) or as a dedicated RGBA atlas page id when T3.x needs them.
- Status: open

## Q37 · 2026-10-21 · T0.8-fix1 · Reading "no frame > 1.5× the median frame time" for 200 new glyphs
- Context: rasterising 200 hinted glyphs with FreeType costs ≈ 3–4 ms of CPU in release (≈ 15–20 µs each, unchanged by this task) and ≈ 15 ms in the dev preset (/Od dependencies). The median *CPU work* of a frame is ≈ 0.1 ms, so no implementation that rasterises within the frame could meet 1.5× of that; the criterion is met for the *frame interval* of a paced animated run (what the user sees), where the old blocking path added a device-idle wait plus a second blocking submit.
- Choice: the window self-test runs a paced animated run, introduces 200 new glyphs in one frame and checks every present-to-present interval ≤ 1.5× the run's median (3 attempts on a shared machine). Enforced in release builds; in the dev preset (unoptimised FreeType) it is measured and printed only. Spreading rasterisation over several frames (showing glyphs late) or a rasterisation thread would be the next step if 240 Hz displays need it.
- Status: open

## Q38 · 2026-10-21 · T0.7-fix4 · Texture ids are reused without a generation
- Context: `destroyTexture` frees the slot once the frames that may sample it are done, and `createTexture` hands the id out again (the table has 256 slots). A draw list that still names the old id would sample the new texture.
- Options: A) plain reuse, caller contract "never draw a destroyed id" (now); B) generation bits in `TextureId` checked by `record()` (stale ids fall back to the white texel).
- Provisional choice: A (zero cost, ids stay small and dense). B is a local change to `TextureId` packing in renderer.cpp/draw_list.h if T3.x lifetimes get hard to reason about.

## Q39 · 2026-10-21 · T3.6 · Automation curve law (needed before T3.15/T3.16 define it)
- Context: `Breakpoint::curve` ∈ [−1, 1] had no law ("T3.15/T3.16"). Insert/delete/duplicate time, shapes and lock-envelope moves must cut segments without changing the envelope, which needs the law now.
- Options: A) exponential family `v = va + (vb − va)·(e^{k·x} − 1)/(e^k − 1)`, k = 6·curve: closed under cutting (the part [0, x0] is the same family with curve·x0, [x0, 1] with curve·(1 − x0)), so every cut is exact; B) a power curve x^p (Ableton-like look, not closed under cutting: cuts would need extra points and still be approximate); C) leave curves to T3.16 and make time edits cut only linear segments exactly.
- Provisional choice: A, in `pq/document/automation_curve.h` (header-only, noexcept). T3.16's `evalAutomation` must use the same law: move the header to `core` then (the engine cannot include `document`). K = 6 is a look choice (at curve 1 the midpoint is at 4.7 % of the rise).
- Status: open

## Q40 · 2026-10-21 · T3.6 · Up/Down on reduced ratios, Harmonic notes, and non-ratio equaves
- Context: SPEC §6.1 says Ratio/Harmonic notes move to `(n ± 1)/d`, and TASKS asks for 5/4 → 6/4. A `PitchId` ratio is stored reduced, so 6/4 is 3/2, and the next Up from 3/2 is 4/2 = 2/1, not 7/4: the fundamental 1/4 is lost.
- Options: A) step on the reduced ratio's own denominator, with an optional shared denominator D from the caller (`harmonicDenominator`: the step uses lcm(d, D)), so a view can keep a chord's fundamental (e.g. D = lcm of the selection's denominators, or a value remembered during a key-repeat gesture); B) store unreduced ratios (changes PitchId's canonical form and equality); C) a per-note "fundamental denominator" field (Note is at its 64-byte budget).
- Provisional choice: A (`ops::stepPitch`). Also: Harmonic notes step voiceIndex ± 1 (≥ 0; the upper bound needs the JI object, which the model does not have yet) and do not move by equave; Ratio Shift multiplies by the tuning's equave ratio when it is exact (EDO ratio equave, Scala ratio equave) or, for a cents equave (MOS, EDO in cents), by p/q with p, q ≤ 64 within 0.001 ¢ of it (2/1, 3/1…); otherwise the note does not move. Cents notes step to the next line of the tuning's degree lattice (all degrees in all equaves, MOS stretch included; a line within 1e-6 ¢ counts as "on" it) or of an EDO display grid when the view passes one.
- Status: open

## Q41 · 2026-10-21 · T3.6 · Clip and time-edit semantics not fixed by SPEC/TASKS
- Context: Live's manual describes these operations but not every edge; T3.6 had to pick.
- Provisional choices (all in `pq/document/edit_ops.h`, pure functions over snapshots, nothing persisted depends on them):
  1. Clips on a track never overlap: inserting, moving, copying, duplicating or growing a clip overwrites what is under it (covered clips removed, partly covered ones trimmed or split). Trimmed clips keep their hidden notes (Q15 "kept").
  2. Split: a non-looped clip's notes go to the piece they start in (same ids); later pieces of a looped clip copy its content (fresh ids) and start at the matching loop position. A note across the cut is cut by the clip end, as Live plays it.
  3. Consolidate: one looped clip over [first start, last end) per track, including unselected clips inside the span; looped content unrolled; notes cut where playback cut them; notes of deactivated clips arrive disabled; the first occurrence of a note keeps its id, repeats get fresh ids; refused when the clips' tuning assignments differ (Degree notes would change pitch).
  4. Time edits act on all tracks. Insert time holds the envelope's value through the gap; delete time leaves a jump at the seam where the two sides differ; duplicate time copies the envelope exactly. Locators inside a deleted range are removed; duplicate time does not duplicate locators. A loop that a deletion collapses keeps its length from the seam. Tempo follows exactly except a jump right after a ramp (not representable in `TempoPoint`, the ramp then ends on the new value). Time-signature changes move with the edit and snap to the next bar line when they no longer land on one. Tempo and signature edits before tick 0 act from tick 0.
  5. Lock envelopes off (`moveAutomation`): only for clips that stay on their track; the vacated range holds the value on its left; the destination range is overwritten. Copies copy the envelope.
  6. Breakpoints on one tick are ordered by id (T3.4); when an edit needs a created point before an existing one on the same tick, the existing one gets a fresh id (rare: jumps at edit seams).
- Status: open
