# OP: CPU optimisation pass (audit OP.0, 2026-11-24)

User request: "optimize the daw and look for ways to better optimize the cpu, jit compiler, etc so that we use the
absolute minimum amount of cpu as possible in all scenarios."

This file is the audit: where the time goes, measured, and the tasks that follow from it (OP.1 … OP.8). No product
code changed. Measured on `79d44dd5` (main; the branch was rebased onto WF.2 afterwards and not re-measured), ship
preset, i7-11700F (8 cores / 16 threads, AVX-512), 60 Hz display, RTX 3060 Ti, Windows 11. Another worker was
building in a second checkout for most of the session (system load 40 – 100 %). The scenario rows (§2.1) are medians
of 3 runs with their spread; the other tables say how many runs they rest on (the stage split and the app rows: one
or two). Treat differences under ~10 % between two rows as noise unless the row says "interleaved A/B".

## 0. What the audit found, in one page

1. **The engine's own machinery is not where the time goes.** Timeline, note stages, automation, meters, the output
   copy and the idle gate are 1 – 3 % of a playing project (§2.2). 95 % and more is inside the track units: generated
   code and the dsp kernels it calls. A silent or stopped project costs 0.02 – 0.45 % of a core.
2. **While playing, the UI thread costs as much as or more than the audio engine.** A 16-track session: UI thread 7 %
   of a core, 85 frames per second on a 60 Hz display. One track with its device view open (Basic Synth, Spectrum,
   Oscilloscope): UI thread 14.7 % of a core at 109 frames per second, against 1.3 % for the audio. Every timer (the
   playhead, the meters, each animated face, the position readout) presents its own frame, so the frame rate is the
   sum of the timer rates, not the display's. Idle is right: 0 frames, 0.1 ms of UI CPU in 10 s.
3. **Voice code is latency-bound, not throughput-bound, at the polyphony people use.** One 8-lane voice group of the
   Operator costs about 70 ns per frame in generated code; eight groups cost about 23 ns each (pairs on zmm). The per-sample
   recursions (four envelopes, the oscillators' phase loops) run one after another in separate loops, each waiting
   on its own dependency chain (33 – 70 instructions, 40 – 45 cycles per frame, under 1 instruction per cycle). They
   are independent of each other and could share one loop.
4. **The EQ runs its filters one after another, scalar**: 8 `Svf::pass` calls per EQ (4 bands × 2 channels), each
   bound by the filter's own recursion (≈ 8.5 cycles per sample): 51 % of an EQ-heavy track; plus 15 % recomputing a
   key that calls `log10` and `lroundf` every block.
5. **Per-block fixed costs are about 1 µs per sounding unit** (≈ 15 ns per frame at 64 frames; 30 % of a light track).
   One piece is avoidable today: voice units re-clamp and re-splat every param every block although nothing changed
   (interleaved A/B: 3 – 8 % of a light synth track).
6. **Bench numbers understate the app by 1.3 – 2.5×.** At a device's cadence (256 frames every 5.33 ms, asleep in
   between) the same blocks cost 1.3× (heavy projects) to 2.5× (light ones) what they cost back to back. In the app
   the session's audio thread measured 16 – 20 % of a core against 7.4 % in the hot bench and 10 – 13 % at cadence.
7. Things that looked suspicious and are fine: the AOT seed's AVX2 code, block size, cache footprint with many
   tracks, the sinc resampler, idle units, the sleep gate, the JIT cache (§5).

Expected total, if OP.1 – OP.7 land as estimated: the playing UI falls from 7 – 15 % of a core to 2 – 5 %; a synth +
EQ session's audio falls by roughly a quarter (§3 has the arithmetic per finding). Nothing here changes what is
heard: every proposal is bit-identical except where a row says otherwise.

## 1. Method and how to reproduce

New harnesses (committed with this file):

