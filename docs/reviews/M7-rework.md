# M7 rework review (notes-first JI groups, Tune to JI + warps, derived bends, vibrato grip, macro mode, `?` help)

Reviewer: independent review agent (T7.15), 2026-11-15, `main` at `ae88e5db` (branch `task/T7.15-review`). I wrote none of
the code. Scope: `git diff 34ec2837..HEAD -- src tests` (T7.11a, T7.12, T7.13, T7.14, KB.help, T8.1).

## What I did

- **Walked the user's sentences as tasks** through the code: fresh MIDI clip, draw a chord, Tune to JI, warp, back, next
  chord, Is Harmonic 5 + 3/2, bends between notes, vibrato with fades, macro mode from an empty clip and from a JI note,
  each entry of the `?` help against its handler.
- **Three read-only sub-reviews** (document + lowering + engine; macro mode + key routing; tests + goldens + leftovers +
  roll CPU). I re-read the load-bearing lines of every High myself.
- **Reproduced six findings** with a throwaway doctest appended to `tests/views/test_piano_roll_groups.cpp` (real
  dispatch through the `roll_fix::Roll` fixture; built `pq_tests_views` in `check`, ran
  `pq_tests_views.exe -tc="REVIEWT715*"`, then reverted the file; nothing of it is committed). These are marked
  **REPRODUCED** with the measured numbers. CONFIRMED = read with certainty; SUSPECTED = plausible, needs the named test.
- **Looked at** every `pianoroll-group-*`, `-vibrato`, `-bends`, `-fundamental-lock`, `-note-lines`, `-macro-*`,
  `-help*`, `-note-menu`, `-12edo` golden and the five inspiration screenshots. A sub-reviewer pixel-diffed all 139
  re-recorded goldens of 5e7c583c (MACRO lamp) and 3536aa28 (`?` button) against their parents: each changed only in the
  lamp / button region (plus the intended midi-prefs rows and help section). Nothing else moved.
