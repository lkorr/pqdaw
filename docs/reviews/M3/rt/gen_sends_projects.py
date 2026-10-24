"""M3 RT review: generate synthetic projects to measure the per-track x per-return cost of implicit sends
(Q95.2) and the steady per-track mixer cost, through `pqdaw --project <dir> --render out.wav --seconds S`.

usage: python -I gen_sends_projects.py <out_root> [tracks...]   (writes <out_root>/t<T>_r<R>[_notes]/project.pqd)
"""
import json, os, sys

def track(tid, notes):
    clips = []
    if notes:
        # one held 3-note chord for 20 s at 120 BPM (27720 ticks per quarter): degrees 0, 10, 18 of 31-EDO
        L = 27720 * 40
        clips = [{"id": tid * 100 + 1, "name": "c", "colour": "#808080", "start": 0, "length": L, "contentStart": 0,
                  "loop": {"enabled": False, "start": 0, "end": L}, "signature": {"numerator": 4, "denominator": 4},
                  "tuning": "inherit", "active": True,
                  "notes": [{"id": tid * 100 + 10 + k, "start": 0, "length": L, "pitch": {"degree": d, "equave": 0},
                             "velocity": 90.0} for k, d in enumerate((0, 10, 18))]}]
    return {"id": tid, "kind": "midi", "name": f"T{tid}", "colour": "#808080", "active": True,
            "tuning": {"main": 8},
            "mixer": {"volumeDb": -12.0, "pan": 0.0, "mute": False, "solo": False, "arm": False, "sends": [],
                      "output": "default"},
            "lanes": [], "clips": clips}

def bus(tid, kind, name):
    return {"id": tid, "kind": kind, "name": name, "colour": "#808080", "active": True, "tuning": "inherit",
            "mixer": {"volumeDb": 0.0, "pan": 0.0, "mute": False, "solo": False, "arm": False, "sends": [],
                      "output": "default"}, "lanes": []}

def project(T, R, notes):
    return {"schemaVersion": 2,
            "timeline": {"tempo": [{"tick": 0, "bpm": 120.0, "curve": "step"}],
                         "signatures": [{"bar": 0, "numerator": 4, "denominator": 4}]},
            "loop": {"start": 0, "end": 27720 * 16, "enabled": False}, "locators": [],
            "tunings": [{"id": 8, "name": "31-EDO", "colour": "#22AA44",
                         "def": {"kind": "edo", "n": 31, "equave": "2/1", "referenceHz": 261.626}}],
            "tracks": [track(1000 + i, notes) for i in range(T)],
            "returns": [bus(500 + r, "return", f"R{r}") for r in range(R)],
            "master": bus(2, "master", "Master"),
            "view": {"tracks": [], "lanes": []}}

root = sys.argv[1]
for T in [int(x) for x in (sys.argv[2:] or ["32"])]:
    for R in (0, 1, 4):
        for notes in (False, True):
            d = os.path.join(root, f"t{T}_r{R}" + ("_notes" if notes else ""))
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "project.pqd"), "w") as f:
                json.dump(project(T, R, notes), f, indent=1)
            print(d)
