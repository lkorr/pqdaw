# MM review (meta modules, Wavetable / Envelope / VCA / Meta Operator, oversample regions, notes as a signal, free patching, param smoothing, panel faces, analysis modules, resize, groups, free-device rack, lane editing, knob curves)

Reviewer: independent MM review agent (TMM.13), 2026-11-02, `main` at `65a5656` (branch `review/MM`). Scope: `1032f28..65a5656` (171 commits): TMM.1–TMM.18 and their fixes, TX.patching, TX.smooth (+fix1), TX.panel-faces, TX.mod-all, TX.spectrum, TX.scope, TX.spectrogram, TX.resize, TX.groups (+fix1), TX.free-rack, TX.lane-edit, TX.knob-curves, and the orchestrator's circuit hover-info commits.

## What I ran

**Boundary runs (on this commit):**
- `./scripts/build.ps1 -Full` (check preset, `CMAKE_BUILD_PARALLEL_LEVEL=12`, `CTEST_PARALLEL_LEVEL=8`). Build 88.9 s (98 % sccache hits), **0 compiler warnings**. Tests (default + slow) 415 s: **87/88 passed**. The failure was `pq_tests_meta`, `test_wavetable.cpp:386` `CHECK(previews >= 5)` (got 3; the full bake took 956 ms under suite load). Re-run alone 3/3: pass (21 previews, full 191–201 ms). That is finding M2, a wall-clock check in the default tier, not a flake on the known list. No GPU SIGSEGV occurred.
- `./scripts/build.ps1 -Preset ship -Target pq_bench_dsp,pq_bench_render,pq_bench_project,pq_bench_device_panels,pq_bench_project_patch,pq_bench_playback -NoTests`: **0 warnings**, all six linked (no C1001 under `/LTCG`).
- Ship bench pass and alias-suite re-run: see "Benchmarks" and "Loose ends".
- Load: the user's desktop was live (OBS, Voicemeeter, Discord), and my own sub-reviewers ran probes and `pq_tests_views` loops for part of the time. Every bench row below records the `Get-Counter` load sampled around it. The rows I use for verdicts were taken at **6–20 %**. Rows taken at 45–75 % were discarded.

**Five parallel deep reads, each with probes.** Probes were compiled with `cl` against the `build\check` libraries of this commit (flags copied from `compile_commands.json`, asserts on), following the M3–M5 pattern. Sources are in [`MM/`](MM/):
1. **engine**, the new audio-thread paths (param ramps and their side table, frame-table slots and swaps, `wavetable_live` / `wavetable_shape`, oversample regions, voice and signal taps): [`MM/engine/probe_pw_backward.cpp`](MM/engine/probe_pw_backward.cpp) (experiments E1–E6, loose end c).
2. **link**, lock-free handoffs, threads and lifetimes (ProjectLink meta slots, TableBuilder, face voice readers, analysis feeds, TexturePool): [`MM/link/probe_link.cpp`](MM/link/probe_link.cpp) (scenarios A–D, H), [`probe_gram.cpp`](MM/link/probe_gram.cpp).
3. **document**, schemas 10–16 and lowering: [`MM/document/fuzz_mm.py`](MM/document/fuzz_mm.py) (794 mutated projects, each loaded and rendered by `pqdaw --render`), [`extra_cases.py`](MM/document/extra_cases.py), [`dup_keys.py`](MM/document/dup_keys.py), [`probe_mm.cpp`](MM/document/probe_mm.cpp) (group round trips, wire laws, migration).
4. **views** code, plus the GPU-golden crash hunt: [`MM/views/probe_views.cpp`](MM/views/probe_views.cpp) (probes A–E), [`crashcatch.cpp`](MM/views/crashcatch.cpp) (a minimal debugger launcher that writes a minidump and names the faulting module), [`symres.cpp`](MM/views/symres.cpp).
5. **UI and aesthetic**: every MM / TX golden viewed at 1× and 2× and across the zoom series. Contrast figures are WCAG ratios computed from pixels sampled in the PNGs.

I re-read the code behind every Critical / High candidate, ran the benches and the alias suites myself, and checked the UI sub-review's most important claims on the PNGs (`project-patch-group`: the clipped "In", the "‖ free" glyph, the empty closed group).

"Verified" means a probe, test, render or measurement ran on this commit. "Code-read" means it was not reproduced.

## Summary verdict

**MM delivers what the user asked for, and its audio core is the strongest work in the project so far. It should not close before H1–H5 are fixed, and the UI needs one focused pass (H6 and the ranked list below) before the user is asked to judge it.**

What is good:
- **No Critical finding.** No audio-thread path allocates, locks or reads freed memory; no silent wrong output in ordinary use was found.
  - Frame-table slots publish a two-entry chain and retire only `cur->prev` through the reclaimer. Kernels read only the loaded entry and its `prev`. The JIT's inline sine frames cache coefficients, never entry pointers.
  - Param ramps (TX.smooth-fix1) are allocated at build time, bounded per block, restart from the last written value, and are transferred on hot swap. At rest the code is the ramp-free program: Operator full JIT ramps / no-ramps **1.008** with an A/A floor of **0.997**, interpreter 1.003 / A/A 1.003 (ship, this review).
  - Taps cost one relaxed load with no reader (`voice_tap` 0.020 ns/sample, `signal_tap` 0.018).
  - Every new RT entry point has an RtScope test except one bounded copy (L5).
- **The DSP numbers hold, and were reproduced.**
  - Frame-table alias suite worst −127.3 / −127.6 / −130.4 dBFS (44.1 / 48 / 96 kHz), backwards −127.0 / −127.6 / −130.2. Module output −129.5 dBFS. Static live-band bake −134.1. LFO / envelope-rate modulation ≤ −120.9 dBFS.
  - L1 / L8 reads 3.04 ns / 19.4 ns (≤ 4 / ≤ 30), single frame 15.4 ns (≤ 20), sine table 3.1 ns per 8 voices.
  - Meta Operator 0.96× the stock Operator doing the same work (≤ 1.2×).
- **The document is robust.** 794 fuzzed projects: 0 reproducible crashes, 0 hangs, 1 NaN (M6), every reader refusal named a path.
  - Ctrl+G / Ctrl+Shift+G: 12 tricky round trips (interleaved free devices, loops, note-reading heads, switched-off members, wires crossing the boundary): ungroup is the identical document, and grouped renders are byte-identical to the series.
  - Wire laws: 20 / 20 DC probes match SPEC §15 and Q201 (octave +1 = +1 travel, linear = units, dB in dB, @wrap, @signed).
  - Migrations 12→13 (Travel) and 15→16 (Units) keep the sound.
- **The faces look like one family**, and the VCA is close to the user's sketch. Spectrum and Spectrogram really carry PQ Meter's look. Resize lays faces out again, crisply, at the new size.