- Not run: the `-Full` suite (the orchestrator's), the app, any audio. Nothing was auditioned.

## Summary verdict

**The model is right and the old Highs are fixed, but a first-time user still gets stuck within five minutes, in the
exact workflow they asked for ("play tempered chords, then warp into JI"), and two everyday edits corrupt tuned chords.
Do not close M7 before H1–H4 and H7.**

What is good:
- **The rework did what the user asked structurally.** One primitive (the note); groups are notes; no object code left
  in `src` (no `JiObject`, `jiVoice`, `PitchKind::Harmonic`, `lockFundamental`, `voiceNotes`, ride, "Edit Vibrato").
- **Old findings fixed for real:** H2 (clone remaps every id), H3 (delete / split promote; consolidate excepted, H6),
  H4 (free curves never read as bends), H5 (6 px detune dead zone + status), M5, M7, M9, M11 (mouse paths exist), M13,
  M17 (Ctrl+Shift+J is a toggle with a chooser), M18 (Add Note at Ratio), M19 (bends derived), L9.
- **Exactness holds.** Walk keeps the anchor's Hz and exact ratios; harmonic numbers use checked arithmetic and capped
  lcm; undo is atomic (one batch per builder); the loader rejects every malformed group / warp / vibrato field tried.
- **The `?` help is genuinely useful**: nine sections, every gesture with a plain-language text, keys read from the
  loaded keymap. It is wrong in two places (H3, L1).
- **Macro mode** matches SPEC §6.2 and the plan's acceptance lines, with a real overlay, layers and fewest-key labels;
  tests go through tree dispatch.

What is wrong, in five clusters:
1. **The "active group" captures notes it should not (H1, H3).** After one Tune to JI, every note drawn anywhere snaps to
   that chord's harmonics and joins it; the panel's "JI" switch does not stop it; Ctrl+Shift+J on the next chord then
   untunes the first.
2. **Editing a whole tuned chord corrupts it (H2, M15).** Up / Shift+Up / a vertical drag of a selected group steps the
   anchor AND every member.
3. **Tune to JI is not a clean switch (M1, M3, M4).** Back drops the group: the chosen reading and the warps are lost;
   chords built in JI cannot go back at all; three cases leave notes as Cents off the rows.
4. **Gestures collide (H4, M14, M11).** Vibrato corners sit exactly on the warp grips and win; Alt + a 4 px edge bends,
   Alt + the body copies; clicking a toolbar button kills every roll shortcut.
5. **Playback drifts from what is drawn (H5, H6, M2).** Tempo changes leave Hz vibratos at the old rate; consolidate
   silences members; a bend from a warped note starts at the wrong pitch.

**Counts: 0 Critical, 8 High, 21 Medium, 18 Low.**

---

## High

### H1 · After one Tune to JI, notes drawn anywhere join that chord, off the rows; tuning the next chord untunes the first — REPRODUCED
`src/views/src/piano_roll/roll_state.cpp:1285-1291` (with nothing selected the last group stays active, Q303),
`roll_ji.cpp:222-227` (`drawPitchFor`: an active group → `DrawPitch::Ratio`), `roll_state.cpp:576-589` (`ratioFor`: the
nearest WHOLE harmonic), `roll_state.cpp:424-430` (the drawn note is a member), `roll_groups.cpp:333-353` (the toggle
untunes when the selection is in one tuned group).
- **Scenario (reproduced):** C–E–G, select, Ctrl+Shift+J (4:5:6). Click empty space, double-click D two bars later: the
  note is a Ratio member of the first group at **386.3 ¢ above C** (the E's pitch, a whole tone above where the user
  clicked). Double-click A: **968.8 ¢** (harmonic 7, 69 ¢ off the A row). The frame now spans both bars and reads
  4:5:6:7. Select the two new notes, Ctrl+Shift+J: status "Back to 12-EDO … 4:5:6:7 is gone": **the first chord is
  untuned and its group deleted**; the new notes stay where the harmonics put them.
- This is the user's own workflow ("play tempered chords, then warp the tuning of that chord"), and it breaks on chord 2.
  The only way out is Esc with nothing selected, which nothing on screen suggests.
- **Fix:** the active group captures a drawn note only inside its frame's time span (or within a grid cell of it);
  elsewhere draw on the rows. Drop the active group on a click on empty space outside every frame. Ctrl+Shift+J on notes
  of a group that are not written-tuned members should tune THEM, never untune the group.
- **Test:** the scenario above through real dispatch: the D is Degree 2, the first group is intact after the second
  Ctrl+Shift+J.

### H2 · Up / Shift+Up / a vertical drag on a selected tuned chord moves every member twice — REPRODUCED
`src/document/src/edit_notes.cpp:378-404` (`stepNotes` steps each selected note on its own), `:232-266` (a member steps
to its next harmonic), `:404` (`dropRepitchedWritten`); called from `roll_state.cpp:1340` (Up / Down) and `:1636` (drag).
- **Scenario (reproduced):** tune C–E–G to 4:5:6, select the three notes (a click on the header does exactly this), press
  Up: the chord becomes **4:6:7** (E/C = 1.5, G/C = 1.75) and the E's written identity is gone, so Back to Tuning cannot
  restore it. Drag the chord two rows up by its anchor: **4:8:9**. By reading, Shift+Up gives 4:10:12. No status says
  anything. Ctrl+A, Up does it to every tuned chord of the clip.
- Transposing a chord is the most common edit there is; the help promises "the frozen note … takes its whole group with
  it".
- **Fix:** in `stepNotes` (and the drag preview) skip a Note-anchored note whose anchor chain is also being stepped; keep
  `written` and step it by the same rows.
- **Test:** select a tuned group, Up, Shift+Up, vertical drag: every Hz ratio unchanged, Back to Tuning restores rows.

### H3 · The clip panel's "Harmonics: JI" switch hides the guides, but drawing still snaps to them — REPRODUCED
`roll_input.cpp:1111-1117` (the toggle calls `setShowHarmonics`), `roll_state.cpp:930-934` (paint flag only),
`roll_paint.cpp:205, :226` (the only readers), `roll_ji.cpp:222-227` (`drawPitchFor` ignores it).
- **Scenario (reproduced):** with a group active, switch "JI" off and double-click D: a Ratio member at 386.3 ¢ (E's
  harmonic) — on a line the user can no longer see. The switch is labelled "JI"; the user asked to "cleanly switch
  between an equal temperament … and JI". This switch looks like exactly that and does not do it.
- The help contradicts the code twice: "While the guide lines show, the notes you add land on the nearest line" and
  "With no guide lines it is the other way round: Shift draws a just note" (`roll_help_content.cpp:186-193`). Its
  info text is stale too ("the line labelled 1/D", `roll_input.cpp:1115`).
- **Fix:** the switch is "draw on harmonics": off = rows, no capture (keep the guides drawn faint if wanted). Same flag
  in `drawPitchFor`.

### H4 · Vibrato on the lowest note of a selected tuned chord makes the warp grips unreachable — REPRODUCED
`roll_input.cpp:306-321` (vibrato parts are hit-tested before group parts), `roll_vibrato.cpp:154-166` (corners at the
note's start / end, `kVibratoGripRise` = 12 px under the line), `roll_groups.cpp:223-226, :295-297` (frame = notes ± 7 px,
bottom = line + lane/2 + 5 px; grips at frame corner ∓ 5.5 px).
- **Scenario (reproduced):** C–E–G–B♭, tune, give the C a vibrato (V2: dragging the ~ on a selected chord gives every
  note one), select the chord. Measured: right grip (739.5, 609) vs vibrato corner 3 (738, 609); left grip (496.5, 609) vs
  corner 0 (498, 609). Dragging the right grip inward made **no warp** and set the C's vibrato `fadeOut` instead.
- "Apply vibrato" and "warp into JI" are both core asks; together the warp silently becomes a vibrato edit.
- **Fix:** give group grips priority over vibrato corners, or draw the corners above the envelope / inside the note
  span; and a test where both exist.

### H5 · A tempo change does not re-lower note tracks: Hz vibratos (and curve timing) play at the old rate — CONFIRMED
`src/app/src/project_link.cpp:1190-1191` (a timeline change only re-publishes the tempo), `:909-913` (the sequence cache
key has no timeline), `src/document/src/pitch_resolve.cpp:424` (vibrato period converted to ticks at lowering),
`project_link_ji.cpp:86, :123` (tempo-dependent cutting / 1 ms thinning). Old L7, now worse because the roll writes every
vibrato in Hz (Q305).
- **Scenario:** 6 Hz vibratos at 120 bpm; set 60 bpm: they play at 3 Hz while the roll (whose paths are rebuilt on a
  timeline change) draws 6 Hz. A track snaps back after any note edit on it, so tracks disagree; undoing the tempo
  change does not fix the edited ones.
- **Fix:** on `kChangeTimeline`, re-lower every track with curves or vibrato (or put the tempo map in `sequenceKey`).

### H6 · Consolidate silences members whose anchor lies outside the played window (H3 still open here) — CONFIRMED
`src/document/src/edit_clips.cpp:367-420` takes only the notes inside `[contentFrom, contentTo)` and never calls
`reanchorWithout` (split and `clonePlaying`, `edit_common.cpp:176-177, :709`, do); `project_link.cpp:196` drops notes
that do not resolve.
- **Scenario:** "Is Harmonic 4" on a note in bar 1, a melody of members in bars 2–8; trim the clip's start to bar 2 (the
  hidden anchor still drives them), Consolidate: the melody is silent; `pruneJi` also drops the group and the written
  identities.
