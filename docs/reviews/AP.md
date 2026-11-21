# AP review (the arpeggiator as a note stage: AP.1 – AP.7, AP.2b)

Reviewer: independent audit agent, 2026-11-21, `main` at `5eeac72a` (AP.1 `b4e802bd` … AP.7 `c0e22ceb` + merges).

## What I ran
Nothing was built or run. Everything below is **code-read** at `5eeac72a` and cited by file:line. Two parallel deep reads
(engine: note_stage / arp / engine.cpp / timeline; document: note_devices / lowering / ProjectLink / note_stage_render /
note_flatten / NotePreviews), then I re-checked every High and most Medium claims myself (takeOver, the mailbox, the
retune handler, Flatten, the tuning source of the chain's degree table). I viewed all 5 `arp-panel-*` goldens and
`pianoroll-arp-preview.png` beside `compressor-panel.png` and `stock-saturator-panel.png` (both 2x; arp 1x).

## Summary verdict
The engine core is clean: no allocation, lock, unbounded loop or memory-order error on the audio path; chain install /
retire / adopt are correct; onsets are exact rationals through the tempo map; RNG keyed by grid step; no per-block sort.
Style / unit / rate are runtime params and a tuning edit posts a table instead of republishing, as the user asked.
The gaps are at the edges of the user's explicit retune requirement (steps that do not follow, a latched set that
never retunes, a mailbox race) and in Flatten, which can write something other than what plays and then switch the
arp off. **Fix the 4 High items before building more note devices on this slot.**

Counts: **C 0 · H 4 · M 9 · L 14**.

---

## High

**H1 — Ratio / Harmonics / Walk / Layer steps do not follow a live tuning modulation.** — **fixed in AP.8a** (every step keeps its source's degree identity; Hz = static × exact ratio: the node's E carries the ratio, note_events.h "Pitch identity")
- **Where:** `src/engine/src/arp.cpp:603-608, 660-700` (`scaled()` / `pitch()` set `degree = equave = NaN`,
  `tuning = 0`); documented as intended in `src/engine/include/pq/engine/arp.h:58-68`.
- **Defect:** only copy 0 and Degrees-unit steps keep a degree identity, so only they follow a `tuning_hz` node drag or
  an LFO on the tuning (T2.6). Every shifted Ratio / Harmonics step, every Walk element and every Layer is static.
- **Scenario:** a MOS generator drag with the arp in Ratio 3/2, Steps 3: copy 0 bends with the drag, copies 1–2 stay —
  the arpeggio detunes against itself. Violates "sounding steps retune live".
- **Fix:** keep the source's degree / equave / tuning on the event and add an exact ratio multiplier (num/den) the
  voice applies on top of its degree-derived Hz.

**H2 — Hold-latched notes never retune.** — **fixed in AP.8a** (tables carry referenceHz; a new table set re-resolves every held note, latched too)
- **Where:** retunes reach the arp only as `kNoteEventRetune` Pitch events (`arp.cpp:1073-1092`), sent by the timeline
  and `live_input.cpp:280` `retuneHeld` only for notes whose key is still down; `retableNow` (`arp.cpp:1844`) then
  multiplies the latched source's OLD Hz by the NEW table's interval.
- **Scenario:** Hold on, release the chord, edit the tuning: the latched set plays old-tuning roots × new intervals.
- **Fix:** the mailbox table carries absolute degree → Hz (reference Hz), and the arp re-resolves Degree-identity
  latched notes itself on `retable`.

**H3 — Flatten writes notes playback would not play, then switches the arp off.**
- **Where:** `src/app/src/note_stage_render.cpp` (knob positions only; `makeArpStage(..., {})`, no `mods`);
  `src/engine/src/note_stage_offline.cpp:68-82` (no `b.mods`, no automation); Q?-AP.7-a (3).
- **Defect:** Q?-AP.7-a scopes this as a preview limitation, but Flatten (`src/views/src/note_preview.cpp:113`
  → `ops::flattenNoteStages`) uses the same render and silently deactivates every note device on the track
  (`src/document/src/note_flatten.cpp:70-75`). An automated Rate / Style or a cabled Gate flattens to different notes.
- **Fix:** feed the automation image into `renderNoteStages`, or refuse Flatten (status message) while any device
  param has automation or a cable. I would overrule Q?-AP.7-a (3) for Flatten.

**H4 — The Degrees unit uses the TRACK tuning's table for clips with their own tuning.** — **fixed in AP.8a** (ArpTables per tuning node, playback and offline render; static-tuning clips: Q?-AP.8a-a (1))
- **Where:** `src/document/src/lower.cpp:4379` (`effectiveTuningRef(p, *at)` without the clip, unlike `:3902`);
  `NoteClip::tuning` (`model.h:376`); the same table in `note_stage_render.cpp`'s Degree identity / `centsOf`.
