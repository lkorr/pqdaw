# EQ plan: the filter / EQ module — an `svf` atom, analytic responses, band groups, reusable displays

Status: **drafted 2026-11-07 from the user's answers (SPEC §15 2026-11-07 "the filter / EQ module"); awaiting the
user's yes on the structure below.** Supersedes docs/plans/GU.md "Next native ingredients" → Filter and EQ (a native
`meta:filter` engine cannot be opened, so custom filters would be impossible). Phases are `EQ.n` in TASKS.md.

## Goal

The base filter / EQ every module will use. An EQ is a group; each node on its curve is a **band**: a child group
holding a filter circuit made of atoms, so the user can open a band, rebuild it, or save a new band type, and the
display draws it correctly with no extra code. The response display (magnitude + spectrum) and the phase display are
ordinary panel element kinds, reusable on any group.

## Structure (the part to approve)

```
EQ group (module:eq)
├─ value blocks: Mode (Stereo | L/R | M/S, literal), Output (dB), Zoom/display state is view state, not blocks
├─ In → matrix(Mode) ─┬─ chain A: band → band → … (series) ─┐
│                     └─ chain B: band → band → …          ─┴─ inverse matrix(Mode) → Output gain → Out
│     Stereo: chain A on the stereo wire (broadcast), chain B folded away (mux on the literal Mode).
│     L/R: A on ch 0, B on ch 1.  M/S: M = (L+R)/2 → A, S = (L−R)/2 → B, then L = M+S, R = M−S.
└─ band group (one per node; group preset `module:eq_band`)
     value blocks with a `bandRole` setting: Frequency, Q, Gain, Stages (literal), Type (literal)
     In → svf atom (mode ← Type, stages ← Stages, cutoff ← Frequency, q ← Q, gain ← Gain) → Out
     bypass = the band group switched off (GU.11: a switched-off group passes through and costs nothing)
```

