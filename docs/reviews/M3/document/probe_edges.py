"""Probe 5: sequence edge cases through --render (overlapping clips, chance, velocity range, 1-tick notes,
disabled notes, inactive clips, odd signatures, polyphony cap)."""
import sys, os, hashlib
sys.path.insert(0, os.path.dirname(__file__))
from pqlib import *

Q = PPQ


def go(name, p, seconds):
    x, log, rc = render(write_project(p, name), seconds)
    if x is None:
        print(f"{name}: render failed rc={rc}: {log}")
    return x


# E1 overlapping clips in a file (the editor never makes them; Q41: later wins)
p = base_project(30.0)
a = clip(0, 4 * Q, [note(0, Q // 8), note(3 * Q, Q // 8)])
b = clip(2 * Q, 4 * Q, [note(0, Q // 8)])
p["tracks"].append(midi([a, b]))
x = go("edge_overlap", p, 14)
if x is not None:
    print("E1 overlapping clips: onsets (s)", [round(o / SR, 3) for o in onsets(x)], "(A's note at 6 s must be cut by B at 4 s)")

# E2 chance 0.5 on 40 notes, rendered twice -> identical and ~half played
p = base_project(60.0)
ns = [note(i * 2 * Q, Q // 8, chance=0.5) for i in range(40)]
p["tracks"].append(midi([clip(0, 80 * Q, ns)]))
x1 = go("edge_chance1", p, 81)
x2 = go("edge_chance2", p, 81)
if x1 is not None and x2 is not None:
    print(f"E2 chance 0.5: {len(onsets(x1))}/40 played, renders identical: {np.array_equal(x1, x2)}")

# E3 velocity range: velocity 20 + u*100 -> amplitude spread
p = base_project(60.0)
ns = [note(i * 2 * Q, Q // 2, vel=20.0, velocityRange=100.0) for i in range(12)]
p["tracks"].append(midi([clip(0, 24 * Q, ns)]))
x = go("edge_velrange", p, 25)
if x is not None:
    pk = [np.abs(x[o:o + 12000]).max() for o in onsets(x)]
    print("E3 velocity range peaks:", [round(v, 4) for v in pk])

# E4 1-tick note (< 1 sample) and a note at tick 0 of length 1
p = base_project(120.0)
p["tracks"].append(midi([clip(0, 4 * Q, [note(0, 1), note(2 * Q, 1)])]))
x = go("edge_1tick", p, 3)
if x is not None:
    os_ = onsets(x)
    print("E4 1-tick notes: onsets", os_, "peaks", [round(float(np.abs(x[o:o + 24000]).max()), 5) for o in os_])

# E5 disabled note + inactive clip
p = base_project(30.0)
p["tracks"].append(midi([clip(0, 2 * Q, [note(0, Q // 8, disabled=True), note(Q, Q // 8)]),
                         clip(2 * Q, 2 * Q, [note(0, Q // 8)], active=False)]))
x = go("edge_disabled", p, 10)
if x is not None:
    print("E5 disabled note/inactive clip: onsets (s)", [round(o / SR, 3) for o in onsets(x)], "(expect only 2.0)")

# E6 odd signatures
for num, den in [(7, 8), (4, 128), (1024, 1), (3, 3)]:
    p = base_project(120.0)
    p["timeline"]["signatures"] = [{"bar": 0, "numerator": num, "denominator": den}]
    p["tracks"].append(midi([clip(0, Q, [note(0, Q // 2)])]))
    x, log, rc = render(write_project(p, f"edge_sig_{num}_{den}"), 1)
    print(f"E6 signature {num}/{den}: rc={rc} {'' if rc == 0 else log}")

# E7 polyphony: 40 simultaneous notes on one track (maxVoices 24)
p = base_project(120.0)
p["tracks"].append(midi([clip(0, 4 * Q, [note(0, 2 * Q, degree=d) for d in range(40)])]))
x = go("edge_poly", p, 2)
if x is not None:
    print("E7 40-note chord: peak", float(np.abs(x).max()))
