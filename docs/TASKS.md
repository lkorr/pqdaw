# pqdaw build plan

Status markers: `[ ]` open · `[~]` in progress (note who and which branch) · `[x]` done (note the commit) · `[!]` blocked (reason in PROGRESS.md).

**Rules** (full version in [CLAUDE.md](../CLAUDE.md)):
- A task is done only when every acceptance criterion holds, verified by commands that were actually run.
- Pick tasks whose dependencies are `[x]`.
- Tasks marked 👁 need the user's eyes. Build them to the acceptance criteria and add the result to `docs/REVIEW.md`, but don't treat their look or feel as final.

Every task also carries these **standing criteria**, which aren't repeated below:
1. `scripts/build.ps1` (dev) and `scripts/build.ps1 -Preset release` both build with zero warnings, and **all** tests pass.
2. New code has tests. RT-path code has an RtScope allocation-trap test (once T0.3 exists).
3. Performance-relevant code has a benchmark, with its result appended to `bench/RESULTS.md` (release build, machine noted).
4. There are no stubs, `TODO: implement` notes or fake returns in code that claims to be done.
5. `docs/PROGRESS.md` has an entry for the task.

---

## M0: Foundation

- [x] **T0.1 Build system and toolchain.** CMake + Ninja + MSVC presets, LLVM 23.1.3 prebuilt linked (/MT), doctest, a smoke test proving that ORC JIT runs, CPU feature detection. *(Done during setup; dev + release green; release smoke exe with full JIT = 29 MB.)*

- [x] **T0.2 RT primitives** *(90951f1)* (`core`). Depends on: T0.1.
  - `SpscRing<T>` (power-of-two, cache-line-padded indices, acquire/release only). `MpscQueue<T>` (bounded). `FixedPool<T>`. `Arena` (bump allocator with 64-byte alignment). `EpochReclaimer` (the audio thread marks quiescent states; reclamation runs off the audio thread). `DenormalGuard` (RAII FTZ/DAZ). `AlignedBuffer<T>`.
  - Accept: unit tests for each. A two-thread stress test pushes 10⁷ items through the SPSC and MPSC queues with no loss, duplication or reordering. A benchmark records the SPSC push+pop cost in ns.

- [x] **T0.3 Allocation trap** *(5dd615d)* (`core`). Depends on: T0.1.
  - Replace global `operator new/delete` (all overloads) in test binaries and dev builds, with a thread-local "realtime" flag set by `RtScope`. Any allocation or free inside an RtScope fails loudly: test failure, or abort with a stack trace in the app.
  - Accept: a test shows that allocating inside an RtScope is caught and that allocating outside it is fine. The trap compiles out of release builds of the app.

- [x] **T0.4 Fast math** *(a0dd5ef)* (`core`). Depends on: T0.1.
  - `fastTan`, `fastExp2`, `fastLog2`, `fastTanh`, `fastSin` (scalar + AVX2 8-wide), with error bounds documented in their headers. `centsToHz`/`hzToCents`, `dbToGain`/`gainToDb`.
  - Accept: tests sweep every function over its stated domain against `double` std:: references and check the documented error bounds (max relative error and max absolute error in cents where relevant). A benchmark compares each against the std:: version.

- [x] **T0.5 Time types** *(ac48462)* (`core`). Depends on: T0.1.
  - `Ticks` (int64, PPQ 27720), a tempo map (with ramps) converting ticks↔seconds↔samples exactly or within ½ sample, and time signatures.
  - Accept: tests include round trips over a 10-hour session at 192 kHz with tempo ramps, staying within ½ sample, and exact tuplet positions for 3/5/7/11.

- [x] **T0.6 Audio device I/O** *(1d54a68)* (`engine`). Depends on: T0.2.
  - RtAudio (pinned) with WASAPI (shared and exclusive) and ASIO. Device enumeration, open/start/stop, an xrun counter, and a `NullDevice` backend for tests (driven by a thread, or manually from a test).
  - `pqdaw --list-devices` and `pqdaw --sine 440 --seconds 3 [--device N] [--asio]`.
  - Accept: NullDevice tests cover the callback cadence and buffer sizes. `--list-devices` output is pasted into PROGRESS.md. A `--sine` run on the default WASAPI device reports 0 xruns over 3 s (no one is listening, so log the callback stats).

- [x] **T0.7 Window + Vulkan renderer** *(b15670c)* (`ui`). Depends on: T0.1. 👁
  - SDL3 window, Vulkan instance/device/swapchain (VMA), and a draw list with the `{float2 pos, float2 uv, u32 rgba8}` vertex format and **32-bit indices**. Pipelines for alpha, additive and textured drawing. Damage-driven redraw, and `DwmFlush` frame pacing.
  - An **offscreen render path**: `pqdaw --screenshot <scene> <out.png> [--size WxH]` renders a named test scene to a PNG with no window.
  - Accept: the `test-pattern` scene (rects, rounded rects, gradients, additive blobs) matches a committed golden PNG within a per-pixel tolerance. The resize and minimise paths don't crash (scripted test).

