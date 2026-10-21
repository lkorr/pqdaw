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
- Timeline positions are **int64** ticks (PPQ 27720) or samples.
- Denormals are flushed on audio threads.

### 4. Definition of done
A task is done only when every acceptance criterion in `docs/TASKS.md` and every standing criterion holds, **verified by commands you actually ran**:
- `./scripts/build.ps1` (dev) and `./scripts/build.ps1 -Preset release`: zero warnings (warnings are errors), all tests pass.
- Benchmarks recorded in `bench/RESULTS.md` where relevant.
- No stubs, placeholder returns, `TODO: implement` notes, or commented-out failing tests in code that claims to be done.
- Never weaken, skip or delete a test to make it pass. If a test is wrong, fix it and explain why in PROGRESS.md.
- If you can't finish, mark the task `[!]` with the exact blocker in PROGRESS.md and move on. A partial result that is honestly labelled is fine; a false "done" is the worst possible outcome.

### 5. Scope discipline
- Implement what the task says. Don't redesign other modules. If an interface in `docs/ARCHITECTURE.md` must change, change the doc in the same commit and explain why in PROGRESS.md.
- Don't edit `SPEC.md` decisions. If the spec is ambiguous or seems wrong, write the question in `docs/QUESTIONS.md` (what the options are, what you chose provisionally and why) and continue with the most reversible choice.
- 👁 tasks (visual/UX) get built to their criteria and listed in `docs/REVIEW.md` for the user.

## Build and test

```powershell
./scripts/build.ps1                                   # dev: RelWithDebInfo, /Od, asserts on
./scripts/build.ps1 -Preset release                   # optimised
./scripts/build.ps1 -Target pq_tests_core -TestRegex core
```
From Git Bash: `scripts/build.sh -Preset release` (forwards to pwsh).

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
- Before merging: rebase onto the latest `main`, run the full dev and release build+tests, then fast-forward `main` and `git push origin main`.
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
