# Questions for the user

Agents: add questions here instead of guessing on anything important. Use this format:

```
## Q<n> · <date> · <task> · <short title>
- Context: …
- Options: A) … B) …
- Provisional choice: … (why it's the most reversible)
- Status: open | answered (answer: …)
```

## Q1 · 2026-10-20 · T2.1 · Which reference does a `Cents` identity use?
- Context: SPEC §3.1 says `Cents` is "absolute cents from the reference" and "never moves" on tuning change, while §3.2 makes the reference pitch a parameter of each tuning (per project/tuning/clip).
- Options: A) the reference of the note's tuning context (so changing that tuning's reference moves Cents notes); B) a project-wide reference that tuning changes never touch.
- Provisional choice: A. `resolveHz` takes the reference from the `Tuning` passed in; Cents notes still ignore every other tuning change (EDO size, MOS generator…). Switching to B only changes which Hz the caller passes, not the stored identity.
- Status: open

## Q2 · 2026-10-20 · T2.2 · EDO row labels in the piano roll (SPEC §4.1)
- Context: "labelled with an equave index … otherwise `0\31`, `1\31`… with equave number" can be read as (a) the step within the equave as `k\n` plus the equave number shown separately, or (b) `e\n` where e is the equave index.
- Options: A) row label = `stepLabel(degree within equave, n)` plus the equave index (`edoRow()` gives both); B) markers labelled `e\n`.
- Provisional choice: A. `pq::tuning` exposes `stepLabel(k, n)` and `edoRow(step, n)` only; the UI composes the text, so B costs one format string.

## Q3 · 2026-10-20 · T0.5 · Straight 1/64 grid is not a whole number of ticks
- Context: PPQ 27720 = 2³·3²·5·7·11, so a whole note is 110880 ticks and 1/64 = 1732.5 ticks. SPEC §9.2 lists fixed grids down to 1/64. All 3/5/7/11-tuplets of 1/64 are exact (they span 1/32, 1/16 or 1/8), only the straight 1/64 is not.
- Options: A) keep PPQ 27720; straight 1/64 grid lines are computed from the whole note and rounded per line (alternating 1732/1733, no drift), time signatures with denominator 64 are rejected. B) double PPQ to 55440 (1/64 exact; still lcm-friendly; int64 range is no issue).
- Provisional choice: A (`noteValueIsExact(64) == false`, `noteValueTicks(64)` rounds, `TimeSigMap` rejects x/64). Reversible: PPQ is one constant, nothing is persisted yet.
- Status: open

## Q4 · 2026-10-20 · T0.5 · Tempo ramp law
- Context: SPEC §9.2 asks for tempo ramps but not their shape.
- Options: A) BPM linear in ticks (musical time), like tempo automation drawn on a beat-based arrangement (Ableton-style); seconds = closed-form log integral. B) BPM linear in seconds; ticks = quadratic in time.
- Provisional choice: A, implemented with log1p/expm1 so it stays accurate for tiny slopes. A ramp drawn as a straight line on the beat grid is then really straight. Reversible: `TempoCurve` is an enum, so a `LinearInTime` curve can be added beside it.
- Status: open

## Q5 · 2026-10-20 · T2.3 · mos-explorer: the CLI (mos-calc.js) and the app (index.html) disagree
- Context: mos-calc.js says it "mirrors index.html logic exactly", but its copies differ: `buildMOSScale` has no `chainSafeGen` nudge and sets s = 0 (not L) for an equalized scale; `computeHardness` returns a 4-digit string and no "1.00" case; `computeUDP` ignores brightness (always `n−1−root|root`). `generatorForHardness` exists only in the CLI and is built on its versions.
- Options: A) port both, the app's as the main API and the CLI's under `pq::tuning::mos_calc` (only what `generatorForHardness` needs); B) port only the app's and rebuild `generatorForHardness` on it (fixtures would then not come from the original code).
- Provisional choice: A, so every fixture is produced by the original JS. Switching `generatorForHardness` to the app functions later is a one-line change plus a fixture regeneration.
- Status: open

## Q6 · 2026-10-20 · T2.3 · Suspected mos-explorer bugs kept for fidelity
- Context: the C++ must match the JS exactly (T2.3 acceptance), so these are ported as-is:
  1. `rotateMOSScale` sets s = 0 when all steps are equal, while `buildMOSScale` sets s = L. `applyMODMOS` uses chroma = L − s, so on an equalized scale an alteration moves a note by a whole step for rotation ≠ 0 and by nothing for rotation 0.
  2. L and s are rounded to 0.01 ¢ before use, so the MODMOS chroma (and every hardness ratio, and therefore `generatorForHardness`'s bisection) is quantised to 0.01 ¢. MODMOS pitches are consequently not exactly linear in the generator.
  3. `updatePanel()` calls `updateHeldNotes()` before it transfers the mode rotation to a new MOS, so when a generator drag crosses into a region of a different size the held notes are re-mapped against a scale built with the stale rotation; on the next frame the size is unchanged, the cheap path runs, and the notes move to the transferred rotation's pitches (a jump). Read from the code, not reproduced in a browser.
  4. `chainSafeGen` reads the UI selection (`S.selected`), so the pitches of a degenerate generator depend on which region is selected. The C++ takes it as an explicit `RegionHint` parameter.
- Options: A) keep the port bit-faithful and fix in the JS first (then regenerate fixtures); B) fix in pqdaw and diverge from the fixtures.
- Provisional choice: A. For tuning-as-signal (T2.6), MODMOS cannot use the linear form exactly because of 2; T2.6 should decide whether audio uses the exact chroma (L − s unrounded).
- Status: open