- [~] **T0.8 Text** *(worker J, branch task/T0.8-T0.9-text-lines)* (`ui`). Depends on: T0.7. 👁
  - FreeType (hinted) + HarfBuzz, a glyph atlas at integer pixel sizes (12/13/14 px plus a large size for meters), Inter (OFL) bundled under `assets/fonts/`.
  - Accept: a `text` screenshot scene matches its golden image. A test checks the shaped advance widths of a known string.

- [~] **T0.9 PQ Meter line primitives** *(worker J, branch task/T0.8-T0.9-text-lines)* (`ui`). Depends on: T0.7. 👁
  - Port `smooth_line.h` (feathered ribbons: 0.75 px feather, 2.5× miter clamp, bevel past 120°, Catmull-Rom subdivision capped at 6000 points), `gradientPolyline`, `gradientBand`, `splineSmooth`, `gradient.h` and `palette_track.h` from `C:\Users\Luke\Desktop\programming\pq meter\src\ui\` and `src\modules\spectrum.cpp:200-325` into the draw-list API.
  - Accept: a `lines` screenshot scene (sine, noise trace, glow passes) matches its golden image, and the PROGRESS.md note says which PQ Meter files the port came from.

## M1: Engine core

- [x] **T1.1 Patch IR** *(1c5ad8d)* (`graph`). Depends on: T0.2.
  - Nodes, ports (kind/rate/poly/width), wires, params (with attrs), exports, stable `NodeId`s, and layout data. Atom registry: name → signature (inputs, outputs, state size, default rates).
  - Accept: tests construct graphs programmatically and validate structure, including rejecting dangling ports and type-mismatched wires.

- [~] **T1.2 Parser + printer** *(worker I, branch task/T1.2-parser-printer)* (`graph`). Depends on: T1.1.
  - Implements `docs/PATCH_LANGUAGE.md` v0 exactly: units, imports, layout, solve blocks. Errors carry file:line:col plus a caret snippet.
  - Accept: a round-trip test (`parse(print(p)) == p`) over every file in `stdlib/` and `tests/graph/cases/`. At least 30 error-case tests with expected messages. A fuzz-ish test runs 10⁴ random token streams with no crashes.

- [ ] **T1.3 Inference** (`graph`). Depends on: T1.2.
  - Rate, kind, poly and width inference with the upper-bound semantics. Poly→global defaults to sum. Explicit conversions are required.
  - Accept: table-driven tests (inputs → inferred types) for 40+ cases, including modulation wires raising an unmodulated `param` input to `audio`, and poly→global summing.

- [ ] **T1.4 Optimiser** (`graph`). Depends on: T1.3.
  - Constant folding (never `param`), DCE, CSE, algebraic simplification (×1, +0, ×0 when the other side is finite), and hoisting param-rate subexpressions out of audio-rate paths.
  - Accept: tests on before/after IR. A test proves `tan(π·cutoff/sr)` is hoisted to param rate when cutoff is unmodulated, and stays audio-rate when it is modulated.

- [ ] **T1.5 Scheduler** (`graph`). Depends on: T1.4.
  - Topological order, Tarjan SCCs, explicit-delay feedback handled, a zero-delay cycle outside `solve` rejected (text) or given an inserted `z1` plus a warning (visual mode flag). Block regions: per-sample for feedback SCCs, blocks elsewhere. Buffer liveness and reuse. State layout keyed by NodeId with type tags.
  - Accept: tests on schedule structure. The scratch-buffer count for a 100-node chain is ≤ 3. The state layout for an unchanged node is identical across reschedules.

- [~] **T1.6 Leaf atom kernels** *(worker G, branch task/T1.6-leaf-atoms)* (`dsp`). Depends on: T0.4.
  - Every leaf atom in SPEC §7.2: math, logic (bool lanes as masks), memory (`z1`, `delayline` with linear/allpass/cubic reads), generators (`phasor` with a **uint32 phase accumulator**, `sine`, `wavetable` with band-limited mipmaps, `noise`), `shaper`, `sampleRead`. Each has a scalar and an AVX2 path, block-wise.
  - Accept: each kernel is tested against a naive `double` reference. A phasor test shows zero drift over 10⁹ samples, with its frequency exact to the uint32 resolution. Benchmarks are recorded per atom (ns/sample, 1 and 8 lanes).

- [ ] **T1.7 Interpreter backend + ProgramImage** (`graph` + `engine`). Depends on: T1.5, T1.6.
  - Flat op list, parameter block, `ProcessContext`, and the state arena.
  - Accept: the interpreted output of the patches in `tests/graph/cases/` matches a slow reference evaluator (sample-by-sample graph walk in `double`) within 1e-5. RtScope tests show process() never allocates.

- [ ] **T1.8 Voices** (`engine`). Depends on: T1.7.
  - Voice allocator (max voices, stealing policy), the event list, voice lanes (8 per AVX2 group), the `voice.*` namespace, voice end on silence/envelope.
  - Accept: tests cover allocation and stealing, note on/off sample accuracy (an event at sample k takes effect at k), and 64 voices with no allocation.

- [ ] **T1.9 Hot swap** (`engine`). Depends on: T1.8.
  - Publishing a ProgramImage from a worker thread. Swap at a block boundary. State transfer by NodeId+type tag. Crossfade (5–20 ms) when audible, instant when silent. Epoch reclamation of old images.
  - Accept: an offline test swaps a modified patch mid-render. Phase continuity of an unchanged oscillator is exact. There is no discontinuity above −90 dBFS at the swap point for an unchanged signal path. RtScope clean.

- [ ] **T1.10 Offline renderer + golden tests + `pqc` CLI** (`engine`, `tools`). Depends on: T1.7.
  - `pqc check|dump-ir|dump-schedule|render <file.pqp> [--notes "…"] [--seconds N] -o out.wav`. A golden-audio comparison helper (absolute + spectral tolerance).
  - Accept: golden renders committed for 5+ patches. `pqc` usage is documented in `tools/README.md`.

- [ ] **T1.11 Benchmark harness** (`bench`). Depends on: T1.7.
  - A Google Benchmark-style runner (or a hand-rolled one with warmup/repeats/median), with CSV output and an append-to-`bench/RESULTS.md` script.
  - Accept: benchmarks exist for SPSC, the atoms, and an interpreted 32-node patch at 1/8/64 voices.

## M1b: JIT

- [ ] **T1b.1 LLVM codegen** (`jit`). Depends on: T1.7.
  - Lowers a Schedule to LLVM IR with the same ProgramImage ABI. Scalar first. Host CPU features and target machine options. Fast-math flags policy: `contract` and `afn` allowed; no `nnan`/`ninf`; denormals FTZ.
  - Accept: every interpreter test passes with the JIT backend producing output identical within 1e-6. Compile time for a 100-node patch is recorded.

- [ ] **T1b.2 Voice-SIMD codegen** (`jit`). Depends on: T1b.1.
  - Vector types across voice lanes (8× AVX2, 16× AVX-512 when the host has it). Bool masks as vector predicates.
  - Accept: 8- and 64-voice benchmarks show at least 5× over scalar JIT for an oscillator+filter voice.

- [ ] **T1b.3 Stock subcircuits** (`stdlib`). Depends on: T1b.1.
  - In `.pqp`: `onepole`, `svf` (TPT), `biquad` (TDF-II), `allpass`, `dcblock`, `comb`, `adsr`, `lfo`, `envfollow`, `slew`, `smooth`. Each has a hand-written C++ reference implementation in `tests/` (not shipped).
  - Accept: output matches the references within 1e-5 (sweeps, impulse responses, modulated cutoff). **JIT'd stdlib filters are within ~10% of the hand-written C++ cost** (benchmark recorded). If they're slower, fix the compiler, not the filter.

- [ ] **T1b.4 AOT stdlib + compile cache** (`jit`). Depends on: T1b.3.
  - At build time, `pqc aot` compiles `stdlib/` into an object file linked into pqdaw. Runtime JIT objects are cached under `cache/` keyed by graph hash + CPU features + compiler version.
  - Accept: opening a stock device at runtime does no JIT compile (test hook counts compiles). A second load of the same user patch hits the cache.

- [ ] **T1b.5 Interpreter→JIT live swap** (`engine` + `jit`). Depends on: T1b.1, T1.9.
  - An edit plays immediately through the interpreter, then swaps to the JIT image when it's ready.
  - Accept: an offline test with a simulated compile delay. The swap is click-free (same criteria as T1.9). The JIT ProgramImage costs less than the interpreter's (benchmark).

- [ ] **T1b.6 LLVM size check**. Depends on: T1b.4.
  - Measure the release `pqdaw.exe` size and the contribution of the LLVM components. Try `/OPT:REF /OPT:ICF` and pruning the component list.
  - Accept: numbers in PROGRESS.md. If the result is > 30 MB, write up the asmjit option (SPEC §2 table) for the user in `docs/QUESTIONS.md`.

## M2: Tuning

- [x] **T2.1 Pitch identities + resolution** *(4f3b5c1)* (`tuning`). Depends on: T0.1.
  - `PitchId` (Degree/Ratio/Cents/Harmonic), a rational type (int64 n/d, overflow-checked) with monzo conversion up to a prime limit, and `resolveHz`. Reference pitch defaults to C3 = 261.626 Hz.
  - Accept: tests; ratio composition of 50 random intervals stays exact; the float32 Hz result is within 0.001 ¢ of a `double` reference.

- [x] **T2.2 EDO** *(4f3b5c1)* (`tuning`). Depends on: T2.1.
  - Any n, any equave. Patent val for primes up to 31. Step labels `k\n`.
  - Accept: tests against known vals (12, 19, 22, 31, 41, 53, 72).

- [x] **T2.3 MOS port** *(344dccf)* (`tuning`). Depends on: T2.1.
  - Port from `C:\Users\Luke\Desktop\programming\music-website\apps\web\public\mos-explorer\index.html` and `mos-calc.js`: `buildMOSScale`, `computeHardness`, `buildMOSRegions` (Stern–Brocot, mirrored, multi-period), `findParentMOS`/`getChildMOSes`, the generator chain, brightness/UDP, `computeGMN`, step pattern, MODMOS, `buildGenSequenceScale`, octave stretch, `generatorForHardness`, `transferModeRotation`, `updateHeldNotes` semantics, and the `TEMPERAMENTS`/`MOS_NAMES`/`MODE_NAMES` data from `mos-data.js`.
  - Accept: `tests/tuning/fixtures/mos_*.json` generated by **running the original JS under Node** (script committed in `tools/fixtures/`) over a sweep of ≥ 2000 (generator, size, period, mode) inputs. The C++ matches every value within 1e-6 ¢ and every string exactly.

- [~] **T2.4 Scala** *(worker K, branch task/T2.4-T2.5-scala-ji)* (`tuning`). Depends on: T2.1.
  - `.scl` and `.kbm` parse/write, following the Scala format spec, including the comment, ratio and cents forms and the edge cases.
  - Accept: tests on 20+ real-world `.scl` files (committed fixtures; public-domain sources noted), plus round trips.

- [~] **T2.5 JI utilities** *(worker K, branch task/T2.4-T2.5-scala-ji)* (`tuning`). Depends on: T2.1.
  - Prime-limit checks, Tenney height, octave reduction, harmonic/subharmonic series helpers, the fewest-keypress BFS from the JI trainer (`game-pianoroll.js`), and interval naming tables (colour notation/FJS) ported from `ji composition trainer/interval names.txt` + `data/intervalNames.js`.
  - Accept: tests, including BFS results matching the JS for a fixture set generated under Node.

- [ ] **T2.6 Tuning as signal** (`tuning` + `graph`). Depends on: T2.3, T1.7.
  - The per-region linear form `cents = k·g − m·P + e·P` (SPEC §3.3). A tuning node in the graph whose generator/period/stretch/reference are modulatable. `voice.hz` derives from it. Region crossings are detected at block rate or at the crossing sample.
  - Accept: a generator sweep through several regions produces continuous per-voice pitch (except at intended region changes) that matches `buildMOSScale` evaluated per sample within 1e-4 ¢. A benchmark shows the per-voice pitch cost.

- [ ] **T2.7 Main tunings model** (`document`). Depends on: T2.6.
  - Named project tunings, track/clip assignment and override, per-tuning reference pitch, serialization.
  - Accept: tests; reassigning a track re-resolves all its notes; JSON round trip.

## M3 onward (outline: refine into tasks before starting; most need user review)
- **M3**: document model + undo, arrangement view, tracks/groups/mixer basics, note clips, piano roll (EDO / proportional MOS / equidistant), MOS wheel, adaptive/tuplet grid, Ableton hotkeys, automation lanes. 👁
- **M4**: circuit-diagram editor + text pane, device panels, cross-device wiring, Audio Effect/Instrument racks + macros, Operator, Utility, EQ Eight, Auto Filter, Saturator. 👁
- **M5**: browser + user library, Simpler, Drum Rack. 👁
- **M6**: audio clips, warp, recording, take lanes, Capture MIDI, PDC, metronome.
- **M7**: JI objects. 👁
- **M8**: macro mode, MIDI/key mapping.
- **M9**: remaining devices + parity suite (needs Live 12 reference renders from the user).
- **M9 (RMSC)**: RMSC per SPEC §8.1. Validate against the original by rendering Borrasca in Live 12 (user-supplied renders) with identical settings; the original's known bugs (RM pre-fade step, NaN, Padé clipper) are expected differences.
- **M10**: groove, transformations, export, freeze, bounce, collect.
- **M11**: interop (MIDI files, MTS-ESP, MPE).
- **M12**: background re-optimisation, multithreaded scheduling.