- **Fix:** `reanchorWithout` over the notes each consolidated segment leaves out. **Test:** that scenario, members
  resolve after consolidate.

### H7 · Macro mode: `b` over any note of a group deletes the whole group — CONFIRMED
`src/views/src/macro/macro_mode.cpp:360-363` (`locked()` is true for members too), `:774-778` (`pressLock` →
`undeclareHarmonic`), `macro_doc.cpp:62-66` (`removeJiGroup`), `edit_ji.cpp:89-93, :121` (members of a non-exact anchor
become Cents).
- **Scenario:** a tuned 12-EDO chord; the pointer rests on its E (the anchor follows the pointer, M2) and the user types
  `b 1 0 Enter` meaning "this is harmonic 10": the group is dissolved, every member becomes a Cents note, the status says
  only "Is Harmonic: removed", and the digits then go on as intervals. `B` is also the global Draw Mode key, so a
  habitual B does it too. One undo step, but nothing tells the user what happened.
- The user's definition: "'is harmonic n' just sets that note as a ji object … keeping it frozen". The help and HOTKEYS
  say "B on a declared note removes the declaration" — the code removes it from ANY member.
- **Fix:** `b` + N on any note declares (a member: re-anchor as the menu does, Q297 b); removal only by `b` + Enter with
  no number on the group's own anchor, or by a separate key; the status names what changed.

### H8 · Macro mode: after an auto-scroll the anchor jumps to whatever note slides under the parked pointer — CONFIRMED
`macro_mode.cpp:524-531` (`step` records `seenHover_` BEFORE the scroll), `:571-591, :600-604, :700` (`afterMove` /
`place` call `reveal()` after `step`), `macro_overlay.cpp:39` (`sync` every frame), `:205-212` (`applySync` adopts a hover
change when nothing is composed).
- **Scenario:** the pointer rests in the roll (the user just clicked there), melody entry `2 Tab 3 Tab …`; when the cursor
  nears the edge the roll scrolls, a note passes under the pointer, and the next frame makes it the anchor (and moves
  the cursor to its start). The following intervals are measured from a note the user never chose.
- **Fix:** refresh `seenHover_` after every `reveal()` scroll, or adopt only on a real pointer move.

---

## Medium

### M1 · Back to Tuning, then Tune to JI again, loses the chosen reading and every warp — REPRODUCED
`roll_groups.cpp:343-352` (untune = `untuneJiGroup` = `dissolve`, `edit_ji.cpp:97-138`), `:358-376` (tuning again takes
the top candidate). Reproduced: chose 20:25:30:36 in the chooser, added a warp, toggled twice → 4:5:6:7, 0 warps.
The user asked to "cleanly switch". **Fix:** untuning keeps the group (anchor, reading, warps) with a `tuned = false`
state; the toggle flips the state. Then warps also survive an A/B listen.

### M2 · A bend from a note whose pitch moves starts at that note's START pitch: an audible seam — REPRODUCED
`src/document/src/note_bend.cpp:151-158` (`noteBendTargets` reads `pitches.hz(other)`, the neighbour's pitch at its own
start; Q292 (1)). Reproduced: tuned C–E–G warping back to 12-EDO at beat 2; F with a bend in from the E: the E ends at
400.0 ¢, the F starts at **386.3 ¢**: a 13.7 ¢ jump at the hand-over (31 ¢ for a 7/4; a full step after a neighbour's own
bend-out). Warps, bends and vibrato are the user's three features and this is where they meet. **Fix:** the far end is
the neighbour's effective pitch at the meeting tick (`noteCurveCents` at its end / start; Q292's "one more lookup").

