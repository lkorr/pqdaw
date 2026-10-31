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

- [x] **T0.7-fix1 Partial redraw** *(partial redraw; see PROGRESS)* (`ui`, `app`). Depends on: T0.7. Before T3.1's damage test is relied on.
  - Persistent colour target (LOAD); damage rects → scissored re-record of intersecting commands → copy to swapchain image (or `VK_KHR_incremental_present`). Draw-list chunks per damage region so unchanged regions aren't rebuilt.
  - Accept: a 2 px-wide animated playhead over the `lines` scene redraws ≤ 2% of the pixels per frame (GPU timestamp or fragment count). UI-thread CPU per frame < 25% of a full redraw. Golden identical to the full-redraw path after 100 random damage sequences.

- [!] **T0.6-fix1 Own WASAPI backend** *(f85ecda; partial: Focusrite WASAPI exclusive minimum is 144 frames and events land on a ~2 ms grid — see PROGRESS)* (`engine`). Depends on: T0.6.
  - Event-driven `IAudioClient`/`IAudioClient3`: exclusive + shared (low-latency shared periods), float32 or int32/24 exclusive formats, no extra ring, MMCSS "Pro Audio". Underruns detected via `GetCurrentPadding` before each write plus `IAudioClock` position vs frames written. Lateness metric (L2).
  - Accept: exclusive 64- and 128-frame streams open on the Focusrite. 60 s `--sine` with max callback interval ≤ 1.5× period. An injected stall (test flag: sleep 3× period once) is reported as exactly 1 underrun. RtScope/alloc-trap clean. Latency reported in frames and matches a loopback measurement within 1 period. RtAudio stays for ASIO until T0.6-fix2.

- [!] **T0.6-fix2 Own ASIO host** *(a17c9a1; works on real drivers; 2 ms driver callback grid + manual reset check pending — see PROGRESS, REVIEW)* (`engine`). Depends on: T0.6-fix1. (User decision Q12: ASIO is required.)
  - Clean-room IASIO COM host under our licence (no Steinberg SDK files): driver enumeration from the registry (HKLM\SOFTWARE\ASIO), CoCreateInstance, init, channel/format queries (Int32LSB, Int24LSB, Float32LSB, Int16LSB at least), createBuffers + bufferSwitch/bufferSwitchTimeInfo callbacks, sample-rate set/query, latency query, reset/resync messages, control panel launch. Same `AudioDevice` interface and CallbackRunner as the WASAPI backend; replaces RtAudio's ASIO (RtAudio can be dropped if nothing else needs it — measure).
  - Accept: Focusrite USB ASIO at 64 frames / 48 kHz: 60 s `--sine` with 0 resync/xrun and callback interval ≤ 1.5× period; ASIO4ALL and Voicemeeter ASIO open and play; device change / sample-rate change / reset request from the driver handled without crash; RtScope-clean callback; latency reported (input/output) from the driver.
- [x] **T0.7-fix2 Widget-grade draw-list primitives** *(e30b7d0)* (`ui`). Depends on: T0.7. Before T3.2.
  - `addRectStroke`, `addRoundedRectStroke`, public `addConvexPolyFilled`, `addCircleFilled/Stroke`, `addArcStroke` (knob rings), `addTriangleFilled`, snapped `addHLine/addVLine`. Fix `addRectFilledMultiColor` to be bilinear (centre vertex or N×N), or correct its doc.
  - Accept: `primitives` golden scene. Unit tests: a 1 px stroke at integer coordinates touches exactly one pixel column at full alpha; arc tessellation chord error ≤ 0.2 px; the 4-corner gradient's centre pixel is within 2/255 of the bilinear value. Benchmarks recorded.

- [x] **T0.8-fix1 Non-blocking glyph uploads, R8 glyph pages, stable runs** *(d009bd3)* (`ui`). Depends on: T0.8.
  - Per-frame staging ring; atlas copies recorded in the frame's command buffer (no `vkDeviceWaitIdle`). R8 glyph pages separate from the RGBA sprite page; multi-page with eviction; loud (once) failure when full. Run-cache flush only at frame start (M8). Ellipsis truncation and per-cluster caret x-positions in `TextRun` (needed by T3.2 text edit).
  - Accept: rasterising 200 new glyphs during an animated run causes no frame > 1.5× the median frame time. A test that shapes 20k distinct strings within one frame keeps every earlier run valid. Atlas memory ≤ 1/4 of today's for the same glyph set.

- [x] **T0.7-fix4 Texture update/destroy** *(d009bd3)* (`ui`). Depends on: T0.8-fix1 (shares the staging ring).
  - `updateTexture(id, rect, data)` (async), `destroyTexture(id)` deferred by frame fence, slot reuse.
  - Accept: a 1024×512 spectrogram scrolling one column per frame at 144 Hz with no CPU wait on the GPU. Creating and destroying 10k textures doesn't leak (validation layer clean, VMA stats back to baseline).

- [x] **T0.7-fix5 Tighter golden comparison** *(6e2e49c)* (`tests`). Depends on: —.
  - Outlier magnitude cap and isolation rule (M5). Failure output: diff PNG plus bounding box.
  - Accept: deleting one glyph from the `text` scene, or moving the scope trace by 1 px, fails the test (mutation checks committed as test cases that render a perturbed scene).

- [x] **T0.9-fix1 Differential test against PQ Meter** *(59c34ae)* (`tests`). Depends on: T0.9.
  - Compile PQ Meter's `smooth_line.h` and the `spectrum.cpp`/`oscilloscope.cpp` helper functions against a minimal `ImDrawList` shim; compare vertex/index buffers with `pq::ui` for fixed inputs.
  - Accept: bit-identical output (or each documented difference, such as the skipped α=0 pass, asserted explicitly).

- [x] **T0.7-fix6 Startup time** *(merged 2026-10-24; < 300 ms met under load, idle re-measure pending)* (`app`). Depends on: T0.7.
  - Measure from process creation; log per-phase times; on-disk `VkPipelineCache`; overlap font loading with device creation.
  - Accept: first frame < 300 ms from process creation on an idle machine, median of 10 cold-ish starts, recorded in RESULTS.md.

- [x] **T0.1-fix1 Build hardening** *(see PROGRESS)* (`cmake`, docs). Depends on: —.
  - All FetchContent deps via release tarballs + `URL_HASH`. `BUILD_SHARED_LIBS OFF CACHE … FORCE` at the top. Clean up DEPENDENCIES.md. AVX2/FMA startup check with a clear message (L9). Move test scenes out of `pq_ui` (L10).
  - Accept: a fresh configure with `git` removed from PATH succeeds. DEPENDENCIES.md lists every dep exactly once with its hash. Running on a CPU without AVX2 (emulated via a test hook that forces `cpuFeatures().avx2 = false`) prints the message and exits 1.