| What | Where |
|---|---|
| Whole-engine scenarios through `ProjectLink` → `Engine` (JIT images), ns per block | `bench/app/bench_scenarios.cpp` → `pq_bench_scenarios` |
| In-process sampling profile of the audio thread per scenario (no admin rights needed) | `PQ_BENCH_SAMPLE=<file>` + `tools/bench/symbolize_samples.py` (uses the linker map the target writes) |
| The same for a running pqdaw (UI thread, audio thread, kernel / user split) | `bench/support/sample_process.cpp` → `pq_sample_process <pid> <s> <file>` |
| Where inside a JIT stage function the samples fall (loops, instructions) | `tools/bench/annotate_jit.py` |
| Stage and unit split (rdtsc scopes), JIT symbol map, `/MAP` for pqdaw: instrumentation, **not on main** | `git apply bench/support/op_profile.patch` (then `PQ_BENCH_STAGES=1`, `PQ_JIT_MAP=<file>`) |
| Cost at a device's cadence | `PQ_BENCH_CADENCE=1` |
| Save a scenario's project to open in the app | `PQ_BENCH_SAVE=<dir>` |

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL=6
./scripts/build.ps1 -Preset ship -Target pq_bench_scenarios,pq_sample_process -NoTests
cd build/ship/bench
./pq_bench_scenarios.exe                                   # §2.1 (≈ 40 s); --filter REGEX, --csv FILE
$env:PQ_BENCH_CADENCE=1; ./pq_bench_scenarios.exe --filter "session/"            # §2.3
$env:PQ_BENCH_SAMPLE="samples.txt"; ./pq_bench_scenarios.exe --filter "fx/eq x8"  # then:
python ../../../tools/bench/symbolize_samples.py samples.txt pq_bench_scenarios.map --top 30
# Stage split and JIT functions (instrumented tree only):
git apply bench/support/op_profile.patch; ./scripts/build.ps1 -Preset ship -Target pq_bench_scenarios -NoTests
$env:PQ_BENCH_STAGES=1; ./pq_bench_scenarios.exe --filter "session/12"
$env:PQ_JIT_MAP="map.txt"; $env:PQ_JIT_DUMP="img"; $env:PQ_BENCH_SAMPLE="s.txt"; ./pq_bench_scenarios.exe --filter "synth/operator 1 voices$"
clang -c -target x86_64-pc-windows-msvc img6_part0.s -o p0.obj; llvm-objdump -d --no-show-raw-insn p0.obj > p0.dis
python tools/bench/annotate_jit.py s.txt map.txt p0.dis pq_<key>_stage1       # (LLVM tools: C:\dev\llvm-23.1.3\bin)
# UI (your OWN instance only; PQ_SETTINGS_DIR / PQ_BUG_REPORTS_DIR to a scratch folder, PQ_NO_AUDIO_DEVICE=1):
$env:PQ_BUGREPORT_SCRIPT="wait;key:space;wait"; pqdaw.exe --project <saved scenario> --idle-check 10   # frames + thread CPU while playing
$env:PQ_BUGREPORT_SCRIPT="wait;click:1028,124;wait;click:1251,789;wait;key:space;wait"                 # select track 1, open the Device view (1280x800)
pq_sample_process <pid> 8 ui.txt; python tools/bench/symbolize_samples.py ui.txt build/ship/src/app/pqdaw.map
```

`--idle-check S` with a script that presses Space never "settles", so after 30 s it measures S seconds of playback:
frames drawn and CPU per thread. That is the UI measurement used below.

A scenario bench must run with FTZ / DAZ set (`DenormalGuard` in `main`), as every audio thread does. The first runs
of this audit lacked it; all numbers below are from runs with it.

## 2. Baseline

### 2.1 Scenarios (ship, engine block 64 frames at 48 kHz unless "@N"; median of 3 runs, min – max)

"% of a core" = ns per block ÷ the block's period. Back to back (hot caches, turbo): see §2.3 for the device cadence.

| Scenario | ns / frame | spread | % of a core |
|---|---|---|---|
| idle: empty project, stopped, engine asleep | 4.5 | 4.4 – 4.5 | 0.02 |
| idle: empty project, stopped, sleep gate off | 7.6 | 7.0 – 7.8 | 0.04 |
| idle: session stopped, asleep (28 s after the stop, see F12) | 4.4 | 4.2 – 4.5 | 0.02 |
| idle: session stopped, not yet asleep | 51.3 | 50.8 – 54.0 | 0.25 |
| idle: empty project playing | 8.0 | 7.9 – 8.1 | 0.04 |
| silent: 16 MIDI tracks playing, no notes | 29.8 | 24.3 – 39.7 | 0.14 |
| silent: 64 MIDI tracks playing, no notes | 94.6 | 92.3 – 108.3 | 0.45 |
| silent: 16 audio tracks, no clips | 20.0 | 19.8 – 30.0 | 0.10 |
| default instrument, 1 / 8 / 16 / 64 voices | 46.6 / 47.6 / 70.5 / 85.4 | ±20 % | 0.22 / 0.23 / 0.34 / 0.41 |
| Operator, 1 / 8 / 16 / 64 voices | 124 / 132 / 163 / 264 | ±12 % | 0.60 / 0.64 / 0.78 / 1.27 |
| Wavetable Synth, 1 / 8 / 16 / 64 voices | 85 / 89 / 145 / 219 | ±15 % | 0.41 / 0.43 / 0.70 / 1.05 |
| arp (4-note chords in 16ths): default / Operator | 48.8 / 188 | | 0.23 / 0.90 |
| default instrument, 16 tracks × 4 voices | 528 | 504 – 630 | 2.53 |
| Operator, 16 tracks × 4 voices | 2396 | 1809 – 2506 | 11.5 |
| default instrument + Utility ("fx/none": the reference for the next rows) | 53.0 | 41 – 53 | 0.25 |
| + EQ (4 bands, stereo) | 82.9 | 62 – 89 | 0.40 |
| + Compressor / Limiter | 55.3 / 68.4 | | 0.27 / 0.33 |
| + Saturator / Delay Tap / Math / LFO / VCA / Noise | 50 / 60 / 52 / 40 / 45 / 63 | | 0.19 – 0.30 |
| + 8 EQs in series | 300 | 293 – 301 | 1.44 |
| + one analyser (13 kinds), unwatched / watched | 40 – 59 / 42 – 60 | | 0.19 – 0.29 |
| arp + 5 analysers, unwatched / watched | 51.0 / 74.8 | | 0.24 / 0.36 |
| 8 audio clips: Off / Off +300 c / Re-Pitch | 61.5 / 160 / 195 | | 0.30 / 0.77 / 0.94 |
| 8 audio clips: Beats / Beats +300 c | 78.4 / 213 | | 0.38 / 1.02 |
| 8 audio clips: Tones / Tones +300 c | 346 / 380 | | 1.66 / 1.82 |
| 8 audio clips: Texture / Texture +300 c | 55.8 / 282 | | 0.27 / 1.35 |
| 8 audio clips: Complex / +300 c | 2206 / 3668 | 2177 – 4968 | 10.6 / 17.6 |
| 8 audio clips: Complex Pro / +300 c | 3108 / 5434 | 2704 – 5679 | 14.9 / 26.1 |
| 64 tracks, 4 sounding / the 4 alone | 224 / 170 | | 1.08 / 0.82 |
| **session**: 12 MIDI (3 instruments × 4, 4 voices, EQ + Compressor) + 4 Beats audio + EQ, master Limiter | 1544 | 1404 – 1946 | 7.41 |
| session @32 / @128 / @256 | 2373 / 1373 / 1196 | | 11.4 / 6.6 / 5.7 |
| session, units compiled for x86-64-v3 (what the AOT seed's code would be) | 1761 | 1582 – 1883 | 8.45 |
| Operator 64 voices, x86-64-v3 code | 345 | 319 – 388 | 1.65 |
| default 8 / 16 / 64 voices, x86-64-v3 code | 46 / 70 / 96 | | 0.22 / 0.33 / 0.46 |

Per-track cost against the number of tracks (4 voices each; 3 runs): default 37 → 33 → 30 → 33 → 34 ns per frame at
1 / 4 / 8 / 16 / 32 tracks; Operator 123 → 120 → 120 → 117 → 130. Linear: no cache cliff up to 32 tracks.

### 2.2 Split by stage (instrumented build, ns per frame; one run each)

| Scenario | total | engine pre | timeline + clips + automation | units | of which note sources | meters | out copy + post |
|---|---|---|---|---|---|---|---|
| session stopped, not asleep | 47.9 | 0.2 | 1.4 | 33.4 (the Master with its Limiter; every track idle) | 2.9 | 10.6 | 0.3 |
| 64 silent MIDI tracks playing | 87.9 | 0.4 | 2.6 | 52.4 (all 65 units idle: ≈ 0.8 each) | 16.4 | 26.1 | 0.5 |
| default, 1 voice | 33.9 | 0.3 | 1.9 | 29.3 | 0.3 | 0.8 | 0.4 |
| default, 64 voices | 63.7 | 0.3 | 1.9 | 59.3 | 0.3 | 0.7 | 0.4 |
| Operator, 1 voice | 122.6 | 0.7 | 2.6 | 115.0 | 0.4 | 2.3 | 0.4 |
| Operator, 64 voices | 270.1 | 0.5 | 3.2 | 262.0 | 0.5 | 2.1 | 0.5 |
| default + Utility + EQ | 62.7 | 0.4 | 2.1 | 57.6 | 0.4 | 0.8 | 0.4 |
| 8 Beats clips | 66.4 | 0.4 | 32.6 (clip players 29.5) | 27.6 | 0 | 4.0 | 0.4 |
| 8 Tones clips | 284.5 | 0.5 | 248.9 (clip players 245) | 28.9 | 0 | 3.6 | 0.5 |
| 64 tracks, 4 sounding | 213.3 | 0.4 | 2.4 | 186.0 (Master 28: it sums 64 inputs) | 15.3 | 22.2 | 0.4 |
| session | 1659.5 | 1.0 | 28.1 (clip players 22.0) | 1607.8 | 4.7 | 18.7 | 0.6 |

Sampling profile of the session (audio thread, 16 000 samples): generated code 37.9 % (voice stages of the three
instruments, each 2 – 6 %), `Svf::pass` 9.3 %, `sineL8` 7.5 %, `reduceKernel` 5.3 %, `ImageHost::begin` 3.9 %,
`Follow::run` 2.2 %, `signalTap1` 2.2 %, `exp24` + `expf8` 3.6 %, `runUnit` 1.7 %, `tuningHz8` 1.6 %,
`ImageHost::run` 1.6 %, `CentsToHz` 1.4 %, `resolveExternals` + `resolveGlobal` 2.6 %, `sleepReports` 1.2 %,
`Svf::run` 1.1 %, `log10` 1.0 %.

### 2.3 At a device's cadence (`PQ_BENCH_CADENCE=1`: 256 frames every 5.33 ms, the thread asleep in between)

| Scenario | hot, ns / frame | at cadence | ratio | p99 at cadence |
|---|---|---|---|---|
| default, 1 voice | 47.6 | 120 | 2.5 | 256 |
| default, 16 voices | 75.7 | 179 | 2.4 | 274 |
| Operator, 1 voice | 165 | 270 | 1.6 | 413 |
| default + Utility + EQ | 93.8 | 195 | 2.1 | 396 |
| Operator, 16 tracks × 4 voices | 2444 | 2540 – 3135 | 1.0 – 1.3 | 4398 |
| session | 1544 – 2022 | 1981 – 2750 (9.5 – 13.2 % of a core) | 1.3 | 3855 |
| session @128 / @256 | | 1905 – 2116 / 1884 – 2182 | | |

A larger engine block does not help at cadence beyond noise (two runs each, ±15 %).

### 2.4 The app (ship `pqdaw.exe` of this worktree, null audio device, 1280 × 800 window, 60 Hz display)

| State | frames in 10 s | UI thread | audio thread | whole process |
|---|---|---|---|---|
| new project, idle | 0 | 0.09 – 0.13 ms in 10 s | 0.13 – 0.33 % (asleep engine: the stream's wake-ups) | 0.26 – 0.50 % |
| session loaded, stopped | 0 | ≈ 0 | 0.26 % | 0.39 % |
| session playing (arrangement, meters) | 850 | 70.6 ms / s = 7.1 % (kernel 7.2 %, user 2.9 % in a second run at 10.2 %) | 16 – 20 % | 24 – 29 % |
| 1 track (arp, 5 analysers), Device view shown, stopped | 0 | ≈ 0 | 0.27 % | 0.31 % |
| the same, playing (Basic Synth faces, Spectrum, Oscilloscope visible) | 1092 | 146.9 ms / s = 14.7 % (kernel 4.3 %, user 7.4 % in the sampled run at 11.7 %) | ≈ 1.3 % | 18.0 % |

Also while playing: the `pqdaw disk` thread 0.6 – 1.6 % of a core (mostly kernel), also in the project without audio
clips; three NVIDIA driver threads 0.25 % together. The F12 report of the playing instance: "build 0.03 ms, submit +
present 0.28 ms" per frame, present mode MAILBOX.

UI thread samples while the faces play (8 000 samples; the thread is waiting in 91 % of them): `RealFft` (FFT +
**its constructor** + the sin / cos of its tables) 2.3 %, `memcpy` 0.8 %, `smoothPolyline` 0.6 %, NVIDIA user-mode
0.9 %, the rest spread thin over the paint code.

### 2.5 Existing benches (ship, this run; the rows a later task will want)

| Row | Value |
|---|---|
| `pq_bench_jit` proto_operator ×8: interp / groups / pairs256 / pairs512 | 62.8 / 6.36 / 5.66 / 6.31 ns per voice-sample |
| `pq_bench_jit` proto_operator ×64: interp / groups / pairs256 / pairs512 | 60.4 / 5.82 / 5.52 / 2.89 |
| `pq_bench_stdlib` adsr ×8: JIT / C++ reference | 0.68 / 0.50 ns per voice-sample (5.5 ns per frame per group) |
| `pq_bench_voices` overhead, 64 voices: held / gliding linear / Exponential / S-curve / Overshoot | 0.72 / 10.9 / 29.9 / 34.2 / 50.4 µs per block |
| `pq_bench_voices` 32 voices, 16 note-ons + 16 offs in one block | 10.0 µs per block |
| `pq_bench_playback` mixer 64 × 8: silent / silent busy / one note per track (JIT) | 0.50 / 4.39 / 20.5 % of a core |
| `pq_bench_playback` audio/64: rate 1 / transposed / no clip yet | 2.18 / 6.72 / 0.37 % |
| `pq_bench_silence` 30-track fixture: asleep / awake (JIT) / awake (interpreter) | 0.06 / 66 / 467 % of a core |
| `pq_bench_jit_cache` 30-track fixture open: no cache / warm cache | 1583 ms (32 compiles, median 293 ms) / 43 ms |
| `pq_bench_project` edit → audible / JIT of the edited unit (642 ops) | 5.5 ms / 219 ms |
| `pq_bench_render` JIT ÷ interpreter | 2.3× (pluck) … 6.7× (drive_zdf) |

### 2.6 Build and JIT configuration (read from the tree)

- ship: `/O2 /Ob2 /Oi /Ot /GL /arch:AVX2 /fp:precise /EHsc`, link `/LTCG /OPT:REF /OPT:ICF`; release: the same without
  `/GL`. No PGO. `/fp:fast` only in the FLAC encoder. `/MT`.
- JIT: host CPU and every host feature (`rocketlake`, AVX-512 here); the light IR pipeline (SROA, EarlyCSE, LICM after
  loop rotation, SimplifyCFG, LSR on functions under 2000 instructions), no InstCombine, no GVN; CodeGenOpt
  Aggressive; `-disable-lsr` in the machine pipeline. No fast-math flags (bit-identity with the interpreter is a
  design rule: Q64); explicit `fma` where the optimiser made a muladd. 256-bit vectors for a voice group, 512-bit for
  a pair of groups on AVX-512 hosts, two 256-bit halves elsewhere. Denormals: FTZ / DAZ from the audio thread's
  `DenormalGuard`.
- The engine runs on one thread per callback (PF.5, units on several threads, is designed and postponed).

## 3. Findings, ranked by (CPU saved in realistic use) × confidence ÷ effort

### F1. The UI presents more frames than the display shows  (OP.1, size S)

- **Evidence.** §2.4: 850 and 1092 frames in 10 s on a 60 Hz display. `interactive()` in
  `src/app/src/commands.cpp:637` paces with `FramePacer::wait` (one frame per compositor pass) only when another
  frame is already due after a present; otherwise it returns to `SDL_WaitEventTimeout` until the widget tree's next
  timer, and that timer's frame is drawn and presented at once. The timers are independent: the playhead (per device
  pixel, clamped to 1/144 s, `arrangement_view.cpp:1109`), the meters (`mixer_view.cpp:160`), each panel face
  (1/60 s, `group_panel_face.cpp:1102`), the device ring (1/30 s), the position readout (down to 4 ms,
  `control_bar.cpp:786`). So frames = the union of the timer ticks. Each present costs kernel time in the driver
  (UI thread kernel 4.3 – 7.2 % of a core, "submit + present 0.28 ms").
- **Change.** Never present more than once per compositor pass: when a frame becomes due less than one compositor
  interval after the previous present, wait for the pacer first (the timers that fire meanwhile fold into that one
  frame). The first frame after an idle wait stays immediate (input latency unchanged). About 20 lines in the loop.
- **Expected.** 85 → ≤ 60 frames (−30 % of the UI thread's playback cost) with the arrangement; 109 → ≤ 60 (−45 %)
  with faces: 2 – 6 % of a core. On a 144 Hz display the saving against today is smaller, but see F2.
- **Risks.** None audible. A frame may show a timer's change up to one refresh later (it could not be seen sooner).
  UI goldens do not move (single frames). The `--idle-check` and perf tests of frame counts need a look.
- **Verify.** `--idle-check 10` with the Space script on the two saved scenarios: frames ≤ 600 + a few, UI thread
  ms; `pq_tests_ui` / `pq_tests_views` default tier.
- **Files.** `src/app/src/commands.cpp` (the loop), maybe `src/ui/src/frame_pacer.cpp`.

### F2. What an animated frame costs, and how many are needed  (OP.2, size M; one decision for the user)

- **Evidence.** With F1 the playing UI is still ≈ 60 presents per second: ≈ 4 – 8 % of a core with faces (user 7.4 %
  at 109 fps → ≈ 4 % at 60; kernel 4.3 % → ≈ 2.4 %). In the user part a `RealFft` is being CONSTRUCTED while
  playing (its constructor 0.54 % of the samples, the sin / cos of its tables ≈ 0.5 %: about 1 % of a core). The
  sampler names the function, not its caller; by reading, the per-frame site is `ScopeTracker::fftOf`
  (`src/dsp/src/scope.cpp:81`), which rebuilds whenever the padded length differs from the previous call (it follows
  the detected period, so it flips between powers of two as the pitch moves); `analysis.cpp:747` rebuilds both
  `SpectrumAnalyzer`s on a config change. The task confirms which. The spectrum's FFT runs per painted frame.
- **Change.** (a) `ScopeTracker` keeps one `RealFft` per size it has used (S). (b) Analysers compute when their tap
  has new blocks, at most once per frame, and not per paint of an unchanged frame (check `analysis.cpp`: S – M).
  (c) A frame-rate cap for animation: meters, faces and analysers at 30 Hz by default (or a preference: 30 / 60 /
  display), the playhead and gestures at the display's rate. **This is a look-and-feel decision: Q?-OP.0-a.**
- **Expected.** (a) −1 % of a core per visible scope. (c) at 30 Hz: the face / meter share of the UI halves again
  (another 2 – 4 % of a core while the Device view is open; more on 120 / 144 Hz displays, where today's cost
  scales with the refresh rate).
- **Risks.** (c) is visible: 30 Hz meters are what most DAWs show, analysers at 30 Hz look slightly less fluid.
- **Verify.** As F1, plus `pq_sample_process` on the UI thread (no `RealFft::RealFft` samples).
- **Files.** `src/dsp/src/scope.cpp`, `src/views/src/devices/analysis.cpp`, `src/views/src/panel/group_panel_face.cpp`,
  `src/views/src/mixer/mixer_view.cpp`, `src/views/src/device_chain/device_chain.cpp`.

### F3. EQ / SVF: the key recomputed per block, the cold tap flag, serial scalar passes  (OP.3 S, OP.7 M)

- **Evidence.** 8 EQs in series (§2.1: 300 ns per frame, 31 per EQ): `Svf::pass<S1>` 50.8 %, `Svf::run` 6.0 %,
  `log10` 4.9 % + `laneKey` 3.1 % + `lroundf` 2.3 % + `svfParamsFromInputs` 2.2 % (= 12.5 %), `signalTap1` 2.3 %.
  - `laneKey()` (`src/dsp/src/kernels_svf.cpp:86`) calls `svfParamsFromInputs(1000, 1, 1, mode, stages, prewarp, sr)`
    every block only to turn three floats into enums: that is where the `log10` and the `lroundf` come from.
  - `signalTap1` (`kernels_signal_tap.cpp:15`): 236 of its 372 samples sit on the instruction after
    `mov eax, [r10 + 0x100]`, the load of `TapRing::readers_`: a cache miss per unwatched tap per block (≈ 55 ns).
  - `pass` is one section over the whole block: 2.06 ns per sample per pass ≈ 8.5 cycles, the latency of the
    filter's own recursion. An EQ makes 8 such passes (4 bands × L, R) one after the other; nothing overlaps.
- **Change.** OP.3 (S): keep the last `(mode, stages, prewarp)` bits and their `LaneKey` in `SvfState` and reuse
  them when the three inputs are bit-equal; move `readers_` to the ring's first cache line or mirror "wanted" where
  the kernel already reads. OP.7 (M): run the sections that are independent or feed-forward of each other in one
  loop: L and R of a band in one 2-lane vector, and the bands of an EQ as a software pipeline (band 2's sample n
  needs band 1's sample n only, so the out-of-order core overlaps the four recursions). Two ways: a multi-section
  kernel the schedule forms from adjacent `svf` ops with block-rate coefficients, or generating the SVF section
  inline so the existing global chains (Q122) fuse it. Same arithmetic per section: bit-identical.
- **Expected.** OP.3: −12 % (key) and −2 % (tap) of an EQ. OP.7: the passes are 57 % of an EQ; at a realistic 3 – 4×
  on them (throughput then binds: 8 recursions × ≈ 12 µops), −38 … −43 %. Together an EQ goes from ≈ 31 to ≈ 15 ns
  per frame. In the session (SVF 12.5 % + tap 2.2 % of the audio): −7 … −9 %.
- **Risks.** OP.3 none (bit-identical by construction; a ramping coefficient keeps today's path). OP.7: the codegen /
  schedule change needs the interpreter == JIT corpus and the EQ goldens; coefficient ramps and per-sample (audio
  rate) cutoff inputs must keep the current code.
- **Verify.** `pq_bench_scenarios --filter "fx/eq"` (interleaved against the old exe), `pq_bench_playback
  --filter "eq oversample"`, `pq_tests_dsp`, `-Affected -Slow`.
- **Files.** OP.3: `src/dsp/src/kernels_svf.cpp`, `src/dsp/include/pq/dsp/atom_state.h` (SvfState),
  `src/core/include/pq/core/tap_ring.h`. OP.7: `src/dsp/src/kernels_svf.cpp` + `src/graph/src/schedule.cpp` or
  `src/jit/src/codegen.cpp`, `src/engine/src/interpreter.cpp`.

### F4. Voice units redo their whole param pass every block  (OP.4, size S)

- **Evidence.** `ProjectProgram::runUnit` (`src/engine/src/project_program.cpp:1662`):
  `ctx.paramsUnchanged = paramsSame && !U.voices;  // a voice engine's images get their own contexts`. Since PF.4b
  the `VoiceEngine` passes the unit's context straight to its image (`voices.cpp:1451`) and never touches the
  params, so the exclusion has no reason left; `ImageHost::begin` then clamps, splats and compares every param of
  every instrument unit every block (3.3 – 6.4 % of a synth track, `begin` in every profile above).
