# Progress log

Newest entries at the bottom. Format is in CLAUDE.md ("Logging your work").

## 2026-10-20 · setup · initial session
- Did:
  - Wrote the spec (SPEC.md drafts 1–3), architecture, patch-language v0, task plan and agent rules.
  - Created the CMake/Ninja/MSVC skeleton, `pq_core` (CPU feature detection), and doctest.
  - Downloaded prebuilt LLVM 23.1.3 to `C:\dev\llvm-23.1.3`, plus zlib/zstd built from source to satisfy LLVM's exported deps.
  - Wrote the JIT smoke test.
- Verified: configure succeeds. Link initially failed: LLVM 23.1.3 references vectorised-STL symbols (`__std_search_1`, `__std_remove_8`, …) missing from MSVC 14.42 (VS 17.12). VS 2022 is being updated to the latest 17.x; see the next entry for the result.
- Toolchain facts: LLVM libs are `/MT`, `_ITERATOR_DEBUG_LEVEL=0`, so there is no Debug config. MSYS2 on PATH must be ignored (it supplied MinGW zlib at first). LLVMDebugInfoPDB needs a placeholder `DIASDK::Diaguids` target.
- Reference material moved to `reference/inspiration/` (git-ignored; the repo is public).

## 2026-10-20 · T0.1 · setup session
- Did: updated VS 2022 17.12.3 → 17.14.42 (MSVC 14.44.35207), which fixes the LLVM 23.1.3 vectorised-STL link errors. Clean rebuild.
- Verified: `scripts/build.sh` (dev): configure+build OK, 1/1 test suites pass (3 cases, 7 assertions). `scripts/build.sh -Preset release`: OK, 1/1 pass. ORC JIT reports host CPU `rocketlake`; AVX2/FMA/AVX-512F detected.
- Perf: release `pq_tests_smoke.exe` with the full LLVM JIT component set = 29.1 MB before pruning (T1b.6 will measure the real app).
- CPU-efficiency check: n/a (toolchain only). Release preset uses /O2 /GL + /LTCG /OPT:REF /OPT:ICF.
- Notes: SPEC now includes RMSC (§8.1, port of Borrasca Sidechainer 3), explicit compressor sidechain/lookahead, and sidechain routing for all dynamics devices.

## 2026-10-20 · T2.1 + T2.2 · worker C / task/T2.1-T2.2-pitch-edo
- Did: new module pq_tuning. Rational (int64, always reduced, positive denominator, no INT64_MIN; mul/div/add/sub/pow checked via std::optional, cross-cancelled before multiplying; exact 128-bit ordering; cents via log2 n − log2 d). Monzo over primes ≤ 61 with toMonzo(r, primeLimit)/toRational returning pq::Result. PitchId (Degree/Ratio/Cents/Harmonic; 24 bytes, 8-aligned, trivially copyable). Tuning = std::variant<Edo> + reference pitch (default C3 = 261.626 Hz); ResolveContext gives anchorHz and a HarmonicFrame. resolveHzDouble/resolveHz: double math, float32 at the end, 0.0f/nullopt for unresolvable input. Edo: any n ≥ 1, equave as ratio or cents; patentVal for primes ≤ 31 in the EDO's equave; mapToSteps; edoRow; stepLabel "k\n". Questions Q1 (Cents reference) and Q2 (EDO row labels) added.
- Verified: build.ps1 (dev): 0 warnings, 3/3 suites (pq_tests_tuning 26 cases / 523,387 assertions). release: 0 warnings, 3/3. 50-interval composition: 2000 walks, 1801 exact, 199 overflows reported exactly at ≥ 2^63. float32 vs double over 20k random pitches: worst 0.000102 ¢ (limit 0.001). Patent vals for 12/19/22/31/41/53/72 match published vals; n = 1..400 match a first-principles formula. Fixed two wrong tests (reversed ordering expectation; sweep degrees that overflowed float32).
- Perf (release, i7-11700F, shared machine): resolveHz degree 4.92 ns, ratio 2.14, cents 4.58, harmonic 3.74, random mix 7.00; checked Rational mul 58.6 ns.
- CPU-efficiency check: resolveHz is one switch plus at most one exp2/divide/multiply in double (needed for the 0.001 ¢ criterion; note-on/block rate only). EDO precomputes log2(step). PitchId packs the tag into padding (24 B not 32). Binary gcd measured no faster than std::gcd, reverted. Per-sample tuning modulation is T2.6's linear form, not resolveHz.
- Notes/blockers: RtScope tests for resolveHz, resolveHzDouble, patentVal, mapToSteps, edoRow and Rational ops are pending T0.3 on main. Open: Q1, Q2.
- Orchestrator verification: fast-forwarded onto main (1e9e2f7 base, no rebase needed); build.ps1 dev + release run by orchestrator: 0 warnings, 3/3 suites pass. Diff reviewed: no stubs, RT entry points noexcept and allocation-free.