What is wrong, in five clusters:
1. **A use-after-free in the Device view (H1).** Delete pressed inside a group's embedded panel destroys the panel's own view while its key handler is still on the stack. With poisoned frees a probe crashes in `ProjectPatchView::dispatch`.
2. **Wrong or blocked audio at the link (H2, H4).**
   - File > Open plays the old project's wavetable for about 0.3 s, because meta slots are keyed by device id and ids restart per project.
   - Every structural compile waits for the whole table builder to go idle. Opening a project with 12 baked Wavetables blocks for 2.0 s, and adding a track while another Wavetable is dragged is heard 2.1 s late.
3. **A patch the UI lets you draw can't be compiled (H3).** The obvious FM-feedback wire (a later effect's output into a Phase accumulator's `hz` at the head of a voice run) is marked Delayed, then the scheduler refuses the whole unit. The track goes silent with an error that names no wire.
4. **Criteria not met on this commit.**
   - TMM.4's far-zoom pan budget is missed and has been getting worse (H5): Phase accumulator faces now cost +0.50 ms median / +0.63 ms p99 against +0.3 ms. 32 Wavetable faces cost +1.17 / +2.06 ms.
   - TMM.5's "≤ 160 ms with bake" (M1): the full baked table arrives at 296–347 ms, and the test now times the 17 ms preview instead.
   - TMM.12's node budget (41 vs ≤ 20) stays open (L12).
5. **The UI is not yet at the user's standard (H6, M9 and "UI and aesthetic").** Cables run through labels, knobs and faces in both views (worst in the collapsed free-rack). Wire colours mean different things in the circuit and the Device view. Several elements look broken: the "free" marker renders as "‖", a closed group is two empty boxes, the group's "In" label is clipped, and the Wavetable's "os" cell is blank.

**Counts:** 0 Critical, 6 High, 12 Medium, 24 Low.
- H1–H5 are verified by probes or measurements; H6 is verified on the goldens.
- Every fix is small or medium. None needs a redesign.

**Recommendation:** run one fix task for H1–H4 plus M3, M5 and M6, which are small. Run one perf task for H5 (far-zoom faces) with M4 (the spectrogram lock). Run one UI task for H6, M9, M10 and the top five items of "UI and aesthetic". Restate or fix TMM.5's bake criterion (M1) in QUESTIONS, and move `test_wavetable.cpp:386` to the `perf` label (M2). After that, close MM.

## Findings

Severity (as in M0–M5):
- **Critical**: silent wrong output in ordinary use, or memory unsafety on the audio thread in ordinary use.
- **High**: wrong or unsafe behaviour a user will hit, or a crash on untrusted input.
- Medium and Low as usual.

### Critical

None.

### High

