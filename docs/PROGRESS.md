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
