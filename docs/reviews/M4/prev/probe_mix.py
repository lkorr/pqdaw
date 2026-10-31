"""Probe 2: mixer laws (volume, pan, group, master, sends) and mute/solo/routing vs an independent model.

Each MIDI track holds one sustained note of its own 12-EDO degree; the output spectrum says which tracks are heard
and at what gain (FFT magnitude at each track's fundamental, window inside the sustain)."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from pqlib import *

Q = PPQ
REF = 261.626


def hz(deg):
    return edo_hz(REF, 12, deg, 0)


def tone_track(deg, **kw):
    return midi([clip(0, 6 * Q, [note(0, 6 * Q, degree=deg, vel=100.0)])], **kw)


def levels(x, degs, a=int(1.5 * SR), b=int(2.5 * SR)):
    """Amplitude (per channel) of each degree's fundamental, by a windowed DFT at that exact frequency."""
    seg = x[a:b]
    n = seg.shape[0]
    w = np.hanning(n)
    t = np.arange(n) / SR
    out = {}
    for d in degs:
        e = np.exp(-2j * np.pi * hz(d) * t) * w
        out[d] = np.abs(e @ seg) * 2 / w.sum()
    return out


def run(name, p, degs, seconds=3):
    d = write_project(p, name)
    x, log, rc = render(d, seconds)
    if x is None:
        print(f"{name}: RENDER FAILED rc={rc}: {log}")
        return None
    return levels(x, degs)


# ---- reference amplitude: one track, 0 dB, centre --------------------------------------------------------------
p = base_project()
p["tracks"].append(tone_track(0))
ref = run("mix_ref", p, [0])[0]
print("reference amplitude per channel (0 dB, pan C, constant power -> -3 dB):", ref)
A0 = ref[0] / math.cos(math.pi / 4)  # the mono instrument amplitude before the pan law


def report(name, lv, expect):
    """expect: {deg: (gainL, gainR)} relative to A0 (0 = silent)."""
    worst = 0.0
    rows = []
    for d, (gl, gr) in expect.items():
        got = lv[d] / A0
        for g, e in zip(got, (gl, gr)):
            if e < 1e-9:
                err = 20 * math.log10(max(g, 1e-9))  # how loud a supposedly silent track is
                bad = g > 1e-4
            else:
                err = 20 * math.log10(max(g, 1e-12) / e)
                bad = abs(err) > 0.05
            if bad:
                worst = max(worst, abs(err))
        rows.append(f"deg{d}: got L{20*math.log10(max(got[0],1e-12)):.2f} R{20*math.log10(max(got[1],1e-12)):.2f} dB, "
                    f"exp L{20*math.log10(max(gl,1e-12)):.2f} R{20*math.log10(max(gr,1e-12)):.2f}")
    print(("OK   " if worst == 0 else "FAIL ") + name + ": " + "; ".join(rows))


def cp(pan, db=0.0):
    l, r = pan_cp(pan)
    return (l * db_gain(db), r * db_gain(db))


def mul(a, b):
    return (a[0] * b[0], a[1] * b[1])


def bal(pan, db=0.0):
    l, r = pan_bal(pan)
    return (l * db_gain(db), r * db_gain(db))


# ---- laws ---------------------------------------------------------------------------------------------------------
p = base_project()
p["tracks"].append(tone_track(0, vol=-6.0, pan=-0.4))
p["tracks"].append(tone_track(4, vol=6.0, pan=1.0))
p["tracks"].append(tone_track(7, vol=-69.9, pan=0.0))
p["tracks"].append(tone_track(9, vol=-70.0, pan=0.0))
report("volume/pan law (MIDI)", run("mix_law", p, [0, 4, 7, 9]),
       {0: cp(-0.4, -6), 4: cp(1.0, 6), 7: cp(0, -69.9), 9: (0, 0)})

p = base_project()
p["tracks"].append(group([tone_track(0, pan=0.0)], vol=-4.0, pan=0.5))
p["master"]["mixer"]["volumeDb"] = -2.0
p["master"]["mixer"]["pan"] = -0.25
report("group balance pan + master", run("mix_group", p, [0]), {0: mul(mul(cp(0), bal(0.5, -4)), bal(-0.25, -2))})

# sends: post-fader send -6 dB to a return at -3 dB pan 0.3; pre-fader send to a second return
p = base_project()
r1, r2 = ret(vol=-3.0, pan=0.3), ret(vol=0.0)
p["returns"] += [r1, r2]
t = tone_track(0, vol=-10.0, pan=0.0, sends=[{"target": r1["id"], "levelDb": -6.0, "preFader": False},
                                             {"target": r2["id"], "levelDb": -12.0, "preFader": True}])