- [x] **T0.2-fix1 Queue and benchmark hardening** *(7a3b10f; idle re-measurement pending)* (`core`, `bench`). Depends on: —.
  - 64-byte multi-word payload stress tests (L5). 128-byte separation for cross-thread hot lines, kept only if pinned benchmarks show a gain (L3). Benchmarks pin threads to separate physical cores and report min/median/p99 over ≥ 5 runs. SPSC round-trip latency benchmark. Fix stale commit refs in RESULTS.md (L4).
  - Accept: RESULTS.md rows re-recorded on an idle machine with the method stated. Mutation check: replacing `acquire` with `relaxed` in `tryPop` is caught by at least the compiler-reordering variant under `/O2` (or documented as undetectable on x86 if it isn't).

- [x] **T0.2-fix2 RT log ring** *(7cb2a43)* (`core`). Depends on: T0.2.
  - `RtLog`: fixed-size POD records (code, args) pushed from RT threads into an SPSC ring, formatted and written by a logger thread (ARCHITECTURE §1 promises it).
  - Accept: RtScope-clean push. 10⁶ records from the NullDevice callback lose none at 4096 capacity when drained at 100 Hz, and report drops exactly when overfilled.

- [x] **T0.x-flake Timing tests robust under load** (`tests`, `ui`, `engine`). Depends on: —. *(13418b6; driver-thread crash still open)*
  - Several timing/perf assertions fail intermittently while other builds run (seen: `pq_tests_ui` test_render.cpp "stalls == 0", `pq_tests_engine` test_null_device.cpp:281 callback within 50 ms, `pqdaw_window_selftest_partial` CPU ratio < 25 %, glyph-upload frame-interval check). `main` must always pass.
  - Accept: each timing assertion either measures something load-independent (thread CPU cycles, counts, deterministic fake clocks) or moves to an opt-in perf label (`ctest -L perf`) that the standard build doesn't run; the functional part of each test stays in the default suite. 10 consecutive full release `ctest` runs pass while a parallel `cmake --build` of the other preset runs.

- [x] **T0.5-fix1 PPQ 221,760** *(merged 2026-10-24)* (`core`, all users). Depends on: T0.5. (User decision Q3.)
  - Change PPQ from 27,720 to 221,760 (2⁶·3²·5·7·11) everywhere: core time.h constant, grid (T3.3: straight values down to 1/256, x/64 signatures legal), document/serialization (schemaVersion bump + migration multiplying stored ticks by 8; existing fixtures migrated), tests/goldens/benchmarks that hard-code tick values.
  - Accept: all tests pass with the new constant; every straight note value 1/1…1/256 and every 3/5/7/9/11-tuplet of them is an exact integer tick count; a schema-1 project loads, migrates and re-saves as schema 2 with all positions × 8; the 10 h @ 192 kHz tempo round-trip test still holds within ½ sample.

- [x] **T0.x-buildspeed Faster builds and test loop** (`cmake`, `scripts`, `tests`). Depends on: —. *(merged; release-build criterion resolved by T0.x-buildspeed-fix1: release no longer LTCG, ship preset keeps it — Q91)*
  - Compiler cache: sccache (Apache-2.0, pinned release, SHA-256 checked, fetched by a script into a tools dir outside the repo) used as the compiler launcher when present; dev debug info switched to /Z7 so objects are cacheable; one shared cache directory for all worktrees.
  - Shared third-party downloads/builds across worktrees (shared FetchContent download dir at minimum; safe under concurrent configures).
  - Test tiers: heavy sweeps (10⁹-sample drift, 10⁴ fuzz runs, full mutation/reference sweeps, long GPU sweeps) behind a `slow` ctest label (doctest test-suite tags), excluded by default in `scripts/build.ps1`; `-Full` runs everything (used before merges to main).
  - Accept: a second fresh worktree's release build is ≥ 3× faster than the first (measured, cache hits reported); default `build.ps1` test time reduced by ≥ 2× with no test deleted (every slow test still runs under `-Full`); full suite still green.

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

- [x] **T1.5 Scheduler** *(346999c; M1-review defects fixed in T1.5-fix1)* (`graph`). Depends on: T1.4.
  - Topological order, Tarjan SCCs, explicit-delay feedback handled, a zero-delay cycle outside `solve` rejected (text) or given an inserted `z1` plus a warning (visual mode flag). Block regions: per-sample for feedback SCCs, blocks elsewhere. Buffer liveness and reuse. State layout keyed by NodeId with type tags.
  - Accept: tests on schedule structure. The scratch-buffer count for a 100-node chain is ≤ 3. The state layout for an unchanged node is identical across reschedules.

- [x] **T1.6 Leaf atom kernels** *(5a0bdec..4138a25)* (`dsp`). Depends on: T0.4.
  - Every leaf atom in SPEC §7.2: math, logic (bool lanes as masks), memory (`z1`, `delayline` with linear/allpass/cubic reads), generators (`phasor` with a **uint32 phase accumulator**, `sine`, `wavetable` with band-limited mipmaps, `noise`), `shaper`, `sampleRead`. Each has a scalar and an AVX2 path, block-wise.
  - Accept: each kernel is tested against a naive `double` reference. A phasor test shows zero drift over 10⁹ samples, with its frequency exact to the uint32 resolution. Benchmarks are recorded per atom (ns/sample, 1 and 8 lanes).

- [x] **T1.7 Interpreter backend + ProgramImage** *(00fe5e5)* (`graph` + `engine`). Depends on: T1.5, T1.6.
  - Flat op list, parameter block, `ProcessContext`, and the state arena.
  - Accept: the interpreted output of the patches in `tests/graph/cases/` matches a slow reference evaluator (sample-by-sample graph walk in `double`) within 1e-5. RtScope tests show process() never allocates.

- [x] **T1.8 Voices** *(039fdea; M1-review defects fixed in T1.8-fix1)* (`engine`). Depends on: T1.7.
  - Voice allocator (max voices, stealing policy), the event list, voice lanes (8 per AVX2 group), the `voice.*` namespace, voice end on silence/envelope.
  - Accept: tests cover allocation and stealing, note on/off sample accuracy (an event at sample k takes effect at k), and 64 voices with no allocation.

- [x] **T1.9 Hot swap** *(2938291)* (`engine`). Depends on: T1.8.
  - Publishing a ProgramImage from a worker thread. Swap at a block boundary. State transfer by NodeId+type tag. Crossfade (5–20 ms) when audible, instant when silent. Epoch reclamation of old images.
  - Accept: an offline test swaps a modified patch mid-render. Phase continuity of an unchanged oscillator is exact. There is no discontinuity above −90 dBFS at the swap point for an unchanged signal path. RtScope clean.

- [x] **T1.10 Offline renderer + golden tests + `pqc` CLI** *(ddc6417)* (`engine`, `tools`). Depends on: T1.7.
  - `pqc check|dump-ir|dump-schedule|render <file.pqp> [--notes "…"] [--seconds N] -o out.wav`. A golden-audio comparison helper (absolute + spectral tolerance).
  - Accept: golden renders committed for 5+ patches. `pqc` usage is documented in `tools/README.md`.

- [x] **T1.11 Benchmark harness** *(ddc6417)* (`bench`). Depends on: T1.7.
  - A Google Benchmark-style runner (or a hand-rolled one with warmup/repeats/median), with CSV output and an append-to-`bench/RESULTS.md` script.
  - Accept: benchmarks exist for SPSC, the atoms, and an interpreted 32-node patch at 1/8/64 voices.

## M1 follow-ups (from the M1 review, docs/reviews/M1.md)
```
- [x] **T1.5-fix1 Scheduler buffer liveness: solve reads, voice outputs, multichannel delay loops** *(4c085d3)* (`graph`). Depends on: T1.5.
  - Pin every SolveEquation lhs/rhs and every unknown value (pin test `v.def <= b.firstOp`); pin every voice OutputRef/export
    value; build the delay-loop read list once per node (schedule.cpp:1482). checkStructure asserts op.numIn == kernel inputs.
    Delay-loop sub-block bound computed in float32 like the runtime (M10).
  - Accept: docs/reviews/M1/probes solve_inplace (y = √2 ± 1e-6), voice_out (o1 peak 1.0), stereo_fb (JIT compiles, matches
    interp bit for bit), delay_lb (block 1/64/1024 bit-identical) pass on both backends. A schedule-replay aliasing checker
    (2 voice groups, 2 sub-block iterations, 3 Newton evaluations, after-solve and end-of-stage reads) runs over the corpus and
    500 random patches WITH voices, multiple outputs, multichannel delays and solve blocks; reverting any one fix fails it.

- [x] **T1.8-fix1 Voice semantics: NaN-safe mix, retrigger, unison, voice_last, overflow** *(653a37b)* (`engine`). Depends on: T1.8.
  - Mix and monitor with a lane-active select (no gain multiply on idle lanes); free lanes read a finite hz (referenceHz).
    Retrigger only Released voices not started this block, with ≥ 1 low gate frame (or an explicit producer flag); never
    merge two held notes. voice_last re-points to the newest live lane when its lane frees. Pitch/NoteOff lookup prefers
    Held, then newest. EventBuffer/maxEventsPerBlock overflow drops the newest NoteOn, never an off; oversize blocks still
    allocate. Ring moves/resets budgeted (lazy reset or ≤ one ring-heavy lane per block). Raised-cosine steal fade.
  - Accept: idle_lane_nan.pqp and nan_lane.pqp render finite on both backends; "60@0:0.5 60@0.5:0.5" re-attacks (peak ≥
    0.69 after 0.5 s); "60@0:1 60@0:1" = 2 lanes, 2× the single-note level; last.pqp holds 220 Hz after 0.42 s; overflow test
    leaves 0 stuck notes; a per-voice 2 s delay at 64 voices with note-on/compaction traffic stays under 25 % of the block
    period on the audio thread (bench row).

- [x] **T1.6-fix1 Kernel edge cases** *(8f27140)* (`dsp`). Depends on: T1.6.
  - stateBytes in size_t + scheduler cap on ring bytes (error); sample_read tiny-negative rate and modulo loop wrap; timer NaN
    clamp (Q55); mod never returns b; NaN hz → 0 Hz; wavetable prevPhase at reset; fast_sin range reduction (or documented
    clamp) and remove it from the optimiser's finite facts; wavetable crossfade between adjacent mip levels.
  - Accept: dsp_probes.pqp DelayBig is a compile error (no crash); SrTiny keeps playing; LoopFast loops at rate 2/64 on a
    1-sample loop; TimerNan outputs 0; the saw wavetable's top harmonic stays ≥ 16 kHz through every octave boundary (spectral
    test); tests use independent references (no copied level/wrap rules).

- [x] **T1b.1-fix1 JIT determinism, Newton, compile time** *(merged 2026-10-24; zdf_tanh 1.5× accepted by the user, Q98)* (`jit` + `engine`). Depends on: T1b.1.
  - contract off by default (Q64 reversed; optional optimiser `mulsub` fusion); Newton step relative with a 1e-6 floor and a
    fallback on non-finite J (both backends); forward-mode AD for inlined solve bodies (or correct Q53/RESULTS); interleave
    capped by code size; CodeGenOptLevel measured; per-call-site scratch arrays (no __chkstk frames); canonical bool masks.
  - Accept: chaos.pqp JIT == interp bit for bit; solve_scale.pqp converges to 0.002; zdf_tanh ≥ 2× faster with AD (or the
    claim removed); mux1200 compiles ≤ 300 ms; a compile-time test asserts 100-node ≤ 100 ms (release, idle); chaos/edge
    probes added to the JIT corpus.

- [x] **T1.x-arch1 Per-track compilation units** *(6b1e487)* (`graph` + `engine` + `jit` + docs). Depends on: T1.5-fix1, T1.8-fix1, T1.9, T1b.1. Before T3.17. (User decision 2026-10-21: per-track units now; whole-project re-optimisation on idle later, M12.)
  - Each track's device chain is one compile unit (own Schedule/ProgramImage/state arena/compile-cache key); a project-level runner executes units in dependency order each block, passing audio between units through preallocated buffers (no added latency for feed-forward paths); tracks connected by cross-track modulation/sidechain wires are merged into one unit; group/return/master summing as units too. Hot swap (T1.9) per unit: an edit recompiles and crossfades only the changed unit.
  - Accept: a 30-track fixture (≈ 20k ops) — one device edit: audible change ≤ 200 ms, peak audio-thread CPU during the edit ≤ 1.3× steady state, unchanged tracks bit-identical through the swap; output of the per-unit project identical (≤ 1e-6) to a single whole-project program of the same graph; cross-track modulation wire merges units and stays sample-accurate.
- [x] **T1.x-arch2 Shared image host + one compile pipeline** *(merged 2026-10-24)* (`engine`, `jit`, `tools`, `app`). Depends on: T1b.1.
  - Extract interpreter/JIT host code (layout, params, inputs, reducers/sends/broadcast, Newton) into one engine-internal
    layer; `pq_compile` library for file → Schedule used by pqc and `pqdaw --play` (Q73 B, gives --play the JIT); renderOffline
    through ProgramHost.
  - Accept: jit_image.cpp/runtime.cpp shrink to codegen-specific code; all interpreter, JIT, voice, hot-swap and golden tests
    pass unchanged; `pqdaw --play --backend jit` works.

- [x] **T1.x-silence Silence and sleep (SPEC §7.7)** *(merged 2026-10-26)* (`graph` + `engine` + `jit`). Depends on: T1.x-arch1.
  - Per-node tail declarations; per-region silent-input detection at block boundaries; constant-output kernels while asleep;
    ABI field reporting output silence; voice end via the same mechanism.
  - Accept: an idle 30-track project with reverbs/delays costs ≤ 1 % of a core after tails end; waking is sample-accurate
    (golden); interpreter and JIT identical.

- [x] **T1.4-fix1 Optimiser: select hoisting, bit-exact folding, units** *(merged 2026-10-24)* (`graph`). Depends on: T1.4.
  - Sink pure ops below select/mux when it lowers the rate; fold with the 8-lane kernel (lane 0); `smp` rejected in params and
    arithmetic reaching seconds inputs; units in the CSE key.
  - Accept: FmVoice's adsr has no audio-rate exp after optimise (IR test) and the JIT fm_voice bench improves (RESULTS row);
    folded == unfolded bit for bit for every exact transcendental over 10^4 arguments; smp_param.pqp is an error.

- [x] **T1.x-text-ids Keep state keys across text edits (Q68)** *(merged 2026-10-24; callers to pass ParseOptions::previous)* (`graph`). Depends on: T1.2. Before M4.
  - Structural diff mapping re-parsed nodes to previous NodeIds before scheduling.
  - Accept: adding an `export`, renaming a binding, or inserting a line in a .pqp keeps ≥ 99 % of state entries matched in
    the transfer plan; hot swap after a text edit is click-free on an unchanged path.

- [~] **T1.11-fix1 Benchmark hygiene** *(tooling merged 2026-10-25; idle re-record of M1 rows pending: tools/bench/rerecord_m1.ps1)* (`bench`, `tools`). Depends on: T1.11.
  - append_results.py refuses dirty trees and load > 25 % (or labels "indicative"); higher-is-better units summarised
    correctly; per-block sample mode for process() benches (p99 of single blocks); proto-Operator 64-voice bench (interp, JIT)
    as the §13 calibration row.
  - Accept: M1 rows re-recorded on an idle machine; the §13 Operator row present with interp/JIT and crossfade numbers.
```

Also decide before T3.17: Q61 option A (M19) and the Q29 default (M21). Both are QUESTIONS answers rather than tasks.

## M1b: JIT

- [~] **T1b.1 LLVM codegen** *(6562fdb; reopened as partial by the M1 review — see T1b.1-fix1)* (`jit`). Depends on: T1.7.
  - Lowers a Schedule to LLVM IR with the same ProgramImage ABI. Scalar first. Host CPU features and target machine options. Fast-math flags policy: `contract` and `afn` allowed; no `nnan`/`ninf`; denormals FTZ.
  - Accept: every interpreter test passes with the JIT backend producing output identical within 1e-6. Compile time for a 100-node patch is recorded.

- [x] **T1b.2 Voice-SIMD codegen** *(merged 2026-10-24)* (`jit`). Depends on: T1b.1.
  - Vector types across voice lanes (8× AVX2, 16× AVX-512 when the host has it). Bool masks as vector predicates.
  - Accept: 8- and 64-voice benchmarks show at least 5× over scalar JIT for an oscillator+filter voice.

- [!] **T1b.3 Stock subcircuits** *(merged 2026-10-24; partial: adsr ×8 1.6× the C++ cost — JIT causes handed to T1b.2, Q109)* (`stdlib`). Depends on: T1b.1.
  - In `.pqp`: `onepole`, `svf` (TPT), `biquad` (TDF-II), `allpass`, `dcblock`, `comb`, `adsr`, `lfo`, `envfollow`, `slew`, `smooth`. Each has a hand-written C++ reference implementation in `tests/` (not shipped).
  - Accept: output matches the references within 1e-5 (sweeps, impulse responses, modulated cutoff). **JIT'd stdlib filters are within ~10% of the hand-written C++ cost** (benchmark recorded). If they're slower, fix the compiler, not the filter.

- [x] **T1b.4 AOT stdlib + compile cache** *(merged 2026-10-25)* (`jit`). Depends on: T1b.3.
  - At build time, `pqc aot` compiles `stdlib/` into an object file linked into pqdaw. Runtime JIT objects are cached under `cache/` keyed by graph hash + CPU features + compiler version.
  - Accept: opening a stock device at runtime does no JIT compile (test hook counts compiles). A second load of the same user patch hits the cache.

- [x] **T1b.5 Interpreter→JIT live swap** *(merged 2026-10-24)* (`engine` + `jit`). Depends on: T1b.1, T1.9.
  - An edit plays immediately through the interpreter, then swaps to the JIT image when it's ready.
  - Accept: an offline test with a simulated compile delay. The swap is click-free (same criteria as T1.9). The JIT ProgramImage costs less than the interpreter's (benchmark).

- [x] **T1b.6 LLVM size check** *(merged 2026-10-25; user kept the 47 MB exe, Q118)*. Depends on: T1b.4.
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

- [x] **T2.6 Tuning as signal** *(merged 2026-10-24; lowering wiring → T2.6-fix1)* (`tuning` + `graph`). Depends on: T2.3, T1.7.
  - The per-region linear form `cents = k·g − m·P + e·P` (SPEC §3.3). A tuning node in the graph whose generator/period/stretch/reference are modulatable. `voice.hz` derives from it. Region crossings are detected at block rate or at the crossing sample.
  - Accept: a generator sweep through several regions produces continuous per-voice pitch (except at intended region changes) that matches `buildMOSScale` evaluated per sample within 1e-4 ¢. A benchmark shows the per-voice pitch cost.

- [x] **T2.7 Main tunings model** *(merged 2026-10-24)* (`document`). Depends on: T2.6.
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

- [x] **T3.7 Views module, app shell and project I/O** *(519155e..776093f)* (`views`, `app`). Depends on: T3.2, T3.5. 👁
  - Create `pq_views` (ARCHITECTURE §1 update). Ableton main layout: control bar (tempo drag box, time signature, transport, position, loop, grid display), track-area host, detail view (Shift+Tab clip/device; device panel empty until M4), info view, resizable splitters.
  - `TransportPort` (play, stop, continue, locate, loop, playhead snapshot struct) with an app-side fake for tests. Global actions: Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y, Ctrl+N/O/S/Shift+S, Space / Shift+Space.
  - `--screenshot <scene> --project <dir>` renders views scenes from fixture projects; `pqdaw --project <dir>` opens a window; idle loop does not redraw.
  - Accept: `--screenshot shell --project tests/views/fixtures/empty` matches its golden; Ctrl+S then reopen gives an identical document; undo/redo hotkeys go through `History`; Space calls the port; Shift+Tab toggles the detail view; idle window draws 0 frames in 10 s at < 0.1% CPU; first frame with an empty project < 300 ms (release, logged).

- [x] **T3.8 Arrangement view: display and navigation** *(1fc93f2; far-zoom settle repaint → T3.8-fix1)* (`views`). Depends on: T3.7, T3.3. 👁
  - Ruler (bars/beats; time-signature and tempo markers), locators, loop brace, playhead (reads `TransportPort` only while playing). Track lanes with headers (name, colour, fold), group indentation. Note clips as Ableton clip bodies with a cached note preview rebuilt only when the clip snapshot pointer changes. Adaptive grid shading; scroll; Ctrl+wheel and ruler-drag zoom; Z / X; follow mode; overview strip. Virtualisation: per-track sorted clips + binary search; only visible tracks/clips emit geometry.
  - Accept: goldens `arrangement-basic` (8 tracks incl. a nested and a folded group, loop, locators) and `arrangement-zoomed-out`. Scripted: Ctrl+wheel zoom keeps the tick under the cursor fixed (±1 px); Z/X round-trip; ruler click locates via the port. Perf (release, 2560×1440, 64 tracks × 200 clips × 50 notes): full redraw < 2 ms CPU; scroll/zoom frame < 2 ms; draw-list memory flat over 1000 scroll frames.

- [x] **T3.8-fix1 Cached clip preview textures** *(merged 2026-10-24)* (`views`, `ui`). Depends on: T3.8, T0.7-fix4.
  - Render each clip's note preview into a texture (per clip snapshot + zoom bucket) and draw it as one textured quad, so far-zoom settled repaints don't emit tens of thousands of note rects. Rebuild off the paint path within a budget.
  - Accept: at 2560×1440, 64 × 200 × 50, settled full repaint < 2 ms at every zoom down to the minimum, including the first repaint after a zoom settles; goldens unchanged (or deliberately regenerated and reviewed); texture memory bounded.

- [x] **T3.9 Arrangement editing** *(28885ca)* (`views`). Depends on: T3.8, T3.6. 👁
  - Selection (clip select, Shift/Ctrl add, rubber band, time selection, Ctrl+A). Clip drag-move with relative snap, Alt+drag copy, edge resize, cross-track drag, Ctrl+Shift+M, Ctrl+E, Ctrl+D, Ctrl+J, Delete, Ctrl+L. Clipboard Ctrl+C/X/V (in-process document fragments). Ctrl+I, Ctrl+Shift+D, delete time. Right-click grid menu (Adaptive Narrowest…Widest / Fixed / Straight, Triplet, 5-, 7-, 11-tuplet / Snap / Off) and Ctrl+1–5. Double-click opens the clip in the detail view.
  - Accept: scripted tests asserting the resulting document plus exactly one undo step: drag-move with snap on a 7-tuplet grid lands on exact ticks; Alt-drag copy; resize; split; duplicate; consolidate; Ctrl+Shift+D; every grid menu item and Ctrl+1–5. A 60-frame drag = one undo entry. Golden `arrangement-selection` (clip + time selection, drag ghost).

- [x] **T3.10 Track headers, groups and mixer section** *(merged 2026-10-24, 0948f2a)* (`views`). Depends on: T3.8, T3.6. 👁
  - Header mixer section (on the right of the lanes, as in Live): volume, pan, track activator = mute (button and `0`, a param — no recompile; Q15), solo, arm, sends per return, output routing dropdown, tuning chip coloured per main tuning. Ctrl+T, Ctrl+R, Ctrl+G, ungroup, colour menu, height resize, fold. Returns and master at the bottom. PQ Meter-style meters fed by a plain-data `MeterSource`. Ableton solo semantics (exclusive; Ctrl-click additive; group solo solos members; member solo keeps parents audible) as a pure function `effectiveAudible(track)` shared with T3.17. Basic mixer view (Ctrl+Alt+M) from the same strips. Knob/fader drags coalesce to one undo step.
  - Accept: goldens `tracks-mixer`, `mixer-view`; 20+ table tests for `effectiveAudible`; scripted Ctrl+T inserts after the selection, Ctrl+G across nested groups, rename via Ctrl+R + Enter, a volume drag is one undo step with a `paramOnly` ChangeSet.

- [x] **T3.11 Piano roll: EDO mode and note editing** *(merged 2026-10-21; release views/app tests verified)* (`views`). Depends on: T3.7, T3.6, T3.3, T2.2. 👁
  - Clip view: gutter keys, ruler, loop/start/end braces, note area, velocity lane (+ chance toggle). EDO mode: one row per step; equave markers labelled per Q2 (C-style names for 12edo-compatible EDOs); scale highlighting. Notes: draw mode B, select, rubber band, move, resize, Alt-drag copy, delete, velocity drag, Ctrl+U. Up/Down and Shift+Up/Down via T3.6. Ratio/Cents notes drawn at true pitch and marked off-grid. Fold to used notes/scale; vertical zoom/scroll; grid shared with the arrangement; audition via `TransportPort::auditionHz`. Virtualised by visible time range.
  - Accept: goldens `pianoroll-12edo`, `pianoroll-31edo`, `pianoroll-velocity`. Scripted: B-mode draw on a 5-tuplet grid lands on exact ticks; move/resize/copy/delete; Up on a Degree note in 31edo = +1\31; Up on 5/4 → 6/4; Shift+Up = one equave; each gesture one undo step. Perf: 100k-note clip with 3000 visible, full redraw < 1.5 ms CPU, drag frame < 1 ms.

- [x] **T3.12 Piano roll: scale/MOS layouts and live morph** *(e13ce4e)* (`views`). Depends on: T3.11, T2.3, T2.4. 👁
  - One row per degree; Proportional (default) and Equidistant layouts ported from `mos-roll.js` `layoutRows` (~747–860): asymmetric half-height rule, s-rows shrink to slivers as the MOS collapses, s-notes under L-notes; layout toggle keeps the centred row. Gutter keys coloured by step class with the hardness-based L/s blend. Scala scales use the same row model. Background EDO overlay for display and snapping. Live morph: generator changes relayout rows and Degree notes every frame without committing until gesture end (one undo step), using `MosLinearTable` within a region and `buildMOSScale` + `transferModeRotation` at region crossings.
  - Accept: goldens `pianoroll-5L2s-prop`, `pianoroll-5L2s-equi`, `pianoroll-5L2s-near-collapse`, `pianoroll-5L2s-over-31edo`, `pianoroll-scala`. Row layout matches `mos-roll.js` run under Node for 50 (generator, size, height) cases (±0.5 px; script in `tools/fixtures/`). Scripted morph across a region boundary keeps Degree notes on their rows, one undo step. Perf: morph frame (24-degree MOS, 10 equaves, 3000 notes) < 2 ms CPU.

- [x] **T3.13 MOS wheel: region chart** *(f3a10e4)* (`views`). Depends on: T3.7, T2.3. 👁
  - Port of mos-explorer `drawAxis`/`drawMOS`/`drawEDOs`/`drawTemperaments`/`drawGeneratorMarker`/`drawSizeScrollbar` (index.html ~3156–3690): generator cents × MOS size, Stern–Brocot bands from `buildMosRegions`, EDO markers up to a configurable max EDO, temperament ranges, generator marker drag with EDO snapping, size scrollbar, hover info (name, UDP, hardness). Emits a `MosSelection` value (no document dependency in the widget).
  - Accept: goldens `mos-chart-default` (5L2s, max EDO 31), `mos-chart-zoomed`; clicking inside a region selects its MOS (checked against `buildMosRegions` for 100 random points); marker near 7\12 snaps exactly to 700 ¢. Perf: chart build at 800×600 < 1 ms CPU.

- [x] **T3.14 MOS wheel: circle view and popover** *(a1332f5)* (`views`). Depends on: T3.13, T3.6. 👁
  - Port of `drawMOSCircle` (index.html ~3693+): step slices, degree spokes with cents, tonic, JI markers, hardness slider (via `generatorForHardness`), mode rotation by step / by generator, parent/child arrows, MODMOS alterations. Popover linking chart + circle; also EDO entry, Scala file pick, temperament presets. Commits a tuning command to a clip / track / main tuning (one undo step); opened from tuning chips (+ hotkey, Q15). The roll previews live while dragging.
  - Accept: goldens `mos-circle-5L2s`, `mos-circle-modmos`, `mos-wheel-popover`; rotate-by-generator matches `rotateByGenerator`; hardness 2.0 gives L/s = 2 within 1e-6; parent/child arrows match `findParentMOS`/`getChildMOSes`; committing to a clip changes only that clip (one undo step); Esc cancels with no history entry.

- [x] **T3.15 Automation lanes** *(merged 2026-10-24)* (`views`). Depends on: T3.9, T3.10, T3.6. 👁
  - Per-track lanes (show/hide; parameter chooser incl. main-tuning params); `A` automation mode over clips; breakpoint add/move (snapped, value readout)/delete/multi-select; curved segments (Alt-drag) with the curve law shared with T3.16; shapes menu (ramp, sine, square, triangle, saw over the time selection); automated-state indication and "Re-enable automation" (state only). Curves drawn as feathered ribbons (T0.9).
  - Accept: goldens `automation-lanes`, `automation-mode-over-clips`; scripted add/move/delete, Alt-drag curvature, shape insertion; each gesture one undo step with a non-structural ChangeSet; view curve evaluation matches `engine::evalAutomation` within 1e-6 at 10⁴ points.

- [x] **T3.16 Engine timeline: transport, note sequencer, automation renderer** *(f9fc667)* (`engine`). Depends on: T1.8, T0.5.
  - Audio-thread transport (play/stop/continue/locate, sample-accurate loop wrap, tempo map) exposed in `ProcessContext` and reported to the UI (lossy SPSC). `SequenceImage`: immutable flat per-track note array built off-thread from a plain-data description (no `document` dependency); clip loops unrolled lazily by a cursor; published by atomic swap + EpochReclaimer; cursor survives swaps. Emits note-on/off events at exact sample offsets; chance/velocity range via a deterministic per-note RNG (note id + loop pass). `AutomationImage`: breakpoints rendered per sample into audio-rate buffers for automated params (incremental cursor); unautomated params stay param-rate. `evalAutomation` header-only, shared with views.
  - Accept: a note at tick t starts at exactly `ticksToSamples(t)` for block sizes 1/32/37/480; loop wrap emits offs/ons at the exact wrap sample; locate during playback releases all sounding voices; swapping the image mid-note keeps unchanged notes and stops removed ones at the next block; automation buffers match `evalAutomation` within 1e-6; RtScope clean with 64 tracks × 200 clips. Perf: per block with no events < 50 ns per track; 100k-note SequenceImage build < 20 ms.

- [x] **T3.17 Project lowering and playback link** *(merged 2026-10-24)* (`document`, `app`). Depends on: T3.16, T3.4, T3.7, T3.10, T1.9, T1.10 (T2.6 for tuning automation).
  - `doc::lower(Snapshot)` → graph Patch + param map + sequence/automation descriptions: per MIDI track an instrument (M3 default instrument, Q15) → volume/pan → send taps → parent group sum → master; returns; mute/solo via `effectiveAudible`; param slots keyed by stable `ParamRef`. App controller per ChangeSet: `structural` → debounced recompile + T1.9 hot swap; `sequenceOnly` → publish a new SequenceImage; `paramOnly` → `EngineCommand` param writes. `TransportPort` over the engine; playhead and meters fed back; audition on click. `pqdaw --project <dir> --render out.wav [--seconds N]`.
  - Accept: golden offline renders (T1.10 helper) of a fixture with 2 MIDI tracks in 31edo and 5L2s, a group with volume automation, a return with a send, a muted track. Compile-count hooks: moving a note or changing velocity → 0 recompiles, effective at the next block; volume change → 0 recompiles; adding a track → exactly 1 recompile, click-free swap (T1.9 criteria). Solo/mute renders match `effectiveAudible`. Idle stopped app ≈ 0% CPU. End-to-end with NullDevice: open fixture, Space, after 1 s of blocks the playhead reported to views is within one block of 1 s.

- [x] **T3.2-fix1 Retheme from the user's Live 12 screenshot** *(merged 2026-10-24)* (`ui`, assets). Depends on: T3.2. 👁 (User decision Q47.)
  - Re-derive `assets/themes/ableton-dark.json` (and its generator) from the user's Live 12 Arrangement screenshot at `reference/inspiration/live12/arrangement.png` (git-ignored: never commit it or crops of it): sample surface/panel/track-lane/header/ruler/grid/text/accent/selection colours, compare control proportions (button heights, header widths, font sizes). Keep the contrast test passing (adjust targets only with justification).
  - Accept: a side-by-side comparison image (our `widgets`/`shell`/arrangement scenes vs the screenshot regions, generated locally, not committed) checked by eye and listed in REVIEW.md with the sampled values; all goldens regenerated deliberately.

- [x] **T2.6-fix1 Tuning signal in project playback** *(merged 2026-10-24)* (`document` lowering, `app`). Depends on: T2.6, T3.17.
  - doc::lower emits `tune_voices` + the tuning node per track whose notes follow a main tuning (Degree notes), tuning params and their automation lanes lowered as params/automation, `Resources::tunings` built from the document (engine::makeTuningTable), NoteDesc pitch from `resolveTrackPitches` (replacing T3.17's O(n²) noteHz). Q100.3, Q101.
  - Accept: golden render of a fixture whose 5L2s generator is automated across a region boundary (pitches follow, jumps only at the crossing); a generator/reference param change while playing is audible next block with 0 recompiles; static projects render bit-identically to before.

- [x] **T3.6-fix1 Virtual fundamental in the model** *(merged 2026-10-24)* (`document`, `tuning`). Depends on: T3.6, T3.11. (User decision Q40.)
  - Ratio notes in a clip share a virtual fundamental derived from what is entered (1/lcm of their denominators relative to the anchor), updated as notes are added, moved or deleted; Up/Down = next harmonic of the current fundamental; explicit fundamental lock (`b`+index, SPEC §6.2) can override. Decide whether the fundamental is stored (with its update rule) or derived on read; keep reduced ratios exact either way.
  - Accept: 5/4 → 6/4 → 7/4 by Up; adding 4/3 moves the fundamental to 1/12; deleting it moves it back; undo restores it; serialization round-trips; the piano roll draws it (SPEC §4.3).

- [x] **T3.18 Audio preferences + device manager** *(merged 2026-10-24)* (`engine`, `views`, `app`). Depends on: T0.6-fix1, T0.6-fix2, T3.7. 👁 (User decision Q12.)
  - Preferences window (Ableton "Audio" tab equivalent): driver type (ASIO / WASAPI shared / WASAPI exclusive), input/output device, sample rate, buffer size, input/output channel configuration, latency readout, test tone, CPU meter; persisted in user settings; device hot-plug and driver reset handled (stop → reopen → resume, Program kept).
  - Accept: switching driver type/device/rate/buffer while playing reopens the stream without crash or allocation on the audio thread; settings persist across restarts; scripted test with NullDevice + fake device list; golden of the preferences window.

## M2/M3 follow-ups (from the M2/M3 review, docs/reviews/M3.md)

**Bundle A — playback link and lowering (engine, document lowering, app)**
- [x] **T3.17-fix1 Idle gate, time base and link robustness** *(merged 2026-10-25)* (`engine`, `app`). Depends on: T3.17.
  - quiet() skips dead zombie slots and is computed inside ProjectProgram::process before quiescent() (C1);
    engine time monotonic across device reopens, auditions released in audioOffline (M15b); compile wait fails
    fast when the output is stopped (L11); metered / uiNotify / lastError races (L12); periodic collect() on UI
    idle, final meter poll at stop (L13).
  - Accept: a sleep-on test (remove a track, zombie dies, collect, publish a unit, 1000 blocks) under a poisoning
    deleter passes; an audition across a reconfigure ends on time; flush() with the output closed returns < 50 ms.
- [x] **T3.17-fix2 Mixer lowering cost + IR lowering with stable ids** *(merged 2026-10-25)* (`document`, `graph`, `app`). Depends on: T3.17. Before M4.
  - Gains at block rate, per-sample smoothing only while moving; closed sends cost nothing; doc::lower builds
    graph::Patch IR directly with NodeIds from document ids (track, return); the return set is not part of a
    track's patch (H1, H2).
  - Accept: 64 tracks × 8 returns, silent, playing ≤ 3 % of a core (interpreter); adding a return while a chord is
    held recompiles only the return + master units and every held voice's state transfers (no re-attack, state
    keys equal); bench rows in RESULTS.
- [x] **T3.17-fix3 Lowering semantics** *(merged 2026-10-25)* (`document`, `app`, `engine`). Depends on: T3.17.
  - Solo follows output routing (M1); return→return sends lowered with a cycle check or removed from model + UI
    (M2); mute lanes played, signal-less tuning lanes refused or note-on-evaluated (M3); automation overrides
    consumed (M4); > 128 MIDI tracks or a visible limit (M5); deactivated-group / return-to-group routing (L1, L2);
    solo on inactive tracks (L3); ≥ 1-sample notes (L4); --render default length cap (L5); chance varies per
    playback/loop pass live, seeded offline (M16).
  - Accept: the M3/document probes (probe_mix, probe_lanes, probe_slots, probe_edges) give the Live-expected result
    for each case, as new tests.


**Bundle B — tuning signal coverage (tuning, dsp, document, app)**
- [x] **T2.6-fix2 Region boundaries, mode transfer, audio-rate tuning automation** *(merged 2026-10-25)* (`tuning`, `dsp`, `engine`). Depends on: T2.6-fix1.
  - Boundary side from the region hint (H3); transferModeRotation per interval in the table, shared with the roll's
    morph (M7); automation buffers bound to the tuning node's audio-rate inputs (M6; also renders one value per
    lane per block where nothing binds).
  - Accept: proj_boundary_auto == proj_boundary_static at the held 720 ¢ (bit-identical pitch); the T2.6 sweep
    includes boundary generators; a fixture with rotation ≠ 0 crossing 720 ¢ matches the explorer per degree;
    crossing lands on the exact sample (pitch_track.py lag 0).