- **Change.** `ctx.paramsUnchanged = paramsSame;` (the fade context already forces `false`).
- **Measured** (interleaved A/B, 6 rounds, the one-line change): default 1 voice 0.92, Operator 8 voices 0.93,
  Wavetable 8 voices 0.92, default 16 tracks 0.94, 8 tracks 0.92, others 0.97 – 1.01. Call it −3 … −8 % on light
  synth tracks, −2 … −4 % on the session.
- **Risks.** A voice image whose params change through a path that does not bump `blockGen_`. The first block after
  a swap is covered (`firstBlock_`). Run the hot-swap, automation and live-param tests.
- **Files.** `src/engine/src/project_program.cpp` (shared with F8, same task).

### F5. Independent per-sample recursions of a voice stage run in separate loops  (OP.5, size M – L)

- **Evidence.** Operator, 1 voice (one group): the voice stage function is 57 % of the unit (§2.2: 115 ns per frame
  → ≈ 66 ns). `annotate_jit.py` on it: 24 loops over the block's frames, separated by kernel calls; the four
  envelope loops (33 – 70 instructions each) take 13.5 + 17.9 + 15.1 + 11.0 = 57.5 % of the function, the four
  oscillator phase loops (31 instructions) 21.8 %. A 33-instruction loop and a 70-instruction loop cost the same
  (≈ 10 ns per frame, 40 – 45 cycles): each is bound by its loop-carried chain, level → max → sub → mul → min → cmp →
  mask → blend → fmadd → fmadd → max → min → blend (the excerpt below is the default instrument's envelope; 54 % of its
  voice stage and 19 % of the whole track). The same shows from outside: one group costs 6.4 ns per voice-sample,
  eight groups in zmm pairs 2.9 (§2.5); the Operator is 124 ns per frame with one group and 264 with eight: each
  further group adds 20.

  ```
  ; (the comments are my reading of msenv's terms, not checked op by op)
  vmaxps   %ymm28, %ymm22, %ymm22        ; b = max(level, floor)             <- carried
  vsubps   %ymm22, %ymm5,  %ymm23        ; dist = peak - b
  vmulps   %ymm16, %ymm23, %ymm23
  vminps   %ymm17, %ymm23, %ymm7         ; clip
  vcmpleps %ymm7,  %ymm6,  %k1           ; reach   (and again into %ymm6: the same compare twice)
  ...
  vfmadd213ps %ymm27, %ymm24, %ymm29
  vfmadd231ps %ymm26, %ymm28, %ymm29     ; target + (b - target) * coef
  vmaxps   %ymm30, %ymm29, %ymm24
  vminps   %ymm22, %ymm24, %ymm22
  vpmovd2m %ymm21, %k1
  vmovaps  %ymm23, %ymm22 {%k1}          ; next level                        -> carried
  ```