### M3 · Tune to JI is one-way for chords built in JI — CONFIRMED
`roll_groups.cpp:179` (`tuned` needs a written identity), `:343-366`. A chord typed as a proportion (`J`), drawn on the
guides, placed with macro mode or Add Note at Ratio has no written identity: Ctrl+Shift+J answers "the notes already are
this proportion" and the menu never offers Back to Tuning. The switch exists only for chords that were written tempered
first. **Fix:** Back to Tuning on such a group writes each member's nearest step as its written identity (and keeps the
group, M1).

### M4 · Back to Tuning is not exact in four cases: notes end up as Cents off the rows, silently — CONFIRMED
- "Is Harmonic N" on a tuned member erases its written identity and freezes it at its JI pitch
  (`edit_ji.cpp:321-331`); Back to Tuning leaves that note 14 ¢ off its row.
- A member moved by a step / drag loses its written identity (Q295-4, `edit_notes.cpp:404`): Back leaves it as Cents.
- A member drawn as JI before the chord was tuned has no written identity: Back makes it Cents (`edit_ji.cpp:123, :435`).
- Tune to JI on a sub-chord whose lowest note is a written member of another group moves that "frozen" anchor
  (`edit_ji.cpp:414-415`).
**Fix:** Q295's alternative (re-write the written identity to the nearest degree) everywhere a member is repitched, and a
status line when Back cannot restore a note.

### M5 · No mouse path to "3/2 above harmonic 5" — CONFIRMED
`roll_state.cpp:576-589`: drawing with a group active snaps to whole harmonics k·F only; 7.5 cannot be drawn, so the
user's own example needs "Add Note at Ratio…" and typing `3/2`. (After the renumbering 15 is drawable: chicken and egg.)
**Fix:** while drawing near a group, also snap to the simplest ratios of the anchor / hovered member (n/d, d ≤ 4) that
are not harmonics, with the would-be renumbering in the hover readout ("3/2 → 10:15").

### M6 · Ctrl+A, Ctrl+Shift+J tunes a whole progression as ONE chord — path CONFIRMED, cents SUSPECTED
`roll_groups.cpp:358` passes every selected note to `tuneToJi`; `edit_ji.cpp:164-216` builds one proportion over every
distinct pitch. A I–IV–V–ii progression becomes one scale-like proportion anchored on its lowest note (e.g. D–F–A as
27:32:40, a 680 ¢ fifth) instead of one group per chord, and the old groups' anchors become members (chains). "Switch the
clip to JI" is the natural first try. **Fix:** split the selection by onset (notes that overlap in time form one chord)
and make one group per chord.

### M7 · Warps move in time whenever the group's earliest note leaves (except via the anchor) — CONFIRMED
`edit_common.cpp:463, :568-573` (only the promotion path calls `shiftedWarps`), `model.cpp:720` (`groupSpan`). Deleting
the first note of an arpeggiated tuned chord, a split that puts earlier members left of the anchor, copying part of a
group, quantizing or start-resizing the earliest note: every warp slides by the gap, so the chord glides into JI at a
different beat. **Fix:** one rule — any builder that changes a group's start shifts its warps in the same batch; or store
warps in content ticks.

### M8 · Any curve record (a vibrato is enough) on the anchor or on every member makes "delete the anchor" dissolve the group — CONFIRMED
`edit_common.cpp:525` (`constant` requires no curve record on the anchor), `:534` (a member with any record cannot lead),
`:484 / :421`. Tune a chord, give it vibrato (V2 does all selected notes), delete the root: group, warps and written
identities are gone, members become Cents — while `pitch_resolve` already treats a vibrato anchor as constant.
**Fix:** ignore vibrato-only and bend-only records there and in the exact-ratio chain (same cause in Remove JI Group /
Tune to JI producing Cents).

### M9 · Tune to JI re-approximates notes that are exact through a common anchor (old M6, partly fixed) — CONFIRMED
`edit_ji.cpp:199-206`: exactness is kept only for notes anchored to the chord's lowest note or both reaching the
reference. Two members 5/4 and 45/32 of a Degree anchor, tuned without the anchor: the ranking picks 7/5 (7.7 ¢ off) over
the exact 9/8. **Fix:** divide the two ratios exactly when both chain to one anchor.

### M10 · Clicking any roll toolbar control drops keyboard focus; roll shortcuts die — CONFIRMED
Fold, Scale, the JI switch, Loop, Layout, Row Labels and the MACRO lamp are non-focusable (`roll_input.cpp:1047, 1106,
1114, 1122, 1172, 1177`; `macro_overlay.cpp:201`); a press with no focusable ancestor sets focus to null
(`ui/src/ui_tree.cpp:596-600`). Only `?` hands focus back (`clip_view.cpp:47-49`). After clicking Fold, Ctrl+Shift+J, J,
Alt+Up / Down, Ctrl+G, Delete and the arrows do nothing until the note area is clicked; with focus in the arrangement
Ctrl+G groups tracks (`ableton.json:56`) instead of gliding. **Fix:** a press on a non-focusable control inside ClipView
focuses the note area (as `?` does), as a tree rule.