t["mixer"]["output"] = "none"
p["tracks"].append(t)
exp = mul(cp(0), (db_gain(-10) * db_gain(-6), db_gain(-10) * db_gain(-6)))
exp = mul(exp, bal(0.3, -3))
exp2 = mul(cp(0), (db_gain(-12),) * 2)
report("sends post (-10-6, return -3 pan .3) + pre (-12), track output none",
       run("mix_sends", p, [0]), {0: (exp[0] + exp2[0], exp[1] + exp2[1])})

# ---- mute / solo / routing -------------------------------------------------------------------------------------
# model: Live's exclusive solo, solo in place; destinations of a soloed track's routing stay audible.


def scenario(name, build, expect):
    p = base_project()
    degs = build(p)
    import re
    lv = run(re.sub(r"[^A-Za-z0-9]+", "_", name)[:40], p, degs)
    if lv is None:
        return
    heard = {d: (lv[d] / A0).max() > 1e-4 for d in degs}
    ok = all(heard[d] == expect[d] for d in degs)
    print(("OK   " if ok else "FAIL ") + name + ": heard " + str({d: heard[d] for d in degs}) + " expected " +
          str(expect))


def s1(p):
    g = group([tone_track(2), tone_track(4, solo=True)])
    p["tracks"] += [tone_track(0), g]
    return [0, 2, 4]


scenario("solo child in group", s1, {0: False, 2: False, 4: True})


def s2(p):
    g = group([tone_track(2), tone_track(4)], solo=True)
    p["tracks"] += [tone_track(0), g]
    return [0, 2, 4]


scenario("solo group", s2, {0: False, 2: True, 4: True})


def s3(p):
    g = group([group([tone_track(2, solo=True), tone_track(4)])], mute=True)
    p["tracks"] += [tone_track(0), g]
    return [0, 2, 4]


scenario("soloed child of a muted outer group (nested)", s3, {0: False, 2: False, 4: False})


def s4(p):
    r = ret(solo=True)
    p["returns"].append(r)
    p["tracks"] += [tone_track(0, sends=[{"target": r["id"], "levelDb": 0.0, "preFader": False}]), tone_track(4)]
    return [0, 4]


scenario("soloed return: senders heard through it only", s4, {0: True, 4: False})


def s5(p):
    r = ret(mute=True)
    p["returns"].append(r)
    p["tracks"] += [tone_track(0, output="none", sends=[{"target": r["id"], "levelDb": 0.0, "preFader": False}])]
    return [0]


scenario("send to a muted return", s5, {0: False})


def s6(p):
    g2 = group([tone_track(7)])
    a = tone_track(0, solo=True)
    a["mixer"]["output"] = {"track": g2["id"]}
    g1 = group([a, tone_track(2)])
    p["tracks"] += [g1, g2]
    return [0, 2, 7]


scenario("soloed track routed to another group (destination must stay open)", s6, {0: True, 2: False, 7: False})


def s7(p):
    g2 = group([tone_track(7)], active=False)
    a = tone_track(0)
    a["mixer"]["output"] = {"track": g2["id"]}
    p["tracks"] += [a, g2]
    return [0, 7]


scenario("track routed to a DEACTIVATED group", s7, {0: False, 7: False})


def s8(p):
    g = group([tone_track(2)])
    r = ret()
    r["mixer"]["output"] = {"track": g["id"]}
    g["mixer"]["mute"] = True
    p["returns"].append(r)
    p["tracks"] += [g, tone_track(0, output="none", sends=[{"target": r["id"], "levelDb": 0.0, "preFader": False}])]
    return [0, 2]


scenario("return routed to a MUTED group", s8, {0: False, 2: False})


def s9(p):
    p["tracks"] += [tone_track(0, solo=True, active=False), tone_track(4)]
    return [0, 4]


scenario("soloed deactivated track (others?)", s9, {0: False, 4: True})


def s10(p):
    r1 = ret()
    r2 = ret()
    r1["mixer"]["sends"] = [{"target": r2["id"], "levelDb": 0.0, "preFader": False}]
    r1["mixer"]["output"] = "none"
    p["returns"] += [r1, r2]
    p["tracks"] += [tone_track(0, output="none", sends=[{"target": r1["id"], "levelDb": 0.0, "preFader": False}])]
    return [0]


scenario("return -> return send (document allows it, Q26.3)", s10, {0: True})
