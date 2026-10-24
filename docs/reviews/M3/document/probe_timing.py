"""Probe 1: note onsets to the sample (constant tempo, tempo ramps, odd ticks, loops, clip offsets)."""
import sys, copy
sys.path.insert(0, __import__("os").path.dirname(__file__))
from pqlib import *

Q = PPQ
results = []


def check_onsets(name, p, expected_ticks, seconds):
    d = write_project(p, name)
    x, log, rc = render(d, seconds)
    if x is None:
        print(name, "RENDER FAILED", rc, log)
        return
    tempo = p["timeline"]["tempo"]
    got = onsets(x)
    exp = [ticks_to_samples(tempo, t) for t in expected_ticks]
    # constant latency between the note-on sample and the first non-zero output sample, taken from the 2nd note
    lat = got[1] - exp[1] if len(got) > 1 and len(exp) > 1 else 0
    diffs = [g - e - lat for g, e in zip(got, exp)]
    print(f"{name}: {len(got)} onsets (expected {len(exp)}), latency {lat}, residuals {diffs}")
    if len(got) != len(exp):
        print("   got", got[:20], "\n   exp", exp[:20])


# 1a: constant 120 BPM, odd ticks (1 s apart + odd offsets)
odd = [0, 1 * Q * 2 + 1, 4 * Q + 12345, 6 * Q + 221759, 8 * Q + 7, 10 * Q + 110880 + 3]
p = base_project(60.0)
p["tracks"].append(midi([clip(0, 12 * Q, [note(t, Q // 4) for t in odd])]))
check_onsets("onset_const", p, odd, 13)

# 1b: tempo ramp 60 -> 180 BPM over 8 beats then step 90
p = base_project(30.0)
p["timeline"]["tempo"] = [{"tick": 0, "bpm": 30.0, "curve": "linear"}, {"tick": 8 * Q, "bpm": 90.0, "curve": "step"},
                          {"tick": 12 * Q, "bpm": 45.0, "curve": "step"}]
ticks = [0, 2 * Q + 999, 4 * Q + 31, 6 * Q + 77777, 8 * Q + 1, 10 * Q + 5, 12 * Q, 14 * Q + 3]
p["tracks"].append(midi([clip(0, 16 * Q, [note(t, Q // 8) for t in ticks])]))
check_onsets("onset_ramp", p, ticks, 30)

# 1c: looped clip: clip at bar 1 (4Q), loop [Q, 3Q) content start 2Q, notes at content 0, Q+5, 2Q+10
loop = {"enabled": True, "start": Q, "end": 3 * Q}
notes = [note(0, Q // 8), note(Q + 5, Q // 8), note(2 * Q + 10, Q // 8)]
p = base_project(30.0)
c = clip(4 * Q, 9 * Q, notes, loop=loop, content_start=2 * Q)
p["tracks"].append(midi([c]))
# pass 0: content [2Q, 3Q) -> note at 2Q+10 at clip start + 10; then passes of [Q,3Q) length 2Q starting at 5Q
exp = [4 * Q + 10]
s = 5 * Q
while s < 13 * Q:
    for off in (5, Q + 10):  # content Q+5 -> s+5 ; 2Q+10 -> s+Q+10
        if s + off < 13 * Q:
            exp.append(s + off)
    s += 2 * Q
# spacing is only 1 beat (0.5 s) here; release tails overlap less than min_gap? use gap detection with long gap
check_onsets("onset_loop", p, sorted(exp), 28)

# 1d: clip starting at a non-zero content start without loop + note straddling the clip end (must be cut)
p = base_project(30.0)
c = clip(2 * Q, 2 * Q, [note(0, Q // 8), note(Q * 2, Q // 8), note(Q * 3, 4 * Q)], content_start=2 * Q)
p["tracks"].append(midi([c]))
x_ = check_onsets("onset_contentstart", p, [2 * Q, 3 * Q], 14)