- [x] **T2.6-fix3 Every note follows its tuning** *(merged 2026-10-25)* (`document`, `dsp`, `app`, `engine`). Depends on: T2.6-fix2.
  - Ratio-on-note anchors follow the anchor's signal, reference-relative Ratio/Cents notes scale by ref(t)/ref₀
    (H4b/c; Q1 and Q100.1 unified); per-voice tuning selection for clip overrides to another main tuning (H4a);
    held notes retuned by Pitch events, never cut (H5); glides compose with the node (M9); description cache keyed
    by the definitions in use.
  - Accept: proj_anchor_ratio keeps 702 ¢ to its anchor throughout; proj_clip_override follows its tuning's
    automation; proj_ref_auto_cents follows the reference; dragging the reference with a held Ratio chord emits no
    note-off (test).
- [x] **T2.6-fix4 Tuning tables and small tuning fixes** *(merged 2026-10-25)* (`app`, `tuning`, `document`). Depends on: T2.6-fix1.
  - Tables keyed by shape, reset with the document, reclaimed by epoch, status when exhausted (M8); Scala value
    strictness (L7); checked lcm (L8); tuning param ranges from paramRange (L9); correct the Q105.3/Q105.4 claims (L10).
  - Accept: New → Open of another project uses no stale table; 1000 wheel size/mode changes keep the pool bounded;
    `3 / 2` and `100,5` are parse errors with a line number.