### M11 · Macro keys act whatever has focus — CONFIRMED
`ui_tree.cpp:666` (the key listener runs before the focused widget), `macro_mode.cpp:79-81` (`active()` checks only that a
clip is open). With focus in the arrangement or mixer, digits place notes in the open clip and scroll it, and `0`
(activate), `A` (automation), `-` / `=` (zoom), `B`, Tab (focus traversal) are taken. **Fix:** take keys only while focus
or the pointer is in the ClipView (or pass through the keys the focused scope binds).

### M12 · Ctrl+CapsLock leaves the OS Caps Lock on: the next rename is typed in capitals — CONFIRMED (Q299 open)
Text fields use `SDL_EVENT_TEXT_INPUT` (`app` window.cpp:177, `ui_tree.cpp:604-605`), so after toggling macro mode a track
name comes out upper case. Q299 (a) was chosen without the user. **Fix:** Q299 (b): re-toggle the lock with one
`SendInput` in `app` after the chord (Windows-only code behind the platform layer), or ask the user for another chord.

### M13 · Alt + a 4 px edge bends, Alt + the body copies: a 1 px miss duplicates the note — CONFIRMED
`roll_widgets.h:30` (`kEdgeGrab` = 4 px), `roll_state.cpp:1095-1098` (no right zone under 8 px, no left zone under 12 px),
`roll_input.cpp:95-96` (Alt on the body = Copy). "Bend between notes" is a core ask; aiming for a 4 px strip with Alt held
and getting a copy dragged instead is the most likely first experience, and 32nd notes at default zoom cannot be bent in
at all. **Fix:** with Alt held, widen the edge zone (e.g. the outer third of the note, ≥ 8 px) and show the bend cursor
there; copy only from the middle.

### M14 · Dragging a group's anchor alone: members stay put in the preview, then jump on release — CONFIRMED
`roll_state.cpp:952-967` (`centsOf` resolves note anchors from the document), `roll_paint.cpp:361, :420`. What is drawn
during the drag is not what the release does. **Fix:** resolve note anchors against the previewed anchor pitch during a
Move with steps ≠ 0.

### M15 · CPU: a ~ grip drag on many selected vibrato notes rebuilds every path ~20 times per frame — CONFIRMED (structure), timing SUSPECTED
`roll_ji.cpp:165-168` (all previews share one scratch slot), `roll_vibrato.cpp:125-165, :396-424` (`steadyY` /
`depthPx` → `startCents` → `pathOf` per corner), `:107-121` (`vibratoNotes` takes every drag target, `std::find` dedupe:
O(k²)). With TASKS' 5.6 µs per build: ~110 µs per note per frame, a dropped frame at ~150 selected notes. The comment at
`roll_pen.cpp:28-29` ("once per frame") is false. **Fix:** a preview path cache by note id, cleared when `curvePreview_`
changes; a sorted set for the targets.

### M16 · CPU: the path cache is dropped wholesale during a MOS morph and on every edit of a clip with any warp — CONFIRMED (structure)
`roll_state.cpp:89` (each morph frame bumps `tuningGen_`) + `roll_ji.cpp:66-69` (clear all); `roll_ji.cpp:164, :77`
(`needIndex` is per clip once any group has a warp, so every curved note is dropped on any edit and the clip re-resolved).
~11 ms per morph frame at 2,000 vibrato notes. **Fix:** keep the points of entries that did not read the index and
recompute only `base`; decide `needIndex` per note (`warped(n.id)`).

### M17 · CPU: lowering and stepping are O(groups × notes) per commit; every note edit re-lowers the whole track — CONFIRMED (structure), cost SUSPECTED
`project_link_ji.cpp:59-65` and `edit_notes.cpp:272-277` call `groupHarmonics` per group (a full clip pass each,
`model.cpp:677`); `findJiGroupByAnchor` is O(groups) (`model.cpp:562`); `project_link.cpp:924` still deep-copies descs per
publish (old M12). 500 tuned 4-note chords ≈ 1 M note visits per commit. **Fix:** one pass bucketing members by anchor id;
an anchor → group index; re-lower only the clip that changed.

### M18 · Engine: the 256-event cap (old M1) still drops note-ons behind curve events — SUSPECTED (arithmetic)
`project_link.cpp:51, :497`; `timeline.cpp:688, :785`. Vibrato flattening is cheap (16–32 points per cycle at 1 ¢,
thinned to ≤ 1000/s): 64 fast vibratos are ~22 events per 64-frame block, fine. But at a loop wrap or chase with ~95+
curved notes, curve events can take the room a note-on needs; that note stays silent (`droppedNoteOns`). **Fix:** reserve
room for note-ons before curve events (drop expression first; old L4 is the same rule).

