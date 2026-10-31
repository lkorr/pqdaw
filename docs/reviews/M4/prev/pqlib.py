"""M3 review probes (document / playback): an independent model of project.pqd timing, pitch and gain laws, plus
helpers to build small projects and render them through `pqdaw.exe --render`.

Nothing here imports pqdaw code: formulas come from SPEC / QUESTIONS (PPQ 221760, tempo ramps linear in BPM per
tick, constant-power pan on MIDI tracks, balance on buses, -70 dB floor, Q39 curve law).
"""
import copy, json, math, os, subprocess, sys, wave
import numpy as np

# M4 review re-run (docs/reviews/M4/prev): paths from the environment; the schema-2 files these probes write load
# through the 2 -> 6 migrations (each MIDI track gets a BasicSynth device).
WT = os.environ.get("PQ_WT", r"C:\Users\Luke\Desktop\programming\pqdaw\.claude\worktrees\agent-afa2fd4414d25a50f")
EXE = os.environ.get("PQ_EXE", os.path.join(WT, "build", "release", "src", "app", "pqdaw.exe"))
SCRATCH = os.environ.get("PQ_SCRATCH", r"C:\Users\Luke\AppData\Local\Temp\claude\C--Users-Luke-Desktop-programming-pqdaw\dc880915-a517-4e42-b333-63d5928f1e7f\scratchpad\prev")
PPQ = 221760
SR = 48000.0


# ---- independent tempo model ----------------------------------------------------------------------------------
def ticks_to_seconds(tempo, t):
    """tempo: [{tick,bpm,curve}], curve 'linear' ramps BPM linearly in ticks to the next point."""
    pts = sorted(tempo, key=lambda p: p["tick"])
    if t < 0:
        return t * 60.0 / (PPQ * pts[0]["bpm"])
    sec = 0.0
    for i, p in enumerate(pts):
        t0 = p["tick"]
        t1 = pts[i + 1]["tick"] if i + 1 < len(pts) else None
        if t1 is not None and t >= t1:
            span = t1 - t0
        else:
            span = t - t0
        b0 = p["bpm"]
        if p.get("curve") == "linear" and t1 is not None and pts[i + 1]["bpm"] != b0:
            b1 = pts[i + 1]["bpm"]
            # bpm(tau) = b0 + (b1-b0) tau / L ; dt = 60/(PPQ bpm) dtau
            L = t1 - t0
            slope = (b1 - b0) / L
            sec += 60.0 / PPQ * (math.log(b0 + slope * span) - math.log(b0)) / slope
        else:
            sec += span * 60.0 / (PPQ * b0)
        if t1 is None or t < t1:
            return sec
    return sec


def ticks_to_samples(tempo, t, sr=SR):
    return int(math.floor(ticks_to_seconds(tempo, t) * sr + 0.5))


# ---- pitch model ------------------------------------------------------------------------------------------------
def edo_hz(ref, n, degree, equave, equave_cents=1200.0):
    return ref * 2.0 ** ((equave * equave_cents + degree * equave_cents / n) / 1200.0)


# ---- gain model -------------------------------------------------------------------------------------------------
def db_gain(db):
    return 0.0 if db <= -70 else 10 ** (db / 20.0)


def pan_cp(p):
    th = (p + 1) * math.pi / 4
    return math.cos(th), math.sin(th)


def pan_bal(p):
    return min(1, 1 - p), min(1, 1 + p)


def curve_value(va, vb, x, curve):
    k = 6.0 * curve
    if abs(k) < 1e-12:
        return va + (vb - va) * x
    return va + (vb - va) * math.expm1(k * x) / math.expm1(k)


def eval_lane(points, t):
    pts = sorted(points, key=lambda p: (p["tick"], p["id"]))
    if t <= pts[0]["tick"]:
        return pts[0]["value"]
    for a, b in zip(pts, pts[1:]):
        if a["tick"] <= t < b["tick"]:
            x = (t - a["tick"]) / (b["tick"] - a["tick"])
            return curve_value(a["value"], b["value"], x, a.get("curve", 0.0))
    return pts[-1]["value"]


# ---- project builder --------------------------------------------------------------------------------------------
_ids = [100]


def nid():
    _ids[0] += 1
    return _ids[0]


