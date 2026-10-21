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
| 2026-10-20 | dee098e | audio callback: engine kernel alone (copy 480 frames 2ch) | release, pq_bench_audio_io | 35.4 ns/call median | baseline for the rows below (same kernel inside the path) |
| 2026-10-20 | dee098e | audio callback: CallbackRunner, 480 / 64 frames, no adapter | release, pq_bench_audio_io | 36.5 / 24.3 ns/callback median | RtScope + DenormalGuard (2 MXCSR writes) + 2 rdtsc + relaxed stats stores; ≈15–20 ns over the kernel, overlapped with the copy at 480 |
| 2026-10-20 | dee098e | audio callback: runner + BlockAdapter direct 480/32, 256/32 | release, pq_bench_audio_io | 150 / 86.6 ns/callback (0.31 / 0.34 ns/frame) | 15 / 8 engine calls per callback on offset pointers, no copies |
| 2026-10-20 | dee098e | audio callback: runner + BlockAdapter FIFO 480/64, 37/32, 1/32 | release, pq_bench_audio_io | 191 / 55.9 / 27.6 ns/callback (0.40 / 1.51 / 27.6 ns/frame) | one copy in + one copy out; 1-frame buffers are dominated by the per-callback fixed cost. All rows < 0.004% of the buffer period |
| 2026-10-20 | dee098e | pqdaw --sine 440 (WASAPI shared, Focusrite USB, 48 kHz, 256 frames, block 32) | release | callback mean 1.01 µs, max 13.7 µs; 0 xruns, 0 deadline misses over 3 s | the sine engine: one fastSin per sample + channel copy |
| 2026-10-20 | 0ff2b68 | dsp atom `add` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.081 ns/sample / 0.060 ns/voice-sample (0.482 ns/frame) | all inputs audio rate (Blk); 2× unrolled AVX2 loop; L1 is vectorised over time |
| 2026-10-20 | 0ff2b68 | dsp atom `sub` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.082 ns/sample / 0.062 ns/voice-sample (0.497 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `mul` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.081 ns/sample / 0.060 ns/voice-sample (0.479 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `div` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.129 ns/sample / 0.129 ns/voice-sample (1.032 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `min` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.083 ns/sample / 0.061 ns/voice-sample (0.489 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `max` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.083 ns/sample / 0.061 ns/voice-sample (0.486 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `mod` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.131 ns/sample / 0.133 ns/voice-sample (1.064 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `pow` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.851 ns/sample / 0.813 ns/voice-sample (6.507 ns/frame) | exact: MSVC SVML |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_pow` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.672 ns/sample / 0.617 ns/voice-sample (4.934 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `cents_to_hz` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 1.258 ns/sample / 1.236 ns/voice-sample (9.887 ns/frame) | exact: evaluated in double (SVML exp2_pd) |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_cents_to_hz` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.284 ns/sample / 0.249 ns/voice-sample (1.996 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `hz_to_cents` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 1.929 ns/sample / 1.908 ns/voice-sample (15.262 ns/frame) | exact: double log2_pd |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_hz_to_cents` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.437 ns/sample / 0.344 ns/voice-sample (2.750 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `neg` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.074 ns/sample / 0.064 ns/voice-sample (0.512 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `abs` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.074 ns/sample / 0.067 ns/voice-sample (0.538 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `sign` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.090 ns/sample / 0.097 ns/voice-sample (0.776 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `floor` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.080 ns/sample / 0.068 ns/voice-sample (0.542 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `frac` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.094 ns/sample / 0.085 ns/voice-sample (0.681 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `sqrt` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.154 ns/sample / 0.154 ns/voice-sample (1.229 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_sqrt` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.154 ns/sample / 0.154 ns/voice-sample (1.234 ns/frame) | = sqrt (hardware sqrt) |
| 2026-10-20 | 0ff2b68 | dsp atom `exp` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.284 ns/sample / 0.253 ns/voice-sample (2.027 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_exp` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.262 ns/sample / 0.250 ns/voice-sample (1.996 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `log` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.325 ns/sample / 0.290 ns/voice-sample (2.319 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_log` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.349 ns/sample / 0.291 ns/voice-sample (2.330 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `sin` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.343 ns/sample / 0.307 ns/voice-sample (2.459 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_sin` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.225 ns/sample / 0.208 ns/voice-sample (1.663 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `tan` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.492 ns/sample / 0.440 ns/voice-sample (3.523 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_tan` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.233 ns/sample / 0.223 ns/voice-sample (1.786 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `tanh` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.948 ns/sample / 0.944 ns/voice-sample (7.556 ns/frame) | exact: SVML |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_tanh` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.259 ns/sample / 0.222 ns/voice-sample (1.773 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `db_to_gain` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 1.354 ns/sample / 1.320 ns/voice-sample (10.561 ns/frame) | exact: double exp10_pd |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_db_to_gain` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.260 ns/sample / 0.238 ns/voice-sample (1.907 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `gain_to_db` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 1.547 ns/sample / 1.546 ns/voice-sample (12.366 ns/frame) | exact: double log10_pd |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_gain_to_db` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.334 ns/sample / 0.278 ns/voice-sample (2.223 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `const` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.074 ns/sample / 0.057 ns/voice-sample (0.459 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `clip` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.118 ns/sample / 0.083 ns/voice-sample (0.663 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `lerp` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.118 ns/sample / 0.089 ns/voice-sample (0.716 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `select` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.120 ns/sample / 0.093 ns/voice-sample (0.746 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `muladd` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.117 ns/sample / 0.078 ns/voice-sample (0.621 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `and` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.081 ns/sample / 0.062 ns/voice-sample (0.497 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `or` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.081 ns/sample / 0.062 ns/voice-sample (0.497 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `xor` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.081 ns/sample / 0.063 ns/voice-sample (0.500 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `nand` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.085 ns/sample / 0.068 ns/voice-sample (0.547 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `nor` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.085 ns/sample / 0.069 ns/voice-sample (0.549 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `not` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.074 ns/sample / 0.064 ns/voice-sample (0.512 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `gt` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.084 ns/sample / 0.059 ns/voice-sample (0.475 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `lt` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.084 ns/sample / 0.060 ns/voice-sample (0.478 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `ge` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.084 ns/sample / 0.061 ns/voice-sample (0.490 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `le` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.084 ns/sample / 0.062 ns/voice-sample (0.499 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `eq` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.085 ns/sample / 0.063 ns/voice-sample (0.505 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `ne` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.084 ns/sample / 0.062 ns/voice-sample (0.494 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `gate_to_real` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.074 ns/sample / 0.065 ns/voice-sample (0.518 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `mux` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.392 ns/sample / 0.361 ns/voice-sample (2.884 ns/frame) | 3 data inputs, Spl/Blk chosen at run time |
| 2026-10-20 | 0ff2b68 | dsp atom `demux` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.413 ns/sample / 0.389 ns/voice-sample (3.113 ns/frame) | 4 outputs |
| 2026-10-20 | 0ff2b68 | dsp atom `schmitt` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.723 ns/sample / 0.091 ns/voice-sample (0.729 ns/frame) | stateful logic: L1 per-sample recurrence, L8 one __m256 per frame |
| 2026-10-20 | 0ff2b68 | dsp atom `rise` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.116 ns/sample / 0.066 ns/voice-sample (0.530 ns/frame) | L1 vectorised over time (shifted load) |
| 2026-10-20 | 0ff2b68 | dsp atom `fall` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.116 ns/sample / 0.066 ns/voice-sample (0.524 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `change` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.116 ns/sample / 0.066 ns/voice-sample (0.528 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `srff` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.477 ns/sample / 0.074 ns/voice-sample (0.589 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `dff` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.600 ns/sample / 0.091 ns/voice-sample (0.727 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `tff` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.573 ns/sample / 0.088 ns/voice-sample (0.707 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `counter` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 2.524 ns/sample / 0.342 ns/voice-sample (2.738 ns/frame) | all 4 inputs audio rate |
| 2026-10-20 | 0ff2b68 | dsp atom `clockdiv` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 1.346 ns/sample / 0.181 ns/voice-sample (1.445 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `pulse` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.966 ns/sample / 0.142 ns/voice-sample (1.134 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `timer` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.823 ns/sample / 0.128 ns/voice-sample (1.024 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `z1` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.084 ns/sample / 0.027 ns/voice-sample (0.218 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `delay` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.651 ns/sample / 0.476 ns/voice-sample (3.809 ns/frame) | max 4096; L1 writes 8 ahead and reads with gathers |
| 2026-10-20 | 0ff2b68 | dsp atom `delay_cubic` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 1.259 ns/sample / 1.029 ns/voice-sample (8.231 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `delay_allpass` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 2.639 ns/sample / 0.506 ns/voice-sample (4.050 ns/frame) | per-sample recurrence (y1) and one division |
| 2026-10-20 | 0ff2b68 | dsp atom `samplehold` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.601 ns/sample / 0.091 ns/voice-sample (0.729 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `latch` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.536 ns/sample / 0.079 ns/voice-sample (0.629 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `z1:read` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.517 ns/sample / 0.065 ns/voice-sample (0.520 ns/frame) | split kernel, normally called with frames = 1 (cost there is the call) |
| 2026-10-20 | 0ff2b68 | dsp atom `z1:write` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.020 ns/sample / 0.002 ns/voice-sample (0.019 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `delay:read` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 2.192 ns/sample / 0.657 ns/voice-sample (5.254 ns/frame) | split kernel, per-sample path; normally called with frames = 1 |
| 2026-10-20 | 0ff2b68 | dsp atom `delay_cubic:read` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 5.370 ns/sample / 1.357 ns/voice-sample (10.856 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `delay_allpass:read` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 3.872 ns/sample / 0.737 ns/voice-sample (5.897 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `delay:write` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.765 ns/sample / 0.118 ns/voice-sample (0.947 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `phasor` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.584 ns/sample / 0.326 ns/voice-sample (2.608 ns/frame) | Blk hz: exact increment in double per sample; L1 uses an in-register prefix sum |
| 2026-10-20 | 0ff2b68 | dsp atom `sine` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.322 ns/sample / 0.301 ns/voice-sample (2.409 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `sine_hz` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.925 ns/sample / 0.609 ns/voice-sample (4.875 ns/frame) | phasor + fast_sin fused |
| 2026-10-20 | 0ff2b68 | dsp atom `wavetable` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.769 ns/sample / 0.559 ns/voice-sample (4.471 ns/frame) | 2048-point saw, 11 mip levels, mip per sample from Δphase, 2 gathers |
| 2026-10-20 | 0ff2b68 | dsp atom `noise` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.191 ns/sample / 0.164 ns/voice-sample (1.315 ns/frame) | counter hash, block-partition independent |
| 2026-10-20 | 0ff2b68 | dsp atom `shaper` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.580 ns/sample / 0.431 ns/voice-sample (3.450 ns/frame) | 1025-point curve, 2 gathers |
| 2026-10-20 | 0ff2b68 | dsp atom `sample_read_linear` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 7.353 ns/sample / 1.034 ns/voice-sample (8.270 ns/frame) | per-sample 32.32 playhead incl. rate→fixed conversion; looping |
| 2026-10-20 | 0ff2b68 | dsp atom `sample_read_cubic` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 9.098 ns/sample / 1.426 ns/voice-sample (11.405 ns/frame) | per-sample 32.32 playhead; 4 gathers |
| 2026-10-20 | 0ff2b68 | dsp atom `sample_pos_linear` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.512 ns/sample / 0.431 ns/voice-sample (3.446 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `sample_pos_cubic` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.973 ns/sample / 0.865 ns/voice-sample (6.916 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `add (blk,spl)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.066 ns/sample / 0.053 ns/voice-sample (0.426 ns/frame) | buffer ⊗ param: second input one vector load per block |
| 2026-10-20 | 0ff2b68 | dsp atom `mul (blk,spl)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.065 ns/sample / 0.042 ns/voice-sample (0.337 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `div (blk,spl)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.128 ns/sample / 0.128 ns/voice-sample (1.025 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `gt (blk,spl)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.067 ns/sample / 0.053 ns/voice-sample (0.427 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `fast_cents_to_hz (blk,spl)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.282 ns/sample / 0.255 ns/voice-sample (2.036 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `muladd (blk,spl,spl)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.078 ns/sample / 0.066 ns/voice-sample (0.532 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `phasor (spl hz, spl reset)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.151 ns/sample / 0.075 ns/voice-sample (0.597 ns/frame) | unmodulated pitch: L1 vectorised over time |
| 2026-10-20 | 0ff2b68 | dsp atom `phasor (blk hz, spl reset)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.546 ns/sample / 0.272 ns/voice-sample (2.177 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `sine_hz (blk hz, spl reset)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.880 ns/sample / 0.551 ns/voice-sample (4.411 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `sine_hz (spl hz, spl reset)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.409 ns/sample / 0.247 ns/voice-sample (1.978 ns/frame) |  |
| 2026-10-20 | 0ff2b68 | dsp atom `delay (spl delay)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 0.649 ns/sample / 0.432 ns/voice-sample (3.456 ns/frame) | param-rate delay time |
| 2026-10-20 | 0ff2b68 | dsp atom `sample_read_cubic (spl rate, spl reset)` (L1 / L8) | release, pq_bench_dsp, 64-frame blocks | 4.935 ns/sample / 0.951 ns/voice-sample (7.604 ns/frame) |  |
