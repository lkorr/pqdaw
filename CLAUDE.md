# pqdaw: instructions for agents

You are building **pqdaw**, a performance-first microtonal DAW in C++20 (MSVC, CMake, Ninja).

## Read only what your task needs
The docs are large (ARCHITECTURE ≈ 95k tokens, the archives more). Everything you read stays in your context and is
re-sent with every later turn, so a big read at the start multiplies the cost of the whole task. Do not read whole files:
- Your task: its line in `docs/TASKS.md` and the matching section of its plan (`docs/plans/<X>.md`).
- Context: the last ~5 entries of `docs/PROGRESS.md`; questions the task names (`grep -n "^## Q228" docs/QUESTIONS.md`,
  older ones in `docs/archive/`).
- `SPEC.md`, `docs/ARCHITECTURE.md`, `docs/PATCH_LANGUAGE.md`: grep for the module / interface / term you touch and
  read that section only. The code and its headers are the truth; read them before the docs.
- `docs/REVIEW.md`: append only; don't read it.

## Non-negotiable rules
1. **CPU efficiency.** Ask at every step whether it is as CPU-efficient as it can be without an obviously bad sacrifice
   (correctness, audible quality, realtime safety, maintainability). Prefer specialisation on hot paths,
   struct-of-arrays, voice-lane SIMD. Measure when it matters; say how you applied this in your PROGRESS entry.
2. **Realtime safety** (audio thread and everything it calls): no allocation, locks, syscalls, I/O, exceptions,
   unbounded loops, `std::function`, or virtual calls in per-sample loops. Preallocate off-thread, hand over lock-free.
   RT-safe functions are `noexcept` with `// RT-safe`, and RT entry points get an `RtScope` allocation-trap test.
3. **Numbers.** Pitch ends as float32 Hz; pitch identities stay exact (integer ratios). Oscillator phase is uint32
   fixed point. Timeline positions are int64 ticks (derive from `pq::kPpq`, never literal ticks) or samples. Denormals
   flushed on audio threads.
4. **Honesty.** No stubs, placeholder returns or `TODO: implement` in code called done. Never weaken, skip or delete a
   test to make it pass; if a test is wrong, fix it and say why. If you can't finish, say exactly what is missing. A
   partial result honestly labelled is fine; a false "done" is the worst outcome.
5. **Scope.** Do what the task says. If an interface in ARCHITECTURE must change, update that section in the same
   commit. Don't edit `SPEC.md` decisions; put real ambiguities in `docs/QUESTIONS.md` (short: context, options,
   choice) and continue with the most reversible choice. Don't ask about choices with an obvious default.
6. **No backward compatibility** until the user says otherwise: there are no user projects. No migrations, load
   conversions or compatibility shims; delete old paths instead of keeping them alive.

## Verification by risk: use judgement
Verify in proportion to what the change can break. Ask: what could this change affect, and which single test would
catch it? Run that, not more. A wider run is not "safer" if nothing in it can fail because of your diff.

| Change | Build | Test |
|---|---|---|
| Docs, comments, strings, colours, sizes, layout constants, preset values, panel/element look | the one target that compiles it (`-Target pqdaw -NoTests`, or the test exe) | the one golden/test that renders it, if any; re-record only goldens the change is meant to move |
| UI behaviour, gestures, views, one module's logic | that module's test exe | its default tier (`-Target pq_tests_views -TestRegex pq_tests_views`) |
| Document model, lowering, shared headers used by several modules | `-Affected` | default tier (what `-Affected` runs) |
| DSP kernels, JIT codegen, engine, RT code, `core`, CMake / build scripts | `-Affected -Slow` | default + slow; DSP/JIT also link `-Preset ship -Target pq_bench_dsp -NoTests` once (MSVC /LTCG has crashed on kernels) |

- Pure UI/visual work never runs playback, audio, or render-sweep suites.
- Iterate on one test executable; run the final check **once**, after your single rebase onto `origin/main`. Re-run
  only what failed.
- Benchmarks only when the task is about performance or touches a hot path; numbers for `bench/RESULTS.md` come from
  `ship`, quick A/B from `release`.
- `./scripts/build.ps1 -Full` (whole suite, default + slow) is the orchestrator's, at milestone boundaries.
- Goldens: when a golden fails, look at the diff of the ones your change should move; re-record those. A golden that
  moves unexpectedly is a bug signal, not something to re-record blindly.

```powershell
./scripts/build.ps1 -Target pq_tests_views -TestRegex pq_tests_views   # one exe, default tier
./scripts/build.ps1 -Target pqdaw -NoTests                              # just compile
./scripts/build.ps1 -Affected [-Slow]                                   # what the diff affects (scripts/affected.ps1 prints the plan)
./scripts/build.ps1 -Full                                               # orchestrator only
./scripts/build.ps1 -Preset dev -Target <t> -NoTests                    # unoptimised, for the debugger
./scripts/build.ps1 -Preset ship -Target pq_bench_dsp -NoTests          # recorded benchmarks
```
From Git Bash: `scripts/build.sh …` (forwards to pwsh). Presets: `check` (default: /O2 + asserts; all verification),
`dev`, `release` (asserts off; perf A/B), `ship` (/GL + /LTCG). Code needing optimised builds tests `PQ_OPTIMIZED`.
Test labels: `slow` (sweeps > ~1 s; tag new ones `* doctest::test_suite("slow")`), `perf` (load-sensitive, only via
`-Perf`). Big executables are sharded (`pq_add_test ... SHARDS n SLOW_SHARDS n`).

