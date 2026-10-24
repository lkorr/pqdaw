"""M3 review probe: measured fundamental (zero crossings of a band-passed channel 0) per window.
usage: py -3 measure.py <wav> <band_lo_hz> <band_hi_hz> [t0 t1 step]"""
import sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfiltfilt

wav, lo, hi = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
t0, t1, step = (float(a) for a in sys.argv[4:7]) if len(sys.argv) > 6 else (0.05, 2.45, 0.2)
sr, x = wavfile.read(wav)
x = x.astype(np.float64)
x = x[:, 0] if x.ndim > 1 else x
x = sosfiltfilt(butter(4, [lo, hi], 'bandpass', fs=sr, output='sos'), x)
s = np.signbit(x)
up = np.nonzero(s[:-1] & ~s[1:])[0]
tz = (up + x[up] / (x[up] - x[up + 1])) / sr
t = t0
while t <= t1:
    sel = tz[(tz >= t) & (tz < t + 0.05)]
    if len(sel) > 3:
        print(f"t={t:.2f}s  {(len(sel) - 1) / (sel[-1] - sel[0]):.4f} Hz")
    t += step
