# pqdaw: instructions for agents

You are building **pqdaw**, a performance-first microtonal DAW in C++. Before working, read in order:
1. `SPEC.md`: what we're building (decisions are in its §15 log)
2. `docs/ARCHITECTURE.md`: modules, dependency direction, interfaces, conventions
3. `docs/TASKS.md`: the plan and acceptance criteria
4. `docs/PROGRESS.md`: what has happened so far (read at least the last 30 entries)
5. `docs/PATCH_LANGUAGE.md`: when touching `graph`, `jit`, `stdlib`

## Non-negotiable rules

### 1. CPU efficiency
At every step, ask whether what you're building is as CPU-efficient as it can possibly be without an obviously bad sacrifice: correctness, audible quality, realtime safety, or a big maintainability cost. Say how you answered that in your PROGRESS.md entry. Prefer specialisation over generality on hot paths, struct-of-arrays, and voice-lane SIMD. Measure; don't guess.

### 2. Realtime safety (audio thread and anything it calls)
No heap allocation or free, no locks or mutexes, no syscalls, no file or console I/O, no exceptions, no unbounded loops, no `std::function`, no virtual calls inside per-sample loops. Everything is preallocated off-thread and handed over through lock-free structures. Mark RT-safe functions `noexcept` with a comment `// RT-safe`. Every RT entry point gets an `RtScope` allocation-trap test once T0.3 exists.

### 3. Numbers
- Pitch is float32 Hz at the end. Pitch identities stay exact (integer ratios).
- Oscillator phase uses **uint32** fixed-point accumulators.
- Timeline positions are **int64** ticks (`pq::kPpq`; never write literal tick values, derive them from the constant) or samples.
- Denormals are flushed on audio threads.

### 4. Definition of done
A task is done only when every acceptance criterion in `docs/TASKS.md` and every standing criterion holds, **verified by commands you actually ran**:
- `./scripts/build.ps1 -Affected` passes, **one run**: the `check` preset (release optimisation, asserts on), zero warnings (warnings are errors), every target and test (default + slow tiers) that `scripts/affected.ps1` maps your diff to. No separate dev or release run.
- Benchmarks recorded in `bench/RESULTS.md` where relevant, measured on the `ship` preset (see Build and test).
- No stubs, placeholder returns, `TODO: implement` notes, or commented-out failing tests in code that claims to be done.
- Never weaken, skip or delete a test to make it pass. If a test is wrong, fix it and explain why in PROGRESS.md.
- If you can't finish, mark the task `[!]` with the exact blocker in PROGRESS.md and move on. A partial result that is honestly labelled is fine; a false "done" is the worst possible outcome.

### 5. Scope discipline
- Implement what the task says. Don't redesign other modules. If an interface in `docs/ARCHITECTURE.md` must change, change the doc in the same commit and explain why in PROGRESS.md.
- Don't edit `SPEC.md` decisions. If the spec is ambiguous or seems wrong, write the question in `docs/QUESTIONS.md` (what the options are, what you chose provisionally and why) and continue with the most reversible choice.
- 👁 tasks (visual/UX) get built to their criteria and listed in `docs/REVIEW.md` for the user.

## Build and test

```powershell
./scripts/build.ps1 -Affected                         # worker verification: what your diff affects (check preset, default + slow)
./scripts/affected.ps1                                # print that plan without building
./scripts/build.ps1 -Target pq_tests_dsp -TestRegex pq_tests_dsp   # iterate: one test executable, default tier
./scripts/build.ps1 -Full                             # whole suite, default + slow (orchestrator only, or when -Affected says EVERYTHING)
./scripts/build.ps1 -Preset dev -Target pq_tests_core -NoTests     # unoptimised, for the debugger
./scripts/build.ps1 -Preset release -Perf             # asserts off: perf A/B and the opt-in, load-sensitive `perf` label
./scripts/build.ps1 -Preset ship -Target pq_bench_dsp -NoTests     # /GL + /LTCG: shipping builds, recorded benchmark numbers
./scripts/get-sccache.ps1                             # once per machine: the shared compiler cache
```
From Git Bash: `scripts/build.sh -Affected` (forwards to pwsh).

