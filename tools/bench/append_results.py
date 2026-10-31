#!/usr/bin/env python3
"""Append benchmark-runner CSV rows to bench/RESULTS.md (T1.11, T1.11-fix1).

The CSV comes from any runner bench (pq_bench.h): `pq_bench_render --csv out.csv`. Each CSV row becomes one row of
the RESULTS.md table

    | Date | Commit | Benchmark | Config | Result | Notes |

  Date      today (local), or --date
  Commit    `git rev-parse --short HEAD`, or "<label> (on <sha>)" with --label (rows measured on a task branch)
  Benchmark the row name
  Config    "<preset>, <suite>" + any --config text (e.g. "block 64, 48 kHz"); the preset is the one the bench was
            built with (ship = /GL + /LTCG, release = no LTCG, dev), from the CSV's `preset` column
  Result    lower-is-better units: "<median> <unit> median (min <min>, p99 <p99>, n=<samples>)";
            higher-is-better units (x realtime, …/s): "<median> <unit> median (worst <min>, p1 <p99>, best <max>,
            n=<samples>)": the CSV's p99 field of such a row is the low tail (pq_bench.h)
  Notes     "indicative: <reasons>" first when the row is not a baseline (see below), then the machine (the CPU from the
            CSV, logical CPUs, OS), the system load while the benchmark ran, and --note

Baselines and indicative rows (M1 review M20). A row is a baseline only when
  - the working tree is clean: nothing but bench/RESULTS.md and the CSV files themselves differs from HEAD
    (tracked or untracked); otherwise the script refuses, always: the commit must name what was measured;
  - the bench was built with the `ship` preset;
  - the system load while its benchmark ran was known and at most 25 % (--max-load).
Rows that fail the preset or load condition are refused unless --indicative, which appends them labelled
"indicative" with the reasons. Dev-build rows additionally need --allow-dev.

Rows are appended at the end of the file (RESULTS.md is append-only). --dry-run prints the rows instead (and runs
every check). Usage:

  python tools/bench/append_results.py results.csv [more.csv ...] [--note TEXT] [--config TEXT] [--label T1.11]
         [--indicative] [--max-load 25] [--results bench/RESULTS.md] [--filter REGEX] [--allow-dev] [--dry-run]
"""

import argparse
import csv
import datetime
import os
import platform
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RESULTS_REL = "bench/RESULTS.md"


