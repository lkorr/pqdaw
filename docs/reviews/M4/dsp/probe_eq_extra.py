"""EQ Eight: 48 dB cut resonance vs q (peak gain), and HQ's half-band passband at several sample rates."""
import numpy as np
from pqprobe import freqresp, db
from probe_eq_eight import band

print("-- Low Cut 48 / High Cut 48 peak gain vs q (48 kHz, 1 kHz), and LC12 for comparison")
for q in (0.7071, 1.0, 1.5, 2.0, 4.0, 18.0):
    out = []
    for t in (0, 7, 1):
        y = band(t, 1000, 0, q, 48000, seconds=2.0)
        f = np.linspace(300, 3000, 4000)
        out.append(np.max(db(freqresp(y[:, 0], f, 48000))))
    print("q %6.4f: LC48 %+7.1f dB  HC48 %+7.1f dB  LC12 %+6.1f dB" % (q, *out))
print("-- HQ (2x) passband: one flat bell, response at the top of the band")
for sr in (22050, 44100, 48000):
    y = band(3, 1000, 0, 1, sr, hq=2)
    f = np.array([0.30, 0.35, 0.40, 0.42, 0.45, 0.48]) * sr
    H = db(freqresp(y[:, 0], f, sr))
    print("sr %6d: " % sr + "  ".join("%5.0f Hz %+6.2f" % (a, b) for a, b in zip(f, H)))
