import sys, math, numpy as np
from scipy.io import wavfile
sr = 48000; N = sr; fb = 0.45
trig = np.zeros(N); trig[::14400] = 1
env = np.zeros(N); st = 0.0
for n in range(N):
    env[n] = st
    st = max(st * 0.9993, trig[n])
inc = int(round(660 * 2 ** 32 / sr)) & 0xFFFFFFFF
ph = (np.arange(N, dtype=np.uint64) * inc) & 0xFFFFFFFF
phs = ph.astype(np.int64); phs[phs >= 2 ** 31] -= 2 ** 32
tone = np.sin(phs * 2 * math.pi / 2 ** 32) * env
L = np.zeros(N); R = np.zeros(N); DL, DR = 8640, 12960
lin = np.zeros(N); rin = np.zeros(N)
for n in range(N):
    L[n] = lin[n - DL] if n >= DL else 0.0
    R[n] = rin[n - DR] if n >= DR else 0.0
    lin[n] = tone[n] + R[n] * fb
    rin[n] = L[n] * fb
l = tone * 0.6 + L * 0.5; r = tone * 0.4 + R * 0.5
_, y = wavfile.read(sys.argv[1]); y = y.astype(np.float64)
for c, ref in enumerate([l, r]):
    d = np.abs(y[:, c] - ref)
    print("ch", c, "max", d.max(), "at", d.argmax())