| # | Where | What's wrong | Evidence | Fix |
|---|---|---|---|---|
| H1 | `src/views/src/project_patch/view.cpp:1057-1063` (`ProjectPatchView::dispatch`: `return ctx_.doc() != before;` after `store().dispatch`); `src/views/src/project_patch/canvas.cpp:1265-1268` (Delete → `deleteSelection`); `device_chain.cpp` `DeviceChainView::sync` (`removeChild` of the group's frame) | **Use-after-free: Delete inside a group's embedded panel, with the group's capsule selected, frees the view that is handling the key.**<br>1. Delete calls `deleteSelection` → `dispatch` → the store.<br>2. The store notifies `DeviceChainView::sync`, which removes the group's frame at once.<br>3. That frame owns the embedded `ProjectPatchView` and its canvas.<br>4. `dispatch` then reads `ctx_` through `this`, which is freed; `deleteSelection` and `ProjectCanvas::onKey` return into freed objects.<br>With a normal heap the stale bytes are usually still there, which is why it hasn't shown. Ctrl+Shift+G in the same panel also destroys the view during its own `onKey`; it survives today only because `ungroupSelection` touches nothing after `apply`. | **Verified** ([`probe_views.cpp`](MM/views/probe_views.cpp) probe E, freed memory poisoned with 0xDD while the key is handled): access violation reading 0xffffffffffffffff at `probe_views.exe+0x2cc493`, which `symres` resolves to `ProjectPatchView::dispatch+0x213`. I re-read the three code sites. | Don't destroy frames synchronously from store listeners: retire them and drop them at the next layout or frame (as TMM.7 does for faces), or have `DeviceChainView::sync` defer removals while a dispatch is in progress. Locally, compare snapshots captured before the store call, not through `ctx_` afterwards. Add a TestDriver test (Delete and Ctrl+Shift+G inside an embedded group panel) under PoisonFrees. |
| H2 | `src/app/src/project_link.cpp:1419-1436` (`metaSlotFor`, key = device id + path + index), `:1438-1466` (`syncMetaSlots` keeps a live slot of the same kind) | **File > Open (or New, or any Reset) reuses the old project's frame-table slot, so the new project's Wavetable plays the old project's table until its own full build arrives.** Device ids restart per project, so the collision is the ordinary case (the first Wavetable of two projects). The new instance's first publish is a focused preview built on top of the old table, so frames away from the read position keep the old content for about 0.3 s. The face draws the wrong table for that time too. The tuning-table code handles exactly this case (`updateTuningTables(p, reset)`); the meta slots don't. | **Verified** ([`probe_link.cpp`](MM/link/probe_link.cpp) D): same slot object after the Reset: yes. The new project's instance reads the old project's table: yes. Its own first table came 23.5 ms later, a preview over the old table; the full table about 0.3 s later. | On Reset, drop every meta slot (`m = MetaSlot{}`) and call `tables_->forget(id)` for each, as `updateTuningTables` does. Also drop the target's queued job in `TableBuilder::forget` (L18). Test: open project A, then B with a Wavetable at the same id and a different table; the first rendered block reads only B's table. |
| H3 | `src/document/src/lower.cpp:1712-1716` (`planWires` marks the loop `Delayed`); `src/graph/src/schedule.cpp:602` (the refusal); `src/app/src/project_link.cpp:1650` (status only) | **One wire the circuit lets you draw makes the track uncompilable, while the wire's state says it plays.** A wire from a later effect's output back into the head of a meta voice run (e.g. `Saturator.out → PhaseAcc.hz`, the obvious feedback-FM patch) is planned as `Delayed`. The scheduler then refuses the unit: "feedback loop through add#10 crosses the voice/global boundary (a voice reducer inside a loop)". The message names no wire or device. `--render` fails; in the app the link sets an error status, the unit doesn't play, and the drawing still shows a working delayed wire. `connectWire` doesn't check this, so the UI creates it. | **Verified**: fuzz case 440, and `extra/loop_voice` (4 devices + one wire): `probe_mm states` reports `wire 900: Delayed`, render exits 1. The same project without the wire renders (exit 0). | In `planWires`, mark a delayed loop that crosses a voice → global reducer as `Refused`, with a reason the views show (as cross-instrument cycles already are). Better, lower the delay per voice (z⁻¹ per lane before the reducer), which is what the user means by this patch. Add the case to the wire-state tests. |
| H4 | `src/app/src/project_link.cpp:1674` (`if (metaFresh_) waitForMetaTables(2.0);`, under `compileMutex_`) | **Every structural compile waits for the whole table builder to go idle, not just for the new slots' tables.**<br>• Opening a project with baked Wavetables blocks `ProjectLink::create` for the full 2 s timeout, and some instances still start on an 8-frame preview.<br>• A structural edit (add a track) while another Wavetable's baked knob is being dragged is heard 2.1 s late, and the wait forces a full build per drag step.<br>• While waiting, `compileMutex_` blocks JIT deliveries and `setCostMetering` from the UI thread. | **Verified** (`probe_link` B and C, 10–11 % load): create with 12 baked Wavetables 2034 ms (4 instances: 972 ms), 4 of 12 live on an 8-frame preview. Adding a track during a 60 Hz baked-fold drag: compile 2100 ms vs 72 ms with the builder idle; 9 full builds in 3 s. | Add `TableBuilder::waitFor(targets, timeout)` and wait only for the fresh slots' jobs, without holding `compileMutex_`. Give the open path a progress status instead of a silent 2 s stall. Test: a structural change during a drag compiles in < 200 ms. |
| H5 | `src/views/src/circuit/circuit_canvas.cpp` (face hosting at Far zoom); `bench/views/bench_project_patch.cpp` (`meta_faces`, `wavetable_faces`) | **TMM.4's perf criterion ("pan median and p99 each within +0.3 ms of the same project with plain blocks", 2×, 64 tracks + 32 faces) fails, and has been getting worse since TMM.4.** Far pan with 32 Phase accumulator faces: TMM.4 +0.16 / +0.18 ms → TX.panel-faces +0.29 → **+0.50 / +0.63 ms** now. 32 Wavetable faces: TMM.7 +0.9 ms → **+1.17 / +2.06 ms** now. A far-zoomed project with a few dozen meta devices therefore costs 0.5–2 ms per pan frame, against 0.02 ms with plain blocks: CLAUDE.md rule 1. Near zoom is fine (+0.025 / +0.06 ms). | **Verified** (ship, this commit, load 6–9 %): `meta_faces.pan frame (far)` faces 0.525 / p99 0.674 ms vs plain 0.022 / 0.039 ms. `wavetable_faces.pan frame (far)` faces 1.188 / p99 2.085 ms vs plain 0.019 / 0.030 ms. A first run at 14–20 % gave 0.573 / 1.024 and 1.345 / 2.260. | Profile the far path: the vertex counts (13.6 k for 32 Phase accumulator faces, 27 k for 32 Wavetables, 96 / 224 tile layers) say each face is still drawn as several layers from per-face tiles. At Far, draw one cached composite tile per face (or fold faces into the far chunk cache, invalidated per instance), and skip the live layer when a face is under a few pixels. Keep the bench criterion in the `perf` tier and track it. |
| H6 | `src/views/src/project_patch/*` (wire routing), `src/views/src/devices/device_chain*.cpp` (cable layer, TX.free-rack); goldens `project-patch-notes`, `-resize`, `-panels`, `-free`, `-band`, `device-free-rack`, `device-free-rack-collapsed` | **Cables run over labels, knobs, displays and whole faces in both views. The Device view's cable layer is unreadable when the free rack is collapsed.**<br>• The dashed note wire runs through "Frequency", "1000 Hz", "Resonance" and splits "30 %" (`project-patch-notes`), and through the Wavetable's position slider (`project-patch-resize`). An LFO wire crosses the Operator panel (`project-patch-band`) and splits "Gain" (`project-patch-panels`).<br>• In `device-free-rack` the mint curves cross the Saturator's "Drive", LFO 1's "100 %", the Auto Filter display and the free Phase accumulator's face. In `device-free-rack-collapsed` about six curves fan out of the tab across Basic Synth's title, "Sustain", "Level 0.20" and the Saturator.<br>For a "ui is everything" product whose new feature is free patching, this is the most visible defect of the milestone. | **Verified** on the goldens (crops sampled). | Route a modulation leg through the gaps between panels and enter the knob from its clear side. Where a crossing can't be avoided, draw the wire under text with a 2 px knock-out halo in the panel colour. In the Device view, draw cables that aren't hovered or selected at about 35 % alpha. When the rack is collapsed, put a small badge on each target knob and draw the cable on hover only. |

### Medium

| # | Where | What's wrong | Evidence | Fix |
|---|---|---|---|---|
| M1 | `tests/app/test_meta_wavetable.cpp:658` ("a keyframe edit reaches the engine within 30 ms (64 frames), 160 ms with a bake", `perf`); TMM.5 criterion | **Loose end (e): TMM.5's "≤ 160 ms with bake" is not met, and the test no longer measures it.** Since TMM.8-fix the Wavetable bakes a live-band table (217 ms, ship), and since TMM.9 / TMM.16 the slot's first change after an edit is the focused preview. The test times a fold knob step and stops at that first change, so it passes on the 17 ms preview. It is `perf`-labelled, so `-Full` / `-Affected` never run it. The preview is exact where the voices read, so what is *heard* there is right after about 17 ms; the rest of the table is stale for about 300 ms. | **Verified** (`probe_link` A, check, 11 % load): keyframe edit without bake 8.7–9.7 ms (passes ≤ 30). Fold knob step: first publish 10.8–16.6 ms, full table 296–306 ms. Keyframe edit with fold 0.5 baked (the criterion's case): first publish 16.5–20 ms, full 296–347 ms. Ship bench: live-band bake 217 ms, bake3 417 ms. | Restate the criterion as "first audible ≤ 30 ms (focused preview), full table ≤ 350 ms" in QUESTIONS, or make the full build fast enough (the live-band bake is 3× the full-band one). Either way, make the test time the full table for a keyframe edit under an active bake, and say which in TASKS. |
| M2 | `tests/meta/test_wavetable.cpp:386` (`CHECK(previews >= 5)`, default tier) | **A wall-clock check in the default tier fails under suite load.** It failed in this review's `-Full` (3 previews; full bake 956 ms under load) and in TX.mod-all's run ("pq_tests_meta preview latency"). Every agent's `-Affected` that reaches `meta` can hit it. | **Verified**: failed in the boundary run; alone 3/3 pass (21 previews). | Move the timing part to a `perf`-labelled case. Keep a load-free functional check in the default tier (newest wins, the full table last, only full tables cached). |
| M3 | `bench/dsp/bench_atoms.cpp:114-117, 200-207` (the generic atom sweep) | **An unfiltered `pq_bench_dsp` run crashes with an access violation**, so no one can run the dsp suite end to end, and the rows after the crash are never measured in a whole run. The sweep benches every atom except a hand-kept list. `wavetable_live` and `wavetable_shape` (TMM.8) take a frame-table resource, which the sweep passes as `nullptr`. | **Verified**: `--filter '^atom/wavetable_live$'` and `'^atom/wavetable_shape$'` exit 0xC0000005 in ship and check. Separately, one 0xC0000409 exit of `atom/wavetable_pos L8 (saw, 1-frame calls)` in ship; 0 in 48 later runs (L24). | Exclude every atom whose `info.resource` is `FrameTable`, `VoiceTap` or `SignalTap` from the sweep by that field, not a name list. Add the unfiltered run to `runner_smoke`. |
| M4 | `src/views/src/devices/analysis.cpp:307-308` (`SpectrumFeed::gramPush` holds `gramMutex_` across `Spectrogram::push`), `:244, :250, :277, :293` (UI-side reads) | **The spectrogram worker holds the lock the UI thread takes every frame, for the whole analysis of a chunk.** One 4096-frame push takes 13–15 ms. A shown spectrogram therefore stalls the UI frame by several ms. | **Verified** ([`probe_gram.cpp`](MM/link/probe_gram.cpp), two threads, one display, 6 s, about 11 % load): UI wait p99 6.6–8.2 ms, max 8.5–10.0 ms; 25–35 % of frames waited > 4 ms. | Emit columns into a worker-private staging ring and take the lock only to copy finished columns, or use an SPSC / seqlock column ring. |
| M5 | `src/views/src/project_patch/canvas.cpp:1265-1285` | **The embedded canvas reports Delete, Ctrl+G, Ctrl+Shift+G, `0` and `f` as handled even when nothing happened.** Once focus is in a group's panel in the Device view, the Device view never gets Ctrl+G (rack), Ctrl+Shift+G or Delete. Clicking the capsule frame selects the group itself, so Ctrl+G then tries to nest it ("Group: groups do not nest"). | **Verified** (`probe_views` A): Ctrl+G in the embedded canvas → `KeyResult::Widget`, history +0, status "groups do not nest". | Return the result of `groupSelection` / `ungroupSelection` / `deleteSelection`; leave the "group" selection out of a scoped view's `selectedDevices` for Ctrl+G. |
| M6 | `src/document/src/serialize.cpp:2236` (wavetable `readRefs`: finite check only) | **Large finite cycle samples make the output NaN / Inf.** A shared preset or project with a cycle at 3e38 renders 71,430 non-finite samples of 144,000 at the master. From about 1e10 the peak already drops (0.949 → 0.364 → 0.189), so the table build degrades well before overflow. | **Verified**: fuzz case 259 (`extra/amp_3e+38`). | Reject \|x\| > 1e6 (or a similar bound) in `dataProblem`, or normalise in double before the build. Add the case to the reader tests. |
| M7 | `tests/dsp/test_wave_live.cpp:300-370` (the backward test, 6 dB margin); `src/dsp/src/kernels_wave_live.cpp:488-607` (`laneEvents` / `stepAt`) | **Loose end (c): the 5.4 dB backward-vs-forward gap with PW modulated at 2 kHz is mostly the worst-bin metric, not aliasing.** The worst bin (8481 Hz, bin 2895 = 23·k0 − 16·km) is a real PW sideband: the 64× reference has a line there (about −96 dBFS), and both renders overshoot it in the same direction (fwd +1.77e-5, bwd +3.27e-5). The total error is nearly the same both ways: RSS over the line bins −78.9 vs −78.2 dB. In empty bins the backward worst is only 1.1 dB above forward. A small systematic part remains, and it comes from the corrections: the worst-bin gap is +3.9 to +5.6 dB in every variant (5 modulator phases, 3 modulator bins, 2 pitches), and falls to +0.2 dB with corrections off. Hypothesis (code-read): `stepAt` evaluates a kink event's slopes with the controls of the sample after the event, and PW is the only mode whose kink moves with a control. | **Verified** ([`probe_pw_backward.cpp`](MM/engine/probe_pw_backward.cpp) E1–E6); reproduced the test's −95.1 / −89.7 dBFS. | Measure the backward check on the empty-bin floor or the RSS, with about a 2 dB margin; the current metric calls a sideband amplitude error "alias". Optionally interpolate the controls to the event time in `stepAt` and re-measure. Close Q190 item 4 with this explanation. |
| M8 | `src/document/src/serialize.cpp:1938` (`checkList` errors at the list's path) | **Device-list refusals name the list, not the device**: `$.tracks[0].devices: a group member's group is not in its track`, `groups do not nest`. With 15+ devices the user can't find the bad one. | **Verified**: fuzz cases 350–364. | Have `checkList` return the device index and append `[i]`. |
| M9 | `src/views/src/devices/device_panel.cpp:389` ("◌ free"); the group capsule's terminals and closed miniature (`project_patch` group drawing); Wavetable face `os` cell | **Elements that look broken in the goldens.**<br>• The "◌ free" marker renders as "‖ free" (the UI font lacks ◌): `project-patch-free`, `project-patch-group`, `device-free-rack`.<br>• A closed group is two empty rectangles with no names or controls (`project-patch-group`, Bass).<br>• The group's "In" label is half inside the terminal outline and clipped; "Out" overlaps its outline too.<br>• The Wavetable's `os` toggle is a blank cream bar in `meta-wavetable-voices`.<br>• The "Linear" knob-curve glyph is a diagonal inside the knob and reads as a second pointer (`device-knob-curves`). | **Verified** on the goldens (I checked the first three on `project-patch-group.png`). | Use a glyph the font has (or a drawn dashed circle). Draw the members' colour-layer miniatures, or at least their names, in a closed group. Place terminal labels outside the outline. Draw the `os` factor ("×4"). Move the curve glyph below the value as a small boxed icon. |
| M10 | Wire colours: circuit `#78BEFF` for audio, modulation and notes alike; Device view mint for modulation, amber for notes | **Wire colours mean different things in the two views, and in the circuit only the dash separates audio, modulation and note wires.** Dashed nets running 4–8 px apart read as one (`project-patch-notes`). REVIEW lists this as a known gap; with free patching it is now the main legibility issue of the circuit. | **Verified** on the goldens (`#78BEFF` on a band title strip `#26526D`: 4.23:1). | One palette in both views (audio blue, modulation mint, notes amber), and parallel routing channels at least 6 px apart. |
| M11 | `src/app/src/project_link.cpp:1544` (`refreshMetaTables` → `meta::dataHash` under `mutex_`), `:997` | **The UI thread waits on the link mutex while the compile path hashes wavetable data.** 1.94 ms per 256-keyframe hash; on a Reset every instance is hashed under the lock, and it runs on every DeviceParam change of any device, including non-meta ones. The UI's per-frame `deviceAnalysisTap` / `TapVoices::follow` take the same mutex. | Hash cost **Verified** (`probe_link` H); the contention is code-read. | Hash outside the lock; only for meta devices. |
| M12 | `src/app/src/face_voices.cpp` (`FaceVoiceHub::anyFresh`), `src/views/src/meta/tap_voices.cpp:28` | **The face voice readers cost work every presented frame and are never pruned.** Each reader's `follow()` takes `mutex_` twice, scans `metaIdLocked` with string compares and copies the `modulated` vector; deleted instances are polled forever. On a long session with many created and deleted meta devices this grows without bound. | Code-read. | Prune readers whose instance is gone (on the document change), and cache the slot id per reader with a generation check instead of the string scan. |

### Low

| # | Where | What | Fix |
|---|---|---|---|
| L1 | `kernels_frame_table.cpp:71-77`, `kernels_wave_live.cpp:827-831`, `voices.cpp:894+` (`moveLane`) | Frame-table generation and fade state are per voice group, not per lane. A voice compacted into a group that missed publishes gets a spurious 64-sample fade from an older table (memory-safe: `e->prev` is alive). Code-read. | Skip the fade when the group's lane mask was 0 last block. |
| L2 | `voice_tap.cpp:31-49`, `tap_ring.h:55-80` | The seqlocks' data stores after the counter store rely on x86 TSO (no release fence). Correct on x64; formally racy. Code-read. | Add a `std::atomic_thread_fence(release)` (free on x64) or a comment pinning the x64 assumption. |
| L3 | `TapRing` ("exactly one writer"); project link | During a recompile crossfade the old and new images both write the same `TapRing`, so the scope and spectrogram get each block twice: a short time-base glitch on every recompile. Code-read (both sub-reviewers). | Let only the incoming (or outgoing) image write analysis taps during the fade. |
| L4 | `timeline.cpp:112-117` | If `registerReader()` fails (> 4 readers), the timeline reads frame-table entries without QSBR protection, silently. Pre-MM code, but frame-table safety now depends on it. Code-read. | Fail loudly (assert + status) instead of continuing unregistered. |
| L5 | `voices.cpp:903-908` (`moveLane` side-span copy); `test_smooth` (`maxVoiceGroups = 1`) | The ramp side-table copy on voice compaction is the only new RT path with no RtScope test, and no test moves a ramp mid-flight across groups. Code-read. | A `test_smooth` case with several groups and compaction mid-ramp, under RtScope. |
| L6 | `src/document/src/serialize.cpp` `finishDevice` | The reader keeps meta state nothing uses: nested paths on a chain meta device, paths the user patch never calls, a wavetable state on a `meta_phase_acc` call. It is ignored by lowering and saved back. Verified (cases 32, 38, 42–44). | Refuse non-empty paths on `meta:` devices; check user-device paths and types against the parsed calls (or drop orphans with a warning). |
| L7 | `src/document/src/checks.cpp:332`; `validate()` | Group invariants not enforced: `curves` on a Group device (cases 357, 385); `free: true` on a group with In/Out chain links, which sounds different (peak 1.29 vs 0.948, case 328); `chain: true` links between non-members and across tracks (366, 442–444). Verified. | Add curves to the group check; validate chain links (both ends the group or its members, one track). |
| L8 | `json_io.cpp:97` (`renderPath`) | The chain meta device's own state (key `""`) prints as `meta[0]` in error paths. Verified. | Render empty keys as `[""]`. |
| L9 | `edit_ops.h` `connectTravelWire`; `lower.cpp:1646` | The builder accepts a Travel wire from an `out` port, which lowering always marks `Invalid`. Verified. | Refuse it in the builder. |
| L10 | `src/jit` (LLVM) | Under memory pressure renders died with 0xC0000409 after LLVM's "Buffer allocation failed", or 0xC0000005. No `llvm::install_bad_alloc_error_handler` is installed, so a JIT allocation failure kills the DAW with unsaved work. Seen in 7 fuzz renders during the `-Full` run; all 7 render cleanly alone. | Install the handler; fail the compile and keep the previous image. |
| L11 | `tests/document/fixtures` | No committed fixtures for schemas 11–15; the 12→13 (Travel) and 15→16 (Units) rules are tested with inline JSON only. | Commit `schema12` (Travel wires) and `schema15` (Add wires) fixtures. |
| L12 | `docs/TASKS.md:633-641` | Bookkeeping: TMM.4, TMM.7, TMM.9, TMM.10, TMM.11 are still `[ ]` although merged (PROGRESS). TMM.12 is `[~]` with "cost 2.23×", which TMM.12-fix1 fixed (0.96× here); its node budget (41 vs ≤ 20) is the open part. TMM.4's perf criterion fails (H5). | Mark them with their commits and real state; TMM.12 `[!]` on the node budget only (or restate the budget in QUESTIONS: Q178 says the plan's 20 left out 18 terminals). |
| L13 | `stdlib/devices/meta_operator.pqp:41-46` | Loose end (d): the header still quotes TMM.12's costs (13.2 / 19.1 / 44.0 ns vs Operator 8.6). Since TMM.12-fix1: nofb 6.86, sine 10.29, hq 22.82 (this review: 9.34 / 10.13 / 29.6 vs stock-four 10.58 at 14–25 % load). The "most of the difference is the band-limited read" sentence is no longer true for sine tables. | Update the comment with the fix1 row and the sine fast path. (Not edited here: changing a stdlib file triggers stdlib tests and goldens.) |
| L14 | `scene.cpp:194` (Source title limited to `kLaneTitleW`), `circuit_canvas.cpp:1469` (labels that don't fit are hidden) | Loose end (b): at Near zoom below about 0.75, neither the lane window title nor the far "Notes" pill is drawn ("Notes" is about 30 px; the room is (48 − 8) × 0.70 ≈ 28 px). The `project-patch-resize` scene sits in that band. Not a scene bug. Verified by measurement. | Let the title extend to the first bar number it doesn't overlap, or fall back to the pill when it doesn't fit. |
| L15 | `panel_face.cpp` `staticKey`; `circuit_canvas.cpp:345` (`faceKey`) | The panel tile key is `this` + `recordings_`: a new PanelFace at a freed one's address can show the old panel's far tiles until pruning. `faceKey` XORs `layer` into `type<<3`, colliding for layer ≥ 8. Code-read. | A per-instance serial; shift the layer field clear of the type. |
| L16 | `overlay.cpp:587` (`openContextMenu`) | Context menus from a hosted inner tree (PanelFace) are not mapped through `OverlayLayer::anchorRect` as `Dropdown` is; latent (no panel widget opens one yet). Code-read. | Map the anchor like `controls.cpp:972`. |
| L17 | `spectrogram_face.cpp:724-731` | When `p.images()` is null the face keeps its old `pool_` / `tex_` and still emits `addImage(tex_)`: a texture from another renderer. Latent. Code-read. | Skip the picture when `pool != pool_`. |
| L18 | `table_builder.cpp:116, 236` (`forget`); `metaSlotFor` free list | `TableBuilder::forget` leaves the target's queued job, which can later record `last_[target]` for the next instance on that id (feeds H2). The "freed id is reused last" comment is wrong: the free list is used before fresh ids. Code-read. | Drop queued jobs in `forget`; take fresh ids before reusing freed ones, or fix the comment. |
| L19 | `project_link.cpp:1317` (`drainDeferred`) | Pre-MM: waits `5000 × sleep_for(1 ms)`, about 80 s with the default timer resolution, not 5 s. Seen: `flush()` 77.8–80.2 s with no audio thread. Normally avoided in the app. Verified (by accident, `probe_link` C). | Wait on a deadline (`steady_clock`), not an iteration count. |
| L20 | GPU-golden SIGSEGVs in `pq_tests_views` (loose end a) | Not reproduced in about 300 process runs (parallel shards, the eight reported cases × 8 at once, whole executable × 12), half under a crash-catching debugger. No shared GPU handle, static texture cache or thread posting into SDL was found; teardown order is right in the test fixtures; `TexturePool` detaches with its renderer. 3 `bad_alloc` failures happened in one round, all at once, during a memory spike from other processes. Conclusion: most likely the NVIDIA driver under load, unproven. | Run `pq_tests_views` through [`crashcatch`](MM/views/crashcatch.cpp) as its ctest launcher so the next crash names its module; give it a resource lock against the jit / dsp slow shards; optionally share one `GpuContext` per process (about 200 instance / device create-destroy cycles per run today). |
| L21 | Phase accumulator pitch arc | ±4 octaves over 72°: a semitone is 0.75°, so +7 st is a 5° sliver; the arc can't show ordinary transpositions (the readout does the work). | A finer central ±12 st zone with semitone ticks. |
| L22 | Device view chain path (`device-chain`, `device-meta-*`, `device-automation`) | The audio path at the panels' feet shows as isolated amber / green dots, and as vertical bars along a folded device. | Draw a visible 1.5 px line, or drop the joints. |
| L23 | Analysis faces | Sockets sit on the plot (output socket over "−36" and the curve end in `device-meta-spectrum`; I/O over the trace in `device-meta-scope` and `-spectrogram`). | Put sockets on the frame's outer edge. |
| L24 | `pq_bench_dsp` `atom/wavetable_pos L8 (saw, 1-frame calls)` | One 0xC0000409 exit (ship) in the first filtered run; 0 in 48 later runs, and the check build runs it. Not reproduced. | Watch it; run the row under `crashcatch` if it recurs. |

## Acceptance-criteria audit

"Real" means the test or bench measures what the criterion says. Rows marked **No** or **Partly** are claimed in TASKS / PROGRESS but not truly verified, or fail.

| Task | Criterion | Test / measurement | Verified? |
|---|---|---|---|
| TMM.1 | Alias suite ≤ −100 dBFS or ≥ 20 kHz; harmonics within 0.1 dB | `test_frame_table_alias.cpp` (default + slow), re-run here | Real: −127.3 / −127.6 / −130.4 dBFS; harmonic error ≤ 5.4e-4 dB. |
| TMM.1 | Reads L1 ≤ 4 ns, L8 ≤ 30 ns blend, ≤ 20 single | `pq_bench_dsp` ship, this review | Real: 3.04 / 19.4 / 15.4 ns. |
| TMM.1 | Build ≤ 20 ms; with bake ≤ 150 ms | ship bench | Real for TMM.1's bake (10.5 / 70.9 ms). The Wavetable now uses the live-band bake (217 ms), see M1. |
| TMM.2 | 64-sample swap click-free; 10⁶ swaps no UAF; RtScope | `test_jit_frame_table`, swap stress with poisoning | Real (click bound reformulated, Q170). Per-group generation (L1) untested. |
| TMM.2 | Taps: no reader equal within noise | ship: 0.020 ns/sample unwatched | Real. |
| TMM.3 | freq 0 bit-identical; drags compile nothing; rename keeps state | `pq_tests_document`, `pq_tests_meta` | Real. Reader keeps unusable state (L6). |
| TMM.4 | Far pan median / p99 within +0.3 ms with 32 faces | `pq_bench_project_patch` | **Fails** (H5): +0.50 / +0.63 ms (Phase acc), +1.17 / +2.06 ms (Wavetable). |
| TMM.4 | 16 faces × 8 voices ≤ 1 ms paint; idle 0 % | ship bench, views perf tier | Real: 0.518 ms; idle "no frames, no timers". |
| TMM.5 | Keyframe edit audible ≤ 30 ms; ≤ 160 ms with bake | `test_meta_wavetable.cpp:658` (`perf`) | **Partly**: 30 ms real; 160 ms **fails** for the full table (296–347 ms) and the test times the preview (M1). |
| TMM.5 | Module alias suite | `pq_tests_playback`, re-run | Real: −129.5 dBFS (48 kHz subset). |
| TMM.6 | Region vs double reference; aliasing; interp == JIT | `test_oversample` | Real (not re-measured beyond the suite). |
| TMM.7 | Live layer ≤ 0.3 ms; 16 animating faces ≤ 1.5 ms | views `perf` | Real at low load: 0.072 / 1.19 ms. Fails under load (TMM.16's note); fine idle. |
| TMM.8 | Audio-rate modulation ≤ −100 dBFS | `test_wave_live.cpp:294` | **No, accepted** (Q176): bend −89.7, mirror −110.1, PW −86.6, fold −91.4, sync −67.4 at ×8; reproduced exactly. The 64× reference folds as much, so this is the modulated signal's own bandwidth. |
| TMM.8 | Shape mode ≤ −100 dBFS at amplitude ≤ 1 | `test_wave_live.cpp:651` | **No, accepted** (Q176): saw −75.5, square −90.1, drawn −94.2. |
| TMM.9 | Lossless draw ↔ harmonics; 512 bars ≤ 0.2 ms | `test_meta_wavetable_edit` | Real: 0.066 ms. |
| TMM.10 | 31edo 8-voice chord ≤ 1e-4 ¢ | `pq_tests_playback` | Real (8.58e-5 ¢ recorded; suite passed). |
| TMM.11 | Bit-identical to msenv | `pq_tests_meta` | Real. |
| TMM.12 | ≤ 1.2× stock Operator | `pq_bench_render` ship | Real since fix1: 0.96× here (load 14–25 %, indicative). |
| TMM.12 | ≤ 20 visible nodes | node-count test | **Fails** (41; Q178), still open (L12). |
| TMM.18 | Backward = time-reversed forward | `test_wave_live.cpp:300` | Real (−146 to −157 dB). The 6 dB alias margin hides a correction residual (M7). |
| TX.smooth-fix1 | At rest identical to the ramp-free program | `test_smooth`, ship A/B with A/A | Real: JIT 1.008 (A/A 0.997), interp 1.003 (A/A 1.003). Moving ramps across compaction untested (L5). |
| TX.patching / TX.knob-curves | Wires add in units / before the curve | document probes | Real: 20 / 20 laws. One drawable wire class fails to compile (H3). |
| TX.groups (+fix1) | Series = group; ungroup exact | render diffs, probes | Real: 12 / 12 round trips, byte-identical renders. |
| TX.spectrum / scope / spectrogram | Hidden taps cost nothing; analysis off the UI thread | ship benches | Taps real (1.3 ns/block hidden). Off the UI thread: **Partly**, the spectrogram's lock stalls the UI (M4). |
| TX.resize / TX.free-rack / TX.lane-edit | Gestures, idle, goldens | views tests | Real. Embedded panel key routing (M5) and the UAF (H1) untested. |

## Benchmarks

Ship preset (`/GL` + `/LTCG`), this commit, main thread pinned to core 1. Load per row as sampled; the desktop was in use (OBS, Voicemeeter), so every row is **indicative**.

| Row | RESULTS.md (task) | This review (load) | Criterion | Verdict |
|---|---|---|---|---|
| frametable/read L1 blend / single / sine | 2.94 / 2.25 / — (TMM.1) | 3.04 / 2.29 / 0.43 ns/sample (12–20 %) | ≤ 4 | pass |
| frametable/read L8 blend / single / sine | 18.8 / 15.6 / 3.00 | 19.4 / 15.4 / 3.11 ns/frame | ≤ 30 / ≤ 20 | pass |
| atom/wavetable_pos L1 / L8 | 3.19 / 19.7 | 3.45 / 19.9 | — | unchanged |
| wavetable_pos crossfade L1 / L8 | 6.59 / 38.8 | 6.98 / 39.8 | — | unchanged |
| wavetable_pos L8 sine / sine 1-frame / saw 1-frame | 3.29 / 28.2 / 47.0 | 3.43 / 29.2 / 48.7 | — | unchanged |
| voice_tap unwatched / watched L8 | 0.020 / 0.546 | 0.020 / 0.591 | equal within noise | pass |
| param_ramp L1 / L8, signal_tap L1 / L8 | — | 0.053 / 0.139, 0.018 / 0.047 ns | — | — |
| frametable/build 64 / + bake / + bake3 | 9.79 / 63.5 / 137 ms | 10.5 / 70.9 / 166 ms | ≤ 20 / ≤ 150 | pass |
| frametable/build + live-band bake / bake3 | 178 / 298 ms (TMM.8-fix) | **217 / 417 ms** | TMM.5 ≤ 160 end to end | **fails** (M1) |
| wavelive/region os 2 / 4 / 8 / 16 | 562 / 720 / 1005 / 1537 ns (TMM.8-fix) | 554–569 / 721–736 / 1048 / 1629–1698 ns (two runs at 7–8 %; a first run at 12–20 % gave 609 / 821 / 1239 / 1975) | — | os 2–8 within noise; os 16 +6–10 % (TMM.18's backward support) |
| operator meta-sine / stock-four jit x64 | 10.29 / 8.97 (fix1) | 10.13 / 10.58 ns/voice-sample (0.96×) (14–25 %) | ≤ 1.2× | pass |
| operator meta-nofb / add / hq | 6.86 / 8.63 / 22.82 | 9.34 / 11.02 / 29.6 | — | noisy run (stock-four itself +18 %) |
| operator stock-full ramps A/B (A/A) jit / interp | 1.005 (0.992–0.995) / 1.008 | **1.008 (0.997) / 1.003 (1.003)** | ≈ 1 | pass |
| device automation 16x4 held / 16x0 | 0.95–0.99 (fix1) | 59.4 / 63.8 µs = 0.93; 42.7 / 42.4 µs = 1.01 (30–50 %) | ≈ 1 | pass (loose end f) |
| mixer/64x8 silent jit | 3400 ns/block | 3359–3380 ns | — | unchanged |
| meta_faces.pan far, faces vs plain | +0.16 / +0.18 ms (TMM.4) | **0.525 / p99 0.674 vs 0.022 / 0.039 ms** (6–9 %) | ≤ +0.3 ms | **fails** (H5) |
| wavetable_faces.pan far, faces vs plain | 0.90 vs 0.017 ms (TMM.7) | **1.188 / p99 2.085 vs 0.019 / 0.030 ms** | ≤ +0.3 ms | **fails** (H5) |
| meta_faces / wavetable_faces pan near | +0.012 ms | +0.025 / +0.062 ms | ≤ +0.3 | pass |
| meta_faces 16 × 8 voices full paint / animation frame | 0.349 ms | 0.518 / 0.638 ms | ≤ 1 ms | pass |
| project_patch.pan far, panels vs plain | 0.125 vs 0.032 ms | 0.125 vs 0.032 ms | — | unchanged |
| project_patch first show, 158 panels | 6.1 ms | 6.8 ms | — | — |
| lane_edit paint / selection frame | 212 µs | 214 / 222 µs | — | unchanged |
| device.signal_tap hidden / watched | 1.14 / 55.5 ns/block | 1.31 / 51.6 | — | unchanged |
| device.spectrogram per column / 4 faces frame | 168 µs / 1.82 ms | 169 µs / 1.76 ms | — | unchanged |
| device.4 scopes per frame | 1.61 ms | 0.81 ms | — | — |

Reading: the audio rows reproduce within load noise. The two face pan rows are the exception: they are far above their criterion and above what TMM.4 / TMM.7 recorded.

## Loose ends assigned to this review

| | Question | Answer |
|---|---|---|
| a | GPU-golden SIGSEGVs spreading beyond the known flake list | Not reproduced in about 300 process runs (debugger-wrapped and parallel), and no pqdaw lifetime bug was found in TexturePool, face tiles, panel recordings or teardown. Most likely driver + load, unproven. One real lifetime bug *was* found in the same area (H1), but it doesn't explain these crashes. Recommendation in L20. |
| b | No "Notes" title in the `project-patch-resize` lane windows | A label-rule gap at Near zoom below about 0.75, not the resize code (L14). |
| c | TMM.18's 5.4 dB backward PW gap | Mostly the worst-bin metric landing on a real sideband; a 1–2 dB residual from the corrections' event-time controls (M7). |
| d | `meta_operator.pqp` header costs | Stale; numbers in L13. |
| e | TMM.5's 160 ms bake criterion | Missed for the full table (296–347 ms); the test times the preview (M1). |
| f | 16x4 held / 16x0 after TX.smooth-fix1 | 0.93 and 1.01 in two runs at 30–50 % load: no held-automation overhead. TX.smooth's 1.30 was load. Closed. |

## UI and aesthetic

I viewed every MM / TX golden: the Phase accumulator (z0.05–z3, chord), Envelope, VCA and Wavetable series (each zoom at 1× and 2×; `-poly`, `-harmonics`, `-draw`, `-voices`), `meta-operator-device` / `-circuit`, `device-meta-{chain,envelope,vca,spectrum,scope,spectrogram}`, `device-free-rack` (+ collapsed), `device-group-panel`, `device-knob-curves`, `device-eq-eight`, `device-auto-filter`, `device-automation`, `device-chain`, `device-wiring`, `audio-rack`, `project-meta-*`, `project-patch-{band,free,group,lane-edit,lanes,notes,overview,panels,resize,zoom}`, `project-circuit-device`, `browser-panel`, and PQ Meter's own `spectrum_both_check.png` / `osc_after_noise.png` for reference. I did not see hover, drags, animation or the running app.

**What works.**
- **The faces read as one family**: dark body, amber values, cream handles, neon-blue signal, glowing voice dots. Crisp at 2× and z2–3.
- **The VCA matches the user's sketch** element for element (✕, pie wedge from 12 o'clock, min—max, curve box, preset chips). The Envelope reads instantly.
- **Polyphony is lovely**: per-voice dots and dotted bars on the Wavetable, frequency colours and orbits on the Phase accumulator chord.
- **Spectrum and Spectrogram carry PQ Meter's look** (Ice / Magma / Inferno ramps, glow, peak line, labelled pads). EQ Eight and Auto Filter now show a live spectrum, and the violet "Measured" curve is distinct.
- **Resize** relays out at the new size (Wavetable at 1.5× / 0.6×, Auto Filter at 1.25×) instead of stretching.
- **Panels as circuit blocks** make the project view read like the Device view.

**Ranked changes** (most user impact first):
1. **Cables over controls and text, in both views** (H6). Route around panels, knock text out under wires, dim inactive cables in the Device view, badge-and-hover when the rack is collapsed.
2. **One wire palette** (M10): audio blue, modulation mint, notes amber in both views; parallel routing channels ≥ 6 px apart.
3. **Titles that collide, vanish or attach to the wrong thing.**
   - `project-patch-resize`: no "Notes" title (L14).
   - `project-patch-group`: "Notes" jammed against the ruler's "2" and the left border.
   - `project-patch-overview`, `project-circuit-device`, `project-patch-zoom`: the "Notes" pill covers the band titles "Keys", "Pad", "Mod".
   - `project-meta-envelope`: names are placed by proximity and land under the wrong node ("wt1" under a ✕ node, "env1" under Σ, "acc1" under "voice.velocity", 30 px above its face).
   
   Put the window title in its own title row with a 4 px inset; hide or offset the pill below the band title; attach a face's name to the face.
4. **Broken-looking elements** (M9): the "‖ free" marker, the empty closed group, the clipped "In", the blank `os` cell, the Linear curve glyph that reads as a pointer.
5. **Contrast failures** (WCAG, sampled):

| Element (golden) | Colours | Ratio |
|---|---|---|
| Scope readout "220.0 Hz 2 cyc …" (`device-meta-scope` / `project-meta-scope`) | `#556165` on `#0F2026` | 2.62 / 2.27:1 |
| Spectrogram chips MID / LOG / MAGMA (hat streaks run through them; the keyboard covers "MID" in Inferno) | `#5B5B5D` on translucent `#0A0A0C` | 2.92:1 |
| Group header "3 devices" | `#7D8C8C` on `#2C4758` | 2.79:1 |
| Lane title "Volume" (`project-patch-lane-edit`) | `#F85A5F` on `#3D4F55` | 2.70:1 |
| Unselected Wavetable glyphs (fold, sync, draw tools), the VCA ✕ | `#4A5D63` on `#0C1A1F` | 2.57:1 (< 3:1 for non-text) |
| VCA min—max rule | | 1.69:1 |
| Face titles acc1 / wt1 / env1 | `#82918F` on `#1B2D33` | 4.35:1 (borderline) |

   Give the spectrogram chips the solid `#0B1116` backing the Scope and Spectrum chips use, and start them right of the keyboard. Lift the scope readout to about `#8A9699`, glyph ink to about `#6E8086`, and the group-header grey. Put "Volume" on a dark pad.
6. **Sockets on the plots** of the analysers (L23).
7. **Wavetable face.**
   - The crossfade gap glyph is ✕, which reads as "delete" and clashes with the VCA's ✕ ("multiply"); use a crossfade curve.
   - The "~" gap glyph lands on the output lead (`-harmonics`, `-draw`).
   - At the Device view's 0.85 scale (`device-meta-chain`) the toolbar icons are about 8 px; let the face scale up to the panel width.
   - The 64-frame stack turns to grey texture at 1×; thin it to about 24 lines at small scales.
8. **Phase accumulator.**
   - The pitch arc can't show ordinary transpositions (L21).
   - Input leads end on the rim, not on the arcs they drive.
   - Every chain scene shows red 0 Hz synthetic voices, which reads as an error: render goldens with real note frequencies.
   - It is the only face without a body frame in the Device view, and it has no title bar in the free rack.
9. **Groups and the free rack.**
   - The group is a rounded rectangle, not the capsule the user named.
   - The Device-view mini-circuit (`device-group-panel`) draws faces at about 0.35 scale, illegible, under 50 px of empty band: fit the contents by default.
   - `device-free-rack` leaves the left 300 px empty: left-align it.
10. **Far zoom.** At z0.05–0.25 the canvas's port labels ("note", "y", "ph", "x") are bigger than the face itself; scale them with the zoom or hide them below the face's size. At fit zoom (`project-meta-faces`) stock-panel text is about 6 px at 3.87:1. "Legible at every zoom" honestly ends around z0.5.
11. **Envelope (minor).** A voice dot on the peak breakpoint turns it blue, so it reads as a voice. The body frame has about 50 px of dead margin. The "done" socket is unlabelled grey.

**Against what the user asked for (SPEC §15):**

| Request | Result |
|---|---|
| Legible and interactive at every zoom | **Partly.** Everything draws at every zoom; below about z0.5 the faces are miniatures and port labels dominate. |
| SeptaBee-minimal / Live Wavetable look | **Partly.** The Phase accumulator, Envelope and VCA are minimal; the Wavetable's stack, highlight and dots match Live, but the stack is texture at 1× and the face is busy in the Device view. |
| Phase accumulator pitch slider (right side, amplitude above) | **Partly.** Placement as asked; the arc can't show small moves. |
| VCA after the sketch | **Yes, mostly.** Every element is there; the ✕ (2.57:1) and the range rule (1.69:1) are too faint. |
| Free patching | **Partly.** Placement and any-to-any wiring work; wires cross labels and faces, and share colour with audio. |
| Analysis modules in PQ Meter's style | **Yes** for Spectrum and Spectrogram; **partly** for the Scope (2.6:1 readouts, sockets over the trace). |
| Resize | **Yes.** |
| Groups: capsules, In / Out terminals, mini-circuit panel | **Partly.** All exist; "In" clipped, closed group empty, panel illegible at its default fit, not a capsule. |
| Free-device rack | **Partly.** Works and matches the circuit's free row; cables unreadable, "free" glyph broken. |
| Knob curves | **Partly.** Works; the Linear glyph reads as a second pointer. |

## Boundary run results

| Run | Result |
|---|---|
| `./scripts/build.ps1 -Full` (check; parallel 12 / ctest 8) | **0 warnings**; build 88.9 s; tests 415 s: **87/88 passed**. `pq_tests_meta` failed `test_wavetable.cpp:386` under load (M2); re-run alone 3/3 pass. No GPU SIGSEGV. |
| `./scripts/build.ps1 -Preset ship -Target pq_bench_dsp,pq_bench_render,pq_bench_project,pq_bench_device_panels,pq_bench_project_patch,pq_bench_playback -NoTests` | **0 warnings**, all six linked (no LTCG internal compiler error); 371 s. |
| Alias suites re-run (check) | Frame table: −127.3 / −127.6 / −130.4 dBFS, backwards −127.0 / −127.6 / −130.2. Live-band static −134.1. Audio-rate proof identical to TMM.8 / TMM.18 records. Slow modulation ≤ −120.9. Shape mode as recorded. Module output −129.5. All cases passed. |
| Views perf tier (faces, check, 9–10 % load) | Envelope 16 faces 0.34 ms; Phase acc 0.40; VCA 0.46; Wavetable 1.19 (≤ 1.5); harmonic rows 0.066 ms. 6/6 passed. |
| Sub-reviewer runs | `pq_tests_views` about 300 process runs (0 SIGSEGV); 794 fuzzed renders; probes as listed above. |

The whole-suite pass hides H1–H4 because no test destroys an embedded panel during its own key handling, no test opens a second project with a Wavetable at the same device id, no wire-state test draws a feedback wire into a voice-run head, and no test times a structural compile while the table builder is busy.
