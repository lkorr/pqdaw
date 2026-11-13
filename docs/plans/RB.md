# RB: wire isolation, autosave + crash recovery, export audio, freeze / flatten / bounce
User 2026-11-12 ("do 1, 2, 3, 5, 6": items 1 and 2). Draft against main df0c63a9. SPEC §9.8, §12; TASKS "TX.wire-isolation".
Tasks WI.n, AS.n, EX.n, FZ.n. ≤ 2 workers at once; lanes never share a file within a phase.

## What exists today
| Area | State on main | Where |
|---|---|---|
| Compile errors | `graph::inferDetailed` reports kind / rate / width errors (infer.cpp `check()` ~l.900, `rootInputType` ~l.534) as `Diagnostic`s located at the unit-root node (the track call), no structured blame. `ProjectCompiler::compileUnit` turns any into an `Error` for the unit; `ProjectLink::compileNow` (project_link.cpp ~l.1815–1985) then fails the WHOLE job: nothing published, old program keeps playing. | `graph/infer.h`, `engine/src/project_compile.cpp:210`, `app/src/project_link.cpp` |
| Wire states | `WireStatus {Lowered, Delayed, Inactive, Invalid, Refused}`, `WireRefusal {None, VoiceLoop}`; views draw Refused/Inactive dimmed with a tooltip; Invalid hidden in project_patch. | `document/lower.h`, `views/src/device_chain/wiring.cpp:330`, `views/src/project_patch/*` |
| Lowered wire nodes | NodeIds are `nodeIdFor(role, entity, k)` (invertible: entity = (id−1024)/48). A single wire into an empty input connects src → pin directly (no wire node). `planWires` knows each wire's (fromNode/fromDev/port → toNode/toDev/pin). | `lower.cpp:1563, 2491, 3354` |
| Save | `doc::saveProject` atomic (tmp + MoveFileEx); snapshots immutable `shared_ptr<const Project>`; `ViewContext::saved_` drives dirty(). | `serialize.h`, `views/src/view_context.cpp:540` |
| Untitled / crash | Session folder `%LOCALAPPDATA%/pqdaw/Unsaved/<pid>-<unix time>/<n>/`; `findStaleUnsaved` + `processAlive` detect dead sessions (only those holding .wav); `RecoverRecordingsPopover` offered at start. | `app/recording.h:79-97`, `project_link_record.cpp:90-140`, `views/shell/recover_recordings.h` |
| Offline render | `ProjectLink::renderOffline(fromTick, frames, sink)` (must not run while a device drives the engine); `--render` builds its own link (`render_command.cpp`). `media::WavWriter` 32f only + `writeWavPcm24` (whole buffer, no dither). Track buses `"<id>:out"` (busKey, busMetered); per-bus latency `ProjectLink::busLatency_` (T6.6). | `app/src/project_link_audio.cpp:583`, `engine/offline_render.h`, `engine/project_program.h:153` |
| Dialogs | `ui::Popover` windows: `PreferencesWindow` (plain-data port `AudioPrefsPort`), `RecoverRecordingsPopover`; `FileDialogs::chooseFolder`. No Export menu item; hotkey `file.exportAudio` Ctrl+Shift+R reserved "M10". | `views/prefs/audio_prefs.h`, `views/view_context.h:130`, `docs/HOTKEYS.md:25` |
| Deps | Decode only: dr_flac, dr_mp3, stb_vorbis. No encoder besides our WAV. | `docs/DEPENDENCIES.md` |

## Core-structure decisions (approved by the user before launch)
- D1 **Wire cuts are compile state, not document state.** A wire whose signal fails type inference gets `WireStatus::Refused`
  with new `WireRefusal::Type` (drawn red, tooltip = the inference message). The link keeps a runtime cut set and re-tries an
  entry when a structural change touches either end's track. A cut MAIN-PATH link lowers as a path break. The document is untouched.
- D2 **A track can be frozen: `Track::frozen` (bool, structural, saved "frozen": true).** A state of the track, not a container.
  Lowering replaces everything the track's OWN blocks contribute to its mixer input by a clip player of an app-owned render;
  blocks whose output also reaches another track keep running; the mixer (volume, pan, sends, mute) stays live (sends post-freeze).