- **Change.** Put the recursions that do not depend on each other into one loop over frames, so their chains
  overlap in the out-of-order window (the T1b.2 chains already do this for adjacent inline regions; here the regions
  are apart because a called kernel, the envelope's curve, the oscillator's table read, sits between them). The
  schedule orders a stage's regions topologically; among the valid orders, group the per-sample inline regions and
  move the called kernels before or after the group; codegen then fuses the group as it fuses a chain today, within
  its cap on live values. No arithmetic changes: bit-identical, interpreter included (it only reorders regions).
- **Expected.** Four chains of ≈ 40 cycles in one loop are throughput-bound at ≈ 170 instructions per frame ≈ 45 – 55
  cycles for all four instead of 4 × 42: the envelope share falls to about a third, the phase loops overlap for free.
  Operator voice stage −40 … −45 %, the Operator unit −20 … −28 % at any polyphony (the pair function has the same
  loops). The default instrument (one envelope + one phase loop + a smoother): −8 … −10 %. Session: −10 … −12 %.
  Confidence medium: the arithmetic is from measured loops, the fused loop is not built.
- **Risks.** Region order is observable in cost meters (the fused group is one metered unit, as chains are today)
  and in the sleep plan (one sleep segment per chain). Compile time of the fused loop (LSR threshold, register
  pressure with 32 ymm on AVX-512, 16 on the baseline target: measure both targets).
