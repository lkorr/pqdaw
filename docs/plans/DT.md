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