- **Scenario:** a 31-EDO clip on a 12-EDO track, Distance 1: each step moves by a 12-EDO semitone and wraps the equave
  at 12; Flatten writes the wrong Degree. Q?-AP.1-a (4) ("one table per stage") did not consider per-clip tunings —
  I would overrule it.
- **Fix:** key the table by the event's `tuning` id (a small per-stage table set, one per tuning the track's clips
  use), in playback and the offline render alike.

## Medium

**M1 — `ArpTableMailbox` race can leave the last tuning edit unapplied.** — **fixed in AP.8a** (4 retired slots, `collect()` from ProjectLink::collect; confirmed by reading: the interleaving is real) `src/engine/src/arp.cpp:197-214`. Audio does
`pending.exchange` (takes t1); `post` does `retired.exchange(null)` (sees null) then `pending = t2`; audio then
`retired.store(cur)`. Now `take()` returns null until another edit is posted — the final value of a drag can stick.
Fix: free `retired_` from the link's `collect()` (a `box->collect()`), not only from `post`.

**M2 — A structural edit ahead of a running arp restarts it.** — **fixed in AP.8a** (adoption by key anywhere, order kept; kNoteEventGone offs) `src/engine/src/note_stage.cpp:43-56`: `takeOver` adopts
only a matching positional PREFIX; inserting / removing / switching off a note device before it, or reordering two,
gives `n == 0` → `old.flush`. Known in PROGRESS (AP.5), but not filed as a Q. Fix: match old stages by `key` anywhere
in the old chain.

**M3 — A tempo edit during playback acts as a locate for the arp.** — **fixed in AP.8a** (StageClock::remapped) `Timeline::swapTempo` (`timeline.cpp:417-425`)
moves `pos_`; the arp sees `seg.pos != expectPos` (`arp.cpp` ~1873) and runs `transport()` → `endAll` + clock reset.
(Clip notes are also released there, T5.4, so the source chord re-triggers — but Hold-latched sets and sounding steps
are cut too.) Fix: a "tempo swap" flag on the clock; re-seek `nextSong` from the tick without `endAll`.

**M4 — O(512²) per frame when the sounding table is full.** — **fixed in AP.8a** (one partial selection per step / pulse frame, a tie table; the per-tie minOff rescan was the larger cost) `arp.cpp:1386, 1430-1438`: each `emit` at capacity does an
O(512) earliest-off scan + `recomputeMinOff`, and Tie adds an O(512) `tieWaiting` scan. Chord Trigger, 128 held,
3 layers = 512 emits in one frame ≈ 0.5 M ops — an xrun risk at small blocks. Fix: cut the excess once per step
(partial select / min-heap on `off`), index tie-waiting notes by (rank, layer).

**M5 — Flatten deletes notes that never play.** `src/document/src/note_flatten.cpp:36-39` removes every enabled note,
including notes outside `[contentStart, contentStart + length)`. Fix: remove only played-span notes / listed sources.

**M6 — Flatten switches off devices for clips it does not write.** `note_flatten.cpp:70-75` vs `playedClips`
(`note_stage_render.cpp:205`): unplaced / muted clips lose their arp without being flattened. Fix: flatten them too,
or say so in the status.

**M7 — Flatten renders from tick 0 to `kMaxTicks` on the UI thread.** `src/views/src/note_preview.cpp:113`
(`computeNow`). AP.md: compute "off the UI thread". A long song with a 1/128 arp stalls the UI. Fix: run it on the
NotePreviews worker and post the edit back.

**M8 — A cable into a lane step looks live but does nothing.** `lower.cpp:4366-4368` lists it in `slot.mods` and
exports the bus; the ArpStage ctor skips `add` mappings for lane steps. Fix: keep lane-step wires out of
`NoteDissolve::params` so they draw Inactive (or route them to `mapLaneStep`).

