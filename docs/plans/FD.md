# FD: lane cables, Spread, Matrix — build your own reverb (user 2026-11-28, SPEC §15 "lanes and the matrix")

Reference: Septabee's Matrix module (Lost Robot, youtube SpL5pTq7ChA); Schlecht & Habets, "On Lossless Feedback Delay
Networks" (arXiv 1606.07729) and "Scattering in Feedback Delay Networks" (arXiv 1912.08888, the velvet / delay
feedback matrix).

## Design (approved by the user 2026-11-28)
- **A lane cable is a width-N cable** (N = 2, 4, 8, 16 total, not per ear). Width is the existing inferred attribute
  (`graph/infer.h:15,40`: mono < N, mono broadcasts, two different N > 1 is an error; `kMaxWidth` 64; `pack` /
  `channel` atoms; one 1-lane op per channel, state per (key, channel)). NOT note-voice lanes.
- Every existing module on a lane cable runs once per lane, unchanged. One Delay Tap = N delay lines; one Filter = N
  filters.
- **Lanes** fans stereo out to N lanes (L → even lanes, R → odd; mono → all). **Merge** sums back to stereo (even → L,
  odd → R, scaled 1/√(N/2)). **Unpack / Pack** convert between a lane cable and N separate cables.
- **Spread**: a source with a different value per lane (min, max, shape). Into a Delay Tap's time: N lengths. Into an
  LFO's phase: decorrelated modulation.
- **Matrix**: lanes in, lanes out, orthogonal at every setting, so a feedback loop through it never gains energy.
  `Mix` 0 = identity (separate echoes) … 1 = Hadamard (full mixing). `Stages` = velvet mode: Mix → short per-lane
  delays → Mix …; the inside is an ordinary group the user can open and edit. No decay, no in/out distribution: the
  loop is ordinary patching (cable back with a gain < 1, as for the Delay Tap, SPEC 2026-11-12).
- **Guard**: a soft clip that is exactly linear in [−1, 1] and bounded above it, at the Matrix output, default on;
  also a Math op so it can go in any loop. The matrix guarantees itself; the guard covers what the user patches in
  (boosts, resonance).
- **Lane mask**: any module on a lane cable can be limited to some lanes; the rest pass through.
- **A lane cable cannot reach the mixer** (or any stereo-only consumer): refused with the usual visible refusal until
  it is merged.
- **Depth is how far you open the same patch**, not a mode: Reverb panel → its 5–8 modules → masks / inside the Matrix
  → Unpack. A panel knob is a wire to something inside.

## FD.1 Scheduler: delay bounds by interval analysis; FDN loops run in sub-blocks (graph; lane A)
Today a loop through a Delay Tap runs per sample: `lowerBound` (`src/graph/src/schedule.cpp:919-985`) knows only
Const, ranged Param, `sr`, `mul`, `add`, `max`, `sub` of a const, `muladd`; the tap's time is
`lerp(Time, synced, Mode)` through a curve (`src/document/src/modules.cpp:1705-1709`), so the bound is unknown, the
delay floors at 1 sample (`schedule.cpp:1004-1005`) and `subBlockOf` (`schedule.cpp:992-1016`) returns 1.
- Replace `lowerBound` by an interval [lo, hi] propagated through the time expression: consts, ranged params, `sr`,
  add / sub / mul / muladd / min / max / lerp / clip / select, the curve atom's output range, bounded sources (sine and
  the other oscillators, phasor, `noise_step` by its range, tanh), and **per channel through `pack` / `channel`** (a
  per-lane time must not force per-sample scheduling). Unknown stays unknown (per sample), never optimistic: a wrong
  bound reads samples not yet written.
