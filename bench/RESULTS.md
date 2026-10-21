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
| 2026-10-20 | ac48462 | exp2: std / fast / fast8 | release, pq_bench_fast_math | 2.03 / 1.53 / 0.219 ns/elem median | 4096-elem buffer, x in [-20,20]; the std loop is auto-vectorised by MSVC |
| 2026-10-20 | ac48462 | log2: std / fast / fast8 | release, pq_bench_fast_math | 2.25 / 2.08 / 0.301 ns/elem median | x in [1e-3,1e5]; includes special-value blends |
| 2026-10-20 | ac48462 | sin: std / fast / fast8 | release, pq_bench_fast_math | 5.35 / 1.49 / 0.208 ns/elem median | x in [-10,10] |
| 2026-10-20 | ac48462 | tan: std / fast / fast8 | release, pq_bench_fast_math | 3.39 / 1.62 / 0.223 ns/elem median | x in [0,1.57]; one division |
| 2026-10-20 | ac48462 | tanh: std / fast / fast8 | release, pq_bench_fast_math | 7.20 / 1.60 / 0.246 ns/elem median | x in [-5,5]; one division |
| 2026-10-20 | ac48462 | centsToHz: precise(double) / fast / fast8 | release, pq_bench_fast_math | 3.36 / 1.84 / 0.261 ns/elem median | cents in [-6000,6000] |
| 2026-10-20 | ac48462 | hzToCents: precise(double) / fast / fast8 | release, pq_bench_fast_math | 8.81 / 2.53 / 0.359 ns/elem median | hz in [20,20000] |
| 2026-10-20 | ac48462 | dbToGain: precise(double) / fast / fast8 | release, pq_bench_fast_math | 11.0 / 1.80 / 0.249 ns/elem median | dB in [-100,24] |
| 2026-10-20 | ac48462 | gainToDb: precise(double) / fast / fast8 | release, pq_bench_fast_math | 4.07 / 2.26 / 0.323 ns/elem median | gain in [1e-5,4] |
| 2026-10-20 | ac48462 | tempo ticksToSamples (1000 segs / constant) | release, pq_bench_time | 44.5 / 3.43 ns/call median | random ticks; a ramp segment costs one log1p |
| 2026-10-20 | ac48462 | tempo samplesToTicks / firstTickAtOrAfterSample | release, pq_bench_time | 54.3 / 148 ns/call median | 1000 segments, random samples; per block/event, not per sample |
| 2026-10-20 | ac48462 | timesig ticksToBarBeat | release, pq_bench_time | 8.32 ns/call median | 5 changes |
| 2026-10-20 | 344dccf | buildMosScale n=7 (5L 2s gens) | release, pq_bench_mos | 350 ns/scale median (min 347) | UI/document path, not RT: chainSafeGen check (n fmods), sort, 3 vector allocations |
| 2026-10-20 | 344dccf | buildMosScale n=24 (random gens) | release, pq_bench_mos | 916 ns/scale median (min 910) | |
| 2026-10-20 | 344dccf | MosLinearTable::lookup n=7 | release, pq_bench_mos | 5.0 ns/lookup median | binary search over 18 Farey boundaries; random g (mispredicts) |
| 2026-10-20 | 344dccf | MosLinearTable::lookup n=24 | release, pq_bench_mos | 23.9 ns/lookup median (min 22.9) | 183 boundaries; random g; block-rate cost for T2.6 |
| 2026-10-20 | 344dccf | lookup + evaluateLinearForm n=7 / n=24 | release, pq_bench_mos | 1.44 / 1.19 ns/degree median | per degree incl. amortised lookup; evaluation alone is one FMA-shaped expression per degree |
| 2026-10-20 | 344dccf | buildMosRegions maxSize=18 | release, pq_bench_mos | 7.7 µs/build median | region chart rebuild (UI) |
| 2026-10-20 | 344dccf | resolveHz/degree (MOS 5L 2s) | release, pq_bench_mos | 7.3 ns/pitch median (min 7.2) | table read + exp2; first version divided by n (10.2 ns), in-range fast path added |
| 2026-10-20 | 8faacb9 | drawlist/addRectFilled (10k) | release, pq_bench_ui | 12.1 ns/rect | 4 vtx + 6 idx; machine shared with 3 parallel builds, ±20 % noise |
| 2026-10-20 | 8faacb9 | drawlist/addRoundedRectFilled r=3 (10k) | release | 152 ns/rect | 32 vtx + 138 idx; was 309 before local write pointers + radius memo |
| 2026-10-20 | 8faacb9 | drawlist/addRoundedRectFilled r=3 (500, in cache) | release | 141 ns/rect | compute-bound, not bandwidth |
| 2026-10-20 | 8faacb9 | drawlist/addBlob (10k) | release | 13.4 ns/blob | textured glow sprite quad |
| 2026-10-20 | 8faacb9 | drawlist/test-pattern build 800×480 | release | 3.7 µs/frame | 804 vtx, 2574 idx |
| 2026-10-20 | 8faacb9 | renderer/upload test-pattern | release, RTX 3060 Ti | 0.64 µs/frame | system-RAM buffers; 4.0 µs with ReBAR device-local memory |
| 2026-10-20 | 8faacb9 | renderer/upload 10k rounded rects (12 MB) | release, RTX 3060 Ti | 0.44 ms/frame | 1.79 ms with ReBAR device-local memory |
| 2026-10-20 | 8faacb9 | offscreen/test-pattern 800×480 frame | release, RTX 3060 Ti | 71 µs/frame | upload + record + submit + fence wait (latency-bound) |
| 2026-10-20 | 8faacb9 | offscreen/10k rounded rects 1920×1080 frame | release, RTX 3060 Ti | 1.25 ms/frame | 2.04 ms with ReBAR device-local draw buffers |
| 2026-10-20 | 8faacb9 | pqdaw window idle (test pattern, no animation) | release | 0.0 ms CPU / 10 s | blocked in SDL_WaitEvent |
| 2026-10-20 | 8faacb9 | pqdaw --animate, 100 Hz display | release | 58–70 ms CPU/s (≈0.6 ms/frame) | MAILBOX 62.5, IMMEDIATE 65.6, FIFO 70.3 ms/s; nearly all driver acquire/submit/present, scene build is 4 µs |
| 2026-10-20 | 8faacb9 | pqdaw first frame | release | 189 ms after process start | from --window-selftest; SPEC target < 300 ms |
| 2026-10-20 | 938038b | patch IR build, 1010 nodes / 1501 wires | release, pq_bench_graph | 70.9 µs median (70 ns/node) | 8 params with attrs, 101 named + placed nodes; includes all vector growth (no reserve) |
| 2026-10-20 | 938038b | patch IR validateAll (valid graph) | release, pq_bench_graph | 21.3 µs median (min 19.1) | messages built lazily, so a valid graph allocates only the driver count + name sort arrays |
| 2026-10-20 | 938038b | patch IR adjacency (CSR) | release, pq_bench_graph | 7.1 µs median | driver per input port + out-wire CSR |
| 2026-10-20 | 938038b | patch IR operator== (structural) | release, pq_bench_graph | 121 µs median | canonical order (DFS) ×2 + sorted wire keys ×2; used by round-trip tests, not on the edit path |
| 2026-10-20 | 938038b | patch IR copy / copy + eraseNodes(10) | release, pq_bench_graph | 10.5 / 32.6 µs median | in-place compaction; the first version rebuilt into a new Patch: 77 µs |
| 2026-10-20 | 938038b | patch IR indexOf(NodeId) no erases / after an erase | release, pq_bench_graph | 2.0 / 12.3 ns/lookup median | one-probe guess (id − first id), else binary search bounded by the guess; plain binary search was 29 ns |