- **Verify.** `pq_bench_scenarios --filter "synth/|arp/|scale/"`, `pq_bench_jit --filter voice-code`,
  `pq_bench_stdlib`, the interpreter == JIT corpus (`pq_tests_jit`), `-Affected -Slow`, ship link of `pq_bench_dsp`.
- **Files.** `src/graph/src/schedule.cpp` (region order), `src/jit/src/codegen.cpp` (chain formation),
  `src/engine/src/image_host.cpp` if the meter / sleep bookkeeping per region needs it.

### F6. Expensive element-wise kernels recompute a constant input every sample  (OP.6, size S – M)

- **Evidence.** Wavetable Synth, 64 voices: `__avx2_exp24` 11.8 % + `elementwise<CentsToHzOp>` 9.1 % (the cents →
  Hz of a pitch that does not move during the block), `tuningHz8` 3.7 % (its constant path still does eight scalar
  `exp2` and `log2` per group per block). Session: 3.4 % + 1.6 %. A voice's `Hz` and `Cents` are audio-rate buffers
  by ABI (glides), so everything derived from them is computed per sample.
- **Change.** In the element-wise kernels whose operation is a transcendental (`cents_to_hz`, `exp`, `pow`, `log`,
  the exact `tan` for audio-rate cutoff): compare each frame's input vector with the previous one (one compare and
  a movemask) and reuse the previous result when equal; `tuningHz8` keeps its last block's inputs and results per
  lane. Same result bits (a pure function of the same inputs).