- D3 **Freeze audio lives in `<project>/cache/freeze/` (deletable, SPEC §12; untitled → its Unsaved folder),** never in the sample
  table. Keyed by (sample rate, tempo map, tunings, frozen part's patch hash); a miss / stale key re-renders in the background
  while the track plays live.
- D4 **Edits of frozen content are refused by one general rule in apply:** a ChangeSet touching devices, clips, notes or device
  lanes of a frozen track (anything but its mixer params and Freeze/Unfreeze) is rejected "Frozen: unfreeze to edit".
- D5 **Flatten changes a track's kind in place (MIDI → Audio):** keeps id, mixer, sends, routing, mixer lanes; deletes blocks,
  note clips and device lanes; the render moves to `samples/frozen/<name>.wav` (a project sample), one audio clip per contiguous region.
- D6 **Autosave never writes into the user's project folder:** `Unsaved/<pid>-<time>/autosave/` (`project.pqd` + `autosave.json`).
  Crash detection = the existing "session folder of a dead process" (no lock file). Undo history is not persisted: a recovered
  document opens dirty with an empty history.
- D7 **Export renders through a second, private ProjectLink built from the snapshot, at the target sample rate (no SRC), on a
  worker thread;** playback and editing continue. Stems = the tracks' `<id>:out` buses captured in ONE pass; "include
  return/master FX" = one solo pass per track (param writes only, no recompile).

## Tasks

### WI.1 Inference blames the cable (graph + engine; one worker)
Goal: a type error says which edge between two track-level nodes brought the bad value.
- `graph/infer.h`: new `struct TypeBlame { std::vector<NodeId> consumer, source; }` (NodeId call paths from the root, cut at
  `InferOptions::blameDepth`, default 0 = off); `InferResult::blames` parallel to `diagnostics` (empty for non-type errors).
- `infer.cpp` `check()` / `rootInputType`: on an error at flat input p, walk upstream from its driver following only inputs that
  carry the offending field (same width for width errors, real for kind, event/higher rate for rate) with a visited set, bounded,
  only on the error path; stop at the first node whose path prefix (to blameDepth) differs from the consumer's; record (consumer,
  source) per crossing (max 4 per diagnostic).
- `engine/project_compile.{h,cpp}`: `compileUnit` sets blameDepth = 2 (unit → track call → track-patch node); on failure store
  `struct UnitTypeFailure { uint32_t track /*graph index*/; uint32_t consumer, source /*raw NodeIds in the track patch*/; std::string message; }`
  in `ProjectCompiler::lastTypeFailures()` (cleared at each update/rebuild/build). Returned Error unchanged.