- [x] **T1.3-fix1 Per-voice free-running sources (Q29 answer)** *(merged 2026-10-24)* (`graph`, `engine`). Depends on: T1.3.
  - `InferOptions::freeRunningPoly` defaults to per-voice inside voice contexts: `noise`, `pulse`, `phasor`/LFOs with no input above const rate get one instance per voice (phase from note-on; noise seeded per voice, decorrelated); an explicit opt-in keeps one shared global instance (document the syntax in PATCH_LANGUAGE).
  - Accept: two simultaneous voices of a patch with `noise()` are decorrelated (|corr| < 0.05) and an LFO restarts at each note-on; the opt-in gives the old global behaviour bit for bit; goldens re-rendered only where a patch relied on the old default (explained); interpreter == JIT.

**Bundle C — views correctness and frame cost (views, ui, app)**
- [x] **T3.x-fix-paint Per-rect paint and automation tiles** *(merged 2026-10-24)* (`app`, `ui`, `views`). Depends on: T3.15, T3.8-fix1.
  - Paint per damage rect (H8); automation bands rasterised into the TileAtlas; text helpers clip (M13); chips
    repaint only on tuning change (M12a).
  - Accept: playing with 16 lanes × 1000 points + meters live: frame ≤ 1 ms CPU at 2560×1440; partial == full over
    the whole shell (incl. the control bar label); volume-drag frame independent of track count.