- **Expected.** Wavetable Synth −15 … −20 %, default instrument −3 %, session −3 … −4 %; a glide pays one compare per
  frame more (< 1 %).
- **Risks.** None for output. Branchy code in a kernel that was branch-free: measure the gliding case.
- **Verify.** `pq_bench_scenarios --filter "synth/wavetable"`, `pq_bench_dsp --filter atom`, `pq_bench_tuning_signal`.
- **Files.** `src/dsp/src/kernels_math.cpp`, `src/dsp/src/kernels_tuning.cpp`, `src/jit/src/runtime.cpp`
  (`expV`, `centsToHzV`, the helpers generated code calls).

### F7. The disk thread polls at 500 Hz while the transport runs  (OP.4, size S)

- **Evidence.** §2.4: `pqdaw disk` 0.6 – 1.6 % of a core while playing, also in a project with no audio clip.
  `SamplePool::diskMain` (`src/media/src/sample_pool.cpp:752`) waits 2 ms per pass while `engaged_ > 0`,
  `waiters_ > 0` or the last pass was busy.
- **Change.** No timeout when no stream is active; otherwise a period derived from the fill of the emptiest ring
  (a ring holds seconds of audio: 20 – 50 ms is still far ahead), with the existing wake event for new claims and
  engages.
- **Expected.** −0.6 … −1.5 % of a core while playing. **Risk:** a stream running dry: keep the underrun counter test
  and the T5.2 streaming tests.
- **Files.** `src/media/src/sample_pool.cpp`.

### F8. Silent buses are metered, idle units are polled, the master sums silent inputs  (OP.4, size S)

- **Evidence.** 64 silent tracks playing = 0.45 % of a core, of which `levelOf` 20 – 34 % (26 – 41 ns per frame: two
  channels of zeros per metered bus per block, `project_program.cpp:1775`), the note sources of idle voice units
  16 – 23 ns per frame (`Engine::onSlot` + `Timeline::onEventSource` per track per block), and with 4 of 64 tracks
  sounding the Master unit costs 28 ns per frame instead of 3 – 4 (it sums 64 buses, 60 of them flagged silent).
