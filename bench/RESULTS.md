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
| 2026-10-20 | 240390d | spsc push+pop (1 thread, batch 64) | release, pq_bench_core | 2.4–4.6 ns/push+pop median (min 2.1) | uint64 items, cap 1024; 3 runs, machine shared with 3 other build agents |
| 2026-10-20 | 240390d | spsc push+pop (1 thread, alternating) | release, pq_bench_core | 2.2–4.3 ns/push+pop median (min 2.2) | push then pop of one item |
| 2026-10-20 | 240390d | spsc 1P/1C throughput | release, pq_bench_core | 3.7–4.9 ns/item median (min 2.8); one outlier run 24.9 | 10^7 uint64, cap 1024, two busy-spinning threads, no pinning |
| 2026-10-20 | 240390d | mpsc push+pop (1 thread, batch 64) | release, pq_bench_core | 10.5–13.2 ns/push+pop median | cost is the producer CAS (lock cmpxchg) |
| 2026-10-20 | 240390d | mpsc 1P/1C throughput | release, pq_bench_core | 25–28 ns/item median (min 19.3) | 10^7 uint64, cap 1024 |
| 2026-10-20 | 240390d | mpsc 4P/1C throughput | release, pq_bench_core | 87–89 ns/item median | producers contend on the enqueue CAS |
| 2026-10-20 | 240390d | mutex+deque push+pop (baseline) | release, pq_bench_core | 51.5 ns/push+pop median | single thread, uncontended std::mutex; for scale only |
| 2026-10-20 | 240390d | fixed pool create+destroy | release, pq_bench_core | 2.4 ns median | 64-byte objects, batch 64 |
| 2026-10-20 | 240390d | arena allocate (64-byte aligned) | release, pq_bench_core | 1.25 ns/alloc median | batch 64 then reset |
| 2026-10-20 | 240390d | epoch quiescent (reader side) | release, pq_bench_core | 0.72 ns/call median | one acquire load + one release store |