def git(*args):
    try:
        return subprocess.run(["git", "-C", REPO, *args], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def rel(path):
    """`path` relative to the repository, with forward slashes (as git prints it), or None outside it."""
    p = os.path.relpath(os.path.abspath(path), REPO).replace(os.sep, "/")
    return None if p.startswith("../") or p == ".." or os.path.isabs(p) else p


def dirty_paths(status, allowed):
    """Paths of `git status --porcelain` output other than the allowed ones (RESULTS.md, the CSVs)."""
    out = []
    for line in status.splitlines():
        if not line.strip():
            continue
        path = line[3:].strip()
        if " -> " in path:  # a rename: both sides count
            out += [p for p in path.split(" -> ") if p.strip('"') not in allowed]
            continue
        if path.strip('"') not in allowed:
            out.append(path)
    return out


def check_clean(csv_paths, results_path, status=None):
    """Exits unless the tree is clean apart from RESULTS.md and the CSV files."""
    allowed = {RESULTS_REL}
    r = rel(results_path)
    if r:
        allowed.add(r)
    allowed.update(p for p in (rel(c) for c in csv_paths) if p)
    if status is None:
        status = git("status", "--porcelain", "--untracked-files=all")
        if status is None:
            sys.exit("append_results: git status failed; cannot check that the tree is clean")
    dirty = dirty_paths(status, allowed)
    if dirty:
        listed = ", ".join(dirty[:8]) + (f" and {len(dirty) - 8} more" if len(dirty) > 8 else "")
        sys.exit(f"append_results: the working tree is dirty ({listed}): commit (or stash) first, so the row's commit "
                 "names what was measured; only bench/RESULTS.md and the CSV may differ")


def commit_ref(label):
    sha = git("rev-parse", "--short", "HEAD") or "unknown"
    return f"{label} (on {sha})" if label else sha


def sig(x):
    """3 significant digits, no exponent for the ranges benchmarks produce."""
    v = float(x)
    if v == 0 or not (abs(v) < 1e15):
        return f"{v:g}"
    digits = max(0, 2 - int(f"{abs(v):e}".split("e")[1]))
    return f"{v:.{digits}f}"


def cell(text):
    return str(text).replace("|", "\\|").replace("\n", " ")


def machine(cpu):
    logical = os.cpu_count() or 0
    return f"{cpu}, {logical} logical CPUs, {platform.system()} {platform.release()}"


def higher_is_better(unit):
    """Units where a larger number is better (the runner's `better` column; this rule for older CSVs)."""
    u = unit.strip().lower()
    return "realtime" in u or u.endswith("/s") or u.endswith("per second")


PRESET_LABEL = {"ship": "ship (LTCG)", "release": "release (no LTCG)", "check": "check (release + asserts)", "dev": "dev"}


def result_text(r, higher):
    if higher:
        return (f"{sig(r['median'])} {r['unit']} median (worst {sig(r['min'])}, p1 {sig(r['p99'])}, "
                f"best {sig(r['max'])}, n={r['samples']})")
    return f"{sig(r['median'])} {r['unit']} median (min {sig(r['min'])}, p99 {sig(r['p99'])}, n={r['samples']})"


def rows_from(path, args, date, ref):
    out = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if args.filter and not re.search(args.filter, r["name"]):
                continue
            # `preset` (T1.11-fix1); older CSVs only know NDEBUG ("release" may have been ship or release).
            preset = r.get("preset") or ("dev" if r["build"] == "dev" else "unknown")
            if preset == "dev" and not args.allow_dev:
                sys.exit(f"{path}: '{r['name']}' is a dev build row; RESULTS.md takes optimised builds only "
                         "(--allow-dev --indicative to override)")
            load = float(r["load_percent"])
            reasons = []
            if preset != "ship":
                reasons.append(f"{PRESET_LABEL.get(preset, preset + ' preset')} build, not ship")
            if load < 0:
                reasons.append("system load unknown")
            elif load > args.max_load:
                reasons.append(f"system load {load:.0f} % > {args.max_load:g} %")
            if reasons and not args.indicative:
                sys.exit(f"{path}: '{r['name']}' is not a baseline ({'; '.join(reasons)}); rerun on an idle machine "
                         "from a ship build, or pass --indicative to record it labelled as such")
            better = (r.get("better") or "").strip()
            higher = better == "higher" if better else higher_is_better(r["unit"])
            config = f"{PRESET_LABEL.get(preset, preset)}, {r['suite']}" + (f", {args.config}" if args.config else "")
            notes = []
            if reasons:
                notes.append("indicative: " + ", ".join(reasons))
            notes += [machine(r["cpu"]), f"system load {load:.0f} %" if load >= 0 else "system load unknown",
                      "runner (pq_bench.h)"]
            if args.note:
                notes.append(args.note)
            out.append(f"| {date} | {cell(ref)} | {cell(r['name'])} | {cell(config)} | {cell(result_text(r, higher))} | "
                       f"{cell('; '.join(notes))} |")
    return out


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv", nargs="+")
    p.add_argument("--results", default=os.path.join(REPO, "bench", "RESULTS.md"))
    p.add_argument("--note", default="")
    p.add_argument("--config", default="")
    p.add_argument("--label", default="")
    p.add_argument("--date", default=datetime.date.today().isoformat())
    p.add_argument("--filter", default="", help="only rows whose name matches this regex")
    p.add_argument("--indicative", action="store_true",
                   help="record rows that are not baselines (not ship, or load above --max-load), labelled")
    p.add_argument("--max-load", type=float, default=25.0, help="system load %% above which a row is indicative")
    p.add_argument("--allow-dev", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    return p.parse_args(argv)


def main(argv=None, status=None):
    args = parse_args(argv)
    check_clean(args.csv, args.results, status)
    ref = commit_ref(args.label)
    rows = []
    for path in args.csv:
        rows += rows_from(path, args, args.date, ref)
    if not rows:
        sys.exit("no rows to append")
    if args.dry_run:
        print("\n".join(rows))
        return rows
    with open(args.results, "rb") as f:
        text = f.read().decode("utf-8")
    if not text.endswith("\n"):
        text += "\n"
    if not re.search(r"^\| Date \| Commit \| Benchmark \|", text, re.M):
        sys.exit(f"{args.results} has no results table")
    with open(args.results, "wb") as f:
        f.write((text + "\n".join(rows) + "\n").encode("utf-8"))
    print(f"appended {len(rows)} rows to {args.results}")
    return rows


if __name__ == "__main__":
    main()
