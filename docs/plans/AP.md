# AP: the arpeggiator (a note stage)

User 2026-11-20: "an arpeggiator that integrates and works well with the special type of piano roll / midi we made; it
should work generally like the ableton arpeggiator, and be placed after the tuning module but before the instrument
rack. add cool stuff to it, like pitch bending params (pitch into / out of curves) and other interesting features that
we can now pull off with this special type of piano roll."

Decisions (asked 2026-11-20):
- **A note stage**: a new, general kind of stage that takes notes and gives notes, RT-safe C++ on the audio thread over
  `pq::engine::NoteEvent`s, before the track's VoiceEngine. The arpeggiator is the first one; later note devices
  (chord, scale, note random, strum) use the same slot. Not built from circuit atoms; not part of the tuning block.
- **Pitch unit selectable per device**: scale Degrees of the note's tuning, Harmonics of its JI anchor (n → n + k),
  or an exact Ratio (e.g. ×3/2 per repeat, a twelfth instead of an octave). Identities stay exact.
- **v1 extras, all four**: harmonic-series arp, pulse / polyrhythm mode, step lanes, roll preview + Flatten to clip.
- Built as a plan + workers (lanes w1 / w2), after the user approves this plan.

## Where it sits (general mechanism, no per-type rule)
Notes are a signal (SPEC §15 2026-11-01): a MIDI band already draws `note source → tuning block → instrument` as a
Notes edge (`views/project_patch/project_patch.h` "tuning", `EdgeKind::Notes`). A note stage is a block whose main
input and main output are NOTES. It sits on that Notes link, after the tuning block and before the first block that
takes notes (an instrument, an Instrument Rack, a meta device fed the note source). Several note stages chain in
order. What makes a device a note stage is its declared input/output signal (Notes in, Notes out), never its type, so
`doc::chainStages` gets one new `ChainSignal` case (Notes → Notes passes notes on) and the rest follows from the
wiring.

Engine: per track slot the events are merged today (timeline + live input + keyboard → `EventBuffer` →
`VoiceEngine::process`, engine.cpp `onSlot`). The note stages run between the merge and the VoiceEngine:
`events → stage 1 → stage 2 → … → VoiceEngine`. A track with no note stage pays nothing (no call, no copy).

## Semantics (orchestrator defaults; note any change as a Q)

### Ableton parity (Live's Arpeggiator)
- **Style**: Up, Down, UpDown, DownUp, Up & Down, Down & Up, Converge, Diverge, Con & Diverge, Pinky Up, Pinky UpDown,
  Thumb Up, Thumb UpDown, Play Order, Chord Trigger, Random, Random Other, Random Once, **+ Harmonic Walk** (below).
  "Up" sorts by the held notes' Hz (pitch order, so it works in any tuning, also for notes with equal degree but
  different curves).
- **Rate**: Synced (1/1 … 1/128, with Straight / Dotted / Triplet **and any n-tuplet** n:m, e.g. 5:4, 7:8) or Free
  (ms). **Gate** 1 … 200 % (> 100 % overlaps: legato with glide). **Groove** (swing amount on every 2nd step).
- **Offset** (rotate the pattern start), **Repeats** (∞ or n), **Retrigger** Off / Note / Beat (with its own rate),
  **Hold** (latch; a new chord replaces it, notes added while one is still held join it).