- **Change.** Skip `levelOf` for a bus whose `silent` flag is set (publish zeros once); ask a unit's event source
  only when the timeline has events for its slot this block (one flag per slot); let a sum over input buses skip
  the ones `inSilent` marks (codegen for the mixer's sum, or zero-input folding per block).
- **Expected.** A silent or mostly idle project −40 … −50 % (0.45 → ≈ 0.25 % of a core at 64 tracks). Small in
  absolute terms; listed because it is what a large, mostly empty project pays for ever.
- **Files.** `src/engine/src/project_program.cpp`, `src/engine/src/engine.cpp`, `src/engine/src/timeline.cpp`.

### F9. Shaped glides are rendered lane by lane, scalar  (OP.6, size S – M)

- **Evidence** (existing bench, §2.5): 64 gliding voices cost 10.9 µs per block linear, 30 – 50 µs with the
  Exponential / S-curve / Overshoot shapes (2.2 – 3.8 % of a core) in `VoiceEngine::renderSegment / renderShaped`
  (`src/engine/src/voices.cpp:763, 854`), against 0.7 µs held. JI warps and S-glides (M7) are this path.
- **Change.** Evaluate a shape over the frames of a segment 8 at a time (the shapes are closed forms of t).
- **Expected.** 3 – 5× on gliding blocks; nothing on held notes. **Risk:** the curve's bits change unless the vector
  form computes the same operations in the same order: keep it exact or re-record the glide goldens knowingly.
- **Files.** `src/engine/src/voices.cpp`.

### F10. Envelopes at a fixed point still run  (not scheduled: size L, after OP.5)

A sustaining envelope's recursion reproduces its own state every sample (`next == level`, bit for bit) while the
gate and the params hold. A region whose inputs are block-constant and whose state is a fixed point could skip its
loop and fill (or flag) a constant, as sleeping regions do for silence. It would take most of what OP.5 leaves of
the envelope cost on held notes. It needs a run-time "constant this block" notion beside "silent this block" in the
image host and the sleep plan: a core-structure change, to be designed with the user first. After OP.5, measure how
much of a real project's time is envelopes at rest before deciding.

### F11. Engine block larger than 64 frames  (not scheduled: Q?-OP.0-b)

Hot, the session costs 7.4 % at 64, 6.6 % at 128, 5.7 % at 256 frames (per-block overhead ≈ 1 µs per sounding unit,
F4 is part of it); the default instrument gains ≈ 15 %, the Operator nothing. At a device's cadence the gain is
inside the noise (§2.3). A larger block also changes param-rate stepping (automation, block-rate modulation), so
live playback would no longer match a 64-frame offline render bit for bit, and device buffers under the block size
gain latency through the adapter. Not worth it on this evidence; OP.4 takes the cheap part of the overhead.

### F12. One-line items (each under 1 % of anything a user would notice)

- A stopped session stays awake 28 s: the master Limiter's gain reduction recovers that long before its region
  reports dormant (0.25 % of a core meanwhile). A settle threshold on the follower state would end it in 1 – 2 s.
- `signal_tap` unwatched: covered in OP.3.
- Fused runs reload the same buffer for every use (the gate: 9 loads and 9 compares in one loop body) and compute a
  compare twice, once to a k-mask and once to a vector: the light IR pipeline has no GVN and the stores between may
  alias. It costs nothing while the loop is latency-bound; after OP.5 it is throughput: a per-run load cache in
  codegen. Fold into OP.5 if the fused loop turns out throughput-bound.
- The phasor converts Hz to its fixed-point increment through double per sample (cvtps2pd, mulpd, cvtpd2dq) even for
  a constant Hz: 31 instructions where 5 would do. Same kind as F6; fold into OP.6 if cheap in codegen.
- `reduceKernel` (voice sum, 5 % everywhere): a horizontal sum per frame; a transposed 8-frame sum as
  `VoiceEngine::mixOutputs` does would roughly halve it (≈ 2 % of a synth track).
- 32 note events in one block cost 10 µs (`VoiceEngine::allocate`): only dense bursts.
- The asleep engine's block is 290 ns; the stream's 190 wake-ups per second are the idle app's 0.26 % (by design:
  AU.stream keeps the stream open).

## 4. Tasks

Two lanes that never touch the same files at the same time. Each task: its own interleaved before / after rows from
`pq_bench_scenarios` (old exe kept beside the new one), the verification row of CLAUDE.md for the files it touches,
and a `bench/RESULTS.md` entry from ship.