- **Roles, not names** (as GU.f1's `rackRole`): the display finds a band's controls by `bandRole`; a band is any child
  group in a chain with a `Frequency` role block. A user band with only Frequency (and maybe Gain) works: the node
  drags what exists.
- Bands per chain: up to 32. Adding / deleting / retyping a band, Stages, Mode: structural (one recompile, the
  ≈ 11 ms swap fade). Frequency / Q / Gain: params (modulatable, automatable, cable targets).
- Switching Stereo → L/R or M/S the first time copies chain A into an empty chain B (one undo step).

## The `svf` atom (EQ.1)

One atom: `svf(x, cutoff, q, gain)` with const inputs `mode` (lp, hp, bp, notch, bell, lowshelf, highshelf, allpass)
and `stages` (1 … 64; bell and notch: 1). Linear, time-invariant for fixed params.

- **Core**: the TPT SVF (stdlib `filters/svf.pqp`'s Simper form: states are the analog filter's, so modulation does
  not click), with an **output mix** y = c_hp·hp + c_bp·bp + c_lp·lp + c_x·x, which realises any 2nd-order numerator.
- **Pre-warping / decramping**: the denominator is set from the matched analog poles and the mix coefficients from
  Vicanek's magnitude-matched 2nd-order designs ("Matched Second Order Digital Filters", 2016), so a bell or shelf
  keeps its analog shape up to Nyquist (no cramping at 15 kHz) while keeping the SVF's states. A `prewarp` const
  (matched default | bilinear) keeps the plain bilinear SVF available (cheaper coefficients, for audio-rate sweeps).
- **Stages**: N cascaded sections inside the atom (one per-sample loop, no 64 atom copies).
  - lp / hp: Butterworth-spread section Qs (Q_k = 1 / (2 cos((2k−1)π/4N))): −3 dB at the cutoff, 12·N dB/oct; the
    user's Q scales the highest-Q section by Q / 0.7071 (exactly Butterworth at 0.7071; N = 1 is plain Q).
  - lowshelf / highshelf: gain / N per section, section Qs Butterworth-spread, user Q as above (total gain stays put,
    N sets the steepness).
  - bp / allpass: N identical sections at the user's Q.
- Cutoff 0 … 20 kHz on the control; the atom clamps to [1e-3 Hz, 0.49·sr].
- **Cost**: coefficients once per block unless cutoff / q / gain are driven at audio rate; the cascade loop is
  voice-lane SIMD across channels / voices; benchmark rows per mode at stages 1, 8, 64 (ship).
- **`dsp/svf_design.h`**: the one coefficient designer and its exact H(e^jω) (from the very coefficients the kernel
  runs), used by the atom, the interpreter, the JIT and the response evaluator. Response == impulse-measured within
  0.01 dB / 0.1° for every mode, stages {1, 2, 8, 64}, cutoffs 20 Hz … 0.45·sr.

## Analytic responses (EQ.2)

`graph::linearResponse` evaluates H(e^jω) of a lowered graph between a source node and a sink, at a list of
frequencies, from the graph alone — so any linear circuit (a band, a whole EQ with its M/S matrix, a user's comb)
draws exactly, without audio.

- Each atom def gets an optional **response rule** (complex gain per input at ω, given its const inputs and current
  param values): add / sub / neg / mul-by-a-non-signal / muladd / lerp(const t) / z1 / delay (e^−jωD) / channel /
  pack / settled gain / mux with a literal index / `svf` (from `svf_design.h`). An atom with no rule is nonlinear:
  its small-signal slope at 0 is used where defined (tanh, clip → 1) and the result is flagged **approximate** (the
  display draws it dashed); otherwise the response is **unavailable** (no curve).
- Evaluation: topological over the graph; strongly connected components (z1 / delay feedback) solved per frequency by
  complex Gaussian elimination over the SCC only. Cost O(nodes × points) for feed-forward bands.
- Lowered once per structural revision of the group (off the UI thread, like the project compile); a param change
  re-evaluates with the new values only (value blocks through their curves, `dsp::evalCurve`). Multichannel: per
  channel pair (in ch i → out ch j), so L/R and M/S chains are drawn per channel.
- SPEC §15 2026-11-01 said the curve is measured; the user chose analytic (2026-11-07). The measured response
  (`dsp::TransferAnalyzer`, existing) stays as an optional overlay chip, for nonlinear devices.

## Displays (EQ.4)

Two element kinds, bound to a group (`panelBinding`-style group id + an optional Signal binding for the analyser):

- **`response`**: x = log(1 + f / f₀), f₀ = 10 Hz, 0 Hz … 20 kHz (0 Hz at the left edge; near-linear below ≈ 10 Hz,
  log above); y = dB, ±24 default, zoom ±6 / 12 / 24 / 48. Draws: the analyser spectrum behind (existing
  `devices::SpectrumPlot` / signal tap on the group's Out; input spectrum dimmed, a chip toggles it), the grid, each
  band's own curve (faint, filled to 0 dB in its colour), the total curve, the nodes. In L/R or M/S the inactive
  chain's total and nodes are drawn at ≈ 35 % alpha behind the active one; a `tabs` element (L | R or M | S, shown when
  Mode ≠ Stereo) picks the active chain. Gestures (real dispatch tests): double-click empty space → add a bell there
  (freq, gain from the pointer); drag a node → frequency (x) and gain (y; Q for gainless types); wheel → Q;
  Alt + wheel → stages; Shift → fine; double-click a node → reset gain; right-click a node → Type, Stages, Bypass,
  Solo, Delete; Delete key → delete the selected band. Edits go through the bound value blocks (one undo step per
  gesture), so modulation rings / automation / cables behave as on knobs.
- **`phase`**: the same x axis; unwrapped phase in degrees, auto-scaled (default), or group delay in ms (toggle). The
  total of the active chain, the selected band faint.
- **Band strip**: per band a column of ordinary elements bound to its value blocks (type menu, Hz, Q, gain slider or
  stages); added / removed / relaid out by the band ops in the same undo step; the selected node highlighted.
- Solo = the other bands of the chain switched off while held (no special audition filter). Output gain knob.
- Reuse: any group can place `response` / `phase` bound to itself or a child (a synth's filter section shows its one
  band's curve for free).

## Ops

`doc::ops::addBand(group, chain, type, freq, gain)`, `removeBand`, `setBandType` (structural: the Type literal),
`setBandStages`, `setEqMode` (copies A → B when B is empty) — each one undo step, panel elements included. A panel
element needs a structural-edit hook: `PanelTarget::run(doc::Edit)` (the host runs it through its history).

## Phases

1. **EQ.1 `svf` atom** (dsp designer + response, graph atom, interpreter, JIT codegen, tests, ship bench).
2. **EQ.2 Linear response evaluator** (atom response rules, SCC solve, param re-evaluation, tests against impulse
   measurement of JIT-compiled random linear circuits). Runs beside EQ.1; EQ.1 adds the `svf` rule after rebasing.
3. **EQ.3 Band and EQ modules + ops** (module:eq_band, module:eq, Mode matrix, ops, PanelTarget::run). After EQ.1.
4. **EQ.4 `response` and `phase` elements, band strip, EQ panel** 👁. After EQ.2, EQ.3.
5. **EQ.5 Custom band types**: save a band group as a band preset (library kind), offered in the node's Type menu;
   a user-edited band draws exactly (or dashed when approximate); docs. After EQ.4.
6. **EQ.6 Milestone**: -Full, critique (docs/reviews/EQ-critique.md), retire `retired/` EQ Eight references in the
   test corpus where the new EQ replaces them.

## Not now

- **Phase lead / free-form phase** (user 2026-11-08, deferred until PDC, M6): a rising phase (an allpass "ramp up") is non-causal; done as MFreeformPhase-style FIR: drawn phase → IFFT of e^{jφ} → shift by N/2 + window → partitioned FFT convolution, N/2 reported as latency. Resolution costs latency; phase lead pre-rings. A "phase" band type (draw a phase, or invert any band) and/or a linear / lead EQ mode; the analytic response draws it (an FIR is delays and gains).

Dynamic EQ, per-band M/S inside one chain, linear-phase mode, oversampled HQ, Auto Filter (next, on the same atom).
