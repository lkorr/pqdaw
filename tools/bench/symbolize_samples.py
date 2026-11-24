#!/usr/bin/env python3
"""OP.0: turns a pq_bench_scenarios sampling profile (PQ_BENCH_SAMPLE=<file>) into a per-scenario table of where the
audio thread's time went: per function (resolved through the linker map of the bench executable) and per group
(JIT code, pq::<module>, CRT / other).

  python tools/bench/symbolize_samples.py <samples file> <pq_bench_scenarios.map> [--top N] [--jit-map FILE]
                                          [--undname <llvm-undname.exe>] [--scenario REGEX]

--jit-map: lines "<hex address> <name>" (a developer build of the JIT that lists its stage functions); JIT samples
are then attributed to the nearest symbol at or below them, else they are one row "JIT code".
Inlined callees count for the function they were inlined into.
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys


def load_map(path):
    base = None
    syms = []
    pat = re.compile(r"^\s[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s")
    with open(path, "r", errors="replace") as f:
        for line in f:
            if base is None:
                m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
                if m:
                    base = int(m.group(1), 16)
                continue
            m = pat.match(line)
            if m:
                addr = int(m.group(2), 16)
                if addr >= base:
                    syms.append((addr - base, m.group(1)))
    syms.sort()
    return syms


def undecorate(names, tool):
    if not tool:
        return {n: n for n in names}
    names = [n for n in names if n.startswith("?")]
    try:
        out = subprocess.run([tool], input="\n".join(names) + "\n", capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return {n: n for n in names}
    # llvm-undname prints, per input line: the mangled name, the demangled one, an empty line.
    lines = out.splitlines()
    res = {}
    for i, l in enumerate(lines):
        if l.startswith("?") and i + 1 < len(lines) and lines[i + 1].strip():
            res[l.strip()] = lines[i + 1].strip()
    return res


def shorten(name):
    name = re.sub(r"\b(public|private|protected): ", "", name)
    name = re.sub(r"\b(static|virtual) ", "", name)
    name = re.sub(r"__cdecl |__vectorcall ", "", name)
    name = re.sub(r"\(.*$", "", name)  # drop the parameter list
    name = re.sub(r"^.*? (?=[\w`]+::|\w+$)", "", name)  # drop the return type
    return name[:110]


def group_of(name):
    m = re.match(r"(pq::\w+)", name)
    if m:
        return m.group(1)
    if name.startswith("JIT"):
        return "JIT code"
    if name.startswith("["):
        return name
    return "CRT / other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("map")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--jit-map")
    ap.add_argument("--undname", default=r"C:\dev\llvm-23.1.3\bin\llvm-undname.exe")
    ap.add_argument("--scenario", default="")
    a = ap.parse_args()
    syms = load_map(a.map)
    rvas = [s[0] for s in syms]
    jit = []
    if a.jit_map:
        with open(a.jit_map) as f:
            for line in f:
                p = line.split()
                if len(p) >= 2:
                    jit.append((int(p[0], 16), p[1]))
        jit.sort()
    jit_addrs = [j[0] for j in jit]
    scenarios = collections.OrderedDict()
    cur = None
    with open(a.samples) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("# "):
                cur = scenarios.setdefault(line[2:].split("\t")[0], collections.Counter())
                continue
            key, n = line.rsplit("\t", 1)
            cur[key] += int(n)
    wanted = set()
    for c in scenarios.values():
        for key in c:
            if key.startswith("E "):
                i = bisect.bisect_right(rvas, int(key[2:], 16)) - 1
                if i >= 0:
                    wanted.add(syms[i][1])
    names = undecorate(wanted, a.undname)
    for sc, c in scenarios.items():
        if a.scenario and not re.search(a.scenario, sc):
            continue
        total = sum(c.values())
        per = collections.Counter()
        for key, n in c.items():
            if key.startswith("E "):
                i = bisect.bisect_right(rvas, int(key[2:], 16)) - 1
                per[shorten(names.get(syms[i][1], syms[i][1])) if i >= 0 else "[exe ?]"] += n
            elif key.startswith("M "):
                per["[" + key.split()[1] + "]"] += n
            else:
                addr = int(key[2:], 16)
                i = bisect.bisect_right(jit_addrs, addr) - 1
                per["JIT " + jit[i][1] if i >= 0 and addr - jit[i][0] < (1 << 20) else "JIT code"] += n
        groups = collections.Counter()
        for name, n in per.items():
            groups[group_of(name)] += n
        print(f"## {sc}  ({total} samples)")
        print("  " + "  ".join(f"{g} {100.0 * n / total:.1f}%" for g, n in groups.most_common()))
        for name, n in per.most_common(a.top):
            print(f"  {100.0 * n / total:6.2f}%  {name}")
        print()


if __name__ == "__main__":
    sys.exit(main())