### Shared machine
- Several agents build at once (8 cores / 16 threads, limited RAM): `$env:CMAKE_BUILD_PARALLEL_LEVEL=6`,
  `$env:CTEST_PARALLEL_LEVEL=4`. Never two builds in one checkout.
- Use `build.ps1`, not bare `cmake --build`: it manages the shared sccache (`%LOCALAPPDATA%\pqdaw\sccache-shared`) and
  repairs foreign header paths in `.ninja_deps`. Never bake absolute paths into code (`pq_use_build_paths`).
- A failure that makes no sense (old constants, crashes in untouched code): stale object; delete
  `build/<preset>/.ninja_deps` and rebuild.
- Known flakes, not yours: NVIDIA-driver-thread SIGSEGV in `pq_tests_ui` / `pq_tests_views`, the ASIO fake-driver
  timing check. Re-run that one exe and note it.
- Nothing you started may still be running when you report.

## Workers (run by an orchestrator)
- Work on `task/<id>-<slug>` in your own worktree. Commit messages `<id>: what`. Rebase once onto `origin/main` at the
  end, push the branch, don't merge.
- Don't edit `docs/TASKS.md` or `docs/PROGRESS.md`; title new questions `## Q?-<task>-a · …` (numbered at merge).
- 👁 (visual) tasks: a short `docs/REVIEW.md` entry for the user.
- **Report ≤ 30 lines**: the PROGRESS entry below, goldens changed, questions, final hash. Not a narrative.

## Orchestrators
- A worker prompt names the exact files / plan section / questions to read, the verification row from the table
  above, and the speed rules. Don't tell workers to read whole docs.
- Before launching work that changes a core structure (data model, hierarchy, what a container is), restate the
  design to the user in a few lines and get a yes. Misread designs have cost whole phases (GU.9 v1 was reverted).
- Check a worker's claimed limitation makes sense before relaying it or filing a task.
- After merging: `git worktree remove --force` the worktree, rebuild the user's `build/release/src/app/pqdaw.exe`
  (`-Preset release -Target pqdaw -NoTests`; fails with LNK1104 while the app is open).

## Toolchain facts
- MSVC (VS 2022 17.x) via `scripts/vsenv.ps1`. CRT is `/MT` everywhere and there is no Debug configuration (the
  prebuilt LLVM at `C:\dev\llvm-23.1.3` is `/MT` release; link it through `pq_llvm`).
- `/arch:AVX2` baseline; the JIT may target AVX-512 at runtime, prebuilt code must not require it.
- MSYS2/MinGW on PATH is excluded (`CMAKE_IGNORE_PREFIX_PATH`); never link it.
- Dependencies only in `cmake/PqDependencies.cmake`, pinned, recorded in `docs/DEPENDENCIES.md`; permissive licences
  only (no GPL).
- Tests: `pq_add_test(...)` in `tests/CMakeLists.txt`; modules: `pq_add_module(...)`.

## Git
- `main` always builds and passes. Never force-push main or rewrite published history.
- Never commit `reference/inspiration/`, `bug_reports/`, secrets, absolute user paths in code, or binaries > 1 MB
  (deliberate goldens excepted).

## Reference material (read-only)
| What | Where | Use for |
|---|---|---|
| MOS explorer (JS) | `C:\Users\Luke\Desktop\programming\music-website\apps\web\public\mos-explorer\` | MOS math, piano-roll morph, wheel UI |
| PQ Meter (C++) | `C:\Users\Luke\Desktop\programming\pq meter\src\` | line rendering, spectrum/scope DSP and look |
| JI trainer (JS) | `C:\Users\Luke\Desktop\programming\python code\musictheory\ji composition trainer\` | macro keymap, interval names, chords |
| Interval macro (AHK) | `C:\Users\Luke\Desktop\programming\python code\musictheory\midi macro\` | macro-mode key semantics |
| Inspiration (private) | `reference/inspiration/` (git-ignored) | JI-object screenshots, performance notes |

Fixture generators from the JS references run with Node; commit the script under `tools/fixtures/`, the JSON under
`tests/<module>/fixtures/`.

## Bug reports (F12)
The user files bugs from the app with F12 into `bug_reports/<YYYY-MM-DD_HH-MM-SS>/` (git-ignored): `report.md`
(title, `tags:`, description, `### Shot N` sections, `## Context`, `## Recent input`), `shot_N.png`,
`shot_N_cursor.png` (1:1 crop around the pointer), optionally `project.pqd` (`pqdaw --project <folder>`). Ideas go to
`bug_reports/suggestions/`. Find by tag: `grep -l "^tags:.*\bui\b" bug_reports/*/report.md`. When you work on one,
append `## Progress` (date, commit, what changed, what's open); never edit the user's sections. Once fixed on main,
move the folder to `bug_reports/fixed/`; never delete a ticket.

## Logging your work
`docs/PROGRESS.md` (newest at the bottom), at most ~8 lines:
```
## <date time> · <task id> · <branch>
- Did: …
- Verified: <exact commands and results, e.g. "pq_tests_views default tier: 572/572">
- Perf / CPU-efficiency check: <numbers, or how the rule was applied>
- Notes/blockers: …
```
