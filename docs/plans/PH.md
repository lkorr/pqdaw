# PH: phase-locked unisons and per-note phase (idea, logged 2026-11-15; not scheduled)

The user, 2026-11-15: "Phase-locked unisons when notes bend into each other, and the ability to define the phase of each
note; you'll enter a phase mode or something and all of the notes that are JI objects / phase locked will become little
phase sliders from -180 to 180, and changing it will just adjust the pitch bend ever so slightly so that when the notes
enter the object they'll be at the appropriate phase. And this will be automatable as well; since the note only exists
for a moment though this automation would have to be a different type of automation from effect automation … it has to
be an automation that only can apply to that particular note during its lifetime. Maybe we can create a really sick way
to visualize / show the automation just within the piano roll, where you can show how the phase will change as just an
aspect of how the notes are drawn, in the same way that you're able to draw pitch shift and actually show bends."

This is a requirements sketch, not a task plan. Refine into tasks (and restate to the user) before starting.

## Desired effect
- Notes that glide into a unison (a JI group's members merging, bends between notes, a warp landing two notes on one
  pitch) meet at a **chosen phase relationship** instead of whatever phase they happened to drift to, so a merge is a
  clean coherent sum (0°, up to +6 dB), a deliberate partial cancellation, or a full null (180°), and a split leaves from
  that state. This is the "splits actually sound smooth" detail from the inspiration chat, under user control.
- Default: 0° (coherent), which is what the earlier object design did with its `Sync` event.

## How (the user's mechanism: steer by pitch)
- An oscillator's phase is the integral of its frequency. Moving a note's phase by Δφ over an approach of T seconds is a
  frequency offset of Δφ / (360° · T) Hz spread over the approach: 180° over 0.5 s is a 1 Hz detune. Inaudible as pitch at
  most register / glide combinations; at low pitches with short approaches it becomes a real detune (1 Hz at 55 Hz ≈ 31 ¢),
  so the approach length is part of the design (and the UI should show when a correction is audible).
- This needs **predictable phase**: the lowering must know each note's phase at any tick, so a note's oscillator phase
  starts at a defined value at note-on (uint32 phase already) and the phase at tick t = start phase + ∫ effective
  frequency (curve + bends + warps + vibrato), computed analytically from the effective curve at lowering. Chase, loops and
  seeks must arrive at the same phase as straight playback (start a chased note at its integrated phase, not at 0).
- The correction is just one more component of the note's effective pitch curve (beside bends, warps, vibrato): the
  lowering solves for the tiny offset curve over the approach window that makes the integrated phase difference equal the
  target at the moment of meeting. No new engine concept for the steering itself.
- The engine's `Sync` event (T7.7: copy a lane's state at a frame) stays the exact fallback for "identical lanes after the
  merge", e.g. for patches whose state is more than one oscillator phase.
- What "phase" means for a patch: the voice's primary oscillator phase (a `voice.phase` / phase-reset convention for
  instruments). Patches without a phase-driven oscillator (samplers, noise) ignore it; say so in the UI.

## Phase mode in the piano roll
- A mode toggle (key to be chosen against HOTKEYS.md; also a roll toolbar button). While on, every note that takes part
  in a unison relationship (JI group members, notes bent into another note) is drawn with a small phase slider,
  −180° … +180°, relative to the note it meets (its group anchor or bend partner). Drag to set; double-click resets to 0°.
  Notes that never meet another note show nothing (phase is meaningless alone).
- Feedback: hovering shows the resulting level of the merge ("+5.9 dB", "−∞") and, when the correction is audible, the
  detune it costs ("1.4 ¢ for 0.3 s").

## Per-note automation (note-scoped expression lanes)
- A new kind of automation that lives on one note and only exists during its lifetime: breakpoints in the note's own
  ticks, stored on the note (like `NoteCurves` bends / vibrato), moved / copied / deleted with it. Phase is the first lane;
  the same mechanism is the natural home for the per-note gain / pan / timbre curves already deferred in M7.
- Automated phase over time = a continuously steered frequency offset (d phase / dt), so it composes with everything else
  in the effective curve.
- Not the track automation lanes: those span the timeline and address device parameters.

## Visualising it in the note itself
- The phase should read as part of how the note is drawn, the way bends and vibrato already bend the line. Ideas to try
  (pick by looking at goldens):
  - a thin second strand twisting around the note line (a helix seen side-on) whose crossing points shift with phase;
  - the note's pill head rendered as a tiny phase dial, the line's colour hue rotating with phase along the note;
  - at a merge, the two meeting lines drawn interlocking (in phase) or as a dashed gap (in anti-phase).
- In phase mode, dragging along a note writes its phase lane directly (like the pen on pitch curves).

## Acceptance (when refined)
- Two notes bending into a unison with target 0° sum within 0.1 dB of a single note at twice the amplitude; at 180° the
  sum is below −60 dB; at 90° it is +3 dB; the same after chase / loop / seek into the merge; interpreter == JIT.
- The correction never exceeds a stated cents bound for default approach lengths; when it would, the UI says so.
- Phase lanes survive every note-id operation, undo, save / load.
