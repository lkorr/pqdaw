# Overnight build prompt

Start a Claude Code session in `C:\Users\Luke\Desktop\programming\pqdaw` (bypass-permissions mode, so it doesn't stall on prompts) and paste the block below.

---

```
You are the lead engineer and orchestrator for pqdaw, working unattended overnight. Use multi-agent orchestration: you coordinate, and worker subagents implement.

Before anything else, read CLAUDE.md, SPEC.md, docs/ARCHITECTURE.md, docs/TASKS.md, docs/PATCH_LANGUAGE.md and docs/PROGRESS.md. CLAUDE.md's rules are binding for you and every worker; paste its "Non-negotiable rules" into every worker's prompt.

Loop until there are no unblocked tasks in M0, M1, M1b and M2, or until you have run for 10 hours:
1. Sync: `git checkout main && git pull`. Run `./scripts/build.ps1` and confirm main is green. If main is red, fixing it is the only task.
2. Choose up to 3 open tasks whose dependencies are [x] and which touch different modules or files. Prefer the critical path: T0.2 → T0.3 → T1.x → T1b.x. The LLVM JIT (M1b) is the biggest technical risk, so get to it early. Tuning (M2) and UI (T0.7–T0.9) are good parallel lanes.
3. Mark them [~] in docs/TASKS.md on main (commit + push) so you don't double-assign.
4. Spawn one worker subagent per task with isolation "worktree". Each worker prompt contains: the task text and acceptance criteria copied verbatim, the non-negotiable rules, the branch name task/<id>-<slug>, and this instruction: "Implement, write the tests and benchmarks, run ./scripts/build.ps1 for both the dev and release presets until everything passes with zero warnings, commit on your branch, and report: files changed, exact verification commands with their output summaries, benchmark numbers, how you applied the CPU-efficiency rule, and anything unfinished. Do not merge to main."
5. When a worker reports back, don't take its word for it. Check out its branch, rebase onto main, and run the dev AND release builds and the full test suite yourself. Read the diff and check it against the acceptance criteria and the rules: no stubs, no weakened tests, RT safety, efficiency. If it falls short, send it back to that worker (SendMessage) with specific feedback, or fix it yourself. Only then fast-forward main, push, mark the task [x] with the commit hash, and append the worker's PROGRESS.md entry plus your verification line.
6. If a task is blocked (missing tool, ambiguous spec), mark it [!], write the blocker in PROGRESS.md (and in docs/QUESTIONS.md if it needs a decision from the user), and move on. Never fake completion.
7. After each merge, refill the worker slots (step 2).

Constraints:
- main must always build and pass. Never force-push. Never commit reference/inspiration/.
- Add dependencies only via cmake/PqDependencies.cmake, pinned, and listed in docs/DEPENDENCIES.md.
- Visual (👁) tasks: meet the criteria, then list them in docs/REVIEW.md for the user.
- Don't change SPEC.md decisions. Put questions in docs/QUESTIONS.md.
- Keep your own context lean: delegate reading and implementation to workers; you verify and integrate.

When you stop, append a "Morning summary" section to docs/PROGRESS.md: tasks completed (with commits), tasks in progress/blocked and why, key benchmark numbers, open questions, and what to review first. Push.
```

---

## Notes for the user
- Expected outcome by morning: a good part of M0–M2 (foundation, engine core, JIT, tuning library), verified by tests. No usable DAW UI yet; that starts at M3 and needs your eyes.
- Check `docs/PROGRESS.md` (morning summary), then `docs/QUESTIONS.md` and `docs/REVIEW.md`.
- Token use: 3 parallel workers plus verification for ~10 h is a lot. Reduce to 2 workers if cost matters.
