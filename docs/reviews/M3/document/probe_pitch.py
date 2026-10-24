"""Probe 4: pitches of EDO / MOS / ratio / cents notes as rendered vs SPEC formulas (fundamental by DFT peak refine)."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from pqlib import *
from scipy.optimize import minimize_scalar

Q = PPQ
REF = 261.626


tunings = [
    {"id": 1, "name": "12", "colour": "#5E9BD6", "def": {"kind": "edo", "n": 12, "equave": "2/1", "referenceHz": REF}},
    {"id": 8, "name": "31", "colour": "#22AA44", "def": {"kind": "edo", "n": 31, "equave": "2/1", "referenceHz": REF}},
    {"id": 9, "name": "5L2s", "colour": "#AA4422",
     "def": {"kind": "mos", "generator": 705.0, "n": 7, "period": 1200.0, "periodsPerEquave": 1, "rotation": 0,
             "modmos": [], "genSequence": [], "octaveStretch": 0.0, "referenceHz": REF}},
    {"id": 10, "name": "13ed3", "colour": "#AA4422", "def": {"kind": "edo", "n": 13, "equave": "3/1", "referenceHz": 200.0}},
]
cases = [  # (tuning id, pitch json, expected Hz, label)
    (8, {"degree": 10, "equave": 0}, edo_hz(REF, 31, 10, 0), "31edo d10"),
    (8, {"degree": -3, "equave": 1}, edo_hz(REF, 31, -3, 1), "31edo d-3 e1"),
    (9, {"degree": 3, "equave": 0}, REF * 2 ** (630 / 1200), "5L2s d3 (Lydian 630c)"),
    (9, {"degree": 6, "equave": -1}, REF * 2 ** ((1125 - 1200) / 1200), "5L2s d6 e-1 (1125-1200c)"),
    (10, {"degree": 5, "equave": 0}, 200.0 * 3 ** (5 / 13), "13ed3 d5"),
    (1, {"ratio": "7/4"}, REF * 7 / 4, "ratio 7/4"),
    (1, {"cents": 386.3137}, REF * 2 ** (386.3137 / 1200), "cents 386.3137"),
]
for tid, pitch, exp, label in cases:
    p = base_project(120.0)
    p["tunings"] = tunings
    n = note(0, 8 * Q)
    n["pitch"] = pitch
    p["tracks"].append(midi([clip(0, 8 * Q, [n])], tuning={"main": tid}))
    name = "pitch_" + "".join(ch if ch.isalnum() else "_" for ch in label)
    x, log, rc = render(write_project(p, name), 4.5)
    if x is None:
        print(label, "FAILED", rc, log)
        continue
    f = est_freq(x, exp, int(1.0 * SR), int(4.0 * SR))
    print(f"{label}: expected {exp:.5f} Hz, got {f:.5f} Hz, error {1200*math.log2(f/exp):+.4f} cents")