- [x] **T3.x-fix-edit Editing correctness** *(merged 2026-10-24)* (`views`, `document`). Depends on: T3.12, T3.9.
  - Alt-copy at zero offset is a no-op (M10); store subscribe during notify (M11); Generator box cancel/default/
    focus (M12b); default fit with outliers (M14); window focus loss cancels drags and gestures, capture loss cancels
    automation drags, Esc restores the range selection, view-state changes mark the project dirty (L19).
  - Accept: a test per item; goldens at 1.5× for the piano roll and popover.
- [x] **T3.x-fix-look Readability** *(merged 2026-10-24)* (`views`, `ui`, theme). Depends on: T3.2-fix1. 👁
  - Harmonic labels off the notes + F chip + toggle (M18); label-collision helper (L18); pan indicator and chip
    truncation (L17); control bar per Q107.1 B with CPU meter and tuning chip; panel gutters on the window colour;
    11-px roles (Q107.4 B), SemiBold track names; detail view collapsed until a clip is opened; dockable / movable
    MOS wheel; Ctrl+T = new audio track / Ctrl+Shift+T = new MIDI track per Live 12 (L15; user confirmed 2026-10-24); arrow-key navigation.
  - Accept: REVIEW entry with a side-by-side (local only); goldens regenerated deliberately.


