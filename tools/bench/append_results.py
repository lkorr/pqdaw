#!/usr/bin/env python3
"""Append benchmark-runner CSV rows to bench/RESULTS.md (T1.11).

The CSV comes from any runner bench (pq_bench.h): `pq_bench_render --csv out.csv`. Each CSV row becomes one row of
the RESULTS.md table

    | Date | Commit | Benchmark | Config | Result | Notes |

  Date      today (local), or --date
  Commit    `git rev-parse --short HEAD` (+ "-dirty" when tracked files other than bench/RESULTS.md are modified), or
            "<label> (on <sha>)" with --label (the style used for rows measured on a task branch)
  Benchmark the row name
  Config    "<build>, <suite>" + any --config text (e.g. "block 64, 48 kHz")
  Result    "<median> <unit> median (min <min>, p99 <p99>, n=<samples>)"
  Notes     machine (the CPU from the CSV, logical CPUs, OS), the system load while the benchmark ran, and --note

Rows are appended at the end of the file (RESULTS.md is append-only). Dev-build rows are refused unless --allow-dev
(RESULTS.md holds release measurements only). --dry-run prints the rows instead.

Usage: python tools/bench/append_results.py results.csv [more.csv ...] [--note TEXT] [--config TEXT]
                                            [--label T1.11] [--results bench/RESULTS.md] [--filter REGEX] [--dry-run]
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


def git(*args):
    try:
        return subprocess.run(["git", "-C", REPO, *args], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def commit_ref(label):
    sha = git("rev-parse", "--short", "HEAD") or "unknown"
    status = git("status", "--porcelain", "--untracked-files=no")
    dirty = any(line[3:].strip() != "bench/RESULTS.md" for line in status.splitlines() if line.strip())
    ref = sha + ("-dirty" if dirty else "")
    return f"{label} (on {ref})" if label else ref


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


def rows_from(path, args, date, ref):
    out = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if args.filter and not re.search(args.filter, r["name"]):
                continue
            if r["build"] != "release" and not args.allow_dev:
                sys.exit(f"{path}: '{r['name']}' is a {r['build']} build row; RESULTS.md takes release builds only "
                         "(--allow-dev to override)")
            config = f"{r['build']}, {r['suite']}" + (f", {args.config}" if args.config else "")
            result = (f"{sig(r['median'])} {r['unit']} median (min {sig(r['min'])}, p99 {sig(r['p99'])}, "
                      f"n={r['samples']})")
            load = float(r["load_percent"])
            notes = [machine(r["cpu"]), f"system load {load:.0f} %" if load >= 0 else "system load unknown",
                     "runner (pq_bench.h)"]
            if args.note:
                notes.append(args.note)
            out.append(f"| {date} | {cell(ref)} | {cell(r['name'])} | {cell(config)} | {cell(result)} | "
                       f"{cell('; '.join(notes))} |")
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv", nargs="+")
    p.add_argument("--results", default=os.path.join(REPO, "bench", "RESULTS.md"))
    p.add_argument("--note", default="")
    p.add_argument("--config", default="")
    p.add_argument("--label", default="")
    p.add_argument("--date", default=datetime.date.today().isoformat())
    p.add_argument("--filter", default="", help="only rows whose name matches this regex")
    p.add_argument("--allow-dev", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    ref = commit_ref(args.label)
    rows = []
    for path in args.csv:
        rows += rows_from(path, args, args.date, ref)
    if not rows:
        sys.exit("no rows to append")
    if args.dry_run:
        print("\n".join(rows))
        return
    with open(args.results, "rb") as f:
        text = f.read().decode("utf-8")
    if not text.endswith("\n"):
        text += "\n"
    if not re.search(r"^\| Date \| Commit \| Benchmark \|", text, re.M):
        sys.exit(f"{args.results} has no results table")
    with open(args.results, "wb") as f:
        f.write((text + "\n".join(rows) + "\n").encode("utf-8"))
    print(f"appended {len(rows)} rows to {args.results}")


if __name__ == "__main__":
    main()