**M9 — Opening a project keeps the previous project's running arp state.** — **fixed in AP.8a** (traced: the link handles the Reset before ViewContext::afterLoad stops the transport, and a stopped free-clock latched set survives a stop anyway; chains cleared + a per-reset key epoch) `src/app/src/project_link.cpp:1324`: Reset
publishes without clearing `noteChains_` (unlike `makeEngine` at `:629`); a same-slot, same-id, same-structure chain
adopts the old held set / Hold latch. Fix: `if (reset) noteChains_.clear()` first. (Plausible, not traced to the
engine's stop/panic path.)

## Low (14)
- L1 (**fixed in AP.8a**) Retune leaves glide state stale (`gens[].c`, `outHz`, `prevHz` / `curHz`, `h.on.noteDegree`): `arp.cpp:1073-1092`.
- L2 (**fixed in AP.8a**: the table now re-resolves key-held notes too; the later retune is absolute) Tuning edit: table taken at block start, source retunes may arrive a block later → a brief old-source × new-interval
  glide (`retableNow`), then a re-glide. Not verified.
- L3 (**fixed in AP.8a**) `e.harmonic` is float; `scaled()` allows up to 2^40 / 2^46 (`arp.cpp:603, 697`): clamp to 2^24 or 0.
- L4 (**fixed in AP.8a**) Stage note ids wrap at 2^24 (`note_stage.h:52`); a long Tie chain can collide after 16 M notes: skip live ids.
- L5 (**fixed in AP.8a**) Idle arps still map ~60 params + `memcmp` every block before the idle early-out (`arp.cpp:2082-2095`); `memcmp` on
  a padded struct is unspecified. Check idle first; compare fields or a version.
- L6 Handover coinciding with a panic: re-sent held note-ons at frame 0 land before the AllNotesOff (`engine.cpp:189-210`).
- L7 Per-type keys on `"arpeggiator"`: `note_stage_render.cpp:24` (stage factory), `note_devices.cpp:352` (panel). Put
  `makeStage` / `panel` on `NoteDeviceType`.
- L8 (**fixed in AP.8a**) `busValueOf_` never frees indices (`project_link.cpp:1069-1076`): after 1024 wire ids cables silently go dead.
- L9 (**fixed in AP.8a**: timelineStageBlock) `note_stage_offline.cpp:68-82` copies `Engine::runStages`' block setup by hand: one shared helper.
- L10 Preview cache key (`roll_state.cpp:1052`) misses a parent group's tuning and project-level wire edits; sample
  rate / seed captured once (`shell_app.cpp:290`). `latest_` never pruned; worker joined without a cancel flag.
- L11 Panel, Pulse fold: Layer 3's knob shows with Layers = 2; the 8 Proportion boxes show with Length 4. I would
  overrule Q?-AP.6-a (2): dim values past Length and layers past the count.
- L12 Panel, Arp block: two controls named "Retrigger" (pattern Off / Note / Beat, and velocity's lit button with a bloom
  that bleeds below it); Triplet chip and Tuplet n:m overlap (3:2 = Triplet). Name the second "Vel Retrig"; derive the
  chips from the tuplet. The Glide fold's Sync / Cents toggles float unlabelled between Out and Time.
- L13 Panel, Pattern view: an empty "Hold notes" box takes a third of the 760 px width in 4 of 5 goldens; off step lanes
  (Gate / Glide / Chance) draw near-full-value bars almost as bright as on ones.
- L14 `pianoroll-arp-preview.png` is drawn from a hand-made `sceneArp` source (`view_scenes.cpp` ~855-889), not the real
  renderer, so it shows no gate gaps and reads as one continuous zigzag; its comment says ghosts are drawn "under" the
  chord, Q?-AP.7-a (8) says over.

## Design vs plan / Q readings
- Delivered as planned: note stage by declared signals (`doc::chainStages`, `meta.cpp:103-108`), zero cost without a
  chain, runtime vs structural split, adopt-on-republish, all four v1 extras.
- Differs with a Q: preview renders from tick 0 instead of "visible range + one bar" (Q?-AP.7-a (2), reasonable).
- Overrule: Q?-AP.1-a (4) (H4), Q?-AP.7-a (3) for Flatten (H3), Q?-AP.6-a (2) (L11).
- Question (**decided in AP.8a**: Pulse Ref defaults to a fixed 8): Q?-AP.4-a (8) — releasing the lowest held note re-times every clock (h_ref = lowest). Musically a released
  bass note shifts every rhythm; consider making Ref "Fixed" the default or keeping h_ref until the set empties.

## CPU
- Per-block mapping is ~40–60 slot reads (lane steps read only when they play): fine; L5 is the only waste.
- 128-held pulse 2.1 µs / 64-frame block ≈ 0.16 % of the block: ≈ 3 onset frames each an O(held) scan. A min-heap of
  next onsets would make it O(log n) per onset; not worth it until a second stage shares the budget.
- Shaped glides 22–31 µs for 64 voices vs Linear 7.6: exact `exp2` per sample; a per-segment polynomial / recurrence
  would be cheaper and inaudible, but only matters with many simultaneous glides.

## Test gaps
- Insert / remove a note device before a running arp (M2); two chained arps through Flatten (`compose`).
- Live tuning drag with Ratio / Harmonics / Layers steps sounding (H1); Hold-latched set + tuning edit (H2).
- Mailbox: post while a take is in flight (M1) — a deterministic interleaving test on `ArpTableMailbox`.
- Flatten / preview with automation or a cable on an arp param (H3); a clip whose tuning differs from the track's (H4).
- Flatten with `contentStart > 0`, notes outside the clip window, a muted / unplaced clip (M5, M6).
- Tempo change during playback with Hold on (M3); Chord Trigger with 128 held + 3 layers + Tie under RtScope timing (M4).
- Cable into a lane step (M8); project reload with a Hold-latched arp (M9).
- The preview golden through the real `renderTrackNoteStages` (L14); preview invalidation on tuning / wire edits (L10).
