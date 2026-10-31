"""Probe 8: CPU cost of the lowered mixer (implicit sends to every return, Q95.2/3) through --render.
Silent tracks (no notes): what remains is the per-track mixer + summing."""
import sys, os, re, time
sys.path.insert(0, os.path.dirname(__file__))
from pqlib import *

Q = PPQ
SECONDS = 20


def build(tracks, returns):
    p = base_project()
    rs = [ret() for _ in range(returns)]
    p["returns"] = rs
    for _ in range(tracks):
        p["tracks"].append(midi([]))
    return p


for tr, rt in [(1, 0), (64, 0), (64, 1), (64, 8), (64, 12)]:
    best = None
    for rep in range(3):
        x, log, rc = render(write_project(build(tr, rt), f"cost_{tr}_{rt}"), SECONDS, timeout=600)
        m = re.search(r"in ([0-9.]+) s \(([0-9.]+)x realtime\)", log)
        if m:
            w = float(m.group(1))
            best = w if best is None else min(best, w)
    if best is None:
        print(tr, rt, "FAILED", log)
        continue
    print(f"{tr:3d} tracks x {rt:2d} returns: {best:.3f} s wall for {SECONDS} s -> {100*best/SECONDS:.2f} % of one core; "
          f"{best/SECONDS/48000/max(tr,1)*1e9:.1f} ns per track-sample")
