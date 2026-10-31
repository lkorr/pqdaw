"""Saturator: per-curve gain staging at default drive, curve formulas vs an independent numpy model, dry/wet with
oversampling (phase alignment), DC. Memory-light: short renders."""
import numpy as np
from pqprobe import render, freqresp, tone_amp, db

NAMES = ["Analog Clip", "Soft Sine", "Bass Shaper", "Medium", "Hard", "Sinoid Fold", "Digital Clip", "Waveshaper", "Shaper"]


def model(t, u):
    if t == 0:
        c = np.clip(u, -1.5, 1.5); return c - 4 / 27 * c ** 3
    if t == 1:
        return np.sin(np.clip(u, -np.pi / 2, np.pi / 2))
    if t == 2:
        a = np.abs(u); return u * (a + 2) / (u * u + a + 1)
    if t == 3:
        return np.tanh(u)
    if t == 4:
        return u / (1 + u ** 4) ** 0.25
    if t == 5:
        return np.sin(u)
    if t == 6:
        return np.clip(u, -1, 1)
    if t == 7:  # defaults wsDrive .5 lin .5 curve .5 depth 0 damp 0
        tt = np.clip(u, -1, 1); ws = 0.5 * u + 0.5 * (1.5 * tt - 0.5 * tt ** 3)
        return u + 0.5 * (ws - u)
    if t == 8:
        return np.clip(u, -1, 1)


def sat(amp, f, t, drive_db=0, hq=1, wet=1.0, sr=48000, secs=0.25):
    body = ("  s = %r * sine(phasor(%rHz))\n  y = saturator((s, s), type: %d, hq: %d, drive: %rdB, dryWet: %r)"
            % (float(amp), float(f), t, hq, float(drive_db), float(wet)))
    _, y = render(body, ["devices/saturator.pqp"], seconds=secs, rate=sr)
    return y[:, 0]


if __name__ == "__main__":
    sr = 48000
    print("-- level of the 1 kHz fundamental re input, drive 0 dB (default), and max curve error vs model")
    print("%-13s %9s %9s %9s %9s" % ("curve", "-20 dBFS", "-6 dBFS", "0 dBFS", "maxerr"))
    for t in range(9):
        row = []
        maxerr = 0
        for a in (0.1, 0.5, 1.0):
            y = sat(a, 1000, t)
            n = np.arange(len(y)); x = a * np.sin(2 * np.pi * 1000 * n / sr)
            maxerr = max(maxerr, np.max(np.abs(y - model(t, x))))
            row.append(db(tone_amp(y[2000:], 1000, sr) / a))
        print("%-13s %+8.2f %+8.2f %+8.2f  %.1e" % (NAMES[t], *row, maxerr))
    print("-- Medium Curve at +12 dB drive, RMS out re in for a -12 dBFS sine (level jump when switching curves)")
    for t in range(8):
        y = sat(0.25, 1000, t, drive_db=12)
        print("%-13s rms %+6.2f dB re input, peak %.3f" % (NAMES[t], db(np.sqrt(np.mean(y[2000:] ** 2)) / (0.25 / np.sqrt(2))), np.max(np.abs(y))))
    print("-- dry/wet 50 %, linear region (impulse 1e-3 into Analog Clip): response vs frequency per HQ")
    f = np.array([1000, 5000, 10000, 15000, 18000, 20000])
    for hq in (1, 2, 4, 8):
        body = ("  imp = 0.001 * gate_to_real(pulse(0))\n  y = saturator((imp, imp), type: 0, hq: %d, dryWet: 0.5)" % hq)
        _, y = render(body, ["devices/saturator.pqp"], seconds=0.05, rate=sr)
        H = db(freqresp(y[:, 0] / 0.001, f, sr))
        print("hq %d: " % hq + "  ".join("%5d Hz %+6.2f" % (a, b) for a, b in zip(f, H)))
