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

## AR.3 · pulse: a polyrhythm you can see and click (engine + panel), after AR.2
Today: period = Base · h_ref / h, h from Harmonic / Proportion / Order, Cycle Base | Bars, Ref — evenly spaced
pulses only, and the model is opaque.
1. **Model**: a **Cycle** length (synced rate num/den of a whole note, default 1/1, up to 16 bars; follows the
   signature map as Bars did) and per held note a **division n**: the cycle is cut into n equal pulses (period =
   Cycle / n, exact rational ticks as today). n comes from **Auto** (the note's JI harmonic; a note without one uses
   its row) or **Rows** (row r = the note's rank by Hz, 8 rows, n 1 … 64, beyond 8 the rows repeat). Rate / Ref /
   Cycle mode / Proportion length / Order are deleted in pulse mode (Rate stays the pattern-mode rate).
2. **Per-row rhythm**: a 32-bit **mask** per row (a structural setting; pulse j plays when bit j mod 32 is set;
   default all on) and a **shift** per row (−0.5 … 0.5 of the row's period: offbeats, flams, swing against the
   others). Mask test is one AND per pulse; no table.
3. **Pulse view** (`arppulse` element, shown when Pulse is on, replaces the number boxes): one row per held note
   (nothing held: the first 4 rows, nominal), a dot per pulse across the cycle, the cycle's beats as faint lines,
   coinciding onsets linked, a playhead and lit dots from the status. Click a dot: mute / unmute (mask). Drag a row
   horizontally (or wheel): its division. Shift+drag: its shift. Row menu: Euclid k of n (writes the mask), Invert,
   Reset. Auto rows show their harmonic and are division-locked (mask / shift still editable).
4. Layers leave the Pulse fold (they apply in both modes): into the Pitch block.
5. `sketchArp` / pattern view / roll preview / Flatten follow the same engine code (no second implementation).
Tests: 8:9:10:12 onsets exact over 4 bars (kept), a masked + shifted row, block-size invariance, RtScope.
Row: engine / RT.

## AR.4 · step lanes + the panel (👁, views + document), after AR.1
1. **Lanes get room**: a lane that is ON is a tall row (Velocity / Gate / Chance / Glide ≈ 44 px, Transpose ≈ 96 px,
   Skip / Tie 22 px); a lane that is OFF is a 14 px strip with only its name switch. The panel's fit height follows.
2. **Transpose**: a grid, not bars — one row per unit step, the zero line marked, equave lines stronger (Degrees: the
   track table's equave; Harmonics / Ratio: every step labelled), the visible range auto-fits the values in use
   (minimum ± 7, grows in whole equaves up to ± 48) with the value written in the step. Wheel = ± 1, Shift+wheel
   = ± an equave, typed value on double-click, Alt+click = 0.
3. All lanes: hover readout (step, value in its unit), wheel fine-adjust, lane menu (right-click the lane name):
   Reset, Randomize (seeded by the device seed, one undo step), Shift ← / →, Reverse, Copy to lane…; the length
   handle shows its number while dragged.
4. **Panel pass** (own judgement, keep every capability reachable): one Rate control (a stepped rate menu 1/1 …
   1/128 with · / T, the n:m tuplet boxes behind a small "n:m" toggle), fewer bare number boxes, consistent knob
   sizes, sections titled, no control without a label, nothing overlapping at 1× and 2× (open AP lows L12 / L13 in
   docs/reviews/AP.md). Do not touch the Glide and Pulse sections' contents (AR.2 / AR.3 own them); leave their y
   bands as functions so the merges stay mechanical.
Row: views → `pq_tests_views` default tier (+ `pq_tests_document` panel tests); goldens re-recorded only for the arp
panel. REVIEW.md entry.

## Order
w1: AR.1 → AR.4. w2: AR.2 → AR.3. Shared files: `note_devices.cpp arpeggiatorPanel` and `arp_elements.cpp` — each
task edits only its own section / element class; the later merge rebases.
