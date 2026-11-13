# DT: the delay tap meta module (user 2026-11-12, SPEC §15 "a delay tap meta module")

## DT.1 Delay tap (one worker)
A meta module built like every other one since GU (a group of blocks with a panel; editing it edits that group): one
main input, one main output = the input delayed. Stereo if the input is stereo. Nothing else in the signal path.

**Params (all automatable, each its own lane)**
- `time` (ms): stored as the slider position x ∈ [0, 1] like the VCA stores its knob position (Q183) or as ms; pick
  whichever makes automation lanes and wired modulation read best and record the choice. Mapping
  ms(x) = 62.5 · (e^(k·x) − 1), k = 2·ln 9 (ms(0.5) = 500, ms(1) = 5000). A wire adds in the param's own units (SPEC
  2026-11-01 "a wire adds its value in the target param's own units").
- `sync`: stepped note values 1/64 … 1/1 with dotted and triplet variants, as a duration in beats (quarter = 1 beat;
  derive ticks from `pq::kPpq`). Seconds = beats · 60 / bpm from the transport's per-block bpm (graph/schedule.h:415);
  clamp to 5 s (slow tempi, dotted whole notes).
- `mode`: ms / sync (enum), automatable.

**DSP**: the existing fractional delay line (cubic read; `DelayCubic` / split read-write kernels so a feedback loop
through the module works per sample). Buffer sized for 5 s at the project rate, preallocated off-thread; per-voice
lanes inside instrument groups come for free from the delay-line lane state. The read position follows the target
delay through a short one-pole smoother (~30 ms; tape-like pitch glide, no clicks) so automation, sync steps and mode
switches never step. Delay ≥ 1 sample. No per-sample exp: map x→samples per block or as a cheap control-rate op; the
per-sample cost should be the delay line plus the smoother.

**Face** (small, cute, resolution-independent, consistent with the VCA / phase accumulator faces)
- A horizontal slider. Above it: tick marks at each note value that falls inside 0 … 5 s at the current tempo (the
  tempo at the playhead, live as tempo changes); straight values (1/1 … 1/32) longer and labelled ("1/4"), dotted and
  triplet shorter and unlabelled; labels that would collide are hidden (shortest first).
- Below it: the readout, "375 ms" / "1.25 s" in ms mode; "1/8." / "1/4T" plus the resulting ms in sync mode.
- Gravity (ms mode, mouse drags only; automation and wires are never snapped): within ~6 px of a marking the knob
  sticks to it; it stays snapped until the pointer moves past that zone. A small magnet toggle turns it off (stored
  per instance, not automatable); holding the fine-adjust modifier the other controls use also bypasses it.
- In sync mode the same slider steps through the sync list (each note value at its ms position, so the knob sits where
  the ms marking is), with the label below.
- A small ms / ♪ toggle for `mode`.
- The slider is a reusable panel control (with tempo markings) other modules (LFO rate, future delays) can use from the
  GU control palette.

**Tests**: the mapping (0, 500 ms at 0.5, 5 s at 1, monotonic, inverse round-trips); sync → seconds at 120 / 90 bpm and
the 5 s clamp; delayed impulse lands at the right sample (integer and fractional), stereo; a feedback loop through a VCA
(0.5) decays as expected; time automation glides without a step; an `RtScope` allocation test; the face's drag with
gravity and with the magnet off through real window events (memory: UI tests via real dispatch); a face golden.

## DT.2 Delay tap sound: 1-sample minimum, click-free jumps (user 2026-11-12)
- **1-sample floor for `delay_cubic`** (src/dsp/src/kernels_memory.cpp:55–59): in [1, 2) samples read linearly
  (taps 1 and 2 samples old, so every tap stays ≥ 1 sample old and feedback loops stay legal); ≥ 2 the cubic read as
  now. At exactly 2 both give x[n−2], so the change is continuous. Per sample one compare + select (SIMD blend); bench.
  The Delay Tap's own clamp / docs (Q242 item 4) follow to 1.
- **No click on a jump.** The click is the 20 ms linear ramp sweeping the read head across a large jump (375 → 500 ms
  in 20 ms ≈ 6× speed). Continuous moves (drags, automation, wires, tempo drift) keep gliding; a DISCONTINUOUS change
  of the target (mode switch, a sync step, a click that jumps the slider) instead **crossfades two read heads** (old
  time → new time, ~30 ms, equal power), like Live's Delay "Fade" mode. Decide the jump test (e.g. a target change
  larger than the ramp can glide without a pitch beyond ±1 octave) and where it lives (a dedicated kernel or atoms in
  the group); the second read runs only during a fade (CPU rule). Expressible as atoms or one new atom; prefer the
  general mechanism other delays will reuse.
- Tests: impulse at 1, 1.5, 2 samples; a feedback loop at 1 sample through a VCA; mode switch and sync step with a
  sounding sine: no sample-to-sample step above what a 30 ms crossfade of two sines allows (measure the click: high-
  pass energy around the switch vs. a plain crossfade reference); continuous automation still glides (pitch, no fade).

## DT.3 Delay tap face: move anywhere, stretch, fraction names, more markings (user 2026-11-12) 👁
- **Move**: a press-drag anywhere on the face that is not the slider knob/track or a button moves the block, as for
  other modules (find why it doesn't: likely the tempo slider's hit area covers the face, or GroupPanelFace swallowing
  pointer-down). Check the other panel faces don't share the bug; fix it generally if they do.
- **Horizontal stretch**: drag the face's left or right edge (↔ cursor on hover) to widen it; height unchanged; stored
  per instance (like S-resize's scale), undoable; the slider fills the width. Make it a face capability others can opt
  into (a face declares it stretches horizontally), not a delay-tap special case.
- **Names**: every marking is a reduced fraction of a whole note: 1/4, 1/12 (not 1/8T), 3/16 (not 1/8.), 1/7, 2/7 …
  Sync readout the same.
- **Markings by simplicity and space**: candidates n/d with d ∈ {1,2,4,8,16,32,64}·{1,3,5,7}, small n, within 0–5 s at
  the tempo. Order by simplicity (powers of two with n = 1; then 3s (dotted, triplets); then 5s, 7s; then other
  numerators). Show a tick when it is ≥ ~4 screen px from every shown simpler tick, a label when its text clears the
  shown labels — in SCREEN pixels, so stretching or zooming the circuit both reveal more. Gravity pulls only to shown
  ticks. The sync lane's list stays the 21 values (renamed) unless the plan section on this is clearly wrong.
- Tests through real events: drag on the face body moves the block; edge-drag stretches and undo restores; at a wide
  stretch 1/7 is labelled and gravity snaps to it, at default width it is hidden and does not pull; goldens default +
  stretched.