Test tiers and caches:
- The default tier skips the ctest labels `slow` (heavy sweeps: doctest cases tagged `* doctest::test_suite("slow")`, the `<exe>_slow` entries) and `perf` (load-sensitive wall-clock checks, T0.x-flake). `-Full` and `-Affected` add `slow`; `perf` is never included (run `-Perf` when your task touches what it measures). Tag a new case `slow` when it takes over ~1 s and is a sweep (fuzzing, exhaustive ranges, whole corpora, long stress runs); its coverage must not be needed by the default tier. Executables whose entry would run over ~30 s are split into parallel shards (`pq_add_test ... SHARDS n SLOW_SHARDS n`: entries `<exe>_s<k>`, `<exe>_slow_s<k>`); every test process gets its own `%TEMP%` (the shared test main).
- **Which tests to run: `scripts/affected.ps1` decides.** It maps `git diff origin/main...HEAD` plus uncommitted files to targets and ctest entries through the CMake target graph (`build/<preset>/pq_graph.json`): a module's tests plus those of everything linking it, test-dir files to their executables, docs to nothing, and `core`/`cmake`/presets/build scripts/test infrastructure to EVERYTHING; its header documents the judgment calls (DSP kernels don't run ui/views tests). Iterate with the one test executable you are changing, then verify once with `-Affected`. Run the whole suite only when `-Affected` says EVERYTHING. Put the plan it printed (executables run, and why) in your report.
- The orchestrator runs one whole-suite `./scripts/build.ps1 -Full` on main at each milestone boundary (and whenever several merged branches interact); a failure there is triaged to the branch that caused it.
- With sccache installed, every checkout shares one compiler cache (`%LOCALAPPDATA%\pqdaw\sccache-shared`, 20 GB) and one server whose base directories are all checkout roots, so a fresh worktree compiles mostly from cache; dependency archives are shared too (`%LOCALAPPDATA%\pqdaw\downloads`). A cache hit carries the other checkout's header paths: after every build `build.ps1` rewrites those in `.ninja_deps` to this checkout (`scripts/NinjaDeps.cs`), recompiles what it can't repair, and fails rather than continue with foreign dependencies. Use `build.ps1`, not a bare `cmake --build`. Never bake an absolute path into a compile definition or source: use `pq_use_build_paths` (`cmake/PqCompilerCache.cmake`). If a failure makes no sense (old constant values, crashes in code you didn't touch), suspect a stale object first: delete `build/<preset>/.ninja_deps` and rebuild.

Presets: `check` (default: `/O2` with asserts on; all verification), `dev` (unoptimised, asserts on: debugging), `release` (`/O2`, asserts off: perf A/B) and `ship` (`release` + `/GL` + `/LTCG`). Code that needs optimised code for a check (wall-clock bounds) tests `PQ_OPTIMIZED`, not `NDEBUG`. Benchmark numbers that go into `bench/RESULTS.md` come from `ship`; build only the bench executable you need there (each `ship` link re-runs code generation for everything it links, minutes of CPU). Quick A/B comparisons while you iterate may use `release`; say which preset in RESULTS.

### Working alongside other agents (shared machine)
Several agents usually build on this machine at once (8 cores, 16 threads). CPU is the bottleneck: oversubscribing it slows everyone, including you, and makes timing measurements meaningless. So:
- Set `$env:CMAKE_BUILD_PARALLEL_LEVEL=8` before building (`build.ps1` runs ctest with `-j8`; `$env:CTEST_PARALLEL_LEVEL` overrides), unless you were told to use less.
- Iterate with `check` builds of only what you touch: `./scripts/build.ps1 -Target pq_tests_<module> -TestRegex pq_tests_<module>`. Don't rebuild everything after each edit.
- Run the final verification (`./scripts/build.ps1 -Affected`) **once**, at the end, after your single rebase onto `origin/main`. Re-run only what failed. Never run two builds at the same time in one checkout.
- `ship` builds are for benchmarks only, and only the bench target.
- Dev builds link with `/DEBUG:FASTLINK` (PDBs point into the `/Z7` objects instead of copying them) and leave bench executables out of `all`: build a bench in dev with `-Target pq_bench_x` (or configure with `-DPQ_DEV_BENCHES=ON`). After a worker's branch merges, its whole worktree is removed (the shared compiler cache needs no cleanup; `scripts/clean-sccache.ps1` deletes caches of the old per-checkout layout).
- Exception: if you change a dsp kernel or JIT codegen, also link `./scripts/build.ps1 -Preset ship -Target pq_bench_dsp -NoTests` once before reporting. MSVC has crashed (C1001, internal compiler error) under `/LTCG` on kernel code that built fine without it.
- Benchmarks and wall-clock acceptance numbers measured while other agents build are indicative: record the system load the runner prints, and say so.
- Known intermittent failures that are not yours: an NVIDIA-driver-thread crash (SIGSEGV) in `pq_tests_ui` / `pq_tests_views` (`test_hidpi`, partial redraw, MOS chart hover, the circuit symbol-sheet golden), the ASIO fake-driver timing check (`test_asio.cpp`). Re-run the one executable (`ctest --test-dir build/<preset> -R <exe>`) and note it in your report. Anything else that fails is yours to explain.
- Before you report, make sure nothing you started is still running: no background builds, test runs or monitors waiting on output. Your report is final; a lingering background task keeps your agent alive and idle.
- Don't edit `docs/TASKS.md` or `docs/PROGRESS.md` when you run as a worker: put your PROGRESS entry in your final report, and title new questions `## Q?-<task>-a · …` (the orchestrator numbers them at merge).
- Disk: build directories are several GB each. Don't create extra ones (scratch worktrees, extra presets) unless the task needs them, and delete them when done.

Toolchain facts (verified during setup):
- MSVC (VS 2022, latest 17.x) via `scripts/vsenv.ps1`. CMake + Ninja. C++20.
- **CRT is `/MT` everywhere, and there is no Debug configuration**, because the prebuilt LLVM at `C:\dev\llvm-23.1.3` is a `/MT` release build. Link it through the `pq_llvm` target.
- `/arch:AVX2` baseline. The host CPU (i7-11700F) also has AVX-512, which the JIT may target at runtime. Prebuilt code must not require it.
- MSYS2/MinGW is on PATH (for other projects). Its libraries are a different ABI and are excluded via `CMAKE_IGNORE_PREFIX_PATH`. Never link against them.
- Add dependencies only in `cmake/PqDependencies.cmake`, pinned to an exact tag or commit, with licence and purpose recorded in `docs/DEPENDENCIES.md`. Permissive licences only (MIT/BSD/zlib/Apache-2.0/Boost/MPL-2.0; OFL for fonts). No GPL.
- Test executables: `pq_add_test(...)` in `tests/CMakeLists.txt` (see `cmake/PqModule.cmake`). Modules: `pq_add_module(...)`.

## Git workflow
- `main` must always build and pass all tests.
- Work on a branch `task/<id>-<slug>` (in your own git worktree when running in parallel with other agents).
- Before merging: rebase onto the latest `main`, run `./scripts/build.ps1 -Affected` (see "Which tests to run"), then fast-forward `main` and `git push origin main`.
- Commit messages: `T1.2: parser + printer for patch language v0`. Small, coherent commits; never commit build outputs.
- **Never commit `reference/inspiration/`** (other people's chat logs; the repo is public). Never commit secrets, absolute user paths in code (paths in docs are fine), or large binaries (> 1 MB) except deliberate golden files.
- Never force-push `main`. Never rewrite published history.

## Reference material (read-only, never modify)
| What | Where | Use for |
|---|---|---|
| MOS explorer (JS) | `C:\Users\Luke\Desktop\programming\music-website\apps\web\public\mos-explorer\` (`index.html`, `mos-roll.js`, `mos-data.js`, `mos-calc.js`) | T2.3 MOS math, piano-roll morph layout, wheel UI |
| PQ Meter (C++) | `C:\Users\Luke\Desktop\programming\pq meter\src\` | T0.9 line rendering, spectrum/scope DSP and look, theme tokens |
| JI trainer (JS) | `C:\Users\Luke\Desktop\programming\python code\musictheory\ji composition trainer\` | macro keymap, BFS labelling, interval names, chord data |
| Interval macro (AHK) | `C:\Users\Luke\Desktop\programming\python code\musictheory\midi macro\` | macro-mode key semantics, presets |
| Inspiration (private) | `reference/inspiration/` (git-ignored) | Zhea's JI-object screenshots, Lost Robot on performance |

To generate fixtures from the JS references, run them with Node (`node` is available via the music-website repo's toolchain; if it's missing, record that as a blocker). Commit the fixture-generating script under `tools/fixtures/` and the generated JSON under `tests/<module>/fixtures/`.

## Logging your work
Append to `docs/PROGRESS.md` (newest at the bottom):
```
## <date time> · <task id> · <agent/branch>
- Did: …
- Verified: <exact commands run and their results, e.g. "build.ps1 dev+release: 0 warnings, 143/143 tests">
- Perf: <numbers, or "n/a">
- CPU-efficiency check: <how the rule was applied>
- Notes/blockers: …
```
