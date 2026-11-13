# NZ: the noise module (user 2026-11-12, SPEC §15 "a noise module")

## NZ.1 Noise (one worker)
One module for every fundamental kind of randomness: audio noise and random modulation are the SAME signal, differing
only in how often a new value is drawn. One optional input (a clock), ONE output. Built like the other engine modules
since GU.7 (a group around one native engine `meta:noise`, `engineModuleBlocks` / `enginePanel`; catalogue item
`module:noise`; chain device + circuit), so opening it shows its insides.

**Signal path, in order** (each value drawn at a step; between steps the output holds or glides)
1. **Draw** a value per step: Distribution **Uniform** (−1 … 1) or **Gaussian** (σ = 1/3, e.g. a sum of 4 hashes,
   centred and scaled). Counter-based hash like today's `noise` atom (lowbias32: output independent of block size).
2. **Colour** (param, −6 … +6 dB/oct, 0 = white): a tilt filter run on the SEQUENCE of drawn values, i.e. once per
   step, not per sample (so slow modulation costs nothing per sample). At audio rate this is ordinary brown (−6) /
   pink (−3) / white / blue (+3) / violet (+6) noise; at low rate brown is a bounded random walk ("drift"). Spec:
   ±0.5 dB of the target slope over 20 Hz – 20 kHz at 48 kHz audio rate (scaled by the step rate below it); output
   RMS constant across Colour (normalised, so sweeping it does not jump in level). Suggested design: a few interleaved
   first-order pole/zero sections spaced geometrically (pinking-filter style), coefficients recomputed per block only
   while Colour or the rate moves; −6 a leaky integrator. Record the design and its measured error.
3. **Fold** into [−1, 1] by reflection (keeps the walk continuous, keeps every output inside the Range).
4. **Density** (param, Hz, exp slider; top = continuous): below continuous, each step keeps its value with
   probability density / step rate, else outputs 0 → dust / crackle / sparse impulses (an exciter for a plucked string).
5. **Steps** (param, 0 = off, else N levels across [−1, 1]; a count: not glided, TX.smooth rules).
6. **Smooth** (param 0 … 1): 0 = stepped (exact holds); s > 0 = glide from the previous value to the new one over
   s × the step period with a monotone C1 shape (smoothstep: no overshoot, never outside the Range). With an external
   or note clock the period is the last measured one.
7. **Range**: `min` / `max` params (default −1 / 1), output = lerp(min, max, (v + 1)/2). Wires add in units.

**When a step happens** (setting `clock`: Rate / Sync / Note; a wired clock input overrides all three)
- **Rate**: param in Hz, exponential, 0.01 Hz … the sample rate (top = a new value every sample = plain noise; in
  between it is also sample-rate-reduced noise). Fractional phase accumulation so non-integer periods are exact on
  average.
- **Sync**: a stepped note value 1/64 … 4/1 with dotted and triplet variants (beats; quarter = 1; from `pq::kPpq` if
  ticks are involved); steps land ON the grid: a new value when floor(transport.beat / beats) changes (stopped: hold).
- **Note**: one new value per note-on (`voice.trigger`) per voice: per-voice detune, humanising.
- **Clock input**: a rising edge (bool, or a real crossing above 0) draws a value.

**Determinism and voices** (settings)
- `seed` (integer param; changing it does not recompile).
- `restart`: **On play** (default: counter and filter state reset to the seed at the transport's start, so every
  playback and bounce is identical), **Free** (never resets: today's `noise`), **On note** (reset at each note-on:
  every note plays the same pattern).
- `voices`: **Per voice** (default: lanes seeded apart, today's rule) or **Shared** (one stream for all voices, the
  `shared(…)` semantics).
- Not included (user): stereo width. Mono out; stereo decorrelation is built in the circuit (two modules, two seeds).

**DSP / cost**: most likely one new atom (e.g. `noise_step`) with a SIMD kernel across voice lanes plus its JIT path;
the existing `noise()` atom stays as it is (stdlib uses it). Settings (distribution, clock mode, restart, voices) are
structural (const inputs: only the chosen path compiles). Targets per voice per sample (ship): Rate = SR, Colour 0,
Smooth 0 ≈ today's `noise` atom; Colour ≠ 0 at audio rate ≤ +8 multiply-adds; slow rates ≈ a compare + the glide.
Silence: it is a generator (promises nothing; stays awake when wired), like `noise`.

**Panel** (small, the VCA / Phase Accumulator family): knobs Rate (or the Sync value in Sync mode), Smooth, Colour,
Density, Steps, Min / Max, Seed; small choice chips Clock (Rate / ♪ / Note), Dist (U / G), Restart, Voices. A rich
`noise` display element: a scrolling trace of the output (per-voice dots, the usual `voice_tap` / Voice binding,
zero cost hidden) and a small slope line tinted by Colour (brown → pink → white → blue → violet). The Sync knob is a
plain stepped note-value control for now; DT.1's reusable tempo-marked slider replaces it if DT.1 has merged before
NZ.1 rebases (else a follow-up).

**Tests**: step timing sample-exact at a given rate (incl. fractional periods); sync steps on the grid at 120 / 90 bpm;
Note clock per voice; clock input edges; Smooth 0 exact holds, Smooth > 0 continuous and monotone between values;
Colour slope within ±0.5 dB at audio rate, constant RMS across Colour, brown at low rate stays in Range; Uniform and
Gaussian moments / histogram; Density rate statistics; Steps levels; On play bit-identical across two renders, Free
not; Per voice lanes differ, Shared identical; RtScope allocation test; interpreter == JIT; bench rows (ship); face
golden(s) looked at; panel gestures through real window events; REVIEW entry.