- **Distance / Steps** (Live's Transpose): the pattern repeats Steps times, each shifted by Distance in the device's
  **Pitch unit** (Degrees / Harmonics / Ratio, below). Mode "Shift" / "Rotate" as in Live (Rotate: within the held
  set's span, up an equave each wrap).
- **Velocity**: Decay (ms), Target, Retrigger on/off; **Velocity source**: as played / fixed / step lane.
- **Clock**: synced steps sit on the song's grid (a step boundary is a multiple of the step length in ticks from tick
  0, plus Offset), so playback, render and the roll preview agree to the sample, at any block size. Stopped
  transport: a free-running internal clock at the song tempo (Live does the same), starting at the first note-on.

### Pitch units (exact identities)
- **Degrees**: degree + k in the note's own tuning (equave carries over). The new Hz comes from the tuning table that
  is already published to the audio thread with the params (TU.1 / T6.x-fix-ui L4). The event carries `degree` /
  `equave` / `tuning`, so a modulated tuning still drives the voice (T2.6).
- **Harmonics**: a note in a JI group (voice.harmonic h > 0, anchor Hz = hz · h_anchor / h, read from the event's
  ratio / harmonic) steps to h + k over the same anchor. A note with no group is treated as harmonic `Base` (a param,
  default 8) of itself. The event carries the new `ratio` / `harmonic` exactly (integers stay integers).
- **Ratio**: an exact p/q (two integer params, default 2/1); repeat j multiplies by (p/q)^j. Ratio stays a fraction
  in the device, Hz is float32 only at the end.

### Pitch into / out of curves (glides)
- **Into**: each step starts at a source pitch and glides to its own. Source = Previous step / Offset (in the Pitch
  unit or cents) / Off. **Time** (ms or note fraction), **Shape**.
- **Out**: over the last **Time** of each step's gate it bends toward a target: Next step / Offset / Off. Same
  Time / Shape params.
- **Shapes**: Linear (in cents), Exponential (bend amount), S-curve, Overshoot (one spring). The VoiceEngine's Pitch
  ramp today is linear in cents only: AP.3 adds a ramp shape to `NoteEvent` (in the free `pad2_`: shape id + bend
  byte), evaluated per sample in the voice's existing glide code. That is one switch per ramp per block (hoisted out
  of the sample loop), not per sample.
- **The held note's own curves carry over**: a Pitch / Gain event of a source note (drawn bends, vibrato, gain curves,
  retunes, M7) applies, as an offset ratio / gain, to every step that note is currently producing. So a bent chord
  arpeggiates bent, and the arp's own glide adds on top.

### Harmonic Walk (style)
For each held note taken as harmonic h of its anchor (Harmonics unit), the pattern is the harmonics h … h + Range (or
undertones: 1/h, 1/(h+1) …, the **Under** switch), not only the held notes. One held anchor plays an overtone run.
Several held notes interleave their walks in the chosen order (Up / Down / UpDown / Random over the union, merged by
Hz, duplicates once).

