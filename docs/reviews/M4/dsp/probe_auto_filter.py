"""Auto Filter: Clean responses vs independent bilinear prototypes (the documented resonance law), small-signal
passband / peak per circuit model (level jumps when switching circuits), self-oscillation and stability at extreme
settings and sample rates, comb tuning. Memory-light."""
import sys
import numpy as np
from pqprobe import render, freqresp, bilinear_resp, db, peak_freq, tone_amp

CIRC = ["Clean", "OSR", "MS2", "SMP", "PRD"]


def af(sig, sr, secs, **kw):
    args = ", ".join("%s: %s" % (k, v) for k, v in kw.items())
    body = "  s = %s\n  y = auto_filter((s, s), %s)" % (sig, args)
    _, y = render(body, ["devices/auto_filter.pqp"], seconds=secs, rate=sr)
    return y[:, 0]


IMP = "0.001 * gate_to_real(pulse(0))"


def part1():
    sr = 48000
    print("-- Clean: max |dB| error vs bilinear prototype (k = max(sqrt2(1-0.8res), 0.02)), 1 kHz")
    f = np.geomspace(20, 20000, 200)
    for slope in (0, 1):
        for res in (0.0, 0.3, 1.0, 1.25):
            k12 = max(np.sqrt(2) * (1 - 0.8 * res), 0.02)
            for typ, kind in ((0, "lp"), (1, "hp"), (2, "bp"), (3, "notch")):
                h = af(IMP, sr, 1.0, type=typ, slope=slope, res=res, freq="1kHz") / 0.001
                H = freqresp(h, f, sr)
                if slope == 0:
                    ref = bilinear_resp(kind, f, 1000, sr, Q=1 / k12)
                else:
                    ref = bilinear_resp(kind, f, 1000, sr, Q=1 / 1.8477590650225735) * bilinear_resp(kind, f, 1000, sr, Q=1 / (0.541196100146197 * k12))
                m = db(ref) > -60
                print("slope %d res %.2f %-5s err %.3f dB  peak %+6.2f dB" % (slope, res, kind, np.max(np.abs(db(H)[m] - db(ref)[m])), np.max(db(H))))


def part2():
    sr = 48000
    print("-- small-signal LP12 at 1 kHz per circuit: gain at 100 Hz (passband) and peak, res 0 / 30 % / 100 %")
    f = np.geomspace(50, 5000, 200)
    for res in (0.0, 0.3, 1.0):
        for c in range(5):
            h = af(IMP, sr, 1.0, type=0, circuit=c, res=res, freq="1kHz") / 0.001
            H = db(freqresp(h, f, sr))
            print("res %.2f %-5s pass %+6.2f dB  peak %+6.2f dB at %5.0f Hz" % (res, CIRC[c], H[0], H.max(), f[np.argmax(H)]))
    print("-- loud input: 0 dBFS 110 Hz sine through LP12 at 2 kHz, res 30 %, drive 0 / 12 / 24 dB: fundamental level")
    for c in range(5):
        row = []
        for d in (0, 12, 24):
            y = af("sine(phasor(110Hz))", sr, 0.3, type=0, circuit=c, res=0.3, freq="2kHz", drive="%ddB" % d)
            row.append(db(tone_amp(y[4800:], 110, sr)))
        print("%-5s %+6.2f %+6.2f %+6.2f dB" % (CIRC[c], *row))


def part3():
    print("-- self-oscillation / stability: impulse kick, then silence. RMS of the last 100 ms and max |y|")
    for sr in (22050, 48000, 192000):
        for c in range(5):
            for fc in ("50Hz", "15kHz"):
                for slope in (0, 1):
                    y = af(IMP, sr, 1.0, type=0, circuit=c, res=1.25, freq=fc, slope=slope, drive="24dB")
                    tail = y[-int(0.1 * sr):]
                    fin = np.all(np.isfinite(y))
                    pf = peak_freq(tail, sr) if np.max(np.abs(tail)) > 1e-6 else float("nan")
                    print("sr %6d %-5s fc %5s slope %d: finite %s  tail rms %+7.1f dBFS  max %.2f  osc %.0f Hz"
                          % (sr, CIRC[c], fc, slope, fin, db(np.sqrt(np.mean(tail ** 2))), np.max(np.abs(y)), pf))


def part4():
    sr = 48000
    print("-- comb: peak (fundamental) frequency vs fc, res 100 %")
    for fc in (100, 1000, 3000, 7000, 10000):
        y = af("0.1 * noise()", sr, 1.0, type=6, res=1.0, freq="%dHz" % fc)
        print("fc %5d Hz: measured fundamental %.1f Hz (%.1f cents)" % (fc, peak_freq(y, sr, fc * 0.7, fc * 1.3), 1200 * np.log2(peak_freq(y, sr, fc * 0.7, fc * 1.3) / fc)))


if __name__ == "__main__":
    for p in sys.argv[1:] or ["1", "2", "3", "4"]:
        globals()["part" + p]()