### M19 · Macro key pressed mid-drag joins the drag's undo entry — SUSPECTED
`place()`, `back()`, `commitLock()` do not check `store().inGesture()` (`undo()` does, `macro_mode.cpp:739`). A note placed
while dragging becomes part of the drag's history entry and Esc on the drag removes it too. **Fix:** refuse while a
gesture or capture is active. **Test:** key during a drag, then Esc.

### M20 · Tests: one bound cannot fail; coverage deleted without replacement; the new Highs have no test — CONFIRMED
- `tests/views/test_piano_roll_vibrato.cpp:350`: `rebuilt <= built + 45` with 40 notes passes even if the cache were
  dropped on every edit (expect ≤ built + 2); no case with a warp in the clip or a morph (M16).
- Deleted without replacement: the timed "roll JI perf … 2 ms" paint test (old `test_piano_roll_ji.cpp:502`) and
  "Tune to JI in an Equidistant MOS roll" (old `test_piano_roll_ji_edit.cpp:664`); no timed test for groups or vibrato
  paint now.
- Esc mid-drag is tested only for warps (`test_piano_roll_groups.cpp:403-412`), not pen, bend, vibrato or detune.
- No test for H1–H4, H7, H8, M10 / M11 (focus elsewhere), M12, macro key during a drag, 12-EDO 5/4 = 4 steps.
- `tests/views/test_affordances.cpp`: the move-damage bound went from 2 px to 28 px for every note (grip room), so every
  note move now repaints ±28 px; justified only for hovered / selected notes.

### M21 · MIDI export drops bends, vibrato and warps silently (old M3) — CONFIRMED
`src/document/src/midi_export.cpp` writes static pitches only, with no counter or status. A tuned, warped, vibrating
chord exports as its start pitches. **Fix:** a status line "N notes lost their pitch curves" now; MPE pitch-bend export
later.

---

## Low

- **L1 · CONFIRMED · `roll_input.cpp:604-707`** "Insert Proportion…" is only in the empty-space menu when NOTHING is
  selected; with a selection, right-clicking empty space shows the note menu. The help says "right-click empty space".
- **L2 · CONFIRMED · `roll_pen.cpp:472`, `roll_input.cpp:661, :681`** "Clear Bends" also deletes the vibrato and is enabled
  when only a vibrato exists. Keep the vibrato, or rename "Clear Curves".
- **L3 · CONFIRMED · `roll_input.cpp:634, 648-649, 677, 699`** menu shortcut labels are literals ("Ctrl+Shift+J",
  "Alt+Up", "Ctrl+G", "J"), not the loaded keymap's keys (the help reads the keymap; the menus do not).
- **L4 · CONFIRMED · golden `pianoroll-group-chooser.png`** the chooser's number column ("47 ¢") is unlabelled, and the
  ticked best reading (4:5:6:7) shows the LARGEST number of the list (total move), so it reads as the worst. Label it
  ("moves 47 ¢ in all") or show the largest single move.
- **L5 · CONFIRMED · goldens `pianoroll-fundamental-lock.png`, `pianoroll-12edo.png`** the "F · 65.93 Hz ↓" chip covers
  the frame's bottom-left corner and the head of the lowest note.
- **L6 · CONFIRMED · golden `pianoroll-macro-tempered.png`** the MACRO lamp truncates its text ("MACRO · 12-E…").
- **L7 · CONFIRMED · `roll_vibrato.cpp:248-252`** depth follows the pointer in the roll's pitch scale: at the default 14 px
  rows 1 px ≈ 7 ¢, so a 20 px drag is a 140 ¢ vibrato. WYSIWYG, but twitchy; Shift is the only fine control.
- **L8 · CONFIRMED** old L15 still open: Draw Mode Ctrl + click without a drag stores nothing (`roll_pen.cpp:197-201`);
  HOTKEYS.md:114 says it adds a breakpoint. Old L16 (Ctrl+Z mid-drag writes the pre-undo record back,
  `roll_state.cpp:49-111`) still open, SUSPECTED. Old L17 partly: header strings measured per paint and per hover, no
  x-cull (`roll_groups.cpp:232-238, :264-266, :283-284`).
- **L9 · CONFIRMED · `roll_groups.cpp:163-180`, `roll_state.cpp:1257-1264`** `groupInfos` walks every note per group per
  snapshot (≈ 1 ms per edit at 300 chords); `bendNeighbour` is O(selected × notes) per gesture (`roll_pen.cpp:307-365`).
- **L10 · CONFIRMED · `macro_mode.cpp:496, 513-519`** `b` digit entry: a leading 0 is ignored, a fifth digit dropped, and
  any unbound key commits the declaration and then also acts (`b` then Space declares and starts playback).
- **L11 · CONFIRMED · `macro_mode.cpp:560-561`** `-` / `=` auto-repeat: holding `-` unwinds placements fast.
- **L12 · CONFIRMED · `macro_mode.cpp:197-204`** the pointer brushing a note with nothing composed resets the cursor tick to
  that note's start (follows the plan's M2; loses a/f navigation).