### Pulse mode (polyrhythm)
Off by default. On: no pattern order. Every held note repeats on its own clock, with period = Base period ·
(h_ref / h). h is the note's harmonic (JI group), else its degree index in a chosen proportion, else its order. So
8:9:10:12 held pulses as a phase-locked 8:9:10:12 polyrhythm. **Base** is a synced rate, h_ref the lowest harmonic
held (or a fixed param). **Phase lock**: all clocks restart together every **Cycle** (a param: the Base period · h_ref, where
integer harmonics coincide exactly, or every n bars), so the polyrhythm never drifts off the grid. Gate, glides, velocity
and step lanes apply per pulse. The SPEC §5 "duplicated octave/twelfth layers" become **Layers**: 0 … 3 extra copies
at an equave / ratio multiple (the Ratio unit's p/q), each with its own level.

### Step lanes
Up to 32 steps (length 1 … 32, independent of the pattern length: polymetric lanes, like Live 12's step-seq
transforms). Lanes: Velocity, Gate, Glide (scales Into/Out amount), Transpose (in the Pitch unit), Chance, and a Skip / Tie
row (tie = the step continues the previous note, gliding if Into is on). Each lane can be switched off. Data is
structural only in its on/off and length. Values are runtime params (a drag never recompiles), automatable like any
param.

### Roll preview + Flatten
- The arp's core is one pure, deterministic object (`ArpCore`: held-note set + params + clock position → events). It
  runs on the audio thread and also offline over a clip's notes. Random styles and Chance use a counter RNG seeded
  by (device seed, step index on the grid), so the preview equals playback.
- **Preview**: the piano roll shows the stage's output for the clip's own notes (not live input) as ghost notes with
  their glides drawn as bends, recomputed off the UI thread on a note / param edit (debounced, bounded to the visible
  range + one bar). A toggle in the roll's toolbar and on the device.
- **Flatten to clip** (device menu + roll command): writes the generated notes into the clip (one undo step), each
  with its exact identity (degree / harmonic + ratio), its velocity / gain, and its glides as note curves (M7 bends),
  and switches the device off. Same code path as the preview.

## Realtime and CPU
- `NoteStage` interface: one plain struct per kind, called through one function pointer per **block** (not per
  sample, not per event), preallocated state (held set ≤ 128 notes, pattern buffer ≤ 128 × Steps ≤ 8, pulse clocks
  ≤ 128, output EventBuffer preallocated at `maxEventsPerTrack`). No allocation, locks, virtuals in loops. `noexcept`,
  `// RT-safe`, RtScope allocation-trap tests for the stage entry point.
- Work per block is O(events + steps that start in the block). A step-free block with no input events costs one
  boundary compare.
- Generated note ids live in their own id space (high bit set + stage index) so they never collide with timeline,
  live-input or other stages' ids.
- Transport events: locate / loop wrap / stop → AllNotesOff from the timeline also resets the arp's clock and ends
  its steps at that frame (Hold keeps the latched set across a loop wrap, not across a stop).
- Params are read at block start (param rate). A step that starts inside a block uses that block's values.

## Tasks
| Task | Lane | Depends | What |
|---|---|---|---|
| **AP.1** | w1 | — | Engine: `pq/engine/note_stage.h` (NoteStage chain per slot between the merge and VoiceEngine, id space, transport resets), `ArpCore` with Ableton parity (styles, rates incl. n-tuplets, gate, groove, offset, repeats, retrigger, hold, velocity, Distance/Steps in Degrees / Ratio), source-expression carry-over. Tests: block-size invariance, loop / locate / stop, RtScope, styles against a hand table, a 128-note held set. Bench: per-block cost in `pq_bench_dsp` (idle, 8-note 1/32 at 48 kHz). |
| **AP.2** | w2 | AP.1's header (fixed by this plan; w2 may start with a stub-free interface commit from w1) | Document + lowering: a device declares Notes in / Notes out (`doc::chainStages` Notes → Notes), catalogue item `note:arpeggiator` "Arpeggiator" (Create menu, browser "Note effects"), insert on the Notes link after the tuning block (a drop onto the link, the Device view's chain left of the instrument), params as runtime params, settings structural; the image gets each track's stage chain. Project patch: the block on the Notes cable. Device view: a frame left of the instrument. Tests: document + lowering, a real-dispatch drop onto the Notes link. |
| **AP.3** | w1 | AP.1 | Pitch: Harmonics unit, Harmonic Walk (+ Under), Into / Out glides with the four shapes (NoteEvent ramp shape, VoiceEngine glide code), tuning-table lookup for Degrees under a modulated tuning. Tests: exact ratio / harmonic on the events, glide shape samples, modulated-tuning degree step. DSP row: -Affected -Slow + ship link of pq_bench_dsp. |
| **AP.4** | w1 | AP.3 | Pulse mode (per-note clocks, phase lock, Layers). Tests: 8:9:10:12 onsets exact on the grid over 4 bars, block-size invariance. |
| **AP.5** | w2 | AP.2, AP.1 | Step lanes (engine + document: lane data, length, on/off; runtime params per step). Tests: polymetric lane vs pattern lengths, tie + glide, chance determinism. |
| **AP.6** 👁 | w2 | AP.2, AP.5 | Device panel (1:1 in the Device view, BR.I): Live-like left block (Style, Rate / sync / tuplet, Gate, Groove, Offset, Repeats, Retrigger, Hold, Velocity), Pitch block (unit, Distance, Steps, Ratio p/q, Base / Range / Under), Glide block (Into / Out: source, time, shape with a drawn curve), Pulse block, the step-lane editor (bars per lane, length handle), and a live pattern view (the held notes and the current step lit). Goldens + real-dispatch tests. REVIEW.md entry. |
| **AP.7** 👁 | w1 | AP.4, AP.5 | Roll preview (ghost notes + glide bends, off-thread, debounced, toggle) and Flatten to clip (identities, curves, one undo step, device switched off). Tests: preview == offline render of the track's events; flatten round-trip plays the same events as the device. |

Verification per CLAUDE.md: AP.1 / AP.3 / AP.4 are engine / RT rows (-Affected -Slow, ship link of pq_bench_dsp);
AP.2 / AP.5 document rows (-Affected); AP.6 / AP.7 views rows (pq_tests_views) plus the one engine test AP.7 adds.

## Open (decided by default; workers file a Q if it bites)
- Arp output feeding an **Instrument Rack**: the rack's key zones read `voice.noteHz` / `noteDegree` of the
  GENERATED note (zones follow the arp, as in Live).
- **MIDI out / recording**: Capture MIDI and recording store the INPUT notes (as in Live), not the arp output; Flatten
  is the way to get the output.
- **MPE / MTS-ESP out** to external synths: unaffected in v1 (no external-instrument path exists yet).
