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
- Status: open
