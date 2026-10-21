# Benchmark results

Release build only. One row per measurement, appended (never edited), so regressions stay visible.
Machine: i7-11700F (8C/16T, AVX2 + AVX-512), 16 GB, Windows 11, unless noted.

| Date | Commit | Benchmark | Config | Result | Notes |
|---|---|---|---|---|---|
| 2026-10-20 | a42bc88 | resolveHz/degree (31edo) | release | 4.92 ns/pitch (min 4.80) | one exp2 in double; 4096 ids/round; machine shared with 3 other builds |
| 2026-10-20 | a42bc88 | resolveHz/ratio | release | 2.14 ns/pitch (min 2.03) | int64→double divide × anchor |
| 2026-10-20 | a42bc88 | resolveHz/cents | release | 4.58 ns/pitch (min 4.04) | one exp2 in double |
| 2026-10-20 | a42bc88 | resolveHz/harmonic | release | 3.74 ns/pitch (min 2.39) | table lookup × fundamental |
| 2026-10-20 | a42bc88 | resolveHz/mixed (random kinds) | release | 7.00 ns/pitch (min 5.46) | unpredictable kind switch costs ~2 ns (branch mispredicts) |
| 2026-10-20 | a42bc88 | Rational/mul (checked) | release | 58.6 ns/op (min 54.3) | dependent chain of 4096 macro-style steps; 2 gcds + 4 divs + 2 checked 64×64; binary (Stein) gcd measured no faster (55–65 ns), reverted |
