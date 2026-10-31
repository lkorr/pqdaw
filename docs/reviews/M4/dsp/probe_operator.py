"""Operator: pitch (ratio / den / fine / fixed / transpose), PM index vs Bessel, feedback, envelope law, velocity,
key scaling, volume, spread, filter cutoff. Memory-light: short renders, one note."""
import sys
import numpy as np
from scipy.special import jv
from pqprobe import render, db, peak_freq, tone_amp


def op(args, notes="69@0:0.9*1", secs=1.0, sr=48000):
    a = ", ".join("%s: %s" % (k, v) for k, v in args.items())
    body = "  o = operator(%s)\n  y = o" % a
    _, y = render(body, ["devices/operator.pqp"], seconds=secs, rate=sr, notes=notes, out_type="audio voice stereo")
    return y


def part1():
    print("-- pitch, note 69 (440 Hz), A alone")
    cases = [({}, 440), ({"coarseA": 3, "denA": 2}, 660), ({"coarseA": 0}, 220), ({"coarseA": 7, "denA": 4}, 770),
             ({"fineA": 500}, 660), ({"fixedA": "true", "freqA": "1234Hz"}, 1234), ({"transpose": "700c"}, 440 * 2 ** (7 / 12)),
             ({"coarseA": 5, "denA": 4, "transpose": "-1200c"}, 275)]
    for args, want in cases:
        y = op(args)[2400:40000, 0]
        f = peak_freq(y, 48000, want * 0.8, want * 1.2)
        print("%-45s want %8.3f got %8.3f Hz (%+.4f c)" % (args, want, f, 1200 * np.log2(f / want)))
    for sr in (22050, 192000):
        y = op({"coarseA": 3, "denA": 2}, sr=sr)[sr // 20: int(sr * 0.8), 0]
        f = peak_freq(y, sr, 500, 800)
        print("sr %d ratio 3/2: %.3f Hz (%+.4f c)" % (sr, f, 1200 * np.log2(f / 660)))
    y = op({}, notes="69@0:0.9*1")
    print("default A sine, volume -12 dB, vel 1: amplitude %.4f (%.2f dBFS); L==R %s" % (np.max(np.abs(y[2400:40000, 0])), db(np.max(np.abs(y[2400:40000, 0]))), np.allclose(y[:, 0], y[:, 1])))


def part2():
    print("-- PM index: algorithm 1, B (ratio 1) -> A (ratio 1), levelB 0 / -6 / -12 dB, sustained; harmonic amplitudes vs Bessel")
    for lb in (0, -6, -12):
        y = op({"onB": 1, "levelB": "%ddB" % lb})[4800:43200, 0]
        a = np.array([tone_amp(y, 440 * h, 48000) for h in range(1, 7)])
        a /= max(a.max(), 1e-12)
        # PM of a sine by an equal-frequency sine: sin(wt + b sin wt) = sum J_n(b) sin((1+n)wt); harmonic h gets J_{h-1}(b) - J_{-(h+1)}(b) ...
        best = None
        for beta in np.linspace(0.05, 8, 800):
            n = np.arange(-12, 13)
            amps = np.zeros(8)
            for k in n:
                h = 1 + k
                if 0 < h < 8:
                    amps[h] += jv(k, beta)
                elif h < 0 and -h < 8:
                    amps[-h] -= jv(k, beta)
            m = np.abs(amps[1:7]); m /= m.max()
            e = np.sum((m - a) ** 2)
            if best is None or e < best[0]:
                best = (e, beta)
        print("levelB %+3d dB: H1..H6 %s  -> best-fit index beta = %.2f rad (doc: pi * level = %.2f)" % (lb, np.round(a, 3), best[1], np.pi * 10 ** (lb / 20)))
    y = op({"onA": 0, "onD": 1, "levelD": "0dB", "algorithm": 11, "feedback": 1.0})[4800:43200, 0]
    a = np.array([tone_amp(y, 440 * h, 48000) for h in range(1, 9)])
    print("D alone, feedback 100 %%: H1..H8 re H1 (dB) %s; peak %.3f" % (np.round(db(a / a[0]), 1), np.max(np.abs(y))))


def part3():
    print("-- envelope: A decay 1 s, sustain -inf; level at t = 0.5 / 1.0 s re peak (doc: -80 dB at d, exponential)")
    y = op({"decayA": "1s", "sustainA": "-70dB"}, notes="69@0:2*1", secs=1.2)[:, 0]
    env = lambda t: np.max(np.abs(y[int(t * 48000) - 120:int(t * 48000) + 120]))
    pk = env(0.003)
    print("peak %.4f, 0.5 s %+.1f dB, 1.0 s %+.1f dB" % (pk, db(env(0.5) / pk), db(env(1.0) / pk)))
    print("-- velocity: velA 100 %, note velocity 1 / 0.5 / 0.1 (dB re vel 1)")
    ref = None
    for v in (1, 0.5, 0.1):
        y = op({"velA": 1.0}, notes="69@0:0.9*%g" % v)[4800:40000, 0]
        a = tone_amp(y, 440, 48000)
        ref = ref or a
        print("vel %.1f: %+.2f dB" % (v, db(a / ref)))
    print("-- key scaling: keyA 100 %, notes 60 (C3) / 72 / 96 / 108 (dB re C3)")
    ref = None
    for n in (60, 72, 96, 108):
        y = op({"keyA": 1.0}, notes="%d@0:0.9*1" % n)[4800:40000, 0]
        a = np.max(np.abs(y))
        ref = ref or a
        print("note %d: %+.2f dB  peak %.3f" % (n, db(a / ref), a))
    print("-- spread 100 %: L / R pitch re 440 (cents)")
    y = op({"spreadOn": 1, "spread": 1.0})[4800:43200]
    for ch in (0, 1):
        f = peak_freq(y[:, ch], 48000, 400, 480)
        print("ch %d: %.3f Hz (%+.2f c)" % (ch, f, 1200 * np.log2(f / 440)))


def part4():
    print("-- filter: A saw-ish (wave 9 Saw 64) through LP12 at 1 kHz, res 0 / 50 / 100 %: H(harmonic) re unfiltered")
    base = op({"waveA": 9})[4800:43200, 0]
    hs = [1, 2, 3, 5, 10]
    a0 = np.array([tone_amp(base, 220 * h, 48000) for h in hs])
    for res in (0.0, 0.5, 1.0):
        for c in (0, 2, 4):
            y = op({"waveA": 9, "filterOn": 1, "filterFreq": "1kHz", "filterRes": res, "filterCircuit": c}, notes="57@0:0.9*1")[4800:43200, 0]
            a = np.array([tone_amp(y, 220 * h, 48000) for h in hs])
            b0 = np.array([tone_amp(op({"waveA": 9}, notes="57@0:0.9*1")[4800:43200, 0], 220 * h, 48000) for h in hs])
            print("res %.1f circuit %d: %s dB at %s Hz" % (res, c, np.round(db(a / b0), 1), [220 * h for h in hs]))


if __name__ == "__main__":
    for p in sys.argv[1:] or ["1", "2", "3", "4"]:
        globals()["part" + p]()