- **L13 · CONFIRMED · `fewest_keypress.cpp:85-87`, `macro_mode.cpp:432`** the key labels ignore a preset's per-EDO `vals`
  and only use 13-limit keys (`7`–`0`, `u`–`p` never show); octave EDOs only. The help says "in an EDO".
- **L14 · CONFIRMED · `macro_mode.cpp:85`, `:485`** turning macro mode off does not turn the computer MIDI keyboard back
  on; a persistent panel (circuit editor / viewer, bug panel) silences macro mode with no status.
- **L15 · CONFIRMED** `J` is still a piano key while the computer MIDI keyboard is on (`computer_keyboard.cpp:155-163`); the
  proportion tool has no button. `?` has no key binding.
- **L16 · CONFIRMED** engine leftovers unchanged since 34ec2837: old L1 (`timeline.cpp:451-494`, `Sounding::shift` not
  refreshed), old L4 (`note_events.h:187`, a NoteOn is evicted first); L2 / L3 / L5 / L6 not re-checked.
- **L17 · Leftovers (CONFIRMED).** Stale "JI object" / Harmonic text: `src/views/src/arrangement/editing.cpp:114`,
  `src/app/include/pq/app/project_link.h:311`, `src/app/src/project_link_ji.h:13-14`,
  `src/engine/include/pq/engine/note_events.h:4, :70, :75`, `src/engine/include/pq/engine/sequence.h:112`,
  `src/engine/src/voices.cpp:897`, `src/document/include/pq/document/midi_export.h:26`; test comments
  `tests/document/test_virtual_fundamental.cpp:174`, `tests/views/test_piano_roll.cpp:529`, `tests/app/test_ji_playback.cpp:14`,
  `tests/views/test_piano_roll_ji.cpp:11-14`, `tests/ui/test_controls.cpp:114`; theme token `roll.jiStop`
  (`src/ui/include/pq/ui/theme.h:114`) now colours warp markers; golden / scene name `pianoroll-fundamental-lock` shows a
  group; `voice.unisonCount` / `voice.index` are never set by the lowering (always 1); unused roll API `hoverGroup()`,
  `morphingGenerator()`, `vibratoDepth()` (`roll_state.h:599, :491, :731`); `setSelectionVibrato` is scene-only. The JI
  switch's info text (H3). No object code survives.
- **L18 · Docs (CONFIRMED).** SPEC §3.1 (`Harmonic (jiObjectId, voiceIndex)`, SPEC.md:106), §4.3 ("or subharmonics"), §5
  (objects, morph, arp, ride) and §6.1 ("M toggles the computer keyboard") describe what no longer exists; only the §15
  log entry supersedes them. Proposed text below.

---

## The user's sentences, walked from a fresh project