| Task | What | Findings | Size | Files | Depends on |
|---|---|---|---|---|---|
| **OP.1** | One present per compositor pass | F1 | S | `src/app/src/commands.cpp`, `src/ui/src/frame_pacer.cpp` | — |
| **OP.2** | Analyser and face frame cost; animation rate (after the user's answer to Q?-OP.0-a) | F2 | M | `src/dsp/src/scope.cpp`, `src/views/src/devices/analysis.cpp`, `src/views/src/panel/`, `src/views/src/mixer/`, `src/views/src/device_chain/` | OP.1 |
| **OP.3** | SVF key cache, tap flag on a hot line | F3 (S part) | S | `src/dsp/src/kernels_svf.cpp`, `src/dsp/include/pq/dsp/atom_state.h`, `src/core/include/pq/core/tap_ring.h` | — |
| **OP.4** | Engine housekeeping: voice-unit param pass, silent meters, idle note sources, silent inputs of the master sum, disk thread poll | F4, F7, F8 | S – M | `src/engine/src/project_program.cpp`, `engine.cpp`, `timeline.cpp`, `src/media/src/sample_pool.cpp` | — |
| **OP.5** | Voice stages: independent recursions in one loop | F5 (+ F12 load cache) | M – L | `src/graph/src/schedule.cpp`, `src/jit/src/codegen.cpp`, `src/engine/src/image_host.cpp` | — |
| **OP.6** | Constant-input fast paths (cents → Hz, exp, tuning), vector glide shapes | F6, F9 | M | `src/dsp/src/kernels_math.cpp`, `kernels_tuning.cpp`, `src/jit/src/runtime.cpp`, `src/engine/src/voices.cpp` | — |
| **OP.7** | SVF sections interleaved (L / R in one vector, an EQ's bands as a pipeline) | F3 (M part) | M | `src/dsp/src/kernels_svf.cpp`, `src/graph/src/schedule.cpp` or `src/jit/src/codegen.cpp`, `src/engine/src/interpreter.cpp` | OP.3, OP.5 (same files) |
| **OP.8** | Re-run this audit's tables on the result; decide F10 with the user | — | S | `docs/plans/OP.md`, `bench/RESULTS.md` | all |

Order: lane 1: OP.3 → OP.5 → OP.7. Lane 2: OP.1 → OP.4 → OP.2 (once answered) → OP.6. OP.1, OP.3, OP.4 are
independent of everything and are the cheapest gains: start with them. OP.4's `project_program.cpp` and OP.5's
`image_host.cpp` are different files; OP.6's `voices.cpp` is not touched by OP.4.

Verification rows: OP.1, OP.2 UI (module test exe, no audio suites); OP.3, OP.5, OP.6, OP.7 `-Affected -Slow` + ship
link of `pq_bench_dsp`; OP.4 `-Affected -Slow`.

## 5. Looked at, already good (do not re-audit)

- **Idle.** Stopped, any project: the engine sleeps after 1 – 6 s of tails (table in the bench output: 1.0 s for the
  instruments, EQ, Limiter, Saturator, Beats clips; 1.5 s Compressor; 5.75 s Delay Tap), then 290 ns per block. The
  UI draws 0 frames and uses 0.1 ms of CPU in 10 s, with or without the Device view open.
- **Idle units while playing.** A track without notes or clips is skipped (≈ 0.8 ns per frame); effects go dormant
  after their tails; an unwatched analyser adds nothing measurable; a watched one ≈ 5 ns per frame (the tap copy).
- **Engine housekeeping**: timeline 0.1 – 0.7, automation 0.2 – 0.4, output copy 0.3 – 0.5, param drain + live input
  0.2 – 1.0 ns per frame.
- **AOT seed's x86-64-v3 code on an AVX-512 host.** The seed only holds default-instrument units, and those cost the
  same either way (46 / 70 / 96 vs 48 / 71 / 85 ns per frame at 8 / 16 / 64 voices). Units the user builds are
  compiled for the host (the Operator at 64 voices would be 30 % slower on v3 code; the session 14 %).
- **Voice pairs on zmm** (T1b.2) and the 256-bit attribute for single groups: confirmed by the ×64 rows.
- **Cache footprint with many tracks**: cost per track is flat from 1 to 32 tracks (§2.1).
- **Engine block size**: 32 is worse; 128 / 256 are within noise at cadence (F11).
- **The sinc resampler** (`resampleImpl<2>`, 55 – 74 % of any transposed or stretched clip, ≈ 10 – 20 ns per stereo
  output frame): 32 taps with interpolated coefficient rows, two fused accumulators per channel, already AVX2 + FMA.
  A zmm variant or a smaller table would give tens of percent at best on something that costs 0.1 % of a core per
  clip.
- **Complex / Complex Pro** (1.3 – 3.3 % of a core per clip): SM.16 just reworked it; not re-profiled here.
- **JIT pipeline and flags**: measured choices are documented in `compiler.cpp` and `bench/RESULTS.md` (light
  pipeline vs O2 / O3, LSR, codegen levels, contraction). No fast-math is a design rule (Q64), not an oversight.
- **JIT cache**: a warm open of the 30-track fixture is 43 ms against 1.6 s; an edit is audible in 5.5 ms on the
  interpreter and the unit's JIT follows in ≈ 220 ms.
- **Damage tracking and partial redraw**: a playing frame builds in 0.03 ms; the cost is the present, hence F1.
- **Build flags.** PGO: the native code that matters is hand-vectorised intrinsics loops and code LLVM generates at
  run time; branchy host code is under 10 % of a playing project, so PGO would be worth 1 – 3 % there at the price of
  a training build step. clang-cl for `pq_dsp`: plausible 5 – 10 % on the scalar kernels (`Svf::pass`, `Follow`),
  nothing on the intrinsics ones; OP.7 takes more from the same code. Neither is proposed.

## 6. Not measured, and why

- **A real audio device.** The app ran on the null device (`PQ_NO_AUDIO_DEVICE=1`) so as not to play through the
  user's speakers. WASAPI / ASIO thread behaviour (MMCSS, the endpoint calls) is not in these numbers.
- **Why the session's audio thread is 16 – 20 % in the app** against 10 – 13 % in the cadence bench: not explained.
  Candidates not separated: the null device's thread at normal priority, the UI thread's meter reads, core frequency
  at 15 % duty. Re-measure after OP.1 (the UI then wakes 30 % less often).
- **Page faults in the callback**: p99 at cadence is 1.4 – 2× the median with no outliers that look like faults;
  no ETW (not an administrator), so not counted directly.
- **Sampler instrument** (needs a sample assigned through the engine's data path) and **automation-heavy projects**
  (the existing `device automation` rows of `pq_bench_playback`: +0.1 % of a core per moving lane per 16 tracks).
- **Racks, cross-track cables, oversampled groups**: the existing playback rows cover them (§2.5); not re-profiled.
- **Threads (PF.5).** One audio thread. In the session the largest unit is 242 of 1904 ns per frame, so units on
  four cores could cut the callback's wall time to about a third; it lowers the risk of dropouts, not the CPU used
  (it adds wake-ups). Postponed by the user on 2026-11-09; nothing here changes that.
- **GPU time.** Only CPU was measured.
- **Displays above 60 Hz**: the playing UI's cost should scale with the refresh rate today (F1 bounds it at one
  present per pass; F2's cap is what would hold it constant).

## 7. Questions

- **Q?-OP.0-a · animation rate.** Meters, faces and analysers at 30 Hz by default (playhead and gestures at the
  display's rate), a preference (30 / 60 / display), or leave them at the display's rate? 30 Hz halves what remains
  of the playing UI after OP.1. Choice until answered: OP.1 only (no visible change).
- **Q?-OP.0-b · engine block.** Keep 64 frames (bit-identical live and offline, lowest latency on small buffers)?
  The measured gain of 128 / 256 is 10 – 20 % hot and not visible at a device's cadence. Choice: keep 64.
