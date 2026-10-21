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
- Differences from the T1.1 registry (Q8), for reconciliation: kernel names follow the registry (`srff dff tff samplehold delay delay_cubic delay_allpass fast_sqrt`); the kernels additionally have `fast_cents_to_hz`, `fast_hz_to_cents`, `fast_db_to_gain`, `fast_gain_to_db`, `muladd`, `const`. Delay and timer durations are in **samples** in the kernels (the compiler multiplies seconds by `sr`); delay minimums are 1 (linear), 2 (cubic), 1.5 (allpass) samples, not 1 for all, because the newest interpolation tap must be ≥ 1 sample old for `x` to be a delayed input. `pulse` takes an optional `period` input (a constant 0 gives the registry's "true on the first sample"). `z1` has no `init` input (state starts at 0; initState could take one). The registry's stateless `sampleRead(sample, pos, loopStart, loopEnd)` corresponds to `sample_pos_*` (no loop wrap yet) and the stateful 32.32 playhead `sample_read_*(rate, reset, start, loopStart, loopEnd)`; a float `pos` input loses sub-sample precision beyond 2^24 samples, which is why the playhead form exists. `phasor`/`sine_hz` have no split kernels yet, so an FM feedback loop through `hz` needs an explicit `z1` until T1.5/T1.7 decide how delayed inputs are scheduled.
- Status: open
