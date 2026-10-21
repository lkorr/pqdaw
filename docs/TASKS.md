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

- [x] **T0.6 Audio device I/O** *(1d54a68; partial: WASAPI exclusive unsupported by RtAudio, ASIO opt-in — see T0.6-fix1/fix2, Q12)* (`engine`). Depends on: T0.2.
  - RtAudio (pinned) with WASAPI (shared and exclusive) and ASIO. Device enumeration, open/start/stop, an xrun counter, and a `NullDevice` backend for tests (driven by a thread, or manually from a test).
  - `pqdaw --list-devices` and `pqdaw --sine 440 --seconds 3 [--device N] [--asio]`.
  - Accept: NullDevice tests cover the callback cadence and buffer sizes. `--list-devices` output is pasted into PROGRESS.md. A `--sine` run on the default WASAPI device reports 0 xruns over 3 s (no one is listening, so log the callback stats).

- [x] **T0.7 Window + Vulkan renderer** *(b15670c)* (`ui`). Depends on: T0.1. 👁
  - SDL3 window, Vulkan instance/device/swapchain (VMA), and a draw list with the `{float2 pos, float2 uv, u32 rgba8}` vertex format and **32-bit indices**. Pipelines for alpha, additive and textured drawing. Damage-driven redraw, and `DwmFlush` frame pacing.
  - An **offscreen render path**: `pqdaw --screenshot <scene> <out.png> [--size WxH]` renders a named test scene to a PNG with no window.
  - Accept: the `test-pattern` scene (rects, rounded rects, gradients, additive blobs) matches a committed golden PNG within a per-pixel tolerance. The resize and minimise paths don't crash (scripted test).

- [x] **T0.8 Text** *(376525c)* (`ui`). Depends on: T0.7. 👁
  - FreeType (hinted) + HarfBuzz, a glyph atlas at integer pixel sizes (12/13/14 px plus a large size for meters), Inter (OFL) bundled under `assets/fonts/`.
  - Accept: a `text` screenshot scene matches its golden image. A test checks the shaped advance widths of a known string.