def base_project(bpm=120.0):
    return {
        "schemaVersion": 2,
        "timeline": {"tempo": [{"tick": 0, "bpm": bpm, "curve": "step"}],
                     "signatures": [{"bar": 0, "numerator": 4, "denominator": 4}]},
        "loop": {"start": 0, "end": 3548160, "enabled": False},
        "locators": [],
        "tunings": [{"id": 1, "name": "12-EDO", "colour": "#5E9BD6",
                     "def": {"kind": "edo", "n": 12, "equave": "2/1", "referenceHz": 261.626}}],
        "tracks": [], "returns": [],
        "master": {"id": 2, "kind": "master", "name": "Master", "colour": "#808080", "active": True,
                   "tuning": "inherit", "mixer": mixer(), "lanes": []},
    }


def mixer(vol=0.0, pan=0.0, mute=False, solo=False, sends=None, output="default"):
    return {"volumeDb": vol, "pan": pan, "mute": mute, "solo": solo, "arm": False, "sends": sends or [],
            "output": output}


def note(start, length, degree=0, equave=0, vel=100.0, **kw):
    n = {"id": nid(), "start": start, "length": length, "pitch": {"degree": degree, "equave": equave},
         "velocity": vel}
    n.update(kw)
    return n


def clip(start, length, notes, loop=None, content_start=0, active=True):
    return {"id": nid(), "name": "c", "colour": "#808080", "start": start, "length": length,
            "contentStart": content_start,
            "loop": loop or {"enabled": False, "start": 0, "end": max(length, 1)},
            "signature": {"numerator": 4, "denominator": 4}, "tuning": "inherit", "active": active,
            "notes": sorted(notes, key=lambda n: (n["start"], n["id"]))}


def midi(clips, tid=None, active=True, tuning="inherit", lanes=None, **mix):
    return {"id": tid or nid(), "kind": "midi", "name": "m", "colour": "#808080", "active": active,
            "tuning": tuning, "mixer": mixer(**mix), "lanes": lanes or [],
            "clips": sorted(clips, key=lambda c: c["start"])}


def group(children, tid=None, active=True, **mix):
    return {"id": tid or nid(), "kind": "group", "name": "g", "colour": "#808080", "active": active,
            "tuning": "inherit", "mixer": mixer(**mix), "lanes": [], "children": children}


def ret(tid=None, active=True, **mix):
    return {"id": tid or nid(), "kind": "return", "name": "r", "colour": "#808080", "active": active,
            "tuning": "inherit", "mixer": mixer(**mix), "lanes": []}


def write_project(p, name):
    d = os.path.join(SCRATCH, "proj", name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "project.pqd"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(p, f, indent=1)
    return d


def write_text(text, name, binary=False):
    d = os.path.join(SCRATCH, "proj", name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "project.pqd"), "wb") as f:
        f.write(text if binary else text.encode("utf-8"))
    return d


def render(d, seconds=None, name=None, timeout=60):
    out = os.path.join(SCRATCH, (name or os.path.basename(d)) + ".wav")
    if os.path.exists(out):
        os.remove(out)
    cmd = [EXE, "--project", d, "--render", out]
    if seconds is not None:
        cmd += ["--seconds", str(seconds)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT", -999
    if r.returncode != 0 or not os.path.exists(out):
        return None, (r.stdout + r.stderr).strip(), r.returncode
    return read_wav(out), (r.stdout + r.stderr).strip(), r.returncode


def read_wav(path):
    import scipy.io.wavfile as wf
    sr, x = wf.read(path)
    x = x.astype(np.float64)
    if x.ndim == 1:
        x = x[:, None]
    return x


def onsets(x, thresh=0.0, min_gap=2400):
    """First non-silent sample of each burst (any channel), bursts separated by >= min_gap silent samples."""
    a = np.max(np.abs(x), axis=1) > thresh
    idx = np.flatnonzero(a)
    if idx.size == 0:
        return []
    starts = [int(idx[0])]
    gaps = np.flatnonzero(np.diff(idx) > min_gap)
    starts += [int(idx[g + 1]) for g in gaps]
    return starts


def rms(x, a, b):
    return np.sqrt(np.mean(x[a:b] ** 2, axis=0))


def est_freq(x, f0, a, b):
    seg = x[a:b, 0] + x[a:b, 1]
    n = seg.size
    w = np.hanning(n)
    t = np.arange(n) / SR

    def neg(f):
        return -abs(np.dot(seg * w, np.exp(-2j * np.pi * f * t)))

    from scipy.optimize import minimize_scalar
    r = minimize_scalar(neg, bounds=(f0 * 0.99, f0 * 1.01), method="bounded", options={"xatol": 1e-7})
    return r.x