**Bundle D — microtonal UI and the M4 gate**
- [x] **T3.19 Tunings and identities UI** *(merged 2026-10-25)* (`views`). Depends on: T2.7, T3.14. 👁
  - Menu bar (File/Edit/Create/View/Options/Help); Tunings panel (add, rename, colour, remove, reference Hz, equave,
    stretch, open in wheel); note identity menu (convert Degree/Ratio/Cents, lock fundamental); Ratio entry (draw
    snapping to harmonics of the virtual fundamental, type-in); ratio/harmonic note labels; row label modes and a
    hover pitch readout (H7).
  - Accept: scripted: create "Tuning B: 22edo", assign a track, automate its generator, enter 5/4 → Up → 6/4 → 7/4,
    convert a Degree note to Ratio and back, each one undo step; goldens.
- [!] **M4-prep gate** *(items merged 2026-10-24…26; NOT met (M4 review H6): 30-track JIT best 37 % of the block (≤ 25 % wanted), M17 ViewContext split / shared drag layer not done — see T4.x-gate)* (orchestrator): merge T1.x-arch2 + T1b.5 (pqdaw plays JIT images); merge T1.x-text-ids; T1b.3
  stdlib filters/envelopes with C++ references; T1b.4; T1.x-silence (replaces the whole-engine idle gate, M15a);
  T3.17-fix2's IR lowering; a QUESTIONS design note for the document device model (devices, ports, wires, macros,
  layout as document entities with stable ids; how the project patch view and the Ableton view edit one model);
  split ViewContext and extract the shared drag/selection/auto-scroll layer (M17).
  - Accept: the 30-track stress fixture plays in pqdaw at ≤ 25 % of the block period (JIT); M4 tasks written
    against the device-model decision.