- [x] **T0.9 PQ Meter line primitives** *(18a6a19)* (`ui`). Depends on: T0.7. 👁
  - Port `smooth_line.h` (feathered ribbons: 0.75 px feather, 2.5× miter clamp, bevel past 120°, Catmull-Rom subdivision capped at 6000 points), `gradientPolyline`, `gradientBand`, `splineSmooth`, `gradient.h` and `palette_track.h` from `C:\Users\Luke\Desktop\programming\pq meter\src\ui\` and `src\modules\spectrum.cpp:200-325` into the draw-list API.
  - Accept: a `lines` screenshot scene (sine, noise trace, glow passes) matches its golden image, and the PROGRESS.md note says which PQ Meter files the port came from.

## M0 follow-ups (from the M0 review, docs/reviews/M0.md)
In TASKS.md format, for the orchestrator to schedule. The order is my priority: H-items first, and before T3.1/T3.2 merge where noted.

- [x] **T0.7-fix3 HiDPI content scale** *(dcb5a0a)* (`ui`, `app`). Depends on: T0.7, T0.8. **Before T3.1/T3.2 merge.**
  - `UiScale` from `SDL_GetWindowDisplayScale`, updated on `DISPLAY_SCALE_CHANGED`. Layout units are logical px snapped to device px. Font roles resolve to `round(role·scale)` integer-px hinted fonts. Line widths and feathers stay in device px. Screenshot `--scale`.
  - Accept: `text` and `test-pattern` goldens at scale 1.0/1.25/1.5/2.0. A scale change at runtime re-renders without restart (scripted window test). No blurry glyphs: every glyph quad lands on integer device pixels (test).

- [~] **T0.7-fix1 Partial redraw** *(worker Y, branch task/T0.7-fix1-partial-redraw)* (`ui`, `app`). Depends on: T0.7. Before T3.1's damage test is relied on.
  - Persistent colour target (LOAD); damage rects → scissored re-record of intersecting commands → copy to swapchain image (or `VK_KHR_incremental_present`). Draw-list chunks per damage region so unchanged regions aren't rebuilt.
  - Accept: a 2 px-wide animated playhead over the `lines` scene redraws ≤ 2% of the pixels per frame (GPU timestamp or fragment count). UI-thread CPU per frame < 25% of a full redraw. Golden identical to the full-redraw path after 100 random damage sequences.

- [!] **T0.6-fix1 Own WASAPI backend** *(f85ecda; partial: Focusrite WASAPI exclusive minimum is 144 frames and events land on a ~2 ms grid — see PROGRESS)* (`engine`). Depends on: T0.6.
  - Event-driven `IAudioClient`/`IAudioClient3`: exclusive + shared (low-latency shared periods), float32 or int32/24 exclusive formats, no extra ring, MMCSS "Pro Audio". Underruns detected via `GetCurrentPadding` before each write plus `IAudioClock` position vs frames written. Lateness metric (L2).
  - Accept: exclusive 64- and 128-frame streams open on the Focusrite. 60 s `--sine` with max callback interval ≤ 1.5× period. An injected stall (test flag: sleep 3× period once) is reported as exactly 1 underrun. RtScope/alloc-trap clean. Latency reported in frames and matches a loopback measurement within 1 period. RtAudio stays for ASIO until T0.6-fix2.

- [ ] **T0.6-fix2 ASIO decision + host** (`engine`). Depends on: user answer to Q12. 👁 (decision)
  - Option B: clean-room IASIO COM host (driver enumeration from the registry, `ASIOInit`, buffer switch callback) under our licence; or keep opt-in.
  - Accept: per the decision. If B: Focusrite USB ASIO 64 frames, 60 s, 0 resync requests, RtScope-clean callback.

- [x] **T0.7-fix2 Widget-grade draw-list primitives** *(e30b7d0)* (`ui`). Depends on: T0.7. Before T3.2.
  - `addRectStroke`, `addRoundedRectStroke`, public `addConvexPolyFilled`, `addCircleFilled/Stroke`, `addArcStroke` (knob rings), `addTriangleFilled`, snapped `addHLine/addVLine`. Fix `addRectFilledMultiColor` to be bilinear (centre vertex or N×N), or correct its doc.
  - Accept: `primitives` golden scene. Unit tests: a 1 px stroke at integer coordinates touches exactly one pixel column at full alpha; arc tessellation chord error ≤ 0.2 px; the 4-corner gradient's centre pixel is within 2/255 of the bilinear value. Benchmarks recorded.

- [x] **T0.8-fix1 Non-blocking glyph uploads, R8 glyph pages, stable runs** *(d009bd3)* (`ui`). Depends on: T0.8.
  - Per-frame staging ring; atlas copies recorded in the frame's command buffer (no `vkDeviceWaitIdle`). R8 glyph pages separate from the RGBA sprite page; multi-page with eviction; loud (once) failure when full. Run-cache flush only at frame start (M8). Ellipsis truncation and per-cluster caret x-positions in `TextRun` (needed by T3.2 text edit).
  - Accept: rasterising 200 new glyphs during an animated run causes no frame > 1.5× the median frame time. A test that shapes 20k distinct strings within one frame keeps every earlier run valid. Atlas memory ≤ 1/4 of today's for the same glyph set.

- [x] **T0.7-fix4 Texture update/destroy** *(d009bd3)* (`ui`). Depends on: T0.8-fix1 (shares the staging ring).
  - `updateTexture(id, rect, data)` (async), `destroyTexture(id)` deferred by frame fence, slot reuse.
  - Accept: a 1024×512 spectrogram scrolling one column per frame at 144 Hz with no CPU wait on the GPU. Creating and destroying 10k textures doesn't leak (validation layer clean, VMA stats back to baseline).

- [~] **T0.7-fix5 Tighter golden comparison** *(worker AA, branch task/T0.7-fix5-test-fidelity)* (`tests`). Depends on: —.
  - Outlier magnitude cap and isolation rule (M5). Failure output: diff PNG plus bounding box.
  - Accept: deleting one glyph from the `text` scene, or moving the scope trace by 1 px, fails the test (mutation checks committed as test cases that render a perturbed scene).

- [~] **T0.9-fix1 Differential test against PQ Meter** *(worker AA, branch task/T0.7-fix5-test-fidelity)* (`tests`). Depends on: T0.9.
  - Compile PQ Meter's `smooth_line.h` and the `spectrum.cpp`/`oscilloscope.cpp` helper functions against a minimal `ImDrawList` shim; compare vertex/index buffers with `pq::ui` for fixed inputs.
  - Accept: bit-identical output (or each documented difference, such as the skipped α=0 pass, asserted explicitly).

- [ ] **T0.7-fix6 Startup time** (`app`). Depends on: T0.7.
  - Measure from process creation; log per-phase times; on-disk `VkPipelineCache`; overlap font loading with device creation.
  - Accept: first frame < 300 ms from process creation on an idle machine, median of 10 cold-ish starts, recorded in RESULTS.md.

- [ ] **T0.1-fix1 Build hardening** (`cmake`, docs). Depends on: —.
  - All FetchContent deps via release tarballs + `URL_HASH`. `BUILD_SHARED_LIBS OFF CACHE … FORCE` at the top. Clean up DEPENDENCIES.md. AVX2/FMA startup check with a clear message (L9). Move test scenes out of `pq_ui` (L10).
  - Accept: a fresh configure with `git` removed from PATH succeeds. DEPENDENCIES.md lists every dep exactly once with its hash. Running on a CPU without AVX2 (emulated via a test hook that forces `cpuFeatures().avx2 = false`) prints the message and exits 1.

- [ ] **T0.2-fix1 Queue and benchmark hardening** (`core`, `bench`). Depends on: —.
  - 64-byte multi-word payload stress tests (L5). 128-byte separation for cross-thread hot lines, kept only if pinned benchmarks show a gain (L3). Benchmarks pin threads to separate physical cores and report min/median/p99 over ≥ 5 runs. SPSC round-trip latency benchmark. Fix stale commit refs in RESULTS.md (L4).
  - Accept: RESULTS.md rows re-recorded on an idle machine with the method stated. Mutation check: replacing `acquire` with `relaxed` in `tryPop` is caught by at least the compiler-reordering variant under `/O2` (or documented as undetectable on x86 if it isn't).

- [x] **T0.2-fix2 RT log ring** *(7cb2a43)* (`core`). Depends on: T0.2.
  - `RtLog`: fixed-size POD records (code, args) pushed from RT threads into an SPSC ring, formatted and written by a logger thread (ARCHITECTURE §1 promises it).
  - Accept: RtScope-clean push. 10⁶ records from the NullDevice callback lose none at 4096 capacity when drained at 100 Hz, and report drops exactly when overfilled.

## M1: Engine core

- [x] **T1.1 Patch IR** *(1c5ad8d)* (`graph`). Depends on: T0.2.
  - Nodes, ports (kind/rate/poly/width), wires, params (with attrs), exports, stable `NodeId`s, and layout data. Atom registry: name → signature (inputs, outputs, state size, default rates).
  - Accept: tests construct graphs programmatically and validate structure, including rejecting dangling ports and type-mismatched wires.

- [x] **T1.2 Parser + printer** *(3b54e02)* (`graph`). Depends on: T1.1.
  - Implements `docs/PATCH_LANGUAGE.md` v0 exactly: units, imports, layout, solve blocks. Errors carry file:line:col plus a caret snippet.
  - Accept: a round-trip test (`parse(print(p)) == p`) over every file in `stdlib/` and `tests/graph/cases/`. At least 30 error-case tests with expected messages. A fuzz-ish test runs 10⁴ random token streams with no crashes.

- [x] **T1.3 Inference** *(cc7c8e4)* (`graph`). Depends on: T1.2.
  - Rate, kind, poly and width inference with the upper-bound semantics. Poly→global defaults to sum. Explicit conversions are required.
  - Accept: table-driven tests (inputs → inferred types) for 40+ cases, including modulation wires raising an unmodulated `param` input to `audio`, and poly→global summing.

- [x] **T1.4 Optimiser** *(399395f)* (`graph`). Depends on: T1.3.
  - Constant folding (never `param`), DCE, CSE, algebraic simplification (×1, +0, ×0 when the other side is finite), and hoisting param-rate subexpressions out of audio-rate paths.
  - Accept: tests on before/after IR. A test proves `tan(π·cutoff/sr)` is hoisted to param rate when cutoff is unmodulated, and stays audio-rate when it is modulated.

- [x] **T1.5 Scheduler** *(346999c)* (`graph`). Depends on: T1.4.
  - Topological order, Tarjan SCCs, explicit-delay feedback handled, a zero-delay cycle outside `solve` rejected (text) or given an inserted `z1` plus a warning (visual mode flag). Block regions: per-sample for feedback SCCs, blocks elsewhere. Buffer liveness and reuse. State layout keyed by NodeId with type tags.
  - Accept: tests on schedule structure. The scratch-buffer count for a 100-node chain is ≤ 3. The state layout for an unchanged node is identical across reschedules.

- [x] **T1.6 Leaf atom kernels** *(5a0bdec..4138a25)* (`dsp`). Depends on: T0.4.
  - Every leaf atom in SPEC §7.2: math, logic (bool lanes as masks), memory (`z1`, `delayline` with linear/allpass/cubic reads), generators (`phasor` with a **uint32 phase accumulator**, `sine`, `wavetable` with band-limited mipmaps, `noise`), `shaper`, `sampleRead`. Each has a scalar and an AVX2 path, block-wise.
  - Accept: each kernel is tested against a naive `double` reference. A phasor test shows zero drift over 10⁹ samples, with its frequency exact to the uint32 resolution. Benchmarks are recorded per atom (ns/sample, 1 and 8 lanes).

- [~] **T1.7 Interpreter backend + ProgramImage** *(worker Z, branch task/T1.7-interpreter)* (`graph` + `engine`). Depends on: T1.5, T1.6.
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

- [x] **T2.4 Scala** *(b187c11)* (`tuning`). Depends on: T2.1.
  - `.scl` and `.kbm` parse/write, following the Scala format spec, including the comment, ratio and cents forms and the edge cases.
  - Accept: tests on 20+ real-world `.scl` files (committed fixtures; public-domain sources noted), plus round trips.

- [x] **T2.5 JI utilities** *(b187c11..)* (`tuning`). Depends on: T2.1.
  - Prime-limit checks, Tenney height, octave reduction, harmonic/subharmonic series helpers, the fewest-keypress BFS from the JI trainer (`game-pianoroll.js`), and interval naming tables (colour notation/FJS) ported from `ji composition trainer/interval names.txt` + `data/intervalNames.js`.
  - Accept: tests, including BFS results matching the JS for a fixture set generated under Node.

- [ ] **T2.6 Tuning as signal** (`tuning` + `graph`). Depends on: T2.3, T1.7.
  - The per-region linear form `cents = k·g − m·P + e·P` (SPEC §3.3). A tuning node in the graph whose generator/period/stretch/reference are modulatable. `voice.hz` derives from it. Region crossings are detected at block rate or at the crossing sample.
  - Accept: a generator sweep through several regions produces continuous per-voice pitch (except at intended region changes) that matches `buildMOSScale` evaluated per sample within 1e-4 ¢. A benchmark shows the per-voice pitch cost.

- [ ] **T2.7 Main tunings model** (`document`). Depends on: T2.6.
  - Named project tunings, track/clip assignment and override, per-tuning reference pitch, serialization.
  - Accept: tests; reassigning a track re-resolves all its notes; JSON round trip.

## M3: Arrangement, piano roll, tuning UI

New module (proposed by the M3 plan): `src/views` = `pq_views` (`pq::views`), depending on `ui`, `document`, `tuning`, `core` (never `engine`). Views read the playhead and send transport requests through a plain-data `TransportPort` interface that `app` implements. `app` depends on `views` + `engine`. T3.7 updates ARCHITECTURE.md §1.
Parallel waves: A: T3.1 (`ui`), T3.3 (`core`), T3.4 (`document`), T3.16 (`engine`, once T1.8 is done) · B: T3.2 (`ui`), T3.5, T3.6 (`document`, separate files) · C: T3.7 · D: T3.8→T3.9/T3.10, T3.11→T3.12, T3.13→T3.14 (separate `views/` subtrees) · E: T3.15, T3.17.
Open decisions for M3 are collected in docs/QUESTIONS.md Q15.

- [x] **T3.1 Widget toolkit core** *(63e25e1)* (`ui`). Depends on: T0.7, T0.8.
  - Retained widget tree; stable path-addressable widget ids (e.g. `arrangement/track[3]/header/mute`); per-widget dirty flags feed `Damage` with the widget's rect.
  - Layout: fixed/flex rows and columns, stack/overlay, splitter (draggable, min/max), scroll view with a virtualisation hook (child reports content extent and draws only the visible range). Integer-pixel snapping.
  - Input model independent of SDL: `PointerEvent` (move/down/up/wheel, modifiers, click count), `KeyEvent` (scancode + key + modifiers, repeat), `TextEvent`; an SDL adapter in `ui/window`. Hit testing, hover enter/leave, pointer capture during drags, focus with Tab traversal.
  - Key routing: focused widget → ancestors → scoped action maps → global actions. `ActionRegistry` (string action ids → handlers + enabled predicates); keymap = data (chord → action id per scope); text fields swallow printable keys; modifier-only passthrough rules leave room for macro mode (M8).
  - Timers/animations: a widget can request frames; idle = no requests.
  - `ui::TestDriver`: builds a tree at a given size, injects synthetic events, advances a fake clock, queries widgets by id/rect, renders through `OffscreenTarget` for goldens.
  - Accept: scripted TestDriver tests (click, double-click, drag with capture outside the widget, hover leave on capture release, wheel scroll, Tab order, key routing precedence incl. a text field blocking `B`, inactive-scope chord does nothing). Layout table tests with exact pixel rects for 20+ flex/fixed/splitter cases. Damage test: toggling one widget damages exactly its rect. Idle test: 0 frames drawn in 2 s of fake time. Perf (release): layout of a 10,000-widget tree < 1 ms; hit test < 2 µs; event dispatch allocates nothing.

- [x] **T3.2 Ableton theme tokens, base controls, default keymap** *(277728d)* (`ui`). Depends on: T3.1, T0.9. 👁
  - Theme as data (`assets/themes/ableton-dark.json`, loaded at startup, hot-reloadable in dev): surface/panel/track/clip/selection/grid/text colours, 12/13/14 px font roles, spacing, radii, Ableton's clip colour palette. Signal-display tokens stay PQ Meter's and only signal widgets use them (SPEC §11.1, §15).
  - Controls: label, button, toggle (mute/solo/arm style), Ableton numeric drag box (vertical drag, Shift fine, double-click type-in, Delete = default), fader, knob (with a modulation-range ring hook), single-line text edit, dropdown, nested keyboard-navigable context menu, popover (anchored, click-outside dismiss), scrollbar, tooltip, info-view text provider. PQ Meter-style peak/RMS meter widget fed from a plain value struct (T0.9 primitives).
  - `assets/keymaps/ableton.json`: SPEC §6.1 bindings → action ids. `docs/HOTKEYS.md`: action, keys, scope, Live 12 manual section, status (implemented / later milestone).
  - Accept: `pqdaw --screenshot widgets` (every control in normal/hover/pressed/focused/disabled) matches a committed golden. Scripted tests for drag box (coarse/fine/type-in/reset), fader, knob, text edit (caret, selection, Enter/Esc), context-menu keyboard navigation, popover dismiss. Keymap test: every chord parses, no duplicate chords per scope, every action id resolves to a registered action or a `deferred:<milestone>` marker. Theme file round-trips; a missing token fails loading with a named error.

- [x] **T3.3 Grid, snap and quantize math** *(ec4e097)* (`core`). Depends on: T0.5.
  - `GridSpec { adaptive(level −3..+3) | fixed(1/1…1/64); tuplet straight|3|5|7|11; snap on/off; off }`. Adaptive = finest power-of-two division whose cell is ≥ N px at the current px/tick (Ctrl+1/2 shift N). Tuplet n divides the parent span of `bit_floor(n)` straight cells into n (T0.5 helpers).
  - Grid-line iteration over a tick range with bar/beat/sub-beat strength and time-signature changes; `snap(tick)`, `snapDelta` (Ableton relative snap while dragging); `quantize(start, end, amount%, startOnly|startEnd)`; ruler labels (bars.beats.sixteenths) at any zoom.
  - Accept: table tests for adaptive levels across zooms, fixed values and every tuplet × note value; all 3/5/7/11 grid lines are exact integer ticks (1/64 per Q3); time-signature change mid-range; quantize at 0/50/100% and to tuplet grids; iterating lines over 10 h allocates nothing. Perf: lines for one 4K-wide viewport < 20 µs.

- [x] **T3.4 Document model core: immutable snapshots, commands, undo/redo** *(82dd693 rebased; p99 note-move tail > 20 µs from page faults, see PROGRESS)* (`document`). Depends on: T0.2, T0.5, T2.1, T2.2, T2.3.
  - Creates `src/document` (`pq_doc`, `pq::doc`). `Project`: tempo map, time-signature map, named main tunings (EDO/MOS/Scale slots; signal binding later), locators, loop, master, returns, track tree. `Track`: kind (Midi, Group, Return, Master; Audio reserved for M6), name, colour, tuning ref (main/private), mixer (volume dB, pan, mute, solo, arm, sends, output), children (nestable groups), clips, automation lanes, fold/height. `NoteClip`: start, length, loop, signature, tuning override. `Note`: start, length, `PitchId`, velocity, release velocity, chance, velocity range (expression curves reserved). `AutomationLane`: `ParamRef` + breakpoints {tick, value, curve}. Every entity has a `StableId`.
  - Structural sharing: `shared_ptr<const T>` nodes; clip notes in an immutable chunked sorted vector (≤ 256 notes/chunk) so a one-note edit copies one chunk plus the path to the root.
  - Commands are data (variant of command structs: label, apply → `Result<Snapshot>`). `History`: unlimited undo/redo; gestures (`begin/commit/cancelGesture`) coalesce drags/knob turns into one step; listing for an undo panel. `DocumentStore`: current snapshot, dispatch, listeners. Each commit yields a `ChangeSet` (touched ids + `structural`/`sequenceOnly`/`paramOnly`) that drives lowering (T3.17).
  - Accept: apply/undo/redo restores the identical snapshot pointer; gesture coalescing; a cancelled gesture leaves no history; ChangeSet classification correct for 30+ command kinds; unchanged subtrees pointer-equal across commits. Perf (release, 100 tracks / 10,000 clips / 1,000,000 notes): move one note < 20 µs and < 16 KB added; undo/redo < 1 µs; 10,000 one-note edits keep total memory < 200 MB.

- [x] **T3.5 Project serialization** *(c1afeb8)* (`document`). Depends on: T3.4, T2.4. Also add `Scale` to `TuningDef` (T3.4 left it out because T2.4 was not yet merged).
  - Project directory with `project.pqd` (JSON, UTF-8, `"schemaVersion": 1`). Exact numbers: ticks as integers, rationals as `"n/d"`, cents as shortest round-trip doubles. Embedded tuning definitions (EDO/MOS params, Scala contents); ids preserved. Migration hook table (n → n+1) with a test hook. Errors with JSON path. Atomic save (temp + rename). nlohmann/json becomes a `pq_doc` dependency (DEPENDENCIES.md).
  - Accept: `load(save(p)) == p` for generated projects with every entity kind and every PitchId kind; committed fixture projects in `tests/document/fixtures/` load; 25+ malformed-file tests with precise errors; 10⁴ mutated files with no crash. Perf: 100k notes save < 150 ms, load < 250 ms.

- [x] **T3.6 Edit operations library** *(9937c76)* (`document`). Depends on: T3.4, T3.3.
  - Pure command builders so views stay thin. Tracks: create MIDI/Group/Return, delete, rename, recolour, reorder, group selection (Ctrl+G, nested, across parents), ungroup, fold, activate/deactivate (`0`). Clips: insert empty (Ctrl+Shift+M), move/copy across tracks, resize start/end (notes keep absolute time), split (Ctrl+E), duplicate (Ctrl+D), delete, consolidate (Ctrl+J), loop to selection (Ctrl+L). Notes: add, delete, move, resize, velocity/chance edits, quantize (Ctrl+U), duplicate. Identity-aware Up/Down: Degree ±1 display-grid step; Ratio/Harmonic → next harmonic `(n±1)/d`; Cents → ±1 display-grid step; Shift = ±1 equave. Time selection: insert time (Ctrl+I), delete time, duplicate time (Ctrl+Shift+D). Automation: add/move/delete breakpoints, segment curve, insert shape over a range; optional "lock envelopes" follow.
  - Accept: table tests on before/after snapshots + one-step undo for each operation; microtonal moves on 31edo, 5L2s, a Scala scale, and Ratio 5/4 → 6/4; split/consolidate preserve absolute time and identity; insert/delete time shifts clips, notes, automation, locators and loop consistently; every operation is a single history entry.

- [~] **T3.7 Views module, app shell and project I/O** *(worker AB, branch task/T3.7-app-shell)* (`views`, `app`). Depends on: T3.2, T3.5. 👁
  - Create `pq_views` (ARCHITECTURE §1 update). Ableton main layout: control bar (tempo drag box, time signature, transport, position, loop, grid display), track-area host, detail view (Shift+Tab clip/device; device panel empty until M4), info view, resizable splitters.
  - `TransportPort` (play, stop, continue, locate, loop, playhead snapshot struct) with an app-side fake for tests. Global actions: Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y, Ctrl+N/O/S/Shift+S, Space / Shift+Space.
  - `--screenshot <scene> --project <dir>` renders views scenes from fixture projects; `pqdaw --project <dir>` opens a window; idle loop does not redraw.
  - Accept: `--screenshot shell --project tests/views/fixtures/empty` matches its golden; Ctrl+S then reopen gives an identical document; undo/redo hotkeys go through `History`; Space calls the port; Shift+Tab toggles the detail view; idle window draws 0 frames in 10 s at < 0.1% CPU; first frame with an empty project < 300 ms (release, logged).

- [ ] **T3.8 Arrangement view: display and navigation** (`views`). Depends on: T3.7, T3.3. 👁
  - Ruler (bars/beats; time-signature and tempo markers), locators, loop brace, playhead (reads `TransportPort` only while playing). Track lanes with headers (name, colour, fold), group indentation. Note clips as Ableton clip bodies with a cached note preview rebuilt only when the clip snapshot pointer changes. Adaptive grid shading; scroll; Ctrl+wheel and ruler-drag zoom; Z / X; follow mode; overview strip. Virtualisation: per-track sorted clips + binary search; only visible tracks/clips emit geometry.
  - Accept: goldens `arrangement-basic` (8 tracks incl. a nested and a folded group, loop, locators) and `arrangement-zoomed-out`. Scripted: Ctrl+wheel zoom keeps the tick under the cursor fixed (±1 px); Z/X round-trip; ruler click locates via the port. Perf (release, 2560×1440, 64 tracks × 200 clips × 50 notes): full redraw < 2 ms CPU; scroll/zoom frame < 2 ms; draw-list memory flat over 1000 scroll frames.

- [ ] **T3.9 Arrangement editing** (`views`). Depends on: T3.8, T3.6. 👁
  - Selection (clip select, Shift/Ctrl add, rubber band, time selection, Ctrl+A). Clip drag-move with relative snap, Alt+drag copy, edge resize, cross-track drag, Ctrl+Shift+M, Ctrl+E, Ctrl+D, Ctrl+J, Delete, Ctrl+L. Clipboard Ctrl+C/X/V (in-process document fragments). Ctrl+I, Ctrl+Shift+D, delete time. Right-click grid menu (Adaptive Narrowest…Widest / Fixed / Straight, Triplet, 5-, 7-, 11-tuplet / Snap / Off) and Ctrl+1–5. Double-click opens the clip in the detail view.
  - Accept: scripted tests asserting the resulting document plus exactly one undo step: drag-move with snap on a 7-tuplet grid lands on exact ticks; Alt-drag copy; resize; split; duplicate; consolidate; Ctrl+Shift+D; every grid menu item and Ctrl+1–5. A 60-frame drag = one undo entry. Golden `arrangement-selection` (clip + time selection, drag ghost).

- [ ] **T3.10 Track headers, groups and mixer section** (`views`). Depends on: T3.8, T3.6. 👁
  - Header mixer section: volume, pan, mute (activator), solo, arm, sends per return, output routing dropdown, tuning chip coloured per main tuning. Ctrl+T, Ctrl+R, Ctrl+G, ungroup, colour menu, height resize, fold. Returns and master at the bottom. PQ Meter-style meters fed by a plain-data `MeterSource`. Ableton solo semantics (exclusive; Ctrl-click additive; group solo solos members; member solo keeps parents audible) as a pure function `effectiveAudible(track)` shared with T3.17. Basic mixer view (Ctrl+Alt+M) from the same strips. Knob/fader drags coalesce to one undo step.
  - Accept: goldens `tracks-mixer`, `mixer-view`; 20+ table tests for `effectiveAudible`; scripted Ctrl+T inserts after the selection, Ctrl+G across nested groups, rename via Ctrl+R + Enter, a volume drag is one undo step with a `paramOnly` ChangeSet.

- [ ] **T3.11 Piano roll: EDO mode and note editing** (`views`). Depends on: T3.7, T3.6, T3.3, T2.2. 👁
  - Clip view: gutter keys, ruler, loop/start/end braces, note area, velocity lane (+ chance toggle). EDO mode: one row per step; equave markers labelled per Q2 (C-style names for 12edo-compatible EDOs); scale highlighting. Notes: draw mode B, select, rubber band, move, resize, Alt-drag copy, delete, velocity drag, Ctrl+U. Up/Down and Shift+Up/Down via T3.6. Ratio/Cents notes drawn at true pitch and marked off-grid. Fold to used notes/scale; vertical zoom/scroll; grid shared with the arrangement; audition via `TransportPort::auditionHz`. Virtualised by visible time range.
  - Accept: goldens `pianoroll-12edo`, `pianoroll-31edo`, `pianoroll-velocity`. Scripted: B-mode draw on a 5-tuplet grid lands on exact ticks; move/resize/copy/delete; Up on a Degree note in 31edo = +1\31; Up on 5/4 → 6/4; Shift+Up = one equave; each gesture one undo step. Perf: 100k-note clip with 3000 visible, full redraw < 1.5 ms CPU, drag frame < 1 ms.

- [ ] **T3.12 Piano roll: scale/MOS layouts and live morph** (`views`). Depends on: T3.11, T2.3, T2.4. 👁
  - One row per degree; Proportional (default) and Equidistant layouts ported from `mos-roll.js` `layoutRows` (~747–860): asymmetric half-height rule, s-rows shrink to slivers as the MOS collapses, s-notes under L-notes; layout toggle keeps the centred row. Gutter keys coloured by step class with the hardness-based L/s blend. Scala scales use the same row model. Background EDO overlay for display and snapping. Live morph: generator changes relayout rows and Degree notes every frame without committing until gesture end (one undo step), using `MosLinearTable` within a region and `buildMOSScale` + `transferModeRotation` at region crossings.
  - Accept: goldens `pianoroll-5L2s-prop`, `pianoroll-5L2s-equi`, `pianoroll-5L2s-near-collapse`, `pianoroll-5L2s-over-31edo`, `pianoroll-scala`. Row layout matches `mos-roll.js` run under Node for 50 (generator, size, height) cases (±0.5 px; script in `tools/fixtures/`). Scripted morph across a region boundary keeps Degree notes on their rows, one undo step. Perf: morph frame (24-degree MOS, 10 equaves, 3000 notes) < 2 ms CPU.

- [ ] **T3.13 MOS wheel: region chart** (`views`). Depends on: T3.7, T2.3. 👁
  - Port of mos-explorer `drawAxis`/`drawMOS`/`drawEDOs`/`drawTemperaments`/`drawGeneratorMarker`/`drawSizeScrollbar` (index.html ~3156–3690): generator cents × MOS size, Stern–Brocot bands from `buildMosRegions`, EDO markers up to a configurable max EDO, temperament ranges, generator marker drag with EDO snapping, size scrollbar, hover info (name, UDP, hardness). Emits a `MosSelection` value (no document dependency in the widget).
  - Accept: goldens `mos-chart-default` (5L2s, max EDO 31), `mos-chart-zoomed`; clicking inside a region selects its MOS (checked against `buildMosRegions` for 100 random points); marker near 7\12 snaps exactly to 700 ¢. Perf: chart build at 800×600 < 1 ms CPU.

- [ ] **T3.14 MOS wheel: circle view and popover** (`views`). Depends on: T3.13, T3.6. 👁
  - Port of `drawMOSCircle` (index.html ~3693+): step slices, degree spokes with cents, tonic, JI markers, hardness slider (via `generatorForHardness`), mode rotation by step / by generator, parent/child arrows, MODMOS alterations. Popover linking chart + circle; also EDO entry, Scala file pick, temperament presets. Commits a tuning command to a clip / track / main tuning (one undo step); opened from tuning chips (+ hotkey, Q15). The roll previews live while dragging.
  - Accept: goldens `mos-circle-5L2s`, `mos-circle-modmos`, `mos-wheel-popover`; rotate-by-generator matches `rotateByGenerator`; hardness 2.0 gives L/s = 2 within 1e-6; parent/child arrows match `findParentMOS`/`getChildMOSes`; committing to a clip changes only that clip (one undo step); Esc cancels with no history entry.

- [ ] **T3.15 Automation lanes** (`views`). Depends on: T3.9, T3.10, T3.6. 👁
  - Per-track lanes (show/hide; parameter chooser incl. main-tuning params); `A` automation mode over clips; breakpoint add/move (snapped, value readout)/delete/multi-select; curved segments (Alt-drag) with the curve law shared with T3.16; shapes menu (ramp, sine, square, triangle, saw over the time selection); automated-state indication and "Re-enable automation" (state only). Curves drawn as feathered ribbons (T0.9).
  - Accept: goldens `automation-lanes`, `automation-mode-over-clips`; scripted add/move/delete, Alt-drag curvature, shape insertion; each gesture one undo step with a non-structural ChangeSet; view curve evaluation matches `engine::evalAutomation` within 1e-6 at 10⁴ points.

- [ ] **T3.16 Engine timeline: transport, note sequencer, automation renderer** (`engine`). Depends on: T1.8, T0.5.
  - Audio-thread transport (play/stop/continue/locate, sample-accurate loop wrap, tempo map) exposed in `ProcessContext` and reported to the UI (lossy SPSC). `SequenceImage`: immutable flat per-track note array built off-thread from a plain-data description (no `document` dependency); clip loops unrolled lazily by a cursor; published by atomic swap + EpochReclaimer; cursor survives swaps. Emits note-on/off events at exact sample offsets; chance/velocity range via a deterministic per-note RNG (note id + loop pass). `AutomationImage`: breakpoints rendered per sample into audio-rate buffers for automated params (incremental cursor); unautomated params stay param-rate. `evalAutomation` header-only, shared with views.
  - Accept: a note at tick t starts at exactly `ticksToSamples(t)` for block sizes 1/32/37/480; loop wrap emits offs/ons at the exact wrap sample; locate during playback releases all sounding voices; swapping the image mid-note keeps unchanged notes and stops removed ones at the next block; automation buffers match `evalAutomation` within 1e-6; RtScope clean with 64 tracks × 200 clips. Perf: per block with no events < 50 ns per track; 100k-note SequenceImage build < 20 ms.

- [ ] **T3.17 Project lowering and playback link** (`document`, `app`). Depends on: T3.16, T3.4, T3.7, T3.10, T1.9, T1.10 (T2.6 for tuning automation).
  - `doc::lower(Snapshot)` → graph Patch + param map + sequence/automation descriptions: per MIDI track an instrument (M3 default instrument, Q15) → volume/pan → send taps → parent group sum → master; returns; mute/solo via `effectiveAudible`; param slots keyed by stable `ParamRef`. App controller per ChangeSet: `structural` → debounced recompile + T1.9 hot swap; `sequenceOnly` → publish a new SequenceImage; `paramOnly` → `EngineCommand` param writes. `TransportPort` over the engine; playhead and meters fed back; audition on click. `pqdaw --project <dir> --render out.wav [--seconds N]`.
  - Accept: golden offline renders (T1.10 helper) of a fixture with 2 MIDI tracks in 31edo and 5L2s, a group with volume automation, a return with a send, a muted track. Compile-count hooks: moving a note or changing velocity → 0 recompiles, effective at the next block; volume change → 0 recompiles; adding a track → exactly 1 recompile, click-free swap (T1.9 criteria). Solo/mute renders match `effectiveAudible`. Idle stopped app ≈ 0% CPU. End-to-end with NullDevice: open fixture, Space, after 1 s of blocks the playhead reported to views is within one block of 1 s.

## M4 onward (outline: refine into tasks before starting; most need user review)
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
