# AR: arpeggiator rework (placement, glide, pulse, lanes, panel)

User 2026-11-23: add it from the Tab module selector, only note cables connect to it; too many glide options — click
the note and drag to make glide shapes, the knobs conform; Pulse is locked to a couple of basic rhythms; step lanes
too small (Transpose especially); "glide doesn't seem to do anything"; then improve UI / options of our own volition.
Aim everywhere: the same results or more with fewer, more direct controls. No compatibility (delete old params).

Base: docs/plans/AP.md (AP.1 – AP.11), `pq/engine/arp.h` (the semantics header is the truth),
`document/src/note_devices.cpp` (`arpeggiatorPanel`), `views/src/panel/arp_elements.cpp` (steplane / arppattern /
arpbend), `document/src/note_pattern.cpp`, `engine/src/arp_sketch.cpp`.

## AR.1 · placement: Tab picker + note cables only (document + project patch)
General mechanism: everything follows the type's DECLARED main signals (`NoteDeviceType::in / out`), never its id.
1. `circuit::nodeCatalog` (node_catalog.cpp `build()`, Project scope): one entry per `doc::noteDeviceTypes()` in the
   existing, empty `NodeCategory::MidiEffects` tab (item Device, id `noteDevicePatch`, inputs {"notes"}, outputs
   {"out"}). Tab on empty canvas of a MIDI band places it free (off-path, unwired); on another band / inside a group
   the entry is refused with the reason shown (checks.cpp already refuses the document edit).
2. Tab over a **Notes cable** (canvas.cpp: today it acts as empty canvas) opens the insert menu with the entries
   whose main in and out are Notes; a pick splices the device onto that cable (the same edit the browser drop does).
   A cable dragged out of a notes output and dropped on empty canvas opens the spawn menu filtered the same way and
   wires the pick.
