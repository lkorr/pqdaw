"""Probe 3: automation as rendered vs the Q39 curve law (block-rate write + 2 ms one-pole smoothing, Q80.6 / Q95.3).

Two renders of one sustained note: with a volume lane and without (0 dB). Their per-sample ratio is the applied gain."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from pqlib import *

Q = PPQ
BLOCK = 64


def project(lane_points, on_group, tempo=None, curve_kind="volume"):
    p = base_project(120.0)
    if tempo:
        p["timeline"]["tempo"] = tempo
    t = midi([clip(0, 10 * Q, [note(0, 10 * Q, degree=0)])])
    if on_group:
        g = group([t])
        if lane_points:
            g["lanes"] = [{"id": nid(), "param": {"kind": "trackVolume", "owner": g["id"]}, "points": lane_points}]
        p["tracks"].append(g)
    else:
        if lane_points:
            t["lanes"] = [{"id": nid(), "param": {"kind": "trackVolume", "owner": t["id"]}, "points": lane_points}]
        p["tracks"].append(t)
    return p


def model_gain(points, tempo, n):
    """Gain per sample: lane value (dB) at each block's first sample tick, -> linear, one-pole smoothed (2 ms)."""
    k = 1 - math.exp(-1 / (0.002 * SR))
    g = np.zeros(n)
    sm = None
    # samples -> ticks (exact inverse of the tempo model by bisection-free stepping: constant tempo segments only here)
    out = np.zeros(n)
    for b0 in range(0, n, BLOCK):
        sec = b0 / SR
        # invert ticks_to_seconds numerically
        lo, hi = 0.0, 100 * Q
        for _ in range(80):
            mid = (lo + hi) / 2
            if ticks_to_seconds(tempo, mid) < sec:
                lo = mid
            else:
                hi = mid
        v = eval_lane(points, lo)
        target = db_gain(v)
        g[b0:b0 + BLOCK] = target
    s = 0.0
    for i in range(n):  # z1(sm + (target - sm) k): output is the previous state
        out[i] = s
        s = s + (g[i] - s) * k
    return out


def run_case(name, points, on_group, tempo=None):
    tempo_pts = tempo or [{"tick": 0, "bpm": 120.0, "curve": "step"}]
    pa = project(points, on_group, tempo)
    pr = project(None, on_group, tempo)
    xa, la, _ = render(write_project(pa, name + "_auto"), 5.5)
    xr, lr, _ = render(write_project(pr, name + "_ref"), 5.5)
    if xa is None or xr is None:
        print(name, "FAILED", la, lr)
        return
    n = xa.shape[0]
    a, r = xa[:, 0], xr[:, 0]
    m = model_gain(points, tempo_pts, n)
    sel = np.flatnonzero(np.abs(r) > 0.02)
    sel = sel[(sel > int(0.2 * SR)) & (sel < int(5.0 * SR))]
    got = a[sel] / r[sel]
    err_db = 20 * np.log10(np.maximum(got, 1e-9) / np.maximum(m[sel], 1e-9))
    worst = np.argmax(np.abs(err_db))
    print(f"{name}: max |error| vs model {np.abs(err_db).max():.4f} dB at t={sel[worst]/SR:.4f}s "
          f"(got {20*np.log10(got[worst]):.3f} dB, model {20*np.log10(m[sel][worst]):.3f} dB); "
          f"p99 {np.percentile(np.abs(err_db), 99):.4f} dB")
    # also: error against the IDEAL continuous lane (what a user expects), to size the block-rate stepping
    ideal = np.array([db_gain(eval_lane(points, s / SR * 2 * Q)) for s in sel]) if tempo is None else None
    if ideal is not None:
        e2 = 20 * np.log10(np.maximum(got, 1e-9) / ideal)
        print(f"   vs ideal per-sample lane: max {np.abs(e2).max():.3f} dB, p99 {np.percentile(np.abs(e2), 99):.3f} dB")


pts = [{"id": nid(), "tick": 2 * Q, "value": -18.0, "curve": 0.4}, {"id": nid(), "tick": 6 * Q, "value": 0.0, "curve": 0.0}]
run_case("auto_curve_pos_midi", pts, False)
pts = [{"id": nid(), "tick": 2 * Q, "value": 0.0, "curve": -1.0}, {"id": nid(), "tick": 6 * Q, "value": -40.0, "curve": 0.0}]
run_case("auto_curve_neg_group", pts, True)
# a jump: two points on one tick (ordered by id)
a, b = nid(), nid()
pts = [{"id": a, "tick": 4 * Q, "value": 0.0, "curve": 0.0}, {"id": b, "tick": 4 * Q, "value": -20.0, "curve": 0.0}]
run_case("auto_jump", pts, False)
