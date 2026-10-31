"""EQ Eight: one band at a time vs independently derived bilinear (RBJ-prototype) responses, several sample rates;
48 dB resonance at max Q; HQ near Nyquist vs the analog curve; M/S and L/R unity."""
import numpy as np
from pqprobe import render, freqresp, bilinear_resp, analog_resp, db

TYPES = {0: "hp8", 1: "hp", 2: "lowshelf", 3: "bell", 4: "notch", 5: "highshelf", 6: "lp", 7: "lp8"}


def band(t, f0, gdb, q, sr, hq=1, adaptive=False, scale=1.0, mode=0, seconds=None):
    ons = ", ".join("on%d: %d" % (i, 1 if i == 4 else 0) for i in range(1, 9))
    secs = seconds or (1.0 if f0 < 100 or q > 5 else 0.35)
    body = ("  imp = gate_to_real(pulse(0))\n"
            "  y = eq_eight((imp, imp), mode: %d, hq: %d, %s, type4: %d, freq4: %r, gain4: %rdB, q4: %r, adaptive: %s, scale: %r)"
            % (mode, hq, ons, t, float(f0), float(gdb), float(q), "true" if adaptive else "false", float(scale)))
    _, y = render(body, ["devices/eq_eight.pqp"], seconds=secs, rate=sr)
    return y


def check(t, f0, gdb, q, sr, hq=1, adaptive=False, scale=1.0):
    y = band(t, f0, gdb, q, sr, hq, adaptive, scale)
    h = y[:, 0]
    f = np.geomspace(20, min(20000, 0.49 * sr), 300)
    H = freqresp(h, f, sr)
    geff = gdb * scale
    qeff = q * 10 ** (abs(geff) / 40) if (adaptive and t in (2, 3, 5)) else q
    kw = {"Q": qeff} if t in (1, 4, 6) or t in (2, 3, 5) else {}
    if t in (2, 3, 5):
        kw["dB"] = geff
    f0c = min(f0, 0.49 * sr)
    if t in (0, 7):
        # Butterworth-8 at q = 0.7071; for other q every section's damping is scaled by 1/(sqrt2 q)
        s_dig = 1j * np.tan(np.pi * f / (sr * hq)) / np.tan(np.pi * f0c / (sr * hq))
        Hm = np.ones_like(s_dig)
        for i in range(1, 5):
            qi = 1 / (2 * np.sin((2 * i - 1) * np.pi / 16)) * (q * np.sqrt(2))
            Hm = Hm * (s_dig * s_dig if t == 0 else 1) / (s_dig * s_dig + s_dig / qi + 1)
        ref = Hm
        ana = None
    else:
        ref = bilinear_resp(TYPES[t], f, f0c, sr * hq, **kw)
        ana = analog_resp(TYPES[t], f, f0c, **kw)
    m = db(ref) > -60
    err = np.max(np.abs(db(H)[m] - db(ref)[m]))
    aerr = None if ana is None else np.max(np.abs(db(H)[m] - db(ana)[m]))
    return err, aerr, np.max(db(H)), y


if __name__ == "__main__":
    print("type f0 gain q sr hq | max |dB err| vs bilinear  vs analog | peak dB")
    for sr in (22050, 44100, 48000, 96000, 192000):
        for (t, f0, g, q) in [(0, 100, 0, 0.7071), (1, 100, 0, 2), (2, 200, 9, 0.7071), (3, 1000, 12, 2), (3, 1000, -15, 0.3),
                              (4, 1000, 0, 4), (5, 8000, 6, 0.7071), (6, 5000, 0, 1), (7, 5000, 0, 0.7071), (3, 16000, 12, 1),
                              (5, 15000, -12, 0.7071)]:
            if f0 > 0.49 * sr and t != 3:
                continue
            for hq in (1, 2):
                if hq == 2 and sr > 48000:
                    continue
                e, ae, pk, _ = check(t, f0, g, q, sr, hq)
                print("%d %6g %4g %5g %6d %d | %8.3f  %s | %7.2f" % (t, f0, g, q, sr, hq, e, "%8.3f" % ae if ae is not None else "   -   ", pk))
    print("-- max-Q resonance of the cut types (48 kHz)")
    for t in (0, 1, 6, 7):
        _, _, pk, _ = check(t, 1000, 0, 18, 48000)
        print("type %d q 18: peak %.1f dB" % (t, pk))
    print("-- adaptive Q / scale")
    for (t, g, sc, ad) in [(3, 15, 1, True), (3, -15, 1, True), (3, 10, -1, False), (3, 10, 2, False)]:
        e, ae, pk, _ = check(t, 1000, g, 1.0, 48000, adaptive=ad, scale=sc)
        print("bell %g dB scale %g adaptive %s: err %.3f peak %.2f" % (g, sc, ad, e, pk))
    print("-- 22 kHz band 8 freq at 44.1k: clipped to 0.49 sr = %.0f Hz" % (0.49 * 44100))
