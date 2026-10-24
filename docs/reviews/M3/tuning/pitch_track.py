"""M3 review probe: measure the pitch track of a rendered project WAV and compare it with an independent model of
a single Degree note under MOS generator automation (parsed from the project's project.pqd). Not product code.

usage: python -I pitch_track.py <project_dir> <wav> [--win 0.05]
The model re-implements buildMOSScale (chain of n generators reduced into the period, sorted) and rotation from the
mos-explorer definition, NOT from the C++, and linear automation interpolation in ticks (curve 0).
"""
import json, math, sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfiltfilt

PPQ = 221760


def mos_pitches(g, n, P, rot):
    ch = sorted(((i * g) % P + P) % P for i in range(n))
    root = ch[rot % n]
    return sorted(((c - root) % P + P) % P for c in ch)


def main():
    proj, wav = sys.argv[1], sys.argv[2]
    win = float(sys.argv[sys.argv.index('--win') + 1]) if '--win' in sys.argv else 0.05
    doc = json.load(open(proj + '/project.pqd'))
    bpm = doc['timeline']['tempo'][0]['bpm']
    tps = PPQ * bpm / 60.0
    tun = {t['id']: t for t in doc['tunings']}
    tr = doc['tracks'][0]
    ct = tr['clips'][0]['tuning']
    tdef = tun[(ct if isinstance(ct, dict) else tr['tuning'])['main']]['def']  # the clip override wins (SPEC 3.2)
    lane = tr['lanes'][0]['points'] if tr['lanes'] else []
    note = tr['clips'][0]['notes'][0]
    deg, eq = note['pitch']['degree'], note['pitch']['equave']
    t_on, t_off = note['start'] / tps, (note['start'] + note['length']) / tps
    ref = tdef['referenceHz']

    def gen_at(t):
        if not lane:
            return tdef['generator']
        tick = t * tps
        if tick <= lane[0]['tick']:
            return lane[0]['value']
        for a, b in zip(lane, lane[1:]):
            if tick <= b['tick']:
                return a['value'] + (b['value'] - a['value']) * (tick - a['tick']) / (b['tick'] - a['tick'])
        return lane[-1]['value']

    def model_hz(t):
        g = gen_at(t)
        P, n, rot = tdef['period'], tdef['n'], tdef['rotation']
        p = mos_pitches(g, n, P, rot)
        N = n * tdef['periodsPerEquave']
        D = eq * N + deg
        q, r = divmod(D, N)
        k, j = divmod(r, n)
        cents = (p[j] + k * P + q * P * tdef['periodsPerEquave']) * (1200 + tdef['octaveStretch']) / 1200
        return ref * 2 ** (cents / 1200)

    sr, x = wavfile.read(wav)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x[:, 0]
    x = sosfiltfilt(butter(4, [150, 450], 'bandpass', fs=sr, output='sos'), x)
    s = np.signbit(x)
    up = np.nonzero(s[:-1] & ~s[1:])[0]
    tz = (up + x[up] / (x[up] - x[up + 1])) / sr  # interpolated upward zero crossings
    rows = []
    t = t_on + 0.02
    while t + win < t_off - 0.02:
        sel = tz[(tz >= t) & (tz < t + win)]
        if len(sel) > 3:
            f = (len(sel) - 1) / (sel[-1] - sel[0])
            tm = (sel[0] + sel[-1]) / 2
            # model: mean frequency over the same span (period-weighted: average of 1/f over crossings)
            ts = np.linspace(sel[0], sel[-1], 200)
            fm = 1.0 / np.mean([1.0 / model_hz(u) for u in ts])
            rows.append((tm, f, fm, 1200 * math.log2(f / fm), gen_at(tm)))
        t += win
    worst = max(rows, key=lambda r: abs(r[3]))
    jumps = [(a[0], b[0], 1200 * math.log2(b[1] / a[1])) for a, b in zip(rows, rows[1:])]
    big = [j for j in jumps if abs(j[2]) > 20]
    print(f"{len(rows)} windows of {win}s; worst |measured - model| = {worst[3]:+.3f} c at t={worst[0]:.3f}s "
          f"(g={worst[4]:.3f}, meas {worst[1]:.4f} Hz, model {worst[2]:.4f} Hz)")
    print("jumps > 20 c between consecutive windows:", [(round(a, 3), round(b, 3), round(c, 2)) for a, b, c in big])
    errs = np.array([r[3] for r in rows])
    smooth = [r for r in rows if not any(a - win <= r[0] <= b + win for a, b, _ in big)]
    print(f"away from jumps: max |err| = {max(abs(r[3]) for r in smooth):.4f} c, mean err {np.mean([r[3] for r in smooth]):+.4f} c")
    for r in rows[:: max(1, len(rows) // 12)]:
        print(f"  t={r[0]:.3f} g={r[4]:.3f} meas={r[1]:.4f} model={r[2]:.4f} err={r[3]:+.4f} c")


if __name__ == '__main__':
    main()
