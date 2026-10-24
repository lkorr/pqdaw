"""Probe 10: more MIDI tracks than the link's 128 event slots: does the 129th track play?"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from pqlib import *

Q = PPQ
for n in (128, 129):
    p = base_project()
    for i in range(n):
        notes = [note(0, Q)] if i == n - 1 else []
        p["tracks"].append(midi([clip(0, 2 * Q, notes)]))
    x, log, rc = render(write_project(p, f"slots_{n}"), 1, timeout=300)
    peak = None if x is None else float(abs(x).max())
    print(f"{n} MIDI tracks, note only on the last: rc={rc} peak={peak}  {log[-160:]}")
