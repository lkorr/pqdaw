import sys, numpy as np
from scipy.io import wavfile
sr, y = wavfile.read(sys.argv[1]); y = y.astype(np.float64)
for k in range(0, len(y), 1500):
    seg = y[k:k+1500]
    print(f"{k/sr*1000:7.1f}ms min {seg.min():+.4f} max {seg.max():+.4f} mean {seg.mean():+.4f}")

# independent reference in double: same patch, Newton to convergence
fs = 48000.0
n = len(y)
t = np.arange(n)
ph = (t * 110.0 / fs) % 1.0          # phasor starting at 0
saw = ph * 2 - 1
lfo = np.sin(2*np.pi*((t*3.0/fs) % 1.0))
fc = 1200 * (1 + 0.5*lfo)
g = np.tan(np.pi*fc/fs)
x = saw * 3.0
s = 0.0
out = np.zeros(n)
v = 0.0
for i in range(n):
    tx = np.tanh(x[i])
    vv = s  # guess
    for it in range(50):
        f = vv - s - g[i]*(tx - np.tanh(vv))
        df = 1 + g[i]*(1 - np.tanh(vv)**2)
        vv -= f/df
    v = vv
    out[i] = v*0.5
    s = 2*v - s
d = np.abs(out - y)
print("ref peak", np.abs(out).max(), "max|ref-golden|", d.max(), "at", d.argmax())
wrap = np.where(np.diff(ph) < 0)[0] + 1
mask = np.ones(n, bool)
for w in wrap:
    mask[max(0, w-3):w+40] = False
print("excluding 40 samples after wraps: max", d[mask].max(), "median", np.median(d))
print("first diffs", d[:8], y[:4], out[:4])