3. **Cable rule** (connectEnds → a new check beside `declaredWireClash`, by declared signal): a main input declared
   Notes takes only a cable that carries notes (the note source / tuning block, a Notes-out device's main output); a
   main output declared Notes goes only into an input that takes notes (an instrument / rack / module main input that
   reads the note source, another Notes-in device). Anything else is refused while dragging (the refused-cable look
   and a one-line reason), and `doc` checks refuse it as an edit. Cables into the device's PARAMS stay as they are
   (modulation, AP.2b).
4. A free arp with both ends wired note source → arp → instrument is a stage (already true through `mainPath`);
   make sure a half-wired one draws its cables amber on the wired side and plays through nothing (the instrument
   keeps its direct notes only when a direct cable exists).
Tests: real-dispatch Tab-place free + wire both ends → `lowerNoteChains` has the stage; Tab on a Notes cable inserts;
an audio cable onto the arp input is refused (view + document). Goldens: the node menu's MIDI effects tab.
Row: document / shared → `-Affected`.

## AR.2 · glide: draw it on the note (engine + panel) — DONE (as built; deviations: Q?-AR.2-a)
Found: "glide doesn't do anything" was (a) — the Glide LANE only scaled Into / Out, both Off by default (and its
steps defaulted to 1 = no change even with a side on). (b) was not broken: a full render (clip → timeline → ArpStage
→ VoiceEngine → the instrument's voice.hz) and the live keyboard path bend the voice's Hz with Into Prev; both are
kept as tests (tests/app/test_note_device_playback.cpp "AR.2").
1. **Params per side = what a drag sets**: `intoOffset` / `outOffset` (cents, ± 2400; 0 = that side off), `intoTime`
   / `outTime` (% of the note, as AP.11), `intoCurve` / `outCurve` (−1 … 1: 0 a line, ± an exponential bend; Spring:
   the overshoot), `intoPrev` / `outNext` (the side aims at the neighbouring step instead of its offset) and
   `intoShape` / `outShape` (Curve | S | Spring: S-curve and Overshoot kept, they are distinct sounds). Gone: the mode
   enum (`intoSource` / `outTarget`), the 4-entry Shape menu's Linear / Exponential split, the Bend knobs,
   `glideCents`. An offset is ALWAYS cents; a drag snaps to the pitch unit's steps of the nominal note (Shift: free).
2. **The bend view is the editor** (`arpbend`, 712 × 150, no Alt): the previous / next step as ghosts (schematic: a
   step below / above); the note's HEAD: up / down = Into offset (snapped; 0 = off), sideways = Into time, dropped on
   the previous step's ghost = `intoPrev`; the TAIL the same for Out / next; a knee dot = that time alone; the middle
   of a curve = its Curve. A press anywhere on the note takes its nearer end. Double-click an end: that side off;
   click a ghost: from / to that step on / off. One undo step per drag; readouts "In −2 deg · 30 %".
   The same head / tail drag works on any note of the **pattern view** (the real neighbours are its ghosts).
3. **Glide lane = slide per step** (303-style): 0 … 1, default 0; a step's value scales the pitch distance of the
   sides that are on, and with NO side on a step > 0 glides in from the previous step (over Into time). The lane
   switched off: glides in full.
4. Panel Glide section: the bend view; per side Amount + Time knobs that follow the drags and the shape chips.
Row: engine / RT → `-Affected -Slow` + ship link of pq_bench_dsp; arp panel goldens re-recorded.

## AR.3 · pulse: a polyrhythm you can see and click (engine + panel), after AR.2 — DONE (as built; deviations: Q?-AR.3-a)
Was: period = Base · h_ref / h, h from Harmonic / Proportion / Order, Cycle Base | Bars, Ref — evenly spaced pulses
only, and the model was opaque.
1. **Model**: a **Cycle** (num / den BARS of the song's signature, default 1 / 1, num ≤ 16, den ≤ 32: in 4/4 that is
   num / den of a whole note; it follows the signature map as Bars did, a fractional cycle being that fraction of its
   bar) and per held note a **division n**: the cycle is cut into n equal pulses (period = Cycle / n, exact rational
   ticks as before). n comes from **Auto** (on by default: the note's JI harmonic, ≤ 1024; a note without one uses its
   row) or its **row** (row r = the note's rank by Hz mod 8, n 1 … 64, defaults 3 … 10). Rate / Ref / Cycle mode /
   Proportion / Order are deleted (Rate stays the pattern-mode rate; Sync off / a stopped transport: the free clock
   from the note-on, the cycle at the song tempo). The old 8:9:10:12 result = Auto with a 1-bar cycle.
2. **Per-row rhythm**: a 32-bit **mask** per row (structural, stored as two 16-bit settings `pulseMaskLo/Hi n`; pulse j
   plays when bit j mod 32 is set; default all on; one AND per pulse, no table) and a **shift** per row (−0.5 … 0.5 of
   the row's period, a runtime param).
3. **Pulse view** (`arppulse`, shown when Pulse is on, replaces the number boxes): one row per held note, lowest at
   the bottom (at least 4, at most 8 rows; nothing held: the last held set, else nominal rows; rows without a note
   dimmed), a dot per pulse across the cycle, the cycle's beats as faint lines, coinciding onsets joined, a playhead and
   lit dots from the status. Click a dot: mute / unmute. Drag a row sideways (or wheel): its division. Shift+drag: its
   shift (snapped to 1/24 of the period; Alt: free). Right-click a row / its chip: Euclid k of n, Rotate Later / Earlier,
   Invert, All On, Reset Row. Auto rows show their harmonic ("h9") and are division-locked (mask / shift still edit).
4. Layers left the Pulse fold (they apply in both modes): a count and up to three level knobs in the Pitch block's last row.
5. `sketchArp` / pattern view / roll preview / Flatten run the same engine code (no second implementation); the view's
   rows come from `doc::notePulseView` (the engine's `arpPulseDiv`).
Tests: 8:9:10:12 onsets exact over 4 bars (1- and 2-bar cycles), a masked + shifted row, signature changes with
fractional cycles, block-size invariance, RtScope; the view through real dispatch (click, drag, Shift+drag, wheel,
menu, the Auto lock); goldens arp-panel-pulse, arp-panel-pulse-chord.
Row: engine / RT.

## AR.4 · step lanes + the panel (👁, views + document), after AR.1 — DONE (as built; deviations: Q?-AR.4-a)
1. **Lanes get room**: one `steplane` element per lane, the panel's width (940): a NAME COLUMN (84 px: the switch
   with the lane's name, the menu's dots, a readout) and 32 steps of ≈ 26 px. On: Velocity / Gate / Glide / Chance
   44 px, Transpose 96 px, Skip / Tie 26 px. Off: only the 14 px name strip — the element spans a `fold` of the new
   look "band" (no header, no press) whose condition is the lane's own switch, so the fit height follows (Device view
   and patch block). A tick between every four steps; Gate has a line at × 1.
2. **Transpose** is look `grid`: a row per step of the unit, the 0 row marked, in Degrees the track table's equaves
   ruled and labelled (`doc::noteDeviceTuning` / `tuningEquaveDegrees`), in Ratio / Harmonics a label every few rows.
   The rows shown fit the values: ± 7, else the whole equaves that hold every value (no equave: sevens), at most ± 48.
   Each step is a block on its row with a stem from 0 and its value written beside it. A press sets the row under
   the pointer; the rows are held while a drag runs and go on past the edge (so a drag reaches beyond ± 7). Wheel =
   ± 1, Shift+wheel = ± an equave, double-click = type the value, Alt+click = 0.
3. **All lanes**: the readout under the name (the step under the pointer / being dragged and its value in its unit:
   "75 %", "+7 deg", "Tie"; else "16 steps"), the hovered step outlined; wheel = 1 % (Shift 10 %; cells: the next
   state), a run of notches one undo step; double-click = type (bars: percent); Alt+click / drag = the default; the
   length handle shows its number while hot. The MENU (the dots, or a right-click on the name column; a right-click on
   a step stays the host's automation menu): Reset, Randomize (the steps in play; seed + lane + a view count, so
   asking again gives another one), Shift Left / Right and Reverse (within the length), Copy to (Velocity / Gate /
   Glide / Chance among themselves: values and length). Each one undo step, param writes (Copy also the length).
4. **Panel pass** (top band 200 px high, was 182):
   - ONE Rate control (`rate` element): a box "1/16", "1/8 T", "1/8 D", "3/16 5:4" — click: 1/1 … 1/128, wheel: the
     next / previous — with the Feel chips Straight / Dotted / Triplet beside it; its "n:m" chip shows the four number
     boxes (Note num / den, Tuplet n : m) in place of the chips (view state; shown anyway while the rate is none of the
     menu's). Free: Rate ms.
   - Arp block in columns: Style, Gate / Groove, Seed / Offset, Repeats / Hold | Sync + Rate, Retrigger (+ "Every"
     n / d on Beat), Velocity (source chips, "Decay restarts — On retrigger" as a switch: no second "Retrigger", no
     button glow; Decay / Target / Fixed knobs).
   - Every knob 48 × 58; no box without a label ("Every", "Note", "Tuplet", "Walk Range", "Walk Order"); the Harmonic
     Walk row lost its caption. Pitch block: the row y 136 … 194 is free for AR.3's Layers.
   - Glide and Pulse sections untouched (their y bands still run from the fold above).
Row: views → `pq_tests_views` default tier + `pq_tests_document`; goldens: the arp panel's + arp-panel-lanes-2x (new).

## Order
w1: AR.1 → AR.4. w2: AR.2 → AR.3. Shared files: `note_devices.cpp arpeggiatorPanel` and `arp_elements.cpp` — each
task edits only its own section / element class; the later merge rebases.
