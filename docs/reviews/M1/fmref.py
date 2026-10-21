# Independent double/uint32 reference of tests/golden fm_voice (FmVoice + atom adsr), voices summed.
import sys, math, numpy as np
from scipy.io import wavfile
sr = 48000
f32 = np.float32
def key_hz(k): return f32(440.0 * 2 ** ((k - 69) / 12))
notes = [(60, 0, .4, .7), (64, .1, .4, .7), (67, .2, .5, .9), (72, .45, .35, .6), (55, .6, .3, .7)]
if len(sys.argv) > 2 and sys.argv[2] == 'arp':
    notes = []
    ks = [60, 64, 67, 72]
    for k in range(4):
        notes.append((ks[k % 4], k * .25, .125, .7))
N = sr
ratio, index, a, d, s, r = 2.0, 1.0, .005, .2, .7, .12
inc_scale = 2.0 ** 32 / sr
def inc(h):
    h = min(max(float(h), -sr / 2), sr / 2)
    return int(round(h * inc_scale)) & 0xFFFFFFFF
def phase(ph): return (ph >> 8) * 2.0 ** -24
total = np.zeros(N)
for key, t0, ln, vel in notes:
    hz = key_hz(key)
    k0 = int(round(t0 * sr)); k1 = int(round((t0 + ln) * sr))
    p1 = p2 = 0; level = 0.0; q = False; prevg = False
    for n in range(k0, N):
        g = n < k1
        m = math.sin(2 * math.pi * phase(p1))
        car = math.sin(2 * math.pi * phase(p2))
        rise = g and not prevg; prevg = g
        reset = (level >= 0.999) or (not g)
        q = False if reset else (True if rise else q)
        target = 1.0 if q else (s if g else 0.0)
        tm = a if q else (d if g else r)
        coef = 1 - math.exp(-1 / max(tm * sr, 1))
        total[n] += car * level * vel
        level = level + (target - level) * coef
        p1 = (p1 + inc(f32(hz) * f32(ratio))) & 0xFFFFFFFF
        p2 = (p2 + inc(f32(hz) + f32(m) * f32(index) * f32(hz))) & 0xFFFFFFFF
_, y = wavfile.read(sys.argv[1]); y = y.astype(np.float64)
dd = np.abs(y - total)
print("max |golden - ref|", dd.max(), "at", dd.argmax(), "median", np.median(dd), "ref peak", np.abs(total).max())