| Ask | Path today | Verdict |
|---|---|---|
| Write notes, tune a tempered chord to JI | draw, select, Ctrl+Shift+J (chooser opens) | works, if the roll has focus (M10) |
| Warp the held chord into JI mid-note | drag the frame's bottom-left corner inward | works, unless the bass note vibrates (H4) |
| Next chord, tempered | double-click / pencil two bars later | **broken**: joins chord 1 off the rows (H1) |
| Switch back to ET | Ctrl+Shift+J again / the panel's JI switch | back works once but forgets reading + warps (M1); the switch does not switch (H3); JI-built chords cannot go back (M3) |
| Transpose a tuned chord | header click, Up / drag | **broken**: 4:5:6 → 4:6:7 (H2) |
| Declare harmonic 5, place 3/2 above | menu Is Harmonic… `5`; menu Add Note at Ratio… `3/2` | works with typing; not with the mouse (M5); renumbers 10:15 correctly |
| Bend between notes | Alt + 4 px edge, Ctrl+G | works; fiddly (M13); seam after a warp (M2) |
| Vibrato with fade in / out | hover → drag ~; drag the corner squares | works, nothing typed (V1–V3 met) |
| Macro mode (Ctrl+CapsLock) | 1…0 / q…p, `` ` ``, Tab, `g`, `b N Enter` | works from an empty clip (Q301) and from a JI note; `b` destroys groups (H7), anchor jumps on scroll (H8), Caps Lock stays on (M12) |
| `?` help | the `?` button | complete, two entries wrong (H3, L1) |
| `M` no longer toggles the keyboard | unbound | done |

## First-time-user dead ends, in the order they are hit

1. Clicks Fold / Scale / the JI switch, then presses Ctrl+Shift+J: nothing (M10).
2. Draws the next chord after tuning the first: it snaps onto chord 1's harmonics and joins its frame (H1).
3. Turns the "JI" switch off to get the rows back: notes still snap (H3).
4. Tunes chord 2: chord 1 is untuned (H1).
5. Transposes a tuned chord with Up or a drag: the voicing changes (H2).
6. Gives the chord vibrato, then tries to warp it: the corner drag edits the vibrato (H4).
7. Toggles back and forth to compare: the chosen reading and the warps are gone (M1).
8. Alt-drags a note edge to bend and gets a copy (M13).
9. Macro mode: `b 10` on a chord note dissolves the group (H7); later renames a track in capitals (M12).
10. Changes the tempo: vibratos change speed in playback but not on screen (H5).

## The five changes I would make first

1. **Contain the active group.** Capture drawn notes only within the group's time span; a click on empty space outside
   frames drops it; the panel switch means "draw on harmonics" (H1, H3).
2. **Group-aware transpose.** Step only the anchor when it is selected (members follow) and step `written` with it;
   same in the drag preview (H2, M14).
3. **Make Tune to JI a state, not a create / dissolve.** Keep the group, reading and warps when going back; give
   JI-built members a written nearest step; re-write `written` instead of dropping it on repitch (M1, M3, M4).
4. **Fix the hit-test order and the zones.** Warp grips before vibrato corners; a wider Alt bend zone; a toolbar click
   returns focus to the notes (H4, M13, M10).
5. **Macro and playback correctness.** `b` never dissolves a group by accident and re-syncs after scrolls; macro keys only
   with the roll focused; re-lower on tempo changes; reanchor in consolidate (H7, H8, M11, H5, H6).

## Proposed SPEC text (for the orchestrator; SPEC decisions are the user's to approve)

**§3.1, the identity table:** delete the `Harmonic` row. `Ratio`: "exact rational relative to an **anchor**: another note
(a JI group's frozen note) or the clip reference".

**§4.3 JI view layer:** "The active JI group's virtual fundamental F and its harmonics are drawn as faint numbered guide
lines (no subharmonics). Notes show their harmonic number in a group, else their ratio or cents offset."

**§5 JI groups (notes-first, after Zheanna Erose)** — replace the section:
- A **JI group** is ordinary notes: one **anchor** note, declared harmonic N and frozen at its own pitch, and the notes
  tied to it by exact ratios (members). F = anchor / (N·m), m the least integer that makes every member a whole harmonic;
  the numbers renumber (5 with a 3/2 above reads 10:15). No subharmonic groups.
- **Declare:** "Is Harmonic N" on a note (menu; macro `b` N). **Tune to JI** (`Ctrl+Shift+J` or menu) on selected notes:
  the lowest becomes the anchor, the others the best simple reading (a chooser lists the others); each member remembers
  what it was written as, and the same key goes **Back to Tuning** exactly.
- **Warps:** per group, `at / glide / shape / toJi` in group ticks: before a toJi warp the members sound as written, after
  it in JI; dragged from the frame's bottom corners, moved by their diamonds and glide grips.
- **Edit as notes:** Up / Down on a member = next harmonic; `Alt+Up / Down` walks every number ±1 with the anchor fixed;
  a typed proportion (`J`, drag a box, or "Insert Proportion…") creates the notes; "Add Note at Ratio…" adds a member;
  deleting the anchor promotes the lowest member.
- **Look:** every note is a line on its exact pitch following its curve; a group is a dashed frame with its proportion
  as header, harmonic numbers on the notes, warp markers and state labels under it.
- **Per-note pitch:** bends name their neighbour (Alt + an edge, `Ctrl+G`) and follow it; a free curve is drawn in
  Draw Mode; vibrato is a note property (the ~ grip: depth up / down, rate sideways; four envelope corners for onset,
  fade-in, fade-out, end; Draw Mode Alt + stroke).
- **Not built (deferred):** morph between proportions as one gesture, pulse arpeggiator, chord sets, ride curves,
  phase-locked unisons for notes that bend into a unison, MPE / MTS export of curves.

**§6.1:** replace "M toggles the computer keyboard …" with "the computer keyboard is switched by its control-bar button
(no default key; `M` is free)".

**§6.2 Macro mode** — replace the Layers and lock parts:
- **Layers** (`g` switches; shown on the MACRO lamp): *Tempered* (default in an EDO / MOS / scale): the placed note is
  the tuning's step for the composed interval (EDO: patent val, overridable per EDO in a preset; MOS / scale: nearest
  degree), `Degree` identity, the error shown. *JI*: the note is the exact ratio to the anchor. In either layer, from a
  JI anchor (a ratio note, a group's anchor or member) the placed note is an exact member of that group. The scale-steps
  layer is not built.
- **Anchor:** with nothing composed, the note under the pointer, else the single selected note; the cursor keys still
  move it.
- **`b` N `Enter`** = Is Harmonic N on the anchor; `s` / `d` then step by that group's harmonics. (`b` removal: per H7.)
- `Ctrl+CapsLock` also flips the OS Caps Lock (Q299, open). While macro mode is on the computer MIDI keyboard is off;
  keys with Ctrl / Alt, and Space, pass through.

## What I could not check

- Nothing was heard: vibrato shape, warp glides, bend seams (M2 is a measured pitch, not a listen), the event cap (M18)
  under a real loop wrap.
- The app with a real mouse and SDL (Caps Lock keycodes, hover timing, grip feel at other DPI scales).
- Timings: M15–M17 and the per-edit costs are estimates from structure and TASKS' own figures; no bench was run.
- H5–H8, M3–M12 were read, not run; the first step of each fix should be the test named in it.
