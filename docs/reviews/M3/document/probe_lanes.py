"""Probe 7: which automation lanes the document accepts are actually audible (tuning reference on EDO / Scale /
clip-override tunings, track mute lanes)."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from pqlib import *

Q = PPQ
REF = 261.626


def lane(kind, owner, pts):
    return {"id": nid(), "param": {"kind": kind, "owner": owner},
            "points": [{"id": nid(), "tick": t, "value": v, "curve": 0.0} for t, v in pts]}


def pitch_case(name, tunings, track_tuning, clip_tuning=None, lane_owner=None):
    p = base_project(120.0)
    p["tunings"] = tunings
    c = clip(0, 10 * Q, [note(0, 10 * Q)])
    if clip_tuning:
        c["tuning"] = clip_tuning
    t = midi([c], tuning=track_tuning)
    t["lanes"] = [lane("tuningReference", lane_owner, [(2 * Q, REF), (4 * Q, 2 * REF)])]
    p["tracks"].append(t)
    x, log, rc = render(write_project(p, name), 4.8)
    if x is None:
        print(name, "FAILED", rc, log)
        return
    f0 = est_freq(x, REF, int(0.4 * SR), int(0.95 * SR))
    f1 = est_freq(x, 2 * REF, int(2.2 * SR), int(4.6 * SR)) if True else 0
    f1b = est_freq(x, REF, int(2.2 * SR), int(4.6 * SR))
    print(f"{name}: before the ramp {f0:.3f} Hz; after it the spectrum peak near 2*ref = {f1:.3f} Hz, near ref = {f1b:.3f} Hz "
          f"-> {'FOLLOWS the lane' if abs(f1 - 2 * REF) < 0.5 and abs(f1b - REF) > 0.5 else 'IGNORES the lane (static pitch)'}")


edo = {"id": 1, "name": "12", "colour": "#5E9BD6", "def": {"kind": "edo", "n": 12, "equave": "2/1", "referenceHz": REF}}
edo2 = {"id": 11, "name": "12b", "colour": "#5E9BD6", "def": {"kind": "edo", "n": 12, "equave": "2/1", "referenceHz": REF}}
scale = {"id": 10, "name": "scl", "colour": "#4422AA",
         "def": {"kind": "scale", "description": "maj", "pitches": ["9/8", "5/4", "4/3", "3/2", "5/3", "15/8", "2/1"],
                 "referenceHz": REF}}
pitch_case("lane_ref_edo_track", [edo], {"main": 1}, lane_owner=1)
pitch_case("lane_ref_scale_track", [edo, scale], {"main": 10}, lane_owner=10)
pitch_case("lane_ref_clip_override", [edo, edo2], {"main": 1}, clip_tuning={"main": 11}, lane_owner=11)

# mute lane: 0 -> 1 at 2 s
p = base_project(120.0)
t = midi([clip(0, 10 * Q, [note(0, 10 * Q)])])
t["lanes"] = [lane("trackMute", t["id"], [(0, 0.0), (4 * Q, 0.0), (4 * Q + 1, 1.0)])]
p["tracks"].append(t)
x, log, rc = render(write_project(p, "lane_mute"), 4.8)
if x is None:
    print("lane_mute FAILED", rc, log)
else:
    print(f"lane_mute (muted from 2 s): rms 1-2 s {rms(x, int(1 * SR), int(2 * SR))[0]:.4f}, "
          f"rms 3-4 s {rms(x, int(3 * SR), int(4 * SR))[0]:.4f}")
