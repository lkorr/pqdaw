#!/usr/bin/env python3
"""OP.0: where inside a JIT stage function the samples of a pq_bench_scenarios profile fall.

A developer build of the JIT that lists its stage functions' addresses (the --jit-map file of symbolize_samples.py)
and PQ_JIT_DUMP give the pieces:
  clang -c -target x86_64-pc-windows-msvc <dump>.s -o part.obj
  llvm-objdump -d --no-show-raw-insn part.obj > part.dis
  python tools/bench/annotate_jit.py <samples> <jit map> part.dis <function name> [--scenario REGEX] [--min PCT]

The object the JIT linked has the same layout as the one assembled from the dump, so (sample address - the function's
JIT address) is the offset into the function in part.dis. Prints every loop (a backward conditional jump) with its
share of the function's samples and its instruction count, then the instructions at or above --min percent.
"""
import argparse
import collections
import re


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("jitmap")
    ap.add_argument("dis")
    ap.add_argument("function")
    ap.add_argument("--scenario", default="")
    ap.add_argument("--min", type=float, default=1.0)
    ap.add_argument("--listing", action="store_true", help="print the whole function with its sample counts")
    a = ap.parse_args()
    base = None
    for line in open(a.jitmap):
        p = line.split()
        if len(p) >= 2 and p[1] == a.function:
            base = int(p[0], 16)
    if base is None:
        raise SystemExit("function not in the JIT map")
    insns = []  # (offset, text)
    start = None
    for line in open(a.dis):
        m = re.match(r"^([0-9a-f]+) <(.+)>:", line)
        if m:
            if start is not None:
                break
            if m.group(2) == a.function:
                start = int(m.group(1), 16)
            continue
        if start is None:
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+(.*)$", line)
        if m:
            insns.append((int(m.group(1), 16) - start, m.group(2).strip()))
    if not insns:
        raise SystemExit("function not in the disassembly")
    size = insns[-1][0] + 16
    offs = [i[0] for i in insns]
    counts = collections.Counter()
    on = False
    total = 0
    for line in open(a.samples):
        if line.startswith("# "):
            on = re.search(a.scenario, line) is not None
            continue
        if on and line.startswith("J "):
            _, addr, n = line.split()
            off = int(addr, 16) - base
            if 0 <= off < size:
                import bisect
                i = bisect.bisect_right(offs, off) - 1
                counts[i] += int(n)
                total += int(n)
    print(f"{a.function}: {total} samples, {len(insns)} instructions")
    # Loops: a conditional jump to an earlier instruction of this function.
    index_of = {o: i for i, (o, _) in enumerate(insns)}
    loops = []
    for i, (o, text) in enumerate(insns):
        m = re.match(r"^j[a-z]+\s+0x([0-9a-f]+)", text)
        if m:
            tgt = int(m.group(1), 16) - start
            if tgt in index_of and index_of[tgt] <= i:
                loops.append((index_of[tgt], i))
    covered = 0
    print("loops (first..last instruction offset, instructions, calls inside, % of the function's samples):")
    for lo, hi in loops:
        n = sum(counts[k] for k in range(lo, hi + 1))
        calls = sum(1 for k in range(lo, hi + 1) if insns[k][1].startswith("call"))
        if any(l2 <= lo and hi <= h2 and (l2, h2) != (lo, hi) for l2, h2 in loops):
            continue  # an inner loop of one listed
        covered += n
        stores = sum(1 for k in range(lo, hi + 1) if re.match(r"^v?mov[a-z0-9]*\s+%[xyz]mm\d+, -?\d*\(", insns[k][1]))
        print(f"  {insns[lo][0]:#7x}..{insns[hi][0]:#7x}  {hi - lo + 1:4d} insns  {stores:2d} vector stores  {calls} calls  "
              f"{100.0 * n / max(total, 1):5.1f} %")
    print(f"  outside loops: {100.0 * (total - covered) / max(total, 1):.1f} %")
    if a.listing:
        for i, (o, text) in enumerate(insns):
            print(f"{counts[i]:6d} {o:#7x}  {text}")
    else:
        for i, (o, text) in enumerate(insns):
            if total and 100.0 * counts[i] / total >= a.min:
                print(f"  {100.0 * counts[i] / total:5.1f} %  {o:#7x}  {text}")


if __name__ == "__main__":
    main()
