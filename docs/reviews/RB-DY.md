# RB + DY review (wire isolation, autosave / recovery, export, freeze, sidechain, Compressor, Limiter, MIDI files)

Reviewer: independent audit agent, 2026-11-13, `main` at `f1e6a932`, range `f699f2e5..f1e6a932` (77 commits, ~22.7k lines).

## What I ran
Nothing was built or run (the orchestrator's `-Full` was using the machine). Everything below is **code-read**. Each finding
was traced in the source at `f1e6a932` and is cited by file:line. Four parallel deep reads were done: MIDI files; the
background services; the dynamics DSP; lowering / link / export / engine / views. I re-checked every C/H claim and most M
claims myself. I viewed all 7 new goldens and compared them with `stock-saturator-panel.png` and `eq-default.png`.

## Summary verdict
RB and DY are solid. The two parts you would most expect to break hold up: the Limiter's ceiling proof and the
cut-wire retry loop. Both are correct, bounded and tested. The weak spots are around the edges of the background
services: what happens on disk after Recover, on a sample-rate change, and when a frozen track depends on a sum.
There is one CPU regression that matters by the project's own rule: the new dynamics modules never let a track sleep.
**Fix the 4 High items before M7.**

Counts: **C 0 · H 4 · M 11 · L 14**.

---

## High

**H1 — Recover deletes the only on-disk copy of the recovered document.**
- **Where:** `src/views/src/view_context.cpp:549` (`dropStaleSession(s)` inside `recoverAutosave`), `:470-485`.
- **Defect:** right after loading the dead session's autosave, the autosave is deleted. If no takes are left, the
  whole session folder goes. The recovered document is dirty, and this session's Autosaver writes only after
  10 s quiet / 60 s (`src/app/src/autosave.cpp:152`).
- **Scenario:** the project that crashed pqdaw (a lowering / JIT / plugin crash) crashes again on the compile that
  Recover starts, within a second or two. Nothing on disk holds the recovered edits any more.
- **Fix:** move (rename) the dead `autosave/` into this session's autosave folder instead of deleting it. Or keep it until
  this session's first successful autosave write or a Save.

**H2 — The freeze render file name does not include the sample rate.**
- **Where:** `src/app/src/freeze_service.cpp:645-646` (`<id>-<hex(content)>.wav`), `:330-331` (`key.rate` is separate
  from `key.content`), `:566-570` (`setRate` re-plans).
- **Defect:** after an audio-device rate change, `lookupCache` misses on the rate, and the re-render targets the same path
  as the render that is still published.
- **Scenario:** `fs::rename(part, wav)` (`:383`) goes onto a file the sample pool holds. On Windows it either fails (the
  track shows Failed and plays live, and every re-plan picks the same name and fails again), or it replaces the file
  while the pool keeps the old decode under the same path (`takeResults` sees `old->path == new path`, `:555`, so
  nothing is reloaded). No test changes the rate on a frozen track.
- **Fix:** put the rate in the file name (or hash it into `content`). Add a rate-change test.

**H3 — A frozen track fed from a sum turns every edit into a UI-thread project serialisation, a recompile to live,
and a full re-render.**
- **Where:** `src/app/src/freeze_service.cpp:229` (`sumTrack` → `whole`), `:245-253` (the whole project as the render
  document, own mixer included), `:328` (`serializeProject`), `:449-451`; the call per document change is at
  `src/app/src/project_link.cpp:1118`.
- **Why it is easy to hit:** the sidechain picker offers group and return tracks as "Pre FX" sources
  (`src/document/src/edit_cables.cpp:43-46`), so "duck the frozen bass from the Drums group" lands in this fallback.
- **Scenario:** in that project, any edit (a fader drag on any track, a note drag) does three things:
  - It changes the key, so `file->current = false`, the frozen slot drops, and the track recompiles live.
  - `planFreeze` re-serialises the whole project on the UI thread under `mutex_` (about 45 ms per 100k notes) for each
    drag step.
  - A full-song + 30 s tail render is queued 0.25 s later.
  The UI hitches and the CPU saving of freezing is gone while mixing. PROGRESS (FZ.2) already notes that this fallback
  is untested.
- **Fix:**
  - Key a sum source by what reaches F (the sum's own subtree minus F, or refuse the sum source while F is frozen).
  - Never serialise on the UI thread: plan on the worker, and keep the old render playing until the new one is
    ready (no live flip for a key move caused by an unrelated track).

**H4 — The Compressor and the Limiter never sleep, and keep their track's whole region awake.**
- **Where:**
  - `src/dsp/src/atoms.cpp:325`: `FastGainToDb` falls to `default: return {}`, so it has no silence rule.
  - `src/document/src/modules.cpp:1750` (Compressor: `fast_gain_to_db(max(level, 1e-6))`) and `:1889` (Limiter `r`).
  - The region merge is in `src/graph/src/schedule.cpp` `openRegion`.
- **Defect:** the input of every downstream `follow` / `window` is never Z, so the region holding them is not sleepable.
  The Compressor's dry/wet `gain_add` (g − 1) is not Z either.
- **Scenario:** a muted or empty track with a Compressor, or a Limiter on a bus that goes silent, runs its detector,
  followers and windows, plus every op merged into the same region, forever. That is about 4–12 ns per stereo sample
  per instance (bench rows), and the instruments and effects in the same region add to it.
- **Why the tests miss it:** the module tests use `sleepAfterMs = 0`, and no test checks that a silent dynamics track
  goes to sleep.
- **Fix (general, not a Compressor special case):**
  - Give `fast_gain_to_db` / `fast_db_to_gain` silence rules.
  - Classify an op whose inputs are only Z or K as K.
  - Treat `follow` / `window` of a K input as quiescent once their state is within ε of it.
  - Add a sleep test per module.

## Medium

**M1 — The EX.x-fix-settle sleep change keeps every segment of a stage awake while any ramp in that stage moves.**
- **Where:** `src/engine/src/image_host.cpp:1314` (`changed = anyRamps_ && rampOn_[stage] != 0`).
- **Defect:** the test is per stage, not per segment.
- **Scenario:** an automated param (or a tempo ramp feeding `rampWatch_`) keeps every otherwise-silent sleepable
  segment of that stage awake for the whole automated passage. Merged sidechain units share stages, so this
  spreads.
- **Fix:** record which segments own each ramp's params (the `segParams_` list already exists) and test only those.

**M2 — Export temps, freeze renders and render-edit WAVs are plain RIFF (no RF64), so long renders fail.**
- **Where:** `src/media/src/wav_file.cpp:229-230`; the writers are at `src/app/src/export_render.cpp:128`,
  `freeze_service.cpp:186` and `render_edits.cpp:84`. Only the recorder calls `enableRf64()` (`recording.cpp:212`).
- **Scenario:** the pass-1 temp is always 32f, so the 4 GiB limit is about 46 min of stereo at 192 kHz, 93 min at
  96 kHz and 3.1 h at 48 kHz. Past that, the export fails mid-render with "WAV file would exceed 4 GiB", including a
  16-bit export. Freeze renders have the same limit.
- **Fix:** call `enableRf64()` on all three writers (the decoder already reads RF64 takes).

**M3 — Limiter Link M-S hard-clips by up to 6 dB.**
- **Where:** `src/document/include/pq/document/modules.h:377-380`; Q266(a) is still open.
- **Scenario:** a hard-panned peak (L = 4c, R = 0) gives M = S = 2c. Each is held at c, so the output is L' = 2c, and the
  "safety" clip does 6 dB of square-wave clipping continuously. The M-S test checks only `peak ≤ c`.
- **Fix:** hold M and S each under c/2, or cap S's gain at M's (Q266 option c). This is the user's call; it should not
  stay open into M7.

**M4 — A single ±inf (or huge) sample mutes the Limiter or Compressor for seconds to minutes.**
- **Where:** `src/dsp/src/kernels_dynamics.cpp:27-32` (`clean` maps inf to FLT_MAX); `r` and GR are unbounded
  (`modules.cpp:1889`, Compressor GR).
- **Scenario:** with the Limiter at defaults (Auto, 300 ms), an inf turns into about 3.4e38 dB of reduction and takes
  about 100 s to release. A 1e30 sample takes about 8 s. The NaN sample itself comes out as −c (`clip` maps NaN to its
  low bound).
- **Fix:** `min(r, 200 dB)` / `min(GR, 200 dB)` before the follower (one op on the control path), and map NaN to 0 in
  the final clip.

**M5 — Untitled sessions that hold only a freeze cache are never offered and never cleaned; superseded renders pile up.**
- **Where:** `src/app/src/recording.cpp:90-107` (skips `cache/` and drops a session with no takes and no autosave),
  `view_context.cpp:478-485` (`dropStaleSession` counts cache files as "left"), and `freeze_service.cpp:555-557`
  (one best-effort delete, with no retry or sweep).
- **Scenario:** a crashed untitled session with frozen tracks leaves hundreds of MB of 32f WAVs in `Unsaved/` forever.
  The disk already filled once (2026-10-26).
- **Fix:**
  - Ignore `cache/` in `dropStaleSession`.
  - Sweep dead sessions that hold only a cache.
  - At sync or load, delete WAVs in `cache/freeze` that no `.json` names and no entry uses.

**M6 — Save As copies `cache/freeze` synchronously on the UI thread, including `.part` files and superseded renders.**
- **Where:** `src/views/src/view_context.cpp:662-669`.
- **Scenario:** a project with a few frozen tracks (hundreds of MB of 32f) freezes the UI for seconds on Save As. The
  stray files are then carried into the new project for good.
- **Fix:** copy only the WAVs the current `.json` files name, on a worker; a miss simply re-renders.

**M7 — MIDI note-off pairing and program lookup are quadratic on hostile files, and run on the UI thread.**
- **Where:**
  - `src/document/src/midi_file.cpp:583-587`: `noteOff` walks the shared (channel, key) list for its own track.
  - `:527-531` and `:745-752`: linear `programs_` scans per note-on and per MTS message.
  - The callers run on the UI thread: `file_drop.cpp:285`, `browser_panel.cpp:888`, `library_load.cpp:266`.
- **Scenario:** a ~3 MB format-1 file (track 0: 500k C4 note-ons; track 1: 500k C4 note-offs) is about 2.5e11 link
  visits, so the app hangs on the drop. The 600-mutation fuzz test cannot reach this, because it is about size, not
  bytes.
- **Fix:** one FIFO per (track, channel, key), and a cached program index per channel.

**M8 — MIDI import: a 1 ¢ bend sends a note down a different pitch path than its unbent neighbours.**
- **Where:** `src/document/src/midi_import.cpp:240-255` (`plain = mn.cents == 0.0`).
- **Defect:** plain keys go through the track's `.kbm` / InputPitchMap. Any bent or MTS key is placed at its absolute
  12-TET pitch.
- **Scenario:** on a 31edo track whose `.kbm` maps one key per degree, key 72 lands about 465 ¢ above key 60. The same
  key bent +1 ¢ lands about 1201 ¢ above. This is a special case on an exact float equality, against "general
  mechanisms".
- **Fix:** decide per file. Absolute pitch if the file carries any bend, MTS or RPN 0, otherwise keys. Or always map the
  key and add the cents. Amend Q263.

**M9 — Compressor laws are mostly untested by value.**
- **Where:** `tests/app/test_compressor_module.cpp:99-190`, `:257-294`.
- **Defect:** Expand (slope R − 1, the 70 dB cap), RMS, the Lin envelope, the Auto two-rate release and Makeup are
  covered only by atom counts.
- **What it would miss:** a sign error in the Expand `under` branch, or a swapped mux input, passes every test. This is
  below the standard of the Limiter tests, which check the release law frame by frame.
- **Fix:** add static-curve cases for Expand and RMS, a Lin step response, and an Auto release check.

**M10 — The autosave offer compares two clocks; `originMtime` is written but never read.**
- **Where:** `src/app/src/autosave.cpp:75-79` (`autosaveSupersedesOrigin`: origin mtime ≤ `savedAt`); the field is
  written at `:253`.
- **Scenario:** a project on a NAS (or under any clock skew) whose file mtime is ahead of the local clock: after a crash,
  the newer autosave is never offered. The edits are lost silently, and the folder is never cleaned either (Q249).
- **Fix:** offer the autosave when the origin's current mtime ≤ the recorded `originMtime`.

**M11 — QUESTIONS hygiene.**
- Q246, Q250, Q252, Q254 and Q256 have no `Status:` line.
- Q258's heading repeats "FZ.4 · FZ.4".
- Q248–Q269 are all still "open", including design calls with user-visible effect: Q266 (M-S clipping), Q261 (the Mode
  chips) and Q263 (M8).
- **Fix:** add the missing lines, and put Q261 / Q263 / Q266 to the user before M7.

## Low

- **L1 — Autosave failure is not retried.** `autosave.cpp:162-163`: `written_` is set when the job is queued, not when
  the write succeeds. A failed write is not retried until the next edit, so the last edits before an idle crash have no
  autosave.
- **L2 — `dropStaleSession` fails toward deletion.** `view_context.cpp:476-482`: one `ec` serves both the iterator and
  `is_directory` / `is_regular_file`. An error there ends the loop with `left == false`, and the whole session is
  discarded.
- **L3 — Export can throw on its worker thread.** `export_render.cpp:360, 486, 555` build error messages with
  `fs::path::string()`, which throws on MSVC for characters outside the ANSI code page (stems are named after
  tracks). On the export worker that is `std::terminate`. Use `u8str`.
- **L4 — Export silently overwrites.** `export_render.cpp:552-557` and the export window: an existing file of the same
  name is replaced without asking. If a later rename fails (the file is open in a player), earlier stems have already
  replaced the previous export.
- **L5 — Quitting cancels running work without a warning.** `shell_app.cpp`: quitting during an export or a render edit
  cancels it (`confirmQuit` only asks about unsaved changes).
- **L6 — Render-edit output folder after a mid-render Save As.** `render_edits.cpp:331` vs `:484`: the folder is fixed at
  `start()`, but the ref is made against the project folder at `poll()`. A Bounce on an untitled project then Save As
  mid-render gives an absolute ref into the Unsaved folder, made after `savedUnsavedRefs_` was computed.
- **L7 — Freeze key gaps.** The key ignores sample file contents (overwriting a WAV in place keeps the old render) and
  has no DSP/atom version (an app update keeps old renders until `cache/` is deleted). Add an engine build hash.
- **L8 — MIDI export: file tick 0 is the earliest clip start, not a bar line.** `midi_export.cpp:132-142`: a clip that
  starts on beat 2 shifts every later signature change off the file's bar grid. All tests use bar-aligned clips.
- **L9 — MIDI odds and ends.**
  - `midi_import.h:63-64`: the "no overflow" claim on `midiPpqTicks` is false (`q * num` can overflow for ppq 1 with
    max deltas).
  - `midi_file.cpp:137-144`: the writer's bridging `FF 01 00` meta keeps running status.
  - SMPTE files are converted at a fixed 120 BPM instead of the project's tempo map.
  - The test "errors — … an audio clip" builds no audio clip (`test_midi_export.cpp:323-330`).
- **L10 — `window` mean can go slightly negative for non-negative input.** `kernels_dynamics.cpp:278, 286`: rounding
  residue. `sqrt(window(x², t, mean))` in a user patch then returns NaN for up to L frames after a ~160 dB drop. Clamp
  at 0 for mean.
- **L11 — `windowStateSmall` rescans from index 0 every block while a candidate.** `kernels_dynamics.cpp:596-608`:
  this is O(L) per block, about 24k floats per 64-frame block for a 1 s window. Keep a "frames since last > ε"
  counter, as the delay lines do.
- **L12 — Docs drift.**
  - TASKS DY.2 says "units are not merged". In fact `partitionProject` merges sidechain tracks unless two instruments are
    involved (`src/graph/src/project.cpp:280-297`), and `lower.h` says they merge.
  - ARCHITECTURE has two near-duplicate "Project graph" bullets (lines 524 and 525; pre-existing, but DY.2 edited both
    instead of merging them).
  - `modules.h:372` claims fast_ errors ≤ 1e-6; `units.h` says about 7e-6 combined.
- **L13 — Compressor panel UX** (`compressor-panel.png`).
  - The two meters next to the curve have no labels. The Limiter labels "GR / Out"; the Compressor has a bare radio
    "GR" jack under them, which reads like a switch.
  - "Auto" (a Release option) floats in the Knee column.
  - Mode mixes the detector and the law (Peak / RMS / Expand), so RMS expansion is impossible (Q261).
  - The meters in both modules have tick marks but no dB numbers.
- **L14 — Toggle wording and frozen look.**
  - Export Audio, Compressor: an off toggle reads "On" in grey, while "On" in yellow means on. This is the existing
    Preferences convention, but on a dense dialog "Include Return and Master Effects: On" (grey) reads as on. Use a
    check / switch glyph or "Off".
  - `tracks-frozen.png`: the frozen tint on Chords (Piano) is barely visible next to Bass's. The red snowflake (failed
    render) has no explanation in the header.

## Checked and fine (one line each)
- **WI.1 / WI.2 cut loop:**
  - Retries are bounded (`kCutRounds` 8, and only while a new wire is found).
  - A failed `update()` is all-or-nothing.
  - Cuts are dropped on edits of either end's track and on reset.
  - A clean project never sets `cutWires` (`typeRetries == 0` tested).
  - Render ≡ the project without the wires, bit for bit.
- **WI.3:** declared-type check only. Undeclared ends are never refused.
- **FZ.1 frozen lowering:**
  - `frozenKeep` follows main-path links (they are wires), so kept blocks keep their whole upstream.
  - Own mixer cables are dropped (not doubled); cables from other tracks into the mixer stay live.
  - The D4 apply rule is one content compare at the end of `apply()`.
- **DY.2 sidechain:** `crossSidechain` is target-based; params stay Modulation; meta inputs get the mid.
- **Export core:**
  - PDC trim is per target (master: `outputLatency`; stems: `busLatency`).
  - The loop wrap keeps the second lap.
  - Solo passes on one link are settled first (EX.x-fix-settle) and tested equal to fresh links.
  - The shaped dither NTF is 1 − 1.623z⁻¹ + 0.982z⁻² − 0.109z⁻³ (−12 dB at DC, +11.4 dB at Nyquist), as intended.
  - Cancel removes temps and parts.
- **Bus capture:** one memcpy per captured channel per image block into preallocated buffers; zeros while asleep;
  RtScope tested.
- **Limiter ceiling proof** (modules.h:374-379), checked independently:
  - With det 7 frames behind and m = n − L + 1, every max window k ∈ [m, n] holds r[m].
  - The instant attack keeps g ≥ h, so the mean b[n] ≥ r[m].
  - Window L and the latency use the same `round(time·sr)`.
  - Tests check the ceiling with the clip bypassed.
- **truepeak:** the BS.1770-4 coefficients, the x[n−6] alignment and Q264's one-sided bound are correct.
- **follow, window:**
  - `follow` handles T ≤ 0, NaN and inf.
  - The van Herk–Gil-Werman max/min is correct.
  - The mean's drift is reset from the ring every L frames.
- **Realtime safety:** no allocation, locks, exceptions or unbounded loops in `kernels_dynamics.cpp`, the capture path
  or the sleep change; RtScope tests exist for the three atoms and both modules.
- **Services:**
  - Autosave and the cache `.json` are atomic (tmp + MoveFileEx).
  - The Autosaver has no lost wakeups, and latest-wins is correct.
  - ExportService, FreezeService and RenderEditService all join on destruction; `alive_` guards the posted closures.
  - Shutdown order is safe.
  - The stale drop of render edits covers track gone, document changed, and tempo / tuning / sample changes.
- **MIDI parser robustness:**
  - VLQ ≤ 4 bytes; every read is bounded; every truncated prefix and 600 mutations are tested.
  - Running-status errors are caught.
  - Sysex continuation, MTS bounds, RPN / MPE zones, velocity-0 offs and FIFO pairing are correct.
  - Ticks are int64 from `kPpq`.
  - MPE export ≤ 0.293 ¢ (Q260 option i).

## UI / aesthetic (goldens viewed)
- **limiter-panel:** clean, labelled meters, consistent with Saturator (same knob, chip and label styles).
- **compressor-panel:** dense but readable. The transfer curve is legible and its −20 dB / 4:1 / 6 dB knee is drawn
  correctly. Issues are in L13.
- **transfer-saturator / transfer-reduction:** the curve, the 1:1 dashed diagonal and the live dot read well. Axis labels
  are small but consistent with the EQ grid.
- **export-audio:** follows the Preferences form. Sample Rate "48000" and Bit Depth "24" lack units (Hz, -bit).
- **tracks-frozen / device-frozen:** the snowflake badge, the "running" pills on kept blocks, the veiled Utility and the
  status line [Unfreeze] are clear. The cross-track cable in device-frozen crosses the whole arrangement (pre-existing
  look).

## What is good
- Unusually disciplined work for its size:
  - Design decisions are recorded and reversible.
  - Hard parts come with real proofs (the Limiter's 7-frame argument is right, and the plan's 6 was caught as
    off-by-one).
  - Tests go through real dispatch and bit-exact nulls (export vs `--render`, frozen vs live, cut wires vs the project
    without them).
- The new DSP is realtime-clean and fast (truepeak at 0.75 ns/sample).
- Wire isolation and the freeze lowering are general mechanisms (compile state and one apply rule), not per-device
  cases.
- EX.x-fix-settle found and fixed a real live bug (gains frozen mid-ramp after a locate) instead of papering over the
  export symptom.

## Recommended fix batch before M7
1. **H1** (move the autosave, don't delete it) and **H2** (rate in the freeze file name, plus a rate-change test). Both are
   small.
2. **H4 + M1** together, as one sleep task: silence rules for the fast_ dB atoms, K-classification, quiescence of a
   follower or window on K, a per-module sleep test, and the ramp wake made per segment.
3. **H3**: plan the freeze off the UI thread, keep the old render playing on key moves caused by other tracks, and
   narrow the sum fallback.
4. **M2** (RF64 on three writers, one line each), **M4** (clamp r / GR), **M5 + M6** (cache hygiene).
5. **M3 / M8 / Q261:** user decisions; ask before M7.
6. **M7, M9, M10, M11:** fix if cheap. The L items can go to the backlog.