- Check a failed `update` / `rebuild` leaves `ProjectCompiler` consistent (today's failure path keeps the old program); fix if not.
Acceptance: unit tests on hand-written patches (stereo callee output into a mono-bound input two levels down; real into bool input;
event into audio) → the blame names the crossing call nodes; clean infer unchanged (byte-equal TypedPatch).
Verify: `-Target pq_tests_graph -TestRegex pq_tests_graph`, plus `pq_tests_project` for the compiler accessor.

### WI.2 One bad wire never fails a unit (document + link + views; after WI.1)
- `lower.h`: `WireRefusal::Type`; `LowerOptions::cutWires` (predicate on WireId; unset = none); `LoweredProject::wireEnds` (per
  lowered wire / main link: WireId, graph track index, from NodeId, to NodeId, its own Wire-role entity). `lower.cpp`: cut wire →
  `Refused/Type`, not lowered; cut main link → path break (existing break rule).
- `project_link.cpp` `compileNow`: on a compile failure with `lastTypeFailures()`, map each to WireIds (NodeId decodes to a
  Wire-role entity that is a wire → that wire; else every `wireEnds` entry with (from, to) = (source, consumer) on that track);
  new ids → `cut_` (WireId → message); re-lower (pass the previous lowering for sharing) and recompile; ≤ 8 rounds or until a
  round finds nothing new (then fail as today). `onDocumentChange`: drop cut entries whose wire is gone or whose ends' tracks are
  in a structural ChangeSet's `tracks` (re-tried). Stats `typeCutWires`; `wireError(WireId)` → message.
- Views: `wiring.cpp` and `project_patch` draw Refused/Type in the error colour, message as tooltip/readout (VoiceLoop's path).
Acceptance: a good Utility chain + one stereo → mono-bound wire + one real → bool-input wire plays the chain (render equals the
project without the two wires, bit-exact); both report Refused/Type; an edit on the source's track re-tries; a clean project
costs nothing extra (no retry, same IR).
Verify: `-Affected`. New tests in `tests/app/` (playback link) and `tests/document/`.

### WI.3 Refuse the obvious at gesture time (views; small; optional, after WI.2)
Wiring gestures refuse a drop where DECLARED types clash (declared-real output onto a bool-bound input, event ↔ non-event);
no-drop state + status line. Everything else is left to WI.2. Verify: `pq_tests_views` default tier.

### AS.1 Autosave (app + small view hooks; one worker)
- New `app/autosave.{h,cpp}`: `class Autosaver`, one low-priority worker thread, mutex-guarded single pending job (latest wins).
  `submit(Snapshot, ViewState copy, Meta)` → `doc::saveProject(<sessionBase>/autosave, p, view)` then `autosave.json` (atomic:
  origin project dir ("" untitled), untitled recordings dir, display name, unix time, origin `project.pqd` mtime). `discard()`
  deletes the folder (through `removeUnsavedFolder`'s guard).
- Policy (polled from `ShellApp::onFramePresented`; idle cost = one pointer compare): save when `store.current()` ≠
  `lastAutosaved_` and ≠ `saved_`, not in a gesture, and (≥ 10 s since the last change or ≥ 60 s since the last autosave).
  Discard after a successful Save/Save As, after New/Open, and on a clean quit. `ViewPrefs::autosave` (on by default).
- CPU: serialization (100k notes ≈ 45 ms) runs off the UI thread; the UI pays a ViewState copy.
Acceptance: an edit is on disk within 10 s; a save removes it; an untitled document's autosave references its recordings by absolute path.
Verify: `pq_tests_app` (Autosaver with a test clock / manual pump; files round-trip through `loadProject`).

### AS.2 Crash recovery at start (app + views; after AS.1)
- `recording.h`: `findStaleUnsaved` also returns sessions holding `autosave/project.pqd` (`StaleUnsavedSession::autosave`, meta).
- `RecoverRecordingsPopover` gets a "Recover" mode when the session has an autosave: "pqdaw did not close properly. <name>,
  autosaved hh:mm" [Recover] [Show in Explorer] [Discard] [Later]. Recover goes through guardUnsaved. Untitled: move the session's
  files into a fresh untitled folder (existing recoverStaleRecordings move + relocateSamples), parse the autosave, reset the store,
  dirty. Saved project: open the autosave text with `projectDir_` = origin (relative samples resolve there), dirty; offered only
  when the autosave is newer than the origin's `project.pqd`. Recordings-only sessions behave as today.
Acceptance: fake session folder with a dead pid → Recover restores the document and its takes.
Verify: `pq_tests_views` (popover actions on a fake session) + `pq_tests_app` (findStale).

### EX.1 Export render core (engine + app + media; one worker)
- Engine bus capture: `Engine::renderOffline(..., const BusCapture* cap)`; `BusCapture` = bus indices + one `AudioSink*` each; the
  program copies those buses' block data into preallocated chunk buffers (RT-safe memcpy per block); sinks written between
  chunks. RtScope test.
- `app/export_render.{h,cpp}`: `ExportRequest { Snapshot; Ticks from, to; enum {Master, Stems, Tracks} what; std::vector<TrackId>
  tracks; bool includeReturnsMaster, renderAsLoop, normalize; uint32_t rate; Bits {Pcm16, Pcm24, Float32}; Dither {None, Tpdf,
  Shaped}; Format {Wav, Flac}; folder, baseName; tailMaxSeconds }`. `Result<ExportReport> runExport(const ExportRequest&,
  std::atomic<float>* progress, const std::atomic<bool>* cancel)`: a private ProjectLink at `rate` (compileThread off, JIT on,
  `waitForJit`), solo cleared in its snapshot copy. Pass 1 writes 32f temp WAVs, tracks peak and last frame above −90 dBFS, trims
  PDC (render frames + outputLatency, drop the first outputLatency frames; per stem bus drop its busLatency). Render-as-loop:
  render 2× the loop from its start and keep the second lap. Stems with master FX: one pass per track soloed by a param write.
  Pass 2: normalize gain (−0.3 dBFS peak), dither (TPDF ±1 LSB; shaped = 3-tap error feedback), quantize, write, delete temp.
- `media/wav_file`: streaming PCM16/PCM24 writer (`WavWriter::open(..., WavFormat)`).
Acceptance: master export equals `--render` for the same range (32f, bit-exact); stems sum to the master (no master FX, null
≤ −120 dB); loop render: frame 0 continues the loop's tail; PDC: a click on bar 1 through an oversampled group lands at frame 0;
cancel leaves no files.
Verify: engine capture → `-Affected -Slow`; export → `pq_tests_playback` new cases (short renders).

### EX.2 Export Audio dialog (views + app; after EX.1) 👁
`views/export/export_window.{h,cpp}`: a `ui::Popover` like PreferencesWindow over `ExportPort` (plain data; the app implements it
over `runExport` on a worker thread; tests use a fake). Rows: Rendered Track (Master / All Individual Tracks / Selected Tracks),
Range (Song / Loop / Time Selection, start + length readouts), Include Return and Master Effects, Render as Loop, Format (WAV /
FLAC when EX.3 is in), Bit Depth, Sample Rate (44.1 … 192, default the engine's), Normalize, Dither (below 32f), Folder + name
[Browse…], [Export] / progress + [Cancel]. File menu "Export Audio…" + Ctrl+Shift+R (HOTKEYS → implemented); choices remembered
in user settings. REVIEW.md entry.
Verify: `pq_tests_views` default tier (fake port) + one golden of the window.

### EX.3 FLAC export (deps + media; parallel with FZ.*)
libFLAC (Xiph, BSD-3-Clause) pinned in `cmake/PqDependencies.cmake` (no Ogg, no programs, static /MT), `media::FlacWriter`
(16/24-bit, level 5), DEPENDENCIES.md row. No MP3 (LAME/shine LGPL, Helix RPSL).
Verify: CMake/deps → `-Affected -Slow` once; round-trip through dr_flac in `pq_tests_media`.

### FZ.1 Frozen tracks in the document and the lowering (document; parallel with EX.1)
- `model.h`: `Track::frozen`; serialize "frozen": true (written only when set); command `SetTrackFrozen` (structural; MIDI and
  Audio tracks); freezing a group freezes each non-group descendant; return/master refused.
- `apply.cpp`: the D4 guard, with tests for the allowed set (mixer params, mute/solo, sends, rename, colour, Unfreeze).
- `lower.h/.cpp`: `LowerOptions::frozenSlotOf(TrackId)` (kNone: render not ready → lower live) and `captureOwn` (tracks whose own
  mixer contribution is exported as `export own`: the freeze render's capture point; never set in playback). Frozen track with a
  slot: x = clipPlayer(slot) + Σ mixer wires from OTHER tracks; its blocks kept only if they reach (transitively, via wires) a
  target on another track or a context hosted elsewhere; everything else not lowered (0 CPU), device lanes / wires Inactive.
  `LoweredProject::frozenKeep` lists kept blocks for the UI. PDC follows from the graph.
Acceptance: frozen MIDI track with an Operator-like circuit = one clip player + mixer; a frozen track with an LFO wired to another
track keeps only the LFO; same IR when unfrozen.
Verify: `-Affected`.

### FZ.2 Freeze service (app; after EX.1 + FZ.1)
`app/freeze_service.{h,cpp}`: on Freeze (or load, or a stale key) render the track's `own` bus with `runExport`'s core (private
link with `captureOwn`, project pruned to the tracks feeding the frozen one, range 0 → last clip end + tail until −90 dBFS (max
30 s), PDC-trimmed, 32f) into `cache/freeze/<track id>.wav` + `.json` key; load into the pool, publish a one-clip audio sequence
(warp Off) in the track's slot, return it via `frozenSlotOf` (one structural recompile). Untitled → its Unsaved folder's `cache/`;
Save As copies `cache/freeze`. Progress and status on the track header.
Acceptance: freezing a CPU-heavy track drops its cost readout to the clip player's; playback matches the live render (≤ −100 dB
null, 32f); a tempo edit re-renders while the track plays live; deleting cache/ re-renders on load.
Verify: `pq_tests_playback` (short project, freeze, render null) + `pq_tests_app`.

### FZ.3 Freeze UI (views; after FZ.1; parallel with FZ.2) 👁
Track header and arrangement context menu: Freeze Track / Unfreeze Track / Flatten (FZ.4). Frozen look: icy tint on clips,
devices and device lanes greyed and not editable (apply guard is the safety net); kept-live blocks marked; a refused edit's status
line offers "Unfreeze". REVIEW.md entry. Verify: `pq_tests_views` default tier + goldens of the frozen look.

### FZ.4 Flatten, bounce to new track, consolidate audio (document + app; after FZ.2)
- Flatten (D5): doc op `flattenTrack(track, sampleRef, regions)`; the app moves the freeze file into `samples/frozen/`, adds it to
  the sample table, applies the op as one undo step.
- Bounce to New Track (track context menu; time selection or whole track): renders `own` (FZ.2 core) → a new audio track below
  with the same routing and colour; the original is deactivated. One undo step.
- Consolidate on audio (Ctrl+J across audio tracks): renders each track's clip player alone over the selection (ProjectLink
  `renderClips(track, range)`, generalising `renderClip`) → `samples/consolidated/…wav`, replacing the clips in the range by one.
Verify: `pq_tests_playback` (null against the source) + `pq_tests_document` for the ops.

### FZ.5 Resampling input (later, not in this batch)
An audio track input "Resampling" = the master output bus fed to the recorder (T6.7 path).

## Schedule
| Phase | Lane 1 | Lane 2 |
|---|---|---|
| 1 | WI.1 (graph/infer, engine/project_compile) | AS.1 (app/autosave new, shell_app.cpp, view_context small) |
| 2 | WI.2 (lower, project_link.cpp, views wiring/project_patch) | AS.2 (recording.*, project_link_record.cpp, views/shell/recover_*) |
| 3 | EX.1 (engine renderOffline/program, app/export_render new, media wav) | FZ.1 (model, serialize, apply, lower) |
| 4 | EX.2 (views/export new, app export port, menu) | FZ.3 (views arrangement/headers) then FZ.2 (app/freeze_service, project_link_audio) |
| 5 | EX.3 (deps, media flac) | FZ.4 (doc ops, app) |
WI.2 and FZ.1 both edit lower.cpp (different phases). AS.* must not touch project_link.h. EX.2 stays in `export_*` + shell_app.cpp;
FZ.2 in `freeze_service` + project_link_audio.cpp/.h.

## Choices (recommended defaults; the user may overrule)
- Freeze a group: freezes each non-group descendant. Tempo/tuning edit on a frozen track: re-render in the background, play live
  meanwhile. Editing frozen content: refused with an "Unfreeze" offer. Bounce: original deactivated.
- Formats: WAV + FLAC (libFLAC, BSD-3); no MP3. Normalize: each file to −0.3 dBFS peak. Whole-song tail: until −90 dBFS for 1 s,
  max 10 s (loop / selection: exact length). Stems: post-fader `<id>:out`, solo cleared, mute respected.
- Autosave: 10 s after the last change, at least every 60 s while editing, on by default. WI cut entries are not saved (re-found
  by the first compile).

## Risks
- WI blame precision: an error inside a callee caused by an internal node finds no crossing edge → the unit fails as today
  (correct), must not loop (≤ 8 rounds).
- A failed `ProjectCompiler::update` must leave the compiler's units intact for the retry (verify in WI.1).
- A second link for export/freeze doubles compile memory while it runs; free it when the job ends.
- Freeze correctness depends on "own contribution" being exactly what lowering sums from own blocks; test cross-track cables into
  the frozen mixer (not doubled).