- **Cut choice**: the sub-block is the shortest bound among the delays that are CUT. A loop that contains long delays
  (the main taps) and short ones (the Matrix's stage delays, a chorus tap) must cut the long ones where that breaks
  every cycle and run the short ones in order inside the sub-block. Check what the scheduler does today and make the
  sub-block as long as the graph allows.
- Tests: an existing Delay Tap → VCA feedback loop now sub-blocks (assert the region's sub-block and bit-compare the
  render with the per-sample schedule, or within float noise if op order moves; memory: bit exactness not required
  where quality holds); a modulated tap (time · (1 + 0.01 · sine)) keeps a sub-block; per-lane times via `pack`;
  an unknown time stays per sample; a bound exactly at the block edge; an 8-lane FDN (pack of 8 delays + add/sub
  butterflies) schedules with sub-block = the shortest main delay.
- Bench (release A/B, then ship row in bench/RESULTS.md): Delay Tap feedback loop before / after; the 8- and 16-lane
  FDN per-sample vs sub-blocked.
- Verification row: DSP / JIT / engine (`-Affected -Slow`, ship `pq_bench_dsp` link once).

## FD.2 Width N through the document: lane cables, Lanes / Merge, Unpack / Pack (document + views; lane B)
Width > 2 has never run through a track. Known stereo assumptions (find the rest):
- a chain-path stereo signal into a meta device with mono outs is reduced to mid (`document/src/lower.cpp:2493-2503`);
- a same-track wire into a meta device's input is reduced to mid (`lower.cpp:2869-2878`);
- a wire into a param carries channel 0 only (`lower.cpp:2880`);
- the track starts at stereo (`lower.cpp:3504`); the `pack` atom block exposes only 2 inputs (`devices.h:107-108`).
Do:
- A width-N signal passes through groups, atom blocks and group modules (Delay Tap, Filter / EQ, VCA, Math, Noise,
  value blocks) per lane, no mid reduction. **A width-N wire into a param makes that param per-lane** (the module
  becomes N wide), in every wire mode (add / multiply / replace). Modules that are stereo by nature (pan, width,
  mixer strip, track output, sends) refuse width > 2: the existing wire-refusal / cut-wire path with a readable reason
  ("merge the lanes first").
- New modules (group-of-atoms, model: Delay Tap, `modules.h:42-56`, `modules.cpp:99,1649-1722`): **Lanes** (N chip
  2 / 4 / 8 / 16, default 8), **Merge**, **Unpack** (1 lane cable → N mono outs), **Pack** (N mono ins → 1 lane cable).
- Views: a lane cable is recognisable in the circuit / project patch view (thicker or a small N badge; pick what
  reads at a glance, one draw path for both views); node catalogue entries; small faces.
- Tests: lowering of a width-8 path through each module kind above (render: 8 impulses delayed by 8 per-lane times
  come out at the right samples after Merge); wide wire into a param in each mode; refusal at the mixer with its
  reason; Unpack → Pack round trip is identity; a feedback loop on the lane cable (Lanes → Delay Tap → VCA 0.5 → back)
  decays; stereo behaviour of every touched path unchanged (existing goldens must not move); an `RtScope` test on the
  new render path; placement of the new modules through real window events (memory: UI tests via real dispatch).
- Verification row: document model / lowering (`-Affected`).

## FD.3 Spread (after FD.2)
A source module, one output of width N. Params: `min`, `max` (automatable, wire targets), `shape`: Even (i/(N−1)),
Random (seeded, a Seed value), Scatter (mutually-prime-ish spacing for delay lengths: no two lanes in a small-integer
ratio; choose and document the rule). `N` chip; when its output is wired to a module on a lane cable it adopts that
cable's N if the document can tell (else the user sets it; a mismatch is the usual width error).
value_i = min + (max − min) · t_i, t_i constants per shape and seed, so FD.1's interval analysis sees through it.
Face: the N values as ticks on a line (the video's visualiser), min / max handles draggable. Tests: each shape's
values; Scatter's ratio rule; into Delay Tap time → N impulse times; face drag through real dispatch; one golden.
Verification row: one module's logic + its face.

## FD.4 Matrix + Soft Clip (after FD.2; needs FD.1 only for speed)
- **Mix**: log2 N stages of butterflies on `channel(x, i)` pairs, each a rotation by θ = Mix · 45°
  (a' = c·a − s·b, b' = s·a + c·b; c, s at param rate), repacked. Orthogonal for every θ, also while θ moves. Built
  from existing atoms so it fuses inline (no kernel call); if the measured cost says otherwise, one atom.
- **Stages** (1 … 4): a group containing [Mix → per-lane delay] × (Stages − 1) → Mix. Stage delays: constant per lane,
  ≥ 1 sample, distinct, spread over a `Smear` time (ms) that doubles per stage, chosen to decorrelate (velvet rule,
  cite the paper's choice). Opening the Matrix shows these blocks; follow how existing modules with a structural
  option rebuild their inside and what happens once the user has edited it (record as a Q).
- **Guard** toggle (default on): y = sign(x) · (min(|x|, 1) + e / (1 + e)), e = max(|x| − 1, 0): identity in [−1, 1],
  C1, bounded by 2, branch-free. The same expression is a new Math op "Soft Clip".
- Face: N chip, Mix knob, Stages, Smear, Guard; small and consistent with the Delay Tap / Math faces.
- Tests: ‖out‖² = ‖in‖² for random input at Mix 0, 0.3, 1 and while Mix is swept at audio rate, N = 2 … 16, every
  Stages (energy over the whole response); Mix 0 is identity, Mix 1 equals the Hadamard matrix up to signs / order;
  Lanes → Delay Tap (Spread times) → Matrix → VCA g → back: bounded for g = 1 over 60 s (lossless: energy constant
  within tolerance), decays for g < 1, and with g = 1.5 the guard keeps |out| ≤ 2; Soft Clip exact identity inside
  ±1; RtScope; face golden; controls through real dispatch.
- Verification row: DSP (`-Affected -Slow`, ship bench link) if a new atom, else module + lowering.

## FD.5 Lane mask (after FD.3 / FD.4)
A per-module setting on any module sitting on a lane cable: which lanes it processes. Lowering: pick the masked
lanes (`channel`) → run the module at that narrower width → repack with the untouched lanes passed through, so a
masked-out lane costs nothing (CPU rule), stateful or not. UI: a strip of N dots on the module, shown only when its
cable has lanes; click toggles, drag paints. All lanes on = no wrapping at all. Tests: allpass on lanes 0–3, lowpass
on 4–7 of one cable; mask change keeps the other lanes' filter state; dots through real dispatch.

## FD.6 Reverb (group preset + panel) (last)
A group, not a new engine: Lanes(8) → sum with feedback → Delay Tap (time × Spread Scatter) → damping filter →
Matrix (Mix 1, Stages 2) → decay gain → back; a Matrix (Stages 2–3) before the loop as input diffusion; Merge;
dry / wet. Panel: Size, Decay (seconds: per-lane gain 10^(−3·t_i / T60) from the lane's delay time, at param rate),
Damp, Mod (depth of a slow LFO with Spread phases on the tap times), Predelay, Mix. Presets Room / Hall / Plate as
saved inner patches behind the same panel. Measure: ship CPU per instance at 8 and 16 lanes against the Delay device;
RT60 within 10 % of the Decay knob at 500 Hz; no ringing mode > 6 dB above the mean in the tail spectrum (Scatter
check). 👁 REVIEW entry; the user's ear decides the presets.

## Lanes
FD.1 ∥ FD.2 → FD.3 ∥ FD.4 → FD.5 → FD.6.