- [x] **T0/T1-lows** *(merged 2026-10-25; M1 L6 deferred to JIT follow-up)* (any worker, small): M0 L11, L13, L14, L2-for-ASIO, L8 (or record won't-fix); M1 L6–L10, L3
  seam, Q14 diagnostic, M14 default-tier compile-time proxy, M7 self-test label; L14 build.ps1 synopsis; L16.

- [x] **T0.x-buildsize Smaller build directories** *(merged 2026-10-26: dev 15.7 → 7.4 GB, one-edit rebuild 13.7 → 6.2 s)* (`cmake`, `scripts`). Measured 2026-10-26: one dev build = 17.5 GB (pdb 5.9, ilk 5.2, obj 3.5, exe 1.8, lib 1.1): ~70 executables, most statically linking all of LLVM.
  - Steps: (1) dev links with `/DEBUG:FASTLINK` (PDBs reference the /Z7 objects instead of copying them); (2) link `pq_jit`/LLVM only into targets that use the JIT (jit/playback/app tests, pqdaw, pqc, JIT benches) — fix the over-broad library dependency that pulls it into core/tuning/document/ui/views tests; (3) bench executables excluded from the dev `all` target (built on request: `-Target pq_bench_x`); (4) keep incremental linking (fast edit → relink).
  - Accept: dev build dir ≤ 6 GB, release ≤ 3 GB; clean dev build time and one-edit rebuild time no worse than before (measured, same machine load); debugging a test in the debugger still works; all tests pass (-Full dev + release).

## M4: Devices and visual programming (plan: docs/plans/M4.md, approved 2026-10-24)
Prerequisites: T1.x-arch2, T1b.5, T1.x-text-ids (done), T1b.2, T1.x-silence, T1b.3, T1b.4, and the M2/M3 review fixes.

- [x] **T4.1 Device chains in the model + Device view** *(merged 2026-10-26)* (`document`, `views`, `app`)
  - Each track holds an ordered device chain (instances of stdlib/user patches with their param values); lowering uses the chain instead of the fixed default instrument; the detail panel's Device tab shows the chain left → right with generic auto-generated panels (one control per param); add (device menu until the M5 browser), remove, reorder, deactivate, copy/paste; undo; serialization (schema bump + migration from "default instrument").
  - Accept: scripted add/reorder/remove/deactivate each one undo step; reorder recompiles only that track's unit; renders match a hand-built ProjectGraph; golden `device-chain`.

- [!] **T4.2 Circuit canvas** *(merged 2026-10-25; partial: pan p99 2.91 ms at 2× scale fully zoomed out)* (`ui`, `views`)
  - Zoomable, pannable canvas widget: schematic symbol library (gates, op-amp triangles, z⁻¹/delay boxes, filter boxes with response glyphs, oscillator circles, IC rectangles, const/param terminals), orthogonal wires with junction dots, colours/thickness by kind and rate (bool/param/audio/poly/modulation dashed), selection and hit testing, level-of-detail and cached tiles (as T3.8-fix1) so 10k atoms stay fast.
  - Accept: goldens of the symbol sheet and a stock filter opened to atoms; pan/zoom frame < 2 ms at 2560×1440 with 10k atoms; idle 0 % CPU.

- [x] **T4.3 Layout + auto-route** *(merged 2026-10-25)* (`graph`, `views`)
  - Stored layout (a layout section in the .pqp text, ignored by the compiler and preserved by the printer), automatic layered placement for patches without layout, Manhattan router with crossing minimisation, a tidy command.
  - Accept: every stdlib patch opens laid out without overlaps; round trip text → layout → text is byte-stable; routing a 500-node patch < 50 ms.

- [x] **T4.4 Circuit editing + text pane, two-way** *(merged 2026-10-25)* (`views`, `graph`)
  - Add/delete/connect/disconnect, make subcircuit / open / close, param pins, rename; a text pane (code editor widget: syntax colouring, inline errors from the parser and checker) synced both ways through the T1.x-text-ids diff; one undo history across both; edits hot-swap the running unit.
  - Accept: scripted edits in either pane update the other; state kept across edits (T1.x-text-ids criterion); hot swap click-free; parse errors never break the running audio.

- [x] **T4.5 Live probing + cost meters** *(merged 2026-10-26)* (`engine`, `views`)
  - Hover a wire: value, logic level, or a mini-scope from an analysis tap (RT copy into a ring, no allocation); bool wires lit when high; per-subcircuit and per-wire ns/sample meters (§7.6).
  - Accept: probe taps RtScope-clean; tap cost when no probe is open = 0; meter values within 20 % of the bench numbers.

- [x] **T4.6 Device panels framework** *(merged 2026-10-25)* (`views`, `stdlib`)
  - A front panel over a subcircuit: a panel description (controls bound to param pins, layout, visualiser slots), Live-style device frame (title bar, activator, fold, preset slot), PQ Meter visualisers hosted in panels (spectrum, scope, transfer curves).
  - Accept: Utility's and Saturator's panels built from descriptions; goldens; panel repaint on param change damages only the control.

- [x] **T4.7 Cross-device wiring** *(merged 2026-10-26)* (`document`, `views`, `app`)
  - Export jacks on top of device panels; drag from a jack to any parameter anywhere (auto-scroll across tracks); bezier cables over the UI coloured by rate, show/hide/fade; modulated knobs show a live range ring; modulation wires in the document (ProjectWire) lowered through the per-track units (merging units per Q84 rules).
  - Accept: wiring an LFO export on track 1 into a filter cutoff on track 3 is audible next compile, one undo step; ring shows the live range; cables follow scrolling at frame rate. Wires add no delay; a wire that closes a delay-free cycle gets an automatic z⁻¹ (1 sample), drawn on the wire (SPEC §15 2026-10-24).

- [x] **T4.8 Racks + macros** *(merged 2026-10-26; partial items in Q140: Instrument Rack zones are per-voice gains, gated chains don't sleep)* (`document`, `views`, `stdlib`)
  - Audio Effect Rack and Instrument Rack as subcircuits: parallel chains with chain mixer (volume/pan/mute/solo), chain-select zones with fades, Instrument Rack pitch zones in Hz or the track's tuning and velocity zones; 16 macros (targets with min/max/curve; macros are signals, so modulatable); macro variations (store/recall/randomise with exclusions); nesting; openable in the circuit editor.
  - Accept: zone crossfades click-free; a macro driving 8 targets costs one param write; rack ⇄ circuit view edits stay in sync; goldens `audio-rack`, `instrument-rack`.

- [x] **T4.9 Utility + Saturator** *(DSP + panel merged 2026-10-25)* (`stdlib`)
  - Built in the patch language with panels; parity per SPEC §8 (Utility all controls; Saturator all curve types, waveshaper, shaper, colour EQ, soft clip, HQ via `oversample`).
  - Accept: transfer curves and frequency responses vs documented formulas / known behaviour (no Live renders for now); CPU per instance in RESULTS (ship).

- [x] **T4.10 EQ Eight** *(DSP + panel merged 2026-10-25)* (`stdlib`, `views`)
  - 8 SVF-based bands, all types and slopes, stereo/L-R/M-S, adaptive Q, HQ, scale, spectrum display (PQ Meter port), audio-rate modulatable band params.
  - Accept: magnitude responses vs analytic responses within 0.1 dB; stable under audio-rate cutoff modulation; panel golden.

- [x] **T4.11 Auto Filter** *(merged 2026-10-26)* (`stdlib`, `views`)
  - Live 12 filter types and circuit models (Clean, OSR, MS2, SMP, PRD, approximated), drive, morph, LFO, envelope follower, sidechain.
  - Accept: responses per model vs published/known model behaviour (listening notes in REVIEW); modulation click-free; panel golden.

- [x] **T4.12 Operator** *(merged 2026-10-26; voices setting fixed by T4.12-fix1; interpreter window: unit edits held since T1b.x-perf (Q137); topology rebuilds that continue a unit (e.g. Operator `voices`) held since T4.x-fix-engine (M4 review M3); still interpreted at first: cold opens without a JIT cache, and units with a new member set (a new track, a wire that merges units))* (`stdlib`, `views`)
  - 4 oscillators, 11 algorithms, ratio/fixed (coarse ratios as JI harmonics), per-osc envelopes, pitch envelope, LFO, filter, user waveforms, glide, spread, voices; exported oscillator outputs; pitch follows the tuning signal.
  - Accept: algorithm routing tests; SPEC §13 calibration row (64 voices, interpreter and JIT, with crossfade numbers) ≤ the budget; panel golden; tuned playback in 31edo and a MOS.

- [x] **T4.12-fix1 Operator: circuit-model filter, voices, interpreter cost** *(merged 2026-10-26; interpreter-window overrun → T1b.x-perf)* (`stdlib`, `document` lowering, `engine`). Depends on: T4.11, T4.12.
  - Operator's filter reuses Auto Filter's circuit models (resolve the resonance-law mismatch, Q133 item 11; reference + panel Circuit/Drive + golden); the `voices` setting read by the chain lowering (track polyphony); reduce the interpreter cost of the full configuration (Q131) or document why the JIT window makes it acceptable.
  - Accept: Operator with MS2/PRD filter at 64 voices within SPEC §13 on the JIT; voices setting changes polyphony (test); reference ≤ 1e-5.

- [x] **T1b.x-perf JIT/optimiser follow-ups from M4 devices** *(merged 2026-10-26)* (`jit`, `graph`, `engine`).
  - Q111 option C (compile voice groups first, swap in the pair image later in the background — Instant, bit-identical); Q122 fuse consecutive global per-sample loop regions like voice "chains" (EQ Eight L/R 2.7× stereo); Q126 per-image fixed overhead (+6 ns/block since the shared host; tiny always-busy units slower on JIT than interpreter); Q132 optimiser folds select(const, voice-rate, literal); M1 L6 (CodeOwner destructor mutex).
  - Accept: each item measured before/after on ship (bench rows), bit-identity interpreter == JIT kept, compile-time/size bounds kept or justified.

- [x] **T4.4-fix1 Circuit editor on the project's devices** *(merged 2026-10-26)* (`views`, `document`, `app`). Depends on: T4.1, T4.4. (User decision 2026-10-26, SPEC §15.)
  - "Open in Circuit View" on a device (context menu / double-click title) opens that device's patch in the circuit editor; edits go through document commands (one undo history with the Ableton view) and recompile only that track's unit with state kept; editing a stock device forks it into a user device (the instance's own patch copy stored in the project; schema change + migration; stock and other instances unchanged; visible "edited" marker); View > Open Patch… on loose .pqp files moves to a developer option.
  - Accept: scripted fork-on-edit (other instances and the stock device unchanged, one undo step, undo restores the stock reference); edits audible after a unit compile with state kept; project save/load round-trips user devices; golden of the editor opened from a device.

- [x] **T4.13 Project patch view** *(merged 2026-10-26; 👁 review pending)* (`views`, `document`; SPEC §15 2026-10-24)
  - The whole project as one editable patch: one band per track in lane order (source node playing that lane's MIDI or audio clips → device chain → compact mixer-strip node), groups as containers with their own chain, returns fed by send wires, master on the right; fully two-way with the Ableton view; semantic zoom from the whole project down to one band's atoms; an animated morph from the arrangement (lanes keep their vertical position and unfold into bands; clips collapse into the source node) and back.
  - Accept: the view of a fixture project matches its ProjectGraph exactly (nodes, wires, groups, sends); every edit kind in the patch view (wire, re-route to a group/return, add/remove device, mixer change) changes the document and shows in the Ableton view, one undo step each; morph animation at 144 Hz with 64 tracks; zoomed-out full-project paint < 2 ms (tiles); goldens `project-patch-overview`, `project-patch-band`.

- [x] **T4.15 Circuit-view legibility: node tooltips, dead-branch hiding, constant labels** *(merged 2026-10-26; 👁 review pending)* (`views`, `graph` read-only helpers; SPEC §15 2026-10-26 legibility). Depends on: T4.4, T4.4-fix1.
  - Hovering any node (atom, subcircuit, param, input/output, constant) shows a tooltip after the usual delay: name and kind, a one-line description (atoms: a description table next to the atom signatures; subcircuits: the first comment line(s) of the patch definition; params: label, unit, range, current value), and the inferred rate/poly. Constants show their value and, where one matches within float precision, a recognised form (small rationals p/q, multiples/fractions of π and 2π, √2 and 1/√2, ln 2, dB-ish values). Branches that a structural constant (`real const` input with a literal argument, folded `select`/`mux`/`?:`) switches off are hidden by default — nodes reachable only through a dead branch and their wires — with a visible "show inactive" toggle (per editor, remembered in prefs) that draws them dimmed; hiding never changes the document, the text pane or the compiled program.
  - Accept: tooltip for each node kind (view tests); recognised-form table tested (4/27, 1/2π, 1/√2, …, and no false match for arbitrary values); Saturator at its defaults shows only its active path (node count asserted) and all 9 curves with the toggle; Operator at defaults likewise; toggling and hovering cost no recompile and keep idle CPU at 0 %; goldens `circuit-tooltip`, `circuit-inactive-hidden`, `circuit-inactive-shown`.

- [x] **T4.16 Interaction affordances + QoL sweep** *(merged 2026-10-28: T4.16a/b/c; deferred items in Q143/b-a/c-a; 👁 review pending)* (`ui`, `views`; user request 2026-10-28). Depends on: T4.x-cursor-api (merged: `Widget::setCursor` / `cursorAt`, `UiTree::cursor`). Three parts, one worker each:
  - **T4.16a controls + shell**: every control and shell part shows the cursor for what a press would do (knob / fader / number box: vertical drag ↕; text fields I-beam; fold triangles, header buttons, dropdowns, menu-opening parts: pointer; scrollbars, splitters, track-height and panel edges: resize), hover states where Live has them, and the missing everyday QoL of the control bar, mixer, preferences, tunings panel, MOS wheel, menus.
  - **T4.16b arrangement + piano roll + automation**: notes and clips show which part is grabbed (body = move, start / end edges = resize ↔, fades / loop / clip-gain handles, velocity bars ↕) with a hover highlight of that edge/handle as in Live; loop brace and locators; lane-height edges; automation breakpoints and segments; draw mode = crosshair/pencil; keyboard: arrow keys nudge/transpose the selected notes and clips by grid / semitone (Shift = octave / fine as Live), Ctrl+arrows, Alt-drag, and other clearly-missing Live editing behaviours.
  - **T4.16c devices + circuit + project patch**: Device view arrow keys move the selected device left / right along the chain (and into / out of rack chains) as in Live, Home/End, Enter/F2 where Live has them; device title = move cursor, fold triangle / activator = pointer, export jacks = crosshair, rack chain list / zone editor / macro mapping parts; circuit canvas and project patch view: node move, pin / wire-drag crosshair, pan (middle / Alt drag) move cursor, hovered wire / pin highlight.
  - Accept (each part): a table in the worker's report and docs/REVIEW.md of every interactive part touched and its cursor; tests that hover each part through `TestDriver` and check `tree().cursor()`; keyboard behaviours tested and listed in docs/HOTKEYS.md; anything larger than a sweep fix (a new feature) recorded in QUESTIONS instead of built; no golden changes except where a hover highlight is part of a golden scene (regenerated, viewed); idle CPU stays 0 % (cursor resolution only on input / frame).

- [~] **T4.17 M4 QoL leftovers** *(paused 2026-10-30, branch task/T4.17-qol-leftovers 8658a93: device names + ungroup document side done; voice.noteHz WIP with 2 failing checks in test_racks.cpp; views items not started)* (`document`, `views`, `ui`; user 2026-10-30: Live parity need not be exact, decide and build). The deferred features of Q140/Q142/Q143/Q144 that are small and need no audio clips:
  - Ungroup (Ctrl+Shift+G: a one-chain rack → its devices in place, mappings dropped, wires kept; several chains: refused with a status message); device names (Meta field + rename command, Ctrl+R, shown in the title bar; schema bump + no-op migration).
  - Typing a value into a selected knob / fader (type-in field over the control); drag across track-header toggles flips each; resize every selected track's height at once.
  - Arrangement: draw a new loop by dragging the empty loop row; Home / End for the insert marker; Up / Down move selected clips to the neighbouring track; arrow-key nudges of selected automation breakpoints; velocity-stem hover highlight.
  - Instrument Rack key zones decided at note-on (hold the voice's pitch from note-on at block rate, so a zone edit never steps a sounding voice).
  - Accept: each behaviour tested through `TestDriver` (one undo step where it edits), HOTKEYS/REVIEW updated, affected tests -Full dev + release.

- [x] **T4.x-fix-safety** *(merged 2026-10-31: H1, H2, L4; M1–M3, M9 deferred, see "M4 deferred")* (`document`, `app`, `engine`; M4 review H1, H2, M1–M3, M9, L4): MoveDevice use-after-free (group → child); CostMeter and probe-tap slots retired through the image keepAlive / epoch reclaimer, not wall time; JIT delivery (re-queue stale images, fallback via deferred_, hold for topology rebuilds); rack nesting of any depth without stack overflow (iterative walks; user 2026-10-30: no nesting limit).
  - Accept: group→child / child→group / sibling moves under PoisonFrees + save/reload; cost labels on + an 11 s held edit under PoisonFrees; output stopped + edits → every unit on its newest JIT image after start; racks nested ~10 000 deep load, save, round-trip and lower without a crash.
- [x] **T4.x-fix-devices** *(merged 2026-10-31: H3, H5; the rest deferred, see "M4 deferred")* (`stdlib`, `document`; M4 review H3, H5, M10–M13, L10, L11): EQ Eight 48 dB cut law; Auto Filter in the stock library + AOT; Saturator HQ dry alignment; Operator key-scaling clamp; Auto Filter model levels; smoothing of gain-like params; Utility start value; Saturator default Analog Clip. Also run the review's unrun DSP renders (docs/reviews/M4/dsp/).
  - Accept: LC48/HC48 peak ≤ LC12 peak + 1 dB for q ∈ [0.1, 18]; Auto Filter addable from the device menu; HQ dry/wet 50 % flat ± 0.1 dB to 18 kHz; Auto Filter model levels within 1 dB at default res; ship link of pq_bench_dsp; CPU rows in RESULTS.
- [ ] **T4.x-fix-ui** 👁 (`views`; M4 review M5–M8, M14–M16, L2, L3, L12–L15, UI list items 2–7). Bundle with the rest of T4.17 (same files).
- [ ] **T4.18 Device automation** (`document`, `views`, `app`; M4 review H4): automation lanes for device params, macros, chain mixers and wire amounts.
- [ ] **T4.x-gate** (orchestrator, idle machine): 30-track pq_bench_live_swap on ship (perf task if > 25 %); decide M17 (do or defer explicitly); tracking pass (review L16, L19); then release + ship -Full boundary runs.

- [x] **T4.14 M4 review** *(M4 closed 2026-10-31: review partial (2026-10-30), H1/H2/H3/H5 fixed, boundary runs on 70ce131 release -Full 51/51 + ship -Full 51/51; everything else in "M4 deferred"; review was docs/reviews/M4.md; stopped early at the user's request; unchecked items listed there and folded into the tasks above)* (after T4.8, T4.13, T4.15, T4.16; then a full `ship`-preset test run; also re-measure the 30-track fixture on the JIT with the machine as idle as possible) (independent review agent, as for M0–M3; critique implementation, UI, aesthetic).

## M4 deferred (user 2026-10-30: finish M4 fast, tackle these later)
M4 closes with the safety fixes H1/H2 (+L4), the device fixes H3/H5 (+L11) and the boundary test runs. Everything below is open, with details in docs/reviews/M4.md:
- **Safety / engine**: done in T4.x-fix-engine (merged 2026-10-31: M1–M4, M9 iterative walks at any depth, L1, L4, L5, L17, L18). Open (Q156): ≤ ~32k rack chains per track patch (16-bit ports), Device view frames 24 nested racks inline, file indent capped at 32 levels.
- **Devices / DSP**: done in T4.x-fix-dsp (merged 2026-10-31: M10, M11, M12 levels, M13, L10, L11, Auto Filter renders; comb in tune). Open: Q150 Auto Filter OSR/SMP/PRD drive at 0 dBFS (−10/−22 dB at +12/+24 dB drive), needs the user's ear; OSR/SMP self-oscillation 24–76 ¢ flat (nonlinear, documented); voice-rate gains stay kernel calls.
- **UI (T4.x-fix-ui)**: M5 latched clip lamp keeps the idle UI at 30 Hz; M6/L2 zone-editor and device-reorder drags lack cancel; M7 Instrument Rack key zones read untuned Hz; M8 user-device rename/kind commit guard; M14 Edit menu ignores the Device view; M15 circuit editor fit view unreadable; M16 zone editor axis; L3, L12–L15; UI list items 2–7.
- **Features**: T4.17 remainder (branch task/T4.17-qol-leftovers: device names + ungroup document side done; voice.noteHz WIP with 2 failing checks; views items not started); T4.18 device automation (H4).
- **Process / tracking**: M4-prep gate 30-track ≤ 25 % JIT (best 37 %), M17 ViewContext split; M17 (review) T4.15 optimiser change merged without a release run (covered by the boundary run); L6–L9, L16, L19.

## M5: Audio tracks, browser, user library (plan: docs/plans/M5.md, approved by the user 2026-10-30; M5b Simpler / Drum Rack skipped for now, user 2026-10-31)
Each task's full description and acceptance criteria are in docs/plans/M5.md under the same id. Schema 7 goes to T5.3 (T4.17's paused branch renumbers on rebase). T5.6 does not wait for T4.17 (deferred).

- [x] **T5.1 Audio file decoding: the `media` module** *(merged 2026-10-31; Vorbis SNR 39 dB (encoder ceiling, Q146))* (docs/plans/M5.md)
- [x] **T5.2 Sample pool + RT-safe disk streaming** *(merged 2026-10-31; app ownership of the pool / prefetch on locate / disk indicator moved to T5.4 (Q149))* (docs/plans/M5.md)
- [x] **T5.3 Audio tracks + audio clips in the document** *(merged 2026-10-31; audible playback = T5.4, arrangement editing = T5.6; Q145)* (docs/plans/M5.md)
- [x] **T5.4 Clip player + resampler** *(merged 2026-10-31; app owns pool + disk thread + prefetch + DISK indicator (Q149); no mip copies (cost ∝ rate above +12 st, Q151); 64-track ≤ 1 % met by the clip player (0.22 %) but whole track 1.63 % is mixer-bound (Q152); looped RePitch on streamed samples can underrun at wraps)* (docs/plans/M5.md)
- [x] **T5.5 Waveform overviews** *(merged 2026-10-31; worker owned by ViewContext, not app (Q154); 2560-px draw 47–48 µs vs ≤ 50, thin margin)* (docs/plans/M5.md)
- [x] **T5.6 Audio clips in the arrangement + the audio Clip View** 👁 *(merged 2026-10-31; in REVIEW; Q155 items 1–2 for the user (handles, crossfade gesture); T4.17-dependent clip keys not done)* (docs/plans/M5.md)
- [x] **T5.7 Drag in: drag sessions + files from Explorer** 👁 *(merged 2026-10-31; own OLE IDropTarget (SDL 3.4 lacks hover file names); 100 files 12–14 ms; real Explorer drop = REVIEW hand check; Q157)* (docs/plans/M5.md)
- [x] **T5.8 Project samples: copy on save, collect all, missing files, relink** *(merged 2026-10-31; samples referenced by default; not built: per-project always-collect option, full-file hash, NTFS-file-ID relink — Q145 item 10)* (docs/plans/M5.md)
- [x] **T5.9 Browser index: places, indexer, watcher, search** *(merged 2026-10-31; 200k revalidate 224 ms, load 19 ms, search median 2.5–3.4 ms; no dir-mtime subtree skip, preset categories sniffed until T5.11 (Q153); app wiring + panel = T5.10)* (docs/plans/M5.md)
- [x] **T5.10 Browser panel + preview through a cue bus** 👁 *(merged 2026-10-31; cue via AudioOutput::setCueSource + Cue Out pref; free-tag + duration/BPM/rate/channel filter chips, tag editing and resizable width moved to T5.11; Q158)* (docs/plans/M5.md)
- [x] **T5.x-perf-audio** *(merged 2026-10-31; Q152 (b) 64 audio tracks 1.68 → 0.98–0.99 % of a core (no margin; Q159), resampler mips for resident samples above rate 2 (+24 st 21.3 → 13.6 ns), loop-wrap pre-claim (0 underruns), 10k audio clips save + load 46–48 ms)*
- [x] **T5.11 User library: presets, racks, user devices, tunings, clips; default presets; hot-swap** 👁 *(merged 2026-10-31, incl. T5.10 leftovers (chips, tag editing, browser width); rack presets carry no internal wires; user devices show the stock title (Q160))* (docs/plans/M5.md)
- [x] **T5.12 Drag out: arrangement → browser, → Explorer** 👁 *(merged 2026-10-31; WAV bit-identical to the solo render; real Explorer drag = REVIEW hand check; Q161)* (docs/plans/M5.md)
- [ ] **T5.13 M5 review** (docs/plans/M5.md)

## M5 onward (outline: refine into tasks before starting; most need user review)
- **M4 (original outline, now the section above)**: circuit-diagram editor + text pane, device panels, cross-device wiring, Audio Effect/Instrument racks + macros, Operator, Utility, EQ Eight, Auto Filter, Saturator.; **project patch view** (SPEC §15 2026-10-24: the whole project as one editable patch — bands per track mirroring the arrangement, source nodes for MIDI/audio clips, device chains, mixer-strip nodes, groups as containers, sends/modulation as wires, semantic zoom, animated morph from the arrangement; builds on the circuit-editor canvas + cross-device wiring). 👁
- **M5** (re-scoped by the user 2026-10-30, SPEC §15): **audio tracks + audio clips** (pulled forward from M6: samples dragged in from the browser or Explorer play on the timeline, without warping beyond Re-Pitch), **browser + user library** (index, search, preview, drag in/out, saving presets / user devices / racks). Plan: docs/plans/M5.md. Simpler and Drum Rack follow (M5b) once sample playback exists. 👁
- **M6**: warp modes beyond Re-Pitch, recording, take lanes, Capture MIDI, PDC, metronome.
- **M7**: JI objects. 👁
- **M8**: macro mode, MIDI/key mapping.
- **M9**: remaining devices + parity suite (needs Live 12 reference renders from the user).
- **M9 (RMSC)**: RMSC per SPEC §8.1. Validate against the original by rendering Borrasca in Live 12 (user-supplied renders) with identical settings; the original's known bugs (RM pre-fade step, NaN, Padé clipper) are expected differences.
- **M10**: groove, transformations, export, freeze, bounce, collect.
- **M11**: interop (MIDI files, MTS-ESP, MPE).
- **M12**: background re-optimisation (incl. whole-project compilation when editing pauses, user decision 2026-10-21), multithreaded scheduling.
